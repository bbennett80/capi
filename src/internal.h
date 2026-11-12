#ifndef CAPI_INTERNAL_H
#define CAPI_INTERNAL_H
#include "capi.h"
#include <stdalign.h>
#include <stdarg.h>

typedef struct { char *data; size_t len, cap; } cp_buf;
typedef struct { capi_str name, value; } cp_header;
typedef struct cp_cleanup {
    capi_cleanup_fn fn;
    void *data;
    struct cp_cleanup *next;
} cp_cleanup;
typedef struct cp_arena {
    struct cp_arena *next;
    size_t used, cap;
    max_align_t alignment;
    unsigned char data[];
} cp_arena;
typedef struct cp_route {
    capi_handler handler;
    void *data;
    char *names[CAPI_MAX_PARAMS];
    size_t nparams;
} cp_route;
typedef struct cp_node {
    char *literal;
    struct cp_node *sibling, *children, *param, *wildcard;
    cp_route *routes[CAPI_ANY + 1];
} cp_node;
typedef struct {
    char *prefix;
    size_t len;
    capi_handler handler;
    void *data;
} cp_middleware;
struct capi_app {
    capi_limits limits;
    cp_node root;
    cp_middleware *middleware;
    size_t nmiddleware, middleware_cap;
    capi_handler not_found;
    void *not_found_data;
    char error[160];
};
struct capi_ctx {
    capi_app *app;
    capi_str method, target, path, query, body;
    cp_header headers[CAPI_MAX_HEADERS];
    size_t nheaders;
    capi_str captures[CAPI_MAX_PARAMS];
    cp_route *route;
    cp_node *matched;
    cp_header response_headers[CAPI_MAX_HEADERS];
    size_t nresponse_headers;
    cp_buf *output;
    cp_arena *arena;
    cp_cleanup *cleanup;
    void *data, *state;
    size_t next_middleware;
    unsigned depth;
    int status;
    bool failed, close, http10, terminal_called;
    bool *next_used;
};

int cp_buf_append(cp_buf *buf, const void *data, size_t len, size_t limit);
int cp_buf_printf(cp_buf *buf, size_t limit, const char *fmt, ...);
void cp_arena_free(cp_arena *arena);
int cp_caseeq(capi_str value, const char *str);
int cp_token(unsigned char c);
int cp_parse(capi_ctx *ctx, const char *data, size_t len, size_t *consumed);
int cp_process(capi_app *app, const char *data, size_t len, cp_buf *body,
               cp_buf *wire, size_t *consumed, bool *close);
uint64_t cp_now_ms(void);
#endif
