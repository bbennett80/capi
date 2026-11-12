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

For a full repeated matrix, open the [HTML performance notebook](benchmarks/2026-11-11/index.html).
Its [raw CSV](benchmarks/2026-11-11/results.csv) contains every measured run,
and the [environment record](benchmarks/2026-11-11/environment.txt) captures
the machine, compiler, core revision, SQLite version, and benchmark source hashes.

To reproduce the report from the repository root:

```sh
./tools/measure.sh docs/benchmarks/my-machine
open docs/benchmarks/my-machine/index.html # macOS; otherwise open in a browser
```

This builds two Release configurations (native reactor and forced poll), starts
an isolated server on an OS-assigned loopback port, and runs six workloads at
1, 8, 32, 64, and 256 persistent connections. Three rounds produce 180 measured
runs of 100,000 requests each, with 5,000 warmup requests before every run.
The second round reverses workload/concurrency order; backends run sequentially.
The script stops its own server and generates a self-contained HTML/CSS/SVG page
using C tooling. SQLite development headers, CMake, a C11 compiler, and a POSIX
shell are required. Allow several minutes and avoid competing CPU work.

For a quicker smoke check, set `CAPI_PERF_REQUESTS=1000` before the command;
the CSV and page reflect the actual request count. The generator refuses an
incomplete or duplicate matrix. To regenerate only the HTML after editing its
presentation:

```sh
./build-perf-native/capi_perf_report \
  docs/benchmarks/2026-11-11/results.csv \
  docs/benchmarks/2026-11-11/environment.txt \
  docs/benchmarks/2026-11-11/index.html
```

The fixture measures plain text, static JSON, a parameterized route plus query
decoding/middleware/JSON escaping, a 16 KiB body, a compiled ten-item HTML
template, and a ten-row SQLite query plus JSON serialization. SQLite is seeded
with 1,000 rows in memory. This is a read-only adapter benchmark, not a disk,
durability, contention, or complete Notes app benchmark. Both processes share
the host; the client can limit throughput. Median throughput and the observed
min/max range are reported per configuration. Latency summaries are medians of
per-run percentiles, not percentiles pooled across all requests.

For a single endpoint measurement:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/capi_hello 8080
# In a second terminal:
./build/capi_bench 127.0.0.1 8080 1 10000
./build/capi_bench 127.0.0.1 8080 64 100000
# Optional request target and machine-readable output:
./build/capi_bench 127.0.0.1 8080 64 100000 / --csv
```

The bundled C client uses nonblocking sockets and poll, requests a selected path
(default `/`) over HTTP/1.1, validates 200 responses and their lengths, and sends the next request
only after the previous response completes. It supports IPv4 and responses
smaller than 64 KiB. Connections are established before timing starts. Latency
includes client scheduling, socket traffic, and server time, but not connect
latency. Quantiles use sorted per-request samples.

## Repeated 0.4 measurements

The November 11, 2026 matrix on an Apple M1 (8 cores, 8 GiB RAM), macOS 27.0.1,
Apple Clang 21.0.0, completed all 180 measured runs and 18 million requests
without a failed run. Measured intervals totaled 266.55 seconds, excluding
warmup, connection setup, sorting, builds, and reporting. At 64 connections:

| Workload | kqueue median req/s | poll median req/s | kqueue median p99 | poll median p99 |
| --- | ---: | ---: | ---: | ---: |
| Plain text | 147,898 | 157,337 | 0.625 ms | 0.552 ms |
| Static JSON | 152,742 | 147,037 | 0.715 ms | 0.749 ms |
| Route + query + middleware | 146,759 | 146,773 | 0.595 ms | 0.650 ms |
| 16 KiB response | 102,865 | 114,510 | 1.146 ms | 0.855 ms |
| Ten-item template | 114,634 | 116,404 | 0.793 ms | 0.813 ms |
| Ten-row SQLite read + JSON | 75,307 | 78,586 | 1.183 ms | 1.150 ms |

Each value is a median across three runs; p99 columns are medians of per-run
p99 values. The [HTML notebook](benchmarks/2026-11-11/index.html) shows every
connection level and throughput range. Backend order was not randomized and
the client shares the machine, so the differences do not establish that one
backend is faster in general. Long tails appeared at 256 connections in several
cases; peak throughput alone does not describe the latency tradeoff.

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

For 0.4, all ten suites passed in native, poll, shared, ASan/UBSan, and separate
ThreadSanitizer builds. The dependency-free core-only build passed its five
suites. New checks cover compiler diagnostics, allocation fault injection,
arena budgets, integer conversion, first-error preservation, and exactly-once
transport reports, including cancellation followed by late cleanup errors.
Installed static/shared consumers and a freshly scaffolded application compiled;
the shared libraries use the distinct 0.4 ABI name. Direct C11 compilation of
the core and hello example also succeeded.

HTTP/input fuzzing completed 523,031 inputs and template fuzzing completed
473,796 inputs, each in 21 seconds, without a crash or sanitizer diagnostic.
A short Release/kqueue loopback run (64 connections, 100,000 requests) measured
150,387 requests/s, p50 0.359 ms, p95 0.495 ms, and p99 0.736 ms. This remains
a single local sanity check, not a controlled performance comparison. These
checks ran on macOS; the Linux CI job was not run locally.
