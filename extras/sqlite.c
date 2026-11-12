#include "capi_sqlite.h"
#include <limits.h>
#include <string.h>

struct capi_sqlite_tx { sqlite3 *db; capi_ctx *ctx; bool active; };

int capi_sqlite_open(const char *path, sqlite3 **db) {
    if (!db) return SQLITE_MISUSE;
    *db = NULL;
    if (!path) return SQLITE_MISUSE;
    int rc = sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_extended_result_codes(*db, 1);
    if (rc == SQLITE_OK) rc = sqlite3_busy_timeout(*db, 0);
    if (rc == SQLITE_OK) rc = sqlite3_exec(*db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    return rc;
}

static void finalize(void *stmt) { (void)sqlite3_finalize(stmt); }

int capi_sqlite_prepare(capi_ctx *ctx, sqlite3 *db, const char *sql, sqlite3_stmt **stmt) {
    if (!stmt) return SQLITE_MISUSE;
    *stmt = NULL;
    if (!ctx || !db || !sql) return SQLITE_MISUSE;
    const char *tail = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, stmt, &tail);
    if (rc != SQLITE_OK) return rc;
    if (!*stmt) return SQLITE_MISUSE;
    while (*tail == ' ' || *tail == '\t' || *tail == '\n' || *tail == '\r' || *tail == '\f' || *tail == '\v') ++tail;
    if (*tail) { sqlite3_finalize(*stmt); *stmt = NULL; return SQLITE_MISUSE; }
    if (capi_defer(ctx, finalize, *stmt)) { sqlite3_finalize(*stmt); *stmt = NULL; return SQLITE_NOMEM; }
    return SQLITE_OK;
}

int capi_sqlite_bind_text(sqlite3_stmt *stmt, int index, capi_str value) {
    if (!stmt || (value.len && !value.data)) return SQLITE_MISUSE;
    if (value.len > INT_MAX) return SQLITE_TOOBIG;
    return sqlite3_bind_text(stmt, index, value.data ? value.data : "", (int)value.len, SQLITE_TRANSIENT);
}

int capi_sqlite_bind_blob(sqlite3_stmt *stmt, int index, capi_str value) {
    if (!stmt || (value.len && !value.data)) return SQLITE_MISUSE;
    if (value.len > INT_MAX) return SQLITE_TOOBIG;
    return sqlite3_bind_blob(stmt, index, value.data ? value.data : "", (int)value.len, SQLITE_TRANSIENT);
}

int capi_sqlite_copy_text(capi_ctx *ctx, sqlite3_stmt *stmt, int column, capi_str *value) {
    if (!value) return SQLITE_MISUSE;
    *value = (capi_str){NULL, 0};
    if (!ctx || !stmt || !sqlite3_data_count(stmt) || column < 0 || column >= sqlite3_column_count(stmt)) return SQLITE_MISUSE;
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) return SQLITE_OK;
    const unsigned char *text = sqlite3_column_text(stmt, column);
    if (!text) return SQLITE_NOMEM;
    int size = sqlite3_column_bytes(stmt, column);
    char *copy = capi_alloc(ctx, (size_t)size + 1);
    if (!copy) return SQLITE_NOMEM;
    memcpy(copy, text, (size_t)size);
    *value = (capi_str){copy, (size_t)size};
    return SQLITE_OK;
}

static void rollback(void *data) {
    capi_sqlite_tx *tx = data;
    if (tx->active && !sqlite3_get_autocommit(tx->db) &&
        sqlite3_exec(tx->db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK)
        capi_fail_with(tx->ctx, CAPI_ERROR_DATABASE, "capi_sqlite_rollback");
    tx->active = false;
}

int capi_sqlite_begin(capi_ctx *ctx, sqlite3 *db, capi_sqlite_tx_mode mode, capi_sqlite_tx **tx) {
    if (!tx) return SQLITE_MISUSE;
    *tx = NULL;
    if (!ctx || !db || (mode != CAPI_SQLITE_DEFERRED && mode != CAPI_SQLITE_IMMEDIATE) || !sqlite3_get_autocommit(db))
        return SQLITE_MISUSE;
    capi_sqlite_tx *transaction = capi_alloc(ctx, sizeof(*transaction));
    if (!transaction || capi_defer(ctx, rollback, transaction)) return SQLITE_NOMEM;
    transaction->db = db;
    transaction->ctx = ctx;
    int rc = sqlite3_exec(db, mode == CAPI_SQLITE_IMMEDIATE ? "BEGIN IMMEDIATE" : "BEGIN DEFERRED", NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    transaction->active = true;
    *tx = transaction;
    return SQLITE_OK;
}

static int finish(capi_sqlite_tx *tx, const char *sql) {
    if (!tx || !tx->active || sqlite3_get_autocommit(tx->db)) return SQLITE_MISUSE;
    int rc = sqlite3_exec(tx->db, sql, NULL, NULL, NULL);
    if (rc == SQLITE_OK) tx->active = false;
    return rc;
}
int capi_sqlite_commit(capi_sqlite_tx *tx) { return finish(tx, "COMMIT"); }
int capi_sqlite_rollback(capi_sqlite_tx *tx) { return finish(tx, "ROLLBACK"); }
