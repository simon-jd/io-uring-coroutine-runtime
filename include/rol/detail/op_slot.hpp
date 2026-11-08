#pragma once
#include "rol/buffer.hpp"
#include "rol/buffers.hpp"
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>
#include <linux/time_types.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace rol {
class reactor;
class cancel_source;

struct io_result {
    int res = 0;
    std::uint32_t flags = 0;
};

namespace detail {

enum class op_kind : std::uint8_t { generic, send_zc, multishot, recvmsg, cancel, xcall };

struct slot_ref {
    std::uint32_t index = 0;
    std::uint32_t gen = 0;
    explicit operator bool() const noexcept { return gen != 0; }
};

// user_data layout: [tag:8][generation:24][slot index:32]. Tag 0 carries a
// pointer to a chain of cross-core messages delivered by IORING_OP_MSG_RING;
// tag 4 returns such a chain to its sender when the kernel refused it. Tag 5
// is a doorbell (a MSG_RING without a message: the messages wait in shared
// memory); tag 6 tells the sender that the kernel refused a doorbell, with the
// target core in the low bits.
enum class ud_tag : std::uint8_t {
    message = 0,
    op = 1,
    linked_timeout = 2,
    cancel = 3,
    chain_failed = 4,
    doorbell = 5,
    doorbell_failed = 6,
};
inline constexpr std::uint32_t gen_mask = 0xFFFFFFu;
inline std::uint64_t encode(ud_tag tag, slot_ref ref) noexcept {
    return (std::uint64_t(tag) << 56) | (std::uint64_t(ref.gen & gen_mask) << 32) | ref.index;
}
inline ud_tag tag_of(std::uint64_t ud) noexcept { return static_cast<ud_tag>(ud >> 56); }
inline slot_ref decode(std::uint64_t ud) noexcept {
    return {static_cast<std::uint32_t>(ud), static_cast<std::uint32_t>(ud >> 32) & gen_mask};
}

// Storage the kernel reads or writes for message-style operations. It lives in
// the operation slot, never in the coroutine frame.
struct msg_storage {
    msghdr msg{};
    iovec iov{};
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int) * 8)]{};
    sockaddr_storage addr{};
    socklen_t addrlen = 0;
};

// A cross-core message. Messages to one core are linked through `next` and
// sent as one chain, either in the user_data of an IORING_OP_MSG_RING or
// through the shared-memory queue for the (sender, target) pair. `stage` is
// the publication word: the sender stores 1 (request) or 2 (reply) with
// release once the chain is complete, before handing it over, and the receiver
// loads it with acquire before reading the node. The kernel's MSG_RING (or the
// queue's own atomic) already orders the accesses; the word makes that
// ordering visible to ThreadSanitizer, which cannot see through io_uring.
struct xnode {
    std::atomic<std::uint32_t> stage{0};
    bool is_call = false;
    bool reply = false;      // a call node travelling back to its origin
    unsigned origin = 0;
    std::uint64_t reply_ud = 0;
    xnode* next = nullptr;
    virtual ~xnode() = default;
    virtual void run(reactor& target) noexcept = 0;
};

// One in-flight operation. Owned by the reactor, not by the awaiting coroutine.
// Everything the kernel may touch (buffers, iovecs, msghdr, control data, file
// descriptors being passed, timespecs, cross-core reply targets) lives here
// until the operation's last CQE has been reaped.
struct op_slot {
    std::uint32_t gen = 0;
    op_kind kind = op_kind::generic;
    bool active = false;
    bool primary_done = false;   // the operation's own result CQE arrived
    bool timeout_pending = false; // a linked timeout CQE is still expected
    bool detached = false;       // the awaiting coroutine is gone
    bool taken = false;          // the awaiter collected the result
    bool cancel_sent = false;
    bool ended = false;          // multishot: final CQE (no F_MORE) arrived
    std::uint8_t pending = 0;    // CQEs still expected from the kernel
    std::uint32_t max_queued = 0; // multishot: cancel once this many results wait
    std::int32_t pinned_file = -1; // direct descriptor index this operation names
    std::int32_t closes_file = -1; // direct descriptor index this close releases
    int res = 0;
    std::uint32_t flags = 0;
    int timeout_res = 0;
    std::coroutine_handle<> waiter;
    cancel_source* source = nullptr;

    owned_buffer owned;
    fixed_lease lease;
    provided_buffer_ring* pring = nullptr;
    std::deque<io_result> queue;
    std::unique_ptr<msg_storage> msg;
    std::vector<unique_fd> fds;
    std::unique_ptr<xnode> xcall;
    __kernel_timespec ts{};

    slot_ref target;                // cancel operations: what they cancel
    cancel_source* reporter = nullptr;
};

} // namespace detail
} // namespace rol
