#include "users.h"
#include <inttypes.h>
#include <string.h>

static void authorize(capi_ctx *ctx) {
    if (!capi_str_eq(capi_header(ctx, "Authorization"), capi_data(ctx))) {
        capi_set_header(ctx, "WWW-Authenticate", "Bearer");
        capi_json(ctx, 401, "{\"error\":\"Use Authorization: Bearer local-demo\"}"); return;
    }
    capi_next(ctx);
}
static void index_action(capi_ctx *ctx) {
    capi_json(ctx, 200, "{\"module\":");
    const char *label = capi_data(ctx);
    capi_json_string(ctx, (capi_str){label, strlen(label)});
    capi_write(ctx, ",\"users\":[]}", 12);
}
static void show_action(capi_ctx *ctx) {
    int64_t id;
    if (capi_param_i64(ctx, "id", 1, INT64_MAX, &id) != CAPI_INPUT_FOUND) {
        capi_json(ctx, 400, "{\"error\":\"Invalid user ID\"}"); return;
    }
    capi_json(ctx, 200, "{\"module\":");
    const char *label = capi_data(ctx);
    capi_json_string(ctx, (capi_str){label, strlen(label)});
    capi_printf(ctx, ",\"id\":%" PRId64 "}", id);
}
capi_router *users_router_new(const char *label) {
    if (!label) return NULL;
    capi_router *users = capi_router_new();
    if (!users) return NULL;
    if (capi_router_use(users, authorize, "Bearer local-demo") ||
        capi_router_route(users, CAPI_GET, "", index_action, (void *)label) ||
        capi_router_route(users, CAPI_GET, "/:id", show_action, (void *)label)) {
        capi_router_free(users); return NULL;
    }
    return users;
}
