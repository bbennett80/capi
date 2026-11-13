#include "users.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static capi_server *server;
static void stop(int n) { (void)n; capi_server_stop(server); }
static void health(capi_ctx *ctx) { capi_text(ctx, 200, "healthy\n"); }
int main(int argc, char **argv) {
    if (argc > 2) { fputs("usage: capi_routers [port]\n", stderr); return 1; }
    unsigned long port = 8080;
    if (argc == 2) {
        char *end; errno = 0; port = strtoul(argv[1], &end, 10);
        if (errno || !*argv[1] || *end || port > 65535) return 1;
    }
    capi_app *app = capi_app_new();
    capi_router *users = users_router_new("users"), *api = capi_router_new();
    if (!app || !users || !api) goto fail;
    if (capi_router_include(api, "/users", users)) {
        fprintf(stderr, "router: %s\n", capi_router_error(api)); goto fail;
    }
    if (capi_include_router(app, "/api/v1", api) ||
        capi_include_router(app, "/api/latest", api) ||
        capi_route(app, CAPI_GET, "/health", health, NULL)) {
        fprintf(stderr, "app: %s\n", capi_app_error(app)); goto fail;
    }
    capi_router_free(users); users = NULL;
    capi_router_free(api); api = NULL;
    server = capi_server_listen(app, (capi_server_options){.port = (uint16_t)port});
    if (!server) { fprintf(stderr, "%s\n", capi_app_error(app)); goto fail; }
    signal(SIGINT, stop); signal(SIGTERM, stop);
    printf("Router example: http://127.0.0.1:%u\n", (unsigned)capi_server_port(server)); fflush(stdout);
    int rc = capi_server_run(server);
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
    capi_server_free(server); capi_app_free(app); return rc ? 1 : 0;
fail:
    capi_router_free(users); capi_router_free(api); capi_app_free(app); return 1;
}
