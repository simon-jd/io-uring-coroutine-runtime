#!/usr/bin/env bash
# Cross-core messaging study (bench/xcore_bench.cpp).
# Usage: scripts/xcore-study.sh [cpu-a,cpu-b] [results-dir]
# Builds a Release tree, runs the benchmark under nice with the process limited
# to two CPUs on different physical cores, records load average before and
# after, and writes the raw JSON lines plus a median summary.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cpus=${1:-4,6}
a=${cpus%,*}
b=${cpus#*,}
out=${2:-$root/results/xcore}
build=${BUILD_ROOT:-$root/build-release}
liburing=${ROL_LIBURING_ROOT:-$root/.cache/liburing-2.14/install}
reps=${REPS:-5}
ops=${OPS:-400000}
mkdir -p "$out"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_C_COMPILER=gcc -DROL_LIBURING_ROOT="$liburing" > /dev/null
nice cmake --build "$build" -j16 --target xcore_bench > /dev/null
raw=$out/xcore.jsonl
meta=$out/xcore-meta.txt
{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "kernel: $(uname -r)"
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "pinned cpus: $cpus (SMT siblings: cpu$a=$(cat /sys/devices/system/cpu/cpu$a/topology/thread_siblings_list), cpu$b=$(cat /sys/devices/system/cpu/cpu$b/topology/thread_siblings_list))"
  echo "command: nice taskset -c $cpus xcore_bench --reps $reps --ops $ops --cpus $cpus"
  echo "loadavg before: $(cut -d' ' -f1-3 /proc/loadavg)"
} > "$meta"
nice taskset -c "$cpus" "$build/xcore_bench" --reps "$reps" --ops "$ops" --cpus "$cpus" > "$raw"
echo "loadavg after: $(cut -d' ' -f1-3 /proc/loadavg)" >> "$meta"
python3 - "$raw" > "$out/xcore-summary.txt" <<'PY'
import json, statistics, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
groups = {}
for r in rows:
    mode = "one MSG_RING each"
    if r.get("transport") == "doorbell":
        mode = "doorbell"
    elif r["batch"]:
        mode = "batched" if r.get("return_posts", True) else "batched, freed on target"
    key = (r["bench"], r["idle"], r["ring"], mode)
    groups.setdefault(key, []).append(r)
cols = [("ns_per_op", "ns/op"), ("target_cpu_ns_per_op", "target cpu ns/op"),
        ("target_enters_per_1k", "target enters/1k"), ("target_vcsw_per_1k", "target vcsw/1k"),
        ("sender_rings_per_1k", "sender MSG_RINGs/1k"),
        ("sender_doorbells_per_1k", "sender doorbells/1k")]
print("Medians over repetitions (min-max of ns/op in brackets).")
print(f"{'bench':<11} {'idle':<6} {'ring':<14} {'messages':<25}" + "".join(f" {c[1]:>20}" for c in cols))
for key in sorted(groups):
    rs = groups[key]
    ns = [r["ns_per_op"] for r in rs]
    cells = []
    for field, _ in cols:
        med = statistics.median(r.get(field, 0.0) for r in rs)
        cells.append(f"{med:>9.1f} [{min(ns):.0f}-{max(ns):.0f}]" if field == "ns_per_op" else f"{med:>20.1f}")
    print(f"{key[0]:<11} {key[1]:<6} {key[2]:<14} {key[3]:<25}" + "".join(f" {c:>20}" for c in cells) + f"  n={len(rs)}")
PY
cat "$out/xcore-summary.txt"
