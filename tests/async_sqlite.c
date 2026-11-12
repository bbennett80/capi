#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
static pid_t child;
static FILE *output;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "%s:%d: %s (errno %d)\n", __FILE__, __LINE__, #expr, errno); if (child > 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); } exit(1); } } while (0)
static unsigned short start(const char *program, const char *path) {
    int startup[2]; CHECK(pipe(startup) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        close(startup[0]);
        if (dup2(startup[1], STDOUT_FILENO) < 0) _exit(1);
        close(startup[1]);
        execl(program, program, path, "0", (char *)NULL); _exit(1);
    }
    close(startup[1]); output = fdopen(startup[0], "r"); CHECK(output);
    char line[512]; CHECK(fgets(line, sizeof(line), output));
    char *colon = strrchr(line, ':'); CHECK(colon);
    char *end; unsigned long port = strtoul(colon + 1, &end, 10);
    CHECK(port > 0 && port <= 65535 && *end == '\n');
    return (unsigned short)port;
}
static void stop(void) {
    CHECK(kill(child, SIGTERM) == 0);
    int status; CHECK(waitpid(child, &status, 0) == child); child = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0); fclose(output); output = NULL;
}
static void request(unsigned short port, const char *method, const char *path, const char *body,
                     size_t len, int status, const char *expected) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); CHECK(fd >= 0);
    struct timeval timeout = {5, 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_port = htons(port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    char wire[1024];
    int n = snprintf(wire, sizeof(wire), "%s %s HTTP/1.1\r\nHost: test\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n", method, path, len);
    CHECK(n > 0 && (size_t)n + len < sizeof(wire));
    if (len) memcpy(wire + n, body, len);
    size_t size = (size_t)n + len, sent = 0;
    while (sent < size) { ssize_t bytes = send(fd, wire + sent, size - sent, 0); CHECK(bytes > 0); sent += (size_t)bytes; }
    size_t used = 0;
    for (;;) {
        CHECK(used < sizeof(wire) - 1);
        ssize_t bytes = recv(fd, wire + used, sizeof(wire) - 1 - used, 0); CHECK(bytes >= 0);
        if (!bytes) break;
        used += (size_t)bytes;
    }
    close(fd); wire[used] = 0;
    char prefix[32]; snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", status);
    if (strncmp(wire, prefix, strlen(prefix))) fprintf(stderr, "%s\n", wire);
    CHECK(!strncmp(wire, prefix, strlen(prefix)));
    char *payload = strstr(wire, "\r\n\r\n"); CHECK(payload); payload += 4;
    CHECK(!strcmp(payload, expected));
    if (!strcmp(method, "HEAD")) CHECK(strstr(wire, "Content-Length: 14\r\n"));
}
int main(int argc, char **argv) {
    CHECK(argc == 2); signal(SIGPIPE, SIG_IGN);
    char path[] = "/tmp/capi-async-sqlite-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
    unsigned short port = start(argv[1], path);
    request(port, "GET", "/messages", NULL, 0, 200, "{\"messages\":0}");
    for (unsigned i = 1; i <= 8; ++i) {
        char expected[64]; snprintf(expected, sizeof(expected), "{\"id\":%u}", i);
        request(port, "POST", "/messages", "a\0'); DROP TABLE messages;--", 28, 201, expected);
        request(port, "GET", "/health", NULL, 0, 200, "{\"status\":\"ready\"}");
    }
    request(port, "GET", "/messages", NULL, 0, 200, "{\"messages\":8}");
    request(port, "HEAD", "/messages", NULL, 0, 200, "");
    stop();
    port = start(argv[1], path);
    request(port, "GET", "/messages", NULL, 0, 200, "{\"messages\":8}");
    stop(); CHECK(unlink(path) == 0);
    printf("async SQLite: %u checks passed\n", checks); return 0;
}
