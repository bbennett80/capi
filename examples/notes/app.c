#include "app.h"
#include "capi_sqlite.h"
#include "capi_template.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct notes_app {
    capi_app *http;
    sqlite3 *db;
    capi_template *index, *card;
    char origin[128];
};

static void database_error(capi_ctx *ctx, notes_app *notes, int rc) {
    if ((rc & 0xff) == SQLITE_BUSY || (rc & 0xff) == SQLITE_LOCKED) {
        capi_set_header(ctx, "Retry-After", "1");
        capi_text(ctx, 503, "Database busy; try again.\n");
    } else {
        fprintf(stderr, "notes: SQLite %d: %s\n", rc, sqlite3_errmsg(notes->db));
        capi_text(ctx, 500, "Could not complete the database operation.\n");
    }
}

static void request_policy(capi_ctx *ctx) {
    notes_app *notes = capi_data(ctx);
    capi_set_header(ctx, "X-Content-Type-Options", "nosniff");
    capi_set_header(ctx, "Cache-Control", "no-store");
    capi_set_header(ctx, "Referrer-Policy", "same-origin");
    capi_set_header(ctx, "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
    if (capi_str_eq(capi_request_method(ctx), "POST") && !capi_str_eq(capi_header(ctx, "Origin"), notes->origin)) {
        capi_text(ctx, 403, "A matching Origin header is required.\n"); return;
    }
    capi_next(ctx);
}

static void index_page(capi_ctx *ctx) {
    notes_app *notes = capi_data(ctx);
    sqlite3_stmt *stmt;
    int rc = capi_sqlite_prepare(ctx, notes->db, "SELECT id, title, body, created_at FROM notes ORDER BY id DESC LIMIT 100", &stmt);
    if (rc != SQLITE_OK) { database_error(ctx, notes, rc); return; }
    capi_value *rows = capi_alloc(ctx, 100 * sizeof(*rows));
    capi_field *fields = capi_alloc(ctx, 100 * 4 * sizeof(*fields));
    if (!rows || !fields) return;
    size_t count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        capi_str title, body, date;
        if ((rc = capi_sqlite_copy_text(ctx, stmt, 1, &title)) != SQLITE_OK ||
            (rc = capi_sqlite_copy_text(ctx, stmt, 2, &body)) != SQLITE_OK ||
            (rc = capi_sqlite_copy_text(ctx, stmt, 3, &date)) != SQLITE_OK) {
            database_error(ctx, notes, rc); return;
        }
        capi_field *row = fields + count * 4;
        row[0] = (capi_field){"id", capi_value_int(sqlite3_column_int64(stmt, 0))};
        row[1] = (capi_field){"title", capi_value_string(title)};
        row[2] = (capi_field){"body", capi_value_string(body)};
        row[3] = (capi_field){"date", capi_value_string(date)};
        rows[count++] = capi_value_object(row, 4);
    }
    if (rc != SQLITE_DONE) { database_error(ctx, notes, rc); return; }
    capi_field root[] = {{"notes", capi_value_list(rows, count)}, {"count", capi_value_int((int64_t)count)}};
    capi_value data = capi_value_object(root, 2);
    capi_partial partials[] = {{"card", notes->card}};
    capi_render_with(ctx, 200, notes->index, &data, partials, 1);
}

static bool valid_text(capi_str text, size_t maximum, bool required) {
    if (text.len > maximum || (required && !text.len)) return false;
    for (size_t i = 0; i < text.len; ++i) {
        unsigned char c = (unsigned char)text.data[i];
        if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return false;
    }
    return true;
}

static void create_note(capi_ctx *ctx) {
    notes_app *notes = capi_data(ctx);
    capi_str title, body;
    if (capi_form(ctx, "title", &title) != 1 || capi_form(ctx, "body", &body) < 0) {
        capi_text(ctx, 400, "Expected a URL-encoded title and body.\n"); return;
    }
    while (title.len && (title.data[0] == ' ' || title.data[0] == '\t' || title.data[0] == '\n' || title.data[0] == '\r')) { ++title.data; --title.len; }
    while (title.len && (title.data[title.len - 1] == ' ' || title.data[title.len - 1] == '\t' || title.data[title.len - 1] == '\n' || title.data[title.len - 1] == '\r')) --title.len;
    if (!valid_text(title, 120, true) || !valid_text(body, 4000, false)) {
        capi_text(ctx, 422, "Use a title of 1-120 bytes and a body of at most 4000 bytes, without control characters.\n"); return;
    }
    capi_sqlite_tx *tx;
    int rc = capi_sqlite_begin(ctx, notes->db, CAPI_SQLITE_IMMEDIATE, &tx);
    if (rc != SQLITE_OK) { database_error(ctx, notes, rc); return; }
    sqlite3_stmt *stmt;
    rc = capi_sqlite_prepare(ctx, notes->db, "INSERT INTO notes(title, body) VALUES(?1, ?2)", &stmt);
    if (rc == SQLITE_OK) rc = capi_sqlite_bind_text(stmt, 1, title);
    if (rc == SQLITE_OK) rc = capi_sqlite_bind_text(stmt, 2, body);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) rc = capi_sqlite_commit(tx);
    if (rc != SQLITE_OK) { database_error(ctx, notes, rc); return; }
    capi_redirect(ctx, 303, "/");
}

static void delete_note(capi_ctx *ctx) {
    notes_app *notes = capi_data(ctx);
    capi_str param = capi_param(ctx, "id");
    int64_t id = 0;
    for (size_t i = 0; i < param.len; ++i) {
        unsigned char c = (unsigned char)param.data[i];
        if (c < '0' || c > '9' || id > (INT64_MAX - (c - '0')) / 10) { capi_text(ctx, 400, "Invalid note ID.\n"); return; }
        id = id * 10 + (c - '0');
    }
    if (!id) { capi_text(ctx, 400, "Invalid note ID.\n"); return; }
    capi_sqlite_tx *tx;
    int rc = capi_sqlite_begin(ctx, notes->db, CAPI_SQLITE_IMMEDIATE, &tx);
    if (rc != SQLITE_OK) { database_error(ctx, notes, rc); return; }
    sqlite3_stmt *stmt;
    rc = capi_sqlite_prepare(ctx, notes->db, "DELETE FROM notes WHERE id=?1", &stmt);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) { database_error(ctx, notes, rc); return; }
    if (!sqlite3_changes(notes->db)) { capi_text(ctx, 404, "Note not found.\n"); return; }
    rc = capi_sqlite_commit(tx);
    if (rc != SQLITE_OK) { database_error(ctx, notes, rc); return; }
    capi_redirect(ctx, 303, "/");
}

int notes_origin(notes_app *notes, const char *origin) {
    if (!notes || !origin || !*origin || strlen(origin) >= sizeof(notes->origin)) return -1;
    snprintf(notes->origin, sizeof(notes->origin), "%s", origin); return 0;
}
capi_app *notes_http(notes_app *notes) { return notes ? notes->http : NULL; }
void notes_close(notes_app *notes) {
    if (!notes) return;
    capi_app_free(notes->http); capi_template_free(notes->index); capi_template_free(notes->card);
    if (notes->db) sqlite3_close(notes->db);
    free(notes);
}

notes_app *notes_open(const char *database, const char *view_directory, char *error, size_t capacity) {
    notes_app *notes = calloc(1, sizeof(*notes));
    if (!notes) { snprintf(error, capacity, "out of memory"); return NULL; }
    if (!database || !view_directory) { snprintf(error, capacity, "database and view directory required"); goto fail; }
    int rc = capi_sqlite_open(database, &notes->db);
    if (rc == SQLITE_OK) rc = sqlite3_exec(notes->db,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS notes("
        "id INTEGER PRIMARY KEY, title TEXT NOT NULL CHECK(length(title) BETWEEN 1 AND 120),"
        "body TEXT NOT NULL CHECK(length(body)<=4000),"
        "created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%d %H:%M','now')))", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { snprintf(error, capacity, "SQLite: %s", notes->db ? sqlite3_errmsg(notes->db) : "open failed"); goto fail; }
    const char *files[] = {"index.html", "card.html"};
    capi_template **views[] = {&notes->index, &notes->card};
    for (size_t i = 0; i < 2; ++i) {
        char path[4096]; capi_template_error detail;
        int n = snprintf(path, sizeof(path), "%s/%s", view_directory, files[i]);
        if (n < 0 || (size_t)n >= sizeof(path)) { snprintf(error, capacity, "view path too long"); goto fail; }
        *views[i] = capi_template_load(path, &detail);
        if (!*views[i]) { snprintf(error, capacity, "%s at byte %zu: %s", files[i], detail.offset, detail.message); goto fail; }
    }
    notes->http = capi_app_new();
    if (!notes->http) { snprintf(error, capacity, "out of memory"); goto fail; }
    notes_origin(notes, "http://127.0.0.1:8080");
    if (capi_app_limits(notes->http, (capi_limits){16384, 32768, 4 * 1024 * 1024}) ||
        capi_use(notes->http, "/", request_policy, notes) ||
        capi_route(notes->http, CAPI_GET, "/", index_page, notes) ||
        capi_route(notes->http, CAPI_POST, "/notes", create_note, notes) ||
        capi_route(notes->http, CAPI_POST, "/notes/:id/delete", delete_note, notes)) {
        snprintf(error, capacity, "%s", capi_app_error(notes->http)); goto fail;
    }
    return notes;
fail:
    notes_close(notes); return NULL;
}
