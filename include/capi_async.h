#ifndef CAPI_ASYNC_H
#define CAPI_ASYNC_H
#include "capi.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Advanced executor-adapter API. Most applications should use capi_workers.h.
 * These functions remain in the core library; this header makes the lifetime
 * contract explicit without exposing tokens in the ordinary HTTP interface. */
typedef struct capi_async capi_async;
typedef void (*capi_async_done_fn)(capi_ctx *ctx, void *data);
/* Begin on the event loop, once per request, during a live server pipeline.
 * Unavailable in capi_dispatch/completions. Middleware unwinds before done;
 * request views, arena, and cleanup remain alive until completion/cleanup. */
bool capi_async_available(const capi_ctx *ctx);
CAPI_NODISCARD capi_async *capi_async_begin(capi_ctx *ctx);
/* Producer-thread functions. Complete EXACTLY ONCE even after cancellation,
 * including when scheduling fails after begin. Callback runs on the loop,
 * with ctx == NULL if cancelled. Complete consumes the token and relinquishes
 * access to request-owned memory/results; never access them afterward.
 * Producers must not use context APIs. done may be NULL. */
bool capi_async_cancelled(const capi_async *pending);
void capi_async_complete(capi_async *pending, capi_async_done_fn done, void *data);

#ifdef __cplusplus
}
#endif
#endif
