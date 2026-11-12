#include "capi.h"
#include "support.h"
#include <errno.h>

/* Only the private test library is compiled with malloc/calloc/realloc renamed.
 * Production builds keep ordinary libc allocation with no indirection. */
static size_t allocation, fail_at;
static unsigned errors, finishes, cleanups;
static bool cleanup_registered;
static capi_request_report report;
static int fail_now(void) {
    if (++allocation == fail_at) { errno = ENOMEM; return 1; }
    return 0;
}
void *cp_test_malloc(size_t bytes) { return fail_now() ? NULL : malloc(bytes); }
void *cp_test_calloc(size_t count, size_t bytes) { return fail_now() ? NULL : calloc(count, bytes); }
void *cp_test_realloc(void *pointer, size_t bytes) { return fail_now() ? NULL : realloc(pointer, bytes); }
static void error_hook(uint64_t id, const capi_error *error, void *data) {
    (void)data;
    CHECK(id != 0 && error->code == CAPI_ERROR_OUT_OF_MEMORY && error->system_error == ENOMEM);
    CHECK(error->operation && error->message); ++errors;
}
static void finish_hook(const capi_request_report *value, void *data) {
    (void)data; report = *value; ++finishes;
    CHECK(value->bytes_sent == 0);
}
static void cleanup(void *data) { CHECK(*(int *)data == 42); ++cleanups; }
static void handle(capi_ctx *ctx) {
    int *resource = capi_alloc(ctx, sizeof(*resource));
    if (!resource) return;
    *resource = 42;
    if (capi_defer(ctx, cleanup, resource)) return;
    cleanup_registered = true;
    capi_str name;
    if (capi_query(ctx, "name", &name) != CAPI_INPUT_FOUND) return;
    CHECK(capi_str_eq(name, "allocator"));
    char header[701]; memset(header, 'x', 700); header[700] = 0;
    if (capi_set_header(ctx, "X-Large", header)) return;
    capi_text(ctx, 200, "");
    capi_printf(ctx, "%04096d", 42);
}
int main(void) {
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_GET, "/", handle, NULL) == 0);
    capi_on_error(app, error_hook, NULL); capi_on_finish(app, finish_hook, NULL);
    const char request[] = "GET /?name=allocator HTTP/1.1\r\nHost: test\r\n\r\n";
    allocation = 0;
    capi_result r = request_text(app, request, 200);
    size_t baseline = allocation; CHECK(baseline > 4);
    CHECK(finishes == 1 && errors == 0 && cleanups == 1);
    /* These helpers exercise the response and keep the support assertions live. */
    CHECK(strlen(response_body(&r)) == 4096);
    capi_result_free(&r);
    for (size_t i = 1; i <= baseline + 1; ++i) {
        fail_at = i; allocation = errors = finishes = cleanups = 0;
        cleanup_registered = false;
        int rc = capi_dispatch(app, request, sizeof(request) - 1, &r);
        CHECK(finishes == 1 && cleanups == (cleanup_registered ? 1U : 0U));
        if (allocation < fail_at) {
            CHECK(rc == 1 && errors == 0 && report.status == 200);
        } else {
            CHECK(errors == 1 && report.error.code == CAPI_ERROR_OUT_OF_MEMORY);
            if (rc == 1) {
                CHECK(report.status == 500 && report.outcome == CAPI_OUTCOME_DISPATCHED);
                char *terminated = realloc(r.data, r.len + 1); CHECK(terminated);
                r.data = terminated; r.data[r.len] = 0;
                body_equals(&r, "Internal Server Error\n");
            } else {
                CHECK(rc == -1 && report.status == 0 && report.outcome == CAPI_OUTCOME_ERROR);
                CHECK(r.data == NULL && report.response_bytes == 0);
            }
        }
        capi_result_free(&r);
    }
    fail_at = 0; capi_app_free(app);
    printf("allocation faults: %u checks passed\n", checks); return 0;
}
