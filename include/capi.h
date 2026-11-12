#ifndef CAPI_H
#define CAPI_H

/* capi 0.2.0 — C11, no dependencies in the core. All lengths are bytes. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPI_VERSION "0.2.0"
#define CAPI_MAX_HEADERS 64
#define CAPI_MAX_PARAMS 16

typedef struct capi_app capi_app;
typedef struct capi_ctx capi_ctx;
typedef struct capi_server capi_server;
typedef struct { const char *data; size_t len; } capi_str;
typedef void (*capi_handler)(capi_ctx *ctx);
typedef enum {
    CAPI_GET, CAPI_POST, CAPI_PUT, CAPI_PATCH, CAPI_DELETE,
    CAPI_HEAD, CAPI_OPTIONS, CAPI_ANY
} capi_method;

typedef struct {
    size_t max_header_bytes;    /* 16 KiB, includes request line */
    size_t max_body_bytes;      /* 1 MiB */
    size_t max_response_bytes;  /* 8 MiB, response body */
} capi_limits;

capi_app *capi_app_new(void);
void capi_app_free(capi_app *app);
/* Configure before dispatch/listen. Registration returns 0 on success. */
int capi_app_limits(capi_app *app, capi_limits limits);
int capi_route(capi_app *app, capi_method method, const char *path,
               capi_handler handler, void *data);
/* Segment-aligned prefixes: /api matches /api and /api/users, not /apix. */
int capi_use(capi_app *app, const char *prefix, capi_handler middleware, void *data);
void capi_not_found(capi_app *app, capi_handler handler, void *data);
const char *capi_app_error(const capi_app *app);

/* Call once in middleware to continue. Code after next runs on the way out. */
void capi_next(capi_ctx *ctx);
void *capi_data(const capi_ctx *ctx); /* current handler's registration data */
void capi_set_state(capi_ctx *ctx, void *state);
void *capi_state(const capi_ctx *ctx); /* application-owned request state */
void *capi_alloc(capi_ctx *ctx, size_t bytes); /* max_align_t aligned, zeroed */
typedef void (*capi_cleanup_fn)(void *data);
/* LIFO cleanup after the pipeline, before response finalization and arena free.
 * Runs on short-circuits and failed responses too. Returns 0 or -1. */
int capi_defer(capi_ctx *ctx, capi_cleanup_fn cleanup, void *data);
void capi_fail(capi_ctx *ctx); /* substitute a clean 500 at finalization */

capi_str capi_request_method(const capi_ctx *ctx);
capi_str capi_path(const capi_ctx *ctx); /* raw, query excluded */
capi_str capi_body(const capi_ctx *ctx);
capi_str capi_header(const capi_ctx *ctx, const char *name); /* case insensitive */
capi_str capi_param(const capi_ctx *ctx, const char *name); /* raw path segment */
/* Query values are percent-decoded (+ becomes space), allocated in ctx.
 * Returns 1 found, 0 absent, -1 malformed encoding/allocation failure. */
int capi_query(capi_ctx *ctx, const char *name, capi_str *value);
/* Like query, for application/x-www-form-urlencoded bodies. Wrong content
 * type returns -1. Parameters such as charset are accepted. No multipart. */
int capi_form(capi_ctx *ctx, const char *name, capi_str *value);
bool capi_str_eq(capi_str value, const char *text);

void capi_status(capi_ctx *ctx, int status);
int capi_response_status(const capi_ctx *ctx);
/* Copies both strings; replaces an existing header. CR/LF are rejected.
 * Framing headers (Content-Length, Connection, Transfer-Encoding) are owned by capi. */
int capi_set_header(capi_ctx *ctx, const char *name, const char *value);
int capi_write(capi_ctx *ctx, const void *bytes, size_t len);
int capi_printf(capi_ctx *ctx, const char *format, ...);
int capi_text(capi_ctx *ctx, int status, const char *text);
int capi_html(capi_ctx *ctx, int status, const char *html);
int capi_json(capi_ctx *ctx, int status, const char *json);
/* Appends a quoted, escaped JSON string; validates UTF-8. */
int capi_json_string(capi_ctx *ctx, capi_str string);
int capi_redirect(capi_ctx *ctx, int status, const char *location);

/* Socket-free integration testing. Parses one complete HTTP request, invokes
 * the same pipeline as the server, and returns an owned wire response.
 * Returns 1 complete, 0 incomplete, -1 API/allocation failure.
 * HTTP errors are complete responses (return 1). */
typedef struct {
    char *data;
    size_t len;
    size_t consumed;
    bool close;
} capi_result;
int capi_dispatch(capi_app *app, const void *request, size_t len, capi_result *result);
void capi_result_free(capi_result *result);

typedef struct {
    const char *host;          /* NULL: 127.0.0.1. IPv4 or IPv6 */
    uint16_t port;             /* 0: select an available port */
    size_t max_connections;   /* 0: 4096 */
    uint32_t idle_timeout_ms;  /* 0: 15000 */
    uint32_t request_timeout_ms; /* 0: 30000, total receive deadline */
    uint32_t shutdown_timeout_ms; /* 0: 5000, drain pending responses */
} capi_server_options;

/* One event loop per server; handlers run synchronously on its thread.
 * Freeze app registration/configuration while a server uses it. */
capi_server *capi_server_listen(capi_app *app, capi_server_options options);
uint16_t capi_server_port(const capi_server *server);
int capi_server_run(capi_server *server);
/* Async-signal-safe; may also be called from a handler. */
void capi_server_stop(capi_server *server);
void capi_server_free(capi_server *server);
const char *capi_server_backend(void);

#ifdef __cplusplus
}
#endif
#endif
