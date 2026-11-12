#include "capi.h"
#include "support.h"
#include <stdalign.h>
#include <stdint.h>
#include <errno.h>

typedef struct {
    unsigned errors, finished;
    uint64_t error_id;
    capi_error_code code;
    char operation[64], path[256], order[8];
    size_t order_len;
    capi_request_report report;
    int mode;
    capi_input_result input_result;
    int64_t value;
} fixture;
static void on_error(uint64_t id, const capi_error *error, void *data) {
    fixture *f = data;
    ++f->errors; f->error_id = id; f->code = error->code;
    CHECK(id != 0 && error->code != CAPI_ERROR_NONE);
    CHECK(error->message && *error->message);
    snprintf(f->operation, sizeof(f->operation), "%s", error->operation);
    f->order[f->order_len++] = 'E';
    errno = EINTR; /* observer side effects must not change API error propagation */
}
static void on_finish(const capi_request_report *report, void *data) {
    fixture *f = data;
    ++f->finished; f->report = *report;
    CHECK(report->id != 0);
    CHECK(report->outcome == CAPI_OUTCOME_DISPATCHED);
    CHECK(report->bytes_sent == 0 && report->response_bytes > 0);
    CHECK(report->error.code == f->code);
    CHECK(!report->error.code || f->error_id == report->id);
    if (report->error.code) CHECK(!strcmp(report->error.operation, f->operation));
    snprintf(f->path, sizeof(f->path), "%.*s", (int)report->path.len, report->path.data ? report->path.data : "");
    f->order[f->order_len++] = 'F';
}
static void cleanup_error(void *data) { capi_fail_with(data, CAPI_ERROR_APPLICATION, "cleanup"); }
static void handler(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    CHECK(capi_request_id(ctx) != 0);
    CHECK(capi_request_error(ctx).code == CAPI_ERROR_NONE);
    switch (f->mode) {
    case 1: CHECK(capi_set_header(ctx, "X-Test", "bad\r\nheader") == -1); capi_fail(ctx); break;
    case 2: CHECK(capi_set_header(ctx, "Content-Length", "1") == -1); break;
    case 3: capi_status(ctx, 100); break;
    case 4: CHECK(capi_json_string(ctx, (capi_str){"\xc0\xaf", 2}) == -1); break;
    case 5: CHECK(capi_write(ctx, NULL, 1) == -1); break;
    case 6: CHECK(capi_write(ctx, "oversized", 9) == -1); break;
    case 7: CHECK(capi_alloc(ctx, SIZE_MAX) == NULL); break;
    case 8: CHECK(capi_alloc(ctx, 4096) == NULL); break;
    case 9:
        CHECK(capi_defer(ctx, cleanup_error, ctx) == 0);
        capi_text(ctx, 201, "will fail during cleanup"); break;
    case 10: {
        char operation[] = "copied_operation";
        errno = EDOM;
        capi_fail_with(ctx, CAPI_ERROR_APPLICATION, operation);
        CHECK(errno == EDOM);
        memset(operation, 'x', sizeof(operation) - 1); break;
    }
    case 11:
        for (size_t i = 0; i < 64; ++i) {
            unsigned char *p = capi_alloc(ctx, 1); CHECK(p && *p == 0);
            CHECK((uintptr_t)p % alignof(max_align_t) == 0); *p = 42;
        }
        capi_write(ctx, "ok", 2); break;
    case 12:
        for (size_t i = 0; i < 100; ++i) {
            capi_str value = {"old", 3};
            CHECK(capi_query(ctx, "missing", &value) == CAPI_INPUT_MISSING);
            CHECK(value.data == NULL && value.len == 0);
        }
        capi_write(ctx, "ok", 2); break;
    case 13: {
        capi_str value;
        f->input_result = capi_query(ctx, "value", &value);
        CHECK(f->input_result == CAPI_INPUT_NO_MEMORY); break;
    }
    case 14: capi_text(ctx, 500, "application response"); break;
    default: capi_text(ctx, 200, "ok"); break;
    }
}
static capi_app *app_for(fixture *f) {
    capi_app *app = capi_app_new(); CHECK(app);
    capi_on_error(app, on_error, f); capi_on_finish(app, on_finish, f);
    CHECK(capi_route(app, CAPI_GET, "/", handler, f) == 0);
    return app;
}
static void diagnostics(void) {
    const capi_error_code expected[] = {
        CAPI_ERROR_NONE, CAPI_ERROR_HEADER, CAPI_ERROR_RESERVED_HEADER, CAPI_ERROR_STATUS,
        CAPI_ERROR_UTF8, CAPI_ERROR_ARGUMENT, CAPI_ERROR_RESPONSE_LIMIT, CAPI_ERROR_MEMORY_LIMIT,
        CAPI_ERROR_MEMORY_LIMIT, CAPI_ERROR_APPLICATION, CAPI_ERROR_APPLICATION, CAPI_ERROR_NONE,
        CAPI_ERROR_NONE, CAPI_ERROR_MEMORY_LIMIT, CAPI_ERROR_NONE
    };
    for (int mode = 0; mode < 15; ++mode) {
        fixture f = {.mode = mode}; capi_app *app = app_for(&f);
        capi_limits limits = capi_limits_default();
        CHECK(limits.max_arena_bytes == 1024 * 1024);
        if (mode == 6) limits.max_response_bytes = 4;
        if (mode == 8 || mode == 11) limits.max_arena_bytes = 4096;
        if (mode == 12) limits.max_arena_bytes = 1;
        if (mode == 13) limits.max_arena_bytes = 1;
        CHECK(capi_app_limits(app, limits) == 0);
        const char *wire = mode == 13 ? "GET /?value=too-large-for-a-64-byte-arena HTTP/1.1\r\nHost: x\r\n\r\n" :
                                       "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        int status = expected[mode] ? 500 : mode == 14 ? 500 : 200;
        capi_result r = request_text(app, wire, status);
        CHECK(f.finished == 1 && f.errors == (expected[mode] ? 1U : 0U));
        CHECK(f.report.status == status && f.report.response_bytes == r.len);
        CHECK(f.code == expected[mode]);
        CHECK(f.order_len == (f.errors ? 2U : 1U) && !memcmp(f.order, f.errors ? "EF" : "F", f.order_len));
        CHECK(f.report.arena_bytes <= limits.max_arena_bytes);
        if (mode == 1) CHECK(!strcmp(f.operation, "capi_set_header"));
        if (mode == 9) CHECK(!strcmp(f.operation, "cleanup"));
        if (mode == 10) CHECK(!strcmp(f.operation, "copied_operation"));
        if (mode == 11) CHECK(f.report.arena_bytes > 1024);
        if (mode == 12) CHECK(f.report.arena_bytes == 0);
        if (expected[mode]) body_equals(&r, "Internal Server Error\n");
        capi_result_free(&r); capi_app_free(app);
    }
    fixture f = {0}; capi_app *app = app_for(&f);
    capi_result r;
    CHECK(capi_dispatch(app, "GET /", 5, &r) == 0); CHECK(f.finished == 0 && f.errors == 0);
    r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    uint64_t first_id = f.report.id; capi_result_free(&r);
    f.order_len = 0;
    r = request_text(app, "GET /absent HTTP/1.1\r\nHost: x\r\n\r\n", 404);
    CHECK(f.report.id > first_id && f.report.status == 404 && !strcmp(f.path, "/absent")); capi_result_free(&r);
    f.order_len = 0;
    r = request_text(app, "GET / HTTP/1.1\r\n\r\n", 400);
    CHECK(f.errors == 0 && f.finished == 3 && f.report.status == 400); capi_result_free(&r);
    capi_app_free(app);
    CHECK(!strcmp(capi_error_name(CAPI_ERROR_MEMORY_LIMIT), "memory_limit"));
    CHECK(!strcmp(capi_outcome_name(CAPI_OUTCOME_TIMEOUT), "timeout"));
}

static void integers(void) {
    struct { const char *text; capi_input_result result; int64_t value; } cases[] = {
        {"0", CAPI_INPUT_FOUND, 0}, {"+12", CAPI_INPUT_FOUND, 12}, {"-12", CAPI_INPUT_FOUND, -12},
        {"-0", CAPI_INPUT_FOUND, 0}, {"00012", CAPI_INPUT_FOUND, 12},
        {"9223372036854775807", CAPI_INPUT_FOUND, INT64_MAX}, {"-9223372036854775808", CAPI_INPUT_FOUND, INT64_MIN},
        {"9223372036854775808", CAPI_INPUT_RANGE, 0}, {"-9223372036854775809", CAPI_INPUT_RANGE, 0},
        {"99999999999999999999999999", CAPI_INPUT_RANGE, 0}, {"99999999999999999999999999x", CAPI_INPUT_VALUE, 0},
        {"", CAPI_INPUT_VALUE, 0}, {"+", CAPI_INPUT_VALUE, 0}, {"-", CAPI_INPUT_VALUE, 0},
        {" 12", CAPI_INPUT_VALUE, 0}, {"12 ", CAPI_INPUT_VALUE, 0}, {"1.5", CAPI_INPUT_VALUE, 0},
        {"0x10", CAPI_INPUT_VALUE, 0}, {"12x", CAPI_INPUT_VALUE, 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        int64_t value = 123;
        CHECK(capi_str_i64((capi_str){cases[i].text, strlen(cases[i].text)}, INT64_MIN, INT64_MAX, &value) == cases[i].result);
        CHECK(value == cases[i].value);
    }
    int64_t value = 123;
    CHECK(capi_str_i64((capi_str){"12", 2}, 1, 10, &value) == CAPI_INPUT_RANGE && value == 0);
    CHECK(capi_str_i64((capi_str){"1\0x", 3}, INT64_MIN, INT64_MAX, &value) == CAPI_INPUT_VALUE && value == 0);
    CHECK(capi_str_i64((capi_str){"1", 1}, 2, 1, &value) == CAPI_INPUT_ARGUMENT && value == 0);
    CHECK(capi_str_i64((capi_str){NULL, 1}, 1, 2, &value) == CAPI_INPUT_ARGUMENT && value == 0);
    CHECK(capi_str_i64((capi_str){"1", 1}, 1, 2, NULL) == CAPI_INPUT_ARGUMENT);
}
static void typed(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    if (f->mode == 0) f->input_result = capi_query_i64(ctx, "id", 1, 100, &f->value);
    else if (f->mode == 1) f->input_result = capi_form_i64(ctx, "id", 1, 100, &f->value);
    else f->input_result = capi_param_i64(ctx, "id", 1, 100, &f->value);
    capi_text(ctx, 200, capi_input_name(f->input_result));
}
static void input_helpers(void) {
    fixture f = {0}; capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_ANY, "/typed", typed, &f) == 0);
    CHECK(capi_route(app, CAPI_GET, "/typed/:id", typed, &f) == 0);
    struct { const char *query; capi_input_result result; int64_t value; } cases[] = {
        {"id=12", CAPI_INPUT_FOUND, 12}, {"id=%2B12", CAPI_INPUT_FOUND, 12},
        {"id=+12", CAPI_INPUT_VALUE, 0}, {"id=101", CAPI_INPUT_RANGE, 0},
        {"id=", CAPI_INPUT_VALUE, 0}, {"other=12", CAPI_INPUT_MISSING, 0},
        {"id=%zz", CAPI_INPUT_ENCODING, 0}, {"id=1%002", CAPI_INPUT_VALUE, 0},
        {"%zz=x&id=12", CAPI_INPUT_ENCODING, 0}, {"other=%zz&id=12", CAPI_INPUT_FOUND, 12},
        {"id=12&id=13", CAPI_INPUT_FOUND, 12}, {"id=&id=13", CAPI_INPUT_VALUE, 0}
    };
    for (int mode = 0; mode < 2; ++mode) {
        f.mode = mode;
        for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
            char request[512]; f.value = 123;
            if (!mode) snprintf(request, sizeof(request), "GET /typed?%s HTTP/1.1\r\nHost: x\r\n\r\n", cases[i].query);
            else snprintf(request, sizeof(request), "POST /typed HTTP/1.1\r\nHost: x\r\nContent-Type: Application/X-Www-Form-Urlencoded; charset=UTF-8\r\nContent-Length: %zu\r\n\r\n%s", strlen(cases[i].query), cases[i].query);
            capi_result r = request_text(app, request, 200);
            CHECK(f.input_result == cases[i].result && f.value == cases[i].value);
            body_equals(&r, capi_input_name(cases[i].result)); capi_result_free(&r);
        }
    }
    capi_result r = request_text(app, "POST /typed HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    CHECK(f.input_result == CAPI_INPUT_CONTENT_TYPE && f.value == 0); capi_result_free(&r);
    f.mode = 2;
    r = request_text(app, "GET /typed/12 HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    CHECK(f.input_result == CAPI_INPUT_FOUND && f.value == 12); capi_result_free(&r);
    r = request_text(app, "GET /typed/%31 HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    CHECK(f.input_result == CAPI_INPUT_VALUE && f.value == 0); capi_result_free(&r);
    r = request_text(app, "GET /typed HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    CHECK(f.input_result == CAPI_INPUT_MISSING && f.value == 0); capi_result_free(&r);
    capi_app_free(app);
}
int main(void) {
    diagnostics(); integers(); input_helpers();
    printf("reliability: %u checks passed\n", checks); return 0;
}
