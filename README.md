# capi

**Small surface. Fast core. Just C.**

capi is a C11 web framework for HTTP APIs and small services, with a
dependency-free core and optional SQLite and HTML template libraries.
Write a handler, register a route, start a server. No code generation, runtime,
package manager, or global application singleton.

```c
#include <capi.h>

static void hello(capi_ctx *ctx) {
    capi_text(ctx, 200, "Hello from capi!\n");
}

int main(void) {
    capi_app *app = capi_app_new();
    if (!app) return 1;
    if (capi_route(app, CAPI_GET, "/", hello, NULL)) {
        capi_app_free(app);
        return 1;
    }
    capi_server *server = capi_server_listen(app,
        (capi_server_options){.port = 8080});
    if (!server) { capi_app_free(app); return 1; }
    int result = capi_server_run(server);
    capi_server_free(server);
    capi_app_free(app);
    return result ? 1 : 0;
}
```

## Run it

Requires a C11 compiler, CMake 3.16+, and a POSIX system. Linux uses **epoll**,
macOS/BSD use **kqueue**, and other POSIX systems use **poll**. No external libraries.

```sh
make test
./build/capi_hello
# In another terminal:
curl http://127.0.0.1:8080/
```

Or build directly, without CMake:

```sh
cc -std=c11 -O2 -Iinclude src/*.c examples/hello.c -o hello
./hello
```

## What you get

- **A nonblocking event loop.** Persistent HTTP/1.0 and HTTP/1.1 connections,
  ordered pipelining, bounded buffers, read/write backpressure, deadlines,
  connection limits, and graceful response draining.
- **A segment trie router.** Literal routes, `:parameters`, and `*catch_all`
  suffixes. Literal segments win over parameters, which win over catch-alls.
  HEAD falls back to GET; OPTIONS and 405 responses include `Allow`.
- **Middleware that composes.** Call `capi_next(ctx)`; unwind afterward for
  logging and headers. Prefixes match whole path segments. Short-circuit by returning.
- **Explicit memory ownership.** Request views reference the input buffer.
  Request allocations come from an aligned, zeroed arena and disappear after dispatch.
  Connection buffers are reused across requests. No per-request routing allocation.
- **Useful response helpers.** Text, JSON, redirects, binary bodies, formatted
  output, copied headers, and an escaping helper that validates UTF-8.
- **Optional application libraries.** Native SQLite integration with request
  cleanup and explicit transactions, plus compiled HTML templates with escaping,
  sections, loops, and partials. Core-only applications link neither library.
- **Tests without sockets.** `capi_dispatch` runs the real parser, router,
  middleware, and serializer against an in-memory HTTP request.
- **A complete development path.** CMake and pkg-config installation,
  `capi new`, examples, a C load generator, sanitizer tests, and a fuzz harness.

## Routes and middleware

```c
static void user(capi_ctx *ctx) {
    capi_str id = capi_param(ctx, "id");
    capi_json(ctx, 200, "{\"id\":");
    capi_json_string(ctx, id);
    capi_write(ctx, "}", 1);
}

static void security_headers(capi_ctx *ctx) {
    capi_set_header(ctx, "X-Content-Type-Options", "nosniff");
    capi_next(ctx);
}

/* During setup: */
capi_use(app, "/", security_headers, NULL);
capi_route(app, CAPI_GET, "/users/:id", user, NULL);
```

Handlers receive their registration data through `capi_data(ctx)`. Middleware
can share per-request application state through `capi_set_state` / `capi_state`.
All registration calls return `0` on success, `-1` on failure; inspect
`capi_app_error(app)` for the reason. Check registration errors during setup.

The [API example](examples/api.c) includes request IDs, structured errors,
query decoding, a byte-preserving echo endpoint, and illustrative bearer auth:

```sh
./build/capi_api
curl 'http://127.0.0.1:8080/hello?name=Ada%20Lovelace'
curl http://127.0.0.1:8080/users/42
curl --data-binary 'hello' http://127.0.0.1:8080/echo
curl -H 'Authorization: Bearer local-demo' http://127.0.0.1:8080/private/status
```

## Use it in your project

Vendored CMake:

```cmake
set(CAPI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAPI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CAPI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/capi)
target_link_libraries(my_app PRIVATE capi::capi)
```

Or install into a prefix you own:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j
cmake --install build
"$HOME/.local/bin/capi" new my-service
cmake -S my-service -B my-service/build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build my-service/build
./my-service/build/app
```

Installed projects use `find_package(capi 0.2 CONFIG REQUIRED)` and
`target_link_libraries(app PRIVATE capi::capi)`. Plain compiler builds can use
`pkg-config --cflags --libs capi` with the install prefix's `lib/pkgconfig`
directory on `PKG_CONFIG_PATH`.

## SQLite and templates

Build the optional SQLite adapter and the [Fieldnotes example](examples/notes/README.md):

```sh
make extras
./build-extras/capi_notes
# Open http://127.0.0.1:8080
```

Fieldnotes saves notes in SQLite and renders them through compiled HTML templates,
with no frontend build step. It demonstrates bound parameters, explicit commits,
automatic cleanup/rollback, form decoding, redirects, partials, and escaped values.

Use `-DCAPI_WITH_SQLITE=ON` to enable the SQLite adapter; development headers and
libraries must be installed. Templates build as a separate library by default,
and can be disabled with `-DCAPI_WITH_TEMPLATES=OFF`. Installed apps request the
components they use:

```cmake
find_package(capi 0.2 CONFIG REQUIRED COMPONENTS sqlite templates)
target_link_libraries(app PRIVATE capi::sqlite capi::templates)
```

The SQLite adapter uses native statements and result codes. Statements finalize
after the request pipeline, and managed transactions roll back unless explicitly
committed. Queries execute synchronously; keep them short and handle contention
explicitly. See the [SQLite guide](docs/sqlite.md).

Templates compile once at startup. `{{name}}` escapes HTML; sections, lists,
dotted lookup, comments, and registered partials support ordinary pages without
an expression language. `{{{trusted_html}}}` explicitly opts out of escaping.
See the [template guide](docs/templates.md).

## Verify and measure

```sh
make test          # parser, API, routing, middleware, real sockets
make sanitize      # AddressSanitizer + UndefinedBehaviorSanitizer
make poll          # run the same suite against the fallback backend
make extras        # also test SQLite, templates, and the notes app

# With capi_hello running:
./build/capi_bench 127.0.0.1 8080 64 100000
```

The load tool reports throughput and p50/p95/p99 end-to-end latency, using
keep-alive with one in-flight request per connection. It is a development tool,
not an independent performance comparison. See [benchmark methodology](docs/performance.md).

## Scope

capi 0.2 is a compact foundation, with an intentionally small API. Handlers run
synchronously on one event-loop thread: keep them short and avoid blocking I/O.
Scale independent instances behind a proxy. There is no built-in TLS, HTTP/2,
WebSocket, streaming, compression, multipart parser, JSON parser,
or background task system. The optional SQLite library is a synchronous adapter,
not an asynchronous database layer. Use a TLS reverse proxy for HTTPS.

Request bodies require `Content-Length`; chunked transfer coding is rejected
with 501, `Expect` with 417, and ambiguous framing with 400. Bodies and responses
are buffered, with configurable limits. The API has not yet had an independent
security audit or a compatibility-stable release.

- [API and ownership guide](docs/api.md)
- [HTTP behavior and operational model](docs/http.md)
- [Performance and benchmarking](docs/performance.md)
- [Contributing](CONTRIBUTING.md)

MIT licensed.
