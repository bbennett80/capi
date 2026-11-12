#ifndef CAPI_WORKERS_H
#define CAPI_WORKERS_H
#include "capi.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct capi_workers capi_workers;
typedef struct capi_job capi_job;
typedef int (*capi_worker_init_fn)(void *data, void **worker_state);
typedef void (*capi_worker_dispose_fn)(void *worker_state);
typedef struct {
    size_t threads;    /* 0: 4; maximum 256 */
    size_t capacity;   /* 0: 128; maximum 65536; ALL outstanding jobs */
    capi_worker_init_fn init; /* on each worker; 0 success, nonzero failure */
    capi_worker_dispose_fn dispose; /* on same worker, for non-NULL state */
    void *data; /* borrowed by init; must outlive the pool */
} capi_workers_options;

/* Waits for all initializers. Failure joins every started thread and disposes
 * non-NULL state, including state returned by a failing initializer. Sets errno.
 * Initializers may run concurrently. The pool may be shared across servers. */
capi_workers *capi_workers_new(capi_workers_options options);
/* Stop accepting, drain/join workers, release the owner's reference.
 * Normally call after server_run returns. Never call from a worker callback.
 * Already posted completions retain the pool until the loop releases them.
 * Do not submit/use the pool after free, or free concurrently with another free. */
void capi_workers_free(capi_workers *pool);

typedef void (*capi_work_fn)(capi_job *job, void *worker_state, void *data);
typedef void (*capi_work_done_fn)(capi_ctx *ctx, void *data);
typedef enum {
    CAPI_WORK_ERROR = -1, CAPI_WORK_OK = 0,
    CAPI_WORK_FULL = 1, CAPI_WORK_UNAVAILABLE = 2
} capi_work_result;
/* Submit from a live request's event-loop thread, once per request.
 * On success, work owns data until done. Request arena/views remain alive;
 * do not mutate submitted data after returning OK. work must not use ctx APIs.
 * done runs on the event loop; it is skipped on timeout/shutdown cancellation.
 * capi_defer cleanup runs on that loop after work/done, even if cancelled.
 * On rejection no callbacks run. Handle FULL/UNAVAILABLE explicitly (e.g. 503).
 * Not available in capi_dispatch or in a completion callback. */
capi_work_result capi_work_submit(capi_ctx *ctx, capi_workers *pool,
                                  capi_work_fn work, capi_work_done_fn done, void *data);
/* Cooperative cancellation. job is valid only within its work callback.
 * Queued cancelled jobs skip work; running callbacks must eventually return. */
bool capi_job_cancelled(const capi_job *job);

typedef struct {
    size_t threads, capacity, queued, running, outstanding;
} capi_workers_stats;
/* Thread-safe snapshot; outstanding includes completions waiting on the loop. */
void capi_workers_get_stats(capi_workers *pool, capi_workers_stats *stats);

#ifdef __cplusplus
}
#endif
#endif
