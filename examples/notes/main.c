#include "app.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef CAPI_NOTES_VIEW_DIR
#define CAPI_NOTES_VIEW_DIR "examples/notes/views"
#endif

static capi_server *server;
static void stop(int n) { (void)n; capi_server_stop(server); }

int main(int argc, char **argv) {
    if (argc > 4) { fputs("usage: capi_notes [database] [port] [view-directory]\n", stderr); return 1; }
    unsigned long port = 8080;
    if (argc > 2) {
        char *end; errno = 0; port = strtoul(argv[2], &end, 10);
        if (errno || !*argv[2] || *end || port > 65535) { fputs("invalid port\n", stderr); return 1; }
    }
    char error[256];
    notes_app *notes = notes_open(argc > 1 ? argv[1] : "notes.db", argc > 3 ? argv[3] : CAPI_NOTES_VIEW_DIR, error, sizeof(error));
    if (!notes) { fprintf(stderr, "notes: %s\n", error); return 1; }
    server = capi_server_listen(notes_http(notes), (capi_server_options){.port = (uint16_t)port});
    if (!server) { fprintf(stderr, "notes: %s\n", capi_app_error(notes_http(notes))); notes_close(notes); return 1; }
    char origin[128]; snprintf(origin, sizeof(origin), "http://127.0.0.1:%u", (unsigned)capi_server_port(server));
    notes_origin(notes, origin);
    signal(SIGINT, stop); signal(SIGTERM, stop);
    printf("Fieldnotes · %s · %s\n", origin, capi_server_backend()); fflush(stdout);
    int result = capi_server_run(server);
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
    capi_server_free(server); notes_close(notes); return result ? 1 : 0;
}
