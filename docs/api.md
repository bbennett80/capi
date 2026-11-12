# API guide

The core public API lives in [`include/capi.h`](../include/capi.h).
Include this header and link `capi::capi` or `-lcapi`.
Optional [SQLite](sqlite.md), [template](templates.md), and [worker](workers.md) libraries have separate
headers and link targets.

## Lifetime and ownership

| Value | Owner | Valid until |
| --- | --- | --- |
| `capi_app *` | Caller | `capi_app_free` |
| `capi_server *` | Caller | `capi_server_free` |
| Handler registration data | Application | Last dispatch/server use |
| Request `capi_str` views | Framework input buffer | Pipeline and deferred cleanup finish |
| Query/form values / `capi_alloc` memory | Request arena | Pipeline and deferred cleanup finish |
| State set with `capi_set_state` | Application | Application-defined; arena allocations remain request-scoped |
| Response header arguments | Copied into arena | Original arguments may immediately change |
| Bytes passed to `capi_write` | Copied into response | Original argument may immediately change |
| `capi_result.data` | Caller | `capi_result_free` |

For asynchronous requests, views and arena allocations remain valid through
work completion and deferred cleanup, even after their socket closes. Submitted
job data is owned by the worker until its completion returns to the event loop;
see the [worker ownership rules](workers.md#ownership-and-execution-order).

`capi_str` is `{ const char *data; size_t len; }`. Views need not be NUL-terminated
and may contain NUL bytes. Do not pass a view to `strlen` or `%s`. Use
`capi_str_eq(view, "literal")` for comparisons. For textual output use
`%.*s` with an appropriate `int` length; use `capi_write` for arbitrary bytes.

Freeze app configuration while it is in use. Separate servers may share an
immutable app on separate threads, provided handlers and registration data are
thread-safe. Each server is operated by one thread; never free a server
concurrently with its event loop. `capi_server_stop` is thread-safe and can
wake the loop from a signal handler. Signal installation and the lifetime of
the server pointer remain the application's responsibility.

## App and routes

```c
capi_app *app = capi_app_new();
if (!app) { /* allocation failed */ }
if (capi_route(app, CAPI_GET, "/users/:id", handler, my_data)) {
    fprintf(stderr, "%s\n", capi_app_error(app));
}
```

| Pattern | Matches | Capture |
| --- | --- | --- |
| `/users/new` | Exactly `/users/new` | None |
| `/users/:id` | One nonempty segment | `capi_param(ctx, "id")` |
| `/files/*rest` | `/files/` and everything below it | Remainder, including `/` |
| `/` | The root path | None |

A root catch-all `/*rest` also matches `/` with an empty capture. `/files/*rest`
requires the slash after `files`; it does not match `/files`.

Routes are case-sensitive. `/users` and `/users/` are distinct. Repeated slashes
are preserved in requests; repeated interior slashes in route patterns are
rejected. Parameters and paths are raw bytes: `%2F` remains `%2F` and never
changes route segmentation. There is no path normalization or redirect.

Literal > parameter > wildcard precedence applies at each segment. If a branch
cannot complete the path, matching backtracks to less specific branches. Once
a path is selected, method lookup happens on that path; another route shape
does not override its 405. Duplicate method/shape registrations are errors,
including `/users/:a` and `/users/:b` for the same method. Different methods
may use different parameter names at the same trie node.

`CAPI_ANY` is a fallback for every syntactically valid method. A dedicated
method route wins. HEAD uses an explicit HEAD handler, then GET, then ANY;
serialization always suppresses the HEAD body. A matched path with no OPTIONS
handler or ANY fallback gets an automatic 204 plus `Allow`. Other method
mismatches get 405 plus `Allow`. Unknown paths get 404, configurable through
`capi_not_found(app, handler, data)`.

Patterns are limited to 4096 bytes, 128 segments, and 16 captures. Wildcards
must be the final segment. Capture names contain ASCII letters, digits, and
underscores and must be unique within a pattern.

## Middleware

```c
static void auth(capi_ctx *ctx) {
    if (!authorized(ctx)) {
        capi_json(ctx, 401, "{\"error\":\"unauthorized\"}");
        return;
    }
    capi_next(ctx);
    /* Runs after later middleware and the endpoint. */
}
if (capi_use(app, "/api", auth, auth_config)) { /* handle setup failure */ }
```

Middleware runs in registration order before all endpoints, including built-in
404/405/OPTIONS handling. Prefix `/api` matches `/api`, `/api/`, and `/api/x`,
but not `/apix`. A trailing slash on a middleware prefix is ignored. `/` applies
globally. The maximum is 128 middleware registrations.

Call `capi_next` at most once per middleware invocation. Extra calls are ignored.
Returning without calling it stops traversal. Endpoint calls to `capi_next`
have no effect. `capi_data` is scoped to the current handler registration;
the previous value is restored when a nested handler returns.

For request state, allocate a struct with `capi_alloc` and store it through
`capi_set_state`. Allocations are zeroed and aligned to `max_align_t`; failure
returns NULL and marks the response as failed. External objects stored as
state are not freed by capi.

Use `capi_defer(ctx, cleanup_fn, data)` for non-arena resources such as files or
database statements. Callbacks receive their data pointer, run once in reverse
registration order after the entire pipeline unwinds, and run even on early
returns and failed responses. Arena memory and request views remain valid
during cleanup. Callbacks must not free arena allocations or dispatch the same
context again. If registering cleanup fails, it returns -1 and marks the
response failed; the caller still owns the resource and must release it.
`capi_fail(ctx)` explicitly marks a response for clean 500 substitution.
The first error is retained for `capi_request_error` and `capi_on_error`.
Use `capi_on_finish` for final logging; code after next runs before async
completion and response-failure substitution. See [request reports](reliability.md).

## Reading a request

- `capi_request_method`: original method spelling.
- `capi_path`: raw target path, excluding the query string.
- `capi_header`: case-insensitive lookup, first matching field, raw trimmed value.
- `capi_param`: route capture, or `{NULL, 0}` if absent.
- `capi_body`: complete buffered body; binary-safe.
- `capi_query`: decoded first value for a query key.
- `capi_form`: decoded first value for a URL-encoded form body.

```c
capi_str name;
capi_input_result found = capi_query(ctx, "name", &name);
if (found == CAPI_INPUT_NO_MEMORY) return; /* already marked failed */
if (found < 0) { capi_text(ctx, 400, "Invalid query\n"); return; }
if (found == CAPI_INPUT_MISSING) name = (capi_str){"world", 5};
```

Query keys and values use percent decoding and `+` to space conversion.
An empty value is found with length 0. Repeated keys return the first value.
Malformed key encodings encountered during the search, or a malformed matching
value, return `CAPI_INPUT_ENCODING`. Unrelated values are not decoded. NUL bytes are preserved;
there is no automatic UTF-8 validation until `capi_json_string` is used.

`capi_form` uses the same decoder on the buffered body and requires a
case-insensitive `application/x-www-form-urlencoded` Content-Type. Parameters
such as `; charset=UTF-8` are accepted; absent/other content types return `CAPI_INPUT_CONTENT_TYPE`.
It has the same found/absent/malformed return convention and first-value
behavior as query lookup. Multipart forms are not implemented.

| Result | Meaning |
| --- | --- |
| `CAPI_INPUT_FOUND` (1) | Value found or conversion succeeded |
| `CAPI_INPUT_MISSING` (0) | No matching field/capture |
| `CAPI_INPUT_ENCODING` (-1) | Malformed percent encoding encountered |
| `CAPI_INPUT_CONTENT_TYPE` (-2) | Form content type absent/unsupported |
| `CAPI_INPUT_RANGE` (-3) | Integer overflow or value outside inclusive bounds |
| `CAPI_INPUT_NO_MEMORY` (-4) | Arena limit/allocation failure; response already failed |
| `CAPI_INPUT_VALUE` (-5) | Empty or malformed decimal integer |
| `CAPI_INPUT_ARGUMENT` (-6) | Invalid API argument or minimum greater than maximum |

`capi_str_i64` converts an explicit-length string; `capi_param_i64`,
`capi_query_i64`, and `capi_form_i64` combine lookup and conversion. They accept
decimal digits with an optional `+`/`-`, including leading zeroes, and reject
whitespace, hex, fractional values, trailing bytes, and embedded NULs. Path
captures remain raw (`%31` is not converted to `1`). Query/form `+` becomes a
space during decoding; encode a numeric plus sign as `%2B`.

```c
int64_t page;
capi_input_result result = capi_query_i64(ctx, "page", 1, 1000, &page);
if (result == CAPI_INPUT_MISSING) page = 1;
else if (result == CAPI_INPUT_NO_MEMORY) return;
else if (result != CAPI_INPUT_FOUND) { capi_text(ctx, 400, "Invalid page\n"); return; }
```

Every unsuccessful integer conversion zeroes the output when non-NULL. String
lookups reset their output to `{NULL, 0}` on failure/missing. Input mistakes
alone do not mark the request failed; the application chooses its 400/415/422
response. `capi_input_name` provides a static name for each result.

## Writing a response

`capi_status` changes the status (200–599). Default: 200. `capi_write` appends
bytes; `capi_printf` appends formatted bytes. Format strings must be trusted,
just as with standard `printf`.

`capi_text`, `capi_html`, and `capi_json` set status and content type, **replace the body**, and
preserve other headers. `capi_json` accepts already serialized JSON; it does
not parse or validate it. `capi_json_string` appends a quoted JSON string,
escaping control bytes, quotes, and backslashes, and rejecting invalid UTF-8.
Build larger objects by combining literal JSON with `capi_json_string`.

```c
capi_json(ctx, 201, "{\"name\":");
capi_json_string(ctx, user_supplied_name);
capi_write(ctx, "}", 1);
```

`capi_set_header` copies name and value, replaces existing names case-insensitively,
and rejects control-character injection. It supports up to 64 distinct fields.
`Content-Length`, `Transfer-Encoding`, `Connection`, `Trailer`, and `Upgrade`
are reserved. Repeated response fields such as multiple `Set-Cookie` headers
are not supported in this release. `capi_redirect` accepts 300–399 excluding 304,
sets `Location`, and clears the body.

Writer/header/encoding errors mark the request as failed. Even if a handler
ignores a return value, finalization substitutes a clean 500 and closes the
connection. A small fixed 500 error body may exceed a custom body limit smaller
than that error message. Allocation failure while serializing can only close
the connection.

No bytes leave the process until the pipeline completes, so middleware can set
headers after `capi_next`. 204 and 304 responses omit body and Content-Length;
205 sends an empty body with Content-Length 0. HEAD reports the size of the body
the corresponding handler generated, then omits those bytes.

## Socket-free tests

```c
const char request[] = "GET / HTTP/1.1\r\nHost: test\r\n\r\n";
capi_result result;
int parsed = capi_dispatch(app, request, sizeof(request) - 1, &result);
/* parsed == 1: result.data/len contain a complete response.
 * parsed == 0: request is incomplete.
 * parsed == -1: invalid API arguments or unrecoverable allocation failure. */
capi_result_free(&result);
```

HTTP errors return 1 with an error response. `result.consumed` indicates how
many bytes belong to the first request. For fatal parsing errors, it consumes
the supplied bytes and sets `result.close`. Successful dispatch can leave
pipelined bytes unconsumed. Result bytes are not NUL-terminated. Always free a
result before reusing it. API arguments must be valid; NULL is accepted only
where the header documents it.

## Server and limits

```c
capi_limits limits = capi_limits_default();
limits.max_arena_bytes = 512 * 1024;
if (capi_app_limits(app, limits)) { /* handle configuration error */ }
capi_server *server = capi_server_listen(app, (capi_server_options){
    .host = "127.0.0.1", .port = 8080,
    .max_connections = 4096,
    .idle_timeout_ms = 15000,
    .request_timeout_ms = 30000,
    .shutdown_timeout_ms = 5000,
    .async_timeout_ms = 30000
});
```

Zero server option fields select defaults, except port 0, which requests an
ephemeral port. Host defaults to loopback; set `"0.0.0.0"` or `"::"` explicitly
for external access. IPv6 dual-stack behavior depends on the operating system.
Host strings are only used during listen setup. Header limits include the
request line and terminator; at most 64 request headers are accepted.
Header limits range from 128 bytes to 1 MiB. Body limit 0 permits empty bodies;
response limits must be nonzero. Overflowing limit combinations are rejected.
The request arena has its own default 1 MiB budget; `max_arena_bytes = 0`
selects that default. Reserved blocks, alignment, and block headers count toward
the budget. Input/response buffers and external application allocations are
separate. See [arena budgeting](reliability.md#arena-memory-budget).

`capi_server_port` reports the bound port. `capi_server_run` blocks until stopped
or an event-loop error occurs. `capi_server_stop` wakes the reactor. Stop closes
the listener and idle/incomplete connections, lets queued responses drain, then
closes remaining connections at the shutdown deadline. A stopped server cannot
be restarted. Pending async requests are cancelled; run additionally waits for
their producers and cleanup even beyond the socket drain deadline. Free the
server before freeing its app. See the examples for
SIGINT/SIGTERM handling.
