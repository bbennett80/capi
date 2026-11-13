#include "capi.h"
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

static void echo(capi_ctx *ctx) {
    capi_str name;
    capi_input_result found = capi_query(ctx, "name", &name);
    if (found == CAPI_INPUT_NO_MEMORY) return;
    capi_json_string(ctx, name);
    int64_t number;
    if (capi_query_i64(ctx, "number", INT64_MIN, INT64_MAX, &number) == CAPI_INPUT_FOUND)
        capi_printf(ctx, "%" PRId64, number);
    if (capi_form(ctx, "name", &name) == 1) capi_json_string(ctx, name);
    capi_str body = capi_body(ctx);
    capi_write(ctx, body.data, body.len);
}
static void group(capi_ctx *ctx) { capi_next(ctx); }
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536) return 0;
    capi_app *app = capi_app_new();
    if (!app) abort();
    capi_router *router = capi_router_new();
    if (!router || capi_router_use(router, group, NULL) ||
        capi_router_route(router, CAPI_ANY, "/", echo, NULL) ||
        capi_router_route(router, CAPI_ANY, "/:id/*rest", echo, NULL) ||
        capi_include_router(app, "", router)) abort();
    capi_router_free(router);
    capi_result result;
    int rc = capi_dispatch(app, data, size, &result);
    if (rc < 0) abort();
    capi_result_free(&result); capi_app_free(app);
    return 0;
}
