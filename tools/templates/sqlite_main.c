#include <capi.h>
#include <capi_sqlite.h>
#include "resources.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static capi_server *server;
static void stop(int n) { (void)n; capi_server_stop(server); }
static void hello(capi_ctx *ctx) { capi_json(ctx, 200, "{\"hello\":\"world\"}"); }
static void policy(capi_ctx *ctx) {
    capi_set_header(ctx, "X-Content-Type-Options", "nosniff");
    capi_set_header(ctx, "Cache-Control", "no-store");
    capi_str method = capi_request_method(ctx), origin = capi_header(ctx, "Origin");
    if (!capi_str_eq(method, "GET") && !capi_str_eq(method, "HEAD") &&
        !capi_str_eq(method, "OPTIONS") && origin.data && !capi_str_eq(origin, capi_data(ctx))) {
        capi_json(ctx, 403, "{\"error\":\"Origin does not match this server\"}"); return;
    }
    capi_next(ctx);
}
int main(int argc, char **argv) {
    if (argc > 3) { fputs("usage: app [database] [port]\n", stderr); return 1; }
    unsigned long port = 8080;
    if (argc > 2) {
        char *end; errno = 0; port = strtoul(argv[2], &end, 10);
        if (errno || !*argv[2] || *end || port > 65535) { fputs("invalid port\n", stderr); return 1; }
    }
    sqlite3 *db = NULL;
    capi_app *app = NULL;
    int rc = capi_sqlite_open(argc > 1 ? argv[1] : "app.db", &db);
    if (rc != SQLITE_OK) goto fail;
    rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto fail;
    app = capi_app_new();
    if (!app) goto fail;
    char origin[128];
    if (capi_use(app, "/", policy, origin) ||
        capi_route(app, CAPI_GET, "/", hello, NULL) || app_resources_register(app, db)) goto fail;
    server = capi_server_listen(app, (capi_server_options){.port = (uint16_t)port});
    if (!server) goto fail;
    snprintf(origin, sizeof(origin), "http://127.0.0.1:%u", (unsigned)capi_server_port(server));
    signal(SIGINT, stop); signal(SIGTERM, stop);
    printf("Listening on %s\n", origin); fflush(stdout);
    int result = capi_server_run(server);
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
    capi_server_free(server); capi_app_free(app); sqlite3_close(db);
    return result ? 1 : 0;
fail:
    fprintf(stderr, "capi: %s; SQLite: %s\n", app ? capi_app_error(app) : "setup failed",
            db ? sqlite3_errmsg(db) : "open failed");
    capi_app_free(app); sqlite3_close(db); return 1;
}
