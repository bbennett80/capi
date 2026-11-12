#include "capi_sqlite.h"
#include "capi_workers.h"
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static capi_server *server;
static void stop(int n) { (void)n; capi_server_stop(server); }
typedef struct { bool insert; capi_str body; int rc; int64_t value; char error[128]; } query;

static int open_worker(void *path, void **state) {
    sqlite3 *db = NULL;
    int rc = capi_sqlite_open(path, &db);
    *state = db;
    return rc == SQLITE_OK ? 0 : -1;
}
static void close_worker(void *state) { (void)sqlite3_close(state); }

static void execute(capi_job *job, void *state, void *data) {
    sqlite3 *db = state;
    query *q = data;
    if (capi_job_cancelled(job)) return;
    sqlite3_stmt *stmt = NULL;
    q->rc = sqlite3_prepare_v2(db, q->insert ? "INSERT INTO messages(body) VALUES(?1)" : "SELECT count(*) FROM messages", -1, &stmt, NULL);
    if (q->rc == SQLITE_OK && q->insert) q->rc = capi_sqlite_bind_blob(stmt, 1, q->body);
    if (q->rc == SQLITE_OK) {
        q->rc = sqlite3_step(stmt);
        if (q->insert && q->rc == SQLITE_DONE) { q->value = sqlite3_last_insert_rowid(db); q->rc = SQLITE_OK; }
        else if (!q->insert && q->rc == SQLITE_ROW) { q->value = sqlite3_column_int64(stmt, 0); q->rc = SQLITE_OK; }
    }
    if (q->rc != SQLITE_OK) snprintf(q->error, sizeof(q->error), "%s", sqlite3_errmsg(db));
    int finalized = sqlite3_finalize(stmt);
    if (q->rc == SQLITE_OK) q->rc = finalized;
    /* Only plain copied results leave this worker. Never return a SQLite view. */
}
static void respond(capi_ctx *ctx, void *data) {
    query *q = data;
    if ((q->rc & 0xff) == SQLITE_BUSY || (q->rc & 0xff) == SQLITE_LOCKED) {
        capi_set_header(ctx, "Retry-After", "1");
        capi_json(ctx, 503, "{\"error\":\"database busy\"}"); return;
    }
    if (q->rc != SQLITE_OK) {
        fprintf(stderr, "database worker: %d %s\n", q->rc, q->error);
        capi_json(ctx, 500, "{\"error\":\"database operation failed\"}"); return;
    }
    capi_json(ctx, q->insert ? 201 : 200, "");
    capi_printf(ctx, "{\"%s\":%" PRId64 "}", q->insert ? "id" : "messages", q->value);
}
static void messages(capi_ctx *ctx) {
    query *q = capi_alloc(ctx, sizeof(*q));
    if (!q) return;
    q->insert = capi_str_eq(capi_request_method(ctx), "POST");
    q->body = capi_body(ctx);
    if (capi_work_submit(ctx, capi_data(ctx), execute, respond, q) != CAPI_WORK_OK) {
        capi_set_header(ctx, "Retry-After", "1");
        capi_json(ctx, 503, "{\"error\":\"work queue unavailable\"}");
    }
}
static void health(capi_ctx *ctx) { capi_json(ctx, 200, "{\"status\":\"ready\"}"); }

int main(int argc, char **argv) {
    if (argc > 3) { fputs("usage: capi_async_sqlite [database] [port]\n", stderr); return 1; }
    const char *path = argc > 1 ? argv[1] : "messages.db";
    if (!*path || !strcmp(path, ":memory:")) { fputs("Use a file database; workers open independent connections.\n", stderr); return 1; }
    unsigned long port = 8080;
    if (argc > 2) {
        char *end; errno = 0; port = strtoul(argv[2], &end, 10);
        if (errno || !*argv[2] || *end || port > 65535) { fputs("invalid port\n", stderr); return 1; }
    }
    sqlite3 *db = NULL;
    int rc = capi_sqlite_open(path, &db);
    if (rc == SQLITE_OK) rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS messages(id INTEGER PRIMARY KEY, body BLOB NOT NULL)", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { fprintf(stderr, "database: %s\n", db ? sqlite3_errmsg(db) : "open failed"); sqlite3_close(db); return 1; }
    sqlite3_close(db);
    capi_workers *pool = capi_workers_new((capi_workers_options){.threads = 4, .capacity = 128,
        .init = open_worker, .dispose = close_worker, .data = (void *)path});
    if (!pool) { perror("workers"); return 1; }
    capi_app *app = capi_app_new();
    if (!app) { capi_workers_free(pool); return 1; }
    if (capi_route(app, CAPI_GET, "/health", health, NULL) ||
        capi_route(app, CAPI_GET, "/messages", messages, pool) ||
        capi_route(app, CAPI_POST, "/messages", messages, pool) ||
        capi_app_limits(app, (capi_limits){16384, 16384, 1024 * 1024, 0})) goto fail;
    server = capi_server_listen(app, (capi_server_options){.port = (uint16_t)port});
    if (!server) goto fail;
    signal(SIGINT, stop); signal(SIGTERM, stop);
    printf("SQLite workers · http://127.0.0.1:%u\nGET /messages counts messages; POST /messages saves a body; GET /health stays on the event loop.\n", (unsigned)capi_server_port(server));
    fflush(stdout);
    rc = capi_server_run(server);
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
    capi_server_free(server); capi_workers_free(pool); capi_app_free(app);
    return rc ? 1 : 0;
fail:
    fprintf(stderr, "capi: %s\n", capi_app_error(app));
    capi_workers_free(pool); capi_app_free(app); return 1;
}
