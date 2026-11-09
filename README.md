# io_uring Coroutine Runtime

An io_uring-based C++20 coroutine runtime whose central contract is buffer
ownership while operations are in flight, and a RESP key-value server built on
it, measured against the same server on epoll and on Asio, and against Valkey.

Scope: the runtime (thread-per-core reactors, operation slots, registered and
provided buffers, direct descriptors, cancellation, shutdown, cross-core
messages over `IORING_OP_MSG_RING` or shared-memory queues with a doorbell), a
sealed-memfd IPC protocol, and the server with its benchmark harness, all on
one Linux machine over loopback.

| GET, loopback, medians of 7 interleaved runs: [session A](results/kv-bench/perf-session-a/summary.md) / [session B](results/kv-bench/perf-session-b/summary.md) | io_uring runtime | same, before the performance round | epoll | Asio | Valkey 9.1.3 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 server core, closed loop, pipeline depth 1: requests/s | 346 k / 351 k | 338 k / 338 k | 327 k / 334 k | 325 k / 325 k | 296 k / 298 k |
| 1 core, depth 1: server CPU per request, µs | **2.88 / 2.86** | 2.91 / 2.96 | 3.02 / 2.99 | 3.03 / 3.05 | 3.41 / 3.34 |
| 1 core, depth 16: requests/s | **3.72 M / 3.70 M** | 2.68 M / 2.96 M | 3.56 M / 3.54 M | 3.55 M / 3.48 M | 2.14 M / 2.16 M |
| 2 cores (shared-nothing keyspace), depth 16: requests/s | 5.47 M / 5.22 M | 5.49 M / 5.37 M | 4.94 M / 5.07 M | 3.64 M / 3.77 M | 3.16 M / 3.15 M |
| 2 cores, depth 16: server CPU per request, µs (**loss**) | 0.303 / 0.315 | 0.364 / 0.367 | **0.295 / 0.290** | 0.328 / 0.318 | 0.650 / 0.654 |
| 4 cores, depth 16: requests/s | 8.92 M / 8.36 M | 8.88 M / 8.80 M | **9.11 M / 8.99 M** | 7.28 M / 7.18 M | 3.99 M / 3.94 M |
| 4 cores, depth 16: server CPU per request, µs (**loss**) | 0.363 / 0.368 | 0.436 / 0.436 | **0.318 / 0.320** | 0.350 / 0.352 | 0.976 / 0.988 |

What holds in both sessions (paired by repetition, [details](results/README.md)): a
performance round cut the server's CPU per request at depth 16 by 16–26%
against the published code on 1, 2 and 4 cores (6 or 7 of 7 repetitions in every
cell), mostly in code every front end shares (a one-pass path for RESP number
lines, an open-addressing key table, one hash per key) and, with more than two
cores, by awaiting all target cores from one awaiter. Against epoll, io_uring
needs 4–6% less CPU per request at 1 core (depth 1, and depth 16 for GET), and
at depth 16 4–8% more at 2 cores (at most 1 of 7 repetitions the other way)
and 11–13% more at 4 cores (none). The extra is user time: about 130
instructions per request more than epoll's loop at 2 cores and 280 at 4
(callgrind), in the runtime's operation slots, message nodes and coroutine
resumptions. Tried and dropped, each
measured against the same baseline: leaner slot bookkeeping, replies written
straight into the send buffer, a registered ring descriptor, a spin before
sleeping (a quarter to a third less throughput) and a cheaper load-generator
receive path ([design trade-offs](docs/design-tradeoffs.md), Part 3). The test
suites pass in 6 compiler × sanitizer configurations, 13 of 13 deliberate
contract violations are caught by named assertions, and both fuzz targets run
clean.

Limitations: one shared machine (other jobs' interference is recorded per
run), loopback only (client and server share the kernel's TCP work), 1, 2 and 4
server cores (4 at depth 16 only: at depth 1 the load generator is the limit),
no rejection rule above 1 core, tail latencies dominated by other jobs, no
hardware counters (`perf` is blocked; callgrind counts instructions, not
cache misses), zero-copy send measured only on loopback.

Details: [results](results/README.md), [ownership contract](docs/CONTRACT.md),
[design trade-offs](docs/design-tradeoffs.md), [IPC protocol](docs/IPC.md).

## Build and test

Linux 6.8 or newer (developed on 7.0), CMake 3.22+, Ninja, GCC 14+ or clang 18+,
Python 3 for the scripts. Dependencies are pinned and checked by SHA256:

```sh
scripts/build-liburing.sh            # liburing 2.14 into .cache/
scripts/fetch-asio.sh                # standalone Asio 1.36.0 headers (Asio front end only)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`-DROL_SANITIZER=address` builds with ASan + UBSan, `-DROL_SANITIZER=thread`
with TSan. Without Asio the build skips that front end and its tests.

| Script | What it does |
| --- | --- |
| `scripts/test-matrix.sh` | GCC and clang, each plain, ASan + UBSan and TSan; all suites |
| `scripts/deliberate-bugs.py` | applies thirteen contract violations one at a time; each must fail its named assertion |
| `scripts/run-fuzz.sh [seconds]` | libFuzzer on the IPC packet validator and the RESP parser |
| `scripts/build-valkey.sh` | Valkey 9.1.3 and valkey-benchmark from source (benchmark reference) |
| `scripts/xcore-study.sh 4,6` | cross-core messaging study on two CPUs |
| `scripts/kv-bench.py --suite main` | the server comparison, with per-run clock and interference records (also `variants`, `diag`, `crosscheck`, `probe`, and `compare` for one build against another: `--arm NAME=IO@BUILD`) |
| `scripts/callgrind-profile.sh`, `scripts/callgrind-summary.py` | user-space instructions per request by module, under callgrind |
| `scripts/kv-summary.py` | tables from the raw benchmark lines |

## Run the server

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
build-release/kv_server --io uring --cores 2 --cpus 2,4 --port 6379
valkey-cli -p 6379 set k v          # or any RESP client
```

`--io epoll|asio` selects a baseline front end. io_uring options: `--ring
defer|coop`, `--xcore ring|doorbell` (cross-core transport), `--no-batch`,
`--direct`, `--send owned|fixed`, `--busy-poll`.
SIGINT or SIGTERM drains: requests already received are answered, then every
connection is closed.

## Layout

```
include/rol/, src/   the runtime: reactor, buffers, runtime, IPC, when_all
kv/                  RESP parser, shards, routing plan (no I/O)
server/              kv_server: io_uring, epoll and Asio front ends
bench/               kvload (load generator), xcore_bench, sanity_bench
tests/, fuzz/        test suites, libFuzzer targets
scripts/             builds of pinned dependencies, test matrix, benchmarks
docs/                ownership contract, IPC protocol, design trade-offs
results/             raw results and the scripts' summaries
```

License: MIT.
