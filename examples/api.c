#include "capi.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static capi_server *server;
static unsigned long request_id;
static void stop(int n) { (void)n; capi_server_stop(server); }

static void request_context(capi_ctx *ctx) {
    unsigned long *id = capi_alloc(ctx, sizeof(*id));
    if (!id) return;
    *id = ++request_id;
    capi_set_state(ctx, id);
    char value[32];
    snprintf(value, sizeof(value), "%lu", *id);
    capi_set_header(ctx, "X-Request-Id", value);
    capi_set_header(ctx, "X-Content-Type-Options", "nosniff");
    capi_next(ctx);
    capi_str method = capi_request_method(ctx), path = capi_path(ctx);
    fprintf(stderr, "[%lu] %.*s %.*s -> %d\n", *id, (int)method.len, method.data,
            (int)path.len, path.data, capi_response_status(ctx));
}

static void authorize(capi_ctx *ctx) {
    const char *expected = capi_data(ctx);
    if (!capi_str_eq(capi_header(ctx, "Authorization"), expected)) {
        capi_set_header(ctx, "WWW-Authenticate", "Bearer");
        capi_json(ctx, 401, "{\"error\":\"provide Authorization: Bearer local-demo\"}");
        return;
    }
    capi_next(ctx);
}

static void index_page(capi_ctx *ctx) {
    capi_json(ctx, 200, "{\"framework\":\"capi\",\"version\":\"" CAPI_VERSION "\","
              "\"routes\":[\"GET /hello?name=Ada\",\"GET /users/:id\",\"POST /echo\",\"GET /private/status\"]}");
}

static void greet(capi_ctx *ctx) {
    capi_str name = {"world", 5}, value;
    int found = capi_query(ctx, "name", &value);
    if (found < 0) { capi_json(ctx, 400, "{\"error\":\"invalid query encoding\"}"); return; }
    if (found) name = value;
    capi_json(ctx, 200, "{\"hello\":");
    capi_json_string(ctx, name);
    capi_write(ctx, "}", 1);
}

static void user(capi_ctx *ctx) {
    capi_json(ctx, 200, "{\"id\":");
    capi_json_string(ctx, capi_param(ctx, "id"));
    capi_printf(ctx, ",\"request_id\":%lu}", *(unsigned long *)capi_state(ctx));
}

static void echo(capi_ctx *ctx) {
    capi_str body = capi_body(ctx);
    capi_set_header(ctx, "Content-Type", "application/octet-stream");
    capi_write(ctx, body.data, body.len);
}
static void status(capi_ctx *ctx) { capi_json(ctx, 200, "{\"status\":\"ready\"}"); }
static void missing(capi_ctx *ctx) { capi_json(ctx, 404, "{\"error\":\"route not found\"}"); }

int main(void) {
    capi_app *app = capi_app_new();
    if (!app) return 1;
    if (capi_use(app, "/", request_context, NULL) ||
        capi_use(app, "/private", authorize, "Bearer local-demo") ||
        capi_route(app, CAPI_GET, "/", index_page, NULL) ||
        capi_route(app, CAPI_GET, "/hello", greet, NULL) ||
        capi_route(app, CAPI_GET, "/users/:id", user, NULL) ||
        capi_route(app, CAPI_POST, "/echo", echo, NULL) ||
        capi_route(app, CAPI_GET, "/private/status", status, NULL)) goto fail;
    capi_not_found(app, missing, NULL);
    server = capi_server_listen(app, (capi_server_options){.port = 8080});
    if (!server) goto fail;
    signal(SIGINT, stop); signal(SIGTERM, stop);
    printf("capi API · http://127.0.0.1:%u · %s\n", (unsigned)capi_server_port(server), capi_server_backend());
    fflush(stdout);
    int result = capi_server_run(server);
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
    capi_server_free(server); capi_app_free(app);
    return result ? 1 : 0;
fail:
    fprintf(stderr, "capi: %s\n", capi_app_error(app)); capi_app_free(app); return 1;
}
