#define _GNU_SOURCE
#include "async.h"
#include "bcrypt.h"
#include "EarthFS.h"

#define COOKIE_CACHE_SIZE 1000

struct cached_cookie {
	uint64_t sessionID;
	str_t *sessionKey;
	uint64_t atime; // TODO: Prune old entries.
};
static struct cached_cookie cookie_cache[COOKIE_CACHE_SIZE] = {};

static bool_t cookie_cache_lookup(uint64_t const sessionID, strarg_t const sessionKey) {
	assert(!async_worker_get_current());
	if(sessionID <= 0 || !sessionKey) return false;
	index_t const x = sessionID+sessionKey[0] % COOKIE_CACHE_SIZE;
	if(cookie_cache[x].sessionID != sessionID) return false;
	if(!cookie_cache[x].sessionKey) return false;
	return 0 == passcmp(sessionKey, cookie_cache[x].sessionKey);
}
static void cookie_cache_store(uint64_t const sessionID, strarg_t const sessionKey) {
	assert(!async_worker_get_current());
	if(sessionID <= 0 || !sessionKey) return;
	index_t const x = sessionID+sessionKey[0] % COOKIE_CACHE_SIZE;
	FREE(&cookie_cache[x].sessionKey);
	cookie_cache[x].sessionID = sessionID;
	cookie_cache[x].sessionKey = strdup(sessionKey);
	cookie_cache[x].atime = uv_now(loop);
}

struct EFSSession {
	EFSRepoRef repo;
	uint64_t userID;
};

str_t *EFSRepoCreateCookie(EFSRepoRef const repo, strarg_t const username, strarg_t const password) {
	if(!repo) return NULL;
	if(!username) return NULL;
	if(!password) return NULL;

	EFSConnection const *conn;
	int rc;

	conn = EFSRepoDBOpen(repo);
	if(!conn) return NULL;
	MDB_txn *txn = NULL;
	rc = mdb_txn_begin(conn->env, NULL, MDB_RDONLY, &txn);
	if(MDB_SUCCESS != rc) {
		EFSRepoDBClose(repo, &conn);
		return NULL;
	}

	uint64_t const username_id = db_string_id(txn, conn->schema, username);
	if(!username_id) {
		EFSRepoDBClose(repo, &conn);
		return NULL;
	}

	DB_VAL(username_val, 1);
	db_bind(username_val, 0, username_id);
	MDB_val userID_val[1];
	MDB_val user_val[1];
	rc = mdb_get(txn, conn->userIDByName, username_val, userID_val);
	if(MDB_SUCCESS == rc) rc = mdb_get(txn, conn->userByID, userID_val, user_val);
	if(MDB_SUCCESS != rc) {
		mdb_txn_abort(txn); txn = NULL;
		EFSRepoDBClose(repo, &conn);
		return NULL;
	}
	uint64_t const userID = db_column(userID_val, 0);
	uint64_t const passhash_id = db_column(user_val, 1);
	str_t *passhash = strdup(db_string(txn, conn->schema, passhash_id));

	mdb_txn_abort(txn); txn = NULL;
	EFSRepoDBClose(repo, &conn);

	if(userID <= 0 || !checkpass(password, passhash)) {
		FREE(&passhash);
		return NULL;
	}
	FREE(&passhash);

	str_t *sessionKey = strdup("not-very-random"); // TODO: Generate
	if(!sessionKey) {
		return NULL;
	}
	str_t *sessionHash = hashpass(sessionKey);
	if(!sessionHash) {
		FREE(&sessionHash);
		FREE(&sessionKey);
		return NULL;
	}

	conn = EFSRepoDBOpen(repo);
	if(conn) rc = mdb_txn_begin(conn->env, NULL, MDB_RDWR, &txn);
	if(!conn || MDB_SUCCESS != rc) {
		FREE(&sessionHash);
		FREE(&sessionKey);
		return NULL;
	}

	uint64_t const sessionID = db_autoincrement(txn, conn->sessionByID);
	uint64_t const sessionHash_id = db_string_id(txn, conn->schema, sessionHash);
	FREE(&sessionHash);

	DB_VAL(sessionID_val, 1);
	db_bind(sessionID_val, 0, sessionID);

	DB_VAL(session_val, 2);
	db_bind(session_val, 0, userID);
	db_bind(session_val, 1, sessionHash_id);
	rc = mdb_put(txn, conn->sessionByID, sessionID_val, session_val, MDB_NOOVERWRITE);
	if(MDB_SUCCESS != rc) {
		mdb_txn_abort(txn); txn = NULL;
		EFSRepoDBClose(repo, &conn);
		FREE(&sessionKey);
		return NULL;
	}

	rc = mdb_txn_commit(txn); txn = NULL;
	EFSRepoDBClose(repo, &conn);
	if(MDB_SUCCESS != rc) {
		FREE(&sessionKey);
		return NULL;
	}

	str_t *cookie = NULL;
	if(asprintf(&cookie, "%lld:%s", sessionID, sessionKey) < 0) cookie = NULL;
	FREE(&sessionKey);
	return cookie;
}
EFSSessionRef EFSRepoCreateSession(EFSRepoRef const repo, strarg_t const cookie) {
	if(!repo) return NULL;
	if(!cookie) return NULL;

	long long sessionID = -1;
	str_t *sessionKey = calloc(strlen(cookie)+1, 1);
	if(!sessionKey) return NULL;
	sscanf(cookie, "s=%lld:%s", &sessionID, sessionKey);
	if(sessionID <= 0 || '\0' == sessionKey[0]) {
		FREE(&sessionKey);
		return NULL;
	}

	EFSConnection const *conn = EFSRepoDBOpen(repo);
	int rc;
	if(!conn) {
		FREE(&sessionKey);
		return NULL;
	}
	MDB_txn *txn = NULL;
	mdb_txn_begin(conn->env, NULL, MDB_RDONLY, &txn);

	DB_VAL(sessionID_val, 1);
	db_bind(sessionID_val, 0, sessionID);
	MDB_val session_val[1];
	rc = mdb_get(txn, conn->sessionByID, sessionID_val, session_val);
	if(MDB_SUCCESS != rc) {
		FREE(&sessionKey);
		mdb_txn_abort(txn); txn = NULL;
		EFSRepoDBClose(repo, &conn);
		return NULL;
	}
	uint64_t const userID = db_column(session_val, 0);
	uint64_t const sessionHash_id = db_column(session_val, 1);
	str_t *sessionHash = strdup(db_string(txn, conn->schema, sessionHash_id));

	mdb_txn_abort(txn); txn = NULL;
	EFSRepoDBClose(repo, &conn);

	if(userID <= 0) {
		FREE(&sessionKey);
		FREE(&sessionHash);
		return NULL;
	}

	if(!cookie_cache_lookup(sessionID, sessionKey)) {
		if(!checkpass(sessionKey, sessionHash)) {
			FREE(&sessionKey);
			FREE(&sessionHash);
			return NULL;
		}
		cookie_cache_store(sessionID, sessionKey);
	}
	FREE(&sessionKey);
	FREE(&sessionHash);

	return EFSRepoCreateSessionInternal(repo, userID);
}
EFSSessionRef EFSRepoCreateSessionInternal(EFSRepoRef const repo, uint64_t const userID) {
	EFSSessionRef const session = calloc(1, sizeof(struct EFSSession));
	if(!session) return NULL;
	session->repo = repo;
	session->userID = userID;
	return session;
}
void EFSSessionFree(EFSSessionRef *const sessionptr) {
	EFSSessionRef session = *sessionptr;
	if(!session) return;
	session->repo = NULL;
	session->userID = -1;
	FREE(sessionptr); session = NULL;
}
EFSRepoRef EFSSessionGetRepo(EFSSessionRef const session) {
	if(!session) return NULL;
	return session->repo;
}
uint64_t EFSSessionGetUserID(EFSSessionRef const session) {
	if(!session) return -1;
	return session->userID;
}

URIListRef EFSSessionCreateFilteredURIList(EFSSessionRef const session, EFSFilterRef const filter, count_t const max) { // TODO: Sort order, pagination.

	return NULL;
// TODO
/*
	if(!session) return NULL;
	// TODO: Check session mode.
	EFSRepoRef const repo = EFSSessionGetRepo(session);
	sqlite3f *db = EFSRepoDBConnect(repo);
	if(!db) return NULL;

	URIListRef const URIs = URIListCreate(); // TODO: Just preallocate a regular array, since we know the maximum size. Get rid of URILists all together.

	// TODO: Pagination
	int64_t const initialSortID = INT64_MAX;
	int64_t const initialFileID = INT64_MAX;

	EFSFilterPrepare(filter, db);

	// It'd be nice to combine these two into one query, but the query optimizer was being stupid. Basically, we're just doing a manual JOIN with `WHERE (sort_id = ?1 AND file_id < ?2) OR sort_id < ?1` and `ORDER BY sort_id DESC, file_id DESC`.
	// The problems with the query optimizer are: 1. it doesn't like SELECT DISTINCT (or GROUP BY) with two args, even if it's sorted on both of them, and 2. we have to use a temp b-tree for the second ORDER BY either way, but I think it's slower in a larger query...
	sqlite3_stmt *selectMetaFiles = QUERY(db,
		"SELECT DISTINCT file_id AS sort_id\n"
		"FROM meta_files\n"
		"WHERE sort_id <= ?\n"
		"ORDER BY sort_id DESC");
	sqlite3_bind_int64(selectMetaFiles, 1, initialSortID);
	sqlite3_stmt *selectFiles = QUERY(db,
		"SELECT f.file_id\n"
		"FROM meta_files AS mf\n"
		"INNER JOIN file_uris AS f ON (f.uri = mf.target_uri)\n"
		"WHERE mf.file_id = ? AND f.file_id < ?\n"
		"ORDER BY f.file_id DESC");

	sqlite3_stmt *selectHash = QUERY(db,
		"SELECT internal_hash\n"
		"FROM files WHERE file_id = ? LIMIT 1");

	EXEC(QUERY(db, "BEGIN DEFERRED TRANSACTION"));
	while(SQLITE_ROW == STEP(selectMetaFiles)) {
		int64_t const sortID = sqlite3_column_int64(selectMetaFiles, 0);

		sqlite3_bind_int64(selectFiles, 1, sortID);
		sqlite3_bind_int64(selectFiles, 2, initialSortID == sortID ? initialFileID : INT64_MAX);
		while(SQLITE_ROW == STEP(selectFiles)) {
			int64_t const fileID = sqlite3_column_int64(selectFiles, 0);
			int64_t const age = EFSFilterMatchAge(filter, sortID, fileID);
//			fprintf(stderr, "{%lld, %lld} -> %lld\n", sortID, fileID, age);
			if(age != sortID) continue;
			sqlite3_bind_int64(selectHash, 1, fileID);
			if(SQLITE_ROW == STEP(selectHash)) {
				strarg_t const hash = (strarg_t)sqlite3_column_text(selectHash, 0);
				str_t *URI = EFSFormatURI(EFS_INTERNAL_ALGO, hash);
				URIListAddURI(URIs, URI, -1);
				FREE(&URI);
			}
			sqlite3_reset(selectHash);
			if(URIListGetCount(URIs) >= max) break;
		}
		sqlite3_reset(selectFiles);
		if(URIListGetCount(URIs) >= max) break;
	}
	EXEC(QUERY(db, "COMMIT"));

	sqlite3f_finalize(selectHash); selectHash = NULL;
	sqlite3f_finalize(selectFiles); selectFiles = NULL;
	sqlite3f_finalize(selectMetaFiles); selectMetaFiles = NULL;

	EFSRepoDBClose(repo, &db);
	return URIs;*/
}
err_t EFSSessionGetFileInfo(EFSSessionRef const session, strarg_t const URI, EFSFileInfo *const info) {
	if(!session) return -1;
	if(!URI) return -1;
	// TODO: Check session mode.
	EFSRepoRef const repo = EFSSessionGetRepo(session);
	EFSConnection const *conn = EFSRepoDBOpen(repo);
	int rc;
	MDB_txn *txn = NULL;
	rc = mdb_txn_begin(conn->env, NULL, MDB_RDONLY, &txn);
	if(MDB_SUCCESS != rc) {
		EFSRepoDBClose(repo, &conn);
		return -1;
	}

	uint64_t const URI_id = db_string_id(txn, conn->schema, URI);
	if(!URI_id) {
		EFSRepoDBClose(repo, &conn);
		return -1;
	}

	DB_VAL(URI_val, 1);
	db_bind(URI_val, 0, URI_id);
	MDB_val fileID_val[1];
	MDB_val file_val[1];
	rc = mdb_get(txn, conn->fileIDByURI, URI_val, fileID_val);
	if(MDB_SUCCESS == rc) rc = mdb_get(txn, conn->fileByID, fileID_val, file_val);
	if(MDB_SUCCESS != rc) {
		mdb_txn_abort(txn); txn = NULL;
		EFSRepoDBClose(repo, &conn);
		return -1;
	}

	if(info) {
		strarg_t const internalHash = db_string(txn, conn->schema, db_column(file_val, 0));
		strarg_t const type = db_string(txn, conn->schema, db_column(file_val, 1));
		info->path = EFSRepoCopyInternalPath(repo, internalHash);
		info->type = strdup(type);
		info->size = db_column(file_val, 2);
	}

	mdb_txn_abort(txn); txn = NULL;
	EFSRepoDBClose(repo, &conn);
	return 0;
}
void EFSFileInfoCleanup(EFSFileInfo *const info) {
	if(!info) return;
	FREE(&info->path);
	FREE(&info->type);
}

