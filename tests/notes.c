#define _POSIX_C_SOURCE 200809L
#include "../examples/notes/app.h"
#include "support.h"
#include <unistd.h>

#ifndef CAPI_NOTES_VIEW_DIR
#define CAPI_NOTES_VIEW_DIR "examples/notes/views"
#endif

static capi_result post(notes_app *notes, const char *path, const char *body, int status) {
    char request[8192];
    int n = snprintf(request, sizeof(request), "POST %s HTTP/1.1\r\nHost: notes.test\r\nOrigin: http://notes.test\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n%s", path, strlen(body), body);
    CHECK(n > 0 && (size_t)n < sizeof(request));
    return request_text(notes_http(notes), request, status);
}
static capi_result get(notes_app *notes) {
    return request_text(notes_http(notes), "GET / HTTP/1.1\r\nHost: notes.test\r\n\r\n", 200);
}
int main(void) {
    char path[] = "/tmp/capi-notes-test-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
    char error[256];
    notes_app *notes = notes_open(path, CAPI_NOTES_VIEW_DIR, error, sizeof(error));
    if (!notes) fprintf(stderr, "%s\n", error);
    CHECK(notes); CHECK(notes_origin(notes, "http://notes.test") == 0);
    capi_result r = get(notes);
    CHECK(strstr(r.data, "A fresh page."));
    CHECK(strstr(r.data, "Content-Type: text/html; charset=utf-8"));
    CHECK(strstr(r.data, "Content-Security-Policy:")); capi_result_free(&r);
    r = request_text(notes_http(notes), "POST /notes HTTP/1.1\r\nHost: notes.test\r\n\r\n", 403); capi_result_free(&r);
    r = request_text(notes_http(notes), "POST /notes HTTP/1.1\r\nHost: notes.test\r\nOrigin: http://attacker.test\r\n\r\n", 403); capi_result_free(&r);
    r = post(notes, "/notes", "body=missing+title", 400); capi_result_free(&r);
    r = post(notes, "/notes", "title=+++&body=x", 422); capi_result_free(&r);
    r = post(notes, "/notes", "title=x%00y", 422); capi_result_free(&r);
    r = post(notes, "/notes", "title=%xy", 400); capi_result_free(&r);
    r = post(notes, "/notes", "title=%3Cscript%3Ealert%281%29%3C%2Fscript%3E&body=Hello%0AWorld+%26+friends", 303);
    CHECK(strstr(r.data, "Location: /\r\n")); body_equals(&r, ""); capi_result_free(&r);
    r = get(notes);
    CHECK(strstr(r.data, "&lt;script&gt;alert(1)&lt;/script&gt;"));
    CHECK(!strstr(r.data, "<script>"));
    CHECK(strstr(r.data, "Hello\nWorld &amp; friends"));
    CHECK(strstr(r.data, "/notes/1/delete"));
    CHECK(!strstr(r.data, "A fresh page.")); capi_result_free(&r);
    r = post(notes, "/notes", "title=x%27%29%3B+DROP+TABLE+notes%3B--&body=bound+parameter", 303); capi_result_free(&r);
    notes_close(notes);
    notes = notes_open(path, CAPI_NOTES_VIEW_DIR, error, sizeof(error)); CHECK(notes);
    CHECK(notes_origin(notes, "http://notes.test") == 0);
    r = get(notes);
    CHECK(strstr(r.data, "DROP TABLE notes;--"));
    CHECK(strstr(r.data, "bound parameter")); CHECK(strstr(r.data, "class=\"count\">2")); capi_result_free(&r);
    r = post(notes, "/notes/1/delete", "", 303); capi_result_free(&r);
    r = post(notes, "/notes/1/delete", "", 404); capi_result_free(&r);
    r = post(notes, "/notes/999999999999999999999999/delete", "", 400); capi_result_free(&r);
    r = post(notes, "/notes/no/delete", "", 400); capi_result_free(&r);
    r = post(notes, "/notes/2/delete", "", 303); capi_result_free(&r);
    r = get(notes); CHECK(strstr(r.data, "A fresh page.")); capi_result_free(&r);
    notes_close(notes); CHECK(unlink(path) == 0);
    printf("notes: %u checks passed\n", checks); return 0;
}
