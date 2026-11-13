#include <capi.h>
#include <capi_async.h> /* separately installed expert header */
#include <string.h>
#ifdef CONSUME_WORKERS
#include <capi_workers.h>
#endif
#ifdef CONSUME_EXTRAS
#include <capi_sqlite.h>
#include <capi_template.h>
#endif

static unsigned reports;
static void observed(const capi_request_report *report, void *data) {
    (void)data;
    if (report->status == 200 && report->outcome == CAPI_OUTCOME_DISPATCHED && report->id) ++reports;
}
static void hello(capi_ctx *ctx) {
    int64_t number;
    if (capi_query_i64(ctx, "n", 1, 10, &number) != CAPI_INPUT_FOUND || number != 7 || capi_async_available(ctx)) {
        capi_fail(ctx); return;
    }
#ifdef CONSUME_EXTRAS
    capi_value value = capi_value_text("installed");
    capi_render(ctx, 200, capi_data(ctx), &value);
#else
    capi_text(ctx, 200, "installed");
#endif
}
int main(void) {
    void *data = NULL;
#ifdef CONSUME_WORKERS
    capi_workers *pool = capi_workers_new((capi_workers_options){.threads = 1, .capacity = 2});
    if (!pool) return 1;
    capi_workers_stats stats;
    capi_workers_get_stats(pool, &stats);
    int pool_failed = stats.threads != 1 || stats.capacity != 2 || stats.outstanding != 0;
    capi_workers_free(pool);
    if (pool_failed) return 1;
#endif
#ifdef CONSUME_EXTRAS
    sqlite3 *db = NULL;
    if (capi_sqlite_open(":memory:", &db) != SQLITE_OK) { sqlite3_close(db); return 1; }
    if (sqlite3_close(db) != SQLITE_OK) return 1;
    capi_template *view = capi_template_compile("{{.}}", 5, NULL);
    if (!view) return 1;
    data = view;
#endif
    capi_app *app = capi_app_new();
    if (!app) return 1;
    capi_on_finish(app, observed, NULL);
    capi_router *router = capi_router_new();
    if (!router || capi_router_route(router, CAPI_GET, "", hello, data) ||
        capi_include_router(app, "", router)) return 1;
    capi_router_free(router); /* the installed library owns its route snapshot */
    const char request[] = "GET /?n=7 HTTP/1.1\r\nHost: test\r\n\r\n";
    capi_result result;
    int rc = capi_dispatch(app, request, sizeof(request) - 1, &result);
    int failed = rc != 1 || result.len < 13 || memcmp(result.data, "HTTP/1.1 200 ", 13) || reports != 1;
    capi_result_free(&result); capi_app_free(app);
#ifdef CONSUME_EXTRAS
    capi_template_free(view);
#endif
    return failed;
}
