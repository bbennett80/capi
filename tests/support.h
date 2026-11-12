#ifndef CAPI_TEST_SUPPORT_H
#define CAPI_TEST_SUPPORT_H
#include "capi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
static capi_result request_bytes(capi_app *app, const char *request, size_t len, int status) {
    capi_result r;
    CHECK(capi_dispatch(app, request, len, &r) == 1);
    char *terminated = realloc(r.data, r.len + 1); CHECK(terminated);
    r.data = terminated; r.data[r.len] = 0;
    char prefix[32]; snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", status);
    if (strncmp(r.data, prefix, strlen(prefix))) fprintf(stderr, "unexpected response: %s\n", r.data);
    CHECK(!strncmp(r.data, prefix, strlen(prefix)));
    return r;
}
static capi_result request_text(capi_app *app, const char *request, int status) {
    return request_bytes(app, request, strlen(request), status);
}
static const char *response_body(const capi_result *r) {
    const char *start = strstr(r->data, "\r\n\r\n"); CHECK(start); return start + 4;
}
static void body_equals(const capi_result *r, const char *expected) {
    const char *body = response_body(r);
    CHECK(r->len - (size_t)(body - r->data) == strlen(expected)); CHECK(!strcmp(body, expected));
}
#endif
