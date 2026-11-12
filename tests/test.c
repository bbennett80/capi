#include "capi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>
#include <stdint.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
static int contains(const capi_result *r, const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= r->len; ++i) if (!memcmp(r->data + i, s, n)) return 1;
    return 0;
}
static size_t body_offset(const capi_result *r) {
    for (size_t i = 0; i + 4 <= r->len; ++i) if (!memcmp(r->data + i, "\r\n\r\n", 4)) return i + 4;
    CHECK(0); return 0;
}
static void body_is(const capi_result *r, const char *s) {
    size_t start = body_offset(r);
    CHECK(r->len - start == strlen(s)); CHECK(!memcmp(r->data + start, s, strlen(s)));
}
static capi_result send_request(capi_app *app, const char *request, int status) {
    capi_result r;
    CHECK(capi_dispatch(app, request, strlen(request), &r) == 1);
    char expected[32]; snprintf(expected, sizeof(expected), " %d ", status);
    if (!contains(&r, expected)) fprintf(stderr, "request: %s\nresponse: %.*s\n", request, (int)r.len, r.data);
    CHECK(contains(&r, expected));
    return r;
}
static void ok(capi_ctx *ctx) { capi_text(ctx, 200, capi_data(ctx) ? capi_data(ctx) : "ok"); }
static void param(capi_ctx *ctx) {
    capi_str id = capi_param(ctx, "id"), rest = capi_param(ctx, "rest");
    CHECK(capi_param(ctx, "absent").data == NULL);
    capi_write(ctx, id.data ? id.data : rest.data, id.data ? id.len : rest.len);
}
static void echo(capi_ctx *ctx) { capi_str b = capi_body(ctx); capi_write(ctx, b.data, b.len); }
static void query(capi_ctx *ctx) {
    capi_str value;
    int found = capi_query(ctx, "name", &value);
    if (found < 0) { capi_text(ctx, 400, "bad query"); return; }
    if (!found) { capi_text(ctx, 200, "missing"); return; }
    capi_json(ctx, 200, ""); capi_json_string(ctx, value);
}
static char order[32]; static size_t order_at;
static void outer(capi_ctx *ctx) {
    CHECK(!strcmp(capi_data(ctx), "outer"));
    int *state = capi_alloc(ctx, sizeof(*state)); CHECK(state && *state == 0);
    CHECK((uintptr_t)state % alignof(max_align_t) == 0);
    *state = 42; capi_set_state(ctx, state);
    order[order_at++] = 'a'; capi_next(ctx); order[order_at++] = 'd';
    CHECK(!strcmp(capi_data(ctx), "outer"));
    capi_set_header(ctx, "X-After", "yes"); capi_next(ctx); /* second next is ignored */
}
static void inner(capi_ctx *ctx) {
    CHECK(*(int *)capi_state(ctx) == 42);
    order[order_at++] = 'b'; capi_next(ctx); order[order_at++] = 'c';
}
static void halt(capi_ctx *ctx) { capi_text(ctx, 403, "stop"); }
static void bad_header(capi_ctx *ctx) { CHECK(capi_set_header(ctx, "X-Test", "yes\r\nInjected: true") == -1); }
static void framing_header(capi_ctx *ctx) { CHECK(capi_set_header(ctx, "Content-Length", "12") == -1); }
static void header_copy(capi_ctx *ctx) {
    char value[] = "first";
    capi_set_header(ctx, "X-Test", value); value[0] = 'x';
    capi_set_header(ctx, "x-test", "second");
    CHECK(capi_str_eq(capi_header(ctx, "X-INPUT"), "value"));
}
static void overflow(capi_ctx *ctx) { char data[64] = {0}; CHECK(capi_write(ctx, data, sizeof(data)) == -1); }
static void invalid_utf8(capi_ctx *ctx) { capi_json_string(ctx, (capi_str){"\xc0\xaf", 2}); }
static void no_content(capi_ctx *ctx) { capi_text(ctx, 204, "must disappear"); }
static void reset_content(capi_ctx *ctx) { capi_text(ctx, 205, "must disappear"); }
static void not_modified(capi_ctx *ctx) { capi_text(ctx, 304, "must disappear"); }
static void redirect(capi_ctx *ctx) { capi_redirect(ctx, 303, "/new"); }
static void arena(capi_ctx *ctx) {
    unsigned char *first = capi_alloc(ctx, 3);
    unsigned char *big = capi_alloc(ctx, 10000);
    long double *last = capi_alloc(ctx, sizeof(*last));
    CHECK(first && big && last);
    CHECK((uintptr_t)last % alignof(max_align_t) == 0);
    CHECK(first[0] == 0 && big[9999] == 0 && *last == 0);
    first[0] = 123; big[9999] = 42;
    CHECK(first[0] == 123 && big[9999] == 42);
    capi_text(ctx, 200, "arena");
}

static void routing(void) {
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_GET, "/users/:id", param, NULL) == 0);
    CHECK(capi_route(app, CAPI_GET, "/users/new", ok, "literal") == 0);
    CHECK(capi_route(app, CAPI_GET, "/files/*rest", param, NULL) == 0);
    CHECK(capi_route(app, CAPI_GET, "/", ok, NULL) == 0);
    CHECK(capi_route(app, CAPI_GET, "/slash/", ok, NULL) == 0);
    CHECK(capi_route(app, CAPI_ANY, "/any", ok, NULL) == 0);
    CHECK(capi_route(app, CAPI_POST, "/users/:other", ok, "post") == 0);
    CHECK(capi_route(app, CAPI_HEAD, "/head", ok, "explicit") == 0);
    CHECK(capi_route(app, CAPI_GET, "/head", ok, "get") == 0);
    CHECK(capi_route(app, CAPI_OPTIONS, "/options", ok, "custom options") == 0);
    CHECK(capi_route(app, CAPI_GET, "/users/:other", ok, NULL) == -1);
    CHECK(strlen(capi_app_error(app)) > 0);
    CHECK(capi_route(app, CAPI_GET, "/:id/:id", ok, NULL) == -1);
    CHECK(capi_route(app, CAPI_GET, "/bad/*rest/other", ok, NULL) == -1);
    CHECK(capi_route(app, CAPI_GET, "/bad//other", ok, NULL) == -1);
    CHECK(capi_route(app, CAPI_GET, "relative", ok, NULL) == -1);
    const char *requests[] = {
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n", "GET /users/new HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /users/abc%20def HTTP/1.1\r\nHost: x\r\n\r\n", "GET /files/a/b HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /files/ HTTP/1.1\r\nHost: x\r\n\r\n", "GET /slash/ HTTP/1.1\r\nHost: x\r\n\r\n",
        "BREW /any HTTP/1.1\r\nHost: x\r\n\r\n"};
    const char *bodies[] = {"ok", "literal", "abc%20def", "a/b", "", "ok", "ok"};
    for (size_t i = 0; i < sizeof(requests) / sizeof(*requests); ++i) {
        capi_result r = send_request(app, requests[i], 200); body_is(&r, bodies[i]); capi_result_free(&r);
    }
    capi_result r = send_request(app, "POST /users/new HTTP/1.1\r\nHost: x\r\n\r\n", 405);
    CHECK(contains(&r, "Allow: GET, HEAD, OPTIONS\r\n")); capi_result_free(&r);
    r = send_request(app, "HEAD /users/new HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    CHECK(contains(&r, "Content-Length: 7\r\n")); body_is(&r, ""); capi_result_free(&r);
    r = send_request(app, "OPTIONS /users/new HTTP/1.1\r\nHost: x\r\n\r\n", 204);
    CHECK(!contains(&r, "Content-Length:")); body_is(&r, ""); capi_result_free(&r);
    r = send_request(app, "GET /slash HTTP/1.1\r\nHost: x\r\n\r\n", 404); capi_result_free(&r);
    r = send_request(app, "GET /users/ HTTP/1.1\r\nHost: x\r\n\r\n", 404); capi_result_free(&r);
    r = send_request(app, "HEAD /head HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    CHECK(contains(&r, "Content-Length: 8\r\n")); body_is(&r, ""); capi_result_free(&r);
    r = send_request(app, "OPTIONS /options HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    body_is(&r, "custom options"); capi_result_free(&r);
    /* Static branch backtracks when it cannot complete the path. */
    CHECK(capi_route(app, CAPI_GET, "/a/b/c", ok, NULL) == 0);
    CHECK(capi_route(app, CAPI_GET, "/a/:id/d", param, NULL) == 0);
    r = send_request(app, "GET /a/b/d HTTP/1.1\r\nHost: x\r\n\r\n", 200); body_is(&r, "b"); capi_result_free(&r);
    capi_not_found(app, ok, "custom missing");
    r = send_request(app, "GET /missing HTTP/1.1\r\nHost: x\r\n\r\n", 200); body_is(&r, "custom missing"); capi_result_free(&r);
    capi_app_free(app);
    app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_GET, "/*rest", param, NULL) == 0);
    r = send_request(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 200); body_is(&r, ""); capi_result_free(&r);
    r = send_request(app, "GET /a/b HTTP/1.1\r\nHost: x\r\n\r\n", 200); body_is(&r, "a/b"); capi_result_free(&r);
    capi_app_free(app);
}

static void middleware(void) {
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_use(app, "/", outer, "outer") == 0);
    CHECK(capi_use(app, "/api/", inner, NULL) == 0);
    CHECK(capi_use(app, "/private", halt, NULL) == 0);
    CHECK(capi_route(app, CAPI_GET, "/api", ok, NULL) == 0);
    capi_result r = send_request(app, "GET /api HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    CHECK(order_at == 4 && !memcmp(order, "abcd", 4)); CHECK(contains(&r, "X-After: yes")); capi_result_free(&r);
    order_at = 0;
    r = send_request(app, "GET /apix HTTP/1.1\r\nHost: x\r\n\r\n", 404);
    CHECK(order_at == 2 && !memcmp(order, "ad", 2)); capi_result_free(&r);
    r = send_request(app, "GET /private HTTP/1.1\r\nHost: x\r\n\r\n", 403); body_is(&r, "stop"); capi_result_free(&r);
    capi_app_free(app);
}

static void parser(void) {
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_ANY, "/", echo, NULL) == 0);
    const char *bad[] = {
        "GET / HTTP/1.1\r\n\r\n", "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1, 1\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost : a\r\n\r\n", "GET / HTTP/1.1\r\nHost: a\r\n folded: no\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nX: \001\r\n\r\n", "GET /#fragment HTTP/1.1\r\nHost: a\r\n\r\n",
        "GET  / HTTP/1.1\r\nHost: a\r\n\r\n", "GET http://x/ HTTP/1.1\r\nHost: a\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a b\r\n\r\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        capi_result r = send_request(app, bad[i], 400); CHECK(r.close); capi_result_free(&r);
    }
    struct { const char *request; int status; } cases[] = {
        {"GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        {"POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n", 417},
        {"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999999999999999999999\r\n\r\n", 413},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n", 413}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        capi_result r = send_request(app, cases[i].request, cases[i].status); CHECK(r.close); capi_result_free(&r);
    }
    const char *complete = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    for (size_t i = 0; i < strlen(complete); ++i) {
        capi_result r; CHECK(capi_dispatch(app, complete, i, &r) == 0); capi_result_free(&r);
    }
    capi_result r = send_request(app, complete, 200); body_is(&r, "hello"); CHECK(!r.close); capi_result_free(&r);
    const char binary[] = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na\0b";
    CHECK(capi_dispatch(app, binary, sizeof(binary) - 1, &r) == 1);
    size_t offset = body_offset(&r); CHECK(r.len - offset == 3 && !memcmp(r.data + offset, "a\0b", 3)); capi_result_free(&r);
    const char *pipeline = "GET / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.0\r\n\r\n";
    r = send_request(app, pipeline, 200); CHECK(r.consumed == 27); capi_result_free(&r);
    r = send_request(app, "GET / HTTP/1.0\r\n\r\n", 200); CHECK(r.close); CHECK(contains(&r, "HTTP/1.0")); capi_result_free(&r);
    r = send_request(app, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 200); CHECK(!r.close); capi_result_free(&r);
    r = send_request(app, "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nConnection: Close\r\n\r\n", 200); CHECK(r.close); capi_result_free(&r);
    char many_headers[4096] = "GET / HTTP/1.1\r\nHost: x\r\n";
    for (size_t i = 0; i < CAPI_MAX_HEADERS - 1; ++i) strcat(many_headers, "X: y\r\n");
    strcat(many_headers, "\r\n");
    r = send_request(app, many_headers, 200); capi_result_free(&r);
    size_t many_len = strlen(many_headers);
    strcpy(many_headers + many_len - 2, "X: z\r\n\r\n");
    r = send_request(app, many_headers, 431); CHECK(r.close); capi_result_free(&r);
    const char nul_header[] = "GET / HTTP/1.1\r\nHost: x\r\nX: a\0b\r\n\r\n";
    CHECK(capi_dispatch(app, nul_header, sizeof(nul_header) - 1, &r) == 1);
    CHECK(contains(&r, " 400 ")); capi_result_free(&r);
    CHECK(capi_app_limits(app, (capi_limits){128, 32, 32}) == 0);
    char large[160]; memset(large, 'x', sizeof(large));
    CHECK(capi_dispatch(app, large, sizeof(large), &r) == 1); CHECK(contains(&r, " 431 ")); capi_result_free(&r);
    CHECK(capi_app_limits(app, (capi_limits){0, 0, 0}) == -1);
    capi_app_free(app);
}

static void responses(void) {
    capi_app *app = capi_app_new(); CHECK(app);
    struct { const char *path; capi_handler handler; int status; } cases[] = {
        {"/bad", bad_header, 500}, {"/framing", framing_header, 500}, {"/copy", header_copy, 200},
        {"/utf8", invalid_utf8, 500}, {"/empty", no_content, 204}, {"/reset", reset_content, 205},
        {"/cached", not_modified, 304}, {"/redirect", redirect, 303}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        CHECK(capi_route(app, CAPI_GET, cases[i].path, cases[i].handler, NULL) == 0);
        char request[128]; snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: x\r\nX-Input: value\r\n\r\n", cases[i].path);
        capi_result r = send_request(app, request, cases[i].status);
        if (cases[i].status >= 500) { CHECK(r.close); body_is(&r, "Internal Server Error\n"); }
        else body_is(&r, "");
        if (cases[i].handler == header_copy) { CHECK(contains(&r, "x-test: second\r\n")); CHECK(!contains(&r, "first")); }
        if (cases[i].handler == redirect) CHECK(contains(&r, "Location: /new"));
        capi_result_free(&r);
    }
    CHECK(capi_route(app, CAPI_GET, "/query", query, NULL) == 0);
    struct { const char *query; const char *body; int status; } queries[] = {
        {"name=Ada+Lovelace", "\"Ada Lovelace\"", 200}, {"na%6de=a%22%5c%0a", "\"a\\\"\\\\\\u000a\"", 200},
        {"name=", "\"\"", 200}, {"other=x", "missing", 200}, {"name=%zz", "bad query", 400},
        {"name=%", "bad query", 400}, {"name=%00", "\"\\u0000\"", 200}, {"name=%E2%9C%93", "\"\xe2\x9c\x93\"", 200},
        {"name=one&name=two", "\"one\"", 200}};
    for (size_t i = 0; i < sizeof(queries) / sizeof(*queries); ++i) {
        char request[256]; snprintf(request, sizeof(request), "GET /query?%s HTTP/1.1\r\nHost: x\r\n\r\n", queries[i].query);
        capi_result r = send_request(app, request, queries[i].status); body_is(&r, queries[i].body); capi_result_free(&r);
    }
    CHECK(capi_route(app, CAPI_GET, "/overflow", overflow, NULL) == 0);
    CHECK(capi_route(app, CAPI_GET, "/arena", arena, NULL) == 0);
    capi_result a = send_request(app, "GET /arena HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    body_is(&a, "arena"); capi_result_free(&a);
    CHECK(capi_app_limits(app, (capi_limits){16384, 1024, 32}) == 0);
    capi_result r = send_request(app, "GET /overflow HTTP/1.1\r\nHost: x\r\n\r\n", 500); capi_result_free(&r);
    capi_app_free(app);
}

static char cleanup_order[32];
static size_t cleanup_at;
static void cleanup_record(void *data) { cleanup_order[cleanup_at++] = *(char *)data; }
static void defer_outer(capi_ctx *ctx) {
    char *letter = capi_alloc(ctx, 1); CHECK(letter); *letter = 'a';
    CHECK(capi_defer(ctx, cleanup_record, letter) == 0);
    capi_next(ctx);
    CHECK(cleanup_at == 0); /* cleanup is after the full middleware unwind */
}
static void defer_endpoint(capi_ctx *ctx) {
    char *letter = capi_alloc(ctx, 1); CHECK(letter); *letter = 'b';
    CHECK(capi_defer(ctx, cleanup_record, letter) == 0);
    if (capi_str_eq(capi_path(ctx), "/fail")) capi_fail(ctx);
    capi_text(ctx, 200, "deferred");
}
static void defer_halt(capi_ctx *ctx) {
    char *letter = capi_alloc(ctx, 1); CHECK(letter); *letter = 'h';
    CHECK(capi_defer(ctx, cleanup_record, letter) == 0);
    capi_text(ctx, 403, "halted");
}
static void form_value(capi_ctx *ctx) {
    capi_str value;
    int result = capi_form(ctx, "name", &value);
    if (result < 0) { capi_text(ctx, 400, "bad form"); return; }
    if (!result) { capi_text(ctx, 200, "absent"); return; }
    capi_json(ctx, 200, ""); capi_json_string(ctx, value);
}
static void lifecycle_and_forms(void) {
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_use(app, "/", defer_outer, NULL) == 0);
    CHECK(capi_use(app, "/halt", defer_halt, NULL) == 0);
    CHECK(capi_route(app, CAPI_GET, "/", defer_endpoint, NULL) == 0);
    CHECK(capi_route(app, CAPI_GET, "/fail", defer_endpoint, NULL) == 0);
    const char *paths[] = {"/", "/fail", "/halt"};
    for (size_t i = 0; i < 3; ++i) {
        cleanup_at = 0;
        char request[128]; snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", paths[i]);
        capi_result r = send_request(app, request, i == 0 ? 200 : i == 1 ? 500 : 403);
        CHECK(cleanup_at == 2 && !memcmp(cleanup_order, i == 2 ? "ha" : "ba", 2)); capi_result_free(&r);
    }
    capi_app_free(app);
    app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_POST, "/", form_value, NULL) == 0);
    struct { const char *input, *expected; int status; } forms[] = {
        {"name=Ada+Lovelace", "\"Ada Lovelace\"", 200}, {"na%6de=%22%3C%26", "\"\\\"<&\"", 200},
        {"name=one&name=two", "\"one\"", 200}, {"name=", "\"\"", 200}, {"other=a", "absent", 200},
        {"name=%00", "\"\\u0000\"", 200}, {"name=%xy", "bad form", 400}
    };
    for (size_t i = 0; i < sizeof(forms) / sizeof(*forms); ++i) {
        char request[512];
        snprintf(request, sizeof(request), "POST / HTTP/1.1\r\nHost: x\r\nContent-Type: Application/X-Www-Form-Urlencoded; charset=UTF-8\r\nContent-Length: %zu\r\n\r\n%s",
                 strlen(forms[i].input), forms[i].input);
        capi_result r = send_request(app, request, forms[i].status); body_is(&r, forms[i].expected); capi_result_free(&r);
    }
    capi_result r = send_request(app, "POST / HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\nContent-Length: 6\r\n\r\nname=x", 400);
    body_is(&r, "bad form"); capi_result_free(&r); capi_app_free(app);
}

int main(void) {
    routing(); middleware(); parser(); responses(); lifecycle_and_forms();
    printf("core: %u checks passed\n", checks); return 0;
}
