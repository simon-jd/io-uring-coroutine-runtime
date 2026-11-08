# io_uring Coroutine Runtime

An io_uring-based C++20 coroutine runtime whose central contract is buffer
ownership while operations are in flight, and a RESP key-value server built on
it, measured against the same server on epoll and on Asio, and against Valkey.

Scope: the runtime (thread-per-core reactors, operation slots, registered and
provided buffers, direct descriptors, cancellation, shutdown, cross-core
messages over `IORING_OP_MSG_RING` or shared-memory queues with a doorbell), a
sealed-memfd IPC protocol, and the server with its benchmark harness, all on
one Linux machine over loopback.

| GET, loopback, medians of 7 interleaved runs: [session A](results/kv-bench/session-a/summary.md) / [session B](results/kv-bench/session-b/summary.md) | io_uring runtime | epoll | Asio | Valkey 9.1.3 |
| --- | ---: | ---: | ---: | ---: |
| 1 server core, closed loop, pipeline depth 1: requests/s | 336 k / 315 k | 316 k / 305 k | 316 k / 306 k | 292 k / 277 k |
| 1 core, depth 1: server CPU per request, µs | **2.94 / 3.13** | 3.15 / 3.29 | 3.16 / 3.26 | 3.39 / 3.57 |
| 1 core, depth 16: requests/s | 2.69 M / 2.55 M | 2.71 M / 2.30 M | 2.67 M / 2.52 M | 2.10 M / 1.96 M |
| 2 cores (shared-nothing keyspace), depth 1: requests/s | 604 k / 563 k | 581 k / 535 k | 534 k / 511 k | 279 k / 272 k |
| 2 cores, depth 16: requests/s (**loss**) | 4.69 M / 4.40 M | **5.14 M / 4.56 M** | 3.01 M / 2.83 M | 3.03 M / 2.90 M |
| 2 cores, depth 16: server CPU per request, µs (**loss**) | 0.417 / 0.449 | **0.383 / 0.418** | 0.424 / 0.464 | 0.683 / 0.712 |

What holds in both sessions: at 1 core and depth 1 the io_uring front end needs
6–7% less server CPU per request than epoll (paired by repetition, 0.927 and
0.939; lower in 7 of 7 repetitions in each session, for GET and for SET), and
because the server is saturated there it serves more requests (6% more in A, 3%
in B). Multishot receive needs no receive system call when each read carries
one request. At 2 cores and depth 16 it needs 3–8% more CPU per request than
epoll (paired ratios 1.03–1.08 for GET and SET; lower in at most 3 of 7
repetitions) and serves fewer requests, although the throughput ranges overlap. At 1 core and depth 16 there is no
consistent difference. Absolute numbers moved between the sessions (B ran at a
lower clock, 5.27 vs. 5.38 GHz median, with more interference); an earlier
single-session 11% throughput lead at depth 1 shrank to 3–6%. A busy SMT sibling
costs about 30% on its own, so 1-core runs with interference were rejected and
repeated under a rule fixed before either session. A doorbell transport for
cross-core messages, built to remove the 2-core loss, is correct and three
times faster for a round trip between spinning cores, but on the server it
delivered 4.78 vs. 5.24 M requests/s at depth 16 with the same CPU per request,
so it is an option, not the default. The test suites pass in 6 compiler ×
sanitizer configurations, 10 of 10 deliberate contract violations are caught by
named assertions, and both fuzz targets run clean ([results](results/README.md)).

Limitations: one shared machine (a training job and a VM ran on the server
CPUs' siblings in session B, per the per-run records), loopback only (client
and server share the kernel's TCP work), 1 and 2 server cores (at 4 the load
generator was the limit), no rejection rule at 2 cores, tail latencies
dominated by other jobs, no profiler (`perf` is blocked), zero-copy send
measured only on loopback.

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
| `scripts/deliberate-bugs.py` | applies ten contract violations one at a time; each must fail its named assertion |
| `scripts/run-fuzz.sh [seconds]` | libFuzzer on the IPC packet validator and the RESP parser |
| `scripts/build-valkey.sh` | Valkey 9.1.3 and valkey-benchmark from source (benchmark reference) |
| `scripts/xcore-study.sh 4,6` | cross-core messaging study on two CPUs |
| `scripts/kv-bench.py --suite main` | the server comparison, with per-run clock and interference records (also `variants`, `diag`, `crosscheck`, `probe`) |
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
