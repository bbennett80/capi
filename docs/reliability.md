# Errors, request reports, and safer APIs

capi 0.4 adds compiler checks, structured request errors, transport-finished
reports, a bounded request arena, and checked input conversion. These facilities
belong to the dependency-free core; applications opt into callbacks explicitly.

## Compiler checks

GCC and Clang check `capi_printf` arguments using the same format rules as printf:

```c
capi_printf(ctx, "%zu", count); /* count must be size_t */
```

They also warn when critical results are discarded: app/route configuration,
cleanup registration, input lookup/conversion, request dispatch, server creation
and execution, worker submission, and SQLite operations. Handle those results.
Writers such as `capi_write`, `capi_text`, and `capi_set_header` still permit
ignored results because their failures automatically mark the request failed.

`CAPI_NODISCARD` and `CAPI_PRINTF` expand to compiler attributes on supported
compilers and to nothing elsewhere. No runtime code or dependency is added.
The compiler-contract tests verify both valid usage and rejected misuse,
including wrong format argument types and unchecked route/worker/commit results.

## Structured errors

```c
#include <capi.h>
#include <inttypes.h>
#include <stdio.h>

static void failed(uint64_t id, const capi_error *error, void *data) {
    (void)data;
    fprintf(stderr, "[%" PRIu64 "] %s in %s: %s\n", id,
            capi_error_name(error->code), error->operation, error->message);
}

/* During setup: */
capi_on_error(app, failed, NULL);
```

Errors carry a category, copied operation name (up to 63 bytes), static message,
and an OS errno value when available. Categories distinguish invalid response
headers/status, reserved headers, header/body limits, arena limits, allocation
failures, formatting, invalid JSON UTF-8, template failures, database cleanup,
and unexpected I/O errors. Request bodies/header values are never automatically
included in error details.

The first failure wins and invokes the callback once. Later failures cannot
obscure it. `capi_request_error(ctx)` reads the same error during the pipeline.
`capi_fail(ctx)` records an application failure; `capi_fail_with(ctx, code,
"operation")` records a more specific application-supplied reason. The operation
name is copied without arena allocation, so reporting still works after a
memory-budget or allocation failure.

Response-construction failures substitute a clean 500 and close the connection.
If serialization itself cannot allocate, the connection closes without a complete
response. A transport failure cannot replace an already serialized/sent status.
Errors stay in developer callbacks; clients do not receive internal details.

Normal HTTP 400/404/405 responses, timeouts, peer disconnects, and shutdown are
represented by status/outcome reports rather than treated as framework errors.
An application deliberately returning a 500 with `capi_text` also does not create
an internal error automatically; use `capi_fail_with` when a diagnostic is desired.

## Request-finished reports

```c
static void finished(const capi_request_report *r, void *data) {
    (void)data;
    fprintf(stderr, "[%" PRIu64 "] %d %s (%" PRIu64 " ms, %zu bytes)\n",
            r->id, r->status, capi_outcome_name(r->outcome), r->duration_ms,
            r->bytes_sent);
}
capi_on_finish(app, finished, NULL);
```

A report fires exactly once when a response has been handed to the socket or
an active request's connection is closed. It also covers incomplete requests
that time out or disconnect. Empty idle connections produce no request report.
`capi_dispatch` emits a report once a complete result or unrecoverable dispatch
failure is available; incomplete input emits none.

| Field | Meaning |
| --- | --- |
| `id` | Incrementing per-app request ID; completion order can differ from assignment order |
| `method`, `path` | Borrowed input views; may be empty for incomplete/malformed requests |
| `status` | Serialized response status, or 0 if no response was prepared |
| `outcome` | `sent`, `dispatched`, `client_closed`, `timeout`, `shutdown`, or `error` |
| `duration_ms` | Time from becoming the current request through transport completion |
| `bytes_sent` | Wire bytes accepted by the OS socket, including headers; 0 for in-memory dispatch |
| `response_bytes` | Complete serialized wire size, or 0 if serialization never completed |
| `arena_bytes` | Peak reserved request-arena bytes at report time |
| `error` | Snapshot of the first internal error known at report time |

Sent bytes do not prove the remote client read them. A sent 500 still has the
`sent` outcome; its HTTP status and error category describe the application
result. Partial writes followed by a disconnect retain the prepared status,
target wire size, and actual bytes accepted. Buffered pipelined requests start
their duration when the preceding response drains and they become current.

`capi_request_id(ctx)` provides the same ID in handlers, middleware, and worker
completions. IDs are allocated atomically per app, so shared apps can correlate
requests across event-loop threads. They are local identifiers, not globally
unique tracing IDs or authentication tokens.

## Asynchronous work and callback lifetime

Middleware's code after `capi_next` executes before background completion, and
before final response-failure substitution. Use the finished hook for final
logging and metrics. The [API example](../examples/api.c) demonstrates this.

A cancelled async request is reported when its socket closes. Its arena and
resources remain alive until the producer finishes and deferred cleanup runs.
That later cleanup does not emit another finish report. A cleanup error can
invoke the error callback after the finish report, under the same request ID.
Finish reports are immutable snapshots and are not retroactively updated.

Callbacks run on the event-loop thread, or on the caller thread for in-memory
dispatch. A shared app may invoke callbacks concurrently on its different loops;
synchronize shared callback data. Keep callbacks short. Do not reenter dispatch
or reconfigure/free the active app/server from a callback. `capi_server_stop` is
allowed. Reports, errors, and their string views are borrowed for the callback;
copy any fields you retain. There is no context pointer to mutate a finished
response or accidentally access state already released by cleanup.

## Arena memory budget

```c
capi_limits limits = capi_limits_default();
limits.max_arena_bytes = 512 * 1024;
if (capi_app_limits(app, limits)) { /* handle invalid configuration */ }
```

The default request-arena limit is 1 MiB; an explicit zero selects that default.
There is no implicit unlimited setting. The budget counts reserved payload,
alignment padding, and capi's arena block headers. It excludes libc allocator
bookkeeping, the context itself, input/response buffers (which have their own
limits), and application heap allocations outside `capi_alloc`.

Blocks grow lazily. Small budgets reduce the usual block size rather than
allocating a full block beyond the limit. An allocation of N bytes requires
space for alignment and block overhead as needed; setting a budget to N does
not guarantee that allocation fits. `capi_alloc(ctx, 0)` still returns NULL
without an error. Exceeding the budget records `memory_limit`, returns NULL,
and causes normal 500 substitution. Physical allocation failure is separately
reported as `out_of_memory`.

The budget remains in force through background work and cleanup. Copied headers,
decoded input values, deferred cleanup records, SQLite row copies, and large
formatted-output temporaries all consume it. Looking through nonmatching query
keys no longer allocates. Large responses can use `capi_write` without requiring
an equally large arena formatting temporary.

## Upgrading from 0.3

- Rebuild applications against the new headers/libraries. Pre-1.0 minor releases
  now use distinct shared-library ABI names (`0.4`) so existing 0.3 binaries
  cannot silently load a library with changed public struct layouts.
- Include `<capi_async.h>` explicitly if building a custom executor adapter.
  The common `<capi.h>` and `<capi_workers.h>` no longer expose raw async tokens.
  Worker applications need no advanced header.
- Input helpers now return `capi_input_result`. FOUND/MISSING remain 1/0 and
  every error is negative. Use named values or `< 0`; do not assume every error
  equals -1. See the [input result table](api.md#reading-a-request).
- Prefer `capi_limits_default()` plus named field overrides. Existing positional
  initializers need a fourth arena-budget field to avoid missing-field warnings;
  zero selects the new default.
- Handle results marked `CAPI_NODISCARD`. Ignoring response-writer results is
  still supported; their failures taint the response automatically.
- Move final logging from code after `capi_next` to `capi_on_finish`.

The test suite includes allocation fault injection against a private test build
of the core. Production allocation remains direct libc calls, with no test
allocator or runtime indirection installed.
