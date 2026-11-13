#include "support.h"

static char trace[1024];
static size_t at;
static unsigned visits, cleaned;
static void middleware(capi_ctx *ctx) {
    const char *pair = capi_data(ctx);
    trace[at++] = pair[0];
    capi_next(ctx);
    CHECK(capi_data(ctx) == pair);
    trace[at++] = pair[1];
    capi_next(ctx); /* once per frame */
}
static void count_middleware(capi_ctx *ctx) { ++visits; capi_next(ctx); }
static void pass(capi_ctx *ctx) { capi_next(ctx); }
static void block(capi_ctx *ctx) { capi_text(ctx, 403, "blocked"); }
static void handler(capi_ctx *ctx) { trace[at++] = 'h'; capi_text(ctx, 200, capi_data(ctx)); }
static void cleanup(void *data) { CHECK(data == &cleaned); ++cleaned; }
static void deferred(capi_ctx *ctx) { CHECK(capi_defer(ctx, cleanup, &cleaned) == 0); capi_next(ctx); }
static capi_result call(capi_app *app, const char *method, const char *path, int status) {
    char request[8192]; snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: test\r\n\r\n", method, path);
    memset(trace, 0, sizeof(trace)); at = 0;
    return request_text(app, request, status);
}
static void expect(capi_app *app, const char *path, int status, const char *body) {
    capi_result r = call(app, "GET", path, status);
    if (body) body_equals(&r, body);
    capi_result_free(&r);
}
static void parameters(capi_ctx *ctx) {
    CHECK(capi_str_eq(capi_path(ctx), "/org/acme/items/42"));
    CHECK(capi_str_eq(capi_param(ctx, "org"), "acme"));
    CHECK(capi_str_eq(capi_param(ctx, "item"), "42"));
    capi_text(ctx, 200, capi_data(ctx));
}
static void groups(void) {
    capi_app *app = capi_app_new(); CHECK(app);
    capi_router *users = capi_router_new(), *api = capi_router_new(); CHECK(users && api);
    char path[] = "/:id";
    CHECK(capi_router_use(users, middleware, "cC") == 0);
    CHECK(capi_router_route(users, CAPI_GET, path, handler, "user") == 0); path[1] = 'x';
    CHECK(capi_router_route(users, CAPI_GET, "/fixed", handler, "literal") == 0);
    CHECK(capi_router_include(api, "/users", users) == 0);
    CHECK(capi_router_use(api, middleware, "bB") == 0); /* also wraps previously included routes */
    CHECK(capi_use(app, "/", middleware, "aA") == 0);
    char prefix[] = "/v1";
    CHECK(capi_include_router(app, prefix, api) == 0); prefix[1] = 'x';
    CHECK(capi_include_router(app, "/latest", api) == 0);
    CHECK(capi_router_route(users, CAPI_GET, "/late", handler, "late") == 0);
    CHECK(capi_router_route(api, CAPI_GET, "/new", handler, "new") == 0);
    CHECK(capi_include_router(app, "/v2", api) == 0);
    capi_router_free(users); capi_router_free(api);
    expect(app, "/v1/users/42", 200, "user"); CHECK(!strcmp(trace, "abchCBA"));
    expect(app, "/latest/users/fixed", 200, "literal"); CHECK(!strcmp(trace, "abchCBA"));
    expect(app, "/v1/new", 404, NULL); CHECK(!strcmp(trace, "aA"));
    expect(app, "/v2/new", 200, "new"); CHECK(!strcmp(trace, "abhBA"));
    /* The child was snapshotted into api before /late existed. */
    expect(app, "/v2/users/late", 200, "user");
    CHECK(capi_route(app, CAPI_GET, "/v1/unrelated", handler, "other") == 0);
    expect(app, "/v1/unrelated", 200, "other"); CHECK(!strcmp(trace, "ahA"));
    capi_app_free(app);
}
static void roots_and_validation(void) {
    capi_router *child = capi_router_new(), *parent = capi_router_new(); CHECK(child && parent);
    CHECK(capi_router_route(child, CAPI_GET, "", handler, "base") == 0);
    CHECK(capi_router_route(child, CAPI_GET, "/", handler, "slash") == 0);
    CHECK(capi_router_include(parent, "", child) == 0);
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_include_router(app, "/api", parent) == 0);
    expect(app, "/api", 200, "base"); expect(app, "/api/", 200, "slash");
    CHECK(capi_include_router(app, "", parent) == -1); /* both would become '/' */
    expect(app, "/", 404, NULL);
    const char *bad[] = {NULL, "relative", "/trailing/", "/bad//prefix", "/bad/*rest", "/:x/:x", "/x?y"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        CHECK(capi_include_router(app, bad[i], child) == -1);
        CHECK(capi_router_include(parent, bad[i], child) == -1);
    }
    CHECK(capi_router_include(child, "/x", child) == -1);
    CHECK(capi_router_route(child, CAPI_GET, "relative", handler, NULL) == -1);
    CHECK(capi_router_route(child, CAPI_GET, "/:x/:x", handler, NULL) == -1);
    CHECK(capi_router_route(child, CAPI_GET, "/x", NULL, NULL) == -1);
    CHECK(capi_router_route(child, CAPI_GET, NULL, handler, NULL) == -1);
    CHECK(capi_router_use(child, NULL, NULL) == -1);
    CHECK(strlen(capi_router_error(child)) > 0 && strlen(capi_app_error(app)) > 0);
    capi_router_free(child); capi_router_free(parent); capi_app_free(app);
    child = capi_router_new(); CHECK(child);
    CHECK(capi_router_route(child, CAPI_GET, "/:item", parameters, "params") == 0);
    CHECK(capi_router_route(child, CAPI_GET, "/:other", handler, NULL) == -1);
    app = capi_app_new(); CHECK(app);
    CHECK(capi_include_router(app, "/org/:org/items", child) == 0);
    CHECK(capi_include_router(app, "/:item", child) == -1);
    expect(app, "/org/acme/items/42", 200, "params");
    capi_router_free(child); capi_app_free(app);
    child = capi_router_new(); app = capi_app_new(); CHECK(child && app);
    CHECK(capi_router_route(child, CAPI_GET, "", handler, "root") == 0);
    CHECK(capi_include_router(app, "/", child) == 0); expect(app, "/", 200, "root");
    capi_router_free(child); capi_app_free(app);
}
static void isolation_and_atomicity(void) {
    capi_app *app = capi_app_new(); CHECK(app);
    capi_router *a = capi_router_new(), *b = capi_router_new(); CHECK(a && b);
    CHECK(capi_router_use(a, count_middleware, NULL) == 0);
    CHECK(capi_router_use(a, deferred, NULL) == 0);
    CHECK(capi_router_route(a, CAPI_GET, "/a", handler, "a") == 0);
    CHECK(capi_router_route(b, CAPI_GET, "/b", handler, "b") == 0);
    CHECK(capi_include_router(app, "/api", a) == 0);
    CHECK(capi_include_router(app, "/api", b) == 0);
    visits = cleaned = 0;
    expect(app, "/api/b", 200, "b"); CHECK(visits == 0 && cleaned == 0);
    expect(app, "/api/missing", 404, NULL); CHECK(visits == 0);
    capi_result r = call(app, "POST", "/api/a", 405); CHECK(strstr(r.data, "Allow: GET, HEAD, OPTIONS")); capi_result_free(&r); CHECK(visits == 0);
    r = call(app, "OPTIONS", "/api/a", 204); capi_result_free(&r); CHECK(visits == 0);
    r = call(app, "HEAD", "/api/a", 200); body_equals(&r, ""); capi_result_free(&r); CHECK(visits == 1 && cleaned == 1);
    CHECK(capi_router_route(b, CAPI_GET, "/a", handler, "conflict") == 0);
    CHECK(capi_include_router(app, "/api", b) == -1); /* /b conflicts, too */
    expect(app, "/api/a", 200, "a"); CHECK(visits == 2 && cleaned == 2);
    capi_router_free(a); capi_router_free(b); capi_app_free(app);
    a = capi_router_new(); b = capi_router_new(); app = capi_app_new(); CHECK(a && b && app);
    CHECK(capi_route(app, CAPI_GET, "/api/clash", handler, "old") == 0);
    CHECK(capi_router_route(a, CAPI_GET, "/new", handler, "new") == 0);
    CHECK(capi_router_route(a, CAPI_GET, "/clash", handler, "clash") == 0);
    CHECK(capi_include_router(app, "/api", a) == -1);
    expect(app, "/api/new", 404, NULL); expect(app, "/api/clash", 200, "old");
    CHECK(capi_router_route(b, CAPI_GET, "/clash", handler, "old") == 0);
    CHECK(capi_router_include(b, "", a) == -1);
    CHECK(capi_include_router(app, "/parent", b) == 0);
    expect(app, "/parent/new", 404, NULL); expect(app, "/parent/clash", 200, "old");
    capi_router_free(a); capi_router_free(b); capi_app_free(app);
}
static void limits_and_short_circuits(void) {
    capi_router *a = capi_router_new(), *b = capi_router_new(); CHECK(a && b);
    CHECK(capi_router_use(a, block, NULL) == 0);
    CHECK(capi_router_route(a, CAPI_ANY, "/any", handler, "unreachable") == 0);
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_include_router(app, "", a) == 0);
    capi_result r = call(app, "BREW", "/any", 403); body_equals(&r, "blocked"); capi_result_free(&r);
    capi_app_free(app); capi_router_free(a);
    a = capi_router_new(); CHECK(a);
    CHECK(capi_router_route(a, CAPI_GET, "/deep", handler, "deep") == 0);
    for (int i = 0; i < 128; ++i) CHECK(capi_router_use(a, pass, NULL) == 0);
    CHECK(capi_router_use(a, pass, NULL) == -1);
    CHECK(capi_router_use(b, pass, NULL) == 0);
    CHECK(capi_router_include(b, "", a) == -1);
    app = capi_app_new(); CHECK(app); CHECK(capi_include_router(app, "", a) == 0);
    expect(app, "/deep", 200, "deep");
    capi_app_free(app); capi_router_free(a); capi_router_free(b);
    CHECK(capi_router_route(NULL, CAPI_GET, "/", handler, NULL) == -1);
    CHECK(capi_include_router(NULL, "", NULL) == -1);
    capi_router_free(NULL);
}
int main(void) {
    groups(); roots_and_validation(); isolation_and_atomicity(); limits_and_short_circuits();
    printf("routers: %u checks passed\n", checks); return 0;
}
