#define _POSIX_C_SOURCE 200809L
#include "capi_sqlite.h"
#include "support.h"
#include <unistd.h>

typedef struct { sqlite3 *db; int mode; } fixture;
static void insert(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    capi_sqlite_tx *tx;
    CHECK(capi_sqlite_begin(ctx, f->db, CAPI_SQLITE_IMMEDIATE, &tx) == SQLITE_OK);
    capi_sqlite_tx *nested;
    CHECK(capi_sqlite_begin(ctx, f->db, CAPI_SQLITE_DEFERRED, &nested) == SQLITE_MISUSE);
    CHECK(nested == NULL);
    sqlite3_stmt *stmt;
    CHECK(capi_sqlite_prepare(ctx, f->db, "INSERT INTO notes(body, data) VALUES(?1, ?2); \n", &stmt) == SQLITE_OK);
    capi_str body = capi_body(ctx);
    CHECK(capi_sqlite_bind_text(stmt, 1, body) == SQLITE_OK);
    CHECK(capi_sqlite_bind_blob(stmt, 2, (capi_str){NULL, 0}) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    if (f->mode == 1) {
        CHECK(capi_sqlite_commit(tx) == SQLITE_OK);
        CHECK(capi_sqlite_commit(tx) == SQLITE_MISUSE);
    } else if (f->mode == 2) {
        CHECK(capi_sqlite_rollback(tx) == SQLITE_OK);
        CHECK(capi_sqlite_rollback(tx) == SQLITE_MISUSE);
    } else if (f->mode == 3) capi_fail(ctx);
    capi_text(ctx, 200, "done");
}
static void query(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    sqlite3_stmt *stmt;
    CHECK(capi_sqlite_prepare(ctx, f->db, "SELECT body, data, NULL FROM notes ORDER BY id", &stmt) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_type(stmt, 1) == SQLITE_BLOB);
    CHECK(sqlite3_column_bytes(stmt, 1) == 0);
    capi_str text, null_value;
    CHECK(capi_sqlite_copy_text(ctx, stmt, 0, &text) == SQLITE_OK);
    CHECK(capi_sqlite_copy_text(ctx, stmt, 2, &null_value) == SQLITE_OK);
    CHECK(null_value.data == NULL && null_value.len == 0);
    CHECK(capi_sqlite_copy_text(ctx, stmt, 99, &null_value) == SQLITE_MISUSE);
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    /* The copied column remains valid after the cursor advanced. */
    capi_write(ctx, text.data, text.len);
}
static void invalid(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    sqlite3_stmt *stmt;
    CHECK(capi_sqlite_prepare(ctx, f->db, "SELECT 1; DELETE FROM notes", &stmt) == SQLITE_MISUSE);
    CHECK(stmt == NULL);
    CHECK(capi_sqlite_prepare(ctx, f->db, "SELECT 1; -- tail comment", &stmt) == SQLITE_MISUSE);
    CHECK(stmt == NULL);
    CHECK(capi_sqlite_prepare(ctx, f->db, "  ", &stmt) == SQLITE_MISUSE);
    CHECK(stmt == NULL);
    CHECK(capi_sqlite_prepare(ctx, f->db, "THIS IS NOT SQL", &stmt) == SQLITE_ERROR);
    CHECK(stmt == NULL);
    CHECK(capi_sqlite_prepare(ctx, f->db, "SELECT ?1, ?2, ?3", &stmt) == SQLITE_OK);
    char mutable[] = "copied";
    CHECK(capi_sqlite_bind_text(stmt, 1, (capi_str){mutable, 6}) == SQLITE_OK);
    mutable[0] = 'x';
    CHECK(capi_sqlite_bind_blob(stmt, 2, (capi_str){"a\0b", 3}) == SQLITE_OK);
    CHECK(capi_sqlite_bind_text(stmt, 3, (capi_str){NULL, 0}) == SQLITE_OK);
    CHECK(capi_sqlite_bind_text(stmt, 5, (capi_str){"x", 1}) == SQLITE_RANGE);
    CHECK(capi_sqlite_bind_text(stmt, 1, (capi_str){NULL, 1}) == SQLITE_MISUSE);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(!strcmp((const char *)sqlite3_column_text(stmt, 0), "copied"));
    CHECK(sqlite3_column_bytes(stmt, 1) == 3 && !memcmp(sqlite3_column_blob(stmt, 1), "a\0b", 3));
    CHECK(sqlite3_column_type(stmt, 2) == SQLITE_TEXT && sqlite3_column_bytes(stmt, 2) == 0);
    capi_text(ctx, 200, "checked");
}
static void constraint(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    capi_sqlite_tx *tx;
    CHECK(capi_sqlite_begin(ctx, f->db, CAPI_SQLITE_DEFERRED, &tx) == SQLITE_OK);
    sqlite3_stmt *stmt;
    CHECK(capi_sqlite_prepare(ctx, f->db, "INSERT INTO children(note_id) VALUES(999)", &stmt) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_CONSTRAINT_FOREIGNKEY);
    CHECK(capi_sqlite_prepare(ctx, f->db, "INSERT INTO deferred_children(note_id) VALUES(999)", &stmt) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    CHECK(capi_sqlite_commit(tx) == SQLITE_CONSTRAINT_FOREIGNKEY);
    CHECK(!sqlite3_get_autocommit(f->db)); /* failed commit still needs rollback */
    capi_text(ctx, 409, "constraint");
}
static void busy(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    capi_sqlite_tx *tx;
    CHECK((capi_sqlite_begin(ctx, f->db, CAPI_SQLITE_IMMEDIATE, &tx) & 0xff) == SQLITE_BUSY);
    CHECK(tx == NULL); capi_text(ctx, 503, "busy");
}
static int64_t count(sqlite3 *db) {
    sqlite3_stmt *stmt;
    CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM notes", -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    int64_t n = sqlite3_column_int64(stmt, 0); CHECK(sqlite3_finalize(stmt) == SQLITE_OK); return n;
}
static void lifecycle(void) {
    fixture f = {0}; CHECK(capi_sqlite_open(":memory:", &f.db) == SQLITE_OK);
    CHECK(sqlite3_exec(f.db, "CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT, data BLOB);"
        "CREATE TABLE children(note_id INTEGER REFERENCES notes(id));"
        "CREATE TABLE deferred_children(note_id INTEGER REFERENCES notes(id) DEFERRABLE INITIALLY DEFERRED);", NULL, NULL, NULL) == SQLITE_OK);
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_POST, "/", insert, &f) == 0);
    CHECK(capi_route(app, CAPI_GET, "/", query, &f) == 0);
    CHECK(capi_route(app, CAPI_GET, "/invalid", invalid, &f) == 0);
    CHECK(capi_route(app, CAPI_GET, "/constraint", constraint, &f) == 0);
    for (int mode = 0; mode < 4; ++mode) {
        f.mode = mode;
        capi_result r = request_text(app, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n<&>'", mode == 3 ? 500 : 200);
        if (mode == 3) body_equals(&r, "Internal Server Error\n"); else body_equals(&r, "done");
        capi_result_free(&r);
        CHECK(count(f.db) == (mode >= 1 ? 1 : 0));
        CHECK(sqlite3_get_autocommit(f.db)); CHECK(sqlite3_next_stmt(f.db, NULL) == NULL);
    }
    capi_result r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    body_equals(&r, "<&>'"); capi_result_free(&r);
    r = request_text(app, "GET /invalid HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    body_equals(&r, "checked"); capi_result_free(&r);
    r = request_text(app, "GET /constraint HTTP/1.1\r\nHost: x\r\n\r\n", 409); capi_result_free(&r);
    CHECK(sqlite3_get_autocommit(f.db)); CHECK(sqlite3_next_stmt(f.db, NULL) == NULL);
    capi_app_free(app); CHECK(sqlite3_close(f.db) == SQLITE_OK);
}
static void disk_and_locking(void) {
    char path[] = "/tmp/capi-sqlite-test-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
    fixture f = {0}; sqlite3 *other = NULL;
    CHECK(capi_sqlite_open(path, &f.db) == SQLITE_OK);
    CHECK(sqlite3_exec(f.db, "CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT, data BLOB)", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(capi_sqlite_open(path, &other) == SQLITE_OK);
    CHECK(sqlite3_exec(other, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_GET, "/busy", busy, &f) == 0);
    CHECK(capi_route(app, CAPI_POST, "/", insert, &f) == 0);
    capi_result r = request_text(app, "GET /busy HTTP/1.1\r\nHost: x\r\n\r\n", 503);
    body_equals(&r, "busy"); capi_result_free(&r);
    CHECK(sqlite3_close(other) == SQLITE_OK);
    f.mode = 1;
    const char payload[] = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na\0b";
    r = request_bytes(app, payload, sizeof(payload) - 1, 200); capi_result_free(&r);
    CHECK(sqlite3_close(f.db) == SQLITE_OK);
    CHECK(capi_sqlite_open(path, &f.db) == SQLITE_OK); CHECK(count(f.db) == 1);
    CHECK(capi_route(app, CAPI_GET, "/", query, &f) == 0);
    r = request_text(app, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    const char *body = response_body(&r);
    CHECK(r.len - (size_t)(body - r.data) == 3 && !memcmp(body, "a\0b", 3)); capi_result_free(&r);
    capi_app_free(app); CHECK(sqlite3_close(f.db) == SQLITE_OK); CHECK(unlink(path) == 0);
}
int main(void) {
    lifecycle(); disk_and_locking(); printf("sqlite: %u checks passed\n", checks); return 0;
}
