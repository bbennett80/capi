#include "capi.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static capi_server *server;
static void stop(int signal_number) { (void)signal_number; capi_server_stop(server); }
static void hello(capi_ctx *ctx) { capi_text(ctx, 200, "Hello from capi!\n"); }

int main(int argc, char **argv) {
    unsigned long port = 8080;
    if (argc > 1) {
        char *end;
        port = strtoul(argv[1], &end, 10);
        if (!*argv[1] || *end || port > 65535) { fputs("usage: capi_hello [port]\n", stderr); return 1; }
    }
    capi_app *app = capi_app_new();
    if (!app) return 1;
    if (capi_route(app, CAPI_GET, "/", hello, NULL)) goto fail;
    server = capi_server_listen(app, (capi_server_options){.port = (uint16_t)port});
    if (!server) goto fail;
    signal(SIGINT, stop); signal(SIGTERM, stop);
    printf("capi %s · http://127.0.0.1:%u · %s\n", CAPI_VERSION,
           (unsigned)capi_server_port(server), capi_server_backend());
    fflush(stdout);
    int result = capi_server_run(server);
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
    capi_server_free(server); capi_app_free(app);
    return result ? 1 : 0;
fail:
    fprintf(stderr, "capi: %s\n", capi_app_error(app)); capi_app_free(app); return 1;
}
