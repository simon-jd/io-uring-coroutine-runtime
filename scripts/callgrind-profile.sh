#!/usr/bin/env bash
# User-space instructions per request of one kv_server front end, from
# callgrind (valgrind serializes the server's threads, so this counts work,
# not time; the kernel's share is not seen).
#
#   scripts/callgrind-profile.sh <build-dir> <io> <cores> <pipeline> <seconds> <out-dir> [server args...]
#
# Starts kv_server under callgrind on CPUs 10-15, preloads the keys, zeroes the
# counters, runs a closed-loop GET load with kvload on CPUs 24-31, dumps the
# counters (out-dir/callgrind.out.1) and stops the server. out-dir/kvload.json
# has the request count. scripts/callgrind-summary.py turns the dump into
# instructions per request by module. Build with -g for function names:
#   cmake -S . -B build-prof -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -g"
set -eu
B=$(realpath "$1"); IO=$2; CORES=$3; PIPE=$4; SECS=$5; OUT=$6; shift 6
mkdir -p "$OUT"; OUT=$(realpath "$OUT"); rm -f "$OUT"/callgrind.out*
PORT=$((18000 + RANDOM % 1000))
CPUS=$(echo 10,12,14,11 | cut -d, -f1-"$CORES")
cd "$OUT"
taskset -c 10-15 valgrind --tool=callgrind --callgrind-out-file="$OUT/callgrind.out" \
    "$B/kv_server" --io "$IO" --cores "$CORES" --cpus "$CPUS" --port "$PORT" "$@" 2> "$OUT/server.stderr" &
VPID=$!
for _ in $(seq 1 200); do
    python3 -c "import socket;s=socket.create_connection(('127.0.0.1',$PORT),1);s.sendall(b'PING\r\n');assert s.recv(16).startswith(b'+PONG')" 2>/dev/null && break
    sleep 0.2
done
taskset -c 24-31 "$B/kvload" preload --port "$PORT" --keys 100000 --value-size 64 > /dev/null
callgrind_control -z "$VPID" > /dev/null
taskset -c 24-31 "$B/kvload" closed --port "$PORT" --pipeline "$PIPE" --duration "$SECS" --warmup 0.5 \
    --conns $((50 * CORES)) --threads 8 --cpus 24-31 --keys 100000 --value-size 64 --get-ratio 1 > "$OUT/kvload.json"
callgrind_control -d "$VPID" > /dev/null
kill -TERM "$VPID"; wait "$VPID" || true
echo "kv_server --io $IO --cores $CORES $* from $B, pipeline $PIPE, $SECS s" > "$OUT/run.txt"
cat "$OUT/kvload.json"
