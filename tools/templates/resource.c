/* Generated once by capi. Edit this controller freely. */
#include "@RESOURCE@.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef enum { STRING, TEXT, INTEGER, BOOLEAN } field_kind;
static const struct { const char *name; field_kind kind; size_t maximum; } fields[] = {
@FIELDS@
};
enum { FIELD_COUNT = sizeof(fields) / sizeof(fields[0]) };
typedef struct { bool present; capi_str text; int64_t integer; } input;

/* Bootstrap only: this does not migrate existing tables. */
static const char schema[] = @SCHEMA@;
static const char select_one[] = @SELECT_ONE@;
static const char select_list[] = @SELECT_LIST@;
static const char insert_sql[] = @INSERT@;
static const char update_sql[] = @UPDATE@;
static const char delete_sql[] = @DELETE@;

static void problem(capi_ctx *ctx, int status, const char *message) {
    capi_json(ctx, status, "{\"error\":");
    capi_json_string(ctx, (capi_str){message, strlen(message)});
    capi_write(ctx, "}", 1);
}
static void database_error(capi_ctx *ctx, sqlite3 *db, int rc) {
    if ((rc & 255) == SQLITE_BUSY || (rc & 255) == SQLITE_LOCKED) {
        capi_set_header(ctx, "Retry-After", "1"); problem(ctx, 503, "Database busy");
    } else if ((rc & 255) == SQLITE_CONSTRAINT) problem(ctx, 409, "Database constraint conflict");
    else {
        fprintf(stderr, "@RESOURCE@: SQLite %d: %s\n", rc, sqlite3_errmsg(db));
        capi_fail_with(ctx, CAPI_ERROR_DATABASE, "@RESOURCE@_database");
    }
}
static bool valid_text(capi_str s) {
    size_t i = 0;
    while (i < s.len) {
        unsigned char c = (unsigned char)s.data[i++];
        if (c < 128) {
            if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return false;
            continue;
        }
        unsigned remaining, code, minimum;
        if (c >= 0xc2 && c <= 0xdf) { remaining = 1; code = c & 31U; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { remaining = 2; code = c & 15U; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; code = c & 7U; minimum = 0x10000; }
        else return false;
        if (s.len - i < remaining) return false;
        while (remaining--) {
            c = (unsigned char)s.data[i++];
            if ((c & 0xc0U) != 0x80U) return false;
            code = (code << 6) | (c & 63U);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
static bool read_input(capi_ctx *ctx, input values[FIELD_COUNT], bool partial) {
    bool any = false;
    for (int i = 0; i < FIELD_COUNT; ++i) {
        capi_str raw;
        capi_input_result result = capi_form(ctx, fields[i].name, &raw);
        if (result == CAPI_INPUT_MISSING && partial) continue;
        if (result == CAPI_INPUT_CONTENT_TYPE) { problem(ctx, 415, "Use application/x-www-form-urlencoded"); return false; }
        if (result == CAPI_INPUT_ENCODING) { problem(ctx, 400, "Malformed form encoding"); return false; }
        if (result == CAPI_INPUT_NO_MEMORY) return false;
        bool valid = result == CAPI_INPUT_FOUND;
        if (valid && (fields[i].kind == STRING || fields[i].kind == TEXT)) {
            valid = raw.len <= fields[i].maximum && (fields[i].kind != STRING || raw.len > 0) && valid_text(raw);
            values[i].text = raw;
        } else if (valid && fields[i].kind == INTEGER) {
            valid = capi_str_i64(raw, INT64_MIN, INT64_MAX, &values[i].integer) == CAPI_INPUT_FOUND;
        } else if (valid) {
            if (capi_str_eq(raw, "true") || capi_str_eq(raw, "1")) values[i].integer = 1;
            else if (capi_str_eq(raw, "false") || capi_str_eq(raw, "0")) values[i].integer = 0;
            else valid = false;
        }
        if (!valid) {
            capi_json(ctx, 422, "{\"error\":\"Invalid or missing field\",\"field\":");
            capi_json_string(ctx, (capi_str){fields[i].name, strlen(fields[i].name)});
            capi_write(ctx, "}", 1); return false;
        }
        values[i].present = true; any = true;
    }
    if (!any) problem(ctx, 422, "Supply at least one known field");
    return any;
}
static int bind_value(sqlite3_stmt *stmt, int parameter, const input *value, int field) {
    if (fields[field].kind == STRING || fields[field].kind == TEXT)
        return capi_sqlite_bind_text(stmt, parameter, value->text);
    return sqlite3_bind_int64(stmt, parameter, value->integer);
}
static bool row_json(capi_ctx *ctx, sqlite3_stmt *stmt) {
    if (capi_printf(ctx, "{\"id\":%" PRId64, (int64_t)sqlite3_column_int64(stmt, 0))) return false;
    for (int i = 0; i < FIELD_COUNT; ++i) {
        if (capi_printf(ctx, ",\"%s\":", fields[i].name)) return false;
        if (fields[i].kind == STRING || fields[i].kind == TEXT) {
            /* Borrow only until the next step/reset; serialize immediately. */
            const char *text = (const char *)sqlite3_column_text(stmt, i + 1);
            if (!text) {
                capi_fail_with(ctx, sqlite3_errcode(sqlite3_db_handle(stmt)) == SQLITE_NOMEM ?
                    CAPI_ERROR_OUT_OF_MEMORY : CAPI_ERROR_DATABASE, "column_text"); return false;
            }
            capi_str value = {text, (size_t)sqlite3_column_bytes(stmt, i + 1)};
            if (capi_json_string(ctx, value)) return false;
        } else if (fields[i].kind == BOOLEAN) {
            const char *value = sqlite3_column_int(stmt, i + 1) ? "true" : "false";
            if (capi_write(ctx, value, strlen(value))) return false;
        } else if (capi_printf(ctx, "%" PRId64, (int64_t)sqlite3_column_int64(stmt, i + 1))) return false;
    }
    return capi_write(ctx, "}", 1) == 0;
}
static bool read_id(capi_ctx *ctx, int64_t *id) {
    if (capi_param_i64(ctx, "id", 1, INT64_MAX, id) == CAPI_INPUT_FOUND) return true;
    problem(ctx, 400, "Invalid ID"); return false;
}
static bool respond_one(capi_ctx *ctx, sqlite3 *db, int64_t id, int status) {
    sqlite3_stmt *stmt;
    int rc = capi_sqlite_prepare(ctx, db, select_one, &stmt);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) { problem(ctx, 404, "Resource not found"); return false; }
    if (rc != SQLITE_ROW) { database_error(ctx, db, rc); return false; }
    if (capi_json(ctx, status, "") || !row_json(ctx, stmt)) return false;
    rc = sqlite3_reset(stmt); /* Release the cursor before a possible commit. */
    if (rc != SQLITE_OK) { database_error(ctx, db, rc); return false; }
    return true;
}
static void index_action(capi_ctx *ctx) {
    sqlite3 *db = capi_data(ctx);
    int64_t limit = 20, offset = 0, value;
    capi_input_result result = capi_query_i64(ctx, "limit", 1, 100, &value);
    if (result == CAPI_INPUT_FOUND) limit = value;
    else if (result != CAPI_INPUT_MISSING) { problem(ctx, 400, "Invalid limit (1-100)"); return; }
    result = capi_query_i64(ctx, "offset", 0, INT64_MAX, &value);
    if (result == CAPI_INPUT_FOUND) offset = value;
    else if (result != CAPI_INPUT_MISSING) { problem(ctx, 400, "Invalid offset"); return; }
    sqlite3_stmt *stmt;
    int rc = capi_sqlite_prepare(ctx, db, select_list, &stmt);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, limit);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 2, offset);
    if (rc != SQLITE_OK) { database_error(ctx, db, rc); return; }
    if (capi_json(ctx, 200, "[")) return;
    bool comma = false;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (comma && capi_write(ctx, ",", 1)) return;
        if (!row_json(ctx, stmt)) return;
        comma = true;
    }
    if (rc != SQLITE_DONE) { database_error(ctx, db, rc); return; }
    capi_write(ctx, "]", 1);
}
static void show_action(capi_ctx *ctx) {
    int64_t id;
    if (read_id(ctx, &id)) respond_one(ctx, capi_data(ctx), id, 200);
}
static void create_action(capi_ctx *ctx) {
    input values[FIELD_COUNT] = {0};
    if (!read_input(ctx, values, false)) return;
    sqlite3 *db = capi_data(ctx);
    capi_sqlite_tx *tx;
    int rc = capi_sqlite_begin(ctx, db, CAPI_SQLITE_IMMEDIATE, &tx);
    if (rc != SQLITE_OK) { database_error(ctx, db, rc); return; }
    sqlite3_stmt *stmt;
    rc = capi_sqlite_prepare(ctx, db, insert_sql, &stmt);
    for (int i = 0; rc == SQLITE_OK && i < FIELD_COUNT; ++i) rc = bind_value(stmt, i + 1, &values[i], i);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) { database_error(ctx, db, rc); return; }
    int64_t id = (int64_t)sqlite3_last_insert_rowid(db);
    char location[96]; snprintf(location, sizeof(location), "/@RESOURCE@/%" PRId64, id);
    if (capi_set_header(ctx, "Location", location) || !respond_one(ctx, db, id, 201)) return;
    rc = capi_sqlite_commit(tx);
    if (rc != SQLITE_OK) database_error(ctx, db, rc);
}
static void update_action(capi_ctx *ctx) {
    int64_t id;
    if (!read_id(ctx, &id)) return;
    input values[FIELD_COUNT] = {0};
    if (!read_input(ctx, values, capi_str_eq(capi_request_method(ctx), "PATCH"))) return;
    sqlite3 *db = capi_data(ctx);
    capi_sqlite_tx *tx;
    int rc = capi_sqlite_begin(ctx, db, CAPI_SQLITE_IMMEDIATE, &tx);
    if (rc != SQLITE_OK) { database_error(ctx, db, rc); return; }
    sqlite3_stmt *stmt;
    rc = capi_sqlite_prepare(ctx, db, update_sql, &stmt);
    for (int i = 0; rc == SQLITE_OK && i < FIELD_COUNT; ++i) {
        rc = sqlite3_bind_int(stmt, i * 2 + 1, values[i].present);
        if (rc == SQLITE_OK) rc = bind_value(stmt, i * 2 + 2, &values[i], i);
    }
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, FIELD_COUNT * 2 + 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) { database_error(ctx, db, rc); return; }
    if (!sqlite3_changes(db)) { problem(ctx, 404, "Resource not found"); return; }
    if (!respond_one(ctx, db, id, 200)) return;
    rc = capi_sqlite_commit(tx);
    if (rc != SQLITE_OK) database_error(ctx, db, rc);
}
static void destroy_action(capi_ctx *ctx) {
    int64_t id;
    if (!read_id(ctx, &id)) return;
    sqlite3 *db = capi_data(ctx);
    capi_sqlite_tx *tx;
    int rc = capi_sqlite_begin(ctx, db, CAPI_SQLITE_IMMEDIATE, &tx);
    if (rc != SQLITE_OK) { database_error(ctx, db, rc); return; }
    sqlite3_stmt *stmt;
    rc = capi_sqlite_prepare(ctx, db, delete_sql, &stmt);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) { database_error(ctx, db, rc); return; }
    if (!sqlite3_changes(db)) { problem(ctx, 404, "Resource not found"); return; }
    rc = capi_sqlite_commit(tx);
    if (rc != SQLITE_OK) { database_error(ctx, db, rc); return; }
    capi_status(ctx, 204);
}
int resource_@RESOURCE@_register(capi_app *app, sqlite3 *db) {
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { fprintf(stderr, "@RESOURCE@ schema: %s\n", sqlite3_errmsg(db)); return -1; }
    /* Fail setup if an existing table lacks expected columns. */
    sqlite3_stmt *check = NULL;
    rc = sqlite3_prepare_v2(db, "PRAGMA table_info(\"@RESOURCE@\")", -1, &check, NULL);
    bool found[FIELD_COUNT + 1] = {0};
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(check)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(check, 1);
        const char *type = (const char *)sqlite3_column_text(check, 2);
        if (!name || !type) { rc = SQLITE_NOMEM; break; }
        if (!strcmp(name, "id")) found[0] = !sqlite3_stricmp(type, "INTEGER") && sqlite3_column_int(check, 5) == 1;
        for (int i = 0; i < FIELD_COUNT; ++i) if (!strcmp(name, fields[i].name)) {
            bool text = fields[i].kind == STRING || fields[i].kind == TEXT;
            found[i + 1] = !sqlite3_stricmp(type, text ? "TEXT" : "INTEGER") && sqlite3_column_int(check, 3) == 1;
        }
    }
    sqlite3_finalize(check);
    bool valid = rc == SQLITE_DONE;
    for (int i = 0; i <= FIELD_COUNT; ++i) valid = valid && found[i];
    if (!valid) { fputs("@RESOURCE@: schema mismatch or schema inspection failed\n", stderr); return -1; }
    return capi_route(app, CAPI_GET, "/@RESOURCE@", index_action, db) ||
        capi_route(app, CAPI_GET, "/@RESOURCE@/:id", show_action, db) ||
        capi_route(app, CAPI_POST, "/@RESOURCE@", create_action, db) ||
        capi_route(app, CAPI_PUT, "/@RESOURCE@/:id", update_action, db) ||
        capi_route(app, CAPI_PATCH, "/@RESOURCE@/:id", update_action, db) ||
        capi_route(app, CAPI_DELETE, "/@RESOURCE@/:id", destroy_action, db) ? -1 : 0;
}
