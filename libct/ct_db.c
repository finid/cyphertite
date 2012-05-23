/*
 * Copyright (c) 2010-2012 Conformal Systems LLC <info@conformal.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <clog.h>
#include <exude.h>
#include <sqlite3.h>

#include "ct_types.h"
#include "ct_db.h"

static int		 ctdb_open(struct ctdb_state *);
static void		ctdb_cleanup(struct ctdb_state *);
static int		ctdb_create(struct ctdb_state *);
static int		ctdb_check_db_mode(struct ctdb_state *);

#define OPS_PER_TRANSACTION	(100)
struct ctdb_state {
	sqlite3			*ctdb_db;
	char			*ctdb_dbfile;
	sqlite3_stmt		*ctdb_stmt_lookup;
	sqlite3_stmt		*ctdb_stmt_insert;
	int			 ctdb_crypt;
	int			 ctdb_genid;
	int			 ctdb_in_transaction;
	int			 ctdb_trans_commit_rem;
};



struct ctdb_state *
ctdb_setup(const char *path, int crypt_enabled)
{
	struct ctdb_state	*state;
	if (path == NULL)
		return (NULL);
	state = e_calloc(1, sizeof(*state));

	state->ctdb_genid = -1;
	state->ctdb_crypt = crypt_enabled;
	state->ctdb_dbfile = e_strdup(path);
	if (ctdb_open(state) != 0) {
		e_free(&state->ctdb_dbfile);
		e_free(&state);
	}
	return (state);
}

void
ctdb_shutdown(struct ctdb_state *state)
{
	if (state == NULL)
		return;

	ctdb_cleanup(state);
	if (state->ctdb_dbfile)
		e_free(&state->ctdb_dbfile);
	e_free(&state);
}

int
ctdb_create(struct ctdb_state *state)
{
	int			rc;
	char			*errmsg = NULL;
	char			sql[4096];

	if (state->ctdb_genid == -1)
		state->ctdb_genid = 0;
	rc = sqlite3_open_v2(state->ctdb_dbfile, &state->ctdb_db,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	if (rc)
		return (rc);

	if (state->ctdb_crypt)
		snprintf(sql, sizeof sql,
		    "CREATE TABLE digests (sha BLOB(%d)"
		    " PRIMARY KEY UNIQUE,"
		    " csha BLOB(%d), iv BLOB(%d));",
		    SHA_DIGEST_LENGTH, SHA_DIGEST_LENGTH,
		    CT_IV_LEN);
	else
		snprintf(sql, sizeof sql,
		    "CREATE TABLE digests (sha BLOB(%d)"
		    " PRIMARY KEY UNIQUE);",
		    SHA_DIGEST_LENGTH);

	CNDBG(CT_LOG_DB, "sql: %s", sql);
	rc = sqlite3_exec(state->ctdb_db, sql, NULL, 0, &errmsg);
	if (rc) {
		CNDBG(CT_LOG_DB, "create failed: %s", errmsg);
		state->ctdb_db = NULL;
		return (rc);
	}
	snprintf(sql, sizeof sql,
	    "CREATE TABLE mode (crypto TEXT, version INTEGER);");
	rc = sqlite3_exec(state->ctdb_db, sql, NULL, 0, &errmsg);
	if (rc) {
		CNDBG(CT_LOG_DB, "mode table creation failed");
		sqlite3_close(state->ctdb_db);
		state->ctdb_db = NULL;
		return (rc);
	}
	snprintf(sql, sizeof sql,
	    "insert into mode (crypto) VALUES ('%c');",
		state->ctdb_crypt ? 'Y': 'N');
	rc = sqlite3_exec(state->ctdb_db, sql, NULL, 0, &errmsg);
	if (rc) {
		CNDBG(CT_LOG_DB, "mode table init failed");
		sqlite3_close(state->ctdb_db);
		state->ctdb_db = NULL;
		return (rc);
	}

	snprintf(sql, sizeof sql,
	    "CREATE TABLE genid (value INTEGER);");
	rc = sqlite3_exec(state->ctdb_db, sql, NULL, 0, &errmsg);
	if (rc) {
		CNDBG(CT_LOG_DB, "gendi table creation failed");
		sqlite3_close(state->ctdb_db);
		state->ctdb_db = NULL;
		return (rc);
	}
	snprintf(sql, sizeof sql,
	    "insert into genid (value) VALUES (%d);", state->ctdb_genid);
	rc = sqlite3_exec(state->ctdb_db, sql, NULL, 0, &errmsg);
	if (rc) {
		CNDBG(CT_LOG_DB, "mode table init failed");
		sqlite3_close(state->ctdb_db);
		state->ctdb_db = NULL;
		return (rc);
	}

	return (SQLITE_OK);
}

int
ctdb_check_db_mode(struct ctdb_state *state)
{
	sqlite3_stmt		*stmt;
	char			*p, wanted;
	int			rc, rv = 0, curgenid;

	CNDBG(CT_LOG_DB, "ctdb mode %d\n", state->ctdb_crypt);
	if (sqlite3_prepare_v2(state->ctdb_db, "SELECT crypto FROM mode",
	    -1, &stmt, NULL)) {
		CNDBG(CT_LOG_DB, "can't prepare mode query statement");
		goto fail;
	}
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_DONE) {
		CNDBG(CT_LOG_DB, "ctdb mode not found");
		goto fail;
	} else if (rc != SQLITE_ROW)
		CNDBG(CT_LOG_DB, "could not step(%d) %d %d %s",
		    __LINE__, rc,
		    sqlite3_extended_errcode(state->ctdb_db),
		    sqlite3_errmsg(state->ctdb_db));
		goto fail;
	p = (char *)sqlite3_column_text(stmt, 0);
	if (p) {
		wanted =  state->ctdb_crypt ? 'Y' : 'N';
		if (sqlite3_column_bytes(stmt, 0) != 1) {
			CNDBG(CT_LOG_DB, "ctdb invalid length of column 1");
			goto fail;
		}
		CNDBG(CT_LOG_DB, "ctdb crypto mode %c %c", p[0], wanted);
		if (p[0] != wanted) {
			CNDBG(CT_LOG_DB, "ctdb crypto mode differs %c %c",
			    p[0], wanted);
			goto fail;
		}
	}
	if (sqlite3_finalize(stmt))
		goto fail;

	if (sqlite3_prepare_v2(state->ctdb_db, "SELECT value FROM genid",
	    -1, &stmt, NULL)) {
		CNDBG(CT_LOG_DB, "old format db detected, reseting db");
		goto fail;
	}
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_DONE) {
		CNDBG(CT_LOG_DB, "ctdb genid not found");
		goto fail;
	} else if (rc != SQLITE_ROW) {
		CNDBG(CT_LOG_DB, "could not step(%d) %d %d %s", __LINE__,
		    rc, sqlite3_extended_errcode(state->ctdb_db),
		    sqlite3_errmsg(state->ctdb_db));
		goto fail;
	}
	curgenid = sqlite3_column_int(stmt, 0);

	if (state->ctdb_genid == -1 || state->ctdb_genid == curgenid) {
		state->ctdb_genid = curgenid;
	} else {
		CNDBG(CT_LOG_DB, "ctdb genid is %d, wanted %d", curgenid,
		    state->ctdb_genid);
		goto fail;
	}

	rv = 1;
fail:
	/* not much we can do if this fails */
	if (sqlite3_finalize(stmt))
		CNDBG(CT_LOG_DB, "can't finalize verification lookup");
	return rv;
}

void
ctdb_reopendb(struct ctdb_state *state, int genid)
{
	if (state == NULL)
		return;

	ctdb_cleanup(state);
	unlink(state->ctdb_dbfile);
	state->ctdb_genid = genid;
	if (ctdb_open(state) != 0) {
		/* XXX free this? */
		e_free(&state->ctdb_dbfile);
		e_free(&state);
	}
}

int
ctdb_open(struct ctdb_state *state)
{
	int			rc;
	int			retry = 1;
	char			*psql;

do_retry:
	rc = sqlite3_open_v2(state->ctdb_dbfile, &state->ctdb_db,
	    SQLITE_OPEN_READWRITE, NULL);
	if (rc == SQLITE_CANTOPEN) {
		CNDBG(CT_LOG_DB, "db file doesn't exist, creating it");
		rc = ctdb_create(state);
		if (rc != SQLITE_OK)
			return 1;
	}
	if (ctdb_check_db_mode(state) == 0) {
		if (retry) {
			retry = 0;
			/* db is in incorrect mode, delete it and try again */
			CNDBG(CT_LOG_DB, "db file wrong mode, removing it");
			sqlite3_close(state->ctdb_db);
			unlink(state->ctdb_dbfile);
			goto do_retry;
		} else {
			/* db recreated in incorrect mode!?! */
			ctdb_cleanup(state);
			return (1);
		}

	}

	/* prepare query here based on crypt mode */
	if (state->ctdb_crypt) {
		psql = "SELECT csha, iv FROM digests WHERE sha=?";
	} else {
		psql = "SELECT sha FROM digests WHERE sha=?";
	}

	if (sqlite3_prepare(state->ctdb_db, psql,
	    -1, &state->ctdb_stmt_lookup, NULL)) {
		CNDBG(CT_LOG_DB, "can't prepare select statement");
		ctdb_cleanup(state);
		return 1;
	}
	CNDBG(CT_LOG_DB, "ctdb_stmt_lookup %p", state->ctdb_stmt_lookup);
	if (state->ctdb_crypt) {
		psql = "INSERT INTO digests(sha, csha, iv) values(?, ?, ?)";
	} else {
		psql = "INSERT INTO digests(sha) values(?)";
	}
	if (sqlite3_prepare_v2(state->ctdb_db, psql,
	    -1, &state->ctdb_stmt_insert, NULL)) {
		CNDBG(CT_LOG_DB, "can not prepare insert statement %s", psql);
		ctdb_cleanup(state);
		return 1;
	}
	CNDBG(CT_LOG_DB, "ctdb_stmt_insert %p", state->ctdb_stmt_insert);

	return 0;
}

void
ctdb_cleanup(struct ctdb_state *state)
{
	char			*errmsg;

	CNDBG(CT_LOG_DB, "cleaning up ctdb");
	if (state->ctdb_in_transaction) {
		state->ctdb_in_transaction = 0;
		if (sqlite3_exec(state->ctdb_db, "commit", NULL, 0, &errmsg))
			CNDBG(CT_LOG_DB, "can't commit %s", errmsg);
	}
	if (state->ctdb_stmt_lookup != NULL) {
		if (sqlite3_finalize(state->ctdb_stmt_lookup))
			CNDBG(CT_LOG_DB, "can't finalize lookup");
	}
	if (state->ctdb_stmt_insert != NULL) {
		if (sqlite3_finalize(state->ctdb_stmt_insert))
			CNDBG(CT_LOG_DB, "can't finalize insert");
	}

	if (state->ctdb_db != NULL)
		sqlite3_close(state->ctdb_db);
}

int
ctdb_get_genid(struct ctdb_state *state)
{
	if (state == NULL || state->ctdb_db == NULL)
		return (-1);
	return (state->ctdb_genid);
}

int
ctdb_lookup_sha(struct ctdb_state *state, uint8_t *sha_k, uint8_t *sha_v,
     uint8_t *iv)
{
	char			shat[SHA_DIGEST_STRING_LENGTH];
	int			rv, rc;
	uint8_t			*p;
	char			*errmsg;
	sqlite3_stmt		*stmt;

	rv = 0;

	if (state == NULL)
		return rv;

	stmt = state->ctdb_stmt_lookup;

	if (state->ctdb_in_transaction == 0) {
		rc = sqlite3_exec(state->ctdb_db, "begin transaction", NULL,
		     0, &errmsg);
		if (rc) {
			CNDBG(CT_LOG_DB, "can't begin transaction: %s",
			    errmsg);
			return (rv);
		}
		state->ctdb_in_transaction = 1;
		state->ctdb_trans_commit_rem = OPS_PER_TRANSACTION;
	}

	if (clog_mask_is_set(CT_LOG_DB)) {
		ct_sha1_encode(sha_k, shat);
		CNDBG(CT_LOG_DB, "looking for bin %s", shat);
	}
	if (sqlite3_bind_blob(stmt, 1, sha_k, SHA_DIGEST_LENGTH,
	    SQLITE_STATIC)) {
		CNDBG(CT_LOG_DB, "could not sha");
		return (rv);
	}

	rc = sqlite3_step(stmt);
	if (rc == SQLITE_DONE) {
		CNDBG(CT_LOG_DB, "not found");
		sqlite3_reset(stmt);
		return rv;
	} else if (rc != SQLITE_ROW) {
		CNDBG(CT_LOG_DB, "could not step(%d) %d %d %s",
		    __LINE__, rc,
		    sqlite3_extended_errcode(state->ctdb_db),
		    sqlite3_errmsg(state->ctdb_db));
		sqlite3_reset(stmt);
		return rv;
	}

	CNDBG(CT_LOG_DB, "found");

	p = (uint8_t *)sqlite3_column_blob(stmt, 0);
	if (p) {
		if (sqlite3_column_bytes(stmt, 0) !=
		    SHA_DIGEST_LENGTH) {
			CNDBG(CT_LOG_DB, "invalid blob size");
			sqlite3_reset(stmt);
			return rv;
		}

		if (clog_mask_is_set(CT_LOG_DB)) {
			ct_sha1_encode(p, shat);
			CNDBG(CT_LOG_DB, "found bin %s", shat);
		}

		rv = 1;
		bcopy (p, sha_v, SHA_DIGEST_LENGTH);
	} else {
		CNDBG(CT_LOG_DB, "no bin found");
	}
	if (state->ctdb_crypt) {
		p = (uint8_t *)sqlite3_column_blob(stmt, 1);
		if (p) {
			if (sqlite3_column_bytes(stmt, 1) != CT_IV_LEN) {
				CNDBG(CT_LOG_DB, "invalid blob size");
				sqlite3_reset(stmt);
				rv = 0;
				return rv;
			}
			if (clog_mask_is_set(CT_LOG_DB)) {
				ct_sha1_encode(p, shat);
				CNDBG(CT_LOG_DB, "found iv (prefix) %s", shat);
			}

			bcopy (p, iv, CT_IV_LEN);
		} else {
			CNDBG(CT_LOG_DB, "no iv found");
			rv = 0;
		}
	}
	sqlite3_reset(stmt);

	state->ctdb_trans_commit_rem--;
	if (state->ctdb_trans_commit_rem <= 0) {
		rc = sqlite3_exec(state->ctdb_db, "commit", NULL, 0, &errmsg);
		if (rc)
			CNDBG(CT_LOG_DB, "can't commit %s", errmsg);
		state->ctdb_in_transaction = 0;
	}

	return rv;
}

int
ctdb_insert_sha(struct ctdb_state *state, uint8_t *sha_k, uint8_t *sha_v, uint8_t *iv)
{
	char			shatk[SHA_DIGEST_STRING_LENGTH];
	char			shatv[SHA_DIGEST_STRING_LENGTH];
	int			rv, rc;
	char			*errmsg;
	sqlite3_stmt		*stmt;

	rv = 0;

	if (state == NULL)
		return rv;

	if (state->ctdb_stmt_insert == NULL) {
		if (state->ctdb_crypt) {
			if (sqlite3_prepare_v2(state->ctdb_db,
			    "insert into digests(sha, csha, iv)"
			    " values(?, ?, ?)",
			    -1, &stmt, NULL)) {
				CNDBG(CT_LOG_DB, "can not prepare insert "
				    "statement");
				return rv;
			}
		} else {
			if (sqlite3_prepare_v2(state->ctdb_db,
			    "insert into digests(sha) values(?)",
			    -1, &stmt, NULL)) {
				CNDBG(CT_LOG_DB, "can not prepare insert "
				    "statement");
				return rv;
			}
		}
		state->ctdb_stmt_insert = stmt;
	} else
		stmt = state->ctdb_stmt_insert;

	if (state->ctdb_in_transaction == 0) {
		CNDBG(CT_LOG_DB, "NEW transaction");
		rc = sqlite3_exec(state->ctdb_db, "begin transaction", NULL,
		    0, &errmsg);
		if (rc) {
			CNDBG(CT_LOG_DB, "can not begin %s", errmsg);
			return rv;
		}

		state->ctdb_in_transaction = 1;
		state->ctdb_trans_commit_rem = OPS_PER_TRANSACTION;
	}

	if (clog_mask_is_set(CT_LOG_DB)) {
		ct_sha1_encode(sha_k, shatk);
		if (sha_v == NULL)
			shatv[0] = '\0';
		else
			ct_sha1_encode(sha_v, shatv);
		CNDBG(CT_LOG_DB, "inserting for bin %s, %s", shatk, shatv);
	}
	if (sqlite3_bind_blob(stmt, 1, sha_k, SHA_DIGEST_LENGTH,
	    SQLITE_STATIC)) {
		CNDBG(CT_LOG_DB, "could not bind sha_k");
		return rv;
	}
	if (state->ctdb_crypt) {
		if (sha_v == NULL || iv == NULL)
			CABORTX("crypt mode, but no sha_v/iv");
		if (sqlite3_bind_blob(stmt, 2, sha_v,
		    SHA_DIGEST_LENGTH, SQLITE_STATIC)) {
			CNDBG(CT_LOG_DB, "could not bind sha_v");
			sqlite3_reset(stmt);
			return rv;
		}

		if (sqlite3_bind_blob(stmt, 3, iv,
		    CT_IV_LEN, SQLITE_STATIC)) {
			CNDBG(CT_LOG_DB, "could not bind iv ");
			sqlite3_reset(stmt);
			return rv;
		}
	}

	rc = sqlite3_step(stmt);
	if (rc == SQLITE_DONE) {
		CNDBG(CT_LOG_DB, "insert completed");
		rv = 1;
	} else if (rc != SQLITE_CONSTRAINT) { 
		CNDBG(CT_LOG_DB, "insert failed %d %d [%s]", rc,
		    sqlite3_extended_errcode(state->ctdb_db),
		    sqlite3_errmsg(state->ctdb_db));
	} else  {
		CNDBG(CT_LOG_DB, "sha already exists");
	}

	sqlite3_reset(stmt);

	/* inserts are more 'costly' than reads */
	state->ctdb_trans_commit_rem -= 4;
	if (state->ctdb_trans_commit_rem <= 0) {
		if ((rc = sqlite3_exec(state->ctdb_db, "commit", NULL,
		    0, &errmsg)) != 0)
			CNDBG(CT_LOG_DB, "can't commit %s", errmsg);
		/* ctdb is just a cache, succeed anyway */
		state->ctdb_in_transaction = 0;
	}

	return rv;
}
