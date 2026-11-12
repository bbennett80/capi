#include "internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int cp_token(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static const char *crlf(const char *p, const char *end) {
    while (p + 1 < end) {
        if (p[0] == '\r' && p[1] == '\n') return p;
        ++p;
    }
    return NULL;
}

static int has_token(capi_str value, const char *token) {
    size_t at = 0;
    while (at < value.len) {
        size_t start, stop;
        while (at < value.len && (value.data[at] == ' ' || value.data[at] == '\t' || value.data[at] == ',')) ++at;
        start = at;
        while (at < value.len && value.data[at] != ',') ++at;
        stop = at;
        while (stop > start && (value.data[stop - 1] == ' ' || value.data[stop - 1] == '\t')) --stop;
        if (cp_caseeq((capi_str){value.data + start, stop - start}, token)) return 1;
    }
    return 0;
}

/* Returns 0 incomplete, 1 parsed, otherwise an HTTP status. No allocation.
 * Framing is intentionally strict: one Content-Length; no transfer coding. */
int cp_parse(capi_ctx *ctx, const char *data, size_t len, size_t *consumed) {
    size_t headlen = 0, content_length = 0;
    bool seen_length = false, transfer = false, expect = false;
    const char *end, *line, *p, *space, *version;
    *consumed = 0;
    for (size_t i = 0; i + 3 < len && i < ctx->app->limits.max_header_bytes; ++i) {
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) { headlen = i + 4; break; }
    }
    if (!headlen) return len >= ctx->app->limits.max_header_bytes ? 431 : 0;
    if (headlen > ctx->app->limits.max_header_bytes) return 431;
    end = data + headlen;
    line = crlf(data, end);
    if (!line) return 400;
    space = memchr(data, ' ', (size_t)(line - data));
    if (!space || space == data) return 400;
    for (p = data; p < space; ++p) if (!cp_token((unsigned char)*p) || !*p) return 400;
    ctx->method = (capi_str){data, (size_t)(space - data)};
    p = space + 1;
    space = memchr(p, ' ', (size_t)(line - p));
    if (!space || space == p || *p != '/') return 400;
    ctx->target = (capi_str){p, (size_t)(space - p)};
    for (const char *q = p; q < space; ++q)
        if ((unsigned char)*q <= 32 || (unsigned char)*q == 127 || *q == '#') return 400;
    version = space + 1;
    if (line - version != 8) return 400;
    if (memcmp(version, "HTTP/1.1", 8) == 0) ctx->http10 = false;
    else if (memcmp(version, "HTTP/1.0", 8) == 0) ctx->http10 = true;
    else return 505;
    ctx->path = ctx->target;
    p = memchr(ctx->target.data, '?', ctx->target.len);
    if (p) {
        ctx->path.len = (size_t)(p - ctx->target.data);
        ctx->query = (capi_str){p + 1, ctx->target.len - ctx->path.len - 1};
    }
    p = line + 2;
    size_t hosts = 0;
    while (p < end - 2) {
        const char *colon, *value, *stop;
        line = crlf(p, end);
        if (!line || line == p || ctx->nheaders == CAPI_MAX_HEADERS) return 431;
        colon = memchr(p, ':', (size_t)(line - p));
        if (!colon || colon == p) return 400;
        for (const char *q = p; q < colon; ++q) if (!*q || !cp_token((unsigned char)*q)) return 400;
        value = colon + 1;
        while (value < line && (*value == ' ' || *value == '\t')) ++value;
        stop = line;
        while (stop > value && (stop[-1] == ' ' || stop[-1] == '\t')) --stop;
        for (const char *q = value; q < stop; ++q)
            if (((unsigned char)*q < 32 && *q != '\t') || (unsigned char)*q == 127) return 400;
        cp_header h = {{p, (size_t)(colon - p)}, {value, (size_t)(stop - value)}};
        ctx->headers[ctx->nheaders++] = h;
        if (cp_caseeq(h.name, "host")) {
            if (++hosts > 1 || !h.value.len) return 400;
            for (size_t i = 0; i < h.value.len; ++i)
                if ((unsigned char)h.value.data[i] <= 32 || h.value.data[i] == ',') return 400;
        } else if (cp_caseeq(h.name, "content-length")) {
            if (seen_length || !h.value.len) return 400;
            seen_length = true;
            for (size_t i = 0; i < h.value.len; ++i) {
                unsigned char digit = (unsigned char)h.value.data[i];
                if (digit < '0' || digit > '9') return 400;
                if (content_length > (SIZE_MAX - (size_t)(digit - '0')) / 10) return 413;
                content_length = content_length * 10 + (size_t)(digit - '0');
            }
        } else if (cp_caseeq(h.name, "transfer-encoding")) transfer = true;
        else if (cp_caseeq(h.name, "expect")) expect = true;
        p = line + 2;
    }
    if (!ctx->http10 && hosts != 1) return 400;
    if (transfer && seen_length) return 400;
    if (transfer) return 501;
    if (content_length > ctx->app->limits.max_body_bytes) return 413;
    if (expect) return 417;
    if (len - headlen < content_length) return 0;
    ctx->body = (capi_str){data + headlen, content_length};
    *consumed = headlen + content_length;
    bool keepalive = false, close = false;
    for (size_t i = 0; i < ctx->nheaders; ++i) {
        if (!cp_caseeq(ctx->headers[i].name, "connection")) continue;
        keepalive |= has_token(ctx->headers[i].value, "keep-alive") != 0;
        close |= has_token(ctx->headers[i].value, "close") != 0;
    }
    ctx->close = close || (ctx->http10 && !keepalive);
    return 1;
}
