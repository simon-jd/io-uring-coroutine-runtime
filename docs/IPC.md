# Linux immutable snapshot protocol v1

The sender creates a memfd, writes a fixed header and payload, then seals write,
grow, shrink and further seal changes before mapping it read-only. The receiver
gets its own descriptor through one `SCM_RIGHTS` message on a connected
`AF_UNIX`/`SOCK_SEQPACKET` socket. Socket setup and peer authentication belong
to the application.

Both directions run through the reactor: `ipc::send_snapshot` is an
`IORING_OP_SENDMSG`, `ipc::receive_snapshot` an `IORING_OP_RECVMSG` with
`MSG_CMSG_CLOEXEC`.

| Offset | Field | Wire width |
| --- | --- | --- |
| 0 | magic `0x524f4c31` | 32 bits |
| 4 | version `1` | 16 bits |
| 6 | header size `48` | 16 bits |
| 8 | random object identity from `getrandom` | 128 bits |
| 24 | nonzero generation | 64 bits |
| 32 | element count, 1 through 1,048,576 | 64 bits |
| 40 | reserved, zero | 64 bits |
| 48 | `uint32_t` payload | `count * 4` bytes |

All integers are little endian; another endian target fails compilation. The same
48-byte header travels with the descriptor and is stored at the start of the
memfd. The receiver checks version, dimensions, reserved bytes, nonzero identity,
required seals, regular-file type, exact file length and byte-for-byte header
agreement before mapping. The descriptor number is never treated as identity.
Identity is a consistency check, not authentication: a malicious peer can create
arbitrary sealed snapshots, and the application decides whose data it accepts.

## Descriptor ownership in the async path

- **Sending.** `reactor::sendmsg` duplicates every descriptor to pass into the
  operation slot before the operation exists. Closing the original, even
  reusing its number, before the kernel executes the send cannot change which
  file is sent (`sendmsg_owns_descriptor_copies`).
- **Receiving.** The control buffer lives in the slot. When the `RECVMSG` CQE
  is reaped, the reactor walks the control bytes (bounds-checked; it does not
  trust `cmsg_len`) and takes ownership of every descriptor in `SCM_RIGHTS`,
  whether or not the receiving coroutine still exists. If it was destroyed, the
  slot closes them (`detached_receive_closes_received_descriptor`).
- **Validating.** `ipc::inspect_packet(payload, control, msg_flags)` is the
  whole packet policy, without system calls: exactly 48 payload bytes, no
  `MSG_TRUNC`/`MSG_CTRUNC`, only `SOL_SOCKET`/`SCM_RIGHTS`, exactly one
  descriptor, a valid header. It never closes anything; rejected packets'
  descriptors are already owned and closed by RAII. It is the libFuzzer target
  (`fuzz/ipc_packet_fuzz.cpp`).

```mermaid
sequenceDiagram
    participant P as Producer process
    participant K as Kernel
    participant C as Consumer reactor
    P->>K: create, populate, seal memfd
    P->>K: IORING_OP_SENDMSG (header + SCM_RIGHTS of a duplicate)
    Note over P: exit or SIGKILL
    C->>K: IORING_OP_RECVMSG, MSG_CMSG_CLOEXEC
    K-->>C: CQE; descriptors now owned by the slot
    C->>C: inspect_packet; adopt: seals, length, header; mmap read-only
    C->>K: munmap and close the last receiver reference
```

## Tests

The crash test waits for the producer's SIGKILL status before the receiver maps
the descriptor. Other tests cover exit before export, replay with changed
identity or generation, unsealed/truncated/non-memfd input, immutability, 2 and
12 descriptors in one message (the second overflows the control buffer and sets
`MSG_CTRUNC`), oversized payloads, and descriptor baselines over 400 malformed
and 200 valid imports. Reusing the same immutable snapshot is permitted; this is
not a one-shot token or revocable capability.

Primary references: [memfd_create and sealing](https://man7.org/linux/man-pages/man2/memfd_create.2.html),
[UNIX sockets and descriptor passing](https://man7.org/linux/man-pages/man7/unix.7.html),
[io_uring_prep_recvmsg](https://man7.org/linux/man-pages/man3/io_uring_prep_recvmsg.3.html).
