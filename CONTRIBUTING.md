# Contributing

Keep capi small, understandable, and dependency-free. Runtime code and tools
are C11. The public API is in `include/capi.h`; private interfaces stay in
`src/internal.h`. Prefer explicit lengths and ownership over hidden allocation.

Before a change:

```sh
make test
make sanitize
make poll
make extras         # requires SQLite development headers/libraries
make sanitize CMAKE_ARGS=-DCAPI_WITH_SQLITE=ON
make workers
make sanitize CMAKE_ARGS='-DCAPI_WITH_SQLITE=ON -DCAPI_WITH_WORKERS=ON'
make thread-sanitize
```

Add behavioral tests for routing, HTTP framing, lifecycle, and memory-safety
changes. Socket tests use only ephemeral loopback ports. Treat malformed traffic
as expected input and preserve strict, unambiguous framing.

Worker changes require both ASan/UBSan and a separate ThreadSanitizer build.
The worker suite covers queue saturation, responsive endpoints during blocked
work, completion ordering, cancellation with socket reuse, retained input,
queued-job skipping, initialization failures, concurrent producers, and shutdown.
ASan and ThreadSanitizer cannot be combined in one build.

For fuzzing, use a Clang distribution that includes libFuzzer (Apple's Command
Line Tools compiler may not). LLVM Clang works on macOS; Linux Clang works in CI.

```sh
CC=clang cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Debug \
  -DCAPI_BUILD_FUZZER=ON -DCAPI_BUILD_TESTS=OFF \
  -DCAPI_BUILD_EXAMPLES=OFF -DCAPI_BUILD_TOOLS=OFF
cmake --build build-fuzz -j
mkdir -p build-fuzz/corpus
printf 'GET / HTTP/1.1\r\nHost: test\r\n\r\n' > build-fuzz/corpus/get
./build-fuzz/capi_fuzz -max_total_time=60 -max_len=65536 build-fuzz/corpus
mkdir -p build-fuzz/template-corpus
printf '{{#items}}<p>{{name}}</p>{{/items}}{{>card}}' > build-fuzz/template-corpus/view
./build-fuzz/capi_template_fuzz -max_total_time=60 -max_len=65536 build-fuzz/template-corpus
```

The HTTP harness runs the complete in-memory dispatch path, including decoded
query/form values and JSON escaping. The template harness compiles arbitrary
sources and renders successful compilations with sections, lists, escaped values,
and recursive partials. Minimize crashes and add the input as a regression
test. Do not commit generated fuzz corpora or build products.

Report performance changes with compiler flags, machine details, request shape,
connection count, and a reproducible command. No unmeasured speed claims.
