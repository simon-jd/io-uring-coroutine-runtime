# io_uring Coroutine Runtime

An io_uring-based C++20 coroutine runtime whose central contract is buffer
ownership while operations are in flight, and a RESP key-value server built on
it, measured against the same server on epoll and on Asio, and against Valkey.

Scope: the runtime (thread-per-core reactors, operation slots, registered and
provided buffers, direct descriptors, cancellation, shutdown, cross-core
messages over `IORING_OP_MSG_RING` or shared-memory queues with a doorbell), a
sealed-memfd IPC protocol, and the server with its benchmark harness, all on
one Linux machine over loopback.

| GET, loopback, medians of 7 interleaved runs: [session 1](results/kv-bench/imp-session-1/summary.md) / [session 2](results/kv-bench/imp-session-2/summary.md) | io_uring runtime | same, published `main` | epoll | Asio | Valkey 9.1.3 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 server core, closed loop, pipeline depth 1: requests/s | 348 k / 343 k | 349 k / 344 k | 329 k / 320 k | 327 k / 322 k | 296 k / 296 k |
| 1 core, depth 1: server CPU per request, µs | 2.87 / **2.88** | **2.86** / 2.92 | 3.02 / 3.15 | 3.01 / 3.07 | 3.39 / 3.34 |
| 1 core, depth 16: requests/s | 3.72 M / 3.75 M | 3.61 M / 3.35 M | 3.65 M / 3.57 M | 3.64 M / 3.55 M | 2.15 M / 2.13 M |
| 2 cores (shared-nothing keyspace), depth 16: requests/s | 5.11 M / 5.08 M | 5.34 M / 5.46 M | 4.81 M / 4.86 M | 3.74 M / 3.38 M | 3.12 M / 3.12 M |
| 2 cores, depth 16: server CPU per request, µs (**loss** here, not in check D) | 0.303 / 0.318 | 0.328 / 0.305 | **0.294 / 0.297** | 0.321 / 0.347 | 0.660 / 0.661 |
| 4 cores, depth 16: requests/s | 6.84 M / 9.00 M | 7.63 M / 9.03 M | 8.08 M / 8.81 M | 5.55 M / 7.05 M | 3.76 M / 3.96 M |
| 4 cores, depth 16: server CPU per request, µs (**loss**) | 0.390 / 0.349 | 0.394 / 0.364 | **0.350 / 0.324** | 0.412 / 0.362 | 1.04 / 0.981 |

What holds in sessions 1 and 2 and in two more from fresh clones (check D;
paired by repetition; [details](results/README.md)): against epoll, io_uring
needs 3–8% less CPU per GET at 1 core and depth 1 (5–7 of 7 repetitions) and
3–8% more for SET at 1 core, depth 16. **Losses** by median in all four
sessions, each with one session under check D's 5 of 7: more CPU at depth 16
for SET at 2 cores (7–11%; 4 of 7 in session 2) and for GET and SET at 4 cores
(5–12%; SET 4 of 7 in check D's session 3; `main` needed 11–18%), and lower
SET throughput at 1 and 2 cores, depth 16 (3–7% and 8–13%; 4 of 6 at 1 core in
check D's session 1, 4 of 7 at 2 cores in session 2). GET at 2 cores needed 3%
and 9% more CPU in sessions 1 and 2, not in check D. This round's changes
(call nodes in their operation slots, a slot table of fixed chunks, one hash
per key; [design trade-offs](docs/design-tradeoffs.md), Part 4) cut user-space
instructions per request by 6% at 2 cores and 11% at 4 (callgrind), and CPU
per request at 4 cores by 3–5% in three decision windows (a 4-core cell at 7
of 7 in each), 1–4% in sessions 1 and 2 (GET 4 of 7 in session 2) and 4–12% in
check D; at 1 and 2 cores no CPU difference against `main` met the rule in all
four sessions. Throughput at 2 cores was below `main`'s: GET by 2–13% in three
of the four sessions (1.02 times in the fourth), SET by 0.3–7% in all four (4
of 7 in check D's session 2; **loss**). What is left is user time (4 cores,
GET: 145–168 ns per request against epoll's 121–135; system time the same):
per read, an operation slot for every send and call, three coroutine
resumptions and a message node per call, the bookkeeping that makes the
ownership contract hold. Tried and dropped: an inline receive path that ran
fewer instructions but needed 4% more CPU. The test suites (156 cases) pass in
6 compiler × sanitizer configurations, 15 of 15 deliberate contract violations
fail their named assertions, and both fuzz targets run clean.

Limitations: one shared machine (other jobs' interference is recorded per
run; session 1's 4-core runs shared the server CPUs' SMT siblings with another
project's job), loopback only (client and server share the kernel's TCP
work), 1, 2 and 4 server cores (4 at depth 16 only: at depth 1 the load
generator is the limit), no rejection rule above 1 core, tail latencies
dominated by other jobs, no hardware counters (`perf` is blocked; callgrind
counts instructions and simulates caches, it does not measure the hardware),
zero-copy send measured only on loopback.

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
| `scripts/deliberate-bugs.py` | applies fifteen contract violations one at a time; each must fail its named assertion |
| `scripts/run-fuzz.sh [seconds]` | libFuzzer on the IPC packet validator and the RESP parser |
| `scripts/build-valkey.sh` | Valkey 9.1.3 and valkey-benchmark from source (benchmark reference) |
| `scripts/xcore-study.sh 4,6` | cross-core messaging study on two CPUs |
| `scripts/kv-bench.py --suite main` | the server comparison, with per-run clock and interference records (also `variants`, `diag`, `crosscheck`, `probe`, and `compare` for one build against another: `--arm NAME=IO@BUILD`) |
| `scripts/callgrind-profile.sh`, `scripts/callgrind-summary.py`, `scripts/callgrind-jobs.py` | user-space instructions per request under callgrind, by module and by the io_uring front end's jobs |
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
