#include "internal.h"
#include <string.h>
#include <errno.h>

static const char *error_names[] = {
    "none", "application", "argument", "status", "header", "reserved_header",
    "header_limit", "response_limit", "memory_limit", "out_of_memory", "format",
    "utf8", "template", "database", "io"
};
static const char *error_messages[] = {
    "no error", "application marked the request failed", "invalid API argument",
    "invalid response status", "invalid response header", "reserved response header",
    "response header limit exceeded", "response body limit exceeded",
    "request arena memory limit exceeded", "memory allocation failed",
    "response formatting failed", "invalid UTF-8 in JSON string",
    "template rendering failed", "database operation failed", "operating system I/O failed"
};
const char *capi_error_name(capi_error_code code) {
    return code >= CAPI_ERROR_NONE && code <= CAPI_ERROR_IO ? error_names[code] : "unknown";
}
const char *capi_error_message(capi_error_code code) {
    return code >= CAPI_ERROR_NONE && code <= CAPI_ERROR_IO ? error_messages[code] : "unknown error";
}
const char *capi_outcome_name(capi_outcome outcome) {
    static const char *names[] = {"sent", "dispatched", "client_closed", "timeout", "shutdown", "error"};
    return outcome >= CAPI_OUTCOME_SENT && outcome <= CAPI_OUTCOME_ERROR ? names[outcome] : "unknown";
}
void capi_on_error(capi_app *app, capi_error_fn handler, void *data) {
    if (app) { app->error_handler = handler; app->error_data = data; }
}
void capi_on_finish(capi_app *app, capi_finish_fn handler, void *data) {
    if (app) { app->finish_handler = handler; app->finish_data = data; }
}
void cp_request_identify(capi_ctx *ctx) {
    if (!ctx->id && ctx->app) ctx->id = atomic_fetch_add_explicit(&ctx->app->next_id, 1, memory_order_relaxed) + 1;
}
uint64_t capi_request_id(const capi_ctx *ctx) { return ctx ? ctx->id : 0; }
capi_error capi_request_error(const capi_ctx *ctx) {
    if (ctx && ctx->error.code != CAPI_ERROR_NONE) return ctx->error;
    return (capi_error){CAPI_ERROR_NONE, NULL, capi_error_message(CAPI_ERROR_NONE), 0};
}
void cp_fail(capi_ctx *ctx, capi_error_code code, const char *operation, int system_error) {
    if (!ctx) return;
    int saved_errno = errno;
    ctx->failed = true;
    if (ctx->error.code != CAPI_ERROR_NONE) return;
    if (code <= CAPI_ERROR_NONE || code > CAPI_ERROR_IO) code = CAPI_ERROR_ARGUMENT;
    if (!operation) operation = "application";
    size_t len = 0;
    while (len + 1 < sizeof(ctx->error_operation) && operation[len]) ++len;
    memcpy(ctx->error_operation, operation, len); ctx->error_operation[len] = 0;
    ctx->error = (capi_error){code, ctx->error_operation, capi_error_message(code), system_error};
    cp_request_identify(ctx);
    if (ctx->app && ctx->app->error_handler) ctx->app->error_handler(ctx->id, &ctx->error, ctx->app->error_data);
    errno = saved_errno;
}
void capi_fail(capi_ctx *ctx) { cp_fail(ctx, CAPI_ERROR_APPLICATION, "capi_fail", 0); }
void capi_fail_with(capi_ctx *ctx, capi_error_code code, const char *operation) { cp_fail(ctx, code, operation, 0); }
void cp_request_observe(capi_ctx *ctx, capi_outcome outcome, size_t sent, size_t response_bytes) {
    if (!ctx || !ctx->active || ctx->observed) return;
    ctx->observed = true;
    cp_request_identify(ctx);
    if (!ctx->app->finish_handler) return;
    int saved_errno = errno;
    uint64_t now = cp_now_ms();
    capi_request_report report = {
        .id = ctx->id, .method = ctx->method, .path = ctx->path,
        .status = ctx->response_ready ? ctx->status : 0, .outcome = outcome,
        .duration_ms = now >= ctx->started_ms ? now - ctx->started_ms : 0,
        .bytes_sent = sent, .response_bytes = ctx->response_ready ? response_bytes : 0, .arena_bytes = ctx->arena_peak,
        .error = capi_request_error(ctx)
    };
    ctx->app->finish_handler(&report, ctx->app->finish_data);
    errno = saved_errno;
}
