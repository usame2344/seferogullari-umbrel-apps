#!/bin/bash
# SPDX-License-Identifier: GPL-3.0
# scripts/run_sanitizers.sh
# Iterates each sanitizer flavor in a SCRATCH build directory, runs:
#   (a) the Catch2 unit tests
#   (b) (optional) the fuzz suite against a sanitized server instance
# Cleans up the scratch dir afterwards.
#
# Usage:
#   ./scripts/run_sanitizers.sh              # asan+ubsan, tsan
#   ./scripts/run_sanitizers.sh asan         # one flavor
#   ./scripts/run_sanitizers.sh --fuzz       # also run the live fuzz suite
#
# Your normal build/ directory is never touched (all work happens in .san/).
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(dirname "$SCRIPT_DIR")}"
SCRATCH="${SCRATCH:-$REPO/.san}"
JOBS="${JOBS:-$(nproc)}"
RUN_FUZZ=0
FLAVORS=()
for a in "$@"; do
    case "$a" in
        --fuzz) RUN_FUZZ=1 ;;
        asan|tsan|ubsan|asan-ubsan) FLAVORS+=("$a") ;;
        *) echo "unknown arg: $a"; exit 2 ;;
    esac
done
[ "${#FLAVORS[@]}" -eq 0 ] && FLAVORS=(asan-ubsan tsan)

cmake_flags_for() {
    case "$1" in
        asan)        echo "-DMKPOOL_ENABLE_ASAN=ON" ;;
        ubsan)       echo "-DMKPOOL_ENABLE_UBSAN=ON" ;;
        asan-ubsan)  echo "-DMKPOOL_ENABLE_ASAN=ON -DMKPOOL_ENABLE_UBSAN=ON" ;;
        tsan)        echo "-DMKPOOL_ENABLE_TSAN=ON" ;;
    esac
}

asan_options() {
    echo "abort_on_error=0:halt_on_error=0:print_stacktrace=1:detect_leaks=0:log_path=/tmp/san_report.${1}.log"
}

OVERALL=0
for FLAVOR in "${FLAVORS[@]}"; do
    echo "==================================================================="
    echo "=== sanitizer: $FLAVOR"
    echo "==================================================================="
    DIR="$SCRATCH/$FLAVOR"
    rm -rf "$DIR" "/tmp/san_report.${FLAVOR}.log".*
    mkdir -p "$DIR"
    cmake_args="$(cmake_flags_for "$FLAVOR")"
    # shellcheck disable=SC2086
    cmake -S "$REPO" -B "$DIR" -DCMAKE_BUILD_TYPE=Debug -DMKPOOL_ENABLE_LTO=OFF -DMKPOOL_NATIVE=OFF $cmake_args 2>&1 | tail -3
    cmake --build "$DIR" -j"$JOBS" --target mkpool_tests mkpool 2>&1 | tail -5
    if [ ! -x "$DIR/tests/mkpool_tests" ]; then
        echo "FAIL [$FLAVOR]: build failed"
        OVERALL=1; continue
    fi
    echo "--- unit tests under $FLAVOR ---"
    ASAN_OPTIONS="$(asan_options "$FLAVOR")" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0" \
    TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1" \
        "$DIR/tests/mkpool_tests" 2>&1 | tail -8
    rc=${PIPESTATUS[0]}
    if [ "$rc" -ne 0 ]; then
        echo "FAIL [$FLAVOR] unit tests rc=$rc"
        OVERALL=1
    else
        echo "OK   [$FLAVOR] unit tests"
    fi

    # Optional live fuzz against the freshly built sanitized binary.
    # (Skip for tsan: TSan + Boost.Asio is noisy and slow.) This runs the
    # sanitized binary on its own copied config in the scratch dir and fuzzes
    # that instance directly. No system service is started or stopped.
    if [ "$RUN_FUZZ" -eq 1 ] && [ "$FLAVOR" != "tsan" ]; then
        echo "--- live fuzz under $FLAVOR ---"
        CFG="$REPO/config.json"
        [ -f "$CFG" ] || CFG="$REPO/config.json.example"
        cp "$CFG" "$DIR/config.json"
        pkill -9 -f "$DIR/mkpool" 2>/dev/null || true
        sleep 1
        ASAN_OPTIONS="$(asan_options "$FLAVOR")" \
        UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0" \
            nohup setsid "$DIR/mkpool" -c "$DIR/config.json" \
            > "/tmp/san_run.${FLAVOR}.log" 2>&1 < /dev/null &
        disown
        sleep 6
        PROC_PATTERN="$DIR/mkpool" UNIT=none "$SCRIPT_DIR/fuzz_suite.sh" || OVERALL=1
        pkill -9 -f "$DIR/mkpool" 2>/dev/null || true
        sleep 1
    fi
    # Surface any sanitizer report files
    if compgen -G "/tmp/san_report.${FLAVOR}.log.*" > /dev/null; then
        echo "SANITIZER REPORTS for $FLAVOR:"
        ls -la /tmp/san_report.${FLAVOR}.log.*
        for f in /tmp/san_report.${FLAVOR}.log.*; do
            echo --- "$f" ---
            head -120 "$f"
        done
        OVERALL=1
    fi
done

echo
if [ "$OVERALL" -eq 0 ]; then
    echo "ALL SANITIZER PASSES CLEAN"
else
    echo "SANITIZER ISSUES FOUND (see above)"
fi
exit $OVERALL
