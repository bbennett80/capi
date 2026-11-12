#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) && !defined(CAPI_FORCE_POLL)
#include <sys/epoll.h>
#define CP_EPOLL 1
#elif (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)) && !defined(CAPI_FORCE_POLL)
#include <sys/event.h>
#define CP_KQUEUE 1
#else
#include <poll.h>
#define CP_POLL 1
#endif

#define CP_LISTENER UINT64_MAX
#define CP_WAKE (UINT64_MAX - 1)
#define CP_BATCH 128
#define CP_BUDGET (64 * 1024)

typedef struct {
    int fd;
    uint32_t generation;
    bool writing, closing, peer_eof;
    cp_buf input, body, wire;
    size_t sent, consumed;
    uint64_t activity, started;
} cp_connection;
typedef struct { uint64_t key; bool read, write, error; } cp_event;

struct capi_server {
    capi_app *app;
    capi_server_options options;
    int listener, wake[2], reactor;
    uint16_t port;
    volatile sig_atomic_t stop;
    bool running;
    cp_connection *connections;
    size_t active, cursor;
#ifdef CP_POLL
    struct pollfd *pollfds;
    uint64_t *pollkeys;
#endif
};

uint64_t cp_now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

const char *capi_server_backend(void) {
#ifdef CP_EPOLL
    return "epoll";
#elif defined(CP_KQUEUE)
    return "kqueue";
#else
    return "poll";
#endif
}

static int fd_prepare(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    flags = fcntl(fd, F_GETFD, 0);
    return flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ? -1 : 0;
}

static uint64_t key_for(capi_server *s, cp_connection *c) {
    return ((uint64_t)c->generation << 32) | (uint32_t)(c - s->connections);
}

static int watch(capi_server *s, int fd, uint64_t key, bool writing, bool add) {
#ifdef CP_EPOLL
    struct epoll_event event = {0};
    event.events = writing ? EPOLLOUT : EPOLLIN;
    event.data.u64 = key;
    return epoll_ctl(s->reactor, add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &event);
#elif defined(CP_KQUEUE)
    struct kevent changes[2];
    (void)add;
    EV_SET(&changes[0], (uintptr_t)fd, EVFILT_READ, EV_ADD | (writing ? EV_DISABLE : EV_ENABLE), 0, 0, (void *)(uintptr_t)key);
    EV_SET(&changes[1], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | (writing ? EV_ENABLE : EV_DISABLE), 0, 0, (void *)(uintptr_t)key);
    return kevent(s->reactor, changes, 2, NULL, 0, NULL);
#else
    (void)s; (void)fd; (void)key; (void)writing; (void)add;
    return 0;
#endif
}

static void unwatch(capi_server *s, int fd) {
#ifdef CP_EPOLL
    (void)epoll_ctl(s->reactor, EPOLL_CTL_DEL, fd, NULL);
#else
    (void)s; (void)fd; /* kqueue removes closed descriptors automatically. */
#endif
}

static int wait_events(capi_server *s, cp_event *events, int timeout) {
#ifdef CP_EPOLL
    struct epoll_event ready[CP_BATCH];
    int count = epoll_wait(s->reactor, ready, CP_BATCH, timeout);
    for (int i = 0; i < count; ++i) {
        events[i] = (cp_event){ready[i].data.u64,
            (ready[i].events & (EPOLLIN | EPOLLHUP)) != 0,
            (ready[i].events & EPOLLOUT) != 0, (ready[i].events & EPOLLERR) != 0};
    }
    return count;
#elif defined(CP_KQUEUE)
    struct kevent ready[CP_BATCH];
    struct timespec duration = {timeout / 1000, (timeout % 1000) * 1000000L};
    int count = kevent(s->reactor, NULL, 0, ready, CP_BATCH, &duration);
    for (int i = 0; i < count; ++i)
        events[i] = (cp_event){(uint64_t)(uintptr_t)ready[i].udata,
            ready[i].filter == EVFILT_READ, ready[i].filter == EVFILT_WRITE,
            (ready[i].flags & EV_ERROR) != 0};
    return count;
#else
    size_t n = 0;
    if (s->listener >= 0) {
        s->pollfds[n] = (struct pollfd){s->listener, POLLIN, 0}; s->pollkeys[n++] = CP_LISTENER;
    }
    s->pollfds[n] = (struct pollfd){s->wake[0], POLLIN, 0}; s->pollkeys[n++] = CP_WAKE;
    for (size_t i = 0; i < s->options.max_connections; ++i) {
        cp_connection *c = &s->connections[i];
        if (c->fd < 0) continue;
        s->pollfds[n] = (struct pollfd){c->fd, c->writing ? POLLOUT : POLLIN, 0};
        s->pollkeys[n++] = key_for(s, c);
    }
    int result = poll(s->pollfds, (nfds_t)n, timeout);
    if (result <= 0) return result;
    int count = 0;
    for (size_t i = 0; i < n && count < CP_BATCH; ++i) {
        short re = s->pollfds[i].revents;
        if (!re) continue;
        events[count++] = (cp_event){s->pollkeys[i], (re & (POLLIN | POLLHUP)) != 0,
            (re & POLLOUT) != 0, (re & (POLLERR | POLLNVAL)) != 0};
    }
    return count;
#endif
}

static void connection_close(capi_server *s, cp_connection *c) {
    if (c->fd < 0) return;
    unwatch(s, c->fd);
    close(c->fd);
    c->fd = -1;
    free(c->input.data); free(c->body.data); free(c->wire.data);
    c->input = (cp_buf){0}; c->body = (cp_buf){0}; c->wire = (cp_buf){0};
    --s->active;
}

static int server_error(capi_server *s, const char *operation) {
    snprintf(s->app->error, sizeof(s->app->error), "%s: %s", operation, strerror(errno));
    return -1;
}

capi_server *capi_server_listen(capi_app *app, capi_server_options options) {
    if (!app) { errno = EINVAL; return NULL; }
    if (!options.max_connections) options.max_connections = 4096;
    if (!options.idle_timeout_ms) options.idle_timeout_ms = 15000;
    if (!options.request_timeout_ms) options.request_timeout_ms = 30000;
    if (!options.shutdown_timeout_ms) options.shutdown_timeout_ms = 5000;
    if (options.max_connections > 65536) {
        snprintf(app->error, sizeof(app->error), "max_connections exceeds 65536"); errno = EINVAL; return NULL;
    }
    capi_server *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->app = app; s->options = options;
    s->listener = s->reactor = s->wake[0] = s->wake[1] = -1;
    s->connections = calloc(options.max_connections, sizeof(*s->connections));
    if (!s->connections) goto fail;
    for (size_t i = 0; i < options.max_connections; ++i) s->connections[i].fd = -1;
#ifdef CP_EPOLL
    s->reactor = epoll_create1(EPOLL_CLOEXEC);
    if (s->reactor < 0) goto fail;
#elif defined(CP_KQUEUE)
    s->reactor = kqueue();
    if (s->reactor < 0 || fcntl(s->reactor, F_SETFD, FD_CLOEXEC) < 0) goto fail;
#else
    s->pollfds = calloc(options.max_connections + 2, sizeof(*s->pollfds));
    s->pollkeys = calloc(options.max_connections + 2, sizeof(*s->pollkeys));
    if (!s->pollfds || !s->pollkeys) goto fail;
#endif
    if (pipe(s->wake) || fd_prepare(s->wake[0]) || fd_prepare(s->wake[1])) goto fail;
    if (watch(s, s->wake[0], CP_WAKE, false, true)) goto fail;
    struct addrinfo hints = {0}, *addresses = NULL;
    char service[6];
    snprintf(service, sizeof(service), "%u", (unsigned)options.port);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_NUMERICSERV;
    int gai = getaddrinfo(options.host ? options.host : "127.0.0.1", service, &hints, &addresses);
    if (gai) {
        snprintf(app->error, sizeof(app->error), "resolve bind address: %s", gai_strerror(gai));
        capi_server_free(s); return NULL;
    }
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol), one = 1;
        if (fd < 0) continue;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (fd_prepare(fd) || bind(fd, a->ai_addr, a->ai_addrlen) || listen(fd, 1024)) { close(fd); continue; }
        s->listener = fd; break;
    }
    freeaddrinfo(addresses);
    if (s->listener < 0) goto fail;
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    if (getsockname(s->listener, (struct sockaddr *)&address, &length)) goto fail;
    s->port = address.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&address)->sin_port) :
        ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
    if (watch(s, s->listener, CP_LISTENER, false, true)) goto fail;
    return s;
fail:
    server_error(s, "listen");
    capi_server_free(s);
    return NULL;
}

uint16_t capi_server_port(const capi_server *s) { return s ? s->port : 0; }

void capi_server_stop(capi_server *s) {
    if (!s) return;
    int saved = errno;
    s->stop = 1;
    char byte = 1;
    (void)write(s->wake[1], &byte, 1);
    errno = saved;
}

void capi_server_free(capi_server *s) {
    if (!s) return;
    if (s->connections) for (size_t i = 0; i < s->options.max_connections; ++i) connection_close(s, &s->connections[i]);
    if (s->listener >= 0) close(s->listener);
    if (s->wake[0] >= 0) close(s->wake[0]);
    if (s->wake[1] >= 0) close(s->wake[1]);
    if (s->reactor >= 0) close(s->reactor);
    free(s->connections);
#ifdef CP_POLL
    free(s->pollfds); free(s->pollkeys);
#endif
    free(s);
}

static void accept_connections(capi_server *s) {
    for (int accepted = 0; accepted < CP_BATCH && !s->stop; ++accepted) {
        int fd = accept(s->listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) { --accepted; continue; }
            return;
        }
        if (s->active == s->options.max_connections || fd_prepare(fd)) { close(fd); continue; }
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        while (s->connections[s->cursor].fd >= 0) s->cursor = (s->cursor + 1) % s->options.max_connections;
        cp_connection *c = &s->connections[s->cursor];
        c->fd = fd;
        if (++c->generation == 0) ++c->generation;
        c->writing = c->closing = c->peer_eof = false;
        c->sent = c->consumed = 0; c->started = 0; c->activity = cp_now_ms();
        ++s->active;
        s->cursor = (s->cursor + 1) % s->options.max_connections;
        if (watch(s, fd, key_for(s, c), false, true)) connection_close(s, c);
    }
}

/* A connection only reads OR writes: bounded buffering and natural backpressure.
 * Pipelined bytes already read are dispatched after the previous response drains. */
static int process_input(capi_server *s, cp_connection *c) {
    if (!c->input.len) return 0;
    int result = cp_process(s->app, c->input.data, c->input.len, &c->body, &c->wire, &c->consumed, &c->closing);
    if (result < 0) return -1;
    if (result == 1) {
        c->writing = true; c->sent = 0;
        if (watch(s, c->fd, key_for(s, c), true, false)) return -1;
    }
    return result;
}

static int read_connection(capi_server *s, cp_connection *c) {
    size_t budget = CP_BUDGET;
    size_t limit = s->app->limits.max_header_bytes + s->app->limits.max_body_bytes;
    while (budget) {
        char bytes[8192];
        size_t amount = limit - c->input.len;
        if (amount > sizeof(bytes)) amount = sizeof(bytes);
        if (amount > budget) amount = budget;
        if (!amount) return -1;
        ssize_t n = recv(c->fd, bytes, amount, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
        if (!n) { c->peer_eof = true; return -1; }
        c->activity = cp_now_ms();
        if (!c->started) c->started = c->activity;
        if (cp_buf_append(&c->input, bytes, (size_t)n, limit)) return -1;
        budget -= (size_t)n;
        int result = process_input(s, c);
        if (result) return result < 0 ? -1 : 0;
    }
    return 0;
}

static int write_connection(capi_server *s, cp_connection *c) {
    size_t budget = CP_BUDGET;
    while (budget && c->writing) {
        size_t amount = c->wire.len - c->sent;
        if (amount > budget) amount = budget;
#ifdef MSG_NOSIGNAL
        ssize_t n = send(c->fd, c->wire.data + c->sent, amount, MSG_NOSIGNAL);
#else
        ssize_t n = send(c->fd, c->wire.data + c->sent, amount, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
        if (!n) return -1;
        c->activity = cp_now_ms(); c->sent += (size_t)n; budget -= (size_t)n;
        if (c->sent != c->wire.len) continue;
        if (c->closing || s->stop) return -1;
        size_t remaining = c->input.len - c->consumed;
        if (remaining) memmove(c->input.data, c->input.data + c->consumed, remaining);
        c->input.len = remaining; c->wire.len = c->body.len = 0;
        c->writing = false; c->started = remaining ? c->activity : 0;
        int result = process_input(s, c);
        if (result < 0) return -1;
        if (!result && watch(s, c->fd, key_for(s, c), false, false)) return -1;
    }
    return 0;
}

int capi_server_run(capi_server *s) {
    if (!s || s->running || s->listener < 0) { errno = EINVAL; return -1; }
    s->running = true;
    uint64_t stopping = 0;
    uint64_t next_sweep = cp_now_ms() + 50;
    int result = 0;
    for (;;) {
        uint64_t now = cp_now_ms();
        if (s->stop && !stopping) {
            stopping = now;
            unwatch(s, s->listener); close(s->listener); s->listener = -1;
            for (size_t i = 0; i < s->options.max_connections; ++i)
                if (s->connections[i].fd >= 0 && !s->connections[i].writing) connection_close(s, &s->connections[i]);
        }
        if (stopping && (!s->active || now - stopping >= s->options.shutdown_timeout_ms)) break;
        cp_event events[CP_BATCH];
        int count = wait_events(s, events, 50);
        if (count < 0) {
            if (errno == EINTR) continue;
            result = server_error(s, "event wait"); break;
        }
        for (int i = 0; i < count; ++i) {
            cp_event e = events[i];
            if (e.key == CP_WAKE) { char bytes[128]; while (read(s->wake[0], bytes, sizeof(bytes)) > 0) {} continue; }
            if (e.key == CP_LISTENER) { if (s->listener >= 0 && !s->stop) accept_connections(s); continue; }
            uint32_t slot = (uint32_t)e.key, generation = (uint32_t)(e.key >> 32);
            if (slot >= s->options.max_connections) continue;
            cp_connection *c = &s->connections[slot];
            if (c->fd < 0 || c->generation != generation) continue;
            if (e.error || (e.read && !c->writing && read_connection(s, c) < 0) ||
                (e.write && c->writing && write_connection(s, c) < 0)) connection_close(s, c);
        }
        now = cp_now_ms();
        if (now < next_sweep) continue;
        next_sweep = now + 50;
        for (size_t i = 0; i < s->options.max_connections; ++i) {
            cp_connection *c = &s->connections[i];
            if (c->fd < 0) continue;
            if (now - c->activity >= s->options.idle_timeout_ms ||
                (!c->writing && c->started && now - c->started >= s->options.request_timeout_ms)) connection_close(s, c);
        }
    }
    for (size_t i = 0; i < s->options.max_connections; ++i) connection_close(s, &s->connections[i]);
    s->running = false;
    return result;
}
