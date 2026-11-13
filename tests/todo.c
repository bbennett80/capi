#define _POSIX_C_SOURCE 200809L
#include "support.h"
#include "web.h"
#include "resources.h"
#include <unistd.h>

#ifndef TODO_VIEW_DIR
#error TODO_VIEW_DIR must point to the example views
#endif
static capi_result get(capi_app *app, const char *path, int status) {
    char request[1024]; snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", path);
    return request_text(app, request, status);
}
static capi_result form(capi_app *app, const char *method, const char *path, const char *body, const char *origin, int status) {
    char request[4096], header[256] = "";
    if (origin) snprintf(header, sizeof(header), "Origin: %s\r\n", origin);
    snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: test\r\n%sContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n%s", method, path, header, strlen(body), body);
    return request_text(app, request, status);
}
static void redirects(capi_result *r, const char *location) {
    char header[128]; snprintf(header, sizeof(header), "Location: %s\r\n", location);
    CHECK(strstr(r->data, header)); body_equals(r, ""); capi_result_free(r);
}
static capi_app *setup(sqlite3 *db, todo_web **web) {
    char error[256];
    *web = todo_web_new(db, TODO_VIEW_DIR, error, sizeof(error)); CHECK(*web);
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(todo_web_register(app, *web) == 0); CHECK(app_resources_register(app, db) == 0);
    return app;
}
int main(void) {
    char path[] = "/tmp/capi-todo-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
    sqlite3 *db; CHECK(capi_sqlite_open(path, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL) == SQLITE_OK);
    todo_web *web;
    capi_app *app = setup(db, &web);
    const char *origin = "http://127.0.0.1:8080";
    capi_result r = get(app, "/", 200);
    CHECK(strstr(r.data, "Content-Type: text/html; charset=utf-8"));
    CHECK(strstr(r.data, "Content-Security-Policy: default-src 'none'; style-src 'self'"));
    CHECK(strstr(response_body(&r), "A little space to begin."));
    CHECK(strstr(response_body(&r), "action=\"/ui/todos\""));
    CHECK(strstr(response_body(&r), "href=\"/style.css\""));
    CHECK(!strstr(response_body(&r), "<script")); capi_result_free(&r);
    r = get(app, "/style.css", 200); CHECK(strstr(r.data, "Content-Type: text/css; charset=utf-8"));
    CHECK(strstr(response_body(&r), "--green: #315c44")); capi_result_free(&r);
    r = get(app, "/style.css/anything", 404); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos", "title=Blocked&completed=false", NULL, 403); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos", "title=Blocked&completed=false", "https://example.invalid", 403); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos", "title=%3Cscript%3E%22x%22%3C%2Fscript%3E&completed=false", origin, 303); redirects(&r, "/");
    r = get(app, "/", 200);
    CHECK(strstr(response_body(&r), "&lt;script&gt;&quot;x&quot;&lt;/script&gt;"));
    CHECK(!strstr(response_body(&r), "<script>"));
    CHECK(strstr(response_body(&r), "action=\"/ui/todos/1/update\"")); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos/1/update", "completed=true&filter=active", origin, 303); redirects(&r, "/?filter=active");
    r = get(app, "/?filter=active", 200); CHECK(strstr(response_body(&r), "All caught up.")); capi_result_free(&r);
    r = get(app, "/?filter=completed", 200); CHECK(strstr(response_body(&r), "&lt;script&gt;")); CHECK(strstr(response_body(&r), "is-complete")); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos/1/update", "title=Read+a+book&filter=completed", origin, 303); redirects(&r, "/?filter=completed");
    r = get(app, "/todos/1", 200); body_equals(&r, "{\"id\":1,\"title\":\"Read a book\",\"completed\":true}"); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos/1/update", "completed=false", origin, 303); redirects(&r, "/");
    r = get(app, "/?filter=completed", 200); CHECK(strstr(response_body(&r), "A fresh start.")); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos", "title=&completed=false", origin, 422);
    CHECK(strstr(r.data, "Content-Type: text/html")); CHECK(strstr(response_body(&r), "role=\"alert\""));
    CHECK(strstr(response_body(&r), "Read a book")); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos", "title=%FF&completed=false", origin, 422); CHECK(!strchr(response_body(&r), (char)0xff)); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos/99/delete", "filter=all", origin, 404); CHECK(strstr(response_body(&r), "no longer exists")); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos/1/update", "title=Read+a+book&filter=%2F%2Fevil.example", origin, 303); redirects(&r, "/");
    /* JSON routes remain usable, with partial PATCH and full PUT semantics. */
    r = form(app, "POST", "/todos", "title=API+task&completed=false", NULL, 201);
    CHECK(strstr(r.data, "Content-Type: application/json")); CHECK(strstr(response_body(&r), "API task")); capi_result_free(&r);
    r = form(app, "PATCH", "/todos/2", "completed=true", NULL, 200); CHECK(strstr(response_body(&r), "\"completed\":true")); capi_result_free(&r);
    r = form(app, "PUT", "/todos/2", "completed=false", NULL, 422); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos/2/delete", "filter=completed", origin, 303); redirects(&r, "/?filter=completed");
    r = get(app, "/todos/2", 404); capi_result_free(&r);
    CHECK(sqlite3_get_autocommit(db)); CHECK(sqlite3_next_stmt(db, NULL) == NULL);
    capi_app_free(app); todo_web_free(web); CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(capi_sqlite_open(path, &db) == SQLITE_OK); app = setup(db, &web);
    r = get(app, "/", 200); CHECK(strstr(response_body(&r), "Read a book")); capi_result_free(&r);
    r = form(app, "POST", "/ui/todos/1/delete", "filter=all", origin, 303); redirects(&r, "/");
    r = get(app, "/", 200); CHECK(strstr(response_body(&r), "A little space to begin.")); capi_result_free(&r);
    CHECK(sqlite3_exec(db, "WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<101)"
        " INSERT INTO todos(title,completed) SELECT 'Many tasks',0 FROM n", NULL, NULL, NULL) == SQLITE_OK);
    r = get(app, "/", 200); CHECK(strstr(response_body(&r), "Showing the newest 100 tasks")); capi_result_free(&r);
    capi_app_free(app); todo_web_free(web); CHECK(sqlite3_close(db) == SQLITE_OK); unlink(path);
    printf("todo web: %u checks passed\n", checks); return 0;
}
