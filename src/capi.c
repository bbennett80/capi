#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <errno.h>

int cp_buf_append(cp_buf *b, const void *data, size_t len, size_t limit) {
    if (b->len > limit || len > limit - b->len || (len && !data)) return -1;
    size_t needed = b->len + len;
    if (needed > b->cap) {
        size_t cap = b->cap ? b->cap : 512;
        while (cap < needed) {
            if (cap > limit / 2) { cap = limit; break; }
            cap *= 2;
        }
        char *p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p;
        b->cap = cap;
    }
    if (len) memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

int cp_buf_printf(cp_buf *b, size_t limit, const char *fmt, ...) {
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0 || (size_t)n >= SIZE_MAX || b->len > limit || (size_t)n > limit - b->len) {
        va_end(ap); return -1;
    }
    char local[256], *p = local;
    if ((size_t)n >= sizeof(local)) p = malloc((size_t)n + 1);
    if (!p) { va_end(ap); return -1; }
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int result = cp_buf_append(b, p, (size_t)n, limit);
    if (p != local) free(p);
    return result;
}

int cp_caseeq(capi_str v, const char *s) {
    size_t len = strlen(s);
    if (len != v.len) return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char a = (unsigned char)v.data[i], b = (unsigned char)s[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

bool capi_str_eq(capi_str v, const char *s) {
    return s && v.len == strlen(s) && (!v.len || memcmp(v.data, s, v.len) == 0);
}

static char *copystr(const char *s, size_t len) {
    char *p = malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = 0; }
    return p;
}

capi_limits capi_limits_default(void) {
    return (capi_limits){16384, 1024 * 1024, 8 * 1024 * 1024, 1024 * 1024};
}

capi_app *capi_app_new(void) {
    capi_app *app = calloc(1, sizeof(*app));
    if (app) { app->limits = capi_limits_default(); atomic_init(&app->next_id, 0); }
    return app;
}

static void free_route(cp_route *r) {
    if (!r) return;
    for (size_t i = 0; i < r->nparams; ++i) free(r->names[i]);
    free(r->middleware);
    free(r);
}

static void free_node(cp_node *node, bool root) {
    if (!node) return;
    cp_node *child = node->children;
    while (child) { cp_node *next = child->sibling; free_node(child, false); child = next; }
    free_node(node->param, false);
    free_node(node->wildcard, false);
    for (size_t i = 0; i <= CAPI_ANY; ++i) free_route(node->routes[i]);
    free(node->literal);
    if (!root) free(node);
}

void capi_app_free(capi_app *app) {
    if (!app) return;
    free_node(&app->root, true);
    for (size_t i = 0; i < app->nmiddleware; ++i) free(app->middleware[i].prefix);
    free(app->middleware);
    free(app);
}

/* Setup-only copy used to make group inclusion transactional. */
static cp_route *clone_route(const cp_route *source) {
    cp_route *copy = calloc(1, sizeof(*copy));
    if (!copy) return NULL;
    copy->handler = source->handler; copy->data = source->data;
    copy->nparams = source->nparams;
    for (size_t i = 0; i < source->nparams; ++i) {
        copy->names[i] = copystr(source->names[i], strlen(source->names[i]));
        if (!copy->names[i]) goto fail;
    }
    if (source->nmiddleware) {
        copy->middleware = malloc(source->nmiddleware * sizeof(*copy->middleware));
        if (!copy->middleware) goto fail;
        memcpy(copy->middleware, source->middleware, source->nmiddleware * sizeof(*copy->middleware));
        copy->nmiddleware = source->nmiddleware;
    }
    return copy;
fail:
    free_route(copy); return NULL;
}
static int clone_node(cp_node *copy, const cp_node *source) {
    if (source->literal) {
        copy->literal = copystr(source->literal, strlen(source->literal));
        if (!copy->literal) return -1;
    }
    for (size_t i = 0; i <= CAPI_ANY; ++i) if (source->routes[i]) {
        copy->routes[i] = clone_route(source->routes[i]);
        if (!copy->routes[i]) return -1;
    }
    const cp_node *branches[] = {source->param, source->wildcard};
    cp_node **slots[] = {&copy->param, &copy->wildcard};
    for (size_t i = 0; i < 2; ++i) if (branches[i]) {
        *slots[i] = calloc(1, sizeof(**slots[i]));
        if (!*slots[i] || clone_node(*slots[i], branches[i])) return -1;
    }
    cp_node **slot = &copy->children;
    for (const cp_node *child = source->children; child; child = child->sibling) {
        *slot = calloc(1, sizeof(**slot));
        if (!*slot || clone_node(*slot, child)) return -1;
        slot = &(*slot)->sibling;
    }
    return 0;
}
capi_app *cp_clone_app_routes(const capi_app *app) {
    capi_app *copy = capi_app_new();
    if (copy && clone_node(&copy->root, &app->root)) { capi_app_free(copy); return NULL; }
    return copy;
}

static int app_error(capi_app *app, const char *message) {
    if (app) snprintf(app->error, sizeof(app->error), "%s", message);
    return -1;
}

const char *capi_app_error(const capi_app *app) { return app ? app->error : "null app"; }

int capi_app_limits(capi_app *app, capi_limits limits) {
    if (!app) return -1;
    if (!limits.max_arena_bytes) limits.max_arena_bytes = capi_limits_default().max_arena_bytes;
    if (limits.max_header_bytes < 128 || limits.max_header_bytes > 1024 * 1024 ||
        limits.max_body_bytes > SIZE_MAX - limits.max_header_bytes ||
        !limits.max_response_bytes || limits.max_response_bytes > SIZE_MAX - limits.max_header_bytes - 1024)
        return app_error(app, "invalid limits");
    app->limits = limits;
    return 0;
}

int capi_route(capi_app *app, capi_method method, const char *path, capi_handler handler, void *data) {
    return cp_route_register(app, method, path, handler, data, NULL, 0);
}
int cp_route_register(capi_app *app, capi_method method, const char *path, capi_handler handler,
                      void *data, const cp_binding *middleware, size_t nmiddleware) {
    if (!app) return -1;
    if (!path || path[0] != '/' || !handler || method < CAPI_GET || method > CAPI_ANY)
        return app_error(app, "route requires a method, absolute path, and handler");
    if (strlen(path) > 4096) return app_error(app, "route is too long");
    cp_route *route = calloc(1, sizeof(*route));
    if (!route) return app_error(app, "out of memory");
    route->handler = handler;
    route->data = data;
    if (nmiddleware) {
        route->middleware = malloc(nmiddleware * sizeof(*route->middleware));
        if (!route->middleware) goto nomem;
        memcpy(route->middleware, middleware, nmiddleware * sizeof(*route->middleware));
        route->nmiddleware = nmiddleware;
    }
    /* Validate the entire pattern before mutating the trie. */
    size_t segments = 0;
    for (const char *p = path + 1; *p;) {
        const char *end = strchr(p, '/');
        if (!end) end = p + strlen(p);
        size_t len = (size_t)(end - p);
        if (++segments > 128 || !len) goto invalid;
        for (size_t i = 0; i < len; ++i)
            if ((unsigned char)p[i] <= 32 || (unsigned char)p[i] == 127 || p[i] == '?' || p[i] == '#') goto invalid;
        if (*p == ':' || *p == '*') {
            if (len < 2 || route->nparams == CAPI_MAX_PARAMS || (*p == '*' && *end)) goto invalid;
            for (size_t i = 1; i < len; ++i)
                if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') ||
                      (p[i] >= '0' && p[i] <= '9') || p[i] == '_')) goto invalid;
            for (size_t i = 0; i < route->nparams; ++i)
                if (strlen(route->names[i]) == len - 1 && memcmp(route->names[i], p + 1, len - 1) == 0) goto invalid;
            char *name = copystr(p + 1, len - 1);
            if (!name) goto nomem;
            route->names[route->nparams++] = name;
        }
        p = *end ? end + 1 : end;
    }
    cp_node *node = &app->root;
    const char *p = path + 1;
    while (*p) {
        const char *end = strchr(p, '/');
        if (!end) end = p + strlen(p);
        size_t len = (size_t)(end - p);
        cp_node **slot;
        if (*p == ':') slot = &node->param;
        else if (*p == '*') slot = &node->wildcard;
        else {
            slot = &node->children;
            while (*slot && !(strlen((*slot)->literal) == len && memcmp((*slot)->literal, p, len) == 0))
                slot = &(*slot)->sibling;
        }
        if (!*slot) {
            *slot = calloc(1, sizeof(**slot));
            if (!*slot) goto nomem;
            if (*p != ':' && *p != '*') {
                (*slot)->literal = copystr(p, len);
                if (!(*slot)->literal) { free(*slot); *slot = NULL; goto nomem; }
            }
        }
        node = *slot;
        p = *end ? end + 1 : end;
    }
    /* /a and /a/ are distinct; represent a trailing slash as an empty literal. */
    if (path[1] && path[strlen(path) - 1] == '/') {
        cp_node **slot = &node->children;
        while (*slot && (*slot)->literal[0]) slot = &(*slot)->sibling;
        if (!*slot) {
            *slot = calloc(1, sizeof(**slot));
            if (!*slot) goto nomem;
            (*slot)->literal = copystr("", 0);
            if (!(*slot)->literal) { free(*slot); *slot = NULL; goto nomem; }
        }
        node = *slot;
    }
    if (node->routes[method]) { free_route(route); return app_error(app, "duplicate route shape and method"); }
    node->routes[method] = route;
    return 0;
invalid:
    free_route(route); return app_error(app, "invalid route pattern (use /path/:name or /path/*rest)");
nomem:
    free_route(route); return app_error(app, "out of memory");
}

int capi_use(capi_app *app, const char *prefix, capi_handler handler, void *data) {
    if (!app) return -1;
    if (!prefix || prefix[0] != '/' || !handler || app->nmiddleware >= 128)
        return app_error(app, "invalid middleware (maximum 128)");
    size_t len = strlen(prefix);
    while (len > 1 && prefix[len - 1] == '/') --len;
    char *copy = copystr(prefix, len);
    if (!copy) return app_error(app, "out of memory");
    if (app->nmiddleware == app->middleware_cap) {
        size_t capacity = app->middleware_cap ? app->middleware_cap * 2 : 8;
        cp_middleware *m = realloc(app->middleware, capacity * sizeof(*m));
        if (!m) { free(copy); return app_error(app, "out of memory"); }
        app->middleware = m;
        app->middleware_cap = capacity;
    }
    app->middleware[app->nmiddleware++] = (cp_middleware){copy, len, handler, data};
    return 0;
}

void capi_not_found(capi_app *app, capi_handler handler, void *data) {
    if (app) { app->not_found = handler; app->not_found_data = data; }
}

void *capi_alloc(capi_ctx *ctx, size_t bytes) {
    if (!ctx || !bytes) return NULL;
    size_t alignment = alignof(max_align_t);
    if (bytes > SIZE_MAX - alignment) { cp_fail(ctx, CAPI_ERROR_MEMORY_LIMIT, "capi_alloc", 0); return NULL; }
    size_t size = (bytes + alignment - 1) / alignment * alignment;
    cp_arena *a = ctx->arena;
    if (!a || size > a->cap - a->used) {
        size_t remaining = ctx->app->limits.max_arena_bytes - ctx->arena_reserved;
        if (remaining <= sizeof(*a) || size > remaining - sizeof(*a)) {
            cp_fail(ctx, CAPI_ERROR_MEMORY_LIMIT, "capi_alloc", 0); return NULL;
        }
        size_t capacity = size < 2048 ? 2048 : size;
        if (capacity > remaining - sizeof(*a)) capacity = remaining - sizeof(*a);
        capacity -= capacity % alignment;
        a = malloc(sizeof(*a) + capacity);
        if (!a) { cp_fail(ctx, CAPI_ERROR_OUT_OF_MEMORY, "capi_alloc", ENOMEM); return NULL; }
        a->next = ctx->arena;
        a->used = 0;
        a->cap = capacity;
        ctx->arena = a;
        ctx->arena_reserved += sizeof(*a) + capacity;
        ctx->arena_peak = ctx->arena_reserved;
    }
    void *p = a->data + a->used;
    a->used += size;
    memset(p, 0, bytes);
    return p;
}

void cp_arena_free(cp_arena *a) {
    while (a) { cp_arena *next = a->next; free(a); a = next; }
}

int capi_defer(capi_ctx *ctx, capi_cleanup_fn cleanup, void *data) {
    if (!ctx || !cleanup) { cp_fail(ctx, CAPI_ERROR_ARGUMENT, "capi_defer", 0); return -1; }
    cp_cleanup *entry = capi_alloc(ctx, sizeof(*entry));
    if (!entry) return -1;
    *entry = (cp_cleanup){cleanup, data, ctx->cleanup};
    ctx->cleanup = entry;
    return 0;
}

void *capi_data(const capi_ctx *ctx) { return ctx->data; }
void *capi_state(const capi_ctx *ctx) { return ctx->state; }
void capi_set_state(capi_ctx *ctx, void *state) { ctx->state = state; }
capi_str capi_request_method(const capi_ctx *ctx) { return ctx->method; }
capi_str capi_path(const capi_ctx *ctx) { return ctx->path; }
capi_str capi_body(const capi_ctx *ctx) { return ctx->body; }
capi_str capi_header(const capi_ctx *ctx, const char *name) {
    for (size_t i = 0; i < ctx->nheaders; ++i)
        if (cp_caseeq(ctx->headers[i].name, name)) return ctx->headers[i].value;
    return (capi_str){NULL, 0};
}
capi_str capi_param(const capi_ctx *ctx, const char *name) {
    if (ctx->route) for (size_t i = 0; i < ctx->route->nparams; ++i)
        if (strcmp(ctx->route->names[i], name) == 0) return ctx->captures[i];
    return (capi_str){NULL, 0};
}

void capi_status(capi_ctx *ctx, int status) {
    if (status < 200 || status > 599) cp_fail(ctx, CAPI_ERROR_STATUS, "capi_status", 0);
    else ctx->status = status;
}
int capi_response_status(const capi_ctx *ctx) { return ctx->status; }

int capi_set_header(capi_ctx *ctx, const char *name, const char *value) {
    if (ctx->failed) return -1;
    capi_error_code error = CAPI_ERROR_HEADER;
    if (!name || !*name || !value) { error = CAPI_ERROR_ARGUMENT; goto fail; }
    for (const char *p = name; *p; ++p) if (!cp_token((unsigned char)*p)) goto fail;
    for (const char *p = value; *p; ++p)
        if (((unsigned char)*p < 32 && *p != '\t') || (unsigned char)*p == 127) goto fail;
    capi_str n = {name, strlen(name)};
    if (cp_caseeq(n, "content-length") || cp_caseeq(n, "connection") ||
        cp_caseeq(n, "transfer-encoding") || cp_caseeq(n, "trailer") || cp_caseeq(n, "upgrade")) {
        error = CAPI_ERROR_RESERVED_HEADER; goto fail;
    }
    size_t slot = ctx->nresponse_headers;
    for (size_t i = 0; i < ctx->nresponse_headers; ++i)
        if (cp_caseeq(ctx->response_headers[i].name, name)) { slot = i; break; }
    error = CAPI_ERROR_HEADER_LIMIT;
    if (slot == CAPI_MAX_HEADERS) goto fail;
    size_t nl = strlen(name), vl = strlen(value), limit = ctx->app->limits.max_header_bytes;
    if (nl > limit - 4 || vl > limit - 4 - nl) goto fail;
    size_t total = nl + vl + 4;
    for (size_t i = 0; i < ctx->nresponse_headers; ++i)
        if (i != slot) total += ctx->response_headers[i].name.len + ctx->response_headers[i].value.len + 4;
    if (total > limit) goto fail;
    char *nc = capi_alloc(ctx, nl + 1), *vc = capi_alloc(ctx, vl + 1);
    if (!nc || !vc) goto fail;
    memcpy(nc, name, nl + 1); memcpy(vc, value, vl + 1);
    ctx->response_headers[slot] = (cp_header){{nc, nl}, {vc, vl}};
    if (slot == ctx->nresponse_headers) ++ctx->nresponse_headers;
    return 0;
fail:
    cp_fail(ctx, error, "capi_set_header", 0); return -1;
}

int capi_write(capi_ctx *ctx, const void *bytes, size_t len) {
    if (ctx->failed) return -1;
    if (len && !bytes) { cp_fail(ctx, CAPI_ERROR_ARGUMENT, "capi_write", 0); return -1; }
    if (len > ctx->app->limits.max_response_bytes - ctx->output->len) {
        cp_fail(ctx, CAPI_ERROR_RESPONSE_LIMIT, "capi_write", 0); return -1;
    }
    if (cp_buf_append(ctx->output, bytes, len, ctx->app->limits.max_response_bytes)) {
        cp_fail(ctx, CAPI_ERROR_OUT_OF_MEMORY, "capi_write", ENOMEM); return -1;
    }
    return 0;
}

int capi_printf(capi_ctx *ctx, const char *format, ...) {
    va_list ap, copy;
    if (ctx->failed) return -1;
    if (!format) { cp_fail(ctx, CAPI_ERROR_ARGUMENT, "capi_printf", 0); return -1; }
    va_start(ap, format); va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (n < 0) { va_end(ap); cp_fail(ctx, CAPI_ERROR_FORMAT, "capi_printf", 0); return -1; }
    if ((size_t)n > ctx->app->limits.max_response_bytes - ctx->output->len) {
        va_end(ap); cp_fail(ctx, CAPI_ERROR_RESPONSE_LIMIT, "capi_printf", 0); return -1;
    }
    char local[512], *p = local;
    if ((size_t)n >= sizeof(local)) p = capi_alloc(ctx, (size_t)n + 1);
    if (!p) { va_end(ap); return -1; }
    vsnprintf(p, (size_t)n + 1, format, ap);
    va_end(ap);
    return capi_write(ctx, p, (size_t)n);
}

static int typed(capi_ctx *ctx, int status, const char *type, const char *body) {
    capi_status(ctx, status);
    if (capi_set_header(ctx, "Content-Type", type)) return -1;
    ctx->output->len = 0;
    return capi_write(ctx, body, body ? strlen(body) : 0);
}
int capi_text(capi_ctx *ctx, int status, const char *text) { return typed(ctx, status, "text/plain; charset=utf-8", text); }
int capi_html(capi_ctx *ctx, int status, const char *html) { return typed(ctx, status, "text/html; charset=utf-8", html); }
int capi_json(capi_ctx *ctx, int status, const char *json) { return typed(ctx, status, "application/json; charset=utf-8", json); }
int capi_redirect(capi_ctx *ctx, int status, const char *location) {
    if (status < 300 || status > 399 || status == 304) { cp_fail(ctx, CAPI_ERROR_STATUS, "capi_redirect", 0); return -1; }
    capi_status(ctx, status);
    ctx->output->len = 0;
    return capi_set_header(ctx, "Location", location);
}

static bool terminal(cp_node *node) {
    for (size_t i = 0; i <= CAPI_ANY; ++i) if (node->routes[i]) return true;
    return false;
}

/* Literal > parameter > wildcard, independent of registration order. */
static cp_node *match(cp_node *node, const char *p, size_t len, bool segment,
                      capi_str captures[CAPI_MAX_PARAMS], size_t n) {
    if (!segment) return terminal(node) ? node : NULL;
    size_t end = 0;
    while (end < len && p[end] != '/') ++end;
    bool more = end < len;
    const char *rest = more ? p + end + 1 : p + end;
    size_t remaining = more ? len - end - 1 : 0;
    cp_node *found;
    for (cp_node *child = node->children; child; child = child->sibling) {
        if (strlen(child->literal) != end || memcmp(child->literal, p, end)) continue;
        found = match(child, rest, remaining, more, captures, n);
        if (found) return found;
    }
    if (node->param && end && n < CAPI_MAX_PARAMS) {
        captures[n] = (capi_str){p, end};
        found = match(node->param, rest, remaining, more, captures, n + 1);
        if (found) return found;
    }
    if (node->wildcard && n < CAPI_MAX_PARAMS && terminal(node->wildcard)) {
        captures[n] = (capi_str){p, len};
        return node->wildcard;
    }
    return NULL;
}

static const char *methods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
static int method_index(capi_str method) {
    for (int i = CAPI_GET; i < CAPI_ANY; ++i) if (capi_str_eq(method, methods[i])) return i;
    return CAPI_ANY;
}

static void invoke(capi_ctx *ctx, capi_handler handler, void *data) {
    void *saved = ctx->data;
    bool used = false, *previous = ctx->next_used;
    ctx->next_used = &used;
    ctx->data = data;
    handler(ctx);
    ctx->data = saved;
    ctx->next_used = previous;
}

static void advance(capi_ctx *ctx) {
    while (ctx->next_middleware < ctx->app->nmiddleware) {
        cp_middleware *m = &ctx->app->middleware[ctx->next_middleware++];
        if (m->len != 1 && !(ctx->path.len >= m->len && memcmp(ctx->path.data, m->prefix, m->len) == 0 &&
            (ctx->path.len == m->len || ctx->path.data[m->len] == '/'))) continue;
        invoke(ctx, m->handler, m->data);
        return;
    }
    if (ctx->terminal_called) return;
    if (ctx->route && ctx->next_route_middleware < ctx->route->nmiddleware) {
        cp_binding *m = &ctx->route->middleware[ctx->next_route_middleware++];
        invoke(ctx, m->handler, m->data);
        return;
    }
    ctx->terminal_called = true;
    if (ctx->route) { invoke(ctx, ctx->route->handler, ctx->route->data); return; }
    if (ctx->matched) {
        char allow[80] = "";
        for (int i = CAPI_GET; i < CAPI_ANY; ++i) {
            if (ctx->matched->routes[i] || (i == CAPI_HEAD && ctx->matched->routes[CAPI_GET]) || i == CAPI_OPTIONS) {
                if (*allow) strcat(allow, ", ");
                strcat(allow, methods[i]);
            }
        }
        capi_set_header(ctx, "Allow", allow);
        if (capi_str_eq(ctx->method, "OPTIONS")) capi_status(ctx, 204);
        else capi_text(ctx, 405, "Method Not Allowed\n");
    } else if (ctx->app->not_found) invoke(ctx, ctx->app->not_found, ctx->app->not_found_data);
    else capi_text(ctx, 404, "Not Found\n");
}

void capi_next(capi_ctx *ctx) {
    if (!ctx->next_used || *ctx->next_used) return;
    *ctx->next_used = true;
    advance(ctx);
}

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted";
    case 204: return "No Content"; case 205: return "Reset Content"; case 206: return "Partial Content";
    case 301: return "Moved Permanently"; case 302: return "Found"; case 303: return "See Other";
    case 304: return "Not Modified"; case 307: return "Temporary Redirect"; case 308: return "Permanent Redirect";
    case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden";
    case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 408: return "Request Timeout";
    case 409: return "Conflict"; case 410: return "Gone"; case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type"; case 417: return "Expectation Failed";
    case 422: return "Unprocessable Content"; case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large"; case 500: return "Internal Server Error";
    case 501: return "Not Implemented"; case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported"; default: return "Response";
    }
}

static int serialize(capi_ctx *ctx, cp_buf *wire) {
    size_t limit = ctx->app->limits.max_response_bytes + ctx->app->limits.max_header_bytes + 1024;
    bool nobody = ctx->status == 204 || ctx->status == 304 || ctx->status == 205;
    size_t length = nobody ? 0 : ctx->output->len;
    if (cp_buf_printf(wire, limit, "HTTP/1.%d %d %s\r\n", ctx->http10 ? 0 : 1, ctx->status, reason(ctx->status))) return -1;
    bool has_date = false;
    for (size_t i = 0; i < ctx->nresponse_headers; ++i) {
        cp_header h = ctx->response_headers[i];
        has_date |= cp_caseeq(h.name, "date") != 0;
        if (cp_buf_printf(wire, limit, "%.*s: %.*s\r\n", (int)h.name.len, h.name.data, (int)h.value.len, h.value.data)) return -1;
    }
    if (!has_date) {
        char date[96]; struct tm utc;
        time_t now = time(NULL);
        /* Numeric names keep the wire format independent of process locale. */
        static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        if (gmtime_r(&now, &utc)) {
            snprintf(date, sizeof(date), "%s, %02d %s %04d %02d:%02d:%02d GMT", days[utc.tm_wday], utc.tm_mday,
                     months[utc.tm_mon], utc.tm_year + 1900, utc.tm_hour, utc.tm_min, utc.tm_sec);
            if (cp_buf_printf(wire, limit, "Date: %s\r\n", date)) return -1;
        }
    }
    if (ctx->status != 204 && ctx->status != 304 && cp_buf_printf(wire, limit, "Content-Length: %zu\r\n", length)) return -1;
    if (cp_buf_printf(wire, limit, "Connection: %s\r\n\r\n", ctx->close ? "close" : "keep-alive")) return -1;
    if (!nobody && !capi_str_eq(ctx->method, "HEAD")) return cp_buf_append(wire, ctx->output->data, length, limit);
    return 0;
}

int cp_request_begin(capi_ctx *request, capi_app *app, const char *data, size_t len,
                     cp_buf *body, size_t *consumed, capi_server *server, void *connection, uint64_t started_ms) {
    *request = (capi_ctx){0};
    capi_ctx *ctx = request;
    ctx->app = app; ctx->output = body; ctx->status = 200;
    ctx->server = server; ctx->connection = connection;
    ctx->active = true;
    ctx->started_ms = started_ms ? started_ms : cp_now_ms();
    body->len = 0;
    int parsed = cp_parse(ctx, data, len, consumed);
    if (!parsed) return 0;
    cp_request_identify(ctx);
    if (parsed != 1) {
        ctx->close = true;
        *consumed = len;
        capi_status(ctx, parsed);
        capi_printf(ctx, "%s\n", reason(parsed));
    } else {
        ctx->matched = match(&app->root, ctx->path.data + 1, ctx->path.len - 1, ctx->path.len > 1, ctx->captures, 0);
        if (!ctx->matched && ctx->path.len == 1 && app->root.wildcard && terminal(app->root.wildcard)) {
            ctx->matched = app->root.wildcard;
            ctx->captures[0] = (capi_str){ctx->path.data + 1, 0};
        }
        if (ctx->matched) {
            int method = method_index(ctx->method);
            ctx->route = method < CAPI_ANY ? ctx->matched->routes[method] : NULL;
            if (!ctx->route && method == CAPI_HEAD) ctx->route = ctx->matched->routes[CAPI_GET];
            if (!ctx->route) ctx->route = ctx->matched->routes[CAPI_ANY];
        }
        ctx->async_allowed = true;
        advance(ctx);
        ctx->async_allowed = false;
    }
    return 1;
}

static void run_cleanup(capi_ctx *ctx) {
    while (ctx->cleanup) {
        cp_cleanup *entry = ctx->cleanup;
        ctx->cleanup = entry->next;
        entry->fn(entry->data);
    }
}

void cp_request_dispose(capi_ctx *ctx) {
    if (!ctx) return;
    run_cleanup(ctx);
    cp_arena_free(ctx->arena);
    ctx->arena = NULL;
    ctx->arena_reserved = 0;
}

int cp_request_finish(capi_ctx *ctx, cp_buf *wire, bool *close) {
    wire->len = 0;
    run_cleanup(ctx);
    if (ctx->failed) {
        ctx->output->len = 0; ctx->nresponse_headers = 0; ctx->status = 500; ctx->close = true;
        if (cp_buf_append(ctx->output, "Internal Server Error\n", 22, SIZE_MAX)) {
            cp_fail(ctx, CAPI_ERROR_OUT_OF_MEMORY, "serialize_error", ENOMEM); cp_request_dispose(ctx); return -1;
        }
    }
    *close = ctx->close;
    int result = serialize(ctx, wire);
    if (result) cp_fail(ctx, CAPI_ERROR_OUT_OF_MEMORY, "serialize_response", ENOMEM);
    else ctx->response_ready = true;
    cp_request_dispose(ctx);
    return result ? -1 : 1;
}

int cp_process(capi_app *app, const char *data, size_t len, cp_buf *body, cp_buf *wire, size_t *consumed, bool *close) {
    capi_ctx ctx;
    int parsed = cp_request_begin(&ctx, app, data, len, body, consumed, NULL, NULL, 0);
    if (!parsed) return 0;
    int result = cp_request_finish(&ctx, wire, close);
    cp_request_observe(&ctx, result < 0 ? CAPI_OUTCOME_ERROR : CAPI_OUTCOME_DISPATCHED, 0, wire->len);
    return result;
}

int capi_dispatch(capi_app *app, const void *request, size_t len, capi_result *result) {
    if (!app || !request || !result) return -1;
    *result = (capi_result){0};
    cp_buf body = {0}, wire = {0};
    int status = cp_process(app, request, len, &body, &wire, &result->consumed, &result->close);
    free(body.data);
    if (status == 1) { result->data = wire.data; result->len = wire.len; }
    else free(wire.data);
    return status;
}

void capi_result_free(capi_result *result) {
    if (result) { free(result->data); *result = (capi_result){0}; }
}
