#include "capi_template.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CT_SOURCE_LIMIT (1024 * 1024)
#define CT_OP_LIMIT 65536
#define CT_DEPTH_LIMIT 64
#define CT_RENDER_BUDGET 1000000

typedef enum { CT_LITERAL, CT_ESCAPED, CT_RAW, CT_SECTION, CT_INVERTED, CT_END, CT_PARTIAL } ct_kind;
typedef struct { ct_kind kind; size_t offset, len, jump; } ct_op;
struct capi_template { char *source; ct_op *ops; size_t count, capacity; };

static void error_at(capi_template_error *error, size_t offset, const char *message) {
    if (error) { error->offset = offset; snprintf(error->message, sizeof(error->message), "%s", message); }
}
static bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool name_valid(const char *name, size_t len) {
    if (!len) return false;
    if (len == 1 && name[0] == '.') return true;
    if (name[0] == '.' || name[len - 1] == '.') return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return false;
        if (c == '.' && i && name[i - 1] == '.') return false;
    }
    return true;
}
static int add_op(capi_template *view, ct_op op) {
    if (view->count == CT_OP_LIMIT) return -1;
    if (view->count == view->capacity) {
        size_t capacity = view->capacity ? view->capacity * 2 : 32;
        ct_op *ops = realloc(view->ops, capacity * sizeof(*ops));
        if (!ops) return -1;
        view->ops = ops; view->capacity = capacity;
    }
    view->ops[view->count++] = op;
    return 0;
}

void capi_template_free(capi_template *view) {
    if (view) { free(view->source); free(view->ops); free(view); }
}

capi_template *capi_template_compile(const char *source, size_t len, capi_template_error *error) {
    if (error) *error = (capi_template_error){0};
    if ((!source && len) || len > CT_SOURCE_LIMIT) { error_at(error, 0, "invalid source or source exceeds 1 MiB"); return NULL; }
    capi_template *view = calloc(1, sizeof(*view));
    if (!view) { error_at(error, 0, "out of memory"); return NULL; }
    view->source = malloc(len + 1);
    if (!view->source) { error_at(error, 0, "out of memory"); capi_template_free(view); return NULL; }
    if (len) memcpy(view->source, source, len);
    view->source[len] = 0;
    size_t stack[CT_DEPTH_LIMIT], depth = 0, at = 0;
    while (at < len) {
        size_t open = at;
        while (open + 1 < len && !(source[open] == '{' && source[open + 1] == '{')) ++open;
        if (open + 1 >= len) open = len;
        if (open > at && add_op(view, (ct_op){CT_LITERAL, at, open - at, 0})) goto memory;
        if (open == len) break;
        bool triple = open + 2 < len && source[open + 2] == '{';
        size_t width = triple ? 3 : 2, begin = open + width, end = begin;
        while (end + width <= len && memcmp(source + end, triple ? "}}}" : "}}", width)) ++end;
        if (end + width > len) { error_at(error, open, "unclosed tag"); goto fail; }
        at = end + width;
        while (begin < end && whitespace(source[begin])) ++begin;
        while (end > begin && whitespace(source[end - 1])) --end;
        if (begin == end) { error_at(error, open, "empty tag"); goto fail; }
        ct_kind kind = triple ? CT_RAW : CT_ESCAPED;
        if (!triple) {
            char prefix = source[begin];
            if (prefix == '!') continue;
            if (prefix == '#') kind = CT_SECTION;
            else if (prefix == '^') kind = CT_INVERTED;
            else if (prefix == '/') kind = CT_END;
            else if (prefix == '>') kind = CT_PARTIAL;
            if (kind != CT_ESCAPED) {
                ++begin;
                while (begin < end && whitespace(source[begin])) ++begin;
            }
        }
        if (!name_valid(source + begin, end - begin)) { error_at(error, open, "invalid tag name"); goto fail; }
        ct_op op = {kind, begin, end - begin, 0};
        if (kind == CT_SECTION || kind == CT_INVERTED) {
            if (depth == CT_DEPTH_LIMIT) { error_at(error, open, "section nesting exceeds 64"); goto fail; }
            stack[depth++] = view->count;
        } else if (kind == CT_END) {
            if (!depth) { error_at(error, open, "closing tag without a section"); goto fail; }
            size_t index = stack[--depth];
            ct_op *start = &view->ops[index];
            if (start->len != op.len || memcmp(source + start->offset, source + op.offset, op.len)) {
                error_at(error, open, "closing tag does not match its section"); goto fail;
            }
            start->jump = view->count;
            op.jump = index;
        }
        if (add_op(view, op)) goto memory;
    }
    if (depth) { error_at(error, view->ops[stack[depth - 1]].offset, "unclosed section"); goto fail; }
    return view;
memory:
    error_at(error, at, "out of memory or template exceeds 65536 operations");
fail:
    capi_template_free(view); return NULL;
}

capi_template *capi_template_load(const char *path, capi_template_error *error) {
    if (error) *error = (capi_template_error){0};
    if (!path) { error_at(error, 0, "null template path"); return NULL; }
    FILE *file = fopen(path, "rb");
    if (!file) { error_at(error, 0, strerror(errno)); return NULL; }
    /* One bounded read also works for nonseekable sources; load only at startup. */
    char *source = malloc(CT_SOURCE_LIMIT + 1);
    if (!source) { fclose(file); error_at(error, 0, "out of memory"); return NULL; }
    size_t len = fread(source, 1, CT_SOURCE_LIMIT + 1, file);
    bool failed = ferror(file) != 0;
    if (fclose(file)) failed = true;
    capi_template *view = NULL;
    if (failed) error_at(error, 0, "could not read template");
    else view = capi_template_compile(source, len, error);
    free(source); return view;
}

capi_value capi_value_string(capi_str string) { return (capi_value){.type = CAPI_VALUE_STRING, .as.string = string}; }
capi_value capi_value_text(const char *text) { return capi_value_string((capi_str){text, text ? strlen(text) : 0}); }
capi_value capi_value_bool(bool value) { return (capi_value){.type = CAPI_VALUE_BOOL, .as.boolean = value}; }
capi_value capi_value_int(int64_t value) { return (capi_value){.type = CAPI_VALUE_INT, .as.integer = value}; }
capi_value capi_value_object(const capi_field *fields, size_t len) {
    return (capi_value){.type = CAPI_VALUE_OBJECT, .as.object = {fields, len}};
}
capi_value capi_value_list(const capi_value *items, size_t len) {
    return (capi_value){.type = CAPI_VALUE_LIST, .as.list = {items, len}};
}

typedef struct ct_frame { const capi_value *value; const struct ct_frame *parent; } ct_frame;
typedef struct {
    capi_ctx *ctx;
    const capi_partial *partials;
    size_t count, budget;
} ct_render;

static const capi_value *field(const capi_value *value, const char *name, size_t len) {
    if (!value || value->type != CAPI_VALUE_OBJECT || !value->as.object.fields) return NULL;
    for (size_t i = 0; i < value->as.object.len; ++i) {
        const capi_field *f = &value->as.object.fields[i];
        if (f->name && strlen(f->name) == len && !memcmp(f->name, name, len)) return &f->value;
    }
    return NULL;
}
static const capi_value *lookup(const ct_frame *frame, const char *name, size_t len) {
    if (len == 1 && name[0] == '.') return frame->value;
    size_t end = 0;
    while (end < len && name[end] != '.') ++end;
    const capi_value *value = NULL;
    for (const ct_frame *f = frame; f && !value; f = f->parent) value = field(f->value, name, end);
    while (value && end < len) {
        size_t start = ++end;
        while (end < len && name[end] != '.') ++end;
        value = field(value, name + start, end - start);
    }
    return value;
}
static bool truthy(const capi_value *value) {
    if (!value) return false;
    switch (value->type) {
    case CAPI_VALUE_NULL: return false;
    case CAPI_VALUE_BOOL: return value->as.boolean;
    case CAPI_VALUE_STRING: return value->as.string.len != 0;
    case CAPI_VALUE_LIST: return value->as.list.len != 0;
    case CAPI_VALUE_INT: case CAPI_VALUE_OBJECT: return true;
    }
    return false;
}
static int escaped(capi_ctx *ctx, capi_str text) {
    if (text.len && !text.data) return -1;
    size_t start = 0;
    for (size_t i = 0; i < text.len; ++i) {
        const char *replacement = NULL;
        switch (text.data[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        }
        if (!replacement) continue;
        if (capi_write(ctx, text.data + start, i - start) || capi_write(ctx, replacement, strlen(replacement))) return -1;
        start = i + 1;
    }
    return capi_write(ctx, text.data ? text.data + start : NULL, text.len - start);
}
static int interpolate(capi_ctx *ctx, const capi_value *value, bool raw) {
    if (!value) return 0;
    switch (value->type) {
    case CAPI_VALUE_STRING:
        return raw ? capi_write(ctx, value->as.string.data, value->as.string.len) : escaped(ctx, value->as.string);
    case CAPI_VALUE_INT: return capi_printf(ctx, "%" PRId64, value->as.integer);
    case CAPI_VALUE_BOOL: return capi_write(ctx, value->as.boolean ? "true" : "false", value->as.boolean ? 4 : 5);
    case CAPI_VALUE_NULL: case CAPI_VALUE_OBJECT: case CAPI_VALUE_LIST: return 0;
    }
    return -1;
}

static int render_range(ct_render *r, const capi_template *view, size_t start, size_t end,
                        const ct_frame *frame, size_t depth) {
    if (depth > CT_DEPTH_LIMIT) return -1;
    for (size_t i = start; i < end; ++i) {
        if (!r->budget) return -1;
        --r->budget;
        ct_op op = view->ops[i];
        const char *text = view->source + op.offset;
        if (op.kind == CT_LITERAL) { if (capi_write(r->ctx, text, op.len)) return -1; continue; }
        if (op.kind == CT_PARTIAL) {
            const capi_template *partial = NULL;
            for (size_t p = 0; p < r->count; ++p)
                if (r->partials[p].name && strlen(r->partials[p].name) == op.len && !memcmp(r->partials[p].name, text, op.len)) {
                    partial = r->partials[p].view; break;
                }
            if (!partial || render_range(r, partial, 0, partial->count, frame, depth + 1)) return -1;
            continue;
        }
        const capi_value *value = lookup(frame, text, op.len);
        if (op.kind == CT_ESCAPED || op.kind == CT_RAW) {
            if (interpolate(r->ctx, value, op.kind == CT_RAW)) return -1;
        } else if (op.kind == CT_SECTION || op.kind == CT_INVERTED) {
            bool active = truthy(value);
            if (op.kind == CT_INVERTED) {
                if (!active && render_range(r, view, i + 1, op.jump, frame, depth + 1)) return -1;
            } else if (active && value->type == CAPI_VALUE_LIST) {
                if (!value->as.list.items) return -1;
                for (size_t item = 0; item < value->as.list.len; ++item) {
                    if (!r->budget) return -1;
                    --r->budget;
                    ct_frame child = {&value->as.list.items[item], frame};
                    if (render_range(r, view, i + 1, op.jump, &child, depth + 1)) return -1;
                }
            } else if (active) {
                ct_frame child = {value, frame};
                if (render_range(r, view, i + 1, op.jump, &child, depth + 1)) return -1;
            }
            i = op.jump;
        } else return -1;
    }
    return 0;
}

int capi_render_with(capi_ctx *ctx, int status, const capi_template *view, const capi_value *data,
                     const capi_partial *partials, size_t count) {
    if (!ctx) return -1;
    if (!view || !data || (count && !partials)) { capi_fail_with(ctx, CAPI_ERROR_ARGUMENT, "capi_render"); return -1; }
    if (capi_html(ctx, status, "")) return -1;
    ct_frame frame = {data, NULL};
    ct_render renderer = {ctx, partials, count, CT_RENDER_BUDGET};
    if (render_range(&renderer, view, 0, view->count, &frame, 0)) { capi_fail_with(ctx, CAPI_ERROR_TEMPLATE, "capi_render"); return -1; }
    return 0;
}
int capi_render(capi_ctx *ctx, int status, const capi_template *view, const capi_value *data) {
    return capi_render_with(ctx, status, view, data, NULL, 0);
}
