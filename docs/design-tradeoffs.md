# Design trade-offs

Decisions, the alternatives rejected, and the measurement or reasoning behind
each. The first part covers the runtime core; the second the key-value server,
the baselines and the round-3 performance study; the third the performance
round that followed (what was optimized, what was tried and dropped, and where
the remaining loss sits); the fourth the improvement round that went after
that loss (what was kept, what was rejected, and what is left). Numbers come
from the files under [results/](../results/README.md); each section names its
source. Numbers from different measurement sessions are never compared with
each other.

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
shrinks (slots are reused; the table is sized by peak concurrency). The table
is a list of fixed chunks of 64 slots, so a slot never moves and a lookup is a
shift, a mask and the generation check (Part 4).

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

*Chosen:* `IORING_OP_MSG_RING` carrying a node pointer in the target CQE's
`user_data` (a call's node lives in the call's slot, a post's on the heap), sent with `IOSQE_CQE_SKIP_SUCCESS` so only failures produce a sender
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

*Chosen:* key k belongs to the core picked by the upper half of its 64-bit
hash (`kv::key_hash`; FNV-1a before the performance round, Part 3), and only
that core's
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
The connection awaits every target at once with `reactor::call_all` (one
target: a plain `call`; `when_all` over a coroutine per target before the
performance round, Part 3). The batch object goes to the target and back and
is reused, so the steady state allocates the call nodes and, with several
targets, one vector for the results.

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
and 7.7% for GET); its sign did not. The performance round (Part 3, "Where
the CPU goes") located it: the extra time is user time, about 120 more
instructions per request in the runtime and front end than in the epoll loop.

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

## Zero-copy send is not in the performance table

Over loopback the kernel copies anyway (`IORING_SEND_ZC_REPORT_USAGE` reports
it), so a loopback SEND_ZC number would not be a zero-copy result. The server
uses plain sends.

## Profiling without perf

`perf` is blocked (`perf_event_paranoid=4`) and there is no sudo. Round 3
used what is available without privileges: `getrusage(RUSAGE_THREAD)` and
voluntary context switches per thread, `/proc/<pid>/task/*/schedstat` and
`status` for the server's CPU time, run-queue wait and context switches,
`/proc/stat` and `scaling_cur_freq` for the machine around it, the reactor's
own counters (`io_uring_enter` calls, SQEs, CQEs, MSG_RINGs, doorbells, queue
pushes) and the kernel source. The performance round added callgrind
(valgrind 3.26) for user-space instruction counts per function
(`scripts/callgrind-profile.sh`, Part 3) and the per-thread user/system split
in every benchmark summary. Neither sees cache misses on real hardware or the
kernel's instructions.

# Part 3: the performance round

The question: how much cheaper can each module on the request path get, and
where does the 2-core loss against epoll come from? Every candidate was built
from its own source tree and measured against the published `main` (3eb4121)
or against the branch before it, in one quiet window, interleaved in rotating
order, under a decision rule committed before the run
(`results/perf/w*/RULES.md`). Kept changes are separate commits; rejected ones
are not in the code, and their diffs are in
[`results/perf/candidates/`](../results/perf/candidates) so they can be
measured again. The comparison with the other front ends at the end of the
round is in [results/](../results/README.md) (sessions `perf-session-a` and
`perf-session-b`).

## Where the CPU goes

*User or kernel.* The harness records every server thread's user and system
ticks. In round 3's sessions (GET, 2 cores, depth 16) the io_uring front end
spent 218 and 244 ns of user time per request against epoll's 195 and 211,
and 196 and 211 ns of system time against 199 and 206
(`results/kv-bench/session-a`, `session-b`, table "split into user and system
time"). The extra cost is user space; the kernel's share (MSG_RING, task work,
the receive and send paths) is the same. Window 3 shows the same at 4 cores,
where the gap is larger: 194 vs. 123 ns user, 200 vs. 194 ns system.

*Which user code.* Callgrind counts user-space instructions per request
(`scripts/callgrind-profile.sh`, `results/profile`). Valgrind runs the
server's threads one at a time, so these are counts of work, not time, and the
kernel is not seen. GET, 2 cores, depth 16, instructions per request:

| instructions per request | `main`, io_uring | `main`, epoll | engine change, io_uring | engine change, epoll |
| --- | ---: | ---: | ---: | ---: |
| RESP parser | 488 | 491 | 248 | 251 |
| reply encoders | 178 | 178 | 149 | 149 |
| engine: plan, batch, shard, hashing | 624 | 626 | 507 | 509 |
| byte copies | 75 | 67 | 72 | 64 |
| allocator | 11 | 1 | 11 | 0 |
| runtime: reactor, slots, cross-core | 123 | — | 123 | — |
| front end | 29 | 30 | 29 | 30 |
| other | 23 | 28 | 14 | 19 |
| **total** | **1,551** | **1,421** | **1,153** | **1,022** |

The parser and the engine, shared by every front end, were most of the
instructions, so they came first. The io_uring front end's own code (runtime,
front end and its allocations, about 160 instructions per request) costs about
130 more than the epoll loop's (about 30), before and after the engine change: per read an operation
slot each for the receive item, the send and the cross-core call, and three
coroutine resumptions (receive, reply, send completion) where epoll calls
`read` and `send` directly. That difference is 8–11% of the instructions,
the size of the measured loss (3–10% more CPU per request at 2 cores).

## Kept: one-pass number lines, an open-addressing shard, one hash function

- *Parser.* A count or length line that is a plain decimal followed by CRLF
  is read in one pass. Anything else (a sign, a leading zero, more than 18
  digits, a line not complete yet) takes the old `find_crlf` + `parse_ll`
  path, so the accepted input and every error message are unchanged. A test
  and the fuzz target compare it with the old parser (kept in
  `tests/kv_reference.hpp`) on commands, outcome and error text, for whole
  streams and every prefix.
- *Reply encoders.* The type byte, the number and the CRLF are built on the
  stack and appended once; a bulk reply is three appends instead of five.
- *Shard.* `std::unordered_map<std::string, std::string>` became open
  addressing with linear probing, at most half full, deletion by backward
  shift (no tombstones). A slot holds the key's 64-bit hash and a pointer to
  one heap block with the key and the value, so a hit reads the slot array
  and that block. Before, it read the bucket array, the node, the key's own
  heap buffer (the 16-byte keys do not fit libstdc++'s 15-byte inline string)
  and the value's. A test runs 60,000 random commands over a small keyspace
  (collisions, wrap-around, deletions inside probe runs) against the old map
  and checks the table's invariants; the fuzz target runs every shard command
  it parses on both.
- *One hash function.* One multiply-fold hash: its upper half picks the core,
  its lower bits the slot (before: FNV-1a for the core, `std::hash` again in
  the map). It is still computed twice per key, once when the command is
  routed and once in the owning core's table; passing it along with the
  batch was not tried.

Window 1 (`results/perf/w1`), CPU per request against `main`, paired by
repetition: median ratio, and in how many of 7 repetitions it was lower:

| | io_uring | epoll |
| --- | ---: | ---: |
| 1 core, GET depth 16 | 0.719 (7) | 0.816 (7) |
| 1 core, SET depth 16 | 0.838 (7) | 0.787 (7) |
| 2 cores, GET depth 16 | 0.837 (6) | 0.847 (7) |
| 2 cores, SET depth 16 | 0.794 (6) | 0.798 (7) |
| 1 core, GET depth 1 | 0.975 (5 of 6) | 0.947 (7) |

Instructions per request fell from 1,551 to 1,153 (io_uring) and 1,421 to
1,022 (epoll). At depth 1 system calls dominate and the change is 3–5%. It
helps both front ends by about the same amount, so it moves the comparison
with Valkey and leaves the gap to epoll where it was (window 2:
io_uring/epoll CPU 1.051 and 1.063 at 2 cores, depth 16). Deliberate bugs k
(delete without the backward shift) and l (the fast path accepts a leading
zero) fail their named assertions.

## Kept: one awaiter for calls to several cores (`reactor::call_all`)

With more than two cores a read sends batches to several cores. The front end
awaited them with `when_all` over one `forward_one` coroutine per target: two
coroutine frames per target and four vectors per read. `call_all` starts
every call from one awaiter; the call slots share a join counter, and the last
reply resumes the caller, which releases every slot before it rethrows a
failure. A destroyed awaiter detaches its slots, which keep their nodes until
the replies arrive. Tests cover results in call order, a throwing call and a
frame destroyed with replies outstanding, in every transport mode; deliberate
bug m (resume at the first reply) trips the invariant "call_all resumed before
every reply arrived".

Window 3 (`results/perf/w3`), CPU per request against the branch before it:
0.929 and 0.918 at 4 cores (GET, SET, depth 16; 7 of 7 lower), 0.949 and 0.959
at 3 cores (5 of 7), 1.016 and 0.996 at 2 cores, where the code path did not
change (control). User time per request at 4 cores fell from 194 to 164 ns;
epoll's is 123. Callgrind at 4 cores (`results/profile`): allocator
instructions per request 199 before, 61 after (epoll: 3), total 1,623 before,
1,399 after (epoll: 1,116).

## Rejected: leaner operation bookkeeping in the reactor

A slot table of fixed chunks with an inline lookup (it is a `std::deque` of
slots), queued multishot results in a vector instead of a `std::deque`,
releasing only the resources a slot holds, and a per-thread free list for
message nodes. It cut the runtime's instructions from 123 to 90 per request
and the allocator's from 11 to 3 (total 1,551 to 1,510, GET, 2 cores, depth
16), but CPU time did not follow (window 1, against `main`): 1.030 at 2 cores
GET depth 16 (2 of 7 lower; the rule needed 6), and 1.065 at 1 core SET depth
16 with 5 of 7 higher, which alone rejects it. A 2.6% change in instructions
is below what 7 paired runs on this machine resolve.

## Rejected: replies assembled straight into the send buffer

The io_uring front end assembled the replies into a string and copied it into
the send buffer (epoll sends from the string). Writing them straight into the
send buffer removed that copy (8 of 75 copy instructions per request) without
changing the total (1,552), and CPU per request did not move in window 1: the
best cell was 0.977 (2 cores, SET depth 16, 5 of 7 lower).

## Rejected: registered ring descriptor; NO_SQARRAY was already in use

`io_uring_register_ring_fd` lets `io_uring_enter` skip the descriptor lookup.
The server enters the kernel 5.7 times per 1,000 requests at depth 16 and 59
times at depth 1 (round-3 diag suite, `results/kv-bench/variants`), so there
is little to save, and window 1 found nothing: best cell 0.976 (2 cores, GET depth 16, 5 of 7 lower).
`IORING_SETUP_NO_SQARRAY` needed no change: liburing 2.14 asks for it by
default and falls back on kernels without it, and the reactor test that prints
the ring's features shows no SQ array on this kernel.

## Rejected: a spin before sleeping (both transports)

The suggested next step for the doorbell transport, which idles more at
depth 16: before sleeping, spin for 20 µs watching the completion queue, the
kernel's "task work pending" flag (`IORING_SQ_TASKRUN`, which needs
`IORING_SETUP_TASKRUN_FLAG`) and the shared-memory queues. Throughput dropped (window 2,
paired against the same transport without the spin, 2 cores): MSG_RING 0.740
(GET) and 0.633 (SET) at depth 16, doorbell 0.737 and 0.767, 0 of 7 higher in
each, with 8–9% more CPU per request. Which part costs it (the submission
before each spin, liburing entering the kernel whenever the flag is set and no
completion is visible yet, or the spin itself) was not measured; the option
was removed.

The doorbell transport itself stays an option, not the default: in the same
window it delivered 0.997 and 0.831 of MSG_RING's requests per second at 2
cores, depth 16, and 1.193 and 1.115 at 4 cores (5 and 4 of 7 higher), which
is short of the 6-of-7 bar.

## Not kept: a cheaper receive path in the load generator

`kvload` parsed replies after copying each receive into a string. Parsing in
the receive buffer lowered its CPU per request at depth 16 GET (0.956, 6 of
7) but not SET (1.004, 2 of 7), and the server's requests per second moved by
more than the 3% the rule allowed (1.095 at 2 cores, GET depth 16), so the
generator was left as it was. The same window answered the question behind
it: at 4 server cores and depth 16 the generator uses 39–59% of its 8 CPUs,
so 4-core comparisons at depth 16 are valid; at depth 1 it uses 86–89% and is
the limit (window 2, load-generator table).

## Not tried

- `IOSQE_CQE_SKIP_SUCCESS` on sends: a send that succeeds then posts no
  completion, so there is no point at which the runtime knows the kernel is
  done with the buffer. The contract hands the buffer back with the result.
- Send and receive bundles (6.10), incremental provided-buffer consumption
  (6.12): a read here is at most a few hundred bytes and fits one provided
  buffer, and buffer handling is a few instructions per request in the
  profile. Both would need a runtime probe and a fallback for Linux 6.8, the
  oldest kernel the README names.
- Zero-copy send for large values: over loopback the kernel copies anyway
  (Part 2, "Zero-copy send is not in the performance table").

## Where the remaining loss sits

At the end of the round (5d86cbd; `results/kv-bench/perf-session-a` and
`perf-session-b`, 1 h apart), io_uring needs less CPU per request than epoll
at 1 core: 0.950 / 0.936 for GET at depth 1 and 0.948 / 0.943 at depth 16
(paired by repetition; 6 or 7 of 7 lower in each session). It needs more
wherever pipelined requests cross cores: 1.036 / 1.059 (GET) and 1.080 / 1.051
(SET) at 2 cores, depth 16, and 1.117 / 1.114 and 1.133 / 1.123 at 4 cores,
where no repetition went the other way. A third check from fresh clones
(`perf-check-c`, 5 repetitions) found the same losses, somewhat larger (1.08
and 1.10 at 2 cores, 1.17 and 1.13 at 4), and GET at 1 core, depth 16 even
(1.02) instead of lower. The difference is user time (4 cores,
GET: 166 / 163 ns of user time per request against 123 / 123; system time
198 / 204 against 196 / 197) and shows in the instruction counts at 4 cores:
1,399 against 1,116 per request, of which the runtime is 218 and the
allocations for call nodes and results 61 (epoll: 3). Per read the io_uring
connection uses an operation slot for its receive item, its send and each
cross-core call and is resumed for each; the epoll loop calls `read` and
`send` and moves batches through a mutex-protected vector. Trimming that
bookkeeping by 2.6% of the instructions (the rejected candidate above) did
not register in CPU time.

Not tried yet, in order of what the profile suggests: a per-thread pool for
message nodes on its own, now that they are the main allocation at 4 cores
(it was part of the rejected bundle, measured at 2 cores, where allocations
were 11 instructions per request); and handling the next receive without
waiting for the previous send's completion (two send buffers per connection
and linked sends to keep the order), which removes one slot and one
resumption per read.

One more observation from the final sessions: at 2 and 4 cores the lower CPU
per GET did not raise GET throughput over `main` (2 cores, depth 16: 5.47 /
5.22 M against 5.49 / 5.37 M requests/s), while SET throughput rose (5.72 /
5.84 M against 5.13 / 5.06 M). The cores do the same GET work in less CPU
time and idle more; why GET leaves that time unused was not investigated.
In check C, GET throughput at 2 cores did rise over `main` (1.14, 5 of 5), so
the observation may be session noise rather than a property of the change.

# Part 4: the improvement round

The question Part 3 left: io_uring needed more CPU per request than epoll
whenever pipelined requests crossed cores (4–10% at 2 cores, 11–17% at 4,
depth 16), and callgrind put the difference in user space, about 130
instructions per request more than epoll's loop at 2 cores and 280 at 4.
The method is Part 3's: every candidate was built from its own source tree,
measured against the published `main` (a48c76c) and against the step before
it, interleaved in one quiet window under rules committed before the run
(`results/improve/w*/RULES.md`), and kept only if its rule held; an effect
under 3% needed a second session at least an hour later. Candidates: c1
(call nodes in their slots), c2 (c1 plus a chunked slot table), c3 (c2 plus
one hash per key), c4 (c3 plus an inline receive path).

## Why Part 3's leaner bookkeeping did not show

That candidate removed 41 instructions per request (1,551 to 1,510, 2.6%;
GET, 2 cores, depth 16). The server spends about half its CPU time in the
kernel (204 ns user and 196 ns system per request in that window, from the
per-thread ticks in `results/perf/w1/compare.jsonl`), so 2.6% of the
user-space instructions is about 1% of the CPU time, while its paired
repetitions in that window ranged from 0.85 to 1.11 and wider. Its user time did not move
either (204 and 206 ns). The change was too small for the instrument; it was
not shown to be useless. This round therefore aimed at changes large enough
to resolve, and the measurements below show the relation holding once they
were: at 4 cores c2 removed 141 instructions per request (1,397 to 1,256)
and user time per GET fell from 161 to 148 ns (epoll: 124), with system time
unchanged (199 and 196 ns; window A). That is 0.49 cycles per removed
instruction at 5.3 GHz, against 0.61 for the server's user code on average,
consistent with bookkeeping that was cheap per instruction (predictable,
cache-resident) but plentiful.

## Where the instructions went

GET, depth 16, user-space instructions per request (callgrind,
`results/improve/profile`):

| | 1 core | 2 cores | 4 cores |
| --- | ---: | ---: | ---: |
| `main`, io_uring | 966 | 1,151 | 1,397 |
| c1: call nodes in their slots | — | 1,139 | 1,321 |
| c2: c1 + chunked slot table (kept) | 944 | 1,105 | 1,256 |
| c3: c2 + one hash per key | — | 1,083 | 1,242 |
| c4: c3 + inline receive path (rejected) | 901 | 1,068 | 1,225 |
| `main`, epoll | 886 | 1,022 | 1,114 |
| c3, epoll | 853 | 997 | 1,093 |

At 4 cores c1 took the allocator from 60 to 2 instructions per request and
c2 the runtime from 199 to 141; c3 takes 16–31 from the engine both front
ends share. Against epoll built from the same tree the gap went from 129 to
86 instructions at 2 cores and from 283 to 149 at 4.

## Kept: call nodes in their slots, slots in fixed chunks

*Call nodes.* A call's node (its function, its result and the cross-core
links) was a heap allocation per call; at 4 cores a read calls three cores,
and the frees came back in bursts larger than glibc's per-thread cache, so
the allocator cost 60 instructions per request (epoll: 2–3). The node is now
built in 128 bytes of storage inside the call's operation slot. Ownership
does not change: the slot keeps the node until the reply has arrived, also
when the caller is gone, and a shutdown that abandons a call keeps the whole
slot table alive (a node is part of it). Larger nodes still go on the heap
(`stats().call_nodes_on_heap`; test `call_nodes_live_in_their_slots`).
`call_all` keeps up to eight slot references inline and can fill a results
vector the caller keeps. Deliberate bug n destroys a node when its caller
detaches; the reply then trips "cross-core reply for a slot that is not
waiting for it".

*Slot table.* Slots were a `std::deque` of 264-byte entries, one deque node
each, and every lookup computed the deque's size for its bounds check: 38
instructions out of line, 25–40 per request. Slots now live in chunks of 64
that never move, found with a shift and a mask; the lookup, slot
acquisition, `finish_take`, `maybe_free`, the SQE reservation's fast path and
the thread check are inline, and freeing a slot touches only what it holds.

Measured (server CPU per request against `main`, paired, 7 repetitions;
below 1 is better):

| | window A | window B |
| --- | ---: | ---: |
| c2, 4 cores, GET depth 16 | 0.954 (6 of 7) | 0.968 (7 of 7) |
| c2, 4 cores, SET depth 16 | 0.964 (7 of 7) | 0.968 (7 of 7) |
| c2, 4 cores, GET requests per second | 1.063 (7 of 7 higher) | 1.050 (7 of 7) |
| c1 alone, 4 cores, GET / SET | 0.971 (5) / 1.007 (3) | — |

In window C, which was run for c3, c2 against `main` came out weaker at 4
cores: 0.990 (5 of 7) for GET and 0.972 (5 of 7) for SET, and 1.005 (4 of 7
higher) in GET requests per second.

At 1 and 2 cores the medians ranged from 0.955 to 1.034 in the two windows;
only 1-core GET depth 1 in window A reached 6 of 7 (0.955), and it did not
repeat in window B (0.984, 4 of 7), so nothing is claimed there. By window
A's rule c2 did not pass window A: at 2 cores, SET depth 16, its median was
1.0203 with 5 of 7 higher, a worse cell (the summary prints 1.020). It passed
window B, where its best cell (0.968) is an effect of 3% or more, which the
rule keeps without a second session; by the same rule it also passed window
C (2 cores, GET and SET depth 16, 6 of 7 each), with an effect under 3%. c1
alone did not pass: no cell reached 6 of 7, and its 1-core GET depth 16 came
out at 1.061 with 6 of 7 higher in window A, a path it does not change and
that came out at 0.965 for c2 in window B; c1 is kept only as part of c2.

## Kept: one hash per key

GET and SET hashed their key twice: once to pick the owning core, once in
that core's table. The plan now hashes once, routes on the upper half and
carries the hash with the command, in the batch or directly to this core's
table (`shard::execute_hashed`). `routes_keys_to_their_cores` now also checks
each table's invariants after routed SETs, and deliberate bug o (carry the
command name's hash instead) fails it. Callgrind: 16–31 fewer instructions
per request on both front ends (about 2%).

Measured (server CPU per request, paired, 7 repetitions; the best cells and
the 4-core cells):

| | window B | window C |
| --- | ---: | ---: |
| epoll, c3 / `main`, 1 core GET depth 16 | 0.965 (5 of 7) | 0.935 (6 of 7) |
| epoll, c3 / `main`, 4 cores SET | 0.947 (7 of 7) | 0.986 (6 of 7) |
| io_uring, c3 / c2, 4 cores GET | 0.989 (5 of 7) | 0.969 (5 of 7) |
| io_uring, c3 / c2, 4 cores SET | 0.991 (6 of 7) | 0.987 (4 of 7) |

It passed its rule in both sessions (a depth-16 cell at 6 of 7 or better, no
cell worse), so it is kept. The instruction count predicts an effect of about
1–2%; that size is an estimate, not a measurement: in check D the epoll build
came out at 0.93–1.02 of `main`'s epoll build at depth 16, and only one cell
of twelve was lower in 5 of 7 repetitions. The 5–6% of the best single cells
is not claimed.

## Rejected: an inline receive path

c4 made the receive path's out-of-line calls inline (the multishot result
queue became a ring instead of a `std::deque`; `recv_stream::pop`, the
provided-buffer ring's bookkeeping and the call-target check moved into the
header; the slot helpers were forced inline). It ran 17 fewer instructions
per request at 4 cores, and callgrind's cache and branch simulation (`--cache-sim=yes
--branch-sim=yes`, `results/improve/profile/sim-*`) found fewer data-cache
misses and mispredicts than c3 (D1 read misses 14.7 against 15.0,
mispredicted branches 5.9 against 6.0 per request; instruction-cache misses
0.06 against 0.03). Measured, it needed more CPU: 4 cores GET 1.040 against
c3 with 6 of 7 higher, user time 162 against 143 ns (window B), so it was
reverted. The simulator models neither the micro-op cache nor code alignment,
and hardware counters are not available here, so the cause is not known;
the forced inlining grew the connection coroutine, the most likely suspect.
The diff is in `results/improve/candidates/`.

## Not done

- Not waiting for the send CQE before the next receive. In the closed loop a
  connection's next request cannot arrive before its replies have left, so
  this saves one coroutine resumption per read, about 3 instructions per
  request at depth 16; it needs a second send buffer and ordering between
  sends. Not worth a measurement it could not resolve.
- One slot for all calls of a read. At 4 cores the three calls of a read
  cost 60 instructions per request (table below); a shared slot would need
  room for several nodes or nodes outside the slot, and would save the two
  extra acquire/release pairs (about 10 per request).
- Faster single calls to the target: the function runs through a virtual
  call, a moved `std::unique_ptr<batch>` and an `std::optional` result.

## What is left and why

With c3 the io_uring build still runs more user-space instructions than the
epoll loop built from the same tree: 60 more per request at 1 core, 86 at 2
and 149 at 4 (GET, depth 16; the engine, parser and encoders are the same
code and cost the same in both). Nearly all of it is the front end's own
work, grouped by job (`scripts/callgrind-jobs.py`, `results/improve/profile`;
the 1-core column is c2, whose runtime and front end are c3's):

| instructions per request | io_uring, 1 core | 2 cores | 4 cores | epoll, 1 core | 2 cores | 4 cores |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| send submission | 13 | 13 | 14 | — | — | — |
| completions: reap, dispatch, resume | 35 | 36 | 39 | — | — | — |
| slot acquire and release | 9 | 19 | 27 | — | — | — |
| cross-core calls | 0 | 14 | 60 | — | — | — |
| the connection coroutine / epoll's loop | 22 | 32 | 54 | 15 | 26 | 47 |
| **total** | **79** | **116** | **195** | **15** | **26** | **47** |

At depth 16 a read carries 16 requests, so per read the io_uring connection
spends about 1,000 instructions more than epoll's loop at 1 core and 2,400
more at 4, on things the loop does not do: it builds an SQE for the send and gets a CQE back for it
and for the receive, gives each operation and each cross-core call its own
slot with a generation (so that a late CQE or reply can never reach a reused
slot, and a destroyed coroutine never loses a buffer), resumes the coroutine
three times (receive, reply, send), and sends every call through a node that
the other core runs and returns. The epoll loop calls `read` and `send`
directly and hands batches over through a locked vector; it keeps no
in-flight state because nothing is in flight. That is the price of the
ownership contract, not of io_uring's system calls: system time per request
is the same for both front ends in every window (for example 195–204 ns at
4 cores, window B).

In CPU time, in the two final sessions (`results/kv-bench/imp-session-1`
and `imp-session-2`, 07:44–08:58 and 11:13–12:28 PST, the build of c3 against
`main`, epoll, Asio and Valkey; paired by repetition):

| server CPU per request | this round / epoll | `main` / epoll | this round / `main` |
| --- | ---: | ---: | ---: |
| 1 core, GET depth 1 | 0.959 (7) / 0.924 (7) | 0.937 (7) / 0.937 (6) | 1.013 (3) / 0.985 (5) |
| 1 core, SET depth 16 | 1.075 (1) / 1.033 (1) | 1.031 (0) / 1.064 (0) | 1.006 (2) / 0.976 (5) |
| 2 cores, GET depth 16 | 1.030 (0) / 1.091 (2) | 1.103 (0) / 1.020 (2) | 0.926 (6) / 1.070 (3) |
| 2 cores, SET depth 16 | 1.082 (1) / 1.075 (3) | 1.102 (1) / 1.093 (3) | 0.961 (6) / 0.984 (4) |
| 4 cores, GET depth 16 | 1.116 (1) / 1.073 (1) | 1.131 (1) / 1.153 (0) | 0.989 (5) / 0.972 (4) |
| 4 cores, SET depth 16 | 1.085 (2) / 1.108 (0) | 1.124 (2) / 1.130 (0) | 0.966 (6) / 0.967 (5) |

(medians, repetitions below 1 of 7 in brackets). Within each session the gap
to epoll at 4 cores is smaller than `main`'s by median (7–12% against 12–15%;
in check D 5–11% against 11–18%); paired by repetition this is the comparison
with `main` that follows. The difference to `main` itself was 1–4% at 4 cores
in both sessions (4–6 of 7; GET at 4 in session 2) and 4–12% in check D (6–7
of 7). It changed sign between the sessions at 2 cores, and at 1 core GET
depth 16 was lower in both (0.990 and 0.957, 5 and 6 of 7) but in check D only
at 4 of 7 (0.991 and 0.998), and 1-core SET depth 16 was lower in check D
(0.941 and 0.974) and in session 2 (0.976) but not in session 1 (1.006). The
decision windows, with fewer arms and no open-loop runs between them, resolved
the 4-core difference at 3–5%. Session 1's 4-core runs shared the server CPUs'
SMT siblings with another project's job (interference median 0.87 of the
window, against 0.05 in session 2). The extra time is user time: at 4 cores,
GET, 168 / 148 ns per request against epoll's 135 / 121 (check D: 145 / 160
against 135 / 128), system time 222 / 201 against 213 / 203 (check D: 202 /
208 against 203 / 204).

## GET throughput at 2 cores

Part 3 saw GET throughput at 2 cores, depth 16, stay flat against 3eb4121
while CPU per request fell, and check C saw it rise. This round's windows show
why one should not read much into it: at 2 cores neither the server nor the
load generator is saturated (server cores busy 67–81% of the time for GET and
85–90% for SET, the generator's 8 CPUs 33–40%), and throughput moves with how
often the server's threads go to sleep. Across the runs of each window,
requests per second correlated with voluntary context switches per 1,000
requests at −0.67 to −0.89 for the io_uring builds (Spearman, 21–28 runs per
cell, GET and SET, windows A and B); with CPU per request it correlated at
−0.70 and −0.84 in window A but only −0.02 and −0.31 in window B. Builds with
the same 2-core code path differed as much as different front ends did: c2 and
c3 served 5.72 and 5.10 M GETs per second in window B. A cheaper request lets
a core run out of work and sleep sooner, and a GET that crosses cores then
waits for the other core to wake. At 4 cores c2 served 5–6% more GETs per
second than `main` in windows A and B (7 of 7), but not in window C (1.005, 4
of 7). This round's build served 0.953 and 0.980 times `main`'s GETs per
second in the final sessions (3 of 7 higher) and 1.021 and 1.034 in check D (5
and 4 of 7), so no throughput gain is claimed. At 2 cores this round's build
served fewer GETs per second than `main` in three of four sessions: 0.977 and
0.926 in the final sessions (2 and 0 of 7 higher), 0.869 and 1.017 in check D
(1 and 5 of 7). It served fewer SETs per second there by median in all four:
0.967 and 0.985 in the final sessions (1 and 2 of 7 higher), 0.926 and 0.997
in check D (2 and 3 of 7). What sets the 2-core figure in detail (wake-up
latency, the other core's loop iteration) needs scheduler tracing, which this
machine does not allow.
