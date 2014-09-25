#include <assert.h>
#include <stdio.h> /* For debugging */
#include <stdlib.h>
#include <limits.h>
#include "async.h"

#define READERS_MAX 512

enum {
	s_write = INT_MAX,
};

typedef struct thread_list thread_list;
struct thread_list {
	cothread_t thread;
	thread_list *next;
};

struct async_rwlock_s {
	int state;

	thread_list *rdhead;
	thread_list *rdtail;

	thread_list *wrhead;
	thread_list *wrtail;

	cothread_t upgrade;
};

static void wake_next(async_rwlock_t *const lock);

async_rwlock_t *async_rwlock_create(void) {
	async_rwlock_t *lock = calloc(1, sizeof(async_rwlock_t));
	if(!lock) return NULL;
	return lock;
}
void async_rwlock_free(async_rwlock_t *const lock) {
	if(!lock) return;
	assert(0 == lock->state);
	assert(!lock->rdhead);
	assert(!lock->rdtail);
	assert(!lock->wrhead);
	assert(!lock->wrtail);
	assert(!lock->upgrade);
	free(lock);
}
void async_rwlock_rdlock(async_rwlock_t *const lock) {
	assert(lock);
	assert(yield);
	assert(co_active() != yield);
	if(async_rwlock_tryrdlock(lock) >= 0) return;
	thread_list us = {
		.thread = co_active(),
		.next = NULL,
	};
	if(!lock->rdhead) lock->rdhead = &us;
	if(lock->rdtail) lock->rdtail->next = &us;
	lock->rdtail = &us;
	async_yield();
	assert(lock->state > 0);
	assert(lock->state <= READERS_MAX);
	assert(!lock->wrhead);
	assert(!lock->upgrade);
}
int async_rwlock_tryrdlock(async_rwlock_t *const lock) {
	assert(lock);
	if(!lock->upgrade && !lock->wrhead && lock->state < READERS_MAX) {
		++lock->state;
		return 0;
	}
	return -1;
}
void async_rwlock_rdunlock(async_rwlock_t *const lock) {
	assert(lock);
	assert(lock->state > 0);
	assert(lock->state <= READERS_MAX);
	--lock->state;
	wake_next(lock);
}
void async_rwlock_wrlock(async_rwlock_t *const lock) {
	assert(lock);
	assert(yield);
	assert(co_active() != yield);
	if(async_rwlock_trywrlock(lock) >= 0) return;
	thread_list us = {
		.thread = co_active(),
		.next = NULL,
	};
	if(!lock->wrhead) lock->wrhead = &us;
	if(lock->wrtail) lock->wrtail->next = &us;
	lock->wrtail = &us;
	async_yield();
	assert(s_write == lock->state);
	assert(!lock->upgrade);
}
int async_rwlock_trywrlock(async_rwlock_t *const lock) {
	assert(lock);
	if(!lock->upgrade && 0 == lock->state) {
		lock->state = s_write;
		return 0;
	}
	return -1;
}
void async_rwlock_wrunlock(async_rwlock_t *const lock) {
	assert(lock);
	assert(s_write == lock->state);
	assert(!lock->upgrade && "Upgrade pending during write");
	lock->state = 0;
	wake_next(lock);
}

int async_rwlock_upgrade(async_rwlock_t *const lock) {
	assert(lock);
	assert(lock->state > 0);
	assert(lock->state <= READERS_MAX);
	if(lock->upgrade) return -1;
	--lock->state;
	if(lock->state > 0) {
		lock->upgrade = co_active();
		async_yield();
		assert(!lock->upgrade && "Upgrade not cleared");
		assert(s_write == lock->state && "Wrong upgrade woken");
	} else {
		lock->state = s_write;
	}
	return 0;
}
void async_rwlock_downgrade(async_rwlock_t *const lock) {
	assert(lock);
	assert(s_write == lock->state);
	assert(!lock->upgrade && "Upgrade pending during write");
	lock->state = 1;
	wake_next(lock);
}

static void wake_next(async_rwlock_t *const lock) {
	if(lock->upgrade) {
		if(lock->state > 0) return;
		lock->state = s_write;
		cothread_t const next = lock->upgrade;
		lock->upgrade = NULL;
		async_wakeup(next);
	} else if(lock->wrhead) {
		if(lock->state > 0) return;
		lock->state = s_write;
		thread_list *const next = lock->wrhead;
		lock->wrhead = next->next;
		if(!lock->wrhead) lock->wrtail = NULL;
		async_wakeup(next->thread);
	} else while(lock->rdhead) {
		if(lock->state >= READERS_MAX) return;
		++lock->state;
		thread_list *const next = lock->rdhead;
		lock->rdhead = next->next;
		if(!lock->rdhead) lock->rdtail = NULL;
		async_wakeup(next->thread);
	}
}
