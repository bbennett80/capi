#include <capi.h>
#include <string.h>
#ifdef CONSUME_WORKERS
#include <capi_workers.h>
#endif
#ifdef CONSUME_EXTRAS
#include <capi_sqlite.h>
#include <capi_template.h>
#endif

static void hello(capi_ctx *ctx) {
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
    if (capi_route(app, CAPI_GET, "/", hello, data)) return 1;
    const char request[] = "GET / HTTP/1.1\r\nHost: test\r\n\r\n";
    capi_result result;
    int rc = capi_dispatch(app, request, sizeof(request) - 1, &result);
    int failed = rc != 1 || result.len < 13 || memcmp(result.data, "HTTP/1.1 200 ", 13);
    capi_result_free(&result); capi_app_free(app);
#ifdef CONSUME_EXTRAS
    capi_template_free(view);
#endif
    return failed;
}
