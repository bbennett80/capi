#include "support.h"
#include "resources/posts.h"
#include "resources/flags.h"
#include "resources/keywords.h"
#include <stdint.h>

static capi_result call(capi_app *app, const char *method, const char *path, const char *form, int status) {
    char request[20000];
    if (form) snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: test\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n%s", method, path, strlen(form), form);
    else snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: test\r\n\r\n", method, path);
    return request_text(app, request, status);
}
static void status_only(capi_app *app, const char *method, const char *path, const char *form, int status) {
    capi_result r = call(app, method, path, form, status); capi_result_free(&r);
}
static sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_int64 n = sqlite3_column_int64(stmt, 0);
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK); return n;
}
static capi_app *setup(sqlite3 *db) {
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(resource_posts_register(app, db) == 0);
    CHECK(resource_flags_register(app, db) == 0);
    CHECK(resource_keywords_register(app, db) == 0);
    return app;
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    sqlite3 *db;
    CHECK(capi_sqlite_open(argv[1], &db) == SQLITE_OK);
    capi_app *app = setup(db);
    capi_result r = call(app, "GET", "/posts", NULL, 200); body_equals(&r, "[]"); capi_result_free(&r);
    const char *form = "title=Alice+%26+%22Bob%22&body=%3Cb%3Ehello%3C%2Fb%3E&views=-9223372036854775808&published=false";
    r = call(app, "POST", "/posts", form, 201);
    CHECK(strstr(r.data, "Location: /posts/1\r\n"));
    body_equals(&r, "{\"id\":1,\"title\":\"Alice & \\\"Bob\\\"\",\"body\":\"<b>hello</b>\",\"views\":-9223372036854775808,\"published\":false}");
    capi_result_free(&r);
    r = call(app, "GET", "/posts/1", NULL, 200); CHECK(strstr(response_body(&r), "Alice &")); capi_result_free(&r);
    r = call(app, "HEAD", "/posts/1", NULL, 200); body_equals(&r, ""); capi_result_free(&r);
    r = call(app, "OPTIONS", "/posts/1", NULL, 204); CHECK(strstr(r.data, "PUT") && strstr(r.data, "PATCH") && strstr(r.data, "DELETE")); capi_result_free(&r);
    status_only(app, "POST", "/posts", "title=missing", 422);
    status_only(app, "POST", "/posts", NULL, 415);
    status_only(app, "POST", "/posts", "title=x&body=x&views=9223372036854775808&published=1", 422);
    status_only(app, "POST", "/posts", "title=x&body=x&views=1&published=yes", 422);
    status_only(app, "POST", "/posts", "title=%FF&body=x&views=1&published=1", 422);
    status_only(app, "POST", "/posts", "title=%ED%A0%80&body=x&views=1&published=1", 422);
    status_only(app, "POST", "/posts", "title=x%00y&body=x&views=1&published=1", 422);
    status_only(app, "POST", "/posts", "title=%&body=x&views=1&published=1", 400);
    status_only(app, "GET", "/posts?limit=0", NULL, 400);
    status_only(app, "GET", "/posts?limit=101", NULL, 400);
    status_only(app, "GET", "/posts?offset=-1", NULL, 400);
    status_only(app, "GET", "/posts?limit=%FF", NULL, 400);
    status_only(app, "GET", "/posts/0", NULL, 400);
    status_only(app, "GET", "/posts/9223372036854775808", NULL, 400);
    status_only(app, "GET", "/posts/%31", NULL, 400);
    status_only(app, "GET", "/posts/999", NULL, 404);
    status_only(app, "PATCH", "/posts/999", "published=true", 404);
    status_only(app, "PUT", "/posts/1", "title=missing", 422);
    status_only(app, "PATCH", "/posts/1", "unknown=value", 422);
    char large[5000];
    memset(large, 'x', 256); strcpy(large + 256, "&body=x&views=1&published=true");
    char input[6000]; snprintf(input, sizeof(input), "title=%s", large);
    status_only(app, "POST", "/posts", input, 422);
    memset(large, 'x', 4097); large[4097] = 0;
    snprintf(input, sizeof(input), "title=x&body=%s&views=1&published=true", large);
    status_only(app, "POST", "/posts", input, 422);
    CHECK(scalar(db, "SELECT count(*) FROM posts") == 1);
    r = call(app, "PATCH", "/posts/1", "published=true", 200);
    CHECK(strstr(response_body(&r), "\"published\":true") && strstr(response_body(&r), "Alice &")); capi_result_free(&r);
    r = call(app, "PUT", "/posts/1", "title=Updated&body=&views=9223372036854775807&published=0", 200);
    body_equals(&r, "{\"id\":1,\"title\":\"Updated\",\"body\":\"\",\"views\":9223372036854775807,\"published\":false}"); capi_result_free(&r);
    status_only(app, "PATCH", "/posts/1", "title=unchanged&views=no", 422);
    CHECK(scalar(db, "SELECT count(*) FROM posts WHERE title='Updated'") == 1);
    status_only(app, "POST", "/posts", "title=Second&body=%F0%9F%8C%B1&views=2&published=1", 201);
    status_only(app, "POST", "/posts", "title=Third&body=x&views=3&published=1", 201);
    r = call(app, "GET", "/posts?limit=1&offset=1", NULL, 200); CHECK(strstr(response_body(&r), "Second") && !strstr(response_body(&r), "Third")); capi_result_free(&r);
    /* All field kinds, quoted SQL identifiers, and bound hostile-looking text. */
    r = call(app, "POST", "/keywords", "select=%27%3BDROP+TABLE+posts%3B--", 201);
    CHECK(strstr(response_body(&r), "DROP TABLE posts")); capi_result_free(&r);
    r = call(app, "POST", "/flags", "active=false&active=true", 201); body_equals(&r, "{\"id\":1,\"active\":false}"); capi_result_free(&r);
    /* Construction failure must roll back a write before it can commit. */
    capi_limits limits = capi_limits_default(); limits.max_response_bytes = 32;
    CHECK(capi_app_limits(app, limits) == 0);
    status_only(app, "POST", "/posts", form, 500);
    status_only(app, "PATCH", "/posts/3", "title=Should+roll+back", 500);
    CHECK(sqlite3_get_autocommit(db)); CHECK(sqlite3_next_stmt(db, NULL) == NULL);
    CHECK(scalar(db, "SELECT count(*) FROM posts") == 3);
    CHECK(scalar(db, "SELECT count(*) FROM posts WHERE title='Third'") == 1);
    CHECK(capi_app_limits(app, capi_limits_default()) == 0);
    sqlite3 *other;
    CHECK(capi_sqlite_open(argv[1], &other) == SQLITE_OK);
    CHECK(sqlite3_exec(other, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    r = call(app, "POST", "/posts", form, 503); CHECK(strstr(r.data, "Retry-After: 1\r\n")); capi_result_free(&r);
    CHECK(sqlite3_exec(other, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK); CHECK(sqlite3_close(other) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "CREATE UNIQUE INDEX title_unique ON posts(title)", NULL, NULL, NULL) == SQLITE_OK);
    status_only(app, "POST", "/posts", "title=Third&body=x&views=3&published=1", 409);
    CHECK(scalar(db, "SELECT count(*) FROM posts") == 3);
    status_only(app, "DELETE", "/posts/1", NULL, 204);
    status_only(app, "DELETE", "/posts/1", NULL, 404);
    capi_app_free(app); CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(capi_sqlite_open(argv[1], &db) == SQLITE_OK); app = setup(db);
    CHECK(scalar(db, "SELECT count(*) FROM posts") == 2);
    status_only(app, "GET", "/posts/1", NULL, 404);
    status_only(app, "DELETE", "/posts/3", NULL, 204);
    r = call(app, "POST", "/posts", form, 201); CHECK(strstr(r.data, "Location: /posts/4\r\n")); capi_result_free(&r);
    capi_app_free(app); CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(capi_sqlite_open(":memory:", &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "CREATE TABLE posts(id INTEGER PRIMARY KEY)", NULL, NULL, NULL) == SQLITE_OK);
    app = capi_app_new(); CHECK(app);
    CHECK(resource_posts_register(app, db) == -1); capi_app_free(app); CHECK(sqlite3_close(db) == SQLITE_OK);
    printf("generated CRUD: %u checks passed\n", checks); return 0;
}
