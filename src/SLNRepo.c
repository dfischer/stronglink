// Copyright 2014-2015 Ben Trask
// MIT licensed (see LICENSE for details)

#include "StrongLink.h"
#include "SLNDB.h"

#define CACHE_SIZE 1000
#define PASS_LEN 16 // Default for auto-generated passwords


// TODO: Put this somewhere.
#define ENTROPY_BYTES 8
static char *tohex2(char const *const buf, size_t const len) {
	char const map[] = "0123456789abcdef";
	char *const hex = calloc(len*2+1, 1);
	if(!hex) return NULL;
	for(size_t i = 0; i < len; ++i) {
		hex[i*2+0] = map[0xf & (buf[i] >> 4)];
		hex[i*2+1] = map[0xf & (buf[i] >> 0)];
	}
	return hex;
}
static char *async_fs_tempnam(char const *dir, char const *prefix) {
	if(!dir) dir = "/tmp"; // TODO: Use ENV
	if(!prefix) prefix = "async";
	char rand[ENTROPY_BYTES];
	if(async_random((unsigned char *)rand, ENTROPY_BYTES) < 0) return NULL;
	char *hex = tohex2(rand, ENTROPY_BYTES);
	char *path = aasprintf("%s/%s-%s", dir, prefix, hex);
	free(hex); hex = NULL;
	return path;
}



struct SLNRepo {
	str_t *dir;
	str_t *name;

	str_t *dataDir;
	str_t *tempDir;
	str_t *cacheDir;
	str_t *DBPath;

	SLNMode pub_mode;
	SLNMode reg_mode;
	SLNSessionCacheRef session_cache;

	KVS_env *db;

	async_mutex_t sub_mutex[1];
	async_cond_t sub_cond[1];
	uint64_t sub_latest;

	SLNPullRef *pulls;
	size_t pull_count;
	size_t pull_size;
};

static int connect_db(SLNRepoRef const repo);
static int add_pull(SLNRepoRef const repo, SLNPullRef *const pull);
static int load_pulls(SLNRepoRef const repo);
static int debug_pulls(SLNRepoRef const repo);

int SLNRepoCreate(strarg_t const dir, strarg_t const name, SLNRepoRef *const out) {
	assert(dir);
	assert(name);
	assert(out);

	SLNRepoRef repo = calloc(1, sizeof(struct SLNRepo));
	if(!repo) return UV_ENOMEM;
	int rc = 0;

	size_t dirlen = strlen(dir);
	while(dirlen > 1 && '/' == dir[dirlen-1]) dirlen--; // Prettier.
	repo->dir = strndup(dir, dirlen);
	repo->name = strdup(name);
	if(!repo->dir || !repo->name) rc = UV_ENOMEM;
	if(rc < 0) goto cleanup;

	repo->dataDir = aasprintf("%s/data", repo->dir);
	repo->tempDir = aasprintf("%s/tmp", repo->dir);
	repo->cacheDir = aasprintf("%s/cache", repo->dir);
	repo->DBPath = aasprintf("%s/sln.db", repo->dir);
	if(!repo->dataDir || !repo->tempDir || !repo->cacheDir || !repo->DBPath) rc = UV_ENOMEM;
	if(rc < 0) goto cleanup;

	// TODO: Configuration
	// TODO: The ability to limit public registration
	repo->pub_mode = 0;
	repo->reg_mode = 0;
	rc = SLNSessionCacheCreate(repo, CACHE_SIZE, &repo->session_cache);
	if(rc < 0) goto cleanup;

	rc = connect_db(repo);
	if(rc < 0) goto cleanup;

	rc = load_pulls(repo);
	if(rc < 0) goto cleanup;

	rc = debug_pulls(repo);
	if(rc < 0) {
		alogf("(pull debug setup failed: %s)\n", sln_strerror(rc));
		rc = 0; // Soft error.
	}

	async_mutex_init(repo->sub_mutex, 0);
	async_cond_init(repo->sub_cond, 0);

	*out = repo; repo = NULL;
cleanup:
	SLNRepoFree(&repo);
	return rc;
}
void SLNRepoFree(SLNRepoRef *const repoptr) {
	SLNRepoRef repo = *repoptr;
	if(!repo) return;

	SLNRepoPullsStop(repo);

	FREE(&repo->dir);
	FREE(&repo->name);

	FREE(&repo->dataDir);
	FREE(&repo->tempDir);
	FREE(&repo->cacheDir);
	FREE(&repo->DBPath);

	repo->pub_mode = 0;
	repo->reg_mode = 0;
	SLNSessionCacheFree(&repo->session_cache);

	kvs_env_close(repo->db); repo->db = NULL;

	async_mutex_destroy(repo->sub_mutex);
	async_cond_destroy(repo->sub_cond);
	repo->sub_latest = 0;

	for(size_t i = 0; i < repo->pull_count; ++i) {
		SLNPullFree(&repo->pulls[i]);
	}
	assert_zeroed(repo->pulls, repo->pull_count);
	FREE(&repo->pulls);
	repo->pull_count = 0;
	repo->pull_size = 0;

	assert_zeroed(repo, 1);
	FREE(repoptr); repo = NULL;
}

strarg_t SLNRepoGetDir(SLNRepoRef const repo) {
	if(!repo) return NULL;
	return repo->dir;
}
strarg_t SLNRepoGetName(SLNRepoRef const repo) {
	if(!repo) return NULL;
	return repo->name;
}

strarg_t SLNRepoGetDataDir(SLNRepoRef const repo) {
	if(!repo) return NULL;
	return repo->dataDir;
}
str_t *SLNRepoCopyInternalPath(SLNRepoRef const repo, strarg_t const internalHash) {
	if(!repo) return NULL;
	assert(repo->dataDir);
	assert(internalHash);
	return aasprintf("%s/%.2s/%s", repo->dataDir, internalHash, internalHash);
}
strarg_t SLNRepoGetTempDir(SLNRepoRef const repo) {
	if(!repo) return NULL;
	return repo->tempDir;
}
str_t *SLNRepoCopyTempPath(SLNRepoRef const repo) {
	if(!repo) return NULL;
	return async_fs_tempnam(repo->tempDir, "sln");
}
strarg_t SLNRepoGetCacheDir(SLNRepoRef const repo) {
	if(!repo) return NULL;
	return repo->cacheDir;
}

SLNMode SLNRepoGetPublicMode(SLNRepoRef const repo) {
	if(!repo) return 0;
	return repo->pub_mode;
}
SLNMode SLNRepoGetRegistrationMode(SLNRepoRef const repo) {
	if(!repo) return 0;
	return repo->reg_mode;
}
SLNSessionCacheRef SLNRepoGetSessionCache(SLNRepoRef const repo) {
	if(!repo) return NULL;
	return repo->session_cache;
}

void SLNRepoDBOpenUnsafe(SLNRepoRef const repo, KVS_env **const dbptr) {
	assert(repo);
	assert(dbptr);
	async_pool_enter(NULL);
	*dbptr = repo->db;
}
void SLNRepoDBClose(SLNRepoRef const repo, KVS_env **const dbptr) {
	assert(dbptr);
	assert(repo || !*dbptr);
	if(!*dbptr) return;
	async_pool_leave(NULL);
	*dbptr = NULL;
}

void SLNRepoSubmissionEmit(SLNRepoRef const repo, uint64_t const sortID) {
	assert(repo);
	async_mutex_lock(repo->sub_mutex);
	if(sortID > repo->sub_latest) {
		repo->sub_latest = sortID;
		async_cond_broadcast(repo->sub_cond);
	}
	async_mutex_unlock(repo->sub_mutex);
}
int SLNRepoSubmissionWait(SLNRepoRef const repo, uint64_t *const sortID, uint64_t const future) {
	assert(repo);
	assert(sortID);
	int rc = 0;
	async_mutex_lock(repo->sub_mutex);
	while(repo->sub_latest <= *sortID) {
		rc = async_cond_timedwait(repo->sub_cond, repo->sub_mutex, future);
		if(rc < 0) break;
	}
	*sortID = repo->sub_latest;
	async_mutex_unlock(repo->sub_mutex);
	return rc;
}

void SLNRepoPullsStart(SLNRepoRef const repo) {
	if(!repo) return;
	for(size_t i = 0; i < repo->pull_count; ++i) {
		SLNPullStart(repo->pulls[i]);
	}
}
void SLNRepoPullsStop(SLNRepoRef const repo) {
	if(!repo) return;
	for(size_t i = 0; i < repo->pull_count; ++i) {
		SLNPullStop(repo->pulls[i]);
	}
}


static int create_admin(SLNRepoRef const repo, KVS_txn *const txn) {
	SLNSessionCacheRef const cache = SLNRepoGetSessionCache(repo);
	SLNSessionRef root = NULL;
	int rc = SLNSessionCreateInternal(cache, 0, NULL, NULL, 0, SLN_ROOT, NULL, &root);
	if(rc < 0) return rc;

	strarg_t username = getenv("USER"); // TODO: Portability?
	if(!username) username = "admin";

	byte_t buf[PASS_LEN/2];
	rc = async_random(buf, sizeof(buf));
	if(rc < 0) return rc;
	char password[PASS_LEN+1];
	tohex(password, buf, sizeof(buf));
	password[PASS_LEN] = '\0';

	rc = SLNSessionCreateUserInternal(root, txn, username, password, SLN_ROOT);
	if(rc < 0) return rc;

	fprintf(stdout, "ACCOUNT CREATED\n");
	fprintf(stdout, "  Username: %s\n", username);
	fprintf(stdout, "  Password: %s\n", password);
	fprintf(stdout, "  Please change your password after logging in\n");

	return 0;
}
static int connect_db(SLNRepoRef const repo) {
	assert(repo);
	size_t mapsize = 1024 * 1024 * 1024 * 1;
	int rc = kvs_env_create(&repo->db);
	rc = rc < 0 ? rc : kvs_env_set_config(repo->db, KVS_CFG_MAPSIZE, &mapsize);
	if(rc < 0) {
		alogf("Database setup error (%s)\n", sln_strerror(rc));
		return rc;
	}
	rc = kvs_env_open(repo->db, repo->DBPath, 0, 0600);
	if(rc < 0) {
		alogf("Database open error (%s)\n", sln_strerror(rc));
		return rc;
	}

	KVS_env *db = NULL;
	SLNRepoDBOpenUnsafe(repo, &db);
	KVS_txn *txn = NULL;
	rc = kvs_txn_begin(db, NULL, KVS_RDWR, &txn);
	if(rc < 0) {
		SLNRepoDBClose(repo, &db);
		alogf("Database transaction error (%s)\n", sln_strerror(rc));
		return rc;
	}

	rc = kvs_schema_verify(txn);
	if(KVS_VERSION_MISMATCH == rc) {
		kvs_txn_abort(txn); txn = NULL;
		SLNRepoDBClose(repo, &db);
		alogf("Database incompatible with this software version\n");
		return rc;
	}
	if(rc < 0) {
		kvs_txn_abort(txn); txn = NULL;
		SLNRepoDBClose(repo, &db);
		alogf("Database schema layer error (%s)\n", sln_strerror(rc));
		return rc;
	}

	// TODO: Application-level schema verification

	KVS_cursor *cursor = NULL;
	rc = kvs_txn_cursor(txn, &cursor);
	if(rc < 0) {
		kvs_txn_abort(txn); txn = NULL;
		SLNRepoDBClose(repo, &db);
		alogf("Database cursor error (%s)\n", sln_strerror(rc));
		return rc;
	}

	KVS_range users[1];
	SLNUserByIDKeyRange0(users, txn);
	rc = kvs_cursor_firstr(cursor, users, NULL, NULL, +1);
	if(KVS_NOTFOUND == rc) {
		rc = create_admin(repo, txn);
	}
	if(rc < 0) {
		kvs_txn_abort(txn); txn = NULL;
		SLNRepoDBClose(repo, &db);
		alogf("Database user error (%s)\n", sln_strerror(rc));
		return rc;
	}

	rc = kvs_txn_commit(txn); txn = NULL;
	SLNRepoDBClose(repo, &db);
	if(rc < 0) {
		alogf("Database commit error (%s)\n", sln_strerror(rc));
		return rc;
	}
	return 0;
}

static int add_pull(SLNRepoRef const repo, SLNPullRef *const pull) {
	if(!repo) return UV_EINVAL;
	if(repo->pull_count+1 > repo->pull_size) {
		repo->pull_size = (repo->pull_count+1) * 2;
		SLNPullRef *pulls = reallocarray(repo->pulls, repo->pull_size, sizeof(SLNPullRef));
		if(!pulls) return UV_ENOMEM;
		repo->pulls = pulls; pulls = NULL;
	}
	repo->pulls[repo->pull_count++] = *pull; *pull = NULL;
	return 0;
}
static int load_pulls(SLNRepoRef const repo) {
	assert(repo);

	KVS_env *db = NULL;
	KVS_txn *txn = NULL;
	KVS_cursor *cur = NULL;
	SLNPullRef pull = NULL;
	int rc;

	SLNRepoDBOpenUnsafe(repo, &db);
	rc = kvs_txn_begin(db, NULL, KVS_RDONLY, &txn);
	if(rc < 0) goto cleanup;
	rc = kvs_cursor_open(txn, &cur);
	if(rc < 0) goto cleanup;

	KVS_range pulls[1];
	SLNPullByIDRange0(pulls, txn);
	KVS_val pullID_key[1];
	KVS_val pull_val[1];
	rc = kvs_cursor_firstr(cur, pulls, pullID_key, pull_val, +1);
	for(; rc >= 0; rc = kvs_cursor_nextr(cur, pulls, pullID_key, pull_val, +1)) {
		uint64_t pullID;
		SLNPullByIDKeyUnpack(pullID_key, txn, &pullID);
		uint64_t userID;
		strarg_t certhash;
		strarg_t host;
		strarg_t path;
		strarg_t query;
		strarg_t cookie;
		SLNPullByIDValUnpack(pull_val, txn, &userID, &certhash, &host, &path, &query, &cookie);

		rc = SLNPullCreate(repo->session_cache, pullID, certhash, host, path, query, cookie, &pull);
		if(rc < 0) goto cleanup;

		rc = add_pull(repo, &pull);
		if(rc < 0) goto cleanup;

		SLNPullFree(&pull);
	}
	if(KVS_NOTFOUND == rc) rc = 0;
	if(rc < 0) goto cleanup;

cleanup:
	kvs_cursor_close(cur); cur = NULL;
	kvs_txn_abort(txn); txn = NULL;
	SLNRepoDBClose(repo, &db);
	SLNPullFree(&pull);
	return rc;
}
static int debug_pulls(SLNRepoRef const repo) {
	assert(repo);
	SLNPullRef pull = NULL;
	int rc = SLNPullCreate(repo->session_cache, 1, NULL, "localhost:7999", "", NULL, NULL, &pull);
	if(rc < 0) return rc;
	rc = add_pull(repo, &pull);
	SLNPullFree(&pull);
	return rc;
}

