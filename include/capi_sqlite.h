#ifndef CAPI_SQLITE_H
#define CAPI_SQLITE_H
#include "capi.h"
#include <sqlite3.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One connection per event-loop thread. Foreign keys enabled, busy timeout 0,
 * extended result codes enabled. No implicit WAL/durability changes.
 * As with sqlite3_open_v2, close *db even if open returns an error. */
int capi_sqlite_open(const char *path, sqlite3 **db);

/* Exactly one nonempty SQL statement; trailing whitespace is allowed.
 * Automatically finalized after this request's pipeline. Never finalize it
 * yourself; sqlite3_reset is available to release a cursor early. */
int capi_sqlite_prepare(capi_ctx *ctx, sqlite3 *db, const char *sql, sqlite3_stmt **stmt);
/* Values are copied with SQLITE_TRANSIENT. Empty values stay empty, not NULL. */
int capi_sqlite_bind_text(sqlite3_stmt *stmt, int index, capi_str value);
int capi_sqlite_bind_blob(sqlite3_stmt *stmt, int index, capi_str value);
/* Request-owned copy, stable across step/reset. SQL NULL -> {NULL, 0}. */
int capi_sqlite_copy_text(capi_ctx *ctx, sqlite3_stmt *stmt, int column, capi_str *value);

typedef struct capi_sqlite_tx capi_sqlite_tx;
typedef enum { CAPI_SQLITE_DEFERRED, CAPI_SQLITE_IMMEDIATE } capi_sqlite_tx_mode;
/* Transactions do not nest. Begin registers rollback on request cleanup;
 * only an explicit, successful commit persists writes. Do not mix native SQL
 * transaction control with a managed transaction. tx is request-owned. */
int capi_sqlite_begin(capi_ctx *ctx, sqlite3 *db, capi_sqlite_tx_mode mode, capi_sqlite_tx **tx);
int capi_sqlite_commit(capi_sqlite_tx *tx);
int capi_sqlite_rollback(capi_sqlite_tx *tx);

#ifdef __cplusplus
}
#endif
#endif
