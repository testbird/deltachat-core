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


#include "dc_context.h"
#include "dc_apeerstate.h"
#include "dc_aheader.h"
#include "dc_hash.h"


/*******************************************************************************
 * dc_apeerstate_t represents the state of an Autocrypt peer - Load/save
 ******************************************************************************/


static void dc_apeerstate_empty(dc_apeerstate_t* peerstate)
{
	if( peerstate == NULL ) {
		return;
	}

	peerstate->m_last_seen           = 0;
	peerstate->m_last_seen_autocrypt = 0;
	peerstate->m_prefer_encrypt      = 0;
	peerstate->m_to_save             = 0;

	free(peerstate->m_addr);
	peerstate->m_addr = NULL;

	free(peerstate->m_public_key_fingerprint);
	peerstate->m_public_key_fingerprint = NULL;

	free(peerstate->m_gossip_key_fingerprint);
	peerstate->m_gossip_key_fingerprint = NULL;

	free(peerstate->m_verified_key_fingerprint);
	peerstate->m_verified_key_fingerprint = NULL;

	if( peerstate->m_public_key ) {
		dc_key_unref(peerstate->m_public_key);
		peerstate->m_public_key = NULL;
	}

	peerstate->m_gossip_timestamp = 0;

	if( peerstate->m_gossip_key ) {
		dc_key_unref(peerstate->m_gossip_key);
		peerstate->m_gossip_key = NULL;
	}

	if( peerstate->m_verified_key ) {
		dc_key_unref(peerstate->m_verified_key);
		peerstate->m_verified_key = NULL;
	}

	peerstate->m_degrade_event = 0;
}


static void dc_apeerstate_set_from_stmt__(dc_apeerstate_t* peerstate, sqlite3_stmt* stmt)
{
	#define PEERSTATE_FIELDS "addr, last_seen, last_seen_autocrypt, prefer_encrypted, public_key, gossip_timestamp, gossip_key, public_key_fingerprint, gossip_key_fingerprint, verified_key, verified_key_fingerprint"
	peerstate->m_addr                     = dc_strdup((char*)sqlite3_column_text  (stmt, 0));
	peerstate->m_last_seen                =                  sqlite3_column_int64 (stmt, 1);
	peerstate->m_last_seen_autocrypt      =                  sqlite3_column_int64 (stmt, 2);
	peerstate->m_prefer_encrypt           =                  sqlite3_column_int   (stmt, 3);
	#define PUBLIC_KEY_COL                                                               4
	peerstate->m_gossip_timestamp         =                  sqlite3_column_int   (stmt, 5);
	#define GOSSIP_KEY_COL                                                               6
	peerstate->m_public_key_fingerprint   = dc_strdup((char*)sqlite3_column_text  (stmt, 7));
	peerstate->m_gossip_key_fingerprint   = dc_strdup((char*)sqlite3_column_text  (stmt, 8));
	#define VERIFIED_KEY_COL                                                             9
	peerstate->m_verified_key_fingerprint = dc_strdup((char*)sqlite3_column_text(stmt, 10));

	if( sqlite3_column_type(stmt, PUBLIC_KEY_COL)!=SQLITE_NULL ) {
		peerstate->m_public_key = dc_key_new();
		dc_key_set_from_stmt(peerstate->m_public_key, stmt, PUBLIC_KEY_COL, DC_KEY_PUBLIC);
	}

	if( sqlite3_column_type(stmt, GOSSIP_KEY_COL)!=SQLITE_NULL ) {
		peerstate->m_gossip_key = dc_key_new();
		dc_key_set_from_stmt(peerstate->m_gossip_key, stmt, GOSSIP_KEY_COL, DC_KEY_PUBLIC);
	}

	if( sqlite3_column_type(stmt, VERIFIED_KEY_COL)!=SQLITE_NULL ) {
		peerstate->m_verified_key = dc_key_new();
		dc_key_set_from_stmt(peerstate->m_verified_key, stmt, VERIFIED_KEY_COL, DC_KEY_PUBLIC);
	}
}


int dc_apeerstate_load_by_addr__(dc_apeerstate_t* peerstate, dc_sqlite3_t* sql, const char* addr)
{
	int           success = 0;
	sqlite3_stmt* stmt;

	if( peerstate==NULL || sql == NULL || addr == NULL ) {
		return 0;
	}

	dc_apeerstate_empty(peerstate);

	stmt = dc_sqlite3_predefine__(sql, SELECT_fields_FROM_acpeerstates_WHERE_addr,
		"SELECT " PEERSTATE_FIELDS
		 " FROM acpeerstates "
		 " WHERE addr=? COLLATE NOCASE;");
	sqlite3_bind_text(stmt, 1, addr, -1, SQLITE_STATIC);
	if( sqlite3_step(stmt) != SQLITE_ROW ) {
		goto cleanup;
	}
	dc_apeerstate_set_from_stmt__(peerstate, stmt);

	success = 1;

cleanup:
	return success;
}


int dc_apeerstate_load_by_fingerprint__(dc_apeerstate_t* peerstate, dc_sqlite3_t* sql, const char* fingerprint)
{
	int           success = 0;
	sqlite3_stmt* stmt;

	if( peerstate==NULL || sql == NULL || fingerprint == NULL ) {
		return 0;
	}

	dc_apeerstate_empty(peerstate);

	stmt = dc_sqlite3_predefine__(sql, SELECT_fields_FROM_acpeerstates_WHERE_fingerprint,
		"SELECT " PEERSTATE_FIELDS
		 " FROM acpeerstates "
		 " WHERE public_key_fingerprint=? COLLATE NOCASE "
		 "    OR gossip_key_fingerprint=? COLLATE NOCASE "
		 " ORDER BY public_key_fingerprint=? DESC;"); // if for, any reasons, different peers have the same key, prefer the peer with the correct public key. should not happen, however.
	sqlite3_bind_text(stmt, 1, fingerprint, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, fingerprint, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, fingerprint, -1, SQLITE_STATIC);
	if( sqlite3_step(stmt) != SQLITE_ROW ) {
		goto cleanup;
	}
	dc_apeerstate_set_from_stmt__(peerstate, stmt);

	success = 1;

cleanup:
	return success;
}


int dc_apeerstate_save_to_db__(const dc_apeerstate_t* peerstate, dc_sqlite3_t* sql, int create)
{
	int           success = 0;
	sqlite3_stmt* stmt;

	if( peerstate==NULL || sql==NULL || peerstate->m_addr==NULL ) {
		return 0;
	}

	if( create ) {
		stmt = dc_sqlite3_predefine__(sql, INSERT_INTO_acpeerstates_a, "INSERT INTO acpeerstates (addr) VALUES(?);");
		sqlite3_bind_text(stmt, 1, peerstate->m_addr, -1, SQLITE_STATIC);
		sqlite3_step(stmt);
	}

	if( (peerstate->m_to_save&DC_SAVE_ALL) || create )
	{
		stmt = dc_sqlite3_predefine__(sql, UPDATE_acpeerstates_SET_lcpp_WHERE_a,
			"UPDATE acpeerstates "
			"   SET last_seen=?, last_seen_autocrypt=?, prefer_encrypted=?, "
			"       public_key=?, gossip_timestamp=?, gossip_key=?, public_key_fingerprint=?, gossip_key_fingerprint=?, verified_key=?, verified_key_fingerprint=? "
			" WHERE addr=?;");
		sqlite3_bind_int64(stmt, 1, peerstate->m_last_seen);
		sqlite3_bind_int64(stmt, 2, peerstate->m_last_seen_autocrypt);
		sqlite3_bind_int64(stmt, 3, peerstate->m_prefer_encrypt);
		sqlite3_bind_blob (stmt, 4, peerstate->m_public_key? peerstate->m_public_key->m_binary : NULL/*results in sqlite3_bind_null()*/, peerstate->m_public_key? peerstate->m_public_key->m_bytes : 0, SQLITE_STATIC);
		sqlite3_bind_int64(stmt, 5, peerstate->m_gossip_timestamp);
		sqlite3_bind_blob (stmt, 6, peerstate->m_gossip_key? peerstate->m_gossip_key->m_binary : NULL/*results in sqlite3_bind_null()*/, peerstate->m_gossip_key? peerstate->m_gossip_key->m_bytes : 0, SQLITE_STATIC);
		sqlite3_bind_text (stmt, 7, peerstate->m_public_key_fingerprint, -1, SQLITE_STATIC);
		sqlite3_bind_text (stmt, 8, peerstate->m_gossip_key_fingerprint, -1, SQLITE_STATIC);
		sqlite3_bind_blob (stmt, 9, peerstate->m_verified_key? peerstate->m_verified_key->m_binary : NULL/*results in sqlite3_bind_null()*/, peerstate->m_verified_key? peerstate->m_verified_key->m_bytes : 0, SQLITE_STATIC);
		sqlite3_bind_text (stmt,10, peerstate->m_verified_key_fingerprint, -1, SQLITE_STATIC);
		sqlite3_bind_text (stmt,11, peerstate->m_addr, -1, SQLITE_STATIC);
		if( sqlite3_step(stmt) != SQLITE_DONE ) {
			goto cleanup;
		}
	}
	else if( peerstate->m_to_save&DC_SAVE_TIMESTAMPS )
	{
		stmt = dc_sqlite3_predefine__(sql, UPDATE_acpeerstates_SET_l_WHERE_a,
			"UPDATE acpeerstates SET last_seen=?, last_seen_autocrypt=?, gossip_timestamp=? WHERE addr=?;");
		sqlite3_bind_int64(stmt, 1, peerstate->m_last_seen);
		sqlite3_bind_int64(stmt, 2, peerstate->m_last_seen_autocrypt);
		sqlite3_bind_int64(stmt, 3, peerstate->m_gossip_timestamp);
		sqlite3_bind_text (stmt, 4, peerstate->m_addr, -1, SQLITE_STATIC);
		if( sqlite3_step(stmt) != SQLITE_DONE ) {
			goto cleanup;
		}
	}

	success = 1;

cleanup:
	return success;
}


/*******************************************************************************
 * Main interface
 ******************************************************************************/


dc_apeerstate_t* dc_apeerstate_new(dc_context_t* context)
{
	dc_apeerstate_t* peerstate = NULL;

	if( (peerstate=calloc(1, sizeof(dc_apeerstate_t)))==NULL ) {
		exit(43); /* cannot allocate little memory, unrecoverable error */
	}

	peerstate->m_context = context;

	return peerstate;
}


void dc_apeerstate_unref(dc_apeerstate_t* peerstate)
{
	if( peerstate==NULL ) {
		return;
	}

	free(peerstate->m_addr);
	dc_key_unref(peerstate->m_public_key);
	dc_key_unref(peerstate->m_gossip_key);
	free(peerstate);
}


/**
 * Render an Autocrypt-Gossip header value.  The contained key is either
 * m_public_key or m_gossip_key if m_public_key is NULL.
 *
 * @memberof dc_apeerstate_t
 *
 * @param peerstate The peerstate object.
 *
 * @return String that can be be used directly in an `Autocrypt-Gossip:` statement,
 *     `Autocrypt-Gossip:` is _not_ included in the returned string. If there
 *     is not key for the peer that can be gossiped, NULL is returned.
 */
char* dc_apeerstate_render_gossip_header(const dc_apeerstate_t* peerstate, int min_verified)
{
	char*         ret = NULL;
	dc_aheader_t* autocryptheader = dc_aheader_new();

	if( peerstate == NULL || peerstate->m_addr == NULL ) {
		goto cleanup;
	}

	autocryptheader->m_prefer_encrypt = DC_PE_NOPREFERENCE; /* the spec says, we SHOULD NOT gossip this flag */
	autocryptheader->m_addr           = dc_strdup(peerstate->m_addr);
	autocryptheader->m_public_key     = dc_key_ref(dc_apeerstate_peek_key(peerstate, min_verified)); /* may be NULL */

	ret = dc_aheader_render(autocryptheader);

cleanup:
	dc_aheader_unref(autocryptheader);
	return ret;
}


/**
 * Return either m_public_key or m_gossip_key if m_public_key is null or not verified.
 * The function does not check if the keys are valid but the caller can assume
 * the returned key has data.
 *
 * This function does not do the Autocrypt encryption recommendation; it just
 * returns a key that can be used.
 *
 * @memberof dc_apeerstate_t
 *
 * @param peerstate The peerstate object.
 * @param min_verified The minimal verification criterion the key should match.
 *     Typically either DC_NOT_VERIFIED (0) if there is no need for the key being verified
 *     or DC_BIDIRECT_VERIFIED (2) for bidirectional verification requirement.
 *
 * @return m_public_key or m_gossip_key, NULL if nothing is available.
 *     the returned pointer MUST NOT be unref()'d.
 */
dc_key_t* dc_apeerstate_peek_key(const dc_apeerstate_t* peerstate, int min_verified)
{
	if(  peerstate == NULL
	 || (peerstate->m_public_key && (peerstate->m_public_key->m_binary==NULL || peerstate->m_public_key->m_bytes<=0))
	 || (peerstate->m_gossip_key && (peerstate->m_gossip_key->m_binary==NULL || peerstate->m_gossip_key->m_bytes<=0))
	 || (peerstate->m_verified_key && (peerstate->m_verified_key->m_binary==NULL || peerstate->m_verified_key->m_bytes<=0)) ) {
		return NULL;
	}

	if( min_verified )
	{
		return peerstate->m_verified_key;
	}

	if( peerstate->m_public_key )
	{
		return peerstate->m_public_key;
	}

	return peerstate->m_gossip_key;
}


/*******************************************************************************
 * Change state
 ******************************************************************************/


int dc_apeerstate_init_from_header(dc_apeerstate_t* peerstate, const dc_aheader_t* header, time_t message_time)
{
	if( peerstate == NULL || header == NULL ) {
		return 0;
	}

	dc_apeerstate_empty(peerstate);
	peerstate->m_addr                = dc_strdup(header->m_addr);
	peerstate->m_last_seen           = message_time;
	peerstate->m_last_seen_autocrypt = message_time;
	peerstate->m_to_save             = DC_SAVE_ALL;
	peerstate->m_prefer_encrypt      = header->m_prefer_encrypt;

	peerstate->m_public_key = dc_key_new();
	dc_key_set_from_key(peerstate->m_public_key, header->m_public_key);
	dc_apeerstate_recalc_fingerprint(peerstate);

	return 1;
}


int dc_apeerstate_init_from_gossip(dc_apeerstate_t* peerstate, const dc_aheader_t* gossip_header, time_t message_time)
{
	if( peerstate == NULL || gossip_header == NULL ) {
		return 0;
	}

	dc_apeerstate_empty(peerstate);
	peerstate->m_addr                = dc_strdup(gossip_header->m_addr);
	peerstate->m_gossip_timestamp    = message_time;
	peerstate->m_to_save             = DC_SAVE_ALL;

	peerstate->m_gossip_key = dc_key_new();
	dc_key_set_from_key(peerstate->m_gossip_key, gossip_header->m_public_key);
	dc_apeerstate_recalc_fingerprint(peerstate);

	return 1;
}


int dc_apeerstate_degrade_encryption(dc_apeerstate_t* peerstate, time_t message_time)
{
	if( peerstate==NULL ) {
		return 0;
	}

	if( peerstate->m_prefer_encrypt == DC_PE_MUTUAL ) {
		peerstate->m_degrade_event |= DC_DE_ENCRYPTION_PAUSED;
	}

	peerstate->m_prefer_encrypt = DC_PE_RESET;
	peerstate->m_last_seen      = message_time; /*last_seen_autocrypt is not updated as there was not Autocrypt:-header seen*/
	peerstate->m_to_save        = DC_SAVE_ALL;

	return 1;
}


void dc_apeerstate_apply_header(dc_apeerstate_t* peerstate, const dc_aheader_t* header, time_t message_time)
{
	if( peerstate==NULL || header==NULL
	 || peerstate->m_addr==NULL
	 || header->m_addr==NULL || header->m_public_key->m_binary==NULL
	 || strcasecmp(peerstate->m_addr, header->m_addr)!=0 ) {
		return;
	}

	if( message_time > peerstate->m_last_seen_autocrypt )
	{
		peerstate->m_last_seen           = message_time;
		peerstate->m_last_seen_autocrypt = message_time;
		peerstate->m_to_save             |= DC_SAVE_TIMESTAMPS;

		if( (header->m_prefer_encrypt==DC_PE_MUTUAL || header->m_prefer_encrypt==DC_PE_NOPREFERENCE) /*this also switches from DC_PE_RESET to DC_PE_NOPREFERENCE, which is just fine as the function is only called _if_ the Autocrypt:-header is preset at all */
		 &&  header->m_prefer_encrypt != peerstate->m_prefer_encrypt )
		{
			if( peerstate->m_prefer_encrypt == DC_PE_MUTUAL && header->m_prefer_encrypt != DC_PE_MUTUAL ) {
				peerstate->m_degrade_event |= DC_DE_ENCRYPTION_PAUSED;
			}

			peerstate->m_prefer_encrypt = header->m_prefer_encrypt;
			peerstate->m_to_save |= DC_SAVE_ALL;
		}

		if( peerstate->m_public_key == NULL ) {
			peerstate->m_public_key = dc_key_new();
		}

		if( !dc_key_equals(peerstate->m_public_key, header->m_public_key) )
		{
			dc_key_set_from_key(peerstate->m_public_key, header->m_public_key);
			dc_apeerstate_recalc_fingerprint(peerstate);
			peerstate->m_to_save |= DC_SAVE_ALL;
		}
	}
}


void dc_apeerstate_apply_gossip(dc_apeerstate_t* peerstate, const dc_aheader_t* gossip_header, time_t message_time)
{
	if( peerstate==NULL || gossip_header==NULL
	 || peerstate->m_addr==NULL
	 || gossip_header->m_addr==NULL || gossip_header->m_public_key->m_binary==NULL
	 || strcasecmp(peerstate->m_addr, gossip_header->m_addr)!=0 ) {
		return;
	}

	if( message_time > peerstate->m_gossip_timestamp )
	{
		peerstate->m_gossip_timestamp    = message_time;
		peerstate->m_to_save             |= DC_SAVE_TIMESTAMPS;

		if( peerstate->m_gossip_key == NULL ) {
			peerstate->m_gossip_key = dc_key_new();
		}

		if( !dc_key_equals(peerstate->m_gossip_key, gossip_header->m_public_key) )
		{
			dc_key_set_from_key(peerstate->m_gossip_key, gossip_header->m_public_key);
			dc_apeerstate_recalc_fingerprint(peerstate);
			peerstate->m_to_save |= DC_SAVE_ALL;
		}
	}
}


/*
 * Recalculate the fingerprints for the keys.
 *
 * If the fingerprint has changed, the verified-state is reset.
 *
 * An explicit call to this function from outside this class is only needed
 * for database updates; the dc_apeerstate_init_*() and dc_apeerstate_apply_*()
 * functions update the fingerprint automatically as needed.
 */
int dc_apeerstate_recalc_fingerprint(dc_apeerstate_t* peerstate)
{
	int            success = 0;
	char*          old_public_fingerprint = NULL, *old_gossip_fingerprint = NULL;

	if( peerstate == NULL ) {
		goto cleanup;
	}

	if( peerstate->m_public_key )
	{
		old_public_fingerprint = peerstate->m_public_key_fingerprint;
		peerstate->m_public_key_fingerprint = dc_key_get_fingerprint(peerstate->m_public_key); /* returns the empty string for errors, however, this should be saved as well as it represents an erroneous key */

		if( old_public_fingerprint == NULL
		 || old_public_fingerprint[0] == 0
		 || peerstate->m_public_key_fingerprint == NULL
		 || peerstate->m_public_key_fingerprint[0] == 0
		 || strcasecmp(old_public_fingerprint, peerstate->m_public_key_fingerprint) != 0 )
		{
			peerstate->m_to_save  |= DC_SAVE_ALL;

			if( old_public_fingerprint && old_public_fingerprint[0] ) { // no degrade event when we recveive just the initial fingerprint
				peerstate->m_degrade_event |= DC_DE_FINGERPRINT_CHANGED;
			}
		}
	}

	if( peerstate->m_gossip_key )
	{
		old_gossip_fingerprint = peerstate->m_gossip_key_fingerprint;
		peerstate->m_gossip_key_fingerprint = dc_key_get_fingerprint(peerstate->m_gossip_key); /* returns the empty string for errors, however, this should be saved as well as it represents an erroneous key */

		if( old_gossip_fingerprint == NULL
		 || old_gossip_fingerprint[0] == 0
		 || peerstate->m_gossip_key_fingerprint == NULL
		 || peerstate->m_gossip_key_fingerprint[0] == 0
		 || strcasecmp(old_gossip_fingerprint, peerstate->m_gossip_key_fingerprint) != 0 )
		{
			peerstate->m_to_save  |= DC_SAVE_ALL;

			if( old_gossip_fingerprint && old_gossip_fingerprint[0] ) { // no degrade event when we recveive just the initial fingerprint
				peerstate->m_degrade_event |= DC_DE_FINGERPRINT_CHANGED;
			}
		}
	}

	success = 1;

cleanup:
	free(old_public_fingerprint);
	free(old_gossip_fingerprint);
	return success;
}


/**
 * If the fingerprint of the peerstate equals the given fingerprint, the
 * peerstate is marked as being verified.
 *
 * The given fingerprint is present only to ensure the peer has not changed
 * between fingerprint comparison and calling this function.
 *
 * @memberof dc_apeerstate_t
 *
 * @param peerstate The peerstate object.
 * @param which_key Which key should be marked as being verified? DC_PS_GOSSIP_KEY (1) or DC_PS_PUBLIC_KEY (2)
 * @param fingerprint Fingerprint expected in the object
 * @param verified DC_BIDIRECT_VERIFIED (2): contact verfied in both directions
 *
 * @return 1=the given fingerprint is equal to the peer's fingerprint and
 *     the verified-state is set; you should call dc_apeerstate_save_to_db__()
 *     to permanently store this state.
 *     0=the given fingerprint is not eqial to the peer's fingerprint,
 *     verified-state not changed.
 */
int dc_apeerstate_set_verified(dc_apeerstate_t* peerstate, int which_key, const char* fingerprint, int verified)
{
	int success = 0;

	if( peerstate == NULL
	 || (which_key!=DC_PS_GOSSIP_KEY && which_key!=DC_PS_PUBLIC_KEY)
	 || (verified!=DC_BIDIRECT_VERIFIED) ) {
		goto cleanup;
	}

	if( which_key == DC_PS_PUBLIC_KEY
	 && peerstate->m_public_key_fingerprint != NULL
	 && peerstate->m_public_key_fingerprint[0] != 0
	 && fingerprint[0] != 0
	 && strcasecmp(peerstate->m_public_key_fingerprint, fingerprint) == 0 )
	{
		peerstate->m_to_save                 |= DC_SAVE_ALL;
		peerstate->m_verified_key             = dc_key_ref(peerstate->m_public_key);
		peerstate->m_verified_key_fingerprint = dc_strdup(peerstate->m_public_key_fingerprint);
		success                               = 1;
	}

	if( which_key == DC_PS_GOSSIP_KEY
	 && peerstate->m_gossip_key_fingerprint != NULL
	 && peerstate->m_gossip_key_fingerprint[0] != 0
	 && fingerprint[0] != 0
	 && strcasecmp(peerstate->m_gossip_key_fingerprint, fingerprint) == 0 )
	{
		peerstate->m_to_save                 |= DC_SAVE_ALL;
		peerstate->m_verified_key             = dc_key_ref(peerstate->m_gossip_key);
		peerstate->m_verified_key_fingerprint = dc_strdup(peerstate->m_gossip_key_fingerprint);
		success                               = 1;
	}

cleanup:
	return success;
}


int dc_apeerstate_has_verified_key(const dc_apeerstate_t* peerstate, const dc_hash_t* fingerprints)
{
	if( peerstate == NULL || fingerprints == NULL ) {
		return 0;
	}

	if( peerstate->m_verified_key
	 && peerstate->m_verified_key_fingerprint
	 && dc_hash_find_str(fingerprints, peerstate->m_verified_key_fingerprint) ) {
		return 1;
	}

	return 0;
}
