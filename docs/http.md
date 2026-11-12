# HTTP behavior and operations

capi is a buffered HTTP/1 origin server. Use it directly on loopback or behind
a reverse proxy. Choose a bind address, limits, and process count for your service.

## Framing rules

| Input | Behavior |
| --- | --- |
| HTTP/1.1 origin-form target (`/path?query`) | Accepted; exactly one nonempty Host required |
| HTTP/1.0 | Accepted; Host optional |
| Absolute-form, CONNECT authority-form, or `OPTIONS *` | 400 |
| Unsupported HTTP version | 505 |
| Bare LF, obsolete folding, invalid field names, control bytes in values | Rejected (400 or header-limit/deadline close if incomplete) |
| Duplicate Host or duplicate Content-Length | 400, including identical lengths |
| Transfer-Encoding with Content-Length | 400 |
| Transfer-Encoding alone | 501; connection closes |
| Expect header | 417 immediately after headers, before waiting for the body |
| Body larger than configured limit or overflowing Content-Length | 413 |
| Too many/too large headers | 431 |
| CR/LF in response headers, reserved framing headers | Clean 500 and close |

Request parsing is length-based and does not mutate the input. Duplicate
ordinary headers are retained, with first-value lookup. Connection tokens
are combined across fields; `close` wins over `keep-alive`. HTTP/1.1 defaults
to persistence; HTTP/1.0 requires explicit `keep-alive`.

capi owns response framing and always knows body length before sending.
It generates Date unless the application supplied one. Invalid requests never
continue into the middleware pipeline. After a fatal parsing error the
connection closes, so unread bytes cannot become another request.

URL percent escapes remain encoded during routing. There is no automatic path
normalization, directory serving, filesystem traversal, or proxy-header trust.
When composing paths for application storage, validate them for that specific
storage interface. capi does not interpret `Forwarded` or `X-Forwarded-*`.

## Concurrency and memory

The reactor uses level-triggered readiness and nonblocking sockets. A single
thread accepts connections and runs handlers. Requests on each connection run
sequentially, and pipelined responses preserve order. Each event has a bounded
read/write budget to prevent one busy connection monopolizing the loop.

The implementation reads until one complete request is ready, buffers its
response, then stops reading until that response drains. A connection can hold
up to the configured header plus body limit of input and up to the configured
response body plus response headers of output. Temporary handler output is
also buffered separately; plan for roughly input + twice the response size,
plus request arena allocations, at a connection's peak. Buffers grow lazily
and are retained for keep-alive reuse, then freed when the connection closes.
There is no automatic shrink policy.

`max_connections` bounds accepted sockets (maximum 65536); it does not preallocate
body buffers. At capacity, newly accepted sockets close immediately. OS file
descriptor limits also apply. The app controls arena allocations; their total
is not independently capped. Keep request handlers' allocations bounded.

Idle deadlines apply to read and write inactivity. An additional total receive
deadline limits slow request trickling, even when bytes keep arriving. Deadline
checks run at roughly 50 ms granularity. Timeout connections close without an
HTTP response. A partially received pipelined request starts its receive deadline
when it becomes the current request after the preceding response drains.

## Deployment model

Place TLS, public certificate management, and HTTP/2 termination at a reverse
proxy. Configure that proxy's request framing and size limits consistently
with capi's documented rules. Preserve the required Host field. Let the proxy
handle features the application needs that capi does not implement, such as
compression, and configure appropriate upstream deadlines.

For CPU parallelism, run independent processes on separate upstream ports.
For shared immutable routes on separate server threads, synchronize any shared
application state yourself. The framework does not spawn worker threads.
Blocking database calls or filesystem I/O block all connections on that loop;
this release does not expose an asynchronous handler completion API.

Stop through `capi_server_stop` on SIGINT/SIGTERM, then let `capi_server_run`
return before freeing resources. Queued responses drain; incomplete requests
are discarded. Long-running synchronous handlers cannot be preempted by a
shutdown or timeout and must return before the loop can act.
