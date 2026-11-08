#!/usr/bin/env bash
# Quick performance sanity run (not the performance study).
# Usage: scripts/perf-sanity.sh [cpu-a,cpu-b] [results-dir]
# Builds a Release tree, runs bench/sanity_bench under nice with the process
# limited to two CPUs (pick two different physical cores) and the two runtime
# threads pinned to them, records load average before and after, and writes
# the raw JSON lines plus a median summary.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cpus=${1:-4,6}
a=${cpus%,*}
b=${cpus#*,}
out=${2:-$root/results/perf-sanity}
build=${BUILD_ROOT:-$root/build-release}
liburing=${ROL_LIBURING_ROOT:-$root/.cache/liburing-2.14/install}
reps=${REPS:-5}
ops=${OPS:-200000}
mkdir -p "$out"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_C_COMPILER=gcc -DROL_LIBURING_ROOT="$liburing" > /dev/null
nice cmake --build "$build" -j16 --target sanity_bench > /dev/null
raw=$out/sanity.jsonl
meta=$out/sanity-meta.txt
{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "kernel: $(uname -r)"
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "pinned cpus: $cpus (SMT siblings: cpu$a=$(cat /sys/devices/system/cpu/cpu$a/topology/thread_siblings_list), cpu$b=$(cat /sys/devices/system/cpu/cpu$b/topology/thread_siblings_list))"
  echo "command: nice taskset -c $cpus sanity_bench --reps $reps --ops $ops --cpus $cpus"
  echo "loadavg before: $(cut -d' ' -f1-3 /proc/loadavg)"
} > "$meta"
nice taskset -c "$cpus" "$build/sanity_bench" --reps "$reps" --ops "$ops" --cpus "$cpus" > "$raw"
echo "loadavg after: $(cut -d' ' -f1-3 /proc/loadavg)" >> "$meta"
python3 - "$raw" > "$out/sanity-summary.txt" <<'PY'
import json, statistics, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
groups = {}
for r in rows:
    groups.setdefault((r["bench"], r["variant"]), []).append(r["ns_per_op"])
print(f"{'bench':<12} {'variant':<15} {'median ns/op':>13} {'min':>9} {'max':>9}  reps")
for (b, v), xs in sorted(groups.items()):
    print(f"{b:<12} {v:<15} {statistics.median(xs):>13.1f} {min(xs):>9.1f} {max(xs):>9.1f}  {len(xs)}")
PY
cat "$out/sanity-summary.txt"
