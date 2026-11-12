#include "capi.h"
#include <stdint.h>
#include <stdlib.h>

static void echo(capi_ctx *ctx) {
    capi_str name;
    capi_query(ctx, "name", &name);
    capi_json_string(ctx, name);
    capi_str body = capi_body(ctx);
    capi_write(ctx, body.data, body.len);
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536) return 0;
    capi_app *app = capi_app_new();
    if (!app) abort();
    capi_route(app, CAPI_ANY, "/", echo, NULL);
    capi_route(app, CAPI_ANY, "/:id/*rest", echo, NULL);
    capi_result result;
    capi_dispatch(app, data, size, &result);
    capi_result_free(&result); capi_app_free(app);
    return 0;
}
