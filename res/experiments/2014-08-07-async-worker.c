

typedef struct async_worker_s async_worker_t;

async_worker_t *async_worker_create(void);
void async_worker_free(async_worker_t *const worker);
void async_worker_enter(async_worker_t *const worker);
void async_worker_leave(async_worker_t *const worker);


struct async_worker_s {
	uv_thread_t thread;
	uv_sem_t sem;
	cothread_t work;
	cothread_t main;
	uv_async_t async;
};


static work(async_worker_t *const worker) {
	worker->main = co_active();
	for(;;) {
		uv_sem_wait(&worker->sem);
		if(!worker->work) break;
		co_switch(worker->work);
		uv_async_send(work->async);
	}
}

static void enter(async_worker_t *const worker) {
	uv_sem_post(worker->sem);
}
static void leave(uv_async_t *const async) {
	async_worker_t *const worker = async->data;
	cothread_t const work = worker->work;
	worker->work = NULL;
	co_switch(work);
}

async_worker_t *async_worker_create(void) {
	async_worker_t *worker = calloc(1, sizeof(struct async_worker_s));
	if(!worker) goto bail;
	if(uv_sem_init(&worker->sem, 0) < 0) goto bail;
	worker->async.data = worker;
	if(uv_async_init(loop, &worker->async, leave) < 0) goto bail;
	if(uv_thread_create(&worker->thread, work, worker) < 0) goto bail;
	return worker;

bail:
	async_worker_free(worker);
	return NULL;
}
void async_worker_free(async_worker_t *const worker) {
	if(!worker) return;
	assert(!worker->work);
	uv_sem_post(worker->sem);
	uv_thread_join(worker->thread);
	uv_sem_destroy(worker->sem);
	uv_close((uv_handle_t *)&worker->async);
	free(worker);
}
void async_worker_enter(async_worker_t *const worker) {
	assert(worker);
	assert(!worker->work);
	worker->work = co_active();
	async_call(enter, worker);
	// Now on worker thread
}
void async_worker_leave(async_worker_t *const worker) {
	assert(worker);
	assert(co_active() == worker->work);
	co_switch(worker->main);
	// Now on original thread
}



