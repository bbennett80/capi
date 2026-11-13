# Compiled HTML templates

For a complete browser app, the [todo walkthrough](../examples/todo/README.md)
shows HTML forms, a template model built in C, local CSS, optional CDN styling,
and CLI-generated SQLite actions working together.

`capi::templates` is a small C11 library with no external dependencies. It uses
a Mustache-inspired syntax and typed C data. Templates compile once at startup
into immutable operations; rendering does no filesystem access or template
parsing. Share compiled templates between requests, keeping them alive until
those requests finish.

It is enabled by default as a separate library, and never linked into the core
implicitly. Disable it with `-DCAPI_WITH_TEMPLATES=OFF`. Installed consumers use:

```cmake
find_package(capi 0.4 CONFIG REQUIRED COMPONENTS templates)
target_link_libraries(app PRIVATE capi::templates)
```

The public header is [`capi_template.h`](../include/capi_template.h).
pkg-config consumers use `capi-templates`.

## Startup and rendering

```c
capi_template_error error;
capi_template *page = capi_template_load("views/profile.html", &error);
if (!page) {
    fprintf(stderr, "template at byte %zu: %s\n", error.offset, error.message);
    return 1;
}
/* Register page as handler data. Free it after the server stops. */
```

Or compile in-memory source with `capi_template_compile(source, len, &error)`.
Source bytes are copied. Error offsets are byte offsets, not line numbers.

```c
static void profile(capi_ctx *ctx) {
    capi_field fields[] = {
        {"name", capi_value_text("Ada Lovelace")},
        {"unread", capi_value_int(3)},
        {"active", capi_value_bool(true)}
    };
    capi_value data = capi_value_object(fields, 3);
    capi_render(ctx, 200, capi_data(ctx), &data);
}
```

```html
<h1>{{name}}</h1>
{{#active}}<p>{{unread}} unread messages</p>{{/active}}
```

`capi_render` replaces the body, sets the status and `text/html; charset=utf-8`,
and preserves other headers. Constructing data does not allocate or copy it:
all strings, fields, and lists must remain valid until rendering returns.
Stack data is fine for immediate rendering; SQLite rows copied into the request
arena are useful for lists. Use `capi_value_string` for length-based values and
`capi_value_text` for NUL-terminated strings.

## Supported syntax

| Syntax | Behavior |
| --- | --- |
| `{{name}}` | HTML-escaped interpolation |
| `{{person.name}}` | Dotted object lookup |
| `{{.}}` | Current section/list item |
| `{{{trusted_html}}}` | Raw interpolation, explicitly bypassing escaping |
| `{{#items}}…{{/items}}` | Loop over a list, or enter a truthy section |
| `{{^items}}…{{/items}}` | Render when absent or false/empty |
| `{{! comment }}` | Omitted |
| `{{>card}}` | Render a registered compiled partial in the current context |

Names use ASCII letters, digits, underscores, hyphens, and dot-separated paths.
Missing values interpolate as empty text. First matching object fields win.
Lookup searches the current context, then parents, for the first path segment;
subsequent dotted segments stay within the selected value. A present null value
shadows an outer value with the same name.

Null, false booleans, empty strings, and empty lists are false. Objects and all
integers, including zero, are true. String/integer/object sections enter that
value as the current context, with parent lookup available. Lists enter each
item in order. Integers render as decimal, booleans as `true`/`false`. Objects
and lists do not interpolate directly. A zero-initialized `capi_value` is null.

```c
capi_field first[] = {{"title", capi_value_text("One")}};
capi_field second[] = {{"title", capi_value_text("Two")}};
capi_value items[] = {capi_value_object(first, 1), capi_value_object(second, 1)};
capi_field root[] = {{"notes", capi_value_list(items, 2)}};
capi_value data = capi_value_object(root, 1);
```

```html
{{#notes}}<article>{{title}}</article>{{/notes}}
{{^notes}}<p>No notes yet.</p>{{/notes}}
```

## Partials and composition

```c
capi_partial partials[] = {{"card", card_template}, {"footer", footer_template}};
capi_render_with(ctx, 200, page_template, &data, partials, 2);
```

Partials come from an explicit registry. There is no path expansion or implicit
file lookup at request time. Missing referenced partials and recursion beyond
the depth limit fail rendering; accidental partial cycles produce a clean 500.
The registry and templates are borrowed for the call. Partials can reuse page
data through normal current/parent lookup.

This is a documented subset, not a full Mustache implementation. Whitespace
is preserved literally, including standalone tag lines; partial indentation
is not adjusted. There are no delimiter changes, lambdas, expressions, helpers,
HTML-context inference, automatic layouts, or runtime includes.

## Escaping and limits

Escaped interpolation replaces `&`, `<`, `>`, `"`, and `'`. It is intended for
HTML text and quoted attributes. It does not validate URLs, sanitize HTML,
or escape JavaScript/CSS. Keep user values out of script/style blocks and
unquoted attributes; validate URL schemes separately. Only use triple braces
for HTML the application has explicitly decided to trust. Strings should be
valid UTF-8 when the response declares UTF-8.

Compilation accepts at most 1 MiB of source, 65536 operations, and 64 nested
sections. Rendering allows 64 levels of nested sections/partials and a budget
of one million operation/loop steps. Loops with empty bodies consume budget too.
These bounds prevent runaway rendering and accidental recursion. Application
data pointers and lengths must describe valid C objects; capi cannot validate
arbitrary pointers.

The normal response size limit applies to rendered output. Missing partials,
exceeded limits, invalid string data, and writer failures return -1 and mark
the response failed, replacing partial output with a clean 500. Compile errors
are startup errors, with a diagnostic message and source offset.
