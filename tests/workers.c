#define _POSIX_C_SOURCE 200809L
#include "capi_workers.h"
#include "capi_async.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static atomic_uint checks;
static pid_t child;
#define CHECK(expr) do { atomic_fetch_add(&checks, 1); if (!(expr)) { fprintf(stderr, "%s:%d: %s (errno %d)\n", __FILE__, __LINE__, #expr, errno); if (child > 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); } exit(1); } } while (0)
typedef struct {
    pthread_t loop;
    capi_workers *pool;
    capi_server *server;
    atomic_bool release;
    atomic_uint initialized, disposed, accepted, started, finished, done, cleaned;
    unsigned reports, job_reports, timeouts, shutdowns, rejected, errors, late_errors;
    bool seen_ids[8192];
    bool fail_init;
} fixture;
typedef struct { fixture *f; pthread_t thread; } worker_state;
typedef struct { fixture *f; capi_str body; uint64_t checksum, request_id; bool hold, cooperate, fail, accepted; } task;
typedef struct { task *job; capi_ctx *ctx; } cleanup_data;
typedef struct { unsigned initialized, accepted, started, finished, done, cleaned; size_t outstanding, queued, running; } snapshot;

static void pause_ms(long ms) {
    struct timespec delay = {ms / 1000, ms % 1000 * 1000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}
static uint64_t checksum(capi_str body) {
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < body.len; ++i) value = (value ^ (unsigned char)body.data[i]) * UINT64_C(1099511628211);
    return value;
}
static int initialize(void *data, void **state) {
    fixture *f = data;
    worker_state *s = malloc(sizeof(*s)); CHECK(s);
    *s = (worker_state){f, pthread_self()};
    *state = s;
    CHECK(!pthread_equal(f->loop, pthread_self()));
    atomic_fetch_add(&f->initialized, 1);
    return f->fail_init ? -1 : 0;
}
static void dispose(void *data) {
    worker_state *s = data;
    CHECK(pthread_equal(s->thread, pthread_self()));
    atomic_fetch_add(&s->f->disposed, 1);
    free(s);
}
static void work(capi_job *job, void *state, void *data) {
    task *t = data;
    worker_state *s = state;
    CHECK(s && s->f == t->f && pthread_equal(s->thread, pthread_self()));
    CHECK(!pthread_equal(t->f->loop, pthread_self()));
    atomic_fetch_add(&t->f->started, 1);
    while (t->hold && !atomic_load(&t->f->release)) {
        if (t->cooperate && capi_job_cancelled(job)) { pause_ms(100); break; }
        pause_ms(1);
    }
    CHECK(checksum(t->body) == t->checksum); /* even after socket timeout/reuse */
    atomic_fetch_add(&t->f->finished, 1);
}
static void done(capi_ctx *ctx, void *data) {
    task *t = data;
    CHECK(pthread_equal(t->f->loop, pthread_self()));
    CHECK(capi_data(ctx) == t->f);
    CHECK(!capi_async_available(ctx));
    CHECK(capi_request_id(ctx) == t->request_id);
    CHECK(checksum(t->body) == t->checksum);
    atomic_fetch_add(&t->f->done, 1);
    if (t->fail) capi_fail(ctx);
    else capi_write(ctx, t->body.data, t->body.len);
}
static void cleaned(void *data) {
    cleanup_data *cleanup = data;
    task *t = cleanup->job;
    CHECK(pthread_equal(t->f->loop, pthread_self()));
    CHECK(checksum(t->body) == t->checksum);
    if (t->accepted) atomic_fetch_add(&t->f->cleaned, 1);
    if (capi_str_eq(t->body, "queued timeout"))
        capi_fail_with(cleanup->ctx, CAPI_ERROR_APPLICATION, "late_cleanup");
}
static void submit(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    task *t = capi_alloc(ctx, sizeof(*t)); CHECK(t);
    t->f = f; t->body = capi_body(ctx); t->checksum = checksum(t->body); t->accepted = true;
    t->request_id = capi_request_id(ctx);
    t->hold = capi_str_eq(capi_path(ctx), "/hold") || capi_str_eq(capi_path(ctx), "/cooperate");
    t->cooperate = capi_str_eq(capi_path(ctx), "/cooperate");
    t->fail = capi_str_eq(capi_path(ctx), "/fail");
    cleanup_data *cleanup = capi_alloc(ctx, sizeof(*cleanup)); CHECK(cleanup);
    *cleanup = (cleanup_data){t, ctx};
    CHECK(capi_defer(ctx, cleaned, cleanup) == 0);
    capi_work_result rc = capi_work_submit(ctx, f->pool, work, done, t);
    if (rc != CAPI_WORK_OK) { t->accepted = false; capi_text(ctx, 503, "full"); return; }
    atomic_fetch_add(&f->accepted, 1);
    if (capi_str_eq(capi_path(ctx), "/retire")) { capi_workers_free(f->pool); f->pool = NULL; }
}
static void health(capi_ctx *ctx) { capi_text(ctx, 200, "health"); }
static void release_work(capi_ctx *ctx) {
    fixture *f = capi_data(ctx); atomic_store(&f->release, true); capi_text(ctx, 200, "released");
}
static void reset(capi_ctx *ctx) {
    fixture *f = capi_data(ctx); atomic_store(&f->release, false); capi_text(ctx, 200, "reset");
}
static void stats(capi_ctx *ctx) {
    fixture *f = capi_data(ctx);
    capi_workers_stats s = {0};
    if (f->pool) capi_workers_get_stats(f->pool, &s);
    capi_printf(ctx, "%u %u %u %u %u %u %zu %zu %zu", atomic_load(&f->initialized),
        atomic_load(&f->accepted), atomic_load(&f->started), atomic_load(&f->finished),
        atomic_load(&f->done), atomic_load(&f->cleaned), s.outstanding, s.queued, s.running);
}
static void shutdown_server(capi_ctx *ctx) {
    fixture *f = capi_data(ctx); capi_text(ctx, 200, "stopping"); capi_server_stop(f->server);
}
static void request_error(uint64_t id, const capi_error *error, void *data) {
    fixture *f = data;
    CHECK(pthread_equal(f->loop, pthread_self()));
    CHECK(id > 0 && id < sizeof(f->seen_ids));
    if (!strcmp(error->operation, "late_cleanup")) { CHECK(f->seen_ids[id]); ++f->late_errors; }
    else CHECK(!f->seen_ids[id]);
    CHECK(error->code == CAPI_ERROR_APPLICATION);
    ++f->errors;
}
static void request_finished(const capi_request_report *report, void *data) {
    fixture *f = data;
    CHECK(pthread_equal(f->loop, pthread_self()));
    CHECK(report->id > 0 && report->id < sizeof(f->seen_ids) && !f->seen_ids[report->id]);
    f->seen_ids[report->id] = true; ++f->reports;
    CHECK(report->bytes_sent <= report->response_bytes);
    if (report->outcome == CAPI_OUTCOME_SENT) CHECK(report->bytes_sent == report->response_bytes && report->bytes_sent > 0);
    bool job = capi_str_eq(report->path, "/quick") || capi_str_eq(report->path, "/hold") ||
        capi_str_eq(report->path, "/cooperate") || capi_str_eq(report->path, "/fail") || capi_str_eq(report->path, "/retire");
    if (!job) return;
    if (report->status == 503) { ++f->rejected; return; }
    ++f->job_reports;
    if (report->outcome == CAPI_OUTCOME_TIMEOUT || report->outcome == CAPI_OUTCOME_SHUTDOWN) {
        CHECK(report->status == 0 && report->bytes_sent == 0 && report->response_bytes == 0);
        CHECK(report->error.code == CAPI_ERROR_NONE);
        CHECK(atomic_load(&f->cleaned) < atomic_load(&f->accepted));
        if (report->outcome == CAPI_OUTCOME_TIMEOUT) ++f->timeouts;
        else ++f->shutdowns;
    } else {
        CHECK(report->outcome == CAPI_OUTCOME_SENT);
        CHECK(report->status == 200 || report->status == 500);
        if (report->status == 500) CHECK(report->error.code == CAPI_ERROR_APPLICATION);
    }
}

static void constructors_and_dispatch(void) {
    CHECK(capi_workers_new((capi_workers_options){.threads = 257}) == NULL && errno == EINVAL);
    CHECK(capi_workers_new((capi_workers_options){.capacity = 65537}) == NULL && errno == EINVAL);
    fixture failed = {.loop = pthread_self(), .fail_init = true};
    CHECK(capi_workers_new((capi_workers_options){.threads = 3, .init = initialize, .dispose = dispose, .data = &failed}) == NULL);
    CHECK(atomic_load(&failed.initialized) == 3 && atomic_load(&failed.disposed) == 3);
    fixture f = {.loop = pthread_self()};
    f.pool = capi_workers_new((capi_workers_options){.threads = 2, .init = initialize, .dispose = dispose, .data = &f}); CHECK(f.pool);
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_POST, "/", submit, &f) == 0);
    const char request[] = "POST / HTTP/1.1\r\nHost: x\r\n\r\n";
    capi_result r;
    CHECK(capi_dispatch(app, request, sizeof(request) - 1, &r) == 1);
    CHECK(r.len >= 13 && !memcmp(r.data, "HTTP/1.1 503 ", 13)); capi_result_free(&r);
    capi_workers_stats s; capi_workers_get_stats(f.pool, &s); CHECK(s.outstanding == 0);
    capi_workers_free(f.pool); CHECK(atomic_load(&f.disposed) == 2); capi_app_free(app);
}

static uint16_t start_server(bool stress) {
    int startup[2]; CHECK(pipe(startup) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        close(startup[0]);
        fixture f = {.loop = pthread_self()};
        f.pool = capi_workers_new((capi_workers_options){.threads = stress ? 4U : 1U, .capacity = stress ? 128U : 2U,
            .init = initialize, .dispose = dispose, .data = &f}); CHECK(f.pool);
        capi_app *app = capi_app_new(); CHECK(app);
        capi_on_error(app, request_error, &f); capi_on_finish(app, request_finished, &f);
        const char *routes[] = {"/quick", "/hold", "/cooperate", "/fail", "/retire"};
        for (size_t i = 0; i < 5; ++i) CHECK(capi_route(app, CAPI_POST, routes[i], submit, &f) == 0);
        CHECK(capi_route(app, CAPI_GET, "/health", health, &f) == 0);
        CHECK(capi_route(app, CAPI_GET, "/release", release_work, &f) == 0);
        CHECK(capi_route(app, CAPI_GET, "/reset", reset, &f) == 0);
        CHECK(capi_route(app, CAPI_GET, "/stats", stats, &f) == 0);
        CHECK(capi_route(app, CAPI_GET, "/shutdown", shutdown_server, &f) == 0);
        f.server = capi_server_listen(app, (capi_server_options){.max_connections = stress ? 128U : 8U,
            .async_timeout_ms = stress ? 5000U : 1000U, .idle_timeout_ms = 10000, .shutdown_timeout_ms = 20}); CHECK(f.server);
        uint16_t port = capi_server_port(f.server);
        CHECK(write(startup[1], &port, sizeof(port)) == (ssize_t)sizeof(port)); close(startup[1]);
        CHECK(capi_server_run(f.server) == 0);
        CHECK(atomic_load(&f.accepted) == atomic_load(&f.cleaned));
        CHECK(f.job_reports == atomic_load(&f.accepted));
        CHECK(f.reports > f.job_reports);
        if (!stress) CHECK(f.timeouts == 2 && f.shutdowns == 2 && f.errors == 2 && f.late_errors == 1 && f.rejected == 2);
        if (f.pool) { capi_workers_stats s; capi_workers_get_stats(f.pool, &s); CHECK(s.outstanding == 0); }
        capi_server_free(f.server); capi_workers_free(f.pool); capi_app_free(app);
        CHECK(atomic_load(&f.initialized) == atomic_load(&f.disposed)); exit(0);
    }
    close(startup[1]); uint16_t port;
    CHECK(read(startup[0], &port, sizeof(port)) == (ssize_t)sizeof(port)); close(startup[0]); return port;
}
static int connect_to(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); CHECK(fd >= 0);
    struct timeval timeout = {10, 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_port = htons(port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0); return fd;
}
static void send_bytes(int fd, const char *bytes, size_t len) {
    while (len) {
        ssize_t n = send(fd, bytes, len, 0);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0); bytes += n; len -= (size_t)n;
    }
}
static void receive_response(int fd, int status, char *body, size_t capacity, size_t *length) {
    char headers[2048]; size_t used = 0;
    while (used < 4 || memcmp(headers + used - 4, "\r\n\r\n", 4)) {
        CHECK(used < sizeof(headers) - 1); CHECK(recv(fd, headers + used, 1, 0) == 1); ++used;
    }
    headers[used] = 0;
    char expected[32]; snprintf(expected, sizeof(expected), "HTTP/1.1 %d ", status);
    CHECK(!strncmp(headers, expected, strlen(expected)));
    char *cl = strstr(headers, "Content-Length: "); CHECK(cl);
    size_t len = (size_t)strtoull(cl + 16, NULL, 10); CHECK(len < capacity);
    size_t at = 0;
    while (at < len) { ssize_t n = recv(fd, body + at, len - at, 0); CHECK(n > 0); at += (size_t)n; }
    body[len] = 0; if (length) *length = len;
}
static int post(uint16_t port, const char *path, const char *body, size_t len) {
    int fd = connect_to(port); char headers[256];
    int n = snprintf(headers, sizeof(headers), "POST %s HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n", path, len);
    CHECK(n > 0 && (size_t)n < sizeof(headers)); send_bytes(fd, headers, (size_t)n); send_bytes(fd, body, len); return fd;
}
static void expect(int fd, int status, const char *expected) {
    char body[1024]; receive_response(fd, status, body, sizeof(body), NULL); CHECK(!strcmp(body, expected));
}
static void get(uint16_t port, const char *path, char *body, size_t capacity) {
    int fd = connect_to(port); char request[256];
    int n = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
    CHECK(n > 0 && (size_t)n < sizeof(request)); send_bytes(fd, request, (size_t)n);
    receive_response(fd, 200, body, capacity, NULL); close(fd);
}
static snapshot read_stats(uint16_t port) {
    char body[1024]; get(port, "/stats", body, sizeof(body)); snapshot s;
    CHECK(sscanf(body, "%u %u %u %u %u %u %zu %zu %zu", &s.initialized, &s.accepted, &s.started,
        &s.finished, &s.done, &s.cleaned, &s.outstanding, &s.queued, &s.running) == 9); return s;
}
static snapshot wait_for(uint16_t port, unsigned started, size_t outstanding) {
    for (int i = 0; i < 3000; ++i) {
        snapshot s = read_stats(port);
        if (s.started >= started && s.outstanding == outstanding) return s;
        pause_ms(1);
    }
    CHECK(0); return (snapshot){0};
}
static void wait_child(void) {
    int status; CHECK(waitpid(child, &status, 0) == child); child = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void lifetimes(void) {
    uint16_t port = start_server(false);
    char response[1024];
    int first = post(port, "/hold", "first", 5);
    wait_for(port, 1, 1);
    int second = post(port, "/hold", "second", 6);
    snapshot s = wait_for(port, 1, 2); CHECK(s.queued == 1 && s.running == 1);
    int rejected = post(port, "/quick", "full", 4); expect(rejected, 503, "full"); close(rejected);
    get(port, "/health", response, sizeof(response)); CHECK(!strcmp(response, "health"));
    get(port, "/release", response, sizeof(response));
    expect(first, 200, "first"); expect(second, 200, "second"); close(first); close(second);
    s = wait_for(port, 2, 0); CHECK(s.done == 2 && s.cleaned == 2);

    get(port, "/reset", response, sizeof(response));
    first = post(port, "/hold", "pipe", 4);
    const char next[] = "GET /health HTTP/1.1\r\nHost: x\r\n\r\n"; send_bytes(first, next, sizeof(next) - 1);
    wait_for(port, 3, 1); get(port, "/health", response, sizeof(response)); CHECK(!strcmp(response, "health"));
    get(port, "/release", response, sizeof(response)); expect(first, 200, "pipe"); expect(first, 200, "health"); close(first);
    wait_for(port, 3, 0);

    first = post(port, "/quick", "a\0b\r\n", 5); CHECK(shutdown(first, SHUT_WR) == 0);
    size_t length; receive_response(first, 200, response, sizeof(response), &length);
    CHECK(length == 5 && !memcmp(response, "a\0b\r\n", 5)); CHECK(recv(first, response, sizeof(response), 0) == 0); close(first);
    first = post(port, "/fail", "failure", 7); expect(first, 500, "Internal Server Error\n"); close(first);
    s = wait_for(port, 5, 0); unsigned previous_done = s.done, previous_cleaned = s.cleaned, previous_started = s.started;

    get(port, "/reset", response, sizeof(response));
    first = post(port, "/hold", "pinned after remote close", 25); wait_for(port, previous_started + 1, 1); close(first);
    second = post(port, "/hold", "queued timeout", 14); wait_for(port, previous_started + 1, 2);
    CHECK(recv(second, response, sizeof(response), 0) == 0); close(second);
    s = read_stats(port); CHECK(s.outstanding == 2 && s.cleaned == previous_cleaned);
    for (int i = 0; i < 32; ++i) { get(port, "/health", response, sizeof(response)); CHECK(!strcmp(response, "health")); }
    rejected = post(port, "/quick", "still bounded", 13); expect(rejected, 503, "full"); close(rejected);
    get(port, "/release", response, sizeof(response));
    s = wait_for(port, previous_started + 1, 0);
    CHECK(s.done == previous_done && s.cleaned == previous_cleaned + 2);
    CHECK(s.started == previous_started + 1); /* queued cancelled job never ran */

    get(port, "/reset", response, sizeof(response));
    first = post(port, "/cooperate", "shutdown", 8); wait_for(port, s.started + 1, 1);
    second = post(port, "/hold", "skip on stop", 12); wait_for(port, s.started + 1, 2);
    get(port, "/shutdown", response, sizeof(response)); CHECK(!strcmp(response, "stopping"));
    CHECK(recv(first, response, sizeof(response), 0) == 0); CHECK(recv(second, response, sizeof(response), 0) == 0);
    close(first); close(second); wait_child();
}

static void concurrent_producers(void) {
    uint16_t port = start_server(true);
    for (int round = 0; round < 10; ++round) {
        int clients[64]; char bodies[64][32];
        for (size_t i = 0; i < 64; ++i) {
            snprintf(bodies[i], sizeof(bodies[i]), "stress-%d-%zu", round, i);
            clients[i] = post(port, "/quick", bodies[i], strlen(bodies[i]));
        }
        for (size_t i = 0; i < 64; ++i) { expect(clients[i], 200, bodies[i]); close(clients[i]); }
    }
    snapshot s = wait_for(port, 640, 0); CHECK(s.done == 640 && s.cleaned == 640 && s.initialized == 4);
    /* Release the pool owner while this request's completion is still posted. */
    int fd = post(port, "/retire", "retired", 7); expect(fd, 200, "retired"); close(fd);
    char body[1024]; get(port, "/health", body, sizeof(body)); CHECK(!strcmp(body, "health"));
    get(port, "/shutdown", body, sizeof(body)); wait_child();
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    constructors_and_dispatch(); lifetimes(); concurrent_producers();
    printf("workers (%s): %u parent checks passed\n", capi_server_backend(), atomic_load(&checks)); return 0;
}
