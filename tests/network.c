#define _POSIX_C_SOURCE 200809L
#include "capi.h"
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned checks;
static pid_t child;
static capi_server *server;
static char large_body[256 * 1024];
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "%s:%d: %s (errno %d)\n", __FILE__, __LINE__, #expr, errno); if (child > 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); } exit(1); } } while (0)
static void stop(int n) { (void)n; capi_server_stop(server); }
static void hello(capi_ctx *ctx) { capi_text(ctx, 200, "hello"); }
static void echo(capi_ctx *ctx) { capi_str body = capi_body(ctx); capi_write(ctx, body.data, body.len); }
static void large(capi_ctx *ctx) { capi_write(ctx, large_body, sizeof(large_body)); }
static void shutdown_handler(capi_ctx *ctx) { large(ctx); capi_server_stop(server); }
static void pause_ms(long ms) {
    struct timespec duration = {ms / 1000, ms % 1000 * 1000000};
    while (nanosleep(&duration, &duration) && errno == EINTR) {}
}
static int connect_to(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); CHECK(fd >= 0);
    struct timeval timeout = {3, 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_port = htons(port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}
static void send_bytes(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t n = send(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0); p += n; size -= (size_t)n;
    }
}
static size_t receive_all(int fd, char *out, size_t cap) {
    size_t len = 0;
    for (;;) {
        CHECK(len < cap);
        ssize_t n = recv(fd, out + len, cap - len, 0);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n >= 0);
        if (!n) return len;
        len += (size_t)n;
    }
}
static void receive_response(int fd, const char *body, size_t len) {
    char headers[2048]; size_t used = 0;
    while (used < 4 || memcmp(headers + used - 4, "\r\n\r\n", 4)) {
        CHECK(used < sizeof(headers) - 1);
        CHECK(recv(fd, headers + used, 1, 0) == 1); ++used;
    }
    headers[used] = 0;
    CHECK(strstr(headers, " 200 ") != NULL);
    char *cl = strstr(headers, "Content-Length: "); CHECK(cl);
    CHECK(strtoull(cl + 16, NULL, 10) == len);
    size_t at = 0; char buffer[4096];
    while (at < len) {
        size_t amount = len - at; if (amount > sizeof(buffer)) amount = sizeof(buffer);
        ssize_t n = recv(fd, buffer, amount, 0); CHECK(n > 0);
        CHECK(!memcmp(body + at, buffer, (size_t)n)); at += (size_t)n;
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    memset(large_body, 'z', sizeof(large_body));
    capi_app *app = capi_app_new(); CHECK(app);
    CHECK(capi_route(app, CAPI_GET, "/", hello, NULL) == 0);
    CHECK(capi_route(app, CAPI_POST, "/echo", echo, NULL) == 0);
    CHECK(capi_route(app, CAPI_GET, "/large", large, NULL) == 0);
    CHECK(capi_route(app, CAPI_GET, "/stop", shutdown_handler, NULL) == 0);
    int startup[2]; CHECK(pipe(startup) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        close(startup[0]);
        server = capi_server_listen(app, (capi_server_options){.idle_timeout_ms = 400, .request_timeout_ms = 180,
            .shutdown_timeout_ms = 1000, .max_connections = 64});
        if (!server) fprintf(stderr, "listen: %s\n", capi_app_error(app));
        CHECK(server);
        uint16_t selected = capi_server_port(server);
        CHECK(write(startup[1], &selected, sizeof(selected)) == (ssize_t)sizeof(selected));
        close(startup[1]);
        signal(SIGTERM, stop);
        int result = capi_server_run(server);
        capi_server_free(server); capi_app_free(app); exit(result ? 1 : 0);
    }
    close(startup[1]);
    uint16_t port;
    CHECK(read(startup[0], &port, sizeof(port)) == (ssize_t)sizeof(port));
    close(startup[0]); CHECK(port != 0);
    capi_app_free(app);
    int fd = connect_to(port);
    const char *request = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    for (size_t i = 0; i < strlen(request); ++i) send_bytes(fd, request + i, 1);
    receive_response(fd, "hello", 5);
    /* Repeated keep-alive requests reuse a connection and its buffers. */
    for (int i = 0; i < 100; ++i) { send_bytes(fd, request, strlen(request)); receive_response(fd, "hello", 5); }
    close(fd);

    fd = connect_to(port);
    char pipeline[4096] = "";
    for (int i = 0; i < 30; ++i) strcat(pipeline, request);
    strcat(pipeline, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    send_bytes(fd, pipeline, strlen(pipeline));
    char output[16384]; size_t length = receive_all(fd, output, sizeof(output) - 1); output[length] = 0;
    unsigned responses = 0; char *next = output;
    while ((next = strstr(next, "HTTP/1.1 200"))) { ++responses; ++next; }
    CHECK(responses == 31); close(fd);

    fd = connect_to(port);
    const char binary[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\na\0b\r\n";
    send_bytes(fd, binary, sizeof(binary) - 1); CHECK(shutdown(fd, SHUT_WR) == 0);
    receive_response(fd, "a\0b\r\n", 5); CHECK(recv(fd, output, sizeof(output), 0) == 0); close(fd);

    fd = connect_to(port);
    const char *big_request = "GET /large HTTP/1.1\r\nHost: x\r\n\r\n";
    send_bytes(fd, big_request, strlen(big_request)); receive_response(fd, large_body, sizeof(large_body)); close(fd);

    int clients[32];
    for (size_t i = 0; i < 32; ++i) { clients[i] = connect_to(port); send_bytes(clients[i], request, strlen(request)); }
    for (size_t i = 0; i < 32; ++i) { receive_response(clients[i], "hello", 5); close(clients[i]); }

    fd = connect_to(port); pause_ms(500); CHECK(recv(fd, output, sizeof(output), 0) == 0); close(fd);
    fd = connect_to(port); send_bytes(fd, "GET / ", 6); pause_ms(110); send_bytes(fd, "H", 1);
    pause_ms(120); CHECK(recv(fd, output, sizeof(output), 0) == 0); close(fd);

    fd = connect_to(port);
    const char *malformed = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx";
    send_bytes(fd, malformed, strlen(malformed)); length = receive_all(fd, output, sizeof(output) - 1); output[length] = 0;
    CHECK(strstr(output, " 400 ")); close(fd);

    /* Stop from a handler: pending response must fully drain before exit. */
    fd = connect_to(port);
    const char *shutdown_request = "GET /stop HTTP/1.1\r\nHost: x\r\n\r\n";
    send_bytes(fd, shutdown_request, strlen(shutdown_request)); receive_response(fd, large_body, sizeof(large_body));
    CHECK(recv(fd, output, sizeof(output), 0) == 0); close(fd);
    int status; CHECK(waitpid(child, &status, 0) == child); child = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* A signal wakes an otherwise idle event loop and exits cleanly. */
    CHECK(pipe(startup) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        close(startup[0]);
        app = capi_app_new(); CHECK(app);
        server = capi_server_listen(app, (capi_server_options){0}); CHECK(server);
        signal(SIGTERM, stop);
        uint16_t selected = capi_server_port(server);
        CHECK(write(startup[1], &selected, sizeof(selected)) == (ssize_t)sizeof(selected));
        close(startup[1]);
        int result = capi_server_run(server);
        capi_server_free(server); capi_app_free(app); exit(result ? 1 : 0);
    }
    close(startup[1]); CHECK(read(startup[0], &port, sizeof(port)) == (ssize_t)sizeof(port)); close(startup[0]);
    CHECK(kill(child, SIGTERM) == 0);
    CHECK(waitpid(child, &status, 0) == child); child = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("network (%s): %u checks passed\n", capi_server_backend(), checks); return 0;
}
