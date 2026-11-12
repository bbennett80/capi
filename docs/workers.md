# Background work and asynchronous requests

`capi::workers` runs blocking work on a bounded POSIX thread pool and returns
completion callbacks to the HTTP event loop. Handlers remain ordinary C
functions. No HTTP context is passed to a worker.

```sh
make workers
./build-workers/capi_background
# In another terminal:
curl --data-binary 'hello' http://127.0.0.1:8080/slow &
curl http://127.0.0.1:8080/health
wait
```

The slow echo waits approximately 200 ms on a worker; health requests continue
on the event loop. To also build `capi_async_sqlite`, with separate database
connections per worker, run `make workers CMAKE_ARGS=-DCAPI_WITH_SQLITE=ON`.
See [the example](../examples/async_sqlite.c).

## Build and lifecycle

Enable `-DCAPI_WITH_WORKERS=ON` and link `capi::workers`. Only this library asks
CMake for system threads. The core does not link or create a worker pool.
Installed consumers use:

```cmake
find_package(capi 0.3 CONFIG REQUIRED COMPONENTS workers)
target_link_libraries(app PRIVATE capi::workers)
```

pkg-config consumers use `capi-workers`. The public header is
[`capi_workers.h`](../include/capi_workers.h).

```c
capi_workers *pool = capi_workers_new((capi_workers_options){
    .threads = 4,
    .capacity = 128
});
if (!pool) { perror("workers"); return 1; }
/* Register pool or an application struct as handler data. */
/* Run the server; stop it and let capi_server_run return. */
capi_server_free(server);
capi_workers_free(pool);
capi_app_free(app);
```

Zero values select 4 threads and 128 outstanding jobs. Maximums are 256 threads
and 65536 jobs. Capacity includes queued work, executing work, and completions
awaiting the event loop; a slow loop cannot create an unbounded completion
backlog. A pool can be shared by multiple servers, provided application data
is designed for that sharing. `capi_workers_get_stats` provides a thread-safe
snapshot of capacity, queued, running, and total outstanding work.

`capi_workers_free` stops acceptance and joins all workers. Call it after the
server returns in the normal lifecycle. Posted completions retain an internal
reference until the loop consumes them, but applications must not access or
submit to a pool after freeing their ownership. Never free from a worker
callback, and do not free a pool twice or concurrently with another free.

## Submitting work

```c
typedef struct { capi_str input; size_t result; } count_job;

static void count(capi_job *job, void *worker_state, void *data) {
    (void)worker_state;
    count_job *j = data;
    for (size_t i = 0; i < j->input.len; ++i) {
        if (capi_job_cancelled(job)) return;
        if (j->input.data[i] == '\n') ++j->result;
    }
}

static void counted(capi_ctx *ctx, void *data) {
    count_job *j = data;
    capi_json(ctx, 200, "");
    capi_printf(ctx, "{\"newlines\":%zu}", j->result);
}

static void endpoint(capi_ctx *ctx) {
    count_job *j = capi_alloc(ctx, sizeof(*j));
    if (!j) return;
    j->input = capi_body(ctx);
    capi_work_result rc = capi_work_submit(ctx, capi_data(ctx), count, counted, j);
    if (rc != CAPI_WORK_OK) {
        capi_set_header(ctx, "Retry-After", "1");
        capi_text(ctx, 503, "Work queue unavailable\n");
    }
}
```

This small computation illustrates ownership; tiny operations usually run
more efficiently directly in a handler. Use workers for work substantial
enough to justify scheduling, especially blocking storage operations.

| Return | Meaning |
| --- | --- |
| `CAPI_WORK_OK` | Accepted; ownership of submitted data transfers to work until completion |
| `CAPI_WORK_FULL` | Pool capacity reached; no callbacks run |
| `CAPI_WORK_UNAVAILABLE` | No live async-capable request, server stopping/async limit reached, or pool stopping |
| `CAPI_WORK_ERROR` | Invalid arguments or allocation failure; errno set |

Submission is allowed once per request, during a live server pipeline, on
the event-loop thread. It is unavailable in `capi_dispatch` or a completion
callback. Socket-free tests can assert an endpoint's overload fallback;
successful async flows use real socket integration tests.

## Ownership and execution order

Work can begin before the submitting handler returns. Initialize all job data
first and do not access or mutate it after successful submission until the
completion callback. Borrowed request views and request-arena allocations
remain valid, even when a connection closes or its slot is reused. Other
application pointers remain application-owned and must outlive the work.

Workers must not call context APIs, use an event-loop-owned database connection,
or retain their `capi_job *` beyond the work callback. They may use their own
thread state and publish results through job data. Completions run on the event
loop, after the pipeline has unwound, and may write headers/body, render a
template, or redirect. `capi_data(ctx)` is restored to the submitting handler's
registration data; `capi_state(ctx)` is preserved.

Middleware's code after `capi_next` runs before completion. It can set common
headers, but cannot inspect finished job results or log the eventual completion
status there. Log async results in completion callbacks. Request cleanup
registered with `capi_defer` runs after work/completion, on the event loop,
including cancellation paths. The request arena is freed only after cleanup.
If work allocates heap-owned results, register request cleanup before submitting
and free those results there, rather than only in done: done may be skipped on
cancellation, while cleanup still waits until the worker has returned.
Pipelined requests remain ordered; later requests on the same connection wait
for the pending response to finish.

## Cancellation, deadlines, and shutdown

`async_timeout_ms` in server options sets a total asynchronous request deadline
(default 30 seconds, checked at roughly 50 ms granularity). It replaces the
ordinary idle deadline while work is pending. Timeout closes the connection
and marks its token cancelled. Request resources remain pinned until the
producer completes, so a late worker cannot access freed input or arena memory.
Outstanding async handoffs are additionally bounded by `max_connections`, even
after socket slots are recycled.

Queued cancelled jobs skip their work callback. Running work can poll
`capi_job_cancelled` and return cooperatively; it must eventually return.
The user's completion callback is skipped for cancelled requests, while request
cleanup still runs. Cancellation does not undo already committed writes or
other external side effects. Use application transactions and explicit policies.

Pending sockets stop reading to provide backpressure. Disconnects are not
necessarily detected until the response write or async deadline, so work may
continue after a client disconnects. Cancellation is a lifetime and cooperative
execution mechanism, not a delivery guarantee.

Stop cancels pending async requests and drains already queued HTTP responses.
The shutdown timeout bounds socket draining. `capi_server_run` additionally
waits for every async producer to complete and for completion/cleanup to run,
even beyond that timeout. C callbacks cannot be safely forcibly interrupted.
Bound I/O and query durations and respond to cancellation; work that never
returns prevents shutdown. Keep app state, pools, and server alive until run
returns. Do not call `capi_server_free` concurrently with a running loop.

## Per-worker resources and SQLite

Pool options include `init(data, &worker_state)` and `dispose(worker_state)`.
Each runs on the same worker thread; non-NULL state is disposed even if that
initializer fails. Creation waits for all initializers, and any failure joins
every started thread before returning NULL. Initializers can run concurrently.

Open a separate SQLite connection in each initializer. In work callbacks use
native `sqlite3_prepare_v2` / `sqlite3_step` / `sqlite3_finalize` and the non-context
binding helpers. Request-scoped `capi_sqlite_prepare` and `capi_sqlite_copy_text`
require a context and must not be called on workers. Copy native column values
into job-owned memory before returning; SQLite column views cannot cross jobs.
Worker shutdown closes each connection on its owning thread.

The [SQLite worker example](../examples/async_sqlite.c) configures its schema/WAL
at startup, then uses four independent connections. GET `/messages` counts saved
messages; POST `/messages` stores the raw body; GET `/health` stays on the loop.
Lock contention returns 503 without automatic write retries.

## Low-level asynchronous handoff

The dependency-free core also provides `capi_async_begin`,
`capi_async_cancelled`, and `capi_async_complete` for adapters to other executors.
Begin on the event loop, arrange your own producer, and call complete exactly
once from that producer, even if cancelled. Completion consumes the token;
never access it, request-owned memory, or published results again. Begin can
fail if unavailable, at the async limit, or
on allocation failure. If scheduling fails after a successful begin, still
complete the token so the request and server can finish.

`capi_async_complete` enqueues a callback rather than calling it inline.
Its callback runs on the loop with the original context, or NULL if cancelled;
low-level producers must handle both cases and release their own result data.
Never use context APIs on producer threads. The core uses a C11 atomic completion
queue and a wake pipe; linking the core does not require a thread library.
