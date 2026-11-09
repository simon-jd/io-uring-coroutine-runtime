# Buffer ownership contract

The runtime's one rule: **memory the kernel may read or write belongs to the
operation, not to the coroutine that started it, until the kernel's last
completion for that operation has been reaped.** Everything below is a
consequence of that rule or a check that enforces it.

## Who owns what

| Resource | Before the operation | While in flight | After the result |
| --- | --- | --- | --- |
| `owned_buffer` (heap) | caller | operation slot | caller (moved back in the result) |
| `fixed_lease` (registered buffer) | caller | operation slot, pool state `in_flight` | caller, state `leased` |
| SEND_ZC buffer (lease or owned) | caller | operation slot | slot until the notification CQE, then pool / freed |
| `provided_buffer` (buffer ring) | kernel (in the ring) | kernel | holder, until `release()` or destruction |
| `msghdr`, `iovec`, control bytes, socket address | — | slot (`msg_storage`) | slot (reused) |
| descriptors passed with `sendmsg` | caller | slot owns duplicates | duplicates closed |
| descriptors received with `recvmsg` | — | kernel | slot from the CQE on; caller after the result |
| timeout `timespec` | — | slot | slot |
| cross-core call node and result | — | calling core's slot (target core writes the result) | caller |
| cross-core post node | sending core | target core while it runs | sending core again (freed there) |
| direct descriptor index | `direct_fd` holder | holder, plus every operation that names it | free only after its close completed and no operation names it |

Operations take buffers **by value**. The public API has no call that hands the
kernel a pointer whose lifetime the runtime does not control.

## Operation slots

An operation lives in a slot in the reactor's slot table. The coroutine's
awaiter holds only `{slot index, generation}`. The SQE's `user_data` encodes
`[tag:8][generation:24][index:32]`, so a CQE (or an `ASYNC_CANCEL` aimed at an
old operation) can never be applied to a slot that was reused.

A slot counts the CQEs it still expects (`pending`): one for the result, plus
one for a linked timeout, plus one more if a SEND_ZC result carries
`IORING_CQE_F_MORE` (a notification follows). The slot is freed only when
`pending == 0` **and** either the awaiter took the result or the awaiter is gone.
Freeing is what returns a lease to its pool, frees an owned buffer, recycles
queued provided buffers and closes owned descriptors.

The awaiting coroutine is resumed when the result CQE (and, for linked timeouts,
the timeout CQE) has arrived. Resumption is inline, during CQE dispatch, so no
queue ever holds a handle to a frame that might be destroyed before it runs.
Non-I/O wake-ups (events, buffer-pool waiters, task start) go through an
intrusive ready list whose nodes live in the waiting object and unlink
themselves when destroyed.

## Destroying a coroutine with I/O in flight

Destroying the frame destroys the awaiter. Its destructor *detaches* the slot:
the waiter is cleared, the slot is marked detached, and an `ASYNC_CANCEL` is
submitted to finish the operation early. The slot keeps every resource until
the operation's own CQE. Nothing touches the destroyed frame afterwards.

Which way the race goes depends on the ring type, and the tests cover both:

- With `DEFER_TASKRUN` (the default), completions run only inside the reactor's
  own `io_uring_enter`, after its submissions. The cancel therefore reaches a
  poll-armed socket operation first, even if data already arrived.
- On a ring without `DEFER_TASKRUN`/`COOP_TASKRUN`, task work runs on any
  return to user space. A `write(2)` by the same thread completes the operation
  — the kernel writes into the slot's buffer — after the frame is gone and
  before the cancel is submitted (`destroyed_coroutine_keeps_fixed_buffer_until_cqe`).

## Cancellation

`cancel_source::request()` submits one `IORING_OP_ASYNC_CANCEL` per registered
in-flight operation. `co_await src.cancel()` also waits for every
acknowledgement and reports how many were `0`, `-ENOENT` (already completed) or
`-EALREADY`. Operations started with a source that was already cancelled
complete with `-ECANCELED` without reaching the kernel.

- A cancelled operation's caller sees **either `-ECANCELED` or the real
  result**. The kernel can finish the operation before the cancel reaches it;
  the race is part of the contract (`cancel_vs_complete_race`: 10,000 rounds per
  ring type, both outcomes must occur, every byte arrives exactly once).
- The acknowledgement is not the release. On this kernel the acknowledgement
  CQE is reaped before the target's `-ECANCELED` CQE in every round of
  `cancel_ack_does_not_release_buffer`; in that window the buffer still belongs
  to the operation.
- A linked timeout (`op_options::timeout`) that fires is reported as `-ETIME`.
  The awaiter resumes after both the operation's and the timeout's CQE.

## Zero-copy send

`send_zc` resumes the caller at the first CQE with the byte count. The buffer
stays in the slot until the `IORING_CQE_F_NOTIF` CQE and only then returns to
its pool (lease) or is freed (owned buffer). With
`IORING_SEND_ZC_REPORT_USAGE` the reactor counts notifications whose data the
kernel copied anyway; over loopback TCP it always does, at delivery time, which
is why reusing the buffer early corrupts what the peer reads
(`send_zc_buffer_held_until_notification`).

## Buffers

- **Fixed pool.** One `mmap` region registered with `io_uring_register_buffers`.
  Each buffer is `free`, `leased` or `in_flight`, with a 32-bit generation that
  moves on with every new lease. A `buffer_handle {pool, index, generation}`
  resolves through `view()` only while that exact lease is live and not in
  flight; stale and foreign handles throw `Failure(stale)`, in-flight ones
  `Failure(busy)`. Leases from another reactor's pool are rejected before
  submission. Waiters for a buffer are served FIFO.
- **Provided-buffer rings.** The kernel picks a buffer for each multishot
  receive completion; the reactor records it as held (`kernel_returned`) and
  hands it out as a `provided_buffer`. Returning it re-adds it to the ring. When
  the ring is empty the multishot ends with `-ENOBUFS`; data stays in the socket
  and a new multishot picks it up once buffers are returned. Destroying a
  stream returns its queued buffers immediately and recycles any buffer the
  kernel still hands to the detached operation.
- **Bounded multishot receive.** `recv_multishot(..., max_queued)` cancels the
  receive once `max_queued` results wait unconsumed. Every core shares one
  buffer ring, so without the bound one connection whose coroutine is blocked
  (for example on a peer that does not read its replies) could hold every
  buffer and starve the others. The data stays in the socket (TCP flow control
  pushes back) and the consumer re-arms after it has caught up
  (`slow_reader_does_not_block_other_connections` in the server tests).

## Direct descriptors

A reactor created with `direct_files = N` registers a sparse file table of N
entries. `adopt_direct(unique_fd&&)` moves a socket into a free entry (the plain
descriptor is closed; the table holds the file) and returns a `direct_fd`.
Operations take a `file_ref`, which converts implicitly from a plain `int` or
from a `direct_fd`; a direct reference carries the entry's index and the
generation of the `direct_fd` that opened it.

- **Stale references are refused.** Destroying the `direct_fd` marks the entry
  closing and submits `IORING_OP_CLOSE` on the fixed index. From that moment an
  operation started through a kept `file_ref` completes with `-EBADF` without
  reaching the kernel, even after the index has been reused for another
  socket (`stale_direct_descriptor_is_refused`). With plain descriptors the
  same mistake reaches whatever socket reused the number.
- **An index belongs to its operations.** Every operation that names an index
  pins it until its last CQE. The index is handed out again only when the close
  has completed and no pin remains
  (`direct_index_reserved_while_operation_in_flight`). An operation already
  submitted when the entry is closed keeps its own reference to the file and
  completes normally.
- Direct descriptors are per ring. The server's accept loop hands a plain
  descriptor to the target core, which adopts it into its own table.

## Cross-core messages

`reactor::post(core, fn)` and `co_await reactor::call(core, fn)` append a heap
node to the reactor's outbox for the target core. Before each loop iteration
submits, every non-empty outbox goes out as one chain (nodes linked through
`next`). With `reactor_config::batch_messages = false` every node is sent on
its own. Two transports carry the chains:

- **MSG_RING (default).** One `IORING_OP_MSG_RING` per chain, carrying the head
  pointer in the target CQE's `user_data` (`IOSQE_CQE_SKIP_SUCCESS`, so only a
  refused send produces a sender CQE).
- **Doorbell (`reactor_config::doorbell = true`).** One shared-memory queue per
  (sender, target) pair: a stack of chains with one producer and one consumer.
  The sender pushes with a compare-and-swap; the target takes the whole stack
  with an exchange after every reap and restores the send order, so messages
  from one sender arrive in the order they were sent. A MSG_RING without a
  message (the doorbell) is sent only to wake a target that may be asleep.

Ownership is the same on both:

- A **call** node belongs to the **calling** core's slot throughout. The target
  runs `fn`, writes the result into the node and appends it, as a reply, to its
  own outbox for the calling core; the caller's slot completes when that chain
  arrives. If the caller's coroutine is destroyed meanwhile, the slot keeps the
  node until the reply arrives, for the same reason it keeps buffers.
- `co_await reactor::call_all(calls)` starts one call per (core, fn) pair from
  one awaiter. Each call has its own slot and node, owned as above; the slots
  share a counter in the awaiter, and the last reply resumes the caller, which
  takes every result (and releases every slot) before it rethrows the first
  failure. If the caller is destroyed with replies outstanding, every slot
  detaches from the counter and keeps its node until its own reply arrives.
- A **post** node is run by the target and, when batching, travels back in the
  next chain to the core that allocated it and is freed there; freeing it on
  the target costs one cross-thread free per message (measured in
  [design-tradeoffs.md](design-tradeoffs.md)). Unbatched, the target frees it.
- Each node carries an atomic publication word. The sender stores it (release)
  only after every `next` link of the chain is final; the receiver loads it
  (acquire) before reading the node. Over MSG_RING the kernel already orders
  these accesses and the word makes that ordering visible to ThreadSanitizer,
  which cannot see through `io_uring_enter`; over the doorbell transport the
  queue's compare-and-swap and exchange order them.
- `runtime::outstanding()` counts posts until they have run (and come back)
  and calls until their reply has been reaped. Each core counts in its own
  cache line; the sum is exact once no core can send, which is the only time
  shutdown relies on it.

### The doorbell rule

A core that is about to wait in `io_uring_enter` first submits its pending
SQEs, then **announces** that it is going to sleep (a per-core flag, set
sequentially consistent), then **looks at its queues once more**; it waits only
if they are all empty, and clears the flag when the wait returns. A sender
**publishes** its chain (sequentially consistent compare-and-swap), then
**reads** the target's flag; if the flag is set, it clears it with an exchange
and rings. Because both sides write and then read with sequentially consistent
operations, at least one of them sees the other: either the sender rings, or
the target finds the chain and does not sleep. The exchange makes one sender
ring per announcement; a target that is woken drains every queue.

`sleeping_core_is_woken_for_every_message` drives the window this rule closes
(a call that arrives while the target finishes other work and is about to
sleep) 4,000 times with nothing else to wake the target; a watchdog thread
that sees no progress for 1 s wakes every core and counts a stall, and the
case requires zero stalls. Deliberate bug j removes the second look at the
queues and fails that assertion within a few rounds instead of hanging.

### Refused MSG_RINGs

The kernel can refuse a MSG_RING (`-ENOMEM` for the request it allocates for a
`DEFER_TASKRUN` target, `-EOVERFLOW` when a completion queue cannot take the
CQE, `-EBADFD` for a disabled ring). The refused chain comes back to the
sender in a CQE and is still the sender's:

- A refused **request** fails: a call completes with `Failure(closed)` carrying
  the kernel's error, a post is dropped and counted in `messages_failed`.
- A refused **reply** (or a post node on its way home) belongs to the other
  core and must arrive, so it is sent again 1 ms later, and again after each
  refusal, until the kernel takes it. Until then the call stays pending; the
  target cannot fail it, because only the caller's thread may complete the
  caller's slot.
- A refused **doorbell** is rung again 1 ms later: the target may be asleep
  with messages queued, and no other sender will ring (the refused sender
  cleared the announcement).

The tests inject real kernel refusals through `reactor_config::message_fault`
(a test hook that points a MSG_RING at a descriptor that is not a ring): a
refused request fails its call with `EBADFD` and is not run; a refused post is
dropped and counted; three refused replies are sent again and every call
completes with its own result; two refused doorbells are rung again and the
call completes.

## Shutdown

`reactor::shutdown()` (also run by the destructor):

1. stop accepting work: `spawn` throws, new operations complete with `-ECANCELED`;
2. cancel every in-flight operation;
3. let spawned tasks observe the cancellation and finish;
4. destroy tasks that did not finish (their operations detach);
5. reap until no slot is in flight;
6. unregister fixed buffers and buffer rings, then close the ring.

Step 5 waits for SEND_ZC notifications too (`shutdown_waits_for_zc_notification`).
If `shutdown_timeout` expires first, the remaining slots are **abandoned**: their
memory is parked in a process-lifetime list instead of being freed, because the
kernel may still use it (`stats().abandoned`).

The thread-per-core `runtime` keeps every ring alive and serving messages until
all root tasks have returned, then refuses new messages, lets each core cancel
and drain its own operations while still answering requests already in flight,
and closes no ring until a global count of outstanding messages is zero.

## What the API cannot prevent

- Code that kept a raw pointer or span into a buffer before moving the buffer
  into an operation can still write through it. The memory stays valid (the
  slot owns it), but the write races with the kernel. C++ cannot forbid this.
- Operations that name a **plain** descriptor number can reach a different
  socket if the number is closed and reused before the operation is submitted
  (the next loop iteration). Direct descriptors close this gap (see above);
  plain descriptors keep it.
- A coroutine frame that the reactor does not own and that outlives the reactor
  must not be destroyed after it: its awaiter would call into a dead reactor.
  Frames handed to `spawn` cannot outlive their reactor.
- A task that ignores `-ECANCELED` and loops forever keeps shutdown busy until
  `shutdown_timeout`.

## Invariant checks

Internal ownership invariants are checked in every build and abort with a
message (`ROL_INVARIANT`): a CQE for a slot that is not in flight, a second
result CQE for one operation, a notification nobody expects, a buffer released
twice or by a stale lease, a provided buffer returned twice, an unpublished
cross-core message, a reply for a slot that is not waiting for one, a
`call_all` caller resumed before every reply arrived, a refused
doorbell for a core that does not exist, a direct descriptor unpinned more
often than pinned.
