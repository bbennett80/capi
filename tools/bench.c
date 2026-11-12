#define _POSIX_C_SOURCE 200809L
/* A deliberately small HTTP/1.1 loopback load generator, written in C.
 * One in-flight request per connection; reports end-to-end latency. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
typedef struct { int fd; size_t sent, used, expected; uint64_t started; char buffer[65536]; } client;
static uint64_t microseconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
static int compare(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static unsigned long number(const char *s, unsigned long max) {
    char *end; errno = 0; unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || !n || n > max) return 0;
    return n;
}
int main(int argc, char **argv) {
    if (argc != 5) {
        fputs("usage: capi_bench <IPv4 address> <port> <connections> <requests>\n", stderr); return 1;
    }
    unsigned long port = number(argv[2], 65535), concurrency = number(argv[3], 4096), total = number(argv[4], 10000000);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_port = htons((uint16_t)port);
    if (!port || !concurrency || !total || concurrency > total || inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) {
        fputs("invalid address or numeric arguments\n", stderr); return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    client *clients = calloc(concurrency, sizeof(*clients));
    struct pollfd *fds = calloc(concurrency, sizeof(*fds));
    uint64_t *latency = calloc(total, sizeof(*latency));
    if (!clients || !fds || !latency) { free(clients); free(fds); free(latency); return 1; }
    for (unsigned long i = 0; i < concurrency; ++i) clients[i].fd = -1;
    int result = 1;
    for (unsigned long i = 0; i < concurrency; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
        if (fd < 0) { perror("socket"); goto done; }
        clients[i].fd = fd;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (connect(fd, (struct sockaddr *)&address, sizeof(address))) { perror("connect"); goto done; }
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) { perror("nonblocking"); goto done; }
        fds[i] = (struct pollfd){fd, POLLOUT, 0};
    }
    unsigned long started = 0, completed = 0;
    uint64_t begin = microseconds();
    while (completed < total) {
        int ready = poll(fds, (nfds_t)concurrency, 5000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { fputs("load generator stalled\n", stderr); goto done; }
        for (unsigned long i = 0; i < concurrency; ++i) {
            client *c = &clients[i];
            short event = fds[i].revents;
            if (event & (POLLERR | POLLNVAL)) { fputs("connection error\n", stderr); goto done; }
            if ((event & POLLOUT) && started < total) {
                if (!c->started) { c->started = microseconds(); ++started; }
                ssize_t n = send(c->fd, request + c->sent, sizeof(request) - 1 - c->sent, 0);
                if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue; perror("send"); goto done; }
                c->sent += (size_t)n;
                if (c->sent == sizeof(request) - 1) fds[i].events = POLLIN;
            } else if ((event & POLLOUT) && c->started) {
                /* Finish a partial write even after the final request was started. */
                ssize_t n = send(c->fd, request + c->sent, sizeof(request) - 1 - c->sent, 0);
                if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue; perror("send"); goto done; }
                c->sent += (size_t)n;
                if (c->sent == sizeof(request) - 1) fds[i].events = POLLIN;
            } else if ((event & POLLOUT) && started == total) fds[i].events = 0;
            if (event & (POLLIN | POLLHUP)) {
                ssize_t n = recv(c->fd, c->buffer + c->used, sizeof(c->buffer) - c->used - 1, 0);
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
                if (n <= 0) { fputs("server closed a connection\n", stderr); goto done; }
                c->used += (size_t)n; c->buffer[c->used] = 0;
                if (!c->expected) {
                    char *end = strstr(c->buffer, "\r\n\r\n");
                    if (end) {
                        char *length = strstr(c->buffer, "Content-Length: ");
                        if (strncmp(c->buffer, "HTTP/1.1 200 ", 13) || !length || length > end) {
                            fputs("expected 200 response with Content-Length\n", stderr); goto done;
                        }
                        unsigned long long body = strtoull(length + 16, NULL, 10);
                        size_t head = (size_t)(end + 4 - c->buffer);
                        if (body > sizeof(c->buffer) - 1 - head) { fputs("response too large (limit 64 KiB)\n", stderr); goto done; }
                        c->expected = head + (size_t)body;
                    }
                }
                if (c->expected && c->used == c->expected) {
                    latency[completed++] = microseconds() - c->started;
                    c->started = 0; c->sent = c->used = c->expected = 0;
                    fds[i].events = started < total ? POLLOUT : 0;
                } else if (c->used == sizeof(c->buffer) - 1) { fputs("response too large\n", stderr); goto done; }
            }
        }
    }
    double seconds = (double)(microseconds() - begin) / 1000000.0;
    qsort(latency, total, sizeof(*latency), compare);
    printf("%lu requests · %lu connections · %.3f s\n", total, concurrency, seconds);
    printf("%.0f requests/s\n", (double)total / seconds);
    printf("latency: p50 %.3f ms · p95 %.3f ms · p99 %.3f ms\n",
           (double)latency[total / 2] / 1000.0, (double)latency[(total - 1) * 95 / 100] / 1000.0,
           (double)latency[(total - 1) * 99 / 100] / 1000.0);
    result = 0;
done:
    for (unsigned long i = 0; i < concurrency; ++i) if (clients[i].fd >= 0) close(clients[i].fd);
    free(clients); free(fds); free(latency); return result;
}
