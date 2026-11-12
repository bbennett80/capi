#include "internal.h"

/* Reject overlong encodings, surrogate code points, and values > U+10FFFF. */
static size_t utf8(const unsigned char *s, size_t len) {
    unsigned char c = s[0];
    size_t n;
    unsigned value, minimum;
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) { n = 2; value = c & 0x1fU; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { n = 3; value = c & 0x0fU; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 4; value = c & 0x07U; minimum = 0x10000; }
    else return 0;
    if (n > len) return 0;
    for (size_t i = 1; i < n; ++i) {
        if ((s[i] & 0xc0U) != 0x80) return 0;
        value = value * 64 + (s[i] & 0x3fU);
    }
    return value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ? 0 : n;
}

int capi_json_string(capi_ctx *ctx, capi_str s) {
    static const char digits[] = "0123456789abcdef";
    if (s.len && !s.data) { ctx->failed = true; return -1; }
    if (capi_write(ctx, "\"", 1)) return -1;
    size_t start = 0, at = 0;
    while (at < s.len) {
        unsigned char c = (unsigned char)s.data[at];
        size_t n = utf8((const unsigned char *)s.data + at, s.len - at);
        if (!n) { ctx->failed = true; return -1; }
        if (c < 32 || c == '"' || c == '\\') {
            if (capi_write(ctx, s.data + start, at - start)) return -1;
            char escaped[6] = {'\\', 'u', '0', '0', digits[c >> 4], digits[c & 15]};
            if (c == '"' || c == '\\') { escaped[1] = (char)c; n = 2; }
            else n = 6;
            if (capi_write(ctx, escaped, n)) return -1;
            ++at; start = at;
        } else at += n;
    }
    if (capi_write(ctx, s.data ? s.data + start : NULL, at - start)) return -1;
    return capi_write(ctx, "\"", 1);
}
