#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_ROUTER_ROUTES = 4096, MAX_ROUTER_MIDDLEWARE = 128 };
typedef struct router_route {
    struct router_route *next;
    char *path;
    capi_method method;
    capi_handler handler;
    void *data;
    cp_binding *middleware; /* snapshot of included inner groups */
    size_t nmiddleware;
} router_route;
struct capi_router {
    router_route *first, *last;
    size_t nroutes;
    cp_binding middleware[MAX_ROUTER_MIDDLEWARE];
    size_t nmiddleware;
    char error[160];
};
static int fail(char error[160], const char *message) {
    snprintf(error, 160, "%s", message); return -1;
}
capi_router *capi_router_new(void) { return calloc(1, sizeof(capi_router)); }
static void free_routes(router_route *route) {
    while (route) {
        router_route *next = route->next;
        free(route->path); free(route->middleware); free(route); route = next;
    }
}
void capi_router_free(capi_router *router) {
    if (router) { free_routes(router->first); free(router); }
}
const char *capi_router_error(const capi_router *router) { return router ? router->error : "null router"; }

/* Share the core's pattern validation instead of maintaining another grammar.
 * This setup-only probe never executes handlers or retains user data. */
static int validate(char error[160], capi_method method, const char *path, capi_handler handler, void *data) {
    if (!path) return fail(error, "route path must not be NULL");
    capi_app *probe = capi_app_new();
    if (!probe) return fail(error, "out of memory");
    int result = capi_route(probe, method, *path ? path : "/", handler, data);
    if (result) fail(error, capi_app_error(probe));
    capi_app_free(probe); return result;
}
static void unused_handler(capi_ctx *ctx) { (void)ctx; }
static int validate_prefix(char error[160], const char *prefix) {
    if (!prefix) return fail(error, "router prefix must not be NULL (use an empty string for root)");
    size_t len = strlen(prefix);
    if (len > 1 && prefix[len - 1] == '/') return fail(error, "router prefix must not end with a slash");
    for (size_t i = 0; i < len; ++i)
        if (prefix[i] == '*' && (!i || prefix[i - 1] == '/')) return fail(error, "router prefix cannot contain a catch-all");
    return validate(error, CAPI_GET, prefix, unused_handler, NULL);
}
static char *join(char error[160], const char *prefix, const char *path) {
    if (!strcmp(prefix, "/")) prefix = "";
    size_t a = strlen(prefix), b = strlen(path);
    if (a > 4096 || b > 4096 - a) { fail(error, "combined route path is too long"); return NULL; }
    char *result = malloc(a + b + 1);
    if (!result) { fail(error, "out of memory"); return NULL; }
    memcpy(result, prefix, a); memcpy(result + a, path, b + 1); return result;
}
/* Empty base and slash remain distinct in a builder; parameter names do not
 * distinguish route shapes, just as in the core trie. */
static bool same_shape(const char *a, const char *b) {
    const char *p = a, *q = b;
    while (*p && *q) {
        bool segment = p == a || p[-1] == '/';
        if (segment && (*p == ':' || *p == '*') && *p == *q) {
            while (*p && *p != '/') ++p;
            while (*q && *q != '/') ++q;
        } else {
            if (*p++ != *q++) return false;
        }
    }
    return *p == *q;
}
static bool conflict(const capi_router *router, capi_method method, const char *path) {
    for (const router_route *r = router->first; r; r = r->next)
        if (r->method == method && same_shape(r->path, path)) return true;
    return false;
}
static router_route *copy_entry(char error[160], const char *path, capi_method method,
                                 capi_handler handler, void *data, const cp_binding *chain, size_t n) {
    router_route *r = calloc(1, sizeof(*r));
    if (!r) { fail(error, "out of memory"); return NULL; }
    r->path = malloc(strlen(path) + 1);
    if (!r->path) goto nomem;
    strcpy(r->path, path);
    if (n) {
        r->middleware = malloc(n * sizeof(*r->middleware));
        if (!r->middleware) goto nomem;
        memcpy(r->middleware, chain, n * sizeof(*r->middleware));
    }
    r->method = method; r->handler = handler; r->data = data; r->nmiddleware = n;
    return r;
nomem:
    free_routes(r); fail(error, "out of memory"); return NULL;
}
static void append(capi_router *router, router_route *route) {
    if (router->last) router->last->next = route;
    else router->first = route;
    router->last = route; ++router->nroutes;
}
int capi_router_route(capi_router *router, capi_method method, const char *path, capi_handler handler, void *data) {
    if (!router) return -1;
    if (validate(router->error, method, path, handler, data)) return -1;
    if (router->nroutes == MAX_ROUTER_ROUTES) return fail(router->error, "router route limit exceeded (4096)");
    if (conflict(router, method, path)) return fail(router->error, "duplicate route shape and method in router");
    router_route *r = copy_entry(router->error, path, method, handler, data, NULL, 0);
    if (!r) return -1;
    append(router, r); return 0;
}
int capi_router_use(capi_router *router, capi_handler handler, void *data) {
    if (!router) return -1;
    if (!handler) return fail(router->error, "router middleware requires a handler");
    if (router->nmiddleware == MAX_ROUTER_MIDDLEWARE) return fail(router->error, "router middleware limit exceeded (128)");
    for (const router_route *r = router->first; r; r = r->next)
        if (r->nmiddleware + router->nmiddleware >= MAX_ROUTER_MIDDLEWARE)
            return fail(router->error, "combined router middleware limit exceeded (128)");
    router->middleware[router->nmiddleware++] = (cp_binding){handler, data}; return 0;
}
int capi_router_include(capi_router *parent, const char *prefix, const capi_router *child) {
    if (!parent) return -1;
    if (!child || parent == child) return fail(parent->error, "include requires a distinct child router");
    if (validate_prefix(parent->error, prefix)) return -1;
    if (child->nroutes > MAX_ROUTER_ROUTES - parent->nroutes) return fail(parent->error, "router route limit exceeded (4096)");
    capi_router pending = {0};
    for (const router_route *r = child->first; r; r = r->next) {
        size_t n = child->nmiddleware + r->nmiddleware;
        if (n + parent->nmiddleware > MAX_ROUTER_MIDDLEWARE) {
            fail(parent->error, "combined router middleware limit exceeded (128)"); goto error;
        }
        cp_binding chain[MAX_ROUTER_MIDDLEWARE];
        memcpy(chain, child->middleware, child->nmiddleware * sizeof(*chain));
        if (r->nmiddleware) memcpy(chain + child->nmiddleware, r->middleware, r->nmiddleware * sizeof(*chain));
        char *path = join(parent->error, prefix, r->path);
        if (!path) goto error;
        if (validate(parent->error, r->method, path, r->handler, r->data)) { free(path); goto error; }
        if (conflict(parent, r->method, path) || conflict(&pending, r->method, path)) {
            fail(parent->error, "duplicate route shape and method in router include"); free(path); goto error;
        }
        router_route *copy = copy_entry(parent->error, path, r->method, r->handler, r->data, chain, n);
        free(path);
        if (!copy) goto error;
        append(&pending, copy);
    }
    if (pending.first) {
        if (parent->last) parent->last->next = pending.first;
        else parent->first = pending.first;
        parent->last = pending.last; parent->nroutes += pending.nroutes;
    }
    return 0;
error:
    free_routes(pending.first); return -1;
}
int capi_include_router(capi_app *app, const char *prefix, const capi_router *router) {
    if (!app) return -1;
    if (!router) return fail(app->error, "include requires a router");
    if (validate_prefix(app->error, prefix)) return -1;
    if (!router->nroutes) return 0;
    capi_app *pending = cp_clone_app_routes(app);
    if (!pending) return fail(app->error, "out of memory");
    for (const router_route *r = router->first; r; r = r->next) {
        cp_binding chain[MAX_ROUTER_MIDDLEWARE];
        memcpy(chain, router->middleware, router->nmiddleware * sizeof(*chain));
        if (r->nmiddleware) memcpy(chain + router->nmiddleware, r->middleware, r->nmiddleware * sizeof(*chain));
        char *path = join(app->error, prefix, r->path);
        if (!path) goto error;
        int rc = cp_route_register(pending, r->method, *path ? path : "/", r->handler, r->data, chain, router->nmiddleware + r->nmiddleware);
        free(path);
        if (rc) { fail(app->error, capi_app_error(pending)); goto error; }
    }
    cp_node old = app->root; app->root = pending->root; pending->root = old;
    capi_app_free(pending); return 0;
error:
    capi_app_free(pending); return -1;
}
