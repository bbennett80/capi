# Performance

capi's fast path is deliberately direct: parse views over buffered input,
walk a segment trie, invoke the middleware/handler chain, serialize a response,
and drain it through the reactor. Static response bodies are copied into
reusable connection buffers. Route matching and request parsing do not allocate.
Response headers and explicit request allocations use a request-scoped arena.

Linux uses epoll, macOS/BSD use kqueue, and the poll backend is available with
`-DCAPI_FORCE_POLL=ON`. Kernel-backed backends wait on ready descriptors instead
of scanning every connection for readiness. Timeout maintenance scans the
connection table at roughly 50 ms intervals, rather than on every request.

## Reproduce a measurement

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/capi_hello 8080
# In a second terminal:
./build/capi_bench 127.0.0.1 8080 1 10000
./build/capi_bench 127.0.0.1 8080 64 100000
```

The bundled C client uses nonblocking sockets and poll, requests `/` over
HTTP/1.1, validates 200 responses and their lengths, and sends the next request
only after the previous response completes. It supports IPv4 and responses
smaller than 64 KiB. Connections are established before timing starts. Latency
includes client scheduling, socket traffic, and server time, but not connect
latency. Quantiles use sorted per-request samples.

## Initial local measurement

Measured capi 0.1.0 on November 11, 2026, on an arm64 MacBook Air, Darwin 27.0.0,
Apple Clang 21.0.0, CMake Release build (`-O3 -DNDEBUG`), kqueue. Server and client
ran on the same machine over loopback. The handler used `capi_text` to return
the 17-byte `Hello from capi!\n` body, with Content-Type, Date, Content-Length,
and Connection headers. One event loop, no TLS, no logging, no pipelining.

| Connections | Requests | Elapsed | Requests/s | p50 | p95 | p99 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 10,000 | 0.269 s | 37,172 | 0.020 ms | 0.033 ms | 0.111 ms |
| 64 | 100,000 | 0.683 s | 146,463 | 0.378 ms | 0.502 ms | 0.866 ms |

These are short, single local runs, not capacity guarantees or comparisons
against other frameworks. The load generator and loopback stack share the
machine's CPU and can limit throughput. For deployment decisions, use a longer
run from a separate host with representative middleware, payload sizes,
connection patterns, proxying, and business logic. Repeat runs and report
variance, errors, memory usage, and tail latency.

## Practical choices

- Keep handlers short. Blocking storage/network work stalls the event loop.
- Use Release builds when measuring; sanitizer builds serve a different purpose.
- Choose connection/body/response limits based on peak retained buffer memory.
- Tune the process's file descriptor limit for the intended connection count.
- Scale using independent processes and upstream load balancing.
- Measure the full service before adding complexity to the framework.

## Validation recorded locally

The native kqueue and fallback poll suites both passed, including
AddressSanitizer/UndefinedBehaviorSanitizer on the native backend. Clang static
analysis was run on the library. A Clang 22.1.6 libFuzzer run completed 997,495
inputs in 31 seconds without a crash, seeded with valid requests and an ambiguous
framing request. Installation and compilation of a generated consumer were
checked in an isolated prefix.

The Linux epoll backend has a CI job; it was not executed on the macOS
development host. Fuzz smoke testing and sanitizer passes are evidence about
the exercised cases, not a security audit.

For 0.2, the SQLite adapter, templates, form decoder, and notes integration tests
passed in sanitizer, poll, and shared builds; the core-only build also passed
without either extension. Installed core-only and component consumers compiled
and ran. Clang static analysis found no diagnostics in the changed runtime code.
A template fuzz run completed 938,587 inputs in 31 seconds, and an HTTP fuzz run
including form decoding completed 647,335 inputs in 21 seconds, without crashes.
The notes form/redirect/escaping flow was also checked over a real loopback HTTP
connection. These are local macOS checks; Linux is covered by the configured CI.

For 0.3, the worker, async SQLite, core, network, template, and notes suites
passed in native, poll, shared, ASan/UBSan, and separate ThreadSanitizer builds.
Core-only builds still passed without optional libraries. Installed static and
shared consumers ran with the worker component, and core consumers required no
SQLite or thread package. The worker suite exercises four simultaneous producers,
640 stress requests, queue saturation, cancellation with socket reuse, retained
body views, per-thread resource disposal, and shutdown beyond the socket deadline.
An additional HTTP fuzz run completed 525,038 inputs in 21 seconds without a crash.

A short 0.3 Release/kqueue loopback check (64 connections, 100,000 requests)
measured 145,155 requests/s, p50 0.362 ms, p95 0.538 ms, and p99 0.935 ms. This
single run is a sanity check, not a statistically controlled comparison. In the
background-work example, a health request completed in 0.652 ms while a concurrent
artificially delayed echo took 268 ms, confirming that the delay stayed off the
event loop. CPU/storage-heavy real workloads still need their own measurements.
