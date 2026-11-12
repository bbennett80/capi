#include "capi_template.h"
#include <stdint.h>
#include <stdlib.h>

typedef struct { capi_template *view; capi_str input; } fixture;
static void render(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    capi_field fields[] = {{"name", capi_value_string(f->input)}, {"yes", capi_value_bool(true)}};
    capi_value items[] = {capi_value_object(fields, 2), capi_value_int(42)};
    capi_field root[] = {{"name", capi_value_string(f->input)}, {"items", capi_value_list(items, 2)},
                         {"yes", capi_value_bool(true)}, {"empty", capi_value_bool(false)}};
    capi_value data = capi_value_object(root, 4);
    capi_partial partials[] = {{"card", f->view}};
    capi_render_with(ctx, 200, f->view, &data, partials, 1);
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    if (len > 65536) return 0;
    capi_template *view = capi_template_compile((const char *)data, len, NULL);
    if (!view) return 0;
    capi_app *app = capi_app_new();
    if (!app) abort();
    fixture f = {view, {(const char *)data, len}};
    if (capi_route(app, CAPI_GET, "/", render, &f)) abort();
    const char request[] = "GET / HTTP/1.1\r\nHost: test\r\n\r\n";
    capi_result result;
    if (capi_dispatch(app, request, sizeof(request) - 1, &result) < 0) abort();
    capi_result_free(&result); capi_app_free(app); capi_template_free(view);
    return 0;
}
