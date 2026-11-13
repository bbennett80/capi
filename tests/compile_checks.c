#include "capi.h"
#ifdef CHECK_WORKERS
#include "capi_workers.h"
#endif
#ifdef CHECK_SQLITE
#include "capi_sqlite.h"
#endif
static void handler(capi_ctx *ctx) { capi_text(ctx, 200, "ok"); }
#ifdef CHECK_WORKERS
static void work(capi_job *job, void *state, void *data) { (void)job; (void)state; (void)data; }
static void done(capi_ctx *ctx, void *data) { (void)data; handler(ctx); }
#endif
void compile_check(capi_app *app, capi_ctx *ctx) {
#if CHECK_CASE == 1
    capi_printf(ctx, "%d", "wrong type");
#elif CHECK_CASE == 2
    capi_route(app, CAPI_GET, "/", handler, NULL);
#elif CHECK_CASE == 3
    capi_work_submit(ctx, NULL, work, done, NULL);
#elif CHECK_CASE == 4
    capi_sqlite_commit(NULL);
#elif CHECK_CASE == 5
    capi_async *advanced_type_must_require_its_header = NULL;
    (void)advanced_type_must_require_its_header;
#elif CHECK_CASE == 6
    capi_include_router(app, "/api", NULL);
#else
    if (capi_route(app, CAPI_GET, "/", handler, NULL)) return;
    capi_printf(ctx, "%d %s", 42, "checked");
#ifdef CHECK_WORKERS
    capi_work_result result = capi_work_submit(ctx, NULL, work, done, NULL);
    if (result != CAPI_WORK_OK) return;
#endif
#ifdef CHECK_SQLITE
    if (capi_sqlite_commit(NULL) != SQLITE_OK) return;
#endif
#endif
}
