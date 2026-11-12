#!/bin/sh
# Run from anywhere. Server, client, fixtures, and report generator are C11.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
destination=${1:-docs/benchmarks/$(date +%Y-%m-%d)}
requests=${CAPI_PERF_REQUESTS:-100000}
case "$requests" in ''|*[!0-9]*) echo 'CAPI_PERF_REQUESTS must be an integer' >&2; exit 1;; esac
if [ "$requests" -lt 256 ] || [ "$requests" -gt 10000000 ]; then
    echo 'CAPI_PERF_REQUESTS must be between 256 and 10000000' >&2; exit 1
fi
mkdir -p "$destination"
server_pid=
cleanup() {
    if [ -n "$server_pid" ]; then kill "$server_pid" 2>/dev/null || :; wait "$server_pid" 2>/dev/null || :; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
for mode in native poll; do
    force=OFF
    if [ "$mode" = poll ]; then force=ON; fi
    cmake -S . -B "build-perf-$mode" -DCMAKE_BUILD_TYPE=Release -DCAPI_FORCE_POLL="$force" \
        -DCAPI_WITH_SQLITE=ON -DCAPI_WITH_TEMPLATES=ON -DCAPI_SANITIZE=OFF -DCAPI_THREAD_SANITIZE=OFF \
        -DCAPI_BUILD_TESTS=OFF -DCAPI_BUILD_EXAMPLES=OFF -DCAPI_BUILD_TOOLS=ON
    cmake --build "build-perf-$mode" --target capi_perf_server capi_bench capi_perf_report -j
done
{
    printf 'Measured (UTC): '
    date -u '+%Y-%m-%d %H:%M:%S'
    printf 'Core revision: '
    git rev-parse HEAD
    printf 'Runtime version: '
    sed -n 's/^#define CAPI_VERSION "\(.*\)"/\1/p' include/capi.h
    printf 'System: '
    uname -smrv
    if command -v sw_vers >/dev/null 2>&1; then sw_vers; fi
    if command -v sysctl >/dev/null 2>&1; then
        sysctl machdep.cpu.brand_string hw.memsize hw.ncpu 2>/dev/null || :
    fi
    if [ -r /proc/cpuinfo ]; then sed -n '/model name/{p;q;}' /proc/cpuinfo; fi
    printf '\nCompiler:\n'
    cc --version
    printf '\nBuild: CMake Release (-O3 -DNDEBUG), no sanitizers, native vs forced poll\n'
    printf 'Protocol: HTTP/1.1 keep-alive, TCP_NODELAY, one request in flight per connection\n'
    printf 'Matrix: 6 workloads x 5 connection levels x 3 repeats x 2 backends\n'
    printf 'Measured requests per run: %s; warmup per run: 5000\n' "$requests"
    printf 'Latency: end-to-end from first send attempt through complete response\n'
    printf 'Order: native then poll; rounds forward, reverse, forward\n'
    printf '\nBenchmark source SHA-256:\n'
    shasum -a 256 tools/bench.c tools/perf_server.c tools/measure.sh
} > "$destination/environment.txt"
csv="$destination/results.csv.tmp"
printf 'backend,workload,repeat,connections,requests,seconds,rps,p50_ms,p95_ms,p99_ms,wire_bytes\n' > "$csv"
for mode in native poll; do
    build="build-perf-$mode"
    : > "$build/server.log"
    "$build/capi_perf_server" > "$build/server.log" 2>&1 &
    server_pid=$!
    attempts=0
    while [ ! -s "$build/server.log" ]; do
        attempts=$((attempts + 1))
        if [ "$attempts" -gt 100 ] || ! kill -0 "$server_pid" 2>/dev/null; then
            cat "$build/server.log" >&2; echo 'Benchmark server failed to start' >&2; exit 1
        fi
        sleep 0.05
    done
    read -r port backend sqlite_version < "$build/server.log"
    printf 'Backend: %s; SQLite runtime: %s\n' "$backend" "$sqlite_version" >> "$destination/environment.txt"
    for repeat in 1 2 3; do
        workloads='text json route bulk template sqlite'
        levels='1 8 32 64 256'
        if [ "$repeat" = 2 ]; then workloads='sqlite template bulk route json text'; levels='256 64 32 8 1'; fi
        for workload in $workloads; do
            path="/$workload"
            if [ "$workload" = route ]; then path='/users/42?name=Ada%20Lovelace'; fi
            for connections in $levels; do
                "$build/capi_bench" 127.0.0.1 "$port" "$connections" 5000 "$path" --csv > /dev/null
                row=$("$build/capi_bench" 127.0.0.1 "$port" "$connections" "$requests" "$path" --csv)
                printf '%s,%s,%s,%s\n' "$backend" "$workload" "$repeat" "$row" >> "$csv"
                printf '%s %-8s round %s / %3s connections: %s\n' "$backend" "$workload" "$repeat" "$connections" "$row"
            done
        done
    done
    kill "$server_pid"
    wait "$server_pid"
    server_pid=
done
mv "$csv" "$destination/results.csv"
./build-perf-native/capi_perf_report "$destination/results.csv" "$destination/environment.txt" "$destination/index.html"
printf '\nReport: %s/index.html\n' "$destination"
