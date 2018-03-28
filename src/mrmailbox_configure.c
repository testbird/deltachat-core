/*******************************************************************************
 *
 *                              Delta Chat Core
 *                      Copyright (C) 2017 Björn Petersen
 *                   Contact: r10s@b44t.com, http://b44t.com
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see http://www.gnu.org/licenses/ .
 *
 ******************************************************************************/


#include <dirent.h>
#include "mrmailbox_internal.h"
#include "mrloginparam.h"
#include "mrimap.h"
#include "mrsmtp.h"
#include "mrosnative.h"
#include "mrsaxparser.h"
#include "mrjob.h"


/*******************************************************************************
 * Tools
 ******************************************************************************/


static char* read_autoconf_file(mrmailbox_t* mailbox, const char* url)
{
	char* filecontent = NULL;
	mrmailbox_log_info(mailbox, 0, "Testing %s ...", url);
	filecontent = (char*)mailbox->m_cb(mailbox, MR_EVENT_HTTP_GET, (uintptr_t)url, 0);
	if( filecontent == NULL ) {
		mrmailbox_log_info(mailbox, 0, "Can't read file."); /* this is not a warning or an error, we're just testing */
		return NULL;
	}
	return filecontent;
}


/*******************************************************************************
 * Thunderbird's Autoconfigure
 ******************************************************************************/


/* documentation: https://developer.mozilla.org/en-US/docs/Mozilla/Thunderbird/Autoconfiguration */


typedef struct moz_autoconfigure_t
{
	const mrloginparam_t* m_in;
	char*                 m_in_emaildomain;
	char*                 m_in_emaillocalpart;

	mrloginparam_t*       m_out;
	int                   m_out_imap_set, m_out_smtp_set;

	/* currently, we assume there is only one emailProvider tag in the
	file, see example at https://wiki.mozilla.org/Thunderbird:Autoconfiguration:ConfigFileFormat
	moreover, we assume, the returned domains match the one queried.  I've not seen another example (bp).
	However, _if_ the assumptions are wrong, we can add a first saxparser-pass that searches for the correct domain
	and the second pass will look for the index found. */

	#define MOZ_SERVER_IMAP 1
	#define MOZ_SERVER_SMTP 2
	int m_tag_server;

	#define MOZ_HOSTNAME   10
	#define MOZ_PORT       11
	#define MOZ_USERNAME   12
	#define MOZ_SOCKETTYPE 13
	int m_tag_config;

} moz_autoconfigure_t;


static void moz_autoconfigure_starttag_cb(void* userdata, const char* tag, char** attr)
{
	moz_autoconfigure_t* moz_ac = (moz_autoconfigure_t*)userdata;
	const char*          p1;

	if( strcmp(tag, "incomingserver")==0 ) {
		moz_ac->m_tag_server = (moz_ac->m_out_imap_set==0 && (p1=mrattr_find(attr, "type"))!=NULL && strcasecmp(p1, "imap")==0)? MOZ_SERVER_IMAP : 0;
		moz_ac->m_tag_config = 0;
	}
	else if( strcmp(tag, "outgoingserver") == 0 ) {
		moz_ac->m_tag_server = moz_ac->m_out_smtp_set==0? MOZ_SERVER_SMTP : 0;
		moz_ac->m_tag_config = 0;
	}
	else if( strcmp(tag, "hostname") == 0   ) { moz_ac->m_tag_config = MOZ_HOSTNAME; }
	else if( strcmp(tag, "port") == 0       ) { moz_ac->m_tag_config = MOZ_PORT; }
	else if( strcmp(tag, "sockettype") == 0 ) { moz_ac->m_tag_config = MOZ_SOCKETTYPE; }
	else if( strcmp(tag, "username") == 0   ) { moz_ac->m_tag_config = MOZ_USERNAME; }
}


static void moz_autoconfigure_text_cb(void* userdata, const char* text, int len)
{
	moz_autoconfigure_t*   moz_ac = (moz_autoconfigure_t*)userdata;

	char* val = safe_strdup(text);
	mr_trim(val);
	mr_str_replace(&val, "%EMAILADDRESS%",   moz_ac->m_in->m_addr);
	mr_str_replace(&val, "%EMAILLOCALPART%", moz_ac->m_in_emaillocalpart);
	mr_str_replace(&val, "%EMAILDOMAIN%",    moz_ac->m_in_emaildomain);

	if( moz_ac->m_tag_server == MOZ_SERVER_IMAP ) {
		switch( moz_ac->m_tag_config ) {
			case MOZ_HOSTNAME: free(moz_ac->m_out->m_mail_server); moz_ac->m_out->m_mail_server = val; val = NULL; break;
			case MOZ_PORT:                                         moz_ac->m_out->m_mail_port   = atoi(val);       break;
			case MOZ_USERNAME: free(moz_ac->m_out->m_mail_user);   moz_ac->m_out->m_mail_user   = val; val = NULL; break;
			case MOZ_SOCKETTYPE:
				if( strcasecmp(val, "ssl")==0 )      { moz_ac->m_out->m_server_flags |=MR_IMAP_SOCKET_SSL; }
				if( strcasecmp(val, "starttls")==0 ) { moz_ac->m_out->m_server_flags |=MR_IMAP_SOCKET_STARTTLS; }
				if( strcasecmp(val, "plain")==0 )    { moz_ac->m_out->m_server_flags |=MR_IMAP_SOCKET_PLAIN; }
				break;
		}
	}
	else if( moz_ac->m_tag_server == MOZ_SERVER_SMTP ) {
		switch( moz_ac->m_tag_config ) {
			case MOZ_HOSTNAME: free(moz_ac->m_out->m_send_server); moz_ac->m_out->m_send_server = val; val = NULL; break;
			case MOZ_PORT:                                         moz_ac->m_out->m_send_port   = atoi(val);       break;
			case MOZ_USERNAME: free(moz_ac->m_out->m_send_user);   moz_ac->m_out->m_send_user   = val; val = NULL; break;
			case MOZ_SOCKETTYPE:
				if( strcasecmp(val, "ssl")==0 )      { moz_ac->m_out->m_server_flags |=MR_SMTP_SOCKET_SSL; }
				if( strcasecmp(val, "starttls")==0 ) { moz_ac->m_out->m_server_flags |=MR_SMTP_SOCKET_STARTTLS; }
				if( strcasecmp(val, "plain")==0 )    { moz_ac->m_out->m_server_flags |=MR_SMTP_SOCKET_PLAIN; }
				break;
		}
	}

	free(val);
}


static void moz_autoconfigure_endtag_cb(void* userdata, const char* tag)
{
	moz_autoconfigure_t* moz_ac = (moz_autoconfigure_t*)userdata;

	if( strcmp(tag, "incomingserver")==0 ) {
		moz_ac->m_tag_server = 0;
		moz_ac->m_tag_config = 0;
		moz_ac->m_out_imap_set = 1;
	}
	else if( strcmp(tag, "outgoingserver")==0 ) {
		moz_ac->m_tag_server = 0;
		moz_ac->m_tag_config = 0;
		moz_ac->m_out_smtp_set = 1;
	}
	else {
		moz_ac->m_tag_config = 0;
	}
}


static mrloginparam_t* moz_autoconfigure(mrmailbox_t* mailbox, const char* url, const mrloginparam_t* param_in)
{
	char*               xml_raw = NULL;
	moz_autoconfigure_t moz_ac;

	memset(&moz_ac, 0, sizeof(moz_autoconfigure_t));

	if( (xml_raw=read_autoconf_file(mailbox, url))==NULL ) {
		goto cleanup;
	}

	moz_ac.m_in                = param_in;
	moz_ac.m_in_emaillocalpart = safe_strdup(param_in->m_addr); char* p = strchr(moz_ac.m_in_emaillocalpart, '@'); if( p == NULL ) { goto cleanup; } *p = 0;
	moz_ac.m_in_emaildomain    = safe_strdup(p+1);
	moz_ac.m_out               = mrloginparam_new();

	mrsaxparser_t                 saxparser;
	mrsaxparser_init            (&saxparser, &moz_ac);
	mrsaxparser_set_tag_handler (&saxparser, moz_autoconfigure_starttag_cb, moz_autoconfigure_endtag_cb);
	mrsaxparser_set_text_handler(&saxparser, moz_autoconfigure_text_cb);
	mrsaxparser_parse           (&saxparser, xml_raw);

	if( moz_ac.m_out->m_mail_server == NULL
	 || moz_ac.m_out->m_mail_port   == 0
	 || moz_ac.m_out->m_send_server == NULL
	 || moz_ac.m_out->m_send_port   == 0 )
	{
		{ char* r = mrloginparam_get_readable(moz_ac.m_out); mrmailbox_log_warning(mailbox, 0, "Bad or incomplete autoconfig: %s", r); free(r); }

		mrloginparam_unref(moz_ac.m_out); /* autoconfig failed for the given URL */
		moz_ac.m_out = NULL;
		goto cleanup;
	}

cleanup:
	free(xml_raw);
	free(moz_ac.m_in_emaildomain);
	free(moz_ac.m_in_emaillocalpart);
	return moz_ac.m_out; /* may be NULL */
}


/*******************************************************************************
 * Outlook's Autodiscover
 ******************************************************************************/


typedef struct outlk_autodiscover_t
{
	const mrloginparam_t* m_in;

	mrloginparam_t*       m_out;
	int                   m_out_imap_set, m_out_smtp_set;

	/* file format: https://msdn.microsoft.com/en-us/library/bb204278(v=exchg.80).aspx */
	#define  OUTLK_TYPE         1
	#define  OUTLK_SERVER       2
	#define  OUTLK_PORT         3
	#define  OUTLK_SSL          4
	#define  OUTLK_REDIRECTURL  5
	#define _OUTLK_COUNT_       6
	int      m_tag_config;

	char*    m_config[_OUTLK_COUNT_];
	char*    m_redirect;

} outlk_autodiscover_t;


static void outlk_clean_config(outlk_autodiscover_t* outlk_ad)
{
	int i;
	for( i = 0; i < _OUTLK_COUNT_; i++ ) {
		free(outlk_ad->m_config[i]);
		outlk_ad->m_config[i] = NULL;
	}
}


static void outlk_autodiscover_starttag_cb(void* userdata, const char* tag, char** attr)
{
	outlk_autodiscover_t* outlk_ad = (outlk_autodiscover_t*)userdata;

	     if( strcmp(tag, "protocol") == 0    ) { outlk_clean_config(outlk_ad); } /* this also cleans "redirecturl", however, this is not problem as the protocol block is only valid for action "settings". */
	else if( strcmp(tag, "type") == 0        ) { outlk_ad->m_tag_config = OUTLK_TYPE; }
	else if( strcmp(tag, "server") == 0      ) { outlk_ad->m_tag_config = OUTLK_SERVER; }
	else if( strcmp(tag, "port") == 0        ) { outlk_ad->m_tag_config = OUTLK_PORT; }
	else if( strcmp(tag, "ssl") == 0         ) { outlk_ad->m_tag_config = OUTLK_SSL; }
	else if( strcmp(tag, "redirecturl") == 0 ) { outlk_ad->m_tag_config = OUTLK_REDIRECTURL; }
}


static void outlk_autodiscover_text_cb(void* userdata, const char* text, int len)
{
	outlk_autodiscover_t* outlk_ad = (outlk_autodiscover_t*)userdata;

	char* val = safe_strdup(text);
	mr_trim(val);

	free(outlk_ad->m_config[outlk_ad->m_tag_config]);
	outlk_ad->m_config[outlk_ad->m_tag_config] = val;
}


static void outlk_autodiscover_endtag_cb(void* userdata, const char* tag)
{
	outlk_autodiscover_t* outlk_ad = (outlk_autodiscover_t*)userdata;

	if( strcmp(tag, "protocol")==0 )
	{
		/* copy collected confituration to m_out (we have to delay this as we do not know when the <type> tag appears in the sax-stream) */
		if( outlk_ad->m_config[OUTLK_TYPE] )
		{
			int port    = atoi_null_is_0(outlk_ad->m_config[OUTLK_PORT]),
			    ssl_on  = (outlk_ad->m_config[OUTLK_SSL] && strcasecmp(outlk_ad->m_config[OUTLK_SSL], "on" )==0),
			    ssl_off = (outlk_ad->m_config[OUTLK_SSL] && strcasecmp(outlk_ad->m_config[OUTLK_SSL], "off")==0);

			if( strcasecmp(outlk_ad->m_config[OUTLK_TYPE], "imap")==0 && outlk_ad->m_out_imap_set==0 ) {
                outlk_ad->m_out->m_mail_server = strdup_keep_null(outlk_ad->m_config[OUTLK_SERVER]);
                outlk_ad->m_out->m_mail_port   = port;
                     if( ssl_on  ) { outlk_ad->m_out->m_server_flags |= MR_IMAP_SOCKET_SSL;   }
                else if( ssl_off ) { outlk_ad->m_out->m_server_flags |= MR_IMAP_SOCKET_PLAIN; }
                outlk_ad->m_out_imap_set = 1;
			}
			else if( strcasecmp(outlk_ad->m_config[OUTLK_TYPE], "smtp")==0 && outlk_ad->m_out_smtp_set==0 ) {
                outlk_ad->m_out->m_send_server = strdup_keep_null(outlk_ad->m_config[OUTLK_SERVER]);
                outlk_ad->m_out->m_send_port   = port;
                     if( ssl_on  ) { outlk_ad->m_out->m_server_flags |= MR_SMTP_SOCKET_SSL;   }
                else if( ssl_off ) { outlk_ad->m_out->m_server_flags |= MR_SMTP_SOCKET_PLAIN; }
                outlk_ad->m_out_smtp_set = 1;
			}
		}

		outlk_clean_config(outlk_ad);
	}
	outlk_ad->m_tag_config = 0;
}


static mrloginparam_t* outlk_autodiscover(mrmailbox_t* mailbox, const char* url__, const mrloginparam_t* param_in)
{
	char*                 xml_raw = NULL, *url = safe_strdup(url__);
	outlk_autodiscover_t  outlk_ad;
	int                   i;

	for( i = 0; i < 10 /* follow up to 10 xml-redirects (http-redirects are followed in read_autoconf_file() */; i++ )
	{
		memset(&outlk_ad, 0, sizeof(outlk_autodiscover_t));

		if( (xml_raw=read_autoconf_file(mailbox, url))==NULL ) {
			goto cleanup;
		}

		outlk_ad.m_in                = param_in;
		outlk_ad.m_out               = mrloginparam_new();

		mrsaxparser_t                 saxparser;
		mrsaxparser_init            (&saxparser, &outlk_ad);
		mrsaxparser_set_tag_handler (&saxparser, outlk_autodiscover_starttag_cb, outlk_autodiscover_endtag_cb);
		mrsaxparser_set_text_handler(&saxparser, outlk_autodiscover_text_cb);
		mrsaxparser_parse           (&saxparser, xml_raw);

		if( outlk_ad.m_config[OUTLK_REDIRECTURL] && outlk_ad.m_config[OUTLK_REDIRECTURL][0] ) {
			free(url);
			url = safe_strdup(outlk_ad.m_config[OUTLK_REDIRECTURL]);
			mrloginparam_unref(outlk_ad.m_out);
			outlk_clean_config(&outlk_ad);
			free(xml_raw); xml_raw = NULL;
		}
		else {
			break;
		}
	}

	if( outlk_ad.m_out->m_mail_server == NULL
	 || outlk_ad.m_out->m_mail_port   == 0
	 || outlk_ad.m_out->m_send_server == NULL
	 || outlk_ad.m_out->m_send_port   == 0 )
	{
		{ char* r = mrloginparam_get_readable(outlk_ad.m_out); mrmailbox_log_warning(mailbox, 0, "Bad or incomplete autoconfig: %s", r); free(r); }
		mrloginparam_unref(outlk_ad.m_out); /* autoconfig failed for the given URL */
		outlk_ad.m_out = NULL;
		goto cleanup;
	}

cleanup:
	free(url);
	free(xml_raw);
	outlk_clean_config(&outlk_ad);
	return outlk_ad.m_out; /* may be NULL */
}


/*******************************************************************************
 * Main interface
 ******************************************************************************/


/**
 * Configure and connect a mailbox.
 *
 * - Before your call this function, you should set at least `addr` and `mail_pw`
 *   using mrmailbox_set_config().
 *
 * - mrmailbox_configure_and_connect() may take a while, so it might be a good idea to let it run in a non-GUI-thread;
 *   to stop the configuration progress, you can use mrmailbox_stop_ongoing_process().
 *
 * - The function sends out a number of #MR_EVENT_CONFIGURE_PROGRESS events that may be used to create
 *   a progress bar or stuff like that.
 *
 * @memberof mrmailbox_t
 *
 * @param mailbox the mailbox object as created by mrmailbox_new().
 *
 * @return 1=configured and connected,
 *     0=not configured, not explicitly connected, however, an existing connection may still be present
 *
 * There is no need to call this every program start, the result is saved in the
 * database. Instead, you can use mrmailbox_connect() which reuses the configuration
 * and is much faster:
 *
 * ```
 * if( mrmailbox_is_configured(mailbox) ) {
 *     mrmailbox_connect(mailbox); // fast, reuse the configuration
 * }
 * else {
 *     mrmailbox_configure_and_connect(mailbox); // may take a while, typically started in a thread
 * }
 * ```
 */
int mrmailbox_configure_and_connect(mrmailbox_t* mailbox)
{
	int             success = 0, locked = 0, i;
	int             imap_connected_here = 0;

	mrloginparam_t* param = NULL;
	char*           param_domain = NULL; /* just a pointer inside param, must not be freed! */
	char*           param_addr_urlencoded = NULL;
	mrloginparam_t* param_autoconfig = NULL;

	if( mailbox == NULL || mailbox->m_magic != MR_MAILBOX_MAGIC ) {
		return 0;
	}

	if( !mrmailbox_alloc_ongoing(mailbox) ) {
		return 0; /* no cleanup as this would call mrmailbox_free_ongoing() */
	}

	#define PROGRESS(p) \
				if( mr_shall_stop_ongoing ) { goto cleanup; } \
				mailbox->m_cb(mailbox, MR_EVENT_CONFIGURE_PROGRESS, (p), 0);

	if( !mrsqlite3_is_open(mailbox->m_sql) ) {
		mrmailbox_log_error(mailbox, 0, "Cannot configure, database not opened.");
		goto cleanup;
	}

	/* disconnect */
	mrmailbox_disconnect(mailbox);

	mrsqlite3_lock(mailbox->m_sql);
	locked = 1;

		//mrsqlite3_set_config_int__(mailbox->m_sql, "configured", 0); -- NO: we do _not_ reset this flag if it was set once; otherwise the user won't get back to his chats (as an alternative, we could change the UI).  Moreover, and not changeable in the UI, we use this flag to check if we shall search for backups.
		mailbox->m_smtp->m_log_connect_errors = 1;
		mailbox->m_imap->m_log_connect_errors = 1;
		mrjob_kill_action__(mailbox, MRJ_CONNECT_TO_IMAP);

	mrsqlite3_unlock(mailbox->m_sql);
	locked = 0;

	mrmailbox_log_info(mailbox, 0, "Configure ...");

	PROGRESS(0)

	if( mailbox->m_cb(mailbox, MR_EVENT_IS_OFFLINE, 0, 0)!=0 ) {
		mrmailbox_log_error(mailbox, MR_ERR_NONETWORK, NULL);
		goto cleanup;
	}

	PROGRESS(100)

	/* 1.  Load the parameters and check email-address and password
	 **************************************************************************/

	param = mrloginparam_new();

	mrsqlite3_lock(mailbox->m_sql);
	locked = 1;

		mrloginparam_read__(param, mailbox->m_sql, "");

	mrsqlite3_unlock(mailbox->m_sql);
	locked = 0;

	if( param->m_addr == NULL ) {
		mrmailbox_log_error(mailbox, 0, "Please enter the email address.");
		goto cleanup;
	}
	mr_trim(param->m_addr);

	param_domain = strchr(param->m_addr, '@');
	if( param_domain==NULL || param_domain[0]==0 ) {
		mrmailbox_log_error(mailbox, 0, "Bad email-address.");
		goto cleanup;
	}
	param_domain++;

	param_addr_urlencoded = mr_url_encode(param->m_addr);

	/* if no password is given, assume an empty password.
	(in general, unset values are NULL, not the empty string, this allows to use eg. empty user names or empty passwords) */
	if( param->m_mail_pw == NULL ) {
		param->m_mail_pw = safe_strdup(NULL);
	}

	PROGRESS(200)


	/* 2.  Autoconfig
	 **************************************************************************/

	if( param->m_mail_server  == NULL
	 && param->m_mail_port    == 0
	/*&&param->m_mail_user    == NULL -- the user can enter a loginname which is used by autoconfig then */
	 && param->m_send_server  == NULL
	 && param->m_send_port    == 0
	 && param->m_send_user    == NULL
	/*&&param->m_send_pw      == NULL -- the password cannot be auto-configured and is no criterion for autoconfig or not */
	 && param->m_server_flags == 0 )
	{
		/* A.  Search configurations from the domain used in the email-address */
		for( i = 0; i <= 1; i++ ) {
			if( param_autoconfig==NULL ) {
				if( i = 0 ) {
					char* url = mr_mprintf("https://autoconfig.%s/mail/config-v1.1.xml?emailaddress=%s", param_domain, param_addr_urlencoded); /* Thunderbird may or may not use SSL */
					param_autoconfig = moz_autoconfigure(mailbox, url, param);
				{

				if( i = 1 ) {
					char* url = mr_mprintf("http://autoconfig.%s/mail/config-v1.1.xml?emailaddress=%s", param_domain, param_addr_urlencoded); /* Probe http using only generic url (not sending email address to random servers)*/
					param_autoconfig = moz_autoconfigure(mailbox, url, param);
					if( param_autoconfig ) {	/* http probing was successful */
						char* url = mr_mprintf("http://autoconfig.%s/mail/config-v1.1.xml?emailaddress=%s", param_domain, param_addr_urlencoded); /* override using http and personal url */
						param_autoconfig = moz_autoconfigure(mailbox, url, param);
					}
				{
				free(url);
				PROGRESS(300+i*50)
			}
		}

		for( i = 0; i <= 1; i++ ) {
			if( param_autoconfig==NULL ) {
				char* url = mr_mprintf("https://%s%s/autodiscover/autodiscover.xml", i==0?"":"autodiscover.", param_domain); /* Outlook uses always SSL but different domains */
				param_autoconfig = outlk_autodiscover(mailbox, url, param);
				free(url);
				PROGRESS(400+i*50)
			}
		}

		/* B.  If we have no configuration yet, search configuration in Thunderbird's centeral database */
		if( param_autoconfig==NULL )
		{
			char* url = mr_mprintf("https://autoconfig.thunderbird.net/v1.1/%s", param_domain); /* always SSL for Thunderbird's database */
			param_autoconfig = moz_autoconfigure(mailbox, url, param);
			free(url);
			PROGRESS(500)
		}

		/* C.  Do we have any result? */
		if( param_autoconfig )
		{
			{ char* r = mrloginparam_get_readable(param_autoconfig); mrmailbox_log_info(mailbox, 0, "Got autoconfig: %s", r); free(r); }

			if( param_autoconfig->m_mail_user ) {
				free(param->m_mail_user);
				param->m_mail_user= strdup_keep_null(param_autoconfig->m_mail_user);
			}
			param->m_mail_server  = strdup_keep_null(param_autoconfig->m_mail_server); /* all other values are always NULL when entering autoconfig */
			param->m_mail_port    =                  param_autoconfig->m_mail_port;
			param->m_send_server  = strdup_keep_null(param_autoconfig->m_send_server);
			param->m_send_port    =                  param_autoconfig->m_send_port;
			param->m_send_user    = strdup_keep_null(param_autoconfig->m_send_user);
			param->m_server_flags =                  param_autoconfig->m_server_flags;

			/* althoug param_autoconfig's data are no longer needed from, it is important to keep the object as
			we may enter "deep guessing" if we could not read a configuration */
		}
	}


	/* 3.  Internal specials (eg. for uploading to chats-folder etc.)
	 **************************************************************************/

	if( strcasecmp(param_domain, "gmail.com")==0 || strcasecmp(param_domain, "googlemail.com")==0 )
	{
		/* NB: Checking GMa'l too often (<10 Minutes) may result in blocking, says https://github.com/itprojects/InboxPager/blob/HEAD/README.md#gmail-configuration
		Also note https://www.google.com/settings/security/lesssecureapps */
		param->m_server_flags |= MR_AUTH_XOAUTH2 | MR_NO_EXTRA_IMAP_UPLOAD | MR_NO_MOVE_TO_CHATS;
	}


	/* 2.  Fill missing fields with defaults
	 **************************************************************************/

	#define TYPICAL_IMAP_SSL_PORT       993 /* our default */
	#define TYPICAL_IMAP_STARTTLS_PORT  143 /* not used very often but eg. by posteo.de, default for PLAIN */

	#define TYPICAL_SMTP_SSL_PORT       465 /* our default */
	#define TYPICAL_SMTP_STARTTLS_PORT  587 /* also used very often, SSL:STARTTLS is maybe 50:50 */
	#define TYPICAL_SMTP_PLAIN_PORT      25

	if( param->m_mail_server == NULL ) {
		param->m_mail_server = mr_mprintf("imap.%s", param_domain);
	}

	if( param->m_mail_port == 0 ) {
		param->m_mail_port = (param->m_server_flags&(MR_IMAP_SOCKET_STARTTLS|MR_IMAP_SOCKET_PLAIN))?  TYPICAL_IMAP_STARTTLS_PORT : TYPICAL_IMAP_SSL_PORT;
	}

	if( param->m_mail_user == NULL ) {
		param->m_mail_user = safe_strdup(param->m_addr);
	}

	if( param->m_send_server == NULL && param->m_mail_server ) {
		param->m_send_server = safe_strdup(param->m_mail_server);
		if( strncmp(param->m_send_server, "imap.", 5)==0 ) {
			memcpy(param->m_send_server, "smtp", 4);
		}
	}

	if( param->m_send_port == 0 ) {
		param->m_send_port = (param->m_server_flags&MR_SMTP_SOCKET_STARTTLS)?  TYPICAL_SMTP_STARTTLS_PORT :
			((param->m_server_flags&MR_SMTP_SOCKET_PLAIN)? TYPICAL_SMTP_PLAIN_PORT : TYPICAL_SMTP_SSL_PORT);
	}

	if( param->m_send_user == NULL && param->m_mail_user ) {
		param->m_send_user = safe_strdup(param->m_mail_user);
	}

	if( param->m_send_pw == NULL && param->m_mail_pw ) {
		param->m_send_pw = safe_strdup(param->m_mail_pw);
	}

	if( !mr_exactly_one_bit_set(param->m_server_flags&MR_AUTH_FLAGS) )
	{
		param->m_server_flags &= ~MR_AUTH_FLAGS;
		param->m_server_flags |= MR_AUTH_NORMAL;
	}

	if( !mr_exactly_one_bit_set(param->m_server_flags&MR_IMAP_SOCKET_FLAGS) )
	{
		param->m_server_flags &= ~MR_IMAP_SOCKET_FLAGS;
		param->m_server_flags |= (param->m_send_port==TYPICAL_IMAP_STARTTLS_PORT?  MR_IMAP_SOCKET_STARTTLS : MR_IMAP_SOCKET_SSL);
	}

	if( !mr_exactly_one_bit_set(param->m_server_flags&MR_SMTP_SOCKET_FLAGS) )
	{
		param->m_server_flags &= ~MR_SMTP_SOCKET_FLAGS;
		param->m_server_flags |= ( param->m_send_port==TYPICAL_SMTP_STARTTLS_PORT?  MR_SMTP_SOCKET_STARTTLS :
			(param->m_send_port==TYPICAL_SMTP_PLAIN_PORT? MR_SMTP_SOCKET_PLAIN: MR_SMTP_SOCKET_SSL) );
	}


	/* do we have a complete configuration? */
	if( param->m_addr         == NULL
	 || param->m_mail_server  == NULL
	 || param->m_mail_port    == 0
	 || param->m_mail_user    == NULL
	 || param->m_mail_pw      == NULL
	 || param->m_send_server  == NULL
	 || param->m_send_port    == 0
	 || param->m_send_user    == NULL
	 || param->m_send_pw      == NULL
	 || param->m_server_flags == 0 )
	{
		mrmailbox_log_error(mailbox, 0, "Account settings incomplete.");
		goto cleanup;
	}

	PROGRESS(600)

	/* try to connect to IMAP */
	{ char* r = mrloginparam_get_readable(param); mrmailbox_log_info(mailbox, 0, "Trying: %s", r); free(r); }

	if( !mrimap_connect(mailbox->m_imap, param) ) {
		goto cleanup;
	}
	imap_connected_here = 1;

	PROGRESS(800)

	/* try to connect to SMTP - if we did not got an autoconfig, the first try was SSL-465 and we do a second try with STARTTLS-587 */
	if( !mrsmtp_connect(mailbox->m_smtp, param) )  {
		if( param_autoconfig ) {
			goto cleanup;
		}

		PROGRESS(850)

		param->m_server_flags &= ~MR_SMTP_SOCKET_FLAGS;
		param->m_server_flags |=  MR_SMTP_SOCKET_STARTTLS;
		param->m_send_port    =   TYPICAL_SMTP_STARTTLS_PORT;
		{ char* r = mrloginparam_get_readable(param); mrmailbox_log_info(mailbox, 0, "Trying: %s", r); free(r); }

		if( !mrsmtp_connect(mailbox->m_smtp, param) ) {
			goto cleanup;
		}
	}

	PROGRESS(900)

	/* configuration success - write back the configured parameters with the "configured_" prefix; also write the "configured"-flag */
	mrsqlite3_lock(mailbox->m_sql);
	locked = 1;

		mrloginparam_write__(param, mailbox->m_sql, "configured_" /*the trailing underscore is correct*/);
		mrsqlite3_set_config_int__(mailbox->m_sql, "configured", 1);

	mrsqlite3_unlock(mailbox->m_sql);
	locked = 0;

	success = 1;
	mrmailbox_log_info(mailbox, 0, "Configure completed successfully.");

cleanup:
	if( locked ) { mrsqlite3_unlock(mailbox->m_sql); }
	if( !success && imap_connected_here ) {
		mrimap_disconnect(mailbox->m_imap);
	}
	mrloginparam_unref(param);
	mrloginparam_unref(param_autoconfig);
	free(param_addr_urlencoded);

	mrmailbox_free_ongoing(mailbox);
	return success;
}


/**
 * Check if the mailbox is already configured.
 *
 * Typically, for unconfigured mailboxes, the user is prompeted for
 * to enter some settings and mrmailbox_configure_and_connect() is called in a thread then.
 *
 * @memberof mrmailbox_t
 *
 * @param mailbox The mailbox object as created by mrmailbox_new().
 *
 * @return 1=mailbox is configured and mrmailbox_connect() can be called directly as needed,
 *     0=mailbox is not configured and a configuration by mrmailbox_configure_and_connect() is required.
 */
int mrmailbox_is_configured(mrmailbox_t* mailbox)
{
	int is_configured;

	if( mailbox == NULL || mailbox->m_magic != MR_MAILBOX_MAGIC ) {
		return 0;
	}

	if( mrimap_is_connected(mailbox->m_imap) ) { /* if we're connected, we're also configured. this check will speed up the check as no database is involved */
		return 1;
	}

	mrsqlite3_lock(mailbox->m_sql);

		is_configured = mrsqlite3_get_config_int__(mailbox->m_sql, "configured", 0);

	mrsqlite3_unlock(mailbox->m_sql);

	return is_configured? 1 : 0;
}


/*
 * Request an ongoing process to start.
 * Returns 0=process started, 1=not started, there is running another process
 */
static int s_ongoing_running = 0;
int        mr_shall_stop_ongoing = 1; /* the value 1 avoids mrmailbox_stop_ongoing_process() from stopping already stopped threads */
int mrmailbox_alloc_ongoing(mrmailbox_t* mailbox)
{
	if( mailbox == NULL || mailbox->m_magic != MR_MAILBOX_MAGIC ) {
		return 0;
	}

	if( s_ongoing_running || mr_shall_stop_ongoing == 0 ) {
		mrmailbox_log_warning(mailbox, 0, "There is already another ongoing process running.");
		return 0;
	}

	s_ongoing_running     = 1;
	mr_shall_stop_ongoing = 0;
	return 1;
}


/*
 * Frees the process allocated with mrmailbox_alloc_ongoing() - independingly of mr_shall_stop_ongoing.
 * If mrmailbox_alloc_ongoing() fails, this function MUST NOT be called.
 */
void mrmailbox_free_ongoing(mrmailbox_t* mailbox)
{
	if( mailbox == NULL || mailbox->m_magic != MR_MAILBOX_MAGIC ) {
		return;
	}

	s_ongoing_running     = 0;
	mr_shall_stop_ongoing = 1; /* avoids mrmailbox_stop_ongoing_process() to stop the thread */
}


/**
 * Signal an ongoing process to stop.
 *
 * After that, mrmailbox_stop_ongoing_process() returns _without_ waiting
 * for the ongoing process to return.
 *
 * The ongoing process will return ASAP then, however, it may
 * still take a moment.  If in doubt, the caller may also decide to kill the
 * thread after a few seconds; eg. the process may hang in a
 * function not under the control of the core (eg. #MR_EVENT_HTTP_GET). Another
 * reason for mrmailbox_stop_ongoing_process() not to wait is that otherwise it
 * would be GUI-blocking and should be started in another thread then; this
 * would make things even more complicated.
 *
 * Typical ongoing processes are started by mrmailbox_configure_and_connect(),
 * mrmailbox_initiate_key_transfer() or mrmailbox_imex(). As there is always at most only
 * one onging process at the same time, there is no need to define _which_ process to exit.
 *
 * @memberof mrmailbox_t
 *
 * @param mailbox The mailbox object.
 *
 * @return None
 */
void mrmailbox_stop_ongoing_process(mrmailbox_t* mailbox)
{
	if( mailbox == NULL || mailbox->m_magic != MR_MAILBOX_MAGIC ) {
		return;
	}

	if( s_ongoing_running && mr_shall_stop_ongoing==0 )
	{
		mrmailbox_log_info(mailbox, 0, "Signaling the ongoing process to stop ASAP.");
		mr_shall_stop_ongoing = 1;
	}
	else
	{
		mrmailbox_log_info(mailbox, 0, "No ongoing process to stop.");
	}
}
