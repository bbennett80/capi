# Contributing

CLI templates live in `tools/templates` and are embedded into the C executable
at configure time. Edit those sources rather than generated headers. With tools
and SQLite enabled, `ctest --test-dir build -R '^cli$' --output-on-failure` generates, installs,
and compiles fresh applications, then exercises the emitted CRUD controllers.

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

## Local CI and pre-commit checks

GitHub Actions can stay disabled: the same verification commands run locally,
with no GitHub credits, downloads, or automatic package installation. Run:

```sh
make check        # full native-host CI checks
make check-quick  # Release + core-only checks
make hooks        # enable the tracked pre-commit hook for this checkout
```

`make hooks` configures this repository's local `core.hooksPath` as `.githooks`.
It refuses to replace an existing hook configuration or executable pre-commit
hook. Each clone opts in independently; Git does not activate hooks on clone.
The hook defaults to **full** mode and blocks the commit on any failure.

Full mode runs Release, ASan/UBSan, poll, core-only, separate ThreadSanitizer,
and two 30-second libFuzzer smoke tests. Release/sanitizer/poll phases also test
fresh installed packages, a generated starter, a core consumer with optional
dependency lookup disabled, and an all-components consumer. Generated CRUD
application tests are included. This takes a few minutes on a cold build.
Quick mode runs Release (including installed consumers) and core-only; it
explicitly omits sanitizers, poll, and fuzzing.

```sh
git config --local capi.preCommitMode quick  # optional, persist quicker checks
git config --local capi.preCommitMode full   # restore the default coverage
CAPI_CHECK_MODE=full git commit             # one-commit mode override
git config --local --unset core.hooksPath   # disable this hook
```

The hook exports a frozen snapshot of the **staged index** into ignored
`build-pre-commit/source`, runs that snapshot's check script, and keeps build
products under `build-pre-commit/build`. It never stashes, stages, formats, or
rewrites your source files. Partially staged files and untracked changes are
handled naturally. A changed index during verification blocks the commit so it
can be checked again. Logs remain at `build-pre-commit/last-run.log`; the final
80 lines are shown on failure. Empty commits skip the checks. Interrupted hooks
may leave `build-pre-commit/lock`; remove the empty lock directory only after
confirming no check is still running. This cache is disposable.

Local checks cover the current host, not both operating systems from the GitHub
matrix. In particular, macOS checks cannot exercise Linux epoll. The Actions
workflow calls `tools/check.sh` too, so it can be re-enabled later without
maintaining a second set of build commands. Enabling the local hook does not
change repository settings on GitHub or enable Actions.

Prerequisites are Git, a POSIX shell, CMake/CTest, a C11 compiler, SQLite headers
for the full/quick modes, a compiler supporting ThreadSanitizer, and Clang with
libFuzzer for full mode. Apple's default Clang may lack libFuzzer. The script
probes Clang and the usual Homebrew LLVM paths, or accepts an explicit compiler:

```sh
CAPI_FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang make check
./tools/check.sh sanitize                  # run one phase
CAPI_CHECK_JOBS=4 ./tools/check.sh quick    # default parallelism is 2
```

`CAPI_CHECK_CC` (or `CC`) selects the ordinary compiler, `CAPI_TSAN_CC` selects
the ThreadSanitizer compiler, and `CAPI_FUZZ_CC` selects the libFuzzer compiler.
Pass a single executable name/path, not shell flags. Missing tools fail clearly;
checks are never silently skipped. `CAPI_FUZZ_SECONDS` adjusts each fuzz budget
(default 30). Direct script runs use `build-check` unless
`CAPI_CHECK_BUILD_ROOT` selects another build directory. Run checks outside a
restrictive sandbox if it prohibits the tests' ephemeral loopback sockets.

## Tests and review

Add behavioral tests for routing, HTTP framing, lifecycle, and memory-safety
changes. Socket tests use only ephemeral loopback ports. Treat malformed traffic
as expected input and preserve strict, unambiguous framing.

Worker changes require both ASan/UBSan and a separate ThreadSanitizer build.
The worker suite covers queue saturation, responsive endpoints during blocked
work, completion ordering, cancellation with socket reuse, retained input,
queued-job skipping, initialization failures, concurrent producers, and shutdown.
ASan and ThreadSanitizer cannot be combined in one build.

The `reliability` tests cover structured errors, first-error preservation,
request reports, arena budgets, and input conversion boundaries. Compiler
contract tests verify valid usage and expected compile failures. Allocation
fault tests compile a separate private core with libc allocation names replaced
by test wrappers; no fault-injection API or allocator indirection ships in capi.

The `routers` suite covers nested snapshots, repeated prefixes, slash and
parameter semantics, middleware isolation, and rollback on conflicts. Allocation
fault tests fail each allocation during app/router inclusion and verify that
existing endpoints stay usable. Worker integration tests include their job
routes through a router and free the builder before serving, exercising async
handler-data and cancellation lifetimes with copied middleware bindings.

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
query/form values, included-router middleware, and JSON escaping. The template harness compiles arbitrary
sources and renders successful compilations with sections, lists, escaped values,
and recursive partials. Minimize crashes and add the input as a regression
test. Do not commit generated fuzz corpora or build products.

Report performance changes with compiler flags, machine details, request shape,
connection count, and a reproducible command. No unmeasured speed claims.
