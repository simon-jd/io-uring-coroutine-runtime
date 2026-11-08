#!/usr/bin/env bash
# Build and test every compiler x sanitizer combination (Debug builds).
# Usage: scripts/test-matrix.sh [results-dir]
# Writes <config>.ctest.log (verbose test output) and summary.txt to the
# results directory; build trees go to $BUILD_ROOT (default build-matrix/).
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/results/test-matrix}
build=${BUILD_ROOT:-$root/build-matrix}
liburing=${ROL_LIBURING_ROOT:-$root/.cache/liburing-2.14/install}
asio=${ROL_ASIO_ROOT:-$root/.cache/asio-1-36-0}
jobs=${JOBS:-16}
compilers=${COMPILERS:-gcc clang}
sanitizers=${SANITIZERS:-none address thread}
mkdir -p "$out" "$build"
summary=$out/summary.txt
{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "kernel: $(uname -r)"
  echo "gcc: $(g++ --version | head -1)"
  echo "clang: $(clang++ --version | head -1)"
} > "$summary"
status=0
for cc in $compilers; do
  case $cc in gcc) cxx=g++ c=gcc ;; clang) cxx=clang++ c=clang ;; *) echo "unknown compiler $cc"; exit 2 ;; esac
  for san in $sanitizers; do
    dir=$build/$cc-$san
    log=$out/$cc-$san.ctest.log
    if cmake -S "$root" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=$cxx \
         -DCMAKE_C_COMPILER=$c -DROL_SANITIZER=$san -DROL_LIBURING_ROOT="$liburing" \
         -DROL_ASIO_ROOT="$asio" \
         > "$dir.configure.log" 2>&1 &&
       nice cmake --build "$dir" -j"$jobs" > "$dir.build.log" 2>&1; then
      ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
        ctest --test-dir "$dir" -V > "$log" 2>&1
      rc=$?
    else
      rc=99
      echo "build failed; see $dir.build.log" > "$log"
      tail -40 "$dir.build.log" >> "$log"
    fi
    # Keep logs free of machine-specific paths.
    sed -i "s|$build|<build>|g; s|$root|.|g; s|$HOME|~|g" "$log"
    [[ $rc -ne 0 ]] && status=1
    echo "$cc $san rc=$rc $(grep -E 'tests passed|tests failed' "$log" | tail -1)" | tee -a "$summary"
  done
done
exit $status
