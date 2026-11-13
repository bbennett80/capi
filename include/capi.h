#ifndef CAPI_H
#define CAPI_H

/* capi 0.4.0 — C11, no dependencies in the core. All lengths are bytes. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPI_VERSION "0.4.0"
#define CAPI_MAX_HEADERS 64
#define CAPI_MAX_PARAMS 16

#if defined(__GNUC__) || defined(__clang__)
#define CAPI_NODISCARD __attribute__((warn_unused_result))
#define CAPI_PRINTF(format_index, first_argument) __attribute__((format(printf, format_index, first_argument)))
#else
#define CAPI_NODISCARD
#define CAPI_PRINTF(format_index, first_argument)
#endif

typedef struct capi_app capi_app;
typedef struct capi_router capi_router;
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
    size_t max_arena_bytes;     /* 0: 1 MiB; includes arena block overhead */
} capi_limits;

capi_limits capi_limits_default(void);
CAPI_NODISCARD capi_app *capi_app_new(void);
void capi_app_free(capi_app *app);
/* Configure before dispatch/listen. Registration returns 0 on success. */
CAPI_NODISCARD int capi_app_limits(capi_app *app, capi_limits limits);
CAPI_NODISCARD int capi_route(capi_app *app, capi_method method, const char *path,
               capi_handler handler, void *data);
/* Segment-aligned prefixes: /api matches /api and /api/users, not /apix. */
CAPI_NODISCARD int capi_use(capi_app *app, const char *prefix, capi_handler middleware, void *data);
void capi_not_found(capi_app *app, capi_handler handler, void *data);
const char *capi_app_error(const capi_app *app);

/* Reusable route groups. Configure before dispatch/listen; 0 succeeds, -1 fails.
 * Includes copy a snapshot, including nested routes and middleware. Builders
 * may then be changed/freed. Handler data is borrowed and must outlive requests.
 * Inclusion is atomic: conflicts, bad paths, or allocation failures add nothing. */
CAPI_NODISCARD capi_router *capi_router_new(void);
void capi_router_free(capi_router *router);
const char *capi_router_error(const capi_router *router);
/* Empty path matches the mount prefix itself; "/" adds a trailing slash. */
CAPI_NODISCARD int capi_router_route(capi_router *router, capi_method method, const char *path,
                                   capi_handler handler, void *data);
/* Shared middleware for this group's matched endpoints, in registration order.
 * App middleware runs first, then outer router groups, then inner groups.
 * Does not run for unrelated routes, 404s, or automatic 405/OPTIONS responses. */
CAPI_NODISCARD int capi_router_use(capi_router *router, capi_handler middleware, void *data);
/* Prefix: "" or "/" means root; otherwise absolute, without trailing slash
 * or catch-all. :parameters are supported. capi_path remains the full path. */
CAPI_NODISCARD int capi_router_include(capi_router *parent, const char *prefix, const capi_router *child);
CAPI_NODISCARD int capi_include_router(capi_app *app, const char *prefix, const capi_router *router);

typedef enum {
    CAPI_ERROR_NONE, CAPI_ERROR_APPLICATION, CAPI_ERROR_ARGUMENT,
    CAPI_ERROR_STATUS, CAPI_ERROR_HEADER, CAPI_ERROR_RESERVED_HEADER,
    CAPI_ERROR_HEADER_LIMIT, CAPI_ERROR_RESPONSE_LIMIT, CAPI_ERROR_MEMORY_LIMIT,
    CAPI_ERROR_OUT_OF_MEMORY, CAPI_ERROR_FORMAT, CAPI_ERROR_UTF8,
    CAPI_ERROR_TEMPLATE, CAPI_ERROR_DATABASE, CAPI_ERROR_IO
} capi_error_code;
typedef struct {
    capi_error_code code;
    const char *operation; /* borrowed, bounded operation name; no request data */
    const char *message;   /* static description */
    int system_error;      /* errno for OS failures, otherwise 0 */
} capi_error;
const char *capi_error_name(capi_error_code code);
const char *capi_error_message(capi_error_code code);
capi_error capi_request_error(const capi_ctx *ctx); /* first error wins */
uint64_t capi_request_id(const capi_ctx *ctx); /* monotonic per app */
typedef void (*capi_error_fn)(uint64_t request_id, const capi_error *error, void *data);
void capi_on_error(capi_app *app, capi_error_fn handler, void *data);

typedef enum {
    CAPI_OUTCOME_SENT, CAPI_OUTCOME_DISPATCHED, CAPI_OUTCOME_CLIENT_CLOSED,
    CAPI_OUTCOME_TIMEOUT, CAPI_OUTCOME_SHUTDOWN, CAPI_OUTCOME_ERROR
} capi_outcome;
typedef struct {
    uint64_t id;
    capi_str method, path;
    int status; /* serialized response status; 0 if none was prepared */
    capi_outcome outcome;
    uint64_t duration_ms; /* time as current request through transport completion */
    size_t bytes_sent; /* wire bytes accepted by socket; 0 for dispatch */
    size_t response_bytes; /* serialized wire size, including headers */
    size_t arena_bytes; /* peak reserved bytes at report time, including overhead */
    capi_error error;
} capi_request_report;
typedef void (*capi_finish_fn)(const capi_request_report *report, void *data);
/* Exactly once per attempted request, including incomplete requests on close.
 * No event for an empty idle connection. Views are borrowed for the callback.
 * Callbacks run on the loop (or dispatch caller), must be short, and must not
 * reconfigure/free/reenter the active app/server. Stopping the server is allowed.
 * Cancellation may precede resource cleanup. */
void capi_on_finish(capi_app *app, capi_finish_fn handler, void *data);
const char *capi_outcome_name(capi_outcome outcome);

/* Call once in middleware to continue. Code after next runs on the way out. */
void capi_next(capi_ctx *ctx);
void *capi_data(const capi_ctx *ctx); /* current handler's registration data */
void capi_set_state(capi_ctx *ctx, void *state);
void *capi_state(const capi_ctx *ctx); /* application-owned request state */
void *capi_alloc(capi_ctx *ctx, size_t bytes); /* max_align_t aligned, zeroed */
typedef void (*capi_cleanup_fn)(void *data);
/* LIFO cleanup after the pipeline and any async completion, before response
 * finalization and arena free.
 * Runs on short-circuits and failed responses too. Returns 0 or -1. */
CAPI_NODISCARD int capi_defer(capi_ctx *ctx, capi_cleanup_fn cleanup, void *data);
void capi_fail(capi_ctx *ctx); /* substitute a clean 500 at finalization */
void capi_fail_with(capi_ctx *ctx, capi_error_code code, const char *operation);

capi_str capi_request_method(const capi_ctx *ctx);
capi_str capi_path(const capi_ctx *ctx); /* raw, query excluded */
capi_str capi_body(const capi_ctx *ctx);
capi_str capi_header(const capi_ctx *ctx, const char *name); /* case insensitive */
capi_str capi_param(const capi_ctx *ctx, const char *name); /* raw path segment */
typedef enum {
    CAPI_INPUT_FOUND = 1, CAPI_INPUT_MISSING = 0,
    CAPI_INPUT_ENCODING = -1, CAPI_INPUT_CONTENT_TYPE = -2,
    CAPI_INPUT_RANGE = -3, CAPI_INPUT_NO_MEMORY = -4,
    CAPI_INPUT_VALUE = -5, CAPI_INPUT_ARGUMENT = -6
} capi_input_result;
/* Percent-decoded (+ becomes space), allocated in ctx. First value wins.
 * Every failure is negative; malformed input alone does not fail the response. */
CAPI_NODISCARD capi_input_result capi_query(capi_ctx *ctx, const char *name, capi_str *value);
/* URL-encoded bodies only; charset parameters accepted. No multipart. */
CAPI_NODISCARD capi_input_result capi_form(capi_ctx *ctx, const char *name, capi_str *value);
/* Strict decimal integer, optional sign, no whitespace/hex/trailing bytes.
 * Inclusive bounds. Every non-FOUND result zeroes *value when non-NULL. */
CAPI_NODISCARD capi_input_result capi_str_i64(capi_str input, int64_t minimum, int64_t maximum, int64_t *value);
CAPI_NODISCARD capi_input_result capi_param_i64(capi_ctx *ctx, const char *name, int64_t minimum, int64_t maximum, int64_t *value);
CAPI_NODISCARD capi_input_result capi_query_i64(capi_ctx *ctx, const char *name, int64_t minimum, int64_t maximum, int64_t *value);
CAPI_NODISCARD capi_input_result capi_form_i64(capi_ctx *ctx, const char *name, int64_t minimum, int64_t maximum, int64_t *value);
const char *capi_input_name(capi_input_result result);
bool capi_str_eq(capi_str value, const char *text);

void capi_status(capi_ctx *ctx, int status);
int capi_response_status(const capi_ctx *ctx);
/* Copies both strings; replaces an existing header. CR/LF are rejected.
 * Framing headers (Content-Length, Connection, Transfer-Encoding) are owned by capi. */
int capi_set_header(capi_ctx *ctx, const char *name, const char *value);
int capi_write(capi_ctx *ctx, const void *bytes, size_t len);
int capi_printf(capi_ctx *ctx, const char *format, ...) CAPI_PRINTF(2, 3);
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
CAPI_NODISCARD int capi_dispatch(capi_app *app, const void *request, size_t len, capi_result *result);
void capi_result_free(capi_result *result);

typedef struct {
    const char *host;          /* NULL: 127.0.0.1. IPv4 or IPv6 */
    uint16_t port;             /* 0: select an available port */
    size_t max_connections;   /* 0: 4096 */
    uint32_t idle_timeout_ms;  /* 0: 15000 */
    uint32_t request_timeout_ms; /* 0: 30000, total receive deadline */
    uint32_t shutdown_timeout_ms; /* 0: 5000, drain pending responses */
    uint32_t async_timeout_ms; /* 0: 30000, total asynchronous request deadline */
} capi_server_options;

/* One event loop per server; handlers and async completions run on its thread.
 * Freeze app registration/configuration while a server uses it. */
CAPI_NODISCARD capi_server *capi_server_listen(capi_app *app, capi_server_options options);
uint16_t capi_server_port(const capi_server *server);
CAPI_NODISCARD int capi_server_run(capi_server *server);
/* Async-signal-safe and thread-safe while server is alive. May be called from
 * a handler. Stop cancels async requests; run still waits for their producers
 * to complete before returning. C work cannot be forcibly interrupted. */
void capi_server_stop(capi_server *server);
void capi_server_free(capi_server *server);
const char *capi_server_backend(void);

#ifdef __cplusplus
}
#endif
#endif
