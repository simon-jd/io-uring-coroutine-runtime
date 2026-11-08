# Design trade-offs

Decisions, the alternatives rejected, and the measurement or reasoning behind
each. The first part covers the runtime core; the second the key-value server,
the baselines and the performance study. Numbers come from the files under
[results/](../results/README.md); each section names its source. Numbers from
different measurement sessions are never compared with each other.

# Part 1: the runtime core

## Operation state lives in a reactor-owned slot, not in the awaiter

*Chosen:* every in-flight operation is a slot in the reactor's table. The awaiter
in the coroutine frame holds `{index, generation}`; the slot holds the result
and every resource the kernel can touch.

*Rejected:* the common pattern of putting the result fields (and the buffer) in
the awaiter and storing the awaiter's address in `user_data`. When a frame is
destroyed with the operation in flight, the CQE handler writes into freed frame
memory and the kernel writes into a freed or reused buffer. The deliberate-bug
check `c-release-on-coroutine-destroy` shows the second half concretely: a new
lease holder's bytes are overwritten by the kernel.

*Cost:* one extra indirection per completion and a slot table that never
shrinks (slots are reused; the table is sized by peak concurrency).

## Buffers move into operations

*Chosen:* operations take `owned_buffer` / `fixed_lease` by value and return them
with the result (the "rent" model used by tokio-uring and monoio).

*Rejected:* `std::span` parameters. The runtime cannot keep a span's memory
alive, so the contract would depend on every caller's discipline. The moved
model makes "free or reuse while in flight" impossible through the API; what it
cannot stop (a raw pointer kept from before the move) is listed in
[CONTRACT.md](CONTRACT.md).

## Inline resumption for I/O, self-unlinking nodes for everything else

*Chosen:* the CQE handler resumes the awaiting coroutine immediately. Other
wake-ups (events, buffer-pool waiters, task start) use an intrusive ready list
whose nodes live in the waiting object and unlink themselves on destruction.

*Rejected:* a run queue of `coroutine_handle<>`. A frame destroyed while its
handle is queued is resumed after free; the test
`destroyed_waiter_returns_handed_lease` destroys exactly such a frame.

*Consequence:* resumed coroutines run while CQE dispatch is in progress. Every
handler therefore copies the CQE and advances the ring before dispatching, and
never touches the slot after `resume()`.

## One ASYNC_CANCEL per operation

*Chosen:* a cancel source submits one cancel per registered operation, each with
its own slot, and reports every acknowledgement.

*Rejected:* `IORING_ASYNC_CANCEL_ANY`/`ALL`. One SQE would do, but the result is a
count, not a per-operation answer, and it would also hit internal operations.
Per-operation acknowledgements are what let the tests observe the window
between the acknowledgement and the target's own CQE (it occurred in 100 of 100
rounds on this kernel).

## Detaching a destroyed coroutine also cancels its operation

*Chosen:* the awaiter's destructor submits a cancel after detaching.

*Rejected:* leaving the operation to complete on its own. A detached recv on an
idle socket would hold its buffer and slot until the peer sends or the reactor
shuts down. With `DEFER_TASKRUN` the cancel always beats data that arrived but
whose task work has not run, so detached operations end promptly.

## DEFER_TASKRUN + SINGLE_ISSUER by default

*Chosen:* `SINGLE_ISSUER | DEFER_TASKRUN | SUBMIT_ALL`, with `setup_flags`
configurable. Completions run only inside the reactor's own `io_uring_enter`,
which makes CQE ordering easier to reason about (the cancel-ack window above is
deterministic) and avoids task-work interrupts. Round 1 found this default
losing on cross-core message floods; round 2 explains the loss, removes it with
batching and confirms the default on the server's numbers (Part 2). The tests
run the lifetime cases on a plain ring as well, because there completions can
land at any return to user space, which is the harder case for ownership.

## Linked timeouts report -ETIME, after both CQEs

*Chosen:* an operation with `op_options::timeout` resumes after both its own CQE
and the link-timeout CQE, and reports `-ETIME` when the timeout fired.

*Rejected:* resuming at the operation's CQE. It would report `-ECANCELED` for a
timeout (indistinguishable from an explicit cancel) and leave the slot waiting
for the timeout CQE anyway. The cost is waiting for the second CQE, which the
kernel posts right after the first (not measured separately).

## SEND_ZC resumes at the first CQE, releases at the notification

*Chosen:* the caller learns the byte count from the first CQE; the buffer goes
back to the pool (or is freed) at the notification.

*Rejected:* resuming only at the notification. Over loopback TCP the
notification arrives after the peer has read and acknowledged the data, which
couples the sender's progress to the receiver's. `IORING_SEND_ZC_REPORT_USAGE`
reported that the kernel copied the data in the loopback test, so zero-copy
brings nothing over loopback; whether it pays off needs a non-loopback path.

## Cross-core messages: MSG_RING with node pointers

*Chosen:* `IORING_OP_MSG_RING` carrying a heap node pointer in the target CQE's
`user_data`, sent with `IOSQE_CQE_SKIP_SUCCESS` so only failures produce a sender
CQE. Since round 2 the pointer is the head of a chain of nodes (see Part 2); a
call's reply travels back in the target's chain for the calling core.

*Rejected:* an eventfd plus a locked queue per core. It needs a lock or an MPSC
queue and an extra syscall per wake-up; MSG_RING delivers into the target's
completion queue, which the target already polls.

*Tried and kept as an option (round 3):* lock-free shared-memory queues with a
MSG_RING only as a doorbell for a sleeping core (`reactor_config::doorbell`).
It is correct and three times faster for a round trip between spinning cores,
but slower on the server; see "The doorbell transport" in Part 2.

*TSan:* the kernel orders the sender's writes before the receiver's reads, but
TSan cannot see that, so each node carries an atomic publication word
(release on send, acquire on receipt).

## A refused reply is sent again, not aborted

*Chosen:* when the kernel refuses a MSG_RING, the chain comes back to the
sender. Requests in it fail (the call throws `Failure(closed)` with the errno;
a post is dropped and counted). Replies, and post nodes on their way home, are
sent again 1 ms later until the kernel accepts them; a refused doorbell is rung
again the same way.

*Rejected:* aborting on a refused reply (round 2), and failing the call. The
reply node belongs to the caller's slot, and only the caller's thread may
complete that slot; the target can reach the caller only through another
MSG_RING (or, with the doorbell transport, shared memory plus a MSG_RING to
wake it). The refusals a live ring can produce (`-ENOMEM` for the request the
kernel allocates, `-EOVERFLOW` for a full completion queue) are transient, so
sending again delivers the real result instead of an error. A refusal that
never clears leaves the call pending; that needs a broken kernel or a ring
that is gone, and the runtime closes no ring while a message is outstanding.

*Tested* with real refusals: a test hook (`reactor_config::message_fault`) points
chosen MSG_RINGs at an eventfd, which the kernel refuses with `-EBADFD`
(`refused_request_fails_the_call`, `refused_post_is_dropped_and_counted`,
`refused_reply_is_sent_again`, `refused_doorbell_is_rung_again`).

## Shutdown keeps memory the kernel may still use

*Chosen:* drain with a deadline; on timeout, park the remaining slots' memory in
a process-lifetime list (`stats().abandoned`) and close the ring.

*Rejected:* freeing everything at the deadline. A SEND_ZC whose peer never reads
can keep the kernel's page references for minutes; freeing the buffer then lets
the next allocation's bytes go out on the wire. The
`shutdown_timeout_abandons_instead_of_freeing` test exercises the parked path.

## Deliberate bugs are source mutations, not build flags

*Chosen:* `scripts/deliberate-bugs.py` copies the tree, applies one exact text
replacement (it must match once), builds, and requires the guarding test to fail
with its named assertion. Build failures and timeouts do not count.

*Rejected:* `#ifdef ROL_BUG_*` switches in the library: they put dead code paths
in the product and can drift from the real code.

## Kept the minimal test harness

*Chosen:* the existing ~60-line harness, extended with named assertions
(`REQUIRE("name", expr)`) and an eager coroutine type that tests can destroy at
any suspension point.

*Rejected:* adding a test framework dependency for round 1. The deliberate-bug
script needs stable assertion names in the output, which the harness provides
directly.

# Part 2: the key-value server and the performance study

## A RESP key-value server as the workload

*Chosen:* a small Valkey/Redis-compatible server: PING, ECHO, GET, SET (with
NX/XX), DEL, EXISTS, DBSIZE, FLUSHALL, INFO, QUIT, CONFIG GET and COMMAND (the
last two as stubs, for clients that probe), pipelining and inline commands.
Standard clients and valkey-benchmark talk to it, and Valkey is a reference an
infrastructure reader already knows.

*Rejected:* an echo server (no state, so no cross-core work) and a custom
binary protocol (no external reference to compare with).

*Not implemented:* expiry, other data types, persistence, replication,
authentication, TLS, transactions, pub/sub. Valkey has all of them; its
per-request cost includes that machinery.

## Shared-nothing keyspace; core 0 hands out connections round-robin

*Chosen:* key k belongs to core `fnv1a(k) mod cores`, and only that core's
thread touches its shard. A connection lives on one core. Per read it runs its
own core's commands in place and sends every other core one batch
(`kv::plan`). Core 0 accepts and hands each connection to the next core in turn
(a `post` carrying the descriptor).

*Rejected:* one map behind a lock (every request contends, and the runtime's
cross-core path would go unused); a `SO_REUSEPORT` listener per core (the
kernel spreads connections by a hash of the 4-tuple, which is uneven at 50–100
connections, so per-core load would depend on port numbers).

*Cost:* with c cores, (c−1)/c of single-key requests cross cores. All front
ends use the same rule, so the comparison is about how each moves batches
between cores, not about routing.

## One batch per target core per read, awaited concurrently

*Chosen:* a read's commands for core t travel as one `kv::batch` with their
argument bytes copied (the receive buffer is reused while the batch is away).
The connection awaits every target at once with `when_all` (one target: a plain
`call`). The batch object goes to the target and back and is reused, so the
steady state allocates only the call node.

*Rejected:* one call per command (sixteen messages per read at pipeline depth
16) and awaiting the targets one after another (one round trip per target).

## Cross-core messages are chained per target per loop iteration

*Chosen (runtime):* `post` and `call` append to an outbox per target core.
Just before each loop iteration submits, every non-empty outbox goes out as one
chain with one `MSG_RING`. Replies and returning post nodes travel the same way.

*Rejected:* one `MSG_RING` per message (round 1). The doorbell alternative is
discussed below.

*Measured* (`results/xcore`, round 3, two P-cores, medians of 7 interleaved
runs, ns per message or call):

| | DEFER_TASKRUN, one MSG_RING each | DEFER_TASKRUN, batched | COOP_TASKRUN, one each | COOP_TASKRUN, batched | doorbell (DEFER_TASKRUN) |
| --- | ---: | ---: | ---: | ---: | ---: |
| one-way posts, receiver sleeps when idle | 192.9 | 54.1 | 106.2 | 47.4 | 52.4 |
| 32 concurrent callers, receiver sleeps | 357.2 | 188.2 | 203.5 | 199.9 | 219.9 |
| 32 concurrent callers, both cores spin | 371.8 | 121.6 | 196.2 | 117.8 | 126.7 |
| one caller, round trip, both cores spin | 932.9 | 907.9 | 737.7 | 743.9 | **294.2** |
| one caller, round trip, receiver sleeps | 2,863 | 2,844 | 3,017 | 2,945 | 3,033 |

Batching pays wherever several messages leave in one iteration and changes
nothing for a lone round trip, which is dominated by waking the other thread
(about 2.8–3.0 µs sleeping vs. 0.7–0.9 µs spinning over MSG_RING).

## Why one-way posts to a DEFER_TASKRUN ring were 1.8x slower

Round 1 measured a post flood at 188 vs. 104 ns per message for a
DEFER_TASKRUN vs. a COOP_TASKRUN receiver (`results/perf-sanity`). The
difference is in the kernel's `MSG_RING` path. In Linux 7.0
(`io_uring/msg_ring.c`), a target ring whose completions only its own task may
post (`SINGLE_ISSUER | DEFER_TASKRUN`, the `task_complete` flag) cannot receive
the CQE from the sender: `io_msg_data_remote()` allocates a zeroed request
(`kmem_cache_alloc`) and queues it as task work on the target
(`io_req_task_work_add_remote`); the target posts the CQE the next time it runs
task work inside its own `io_uring_enter`, then frees the request with
`kfree_rcu`. For any other target the sender writes the CQE directly
(`io_post_aux_cqe`). The study shows the cost on the receiving thread (one
MSG_RING per post, receiver sleeping): 144.9 ns of CPU per message and 73.5
`io_uring_enter` calls per 1,000 messages with DEFER_TASKRUN, against 68.8 ns
and 15.6 with COOP_TASKRUN, where the receiver reaps directly posted CQEs
without entering the kernel.

Batching removes almost all of that kernel work (15.6 MSG_RINGs per 1,000
posts), and the two ring types end up within 7 ns of each other (54.1 vs. 47.4
ns). What remained was in user space: freeing each post node on the receiving
thread is a cross-thread free in glibc's allocator, and the batched flood with
nodes freed on the target took 165.2 ns per post (receiver: 145.6 ns CPU per
message, and 22 voluntary context switches per 1,000 messages even when
spinning, which points at lock waits in the allocator). Sending each node back
to its own core in the next chain, to be freed there, brings it to 54.1 ns.

## Default ring flags: DEFER_TASKRUN with batching

The runtime keeps `SINGLE_ISSUER | DEFER_TASKRUN | SUBMIT_ALL` and batching on.
The decision comes from the server, not the micro-benchmark, where the two ring
types tie once batched. Server, 2 cores, GET, closed loop
(`results/kv-bench/variants`, round 3, medians of 7 with [min–max]; MSG_RING
counts from the diag suite of the same session):

| | DEFER_TASKRUN (default) | COOP_TASKRUN | DEFER_TASKRUN, doorbell | COOP_TASKRUN, doorbell |
| --- | ---: | ---: | ---: | ---: |
| pipeline depth 1, requests/s | 599 k [535–632 k] | 617 k [607–632 k] | 598 k [525–639 k] | 634 k [626–647 k] |
| pipeline depth 16, requests/s | **5.24 M** [5.14–5.39 M] | 3.26 M [3.20–3.39 M] | 4.78 M [4.56–4.99 M] | 3.37 M [2.94–3.48 M] |
| depth 16, server CPU per request | 0.377 µs | 0.379 µs | 0.377 µs | 0.384 µs |
| depth 16, voluntary context switches per 1,000 requests | 0.21 | 1.31 | 0.51 | 2.05 |

Batching (diag suite): 5.5 MSG_RINGs per 1,000 requests at depth 16 against
124.7 unbatched. At depth 1 the configurations are within the run-to-run
spread. At depth 16 COOP_TASKRUN is clearly worst (3.26 M, ranges do not
overlap) with the same CPU per request: its cores context-switch six times as
often and sit idle. The doorbell transport does not change that (3.37 M), so
the stall is not in the MSG_RING path. Where the wake-ups are lost is not
established; that needs a profiler or scheduler tracing, neither of which is
available here (below).

## The doorbell transport: correct, kept as an option, not the default

The round-2 open question was whether moving message chains into shared memory
and using MSG_RING only as a wake-up would remove the cross-core loss. Built in
round 3 (`reactor_config::doorbell`, `kv_server --xcore doorbell`):

- *Design.* One queue per (sender, target) pair: a stack of chains with one
  producer (compare-and-swap) and one consumer (exchange, then reverse to send
  order). The target drains its queues after every reap. A sender rings (a
  MSG_RING without a message) only when the target has announced that it is
  about to sleep. The handshake is the one in [CONTRACT.md](CONTRACT.md): the
  target announces, then looks at its queues again; the sender publishes, then
  reads the announcement; all four accesses are sequentially consistent.
- *Checks.* Every runtime test case also runs over the doorbell transport,
  sleeping and busy-polling, in all six sanitizer configurations (TSan sees the
  queue's atomics directly); the server suite runs against it too. A stress
  case aims calls at the window in which a core finishes other work and is
  about to sleep, with nothing else to wake it, 4,000 times; a watchdog thread
  turns a stall of 1 s into a counted failure. Deliberate bug j (no second look
  at the queues) fails it within a few rounds.
- *Micro-benchmark* (`results/xcore`): a round trip between two spinning cores
  takes 294 ns against 908 ns over MSG_RING, because neither side enters the
  kernel. With a sleeping receiver every message needs a doorbell anyway, plus
  a second `io_uring_enter` (below): a lone round trip takes 3,033 vs. 2,844 ns
  (7% slower) and 32 callers 220 vs. 188 ns per call (17% slower); one-way
  posts are even (52.4 vs. 54.1 ns).
- *Server* (`results/kv-bench/variants`, 2 cores, 7 repetitions): at depth 16
  it delivers 4.78 M requests/s against 5.24 M for MSG_RING (ranges do not
  overlap) at the same CPU per request (0.377 µs); at depth 1 the two tie
  (598 k vs. 599 k). The doorbell cores are idle more often (0.51 vs. 0.21
  voluntary context switches per 1,000 requests). Why requests wait longer
  when the cores do the same work is not established.
- *Announce after submitting.* The first version announced the sleep before
  the `io_uring_enter` that submits the iteration's sends and then waits. The
  sends keep that call in the kernel for a while, every sender in that time saw
  an announced core and rang, and the doorbell waited behind the sends. Kept as
  `--xcore doorbell-early` to measure it: 28.4 doorbells per 32.5 queue pushes
  (per 1,000 requests, depth 1) and 3.48 M requests/s at depth 16, against 5.9
  per 50.2 and 4.78 M when the core submits first (one extra `io_uring_enter`
  per wait: 102 vs. 59 per 1,000 requests at depth 1).

*Decision rule, fixed before the variants run:* the doorbell becomes the
default only if its median depth-16 throughput is higher and its median depth-1
throughput is at most 2% lower. It lost on depth 16, so MSG_RING stays the
default and the doorbell is an option. The variants session had no epoll arm
in its throughput runs, so whether the doorbell would have closed the gap to
epoll is answered only indirectly: it did not lower CPU per request against
MSG_RING (0.377 µs both), and the gap is a CPU gap (next sections).

## Where the runtime wins: one request per read

At 1 core and pipeline depth 1 the io_uring front end needs less CPU per
request than the epoll and Asio front ends, in both main sessions
(`results/kv-bench/session-a`, `session-b`, medians of 7):

| GET, 1 core, depth 1 | io_uring | epoll | Asio |
| --- | ---: | ---: | ---: |
| requests/s, A / B | 336 k / 315 k | 316 k / 305 k | 316 k / 306 k |
| server CPU per request, A / B | 2.94 / 3.13 µs | 3.15 / 3.29 µs | 3.16 / 3.26 µs |
| thousand cycles per request, A / B | 15.9 / 16.7 | 17.1 / 17.5 | 17.1 / 17.5 |
| io_uring / epoll CPU, paired by repetition, A / B | 0.927 / 0.939 | | |

io_uring needed less CPU than epoll in every one of the 7 repetitions of each
session, for GET and for SET (0.941 / 0.944). The server is saturated at depth
1 (one core, 50 connections), so throughput follows CPU per request. Each
request is one receive and one reply. The multishot receive stays armed, so
receiving costs no system call, and replies are submitted in the same
`io_uring_enter` that waits for the next completions: the diag suite
(`results/kv-bench/variants`) counts 59 `io_uring_enter` calls per 1,000
requests at 2 cores and depth 1, against 60 `epoll_wait` calls plus, by
construction, one `read` and one `send` per request for epoll.

What moved: the size of the lead. Round 2 measured 336 k vs. 302 k (11%) in one
session; the round-3 sessions measured 6% and 3% more requests/s. At 2 cores
and depth 1, GET favours io_uring in 5 of 7 repetitions in both sessions (0.959,
0.940) and SET is even (0.980, 1.006). At depth 16 the system-call savings are
spread over sixteen requests and the front ends show no consistent difference
at 1 core (paired ratios 0.99–1.04, either sign).

## Where the runtime loses: pipelined requests across cores

At 2 cores and depth 16 the io_uring front end needs more CPU per request than
the epoll front end in both sessions, and serves fewer requests:

| GET, 2 cores, depth 16 | io_uring | epoll |
| --- | ---: | ---: |
| requests/s, A | 4.69 M [4.43–5.21 M] | 5.14 M [4.04–5.36 M] |
| requests/s, B | 4.40 M [3.68–4.48 M] | 4.56 M [3.65–5.13 M] |
| server CPU per request, A / B | 0.417 / 0.449 µs | 0.383 / 0.418 µs |
| io_uring / epoll CPU, paired by repetition, A / B | 1.038 / 1.077 | |

SET shows the same (1.029 / 1.028). io_uring needed less CPU than epoll in at
most 3 of the 7 repetitions in any of these four comparisons. The throughput
ranges overlap, so the throughput difference alone would not be conclusive.
At 1 core the two are even, so the extra cost is in the cross-core path.

Both front ends hand over about the same number of batches: per 1,000 requests
the runtime sends 5.5 MSG_RINGs; the epoll front end makes 4.8 hand-offs (a
mutex-protected vector append) and 3.8 eventfd wake-ups (diag suite). Each
MSG_RING to a DEFER_TASKRUN ring is a kernel allocation, a task-work item on
the target and an RCU-deferred free (see above), and the reply needs another.
Moving the chains into shared memory and ringing only sleeping cores (the
doorbell transport above) removed those MSG_RINGs but not the loss: same CPU
per request, lower throughput. Its size moved between the main sessions (3.8%
and 7.7% for GET); its sign did not. Where the remaining cost sits is not
established without a profiler (`perf_event_paranoid=4` on this host).

## Multishot receive into provided buffers, parsed in place, bounded

*Chosen:* one multishot receive per connection into a per-core provided-buffer
ring (2,048 × 4 KiB). The parser reads straight from the provided buffer unless
a partial command is waiting; this core's commands run before the buffer is
released and other cores' arguments are copied into their batches. The epoll
and Asio front ends got the same rule (parse the fresh read in place unless
something is buffered), so none of them pays an extra copy on the common path.

*Bound:* `max_queued = 8` results per connection, then the reactor cancels the
receive and the data waits in the socket (CONTRACT.md). Without it, a client
that keeps sending while not reading its replies can hold the whole ring and
stall every other connection on its core.

## Replies from a heap buffer, not a registered buffer

*Measured* (round 2, `results/kv-bench/round2`, variants suite, 2 cores, GET;
not repeated in round 3): replies copied into a registered
buffer and sent with `WRITE_FIXED` gave 581 k vs. 577 k requests/s at depth 1
and 4.84 vs. 5.08 M at depth 16, with 3.42 vs. 3.48 and 0.414 vs. 0.396 µs of
CPU per request: no gain beyond the spread. *Chosen:* a per-connection heap
buffer reused for every reply (`--send owned`), which needs no pool, no size
limit and no fallback path. `--send fixed` stays as an option.

## Direct descriptors: correct, but no measurable gain here

*Measured* (round 2, `results/kv-bench/round2`, variants suite; not repeated
in round 3): 576 k vs. 577 k requests/s at depth 1, 4.55 M
[4.29–5.10 M] vs. 5.08 M [4.57–5.21 M] at depth 16, CPU per request 3.49 vs.
3.48 and 0.405 vs. 0.396 µs. The multishot receive takes its file reference
once, so only sends look the descriptor up per operation, once per read.
*Chosen:* off by default (`--direct` turns it on); the runtime support stays
because it closes the descriptor-reuse gap from round 1, and its two rules
(generation check, pins until the last CQE) are covered by tests and deliberate
bugs e and f.

## Graceful drain

*Chosen:* on SIGTERM, core 0 tells every core to drain: stop accepting, cancel
every multishot receive, answer what each connection already received, close.
After `--drain-ms` (2 s) sends still blocked on peers that do not read are
cancelled. The tests check that a request in progress (DEBUG SLEEP) is
answered, that idle connections see EOF, and that a peer that never reads
delays the exit by the drain limit only.

*Rejected:* closing at once (requests already received would lose their
replies; deliberate bug h does that and is caught) and waiting without a
deadline (one peer that never reads would block shutdown forever).

## Baselines

- **epoll** (`server/epoll_server.cpp`): same engine and threading model;
  level-triggered epoll, `read`/`send`; cross-core batches through a per-core
  mutex-protected inbox, woken by an eventfd write when the inbox was empty,
  handed over once per loop iteration.
- **Asio** (`server/asio_server.cpp`): standalone Asio 1.36.0 (the non-Boost
  distribution of Boost.Asio), header-only, default epoll reactor rather than
  its optional io_uring backend, one `io_context` per core thread,
  `shared_ptr` connections, read → process → write → read; batches handed
  over with one `asio::post` per target from a flush handler. It falls behind
  at 2 cores with depth 16 (3.01 M and 2.83 M in sessions A and B); not
  investigated.
- **Valkey 9.1.3** built from source (bundled jemalloc, upstream optimisation
  flags), `--save "" --appendonly no --io-threads <cores> --server-cpulist
  <server CPUs>`. Its I/O threads read, parse and write; one main thread runs
  every command, so its second core helps only with I/O (279 k / 272 k
  requests/s at 2 cores and depth 1, against 292 k / 277 k at 1 core, sessions
  A / B).

## Load generation

*Chosen:* `bench/kvload` for both modes: a closed loop with valkey-benchmark's
pipeline model, and an open loop at a fixed rate in which every connection
sends on its own schedule and latency runs from the scheduled send time, so a
stalled server is charged for the whole stall (no coordinated omission).
In round 2 valkey-benchmark from the same Valkey build was run as a
cross-check at 1 core and gave the same order at depth 1 as kvload in that
session (`results/kv-bench/round2/crosscheck.jsonl`); it was not repeated in
round 3. Its throughput is computed from a millisecond timer, so its values are
coarse.

*Rejected:* valkey-benchmark alone: it is closed-loop only, so its latency
percentiles hide queueing.

*Result on tails:* median latencies agree across the front ends at every rate
(for example 15, 16, 15, 16 µs at 200 k requests/s on 1 core in session A). p99
and p99.9 vary from tens of microseconds to tens of milliseconds between
repetitions of the same arm, so they measure the other jobs on the machine more
than the servers; the summaries list them with their spread and no ranking is
drawn.

## CPU sets, core counts and noise

Server threads on P-cores 2 and 4 (one per physical core; their SMT siblings 3
and 5 get no benchmark work), load generator on E-cores 16–23, both under
`nice` and `taskset`, the harness itself on CPU 9, arms interleaved in a
rotating order. Four server cores are not compared: in round 2's 4-core
calibration (`results/kv-bench/round2/calibration-4core`) the load generator's
CPUs were 85% busy at depth 1 and the io_uring and epoll servers both measured
1.11 M requests/s, the generator's limit.

The machine is shared (a training job, container services, another project's
builds and VMs), and the round-2 headline (io_uring 336 k vs. epoll 302 k
requests/s at 1 core, depth 1) did not reproduce in an independent 3-repetition rerun from a
fresh clone (309 k vs. 305 k). Round 3 records, for every run, the server CPUs'
clock (`scaling_cur_freq`, sampled every 0.1 s), the time other work spent on
the server CPUs and on their SMT siblings (from `/proc/stat`, minus the
server's own CPU time), the server's context switches and run-queue wait, and
the load on the other P-cores. Then it measured what moves the depth-1 result
(`results/probe`, 1 core, GET, depth 1, 6 repetitions, one quiet window):

| condition | io_uring req/s | epoll req/s | io_uring k cycles/req | epoll k cycles/req |
| --- | ---: | ---: | ---: | ---: |
| nothing added (clock 5.37 GHz) | 334 k | 308 k | 16.17 | 17.67 |
| spinners on three other P-cores (clock 5.21–5.25 GHz) | 326 k | 302 k | 16.06 | 17.30 |
| a spinner on the server CPU's SMT sibling | 232 k | 220 k | 22.38 | 23.78 |

- A busy SMT sibling costs about 30% (both front ends). It happens on its own:
  in one repetition without a spinner, other jobs kept the sibling about half
  busy and both servers dropped to 260 k.
- Turbo headroom matters less than expected here: three more busy P-cores cut
  the clock by 3% and throughput by 2–3%, and cycles per request (CPU time
  times sampled clock) stayed put.
- Wake-ups do not enter at 1 core, depth 1: the server never sleeps (0.0
  voluntary context switches per 1,000 requests).

That rerun overlapped another project's QEMU runs and a training
job; it has no per-run data, so which of these hit it is not known. From this,
the protocol for the two main sessions was fixed before either ran: 7
repetitions; closed-loop runs on one server core whose interference exceeds 5%
of the window are kept in the raw file, marked rejected, and repeated (up to 3
times, every arm alike); every table reports CPU per request and cycles per
request; the uring/epoll CPU ratio is also reported paired by repetition. The
rule is not applied at 2 cores: there the server is partly idle, other jobs
fill the siblings, and in a first attempt of the variants suite most runs
exceeded the limit even after three repeats (stopped after one repetition;
kept as `results/kv-bench/variants/attempt-with-rejection.jsonl`, not used for
any comparison).

Under that rule session A rejected and repeated 18 runs and session B 33; one
cell of B (repetition 2, io_uring, GET depth 16) had no accepted run after
three repeats. Session B, run with a harness that also notes which other
processes used CPU time on the server CPUs and their siblings, names the main
ones: processes named `python` (the machine's training job, unpinned) and a
QEMU VM; `python` was the largest of them in 11 of the 33 rejected runs.

## Zero-copy send is not in the performance table## Zero-copy send is not in the performance table

Over loopback the kernel copies anyway (`IORING_SEND_ZC_REPORT_USAGE` reports
it), so a loopback SEND_ZC number would not be a zero-copy result. The server
uses plain sends.

## No profiler

`perf` is blocked (`perf_event_paranoid=4`) and there is no sudo. The study
uses what is available without privileges: `getrusage(RUSAGE_THREAD)` and
voluntary context switches per thread, `/proc/<pid>/task/*/schedstat` and
`status` for the server's CPU time, run-queue wait and context switches,
`/proc/stat` and `scaling_cur_freq` for the machine around it, the reactor's
own counters (`io_uring_enter` calls, SQEs, CQEs, MSG_RINGs, doorbells, queue
pushes) and the kernel source.
