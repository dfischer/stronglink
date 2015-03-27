#include "SLNFilter.h"
#include "../util/fts.h"

@implementation EFSIndividualFilter
- (void)free {
	curtxn = NULL;
	db_cursor_close(step_target); step_target = NULL;
	db_cursor_close(step_files); step_files = NULL;
	db_cursor_close(age_uris); age_uris = NULL;
	db_cursor_close(age_metafiles); age_metafiles = NULL;
	[super free];
}

- (EFSFilter *)unwrap {
	return self;
}

- (int)prepare:(DB_txn *const)txn {
	if([super prepare:txn] < 0) return -1;
	db_cursor_renew(txn, &step_target); // EFSMetaFileByID
	db_cursor_renew(txn, &step_files); // EFSURIAndFileID
	db_cursor_renew(txn, &age_uris); // EFSFileIDAndURI
	db_cursor_renew(txn, &age_metafiles); // EFSTargetURIAndMetaFileID
	curtxn = txn;
	return 0;
}
- (void)seek:(int const)dir :(uint64_t const)sortID :(uint64_t const)fileID {
	int rc;
	uint64_t const actualSortID = [self seekMeta:dir :sortID];
	if(!valid(actualSortID)) return;
	DB_val metaFileID_key[1];
	EFSMetaFileByIDKeyPack(metaFileID_key, curtxn, actualSortID);
	DB_val metaFile_val[1];
	rc = db_cursor_seek(step_target, metaFileID_key, metaFile_val, 0);
	assertf(DB_SUCCESS == rc, "Database error %s", db_strerror(rc));
	uint64_t metaFileFileID;
	strarg_t targetURI;
	EFSMetaFileByIDValUnpack(metaFile_val, curtxn, &metaFileFileID, &targetURI);

	DB_range fileIDs[1];
	EFSURIAndFileIDRange1(fileIDs, curtxn, targetURI);
	if(sortID == actualSortID) {
		DB_val fileID_key[1];
		EFSURIAndFileIDKeyPack(fileID_key, curtxn, targetURI, fileID);
		rc = db_cursor_seekr(step_files, fileIDs, fileID_key, NULL, dir);
	} else {
		DB_val fileID_key[1];
		rc = db_cursor_firstr(step_files, fileIDs, fileID_key, NULL, dir);
	}
	assertf(DB_SUCCESS == rc || DB_NOTFOUND == rc, "Database error %s", db_strerror(rc));
}
- (void)current:(int const)dir :(uint64_t *const)sortID :(uint64_t *const)fileID {
	DB_val fileID_key[1];
	int rc = db_cursor_current(step_files, fileID_key, NULL);
	if(DB_SUCCESS == rc) {
		strarg_t targetURI;
		uint64_t _fileID;
		EFSURIAndFileIDKeyUnpack(fileID_key, curtxn, &targetURI, &_fileID);
		if(sortID) *sortID = [self currentMeta:dir];
		if(fileID) *fileID = _fileID;
	} else {
		if(sortID) *sortID = invalid(dir);
		if(fileID) *fileID = invalid(dir);
	}
}
- (void)step:(int const)dir {
	int rc;
	DB_val fileID_key[1];
	rc = db_cursor_current(step_files, fileID_key, NULL);
	if(DB_SUCCESS == rc) {
		strarg_t targetURI;
		uint64_t fileID;
		EFSURIAndFileIDKeyUnpack(fileID_key, curtxn, &targetURI, &fileID);
		DB_range fileIDs[1];
		EFSURIAndFileIDRange1(fileIDs, curtxn, targetURI);
		rc = db_cursor_nextr(step_files, fileIDs, fileID_key, NULL, dir);
		if(DB_SUCCESS == rc) return;
	}

	for(uint64_t sortID = [self stepMeta:dir]; valid(sortID); sortID = [self stepMeta:dir]) {
		DB_val metaFileID_key[1];
		EFSMetaFileByIDKeyPack(metaFileID_key, curtxn, sortID);
		DB_val metaFile_val[1];
		rc = db_cursor_seek(step_target, metaFileID_key, metaFile_val, 0);
		assertf(DB_SUCCESS == rc, "Database error %s", db_strerror(rc));
		uint64_t f;
		strarg_t targetURI;
		EFSMetaFileByIDValUnpack(metaFile_val, curtxn, &f, &targetURI);

		DB_range fileIDs[1];
		EFSURIAndFileIDRange1(fileIDs, curtxn, targetURI);
		DB_val fileID_key[1];
		rc = db_cursor_firstr(step_files, fileIDs, fileID_key, NULL, +1);
		if(DB_SUCCESS != rc) continue;
		return;
	}
}
- (uint64_t)age:(uint64_t const)sortID :(uint64_t const)fileID {
	uint64_t earliest = UINT64_MAX;
	int rc;

	DB_range URIs[1];
	EFSFileIDAndURIRange1(URIs, txn, fileID);
	DB_val URI_val[1];
	rc = db_cursor_firstr(age_uris, URIs, URI_val, NULL, +1);
	assert(DB_SUCCESS == rc || DB_NOTFOUND == rc);

	for(; DB_SUCCESS == rc; rc = db_cursor_nextr(age_uris, URIs, URI_val, NULL, +1)) {
		uint64_t f;
		strarg_t targetURI;
		EFSFileIDAndURIKeyUnpack(URI_val, curtxn, &f, &targetURI);
		assert(fileID == f);

		DB_range metafiles[1];
		EFSTargetURIAndMetaFileIDRange1(metafiles, curtxn, targetURI);
		DB_val metaFileID_key[1];
		rc = db_cursor_firstr(age_metafiles, metafiles, metaFileID_key, NULL, +1);
		assert(DB_SUCCESS == rc || DB_NOTFOUND == rc);
		for(; DB_SUCCESS == rc; rc = db_cursor_nextr(age_metafiles, metafiles, metaFileID_key, NULL, +1)) {
			strarg_t u;
			uint64_t metaFileID;
			EFSTargetURIAndMetaFileIDKeyUnpack(metaFileID_key, curtxn, &u, &metaFileID);
			assert(0 == strcmp(targetURI, u));
			if(metaFileID > sortID) break;
			if(![self match:metaFileID]) continue;
			if(metaFileID < earliest) earliest = metaFileID;
			break;
		}
	}
	return earliest;
}
@end

@implementation EFSAllFilter
- (void)free {
	db_cursor_close(metafiles); metafiles = NULL;
	[super free];
}

- (EFSFilterType)type {
	return EFSAllFilterType;
}
- (void)print:(count_t const)depth {
	indent(depth);
	fprintf(stderr, "(all)\n");
}
- (size_t)getUserFilter:(str_t *const)data :(size_t const)size :(count_t const)depth {
	if(depth) return wr(data, size, "*");
	return wr(data, size, "");
}

- (int)prepare:(DB_txn *const)txn {
	if([super prepare:txn] < 0) return -1;
	db_cursor_renew(txn, &metafiles); // EFSMetaFileByID
	return 0;
}

- (uint64_t)seekMeta:(int const)dir :(uint64_t const)sortID {
	DB_range range[1];
	EFSMetaFileByIDRange0(range, curtxn);
	DB_val sortID_key[1];
	EFSMetaFileByIDKeyPack(sortID_key, curtxn, sortID);
	int rc = db_cursor_seekr(metafiles, range, sortID_key, NULL, dir);
	if(DB_SUCCESS != rc) return invalid(dir);
	uint64_t actualSortID;
	EFSMetaFileByIDKeyUnpack(sortID_key, curtxn, &actualSortID);
	return actualSortID;
}
- (uint64_t)currentMeta:(int const)dir {
	DB_val sortID_key[1];
	int rc = db_cursor_current(metafiles, sortID_key, NULL);
	if(DB_SUCCESS != rc) return invalid(dir);
	uint64_t sortID;
	EFSMetaFileByIDKeyUnpack(sortID_key, curtxn, &sortID);
	return sortID;
}
- (uint64_t)stepMeta:(int const)dir {
	DB_range range[1];
	EFSMetaFileByIDRange0(range, curtxn);
	DB_val sortID_key[1];
	int rc = db_cursor_nextr(metafiles, range, sortID_key, NULL, dir);
	if(DB_SUCCESS != rc) return invalid(dir);
	uint64_t sortID;
	EFSMetaFileByIDKeyUnpack(sortID_key, curtxn, &sortID);
	return sortID;
}
- (bool)match:(uint64_t const)metaFileID {
	return true;
}
@end

@implementation EFSFulltextFilter
- (void)free {
	FREE(&term);
	for(index_t i = 0; i < count; ++i) {
		FREE(&tokens[i].str);
	}
	assert_zeroed(tokens, count);
	FREE(&tokens);
	count = 0;
	asize = 0;
	db_cursor_close(metafiles); metafiles = NULL;
	db_cursor_close(match); match = NULL;
	[super free];
}

- (EFSFilterType)type {
	return EFSFulltextFilterType;
}
- (strarg_t)stringArg:(index_t const)i {
	if(0 != i) return NULL;
	return term;
}
- (int)addStringArg:(strarg_t const)str :(size_t const)len {
	if(!str) return -1;
	if(0 == len) return -1;
	if(term) return -1;
	if(count) return -1;
	term = strndup(str, len);

	// TODO: libstemmer?
	sqlite3_tokenizer_module const *fts = NULL;
	sqlite3_tokenizer *tokenizer = NULL;
	fts_get(&fts, &tokenizer);

	sqlite3_tokenizer_cursor *tcur = NULL;
	int rc = fts->xOpen(tokenizer, term, strlen(term), &tcur);
	assert(SQLITE_OK == rc);

	for(;;) {
		strarg_t token;
		int tlen;
		int ignored1, ignored2, ignored3;
		rc = fts->xNext(tcur, &token, &tlen, &ignored1, &ignored2, &ignored3);
		if(SQLITE_OK != rc) break;
		if(count+1 > asize) {
			asize = MAX(8, asize*2);
			tokens = realloc(tokens, sizeof(tokens[0]) * asize);
			assert(tokens); // TODO
		}
		tokens[count++].str = strndup(token, tlen);
	}

	fts->xClose(tcur);

	if(!count) return -1;
	return 0;
}
- (void)print:(count_t const)depth {
	indent(depth);
	fprintf(stderr, "(fulltext %s)\n", term);
}
- (size_t)getUserFilter:(str_t *const)data :(size_t const)size :(count_t const)depth {
	return wr_quoted(data, size, term);
}

- (int)prepare:(DB_txn *const)txn {
	if([super prepare:txn] < 0) return -1;
	db_cursor_renew(txn, &metafiles);
	db_cursor_renew(txn, &match);
	return 0;
}

- (uint64_t)seekMeta:(int const)dir :(uint64_t const)sortID {
	assert(count);
	DB_range range[1];
	EFSTermMetaFileIDAndPositionRange1(range, curtxn, tokens[0].str);
	DB_val sortID_key[1];
	EFSTermMetaFileIDAndPositionKeyPack(sortID_key, curtxn, tokens[0].str, sortID, 0);
	// TODO: In order to handle seeking backwards over document with several matching positions, we need to use sortID+1... But sortID might be UINT64_MAX, so be careful.
	int rc = db_cursor_seekr(metafiles, range, sortID_key, NULL, dir);
	if(DB_SUCCESS != rc) return invalid(dir);
	strarg_t token;
	uint64_t actualSortID, position;
	EFSTermMetaFileIDAndPositionKeyUnpack(sortID_key, curtxn, &token, &actualSortID, &position);
	assert(0 == strcmp(tokens[0].str, token));
	return actualSortID;
}
- (uint64_t)currentMeta:(int const)dir {
	assert(count);
	DB_val sortID_key[1];
	int rc = db_cursor_current(metafiles, sortID_key, NULL);
	if(DB_SUCCESS != rc) return invalid(dir);
	strarg_t token;
	uint64_t sortID, position;
	EFSTermMetaFileIDAndPositionKeyUnpack(sortID_key, curtxn, &token, &sortID, &position);
	assert(0 == strcmp(tokens[0].str, token));
	return sortID;
}
- (uint64_t)stepMeta:(int const)dir {
	assert(count);
	DB_range range[1];
	EFSTermMetaFileIDAndPositionRange1(range, curtxn, tokens[0].str);
	DB_val sortID_key[1];
	int rc = db_cursor_nextr(metafiles, range, sortID_key, NULL, dir);
	if(DB_SUCCESS != rc) return invalid(dir);
	strarg_t token;
	uint64_t sortID, position;
	EFSTermMetaFileIDAndPositionKeyUnpack(sortID_key, curtxn, &token, &sortID, &position);
	assert(0 == strcmp(tokens[0].str, token));
	return sortID;
}
- (bool)match:(uint64_t const)metaFileID {
	assert(count);
	DB_range range[1];
	EFSTermMetaFileIDAndPositionRange2(range, curtxn, tokens[0].str, metaFileID);
	DB_val sortID_key[1];
	int rc = db_cursor_firstr(match, range, sortID_key, NULL, +1);
	if(DB_SUCCESS == rc) return true;
	if(DB_NOTFOUND == rc) return false;
	assertf(0, "Database error %s", db_strerror(rc));
}
@end

@implementation EFSMetadataFilter
- (void)free {
	FREE(&field);
	FREE(&value);
	db_cursor_close(metafiles); metafiles = NULL;
	db_cursor_close(match); match = NULL;
	[super free];
}

- (EFSFilterType)type {
	return EFSMetadataFilterType;
}
- (strarg_t)stringArg:(index_t const)i {
	switch(i) {
		case 0: return field;
		case 1: return value;
		default: return NULL;
	}
}
- (int)addStringArg:(strarg_t const)str :(size_t const)len {
	if(!field) {
		field = strndup(str, len);
		return 0;
	}
	if(!value) {
		value = strndup(str, len);
		return 0;
	}
	return -1;
}
- (void)print:(count_t const)depth {
	indent(depth);
	fprintf(stderr, "(metadata \"%s\" \"%s\")\n", field, value);
}
- (size_t)getUserFilter:(str_t *const)data :(size_t const)size :(count_t const)depth {
	size_t len = 0;
	if(0 != strcasecmp("link", field)) { // TODO: Have a separate filter for link tracking?
		len += wr_quoted(data+len, size-len, field);
		len += wr(data+len, size-len, "=");
	}
	len += wr_quoted(data+len, size-len, value);
	return len;
}

- (int)prepare:(DB_txn *const)txn {
	if([super prepare:txn] < 0) return -1;
	if(!field || !value) return -1;
	db_cursor_renew(txn, &metafiles); // EFSFieldValueAndMetaFileID
	db_cursor_renew(txn, &match); // EFSFieldValueAndMetaFileID
	curtxn = txn;
	return 0;
}

- (uint64_t)seekMeta:(int const)dir :(uint64_t const)sortID {
	DB_range range[1];
	EFSFieldValueAndMetaFileIDRange2(range, curtxn, field, value);
	DB_val metadata_key[1];
	EFSFieldValueAndMetaFileIDKeyPack(metadata_key, curtxn, field, value, sortID);
	int rc = db_cursor_seekr(metafiles, range, metadata_key, NULL, dir);
	if(DB_SUCCESS != rc) return invalid(dir);
	strarg_t f, v;
	uint64_t actualSortID;
	EFSFieldValueAndMetaFileIDKeyUnpack(metadata_key, curtxn, &f, &v, &actualSortID);
	return actualSortID;
}
- (uint64_t)currentMeta:(int const)dir {
	DB_val metadata_key[1];
	int rc = db_cursor_current(metafiles, metadata_key, NULL);
	if(DB_SUCCESS != rc) return invalid(dir);
	strarg_t f, v;
	uint64_t sortID;
	EFSFieldValueAndMetaFileIDKeyUnpack(metadata_key, curtxn, &f, &v, &sortID);
	return sortID;
}
- (uint64_t)stepMeta:(int const)dir {
	DB_range range[1];
	EFSFieldValueAndMetaFileIDRange2(range, curtxn, field, value);
	DB_val metadata_key[1];
	int rc = db_cursor_nextr(metafiles, range, metadata_key, NULL, dir);
	if(DB_SUCCESS != rc) return invalid(dir);
	strarg_t f, v;
	uint64_t sortID;
	EFSFieldValueAndMetaFileIDKeyUnpack(metadata_key, curtxn, &f, &v, &sortID);
	return sortID;
}
- (bool)match:(uint64_t const)metaFileID {
	DB_val metadata_key[1];
	EFSFieldValueAndMetaFileIDKeyPack(metadata_key, curtxn, field, value, metaFileID);
	int rc = db_cursor_seek(match, metadata_key, NULL, 0);
	if(DB_SUCCESS == rc) return true;
	if(DB_NOTFOUND == rc) return false;
	assertf(0, "Database error %s", db_strerror(rc));
}
@end
