#!/usr/bin/env bash
# Build the libFuzzer targets (clang, libFuzzer + ASan/UBSan) and run each.
# Usage: scripts/run-fuzz.sh [seconds-per-target] [results-dir]
#   TARGETS="ipc_packet_fuzz resp_parser_fuzz" (default: both)
# Corpora are kept in .cache/fuzz-corpus (IPC) and .cache/fuzz-corpus-resp;
# one log per target goes to the results dir.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
seconds=${1:-600}
out=${2:-$root/results/fuzz}
build=${BUILD_ROOT:-$root/build-fuzz}
liburing=${ROL_LIBURING_ROOT:-$root/.cache/liburing-2.14/install}
targets=${TARGETS:-ipc_packet_fuzz resp_parser_fuzz}
mkdir -p "$out" "$root/.cache"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DROL_SANITIZER=address \
  -DROL_FUZZ=ON -DROL_LIBURING_ROOT="$liburing" > /dev/null
# shellcheck disable=SC2086
nice cmake --build "$build" -j16 --target $targets > /dev/null

seed_ipc() {
  # A valid packet, and variants with two descriptors, a truncation flag, a
  # foreign cmsg type and a lying cmsg_len.
  python3 - "$1" <<'PY'
import struct, sys, pathlib
d = pathlib.Path(sys.argv[1])
hdr = struct.pack("<IHH2QQQQ", 0x524f4c31, 1, 48, 42, 84, 1, 3, 0)
def cmsg(level, typ, data, length=None):
    n = 16 + len(data) if length is None else length
    body = struct.pack("<Qii", n, level, typ) + data
    return body + b"\0" * ((-len(body)) % 8)
seeds = {
    "valid": bytes([0, 48]) + hdr + cmsg(1, 1, struct.pack("<i", 7)),
    "two_fds": bytes([0, 48]) + hdr + cmsg(1, 1, struct.pack("<ii", 7, 8)),
    "ctrunc": bytes([2, 48]) + hdr + cmsg(1, 1, struct.pack("<i", 7)),
    "creds": bytes([0, 48]) + hdr + cmsg(1, 2, struct.pack("<iii", 1, 2, 3)),
    "lying_len": bytes([0, 48]) + hdr + cmsg(1, 1, struct.pack("<i", 7), length=4096),
    "short": bytes([0, 47]) + hdr[:47] + cmsg(1, 1, struct.pack("<i", 7)),
}
for name, data in seeds.items():
    (d / f"seed-{name}").write_bytes(data)
PY
}

seed_resp() {
  python3 - "$1" <<'PY'
import sys, pathlib
d = pathlib.Path(sys.argv[1])
def resp(*args):
    return b"*%d\r\n" % len(args) + b"".join(b"$%d\r\n%s\r\n" % (len(a), a) for a in args)
seeds = {
    "pipeline": bytes([7]) + resp(b"SET", b"k", b"v") + resp(b"GET", b"k") + b"PING\r\n" + resp(b"DEL", b"k", b"x", b"y"),
    "inline": bytes([1]) + b"SET a 1\r\nGET a\r\n\r\nEXISTS a b\n",
    "empty": bytes([3]) + b"*0\r\n*-1\r\n" + resp(b"DBSIZE") + resp(b"FLUSHALL"),
    "errors": bytes([5]) + b"*2\r\n$3\r\nGET\r\n$01\r\nx\r\n",
    "big": bytes([63]) + resp(b"SET", b"key", b"x" * 400) + resp(b"QUIT") + resp(b"PING"),
}
for name, data in seeds.items():
    (d / f"seed-{name}").write_bytes(data)
PY
}

status=0
for target in $targets; do
  case $target in
    ipc_packet_fuzz) corpus=${CORPUS:-$root/.cache/fuzz-corpus}; mkdir -p "$corpus"; seed_ipc "$corpus" ;;
    resp_parser_fuzz) corpus=${CORPUS_RESP:-$root/.cache/fuzz-corpus-resp}; mkdir -p "$corpus"; seed_resp "$corpus" ;;
    *) echo "unknown target $target"; exit 2 ;;
  esac
  log=$out/$target.log
  {
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "commit: $(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "clang: $(clang++ --version | head -1)"
    echo "command: $target -max_total_time=$seconds -print_final_stats=1 <corpus>"
    echo
  } > "$log"
  set +e
  nice "$build/$target" -max_total_time="$seconds" -print_final_stats=1 \
    -artifact_prefix="$root/.cache/fuzz-crash-$target-" "$corpus" > "$build/$target-full.log" 2>&1
  rc=$?
  set -e
  # Keep the head, the periodic status lines (thinned) and the final stats.
  { grep -E '^(INFO|#[0-9]+.*(INITED|DONE|pulse|NEW))' "$build/$target-full.log" || true; } |
    awk 'NR<=12 || NR%200==0' >> "$log"
  grep -E '^(stat::|==[0-9]+==|SUMMARY|artifact|Done)' "$build/$target-full.log" >> "$log" || true
  echo "exit code: $rc" >> "$log"
  sed -i "s|$build|<build>|g; s|$root|.|g; s|$HOME|~|g" "$log"
  echo "$target: exit code $rc; log in $log"
  [[ $rc -ne 0 ]] && status=1
done
exit $status
