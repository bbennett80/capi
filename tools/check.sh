#!/bin/sh
# Shared local/CI checks. This script never installs packages or downloads tools.
set -eu
capi_source=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
capi_mode=${1:-full}
if [ "$#" -gt 1 ]; then capi_mode=invalid; fi
case "$capi_mode" in
    quick) capi_phases='release core' ;;
    full) capi_phases='release sanitize poll core thread-sanitize fuzz' ;;
    release|sanitize|poll|core|thread-sanitize|fuzz) capi_phases=$capi_mode ;;
    --help|help)
        printf 'usage: tools/check.sh [full|quick|release|sanitize|poll|core|thread-sanitize|fuzz]\n'
        printf 'full (default): all native-host CI checks; quick: Release + core-only.\n'
        printf 'Options: CAPI_CHECK_JOBS, CAPI_CHECK_BUILD_ROOT, CAPI_CHECK_CC, CAPI_TSAN_CC,\n'
        printf '         CAPI_FUZZ_CC, CAPI_FUZZ_SECONDS (30 per harness by default).\n'
        exit 0 ;;
    *) printf 'capi check: unknown mode: %s (use --help)\n' "$capi_mode" >&2; exit 2 ;;
esac
capi_jobs=${CAPI_CHECK_JOBS:-2}
capi_seconds=${CAPI_FUZZ_SECONDS:-30}
for capi_number in "$capi_jobs" "$capi_seconds"; do
    case "$capi_number" in ''|*[!0-9]*) echo 'capi check: jobs and fuzz seconds must be positive integers' >&2; exit 2;; esac
    if [ "$capi_number" -lt 1 ] || [ "$capi_number" -gt 3600 ]; then
        echo 'capi check: jobs and fuzz seconds must be between 1 and 3600' >&2; exit 2
    fi
done
for capi_tool in cmake ctest git; do
    if ! command -v "$capi_tool" >/dev/null 2>&1; then printf 'capi check: missing %s\n' "$capi_tool" >&2; exit 1; fi
done
capi_build_root=${CAPI_CHECK_BUILD_ROOT:-"$capi_source/build-check"}
mkdir -p "$capi_build_root"
capi_build_root=$(CDPATH= cd -- "$capi_build_root" && pwd)
capi_cc=${CAPI_CHECK_CC:-${CC:-cc}}
capi_tsan_cc=${CAPI_TSAN_CC:-clang}
capi_fuzz_cc=

find_fuzzer() {
    # Probe the linker as well as the compiler: Apple Clang may lack libFuzzer.
    # Each candidate is a single executable name/path, never a shell command.
    if [ -n "${CAPI_FUZZ_CC:-}" ]; then
        set -- "$CAPI_FUZZ_CC"
    else
        set -- clang /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang
    fi
    for capi_candidate do
        if ! command -v "$capi_candidate" >/dev/null 2>&1; then continue; fi
        if printf '#include <stddef.h>\n#include <stdint.h>\nint LLVMFuzzerTestOneInput(const uint8_t *p, size_t n) { (void)p; (void)n; return 0; }\n' |
            "$capi_candidate" -x c - -fsanitize=fuzzer,address,undefined -o "$capi_build_root/fuzzer-probe" > "$capi_build_root/fuzzer-probe.log" 2>&1; then
            capi_fuzz_cc=$capi_candidate
            return
        fi
    done
    printf 'capi check: full/fuzz checks require Clang with libFuzzer.\nSet CAPI_FUZZ_CC to its executable path (LLVM Clang on macOS).\nNo tools were installed. See %s/fuzzer-probe.log\n' "$capi_build_root" >&2
    exit 1
}
case " $capi_phases " in *' fuzz '*) find_fuzzer;; esac

installed_consumers() {
    # A fresh prefix detects missing installed files rather than reusing old ones.
    # All four directories belong to this script's CMake build directory.
    rm -rf "$capi_build/install" "$capi_build/starter" "$capi_build/consumer-core" "$capi_build/consumer-extras"
    cmake --install "$capi_build" --prefix "$capi_build/install"
    "$capi_build/capi" new "$capi_build/starter"
    cmake -S "$capi_build/starter" -B "$capi_build/starter/build" \
        "-DCMAKE_PREFIX_PATH=$capi_build/install" "-DCMAKE_C_COMPILER=$capi_compiler" \
        "-DCMAKE_BUILD_TYPE=$capi_type" "-DCMAKE_C_FLAGS=$capi_consumer_flags"
    cmake --build "$capi_build/starter/build" --parallel "$capi_jobs"
    cmake -S "$capi_source/tests/consumer" -B "$capi_build/consumer-core" \
        "-DCMAKE_PREFIX_PATH=$capi_build/install" "-DCMAKE_C_COMPILER=$capi_compiler" \
        "-DCMAKE_BUILD_TYPE=$capi_type" "-DCMAKE_C_FLAGS=$capi_consumer_flags" \
        -DCAPI_CONSUME_EXTRAS=OFF -DCAPI_CONSUME_WORKERS=OFF \
        -DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=ON -DCMAKE_DISABLE_FIND_PACKAGE_Threads=ON
    cmake --build "$capi_build/consumer-core" --parallel "$capi_jobs"
    "$capi_build/consumer-core/consumer"
    cmake -S "$capi_source/tests/consumer" -B "$capi_build/consumer-extras" \
        "-DCMAKE_PREFIX_PATH=$capi_build/install" "-DCMAKE_C_COMPILER=$capi_compiler" \
        "-DCMAKE_BUILD_TYPE=$capi_type" "-DCMAKE_C_FLAGS=$capi_consumer_flags" \
        -DCAPI_CONSUME_EXTRAS=ON -DCAPI_CONSUME_WORKERS=ON
    cmake --build "$capi_build/consumer-extras" --parallel "$capi_jobs"
    "$capi_build/consumer-extras/consumer"
}

printf 'capi check: %s mode on %s; source: %s\n' "$capi_mode" "$(uname -s)" "$capi_source"
for capi_phase in $capi_phases; do
    capi_build="$capi_build_root/$capi_phase"
    capi_type=Release
    capi_compiler=$capi_cc
    capi_consumer_flags=-Werror
    set -- -DCAPI_SANITIZE=OFF -DCAPI_THREAD_SANITIZE=OFF -DCAPI_FORCE_POLL=OFF \
        -DCAPI_BUILD_FUZZER=OFF -DCAPI_BUILD_TESTS=ON -DCAPI_BUILD_EXAMPLES=ON \
        -DCAPI_BUILD_TOOLS=ON -DCAPI_WITH_SQLITE=ON -DCAPI_WITH_TEMPLATES=ON \
        -DCAPI_WITH_WORKERS=ON -DBUILD_SHARED_LIBS=OFF
    case "$capi_phase" in
        sanitize)
            capi_type=Debug
            capi_consumer_flags='-Werror -fsanitize=address,undefined -fno-omit-frame-pointer'
            set -- "$@" -DCAPI_SANITIZE=ON ;;
        poll) set -- "$@" -DCAPI_FORCE_POLL=ON ;;
        core)
            set -- "$@" -DCAPI_WITH_SQLITE=OFF -DCAPI_WITH_TEMPLATES=OFF -DCAPI_WITH_WORKERS=OFF \
                -DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=ON -DCMAKE_DISABLE_FIND_PACKAGE_Threads=ON ;;
        thread-sanitize)
            capi_type=Debug; capi_compiler=$capi_tsan_cc
            set -- "$@" -DCAPI_THREAD_SANITIZE=ON -DCAPI_WITH_SQLITE=OFF ;;
        fuzz)
            capi_type=Debug; capi_compiler=$capi_fuzz_cc
            set -- "$@" -DCAPI_BUILD_FUZZER=ON -DCAPI_BUILD_TESTS=OFF -DCAPI_BUILD_EXAMPLES=OFF \
                -DCAPI_BUILD_TOOLS=OFF -DCAPI_WITH_SQLITE=OFF -DCAPI_WITH_WORKERS=OFF ;;
    esac
    printf '\ncapi check: starting %s\n' "$capi_phase"
    cmake -S "$capi_source" -B "$capi_build" "-DCMAKE_C_COMPILER=$capi_compiler" \
        "-DCMAKE_BUILD_TYPE=$capi_type" -DCMAKE_C_FLAGS=-Werror "$@"
    cmake --build "$capi_build" --parallel "$capi_jobs"
    if [ "$capi_phase" = fuzz ]; then
        mkdir -p "$capi_build/corpus" "$capi_build/template-corpus" "$capi_build/artifacts"
        printf 'GET / HTTP/1.1\r\nHost: test\r\n\r\n' > "$capi_build/corpus/get"
        printf 'POST /id/path?name=Ada%%20Lovelace HTTP/1.1\r\nHost: test\r\nContent-Length: 3\r\n\r\nabc' > "$capi_build/corpus/post"
        printf '{{#items}}<p>{{name}}</p>{{/items}}{{>card}}' > "$capi_build/template-corpus/view"
        "$capi_build/capi_fuzz" "-max_total_time=$capi_seconds" -max_len=65536 -timeout=5 \
            "-artifact_prefix=$capi_build/artifacts/" "$capi_build/corpus"
        "$capi_build/capi_template_fuzz" "-max_total_time=$capi_seconds" -max_len=65536 -timeout=5 \
            "-artifact_prefix=$capi_build/artifacts/" "$capi_build/template-corpus"
    else
        ctest --test-dir "$capi_build" --output-on-failure --timeout 120
        case "$capi_phase" in release|sanitize|poll) installed_consumers;; esac
    fi
    printf 'capi check: passed %s\n' "$capi_phase"
done
printf '\ncapi check: all %s checks passed on this host.\n' "$capi_mode"
