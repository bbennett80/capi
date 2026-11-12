#define _POSIX_C_SOURCE 200809L
#include "capi_workers.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

typedef struct { capi_workers *pool; pthread_t thread; } cw_thread;
struct capi_workers {
    capi_workers_options options;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    cw_thread *threads;
    capi_job **queue;
    size_t nthreads, ready, head, tail, queued, running, outstanding;
    bool stopping, init_failed, owner_released;
};
struct capi_job {
    capi_workers *pool;
    capi_async *pending;
    capi_work_fn work;
    capi_work_done_fn done;
    void *data;
};

static void destroy_pool(capi_workers *pool) {
    pthread_cond_destroy(&pool->wake);
    pthread_mutex_destroy(&pool->mutex);
    free(pool->threads); free(pool->queue); free(pool);
}

bool capi_job_cancelled(const capi_job *job) {
    return !job || capi_async_cancelled(job->pending);
}

static void complete(capi_ctx *ctx, void *data) {
    capi_job *job = data;
    capi_workers *pool = job->pool;
    if (ctx) job->done(ctx, job->data);
    pthread_mutex_lock(&pool->mutex);
    --pool->outstanding;
    bool release = pool->owner_released && !pool->outstanding;
    pthread_mutex_unlock(&pool->mutex);
    free(job);
    if (release) destroy_pool(pool);
}

static void *worker_main(void *argument) {
    cw_thread *thread = argument;
    capi_workers *pool = thread->pool;
    void *state = NULL;
    int rc = pool->options.init ? pool->options.init(pool->options.data, &state) : 0;
    pthread_mutex_lock(&pool->mutex);
    if (rc) pool->init_failed = true;
    ++pool->ready;
    pthread_cond_broadcast(&pool->wake);
    for (;;) {
        if (rc) break;
        while (!pool->queued && !pool->stopping) pthread_cond_wait(&pool->wake, &pool->mutex);
        if (!pool->queued) break;
        capi_job *job = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->options.capacity;
        --pool->queued; ++pool->running;
        pthread_mutex_unlock(&pool->mutex);
        if (!capi_job_cancelled(job)) job->work(job, state, job->data);
        pthread_mutex_lock(&pool->mutex);
        --pool->running;
        pthread_mutex_unlock(&pool->mutex);
        /* The loop may free job before this call returns. Do not touch it after. */
        capi_async_complete(job->pending, complete, job);
        pthread_mutex_lock(&pool->mutex);
    }
    pthread_mutex_unlock(&pool->mutex);
    if (state && pool->options.dispose) pool->options.dispose(state);
    return NULL;
}

capi_workers *capi_workers_new(capi_workers_options options) {
    if (!options.threads) options.threads = 4;
    if (!options.capacity) options.capacity = 128;
    if (options.threads > 256 || options.capacity > 65536) { errno = EINVAL; return NULL; }
    capi_workers *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;
    pool->options = options;
    pool->threads = calloc(options.threads, sizeof(*pool->threads));
    pool->queue = calloc(options.capacity, sizeof(*pool->queue));
    if (!pool->threads || !pool->queue) {
        free(pool->threads); free(pool->queue); free(pool); errno = ENOMEM; return NULL;
    }
    int rc = pthread_mutex_init(&pool->mutex, NULL);
    if (rc) { free(pool->threads); free(pool->queue); free(pool); errno = rc; return NULL; }
    rc = pthread_cond_init(&pool->wake, NULL);
    if (rc) { pthread_mutex_destroy(&pool->mutex); free(pool->threads); free(pool->queue); free(pool); errno = rc; return NULL; }
    for (size_t i = 0; i < options.threads; ++i) {
        pool->threads[i].pool = pool;
        rc = pthread_create(&pool->threads[i].thread, NULL, worker_main, &pool->threads[i]);
        if (rc) break;
        ++pool->nthreads;
    }
    pthread_mutex_lock(&pool->mutex);
    while (pool->ready < pool->nthreads) pthread_cond_wait(&pool->wake, &pool->mutex);
    bool failed = rc || pool->init_failed;
    pthread_mutex_unlock(&pool->mutex);
    if (failed) { int error = rc ? rc : EIO; capi_workers_free(pool); errno = error; return NULL; }
    return pool;
}

void capi_workers_free(capi_workers *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    pthread_cond_broadcast(&pool->wake);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i = 0; i < pool->nthreads; ++i) (void)pthread_join(pool->threads[i].thread, NULL);
    pthread_mutex_lock(&pool->mutex);
    pool->owner_released = true;
    bool release = !pool->outstanding;
    pthread_mutex_unlock(&pool->mutex);
    if (release) destroy_pool(pool);
}

capi_work_result capi_work_submit(capi_ctx *ctx, capi_workers *pool,
                                  capi_work_fn work, capi_work_done_fn done, void *data) {
    if (!ctx || !pool || !work || !done) { errno = EINVAL; return CAPI_WORK_ERROR; }
    if (!capi_async_available(ctx)) return CAPI_WORK_UNAVAILABLE;
    pthread_mutex_lock(&pool->mutex);
    if (pool->stopping) { pthread_mutex_unlock(&pool->mutex); return CAPI_WORK_UNAVAILABLE; }
    if (pool->outstanding == pool->options.capacity) { pthread_mutex_unlock(&pool->mutex); return CAPI_WORK_FULL; }
    capi_job *job = malloc(sizeof(*job));
    if (!job) { pthread_mutex_unlock(&pool->mutex); capi_fail(ctx); return CAPI_WORK_ERROR; }
    capi_async *pending = capi_async_begin(ctx);
    if (!pending) { free(job); pthread_mutex_unlock(&pool->mutex); return errno == EAGAIN ? CAPI_WORK_UNAVAILABLE : CAPI_WORK_ERROR; }
    *job = (capi_job){pool, pending, work, done, data};
    pool->queue[pool->tail] = job;
    pool->tail = (pool->tail + 1) % pool->options.capacity;
    ++pool->queued; ++pool->outstanding;
    pthread_cond_signal(&pool->wake);
    pthread_mutex_unlock(&pool->mutex);
    return CAPI_WORK_OK;
}

void capi_workers_get_stats(capi_workers *pool, capi_workers_stats *stats) {
    if (!pool || !stats) return;
    pthread_mutex_lock(&pool->mutex);
    *stats = (capi_workers_stats){pool->nthreads, pool->options.capacity, pool->queued, pool->running, pool->outstanding};
    pthread_mutex_unlock(&pool->mutex);
}
