# capi

**Small surface. Fast core. Just C.**

capi is a C11 web framework for HTTP APIs and small services, with a
dependency-free core and optional SQLite, HTML template, and worker libraries.
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
  common headers. Prefixes match whole path segments. Short-circuit by returning.
  Finished-request hooks report final outcomes for both sync and async work.
- **Explicit memory ownership.** Request views reference the input buffer.
  Request allocations come from an aligned, zeroed arena and disappear after dispatch.
  Connection buffers are reused across requests. No per-request routing allocation.
- **Useful response helpers.** Text, JSON, redirects, binary bodies, formatted
  output, copied headers, and an escaping helper that validates UTF-8.
- **Mistakes you can diagnose.** Compiler format/return-value checks, named
  input results, checked integer conversion, structured first-error callbacks,
  per-app request IDs, finished-request reports, and an arena memory budget.
- **Optional application libraries.** Native SQLite integration with request
  cleanup and explicit transactions, plus compiled HTML templates with escaping,
  sections, loops, and partials. A bounded worker pool keeps blocking work off
  the event loop and resumes responses there. Core-only applications link none
  of these libraries.
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
if (capi_use(app, "/", security_headers, NULL) ||
    capi_route(app, CAPI_GET, "/users/:id", user, NULL)) {
    /* Handle setup failure; inspect capi_app_error(app). */
}
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

Installed projects use `find_package(capi 0.4 CONFIG REQUIRED)` and
`target_link_libraries(app PRIVATE capi::capi)`. Plain compiler builds can use
`pkg-config --cflags --libs capi` with the install prefix's `lib/pkgconfig`
directory on `PKG_CONFIG_PATH`.

## Generate a CRUD API

New to capi? Follow [Build a todo app](examples/todo/README.md) from installation
through CLI route generation, HTTP requests, SQLite persistence, and a custom C
route.

With the SQLite component installed:

```sh
capi new my-api --sqlite
cd my-api
capi generate resource posts title:string body:text views:integer published:boolean
capi routes
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local"
cmake --build build
./build/app
```

The generator creates editable C controllers for list, show, create, PUT, PATCH,
and delete, using SQLite bound parameters, validated form input, JSON responses,
pagination, and explicit transactions. New resources are wired into the build
automatically. Existing controllers are never overwritten; add `--dry-run` to
preview generation. The schema bootstraps missing tables; schema migrations
remain explicit. See the [CLI quick start](docs/cli.md) for installation, curl
examples, field types, and customization.

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
find_package(capi 0.4 CONFIG REQUIRED COMPONENTS sqlite templates)
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

## Background work

Use the optional worker pool for blocking database/filesystem work and substantial
computations. Workers receive job data; completions run on the event loop to write
the response. Request memory stays alive until work finishes, including on timeout.
Queue capacity is bounded, overload is explicit, and cancellation is cooperative.

```sh
make workers
./build-workers/capi_background
# In another terminal:
curl --data-binary 'hello' http://127.0.0.1:8080/slow &
curl http://127.0.0.1:8080/health
wait
```

Enable `-DCAPI_WITH_WORKERS=ON` and link `capi::workers`. Installed consumers
request the `workers` component. The [worker guide](docs/workers.md) explains
ownership, deadlines, shutdown, and per-worker resources. The
[SQLite worker example](examples/async_sqlite.c) opens a separate connection per
worker and returns copied results through event-loop completions.

## Reliability and diagnostics

Use `capi_on_error` to receive the first internal failure and `capi_on_finish`
to observe final status, outcome, duration, and actual wire bytes sent. Reports
work for synchronous handlers, background completions, timeouts, and disconnects.
The [API example](examples/api.c) demonstrates final request logging.

Input helpers distinguish missing fields, bad encoding, unsupported content
types, invalid numbers, range errors, and allocation failures. For example:

```c
int64_t id;
if (capi_param_i64(ctx, "id", 1, INT64_MAX, &id) != CAPI_INPUT_FOUND) {
    capi_text(ctx, 400, "Invalid ID\n");
    return;
}
```

The request arena defaults to a 1 MiB budget, including block overhead. Configure
it through `capi_limits_default()` and `limits.max_arena_bytes`. GCC/Clang also
check `capi_printf` formats and warn about unchecked critical results.
See the [reliability guide and 0.3 migration notes](docs/reliability.md).

## Verify and measure

See the [performance notebook](docs/benchmarks/2026-11-11/index.html) for repeated
measurements of six workloads, concurrency scaling, latency, and raw data.
Run `./tools/measure.sh` to collect the matrix on your own machine; see
[performance and methodology](docs/performance.md) for details.

```sh
make check         # full local CI: builds, sanitizers, consumers, fuzzing
make check-quick   # Release + core-only checks
make hooks         # opt into staged-file pre-commit checks for this checkout
make test          # parser, API, routing, middleware, real sockets
make sanitize      # AddressSanitizer + UndefinedBehaviorSanitizer
make poll          # run the same suite against the fallback backend
make extras        # also test SQLite, templates, and the notes app
make workers       # add worker/lifetime/overload tests and async examples
make thread-sanitize # ThreadSanitizer, separate from ASan

# With capi_hello running:
./build/capi_bench 127.0.0.1 8080 64 100000
```

The local hook defaults to full verification and uses the same check script as
GitHub Actions. It runs without GitHub credits and checks a staged snapshot
without changing your working files. See [local CI setup](CONTRIBUTING.md#local-ci-and-pre-commit-checks)
for prerequisites, quick mode, logs, and disabling the hook.

The load tool reports throughput and p50/p95/p99 end-to-end latency, using
keep-alive with one in-flight request per connection. It is a development tool,
not an independent performance comparison. See [benchmark methodology](docs/performance.md).

## Scope

capi 0.4 is a compact foundation, with an intentionally small API. Handlers and
completions run on one event-loop thread: keep them short and move blocking work
to an explicitly configured worker pool.
Scale independent instances behind a proxy. There is no built-in TLS, HTTP/2,
WebSocket, streaming, compression, multipart parser, JSON parser, or durable job
system. The optional worker pool runs in-process; jobs do not survive a process
exit. The SQLite adapter is synchronous, with per-worker connections available
through the pool's lifecycle hooks. Use a TLS reverse proxy for HTTPS.

Request bodies require `Content-Length`; chunked transfer coding is rejected
with 501, `Expect` with 417, and ambiguous framing with 400. Bodies and responses
are buffered, with configurable limits. The API has not yet had an independent
security audit or a compatibility-stable release.

- [API and ownership guide](docs/api.md)
- [HTTP behavior and operational model](docs/http.md)
- [Errors, request reports, and safer APIs](docs/reliability.md)
- [Performance and benchmarking](docs/performance.md)
- [Contributing](CONTRIBUTING.md)

MIT licensed.
