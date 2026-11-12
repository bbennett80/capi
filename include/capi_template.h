#ifndef CAPI_TEMPLATE_H
#define CAPI_TEMPLATE_H
#include "capi.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct capi_template capi_template;
typedef struct { size_t offset; char message[160]; } capi_template_error;
/* Copies source; compiled templates are immutable and may be shared.
 * Limits: 1 MiB source, 65536 operations, 64 nested sections. */
capi_template *capi_template_compile(const char *source, size_t len, capi_template_error *error);
capi_template *capi_template_load(const char *path, capi_template_error *error);
void capi_template_free(capi_template *view);

typedef struct capi_value capi_value;
typedef struct capi_field capi_field;
typedef enum {
    CAPI_VALUE_NULL, CAPI_VALUE_STRING, CAPI_VALUE_BOOL, CAPI_VALUE_INT,
    CAPI_VALUE_OBJECT, CAPI_VALUE_LIST
} capi_value_type;
struct capi_value {
    capi_value_type type;
    union {
        capi_str string;
        bool boolean;
        int64_t integer;
        struct { const capi_field *fields; size_t len; } object;
        struct { const capi_value *items; size_t len; } list;
    } as;
};
struct capi_field { const char *name; capi_value value; };

/* Constructors borrow data. Keep fields/items/strings alive through render. */
capi_value capi_value_text(const char *text);
capi_value capi_value_string(capi_str string);
capi_value capi_value_bool(bool boolean);
capi_value capi_value_int(int64_t integer);
capi_value capi_value_object(const capi_field *fields, size_t len);
capi_value capi_value_list(const capi_value *items, size_t len);

typedef struct { const char *name; const capi_template *view; } capi_partial;
/* Replace body, set status and text/html, render without filesystem access.
 * {{name}} escapes HTML; {{{name}}} is explicitly trusted raw HTML.
 * Sections: {{#items}}...{{/items}}; inverted: {{^items}}...{{/items}}.
 * Dotted lookup, parent lookup, {{.}}, comments, and {{>partial}} supported.
 * Not a full Mustache implementation. No delimiter changes or lambdas.
 * Depth/operation limits or invalid data mark the response failed (500).
 * HTML escaping is for text/quoted attributes, not JS, CSS, or URL validation. */
int capi_render(capi_ctx *ctx, int status, const capi_template *view, const capi_value *data);
int capi_render_with(capi_ctx *ctx, int status, const capi_template *view, const capi_value *data,
                     const capi_partial *partials, size_t count);

#ifdef __cplusplus
}
#endif
#endif
