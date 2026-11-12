#define _POSIX_C_SOURCE 200809L
/* Reproducible HTTP workloads; deliberately no access logging. */
#include "capi.h"
#include "capi_sqlite.h"
#include "capi_template.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static capi_server *server;
static sqlite3 *db;
static capi_template *view;
static char bulk[16384];
static void stop(int sig) { (void)sig; capi_server_stop(server); }
static void text(capi_ctx *ctx) { capi_text(ctx, 200, "Hello from capi!\n"); }
static void json(capi_ctx *ctx) {
    capi_json(ctx, 200, "{\"framework\":\"capi\",\"version\":\"" CAPI_VERSION "\",\"ready\":true,\"language\":\"C11\"}");
}
static void payload(capi_ctx *ctx) {
    capi_set_header(ctx, "Content-Type", "application/octet-stream");
    capi_write(ctx, bulk, sizeof(bulk));
}
static void middleware(capi_ctx *ctx) {
    capi_set_header(ctx, "X-Content-Type-Options", "nosniff");
    capi_next(ctx);
}
static void routed(capi_ctx *ctx) {
    int64_t id;
    capi_str name;
    if (capi_param_i64(ctx, "id", 1, INT64_MAX, &id) != CAPI_INPUT_FOUND ||
        capi_query(ctx, "name", &name) != CAPI_INPUT_FOUND) {
        capi_text(ctx, 400, "Invalid input"); return;
    }
    capi_json(ctx, 200, "{\"name\":");
    capi_json_string(ctx, name);
    capi_printf(ctx, ",\"id\":%lld}", (long long)id);
}
static void templates(capi_ctx *ctx) {
    capi_field fields[10][2];
    capi_value rows[10];
    for (int i = 0; i < 10; ++i) {
        fields[i][0] = (capi_field){"id", capi_value_int(i + 1)};
        fields[i][1] = (capi_field){"title", capi_value_text("A note with <tags> & \"quotes\"")};
        rows[i] = capi_value_object(fields[i], 2);
    }
    capi_field root = {"notes", capi_value_list(rows, 10)};
    capi_value model = capi_value_object(&root, 1);
    capi_render(ctx, 200, view, &model);
}
static void database(capi_ctx *ctx) {
    sqlite3_stmt *stmt;
    if (capi_sqlite_prepare(ctx, db, "SELECT id,title FROM notes WHERE id >= ? ORDER BY id LIMIT 10", &stmt) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 1, 42) != SQLITE_OK) { capi_fail_with(ctx, CAPI_ERROR_DATABASE, "prepare"); return; }
    capi_json(ctx, 200, "[");
    int rc, count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        capi_str title;
        if (capi_sqlite_copy_text(ctx, stmt, 1, &title) != SQLITE_OK) {
            capi_fail_with(ctx, CAPI_ERROR_DATABASE, "copy_title"); return;
        }
        capi_printf(ctx, "%s{\"id\":%lld,\"title\":", count++ ? "," : "", (long long)sqlite3_column_int64(stmt, 0));
        capi_json_string(ctx, title);
        capi_write(ctx, "}", 1);
    }
    if (rc != SQLITE_DONE || count != 10) capi_fail_with(ctx, CAPI_ERROR_DATABASE, "read_notes");
    capi_write(ctx, "]", 1);
}
int main(void) {
    memset(bulk, 'x', sizeof(bulk));
    const char *source = "<!doctype html><html><body><h1>Notes</h1><ul>{{#notes}}<li data-id=\"{{id}}\">{{title}}</li>{{/notes}}</ul></body></html>";
    capi_template_error error;
    view = capi_template_compile(source, strlen(source), &error);
    if (!view || capi_sqlite_open(":memory:", &db) != SQLITE_OK) return 1;
    if (sqlite3_exec(db, "CREATE TABLE notes(id INTEGER PRIMARY KEY,title TEXT NOT NULL);"
        "WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<1000)"
        " INSERT INTO notes SELECT i,'A note with <tags> & quotes' FROM n;", NULL, NULL, NULL) != SQLITE_OK) return 1;
    capi_app *app = capi_app_new();
    if (!app || capi_route(app, CAPI_GET, "/text", text, NULL) ||
        capi_route(app, CAPI_GET, "/json", json, NULL) ||
        capi_route(app, CAPI_GET, "/bulk", payload, NULL) ||
        capi_use(app, "/users", middleware, NULL) ||
        capi_route(app, CAPI_GET, "/users/:id", routed, NULL) ||
        capi_route(app, CAPI_GET, "/template", templates, NULL) ||
        capi_route(app, CAPI_GET, "/sqlite", database, NULL)) return 1;
    server = capi_server_listen(app, (capi_server_options){.port = 0});
    if (!server) return 1;
    signal(SIGINT, stop); signal(SIGTERM, stop);
    printf("%u %s %s\n", (unsigned)capi_server_port(server), capi_server_backend(), sqlite3_libversion()); fflush(stdout);
    int result = capi_server_run(server);
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
    capi_server_free(server); capi_app_free(app);
    sqlite3_close(db); capi_template_free(view);
    return result ? 1 : 0;
}
