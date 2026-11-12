#include "internal.h"
#include <string.h>

static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int next_byte(capi_str input, size_t *at) {
    unsigned char c = (unsigned char)input.data[(*at)++];
    if (c == '+') return ' ';
    if (c != '%') return c;
    if (input.len - *at < 2) return -1;
    int a = hex((unsigned char)input.data[*at]), b = hex((unsigned char)input.data[*at + 1]);
    if (a < 0 || b < 0) return -1;
    *at += 2;
    return a * 16 + b;
}
static int key_matches(capi_str input, const char *name) {
    size_t at = 0, n = 0, len = strlen(name);
    bool matches = true;
    while (at < input.len) {
        int c = next_byte(input, &at);
        if (c < 0) return -1;
        if (n >= len || (unsigned char)name[n] != (unsigned char)c) matches = false;
        ++n;
    }
    return matches && n == len ? 1 : 0;
}
static int decode(capi_str input, char *out, size_t *len) {
    size_t at = 0, n = 0;
    while (at < input.len) {
        int c = next_byte(input, &at);
        if (c < 0) return -1;
        if (out) out[n] = (char)c;
        ++n;
    }
    if (out) out[n] = 0;
    *len = n;
    return 0;
}
static capi_input_result pairs(capi_ctx *ctx, capi_str input, const char *name, capi_str *value) {
    if (value) *value = (capi_str){NULL, 0};
    if (!ctx || !name || !value) return CAPI_INPUT_ARGUMENT;
    size_t at = 0;
    while (at < input.len) {
        size_t end = at, eq;
        while (end < input.len && input.data[end] != '&') ++end;
        eq = at;
        while (eq < end && input.data[eq] != '=') ++eq;
        int matches = key_matches((capi_str){input.data + at, eq - at}, name);
        if (matches < 0) return CAPI_INPUT_ENCODING;
        if (matches) {
            size_t start = eq < end ? eq + 1 : end, len = 0;
            capi_str raw = {input.data + start, end - start};
            if (decode(raw, NULL, &len)) return CAPI_INPUT_ENCODING;
            char *copy = capi_alloc(ctx, len + 1);
            if (!copy) return CAPI_INPUT_NO_MEMORY;
            if (decode(raw, copy, &len)) return CAPI_INPUT_ENCODING;
            *value = (capi_str){copy, len};
            return CAPI_INPUT_FOUND;
        }
        at = end + 1;
    }
    return CAPI_INPUT_MISSING;
}
capi_input_result capi_query(capi_ctx *ctx, const char *name, capi_str *value) {
    return pairs(ctx, ctx ? ctx->query : (capi_str){NULL, 0}, name, value);
}
capi_input_result capi_form(capi_ctx *ctx, const char *name, capi_str *value) {
    if (value) *value = (capi_str){NULL, 0};
    if (!ctx || !name || !value) return CAPI_INPUT_ARGUMENT;
    capi_str type = capi_header(ctx, "Content-Type");
    size_t end = 0;
    while (end < type.len && type.data[end] != ';') ++end;
    while (end && (type.data[end - 1] == ' ' || type.data[end - 1] == '\t')) --end;
    if (!cp_caseeq((capi_str){type.data, end}, "application/x-www-form-urlencoded")) return CAPI_INPUT_CONTENT_TYPE;
    return pairs(ctx, ctx->body, name, value);
}

capi_input_result capi_str_i64(capi_str input, int64_t minimum, int64_t maximum, int64_t *value) {
    if (value) *value = 0;
    if (!value || minimum > maximum || (input.len && !input.data)) return CAPI_INPUT_ARGUMENT;
    if (!input.len) return CAPI_INPUT_VALUE;
    size_t at = 0;
    bool negative = input.data[0] == '-';
    if (negative || input.data[0] == '+') ++at;
    if (at == input.len) return CAPI_INPUT_VALUE;
    uint64_t number = 0, limit = (uint64_t)INT64_MAX + (negative ? 1U : 0U);
    bool overflow = false;
    for (; at < input.len; ++at) {
        unsigned char c = (unsigned char)input.data[at];
        if (c < '0' || c > '9') return CAPI_INPUT_VALUE;
        uint64_t digit = (uint64_t)(c - '0');
        if (number > (limit - digit) / 10) overflow = true;
        if (!overflow) number = number * 10 + digit;
    }
    if (overflow) return CAPI_INPUT_RANGE;
    int64_t result;
    if (negative) result = number == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)number;
    else result = (int64_t)number;
    if (result < minimum || result > maximum) return CAPI_INPUT_RANGE;
    *value = result;
    return CAPI_INPUT_FOUND;
}
static capi_input_result integer(capi_input_result found, capi_str input, int64_t min, int64_t max, int64_t *out) {
    if (out) *out = 0;
    if (!out || min > max) return CAPI_INPUT_ARGUMENT;
    return found == CAPI_INPUT_FOUND ? capi_str_i64(input, min, max, out) : found;
}
capi_input_result capi_param_i64(capi_ctx *ctx, const char *name, int64_t min, int64_t max, int64_t *out) {
    if (!ctx || !name) return integer(CAPI_INPUT_ARGUMENT, (capi_str){NULL, 0}, min, max, out);
    capi_str input = capi_param(ctx, name);
    return integer(input.data ? CAPI_INPUT_FOUND : CAPI_INPUT_MISSING, input, min, max, out);
}
capi_input_result capi_query_i64(capi_ctx *ctx, const char *name, int64_t min, int64_t max, int64_t *out) {
    if (!out || min > max) return integer(CAPI_INPUT_ARGUMENT, (capi_str){NULL, 0}, min, max, out);
    capi_str input;
    capi_input_result found = capi_query(ctx, name, &input);
    return integer(found, input, min, max, out);
}
capi_input_result capi_form_i64(capi_ctx *ctx, const char *name, int64_t min, int64_t max, int64_t *out) {
    if (!out || min > max) return integer(CAPI_INPUT_ARGUMENT, (capi_str){NULL, 0}, min, max, out);
    capi_str input;
    capi_input_result found = capi_form(ctx, name, &input);
    return integer(found, input, min, max, out);
}
const char *capi_input_name(capi_input_result result) {
    switch (result) {
    case CAPI_INPUT_FOUND: return "found";
    case CAPI_INPUT_MISSING: return "missing";
    case CAPI_INPUT_ENCODING: return "encoding";
    case CAPI_INPUT_CONTENT_TYPE: return "content_type";
    case CAPI_INPUT_RANGE: return "range";
    case CAPI_INPUT_NO_MEMORY: return "no_memory";
    case CAPI_INPUT_VALUE: return "value";
    case CAPI_INPUT_ARGUMENT: return "argument";
    }
    return "unknown";
}
