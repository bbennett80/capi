#define _POSIX_C_SOURCE 200809L
#include "capi_workers.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static capi_server *server;
static void stop(int n) { (void)n; capi_server_stop(server); }
typedef struct { capi_str body; } job_data;

static void work(capi_job *job, void *worker_state, void *data) {
    (void)worker_state; (void)data;
    /* Artificial slow work for the demo: health requests keep responding. */
    for (int i = 0; i < 20 && !capi_job_cancelled(job); ++i) {
        struct timespec delay = {0, 10000000};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
    }
}
static void done(capi_ctx *ctx, void *data) {
    job_data *job = data;
    capi_set_header(ctx, "Content-Type", "application/octet-stream");
    capi_write(ctx, job->body.data, job->body.len);
}
static void slow_echo(capi_ctx *ctx) {
    job_data *job = capi_alloc(ctx, sizeof(*job));
    if (!job) return;
    job->body = capi_body(ctx); /* borrowed view stays alive across the handoff */
    capi_work_result rc = capi_work_submit(ctx, capi_data(ctx), work, done, job);
    if (rc != CAPI_WORK_OK) {
        capi_set_header(ctx, "Retry-After", "1");
        capi_text(ctx, 503, "Work queue unavailable; try again.\n");
    }
}
static void health(capi_ctx *ctx) { capi_json(ctx, 200, "{\"status\":\"ready\"}"); }

int main(int argc, char **argv) {
    unsigned long port = 8080;
    if (argc > 2) { fputs("usage: capi_background [port]\n", stderr); return 1; }
    if (argc > 1) {
        char *end; errno = 0; port = strtoul(argv[1], &end, 10);
        if (errno || !*argv[1] || *end || port > 65535) { fputs("invalid port\n", stderr); return 1; }
    }
    capi_workers *pool = capi_workers_new((capi_workers_options){.threads = 2, .capacity = 16});
    if (!pool) { perror("workers"); return 1; }
    capi_app *app = capi_app_new();
    if (!app) { capi_workers_free(pool); return 1; }
    if (capi_route(app, CAPI_GET, "/health", health, NULL) ||
        capi_route(app, CAPI_POST, "/slow", slow_echo, pool)) goto fail;
    server = capi_server_listen(app, (capi_server_options){.port = (uint16_t)port});
    if (!server) goto fail;
    signal(SIGINT, stop); signal(SIGTERM, stop);
    printf("Background work · http://127.0.0.1:%u\nPOST /slow echoes after 200 ms; GET /health responds immediately.\n", (unsigned)capi_server_port(server));
    fflush(stdout);
    int rc = capi_server_run(server);
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
    capi_server_free(server); capi_workers_free(pool); capi_app_free(app);
    return rc ? 1 : 0;
fail:
    fprintf(stderr, "capi: %s\n", capi_app_error(app));
    capi_workers_free(pool); capi_app_free(app); return 1;
}
