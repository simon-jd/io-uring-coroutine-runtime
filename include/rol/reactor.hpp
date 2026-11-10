#pragma once
#include "rol/buffer.hpp"
#include "rol/buffers.hpp"
#include "rol/detail/op_slot.hpp"
#include "rol/detail/ready.hpp"
#include "rol/error.hpp"
#include "rol/task.hpp"
#include <liburing.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>
#include <fcntl.h>

namespace rol {

class runtime;
class reactor;

struct buffer_result {
    int res = 0;
    owned_buffer buffer;
};
struct lease_result {
    int res = 0;
    fixed_lease lease;
};
struct recvmsg_result {
    int res = 0;
    int msg_flags = 0;
    owned_buffer payload;
    owned_buffer control;          // control bytes as the kernel returned them
    std::vector<unique_fd> fds;    // every descriptor received, already owned
};
struct recv_item {
    int res = 0;
    provided_buffer buffer;
    bool more = false;
};
struct send_all_result {
    int res = 0;                   // 0 or the first negative result
    std::size_t sent = 0;
    unsigned submissions = 0;      // > 1 means the kernel returned short writes
    owned_buffer buffer;
};

class direct_fd;

// The file an operation works on: a plain descriptor, or an entry in the
// reactor's registered file table (a direct descriptor) with the generation of
// the direct_fd that opened it. A reference whose direct_fd has been closed is
// refused (-EBADF) instead of reaching whatever later reuses the index.
struct file_ref {
    int fd = -1;
    bool fixed = false;
    std::uint32_t gen = 0;
    file_ref() = default;
    file_ref(int d) noexcept : fd(d) {} // NOLINT: plain descriptors convert implicitly
    file_ref(const direct_fd& d) noexcept;
};

struct op_options {
    cancel_source* cancel = nullptr;
    std::chrono::nanoseconds timeout{0}; // > 0 adds an IORING_OP_LINK_TIMEOUT
};

inline constexpr unsigned default_setup_flags =
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL;

struct reactor_config {
    unsigned sq_entries = 256;
    unsigned cq_entries = 0; // 0 means 4 * sq_entries
    unsigned setup_flags = default_setup_flags;
    unsigned fixed_buffers = 0;
    std::size_t fixed_buffer_size = 0;
    std::chrono::milliseconds shutdown_timeout{5000};
    // Cross-core messages to one core are collected during a loop iteration
    // and sent as one chain per IORING_OP_MSG_RING when the iteration submits.
    // false sends one MSG_RING per message.
    bool batch_messages = true;
    // When batching, a post node goes back to the core that allocated it to be
    // freed there; false frees it on the target (kept to measure the cost).
    bool return_posts = true;
    // Never sleep in io_uring_enter; poll for completions instead.
    bool busy_poll = false;
    // Size of the registered file table for direct descriptors (0: none).
    unsigned direct_files = 0;
    // Cross-core transport. false: every chain travels in the user_data of an
    // IORING_OP_MSG_RING. true: chains go through a shared-memory queue per
    // (sender, target) pair; the target drains its queues every loop
    // iteration, and the sender rings the target (a MSG_RING without a
    // message) only when the target has announced that it is about to sleep.
    bool doorbell = false;
    // Doorbell transport: submit the iteration's SQEs before announcing the
    // sleep (default). false announces first and submits in the same
    // io_uring_enter that waits; kept to measure that alternative.
    bool submit_before_announce = true;
    // Test hook, empty in production: called for every MSG_RING this reactor
    // sends (target core, true for a doorbell). A non-negative result replaces
    // the target ring's descriptor; a descriptor that is not a ring makes the
    // kernel refuse the MSG_RING (-EBADFD).
    std::function<int(unsigned target, bool doorbell)> message_fault{};
};

struct reactor_stats {
    std::uint64_t sqes = 0;
    std::uint64_t cqes = 0;
    std::uint64_t ops_started = 0;
    std::uint64_t ops_completed = 0;
    std::uint64_t ops_short_circuited = 0; // cancelled before reaching the kernel
    std::uint64_t detached = 0;            // awaiting coroutine destroyed in flight
    std::uint64_t detached_released = 0;   // detached ops whose last CQE released them
    std::uint64_t cancels = 0;
    std::uint64_t cancel_ok = 0;
    std::uint64_t cancel_not_found = 0;
    std::uint64_t cancel_already = 0;
    std::uint64_t cancel_other = 0;
    std::uint64_t multishot_paused = 0;    // multishot receives stopped by max_queued
    std::uint64_t zc_notifications = 0;
    std::uint64_t zc_copied = 0;
    std::uint64_t messages_sent = 0;     // posts and calls started here
    std::uint64_t messages_received = 0; // posts and calls run here
    std::uint64_t messages_failed = 0;
    std::uint64_t call_nodes_on_heap = 0; // call nodes too large for their slot's storage
    std::uint64_t replies_sent = 0;
    std::uint64_t message_rings = 0;     // MSG_RING SQEs carrying message chains
    std::uint64_t queue_pushes = 0;      // chains pushed onto shared-memory queues
    std::uint64_t doorbells = 0;         // MSG_RING SQEs sent only to wake a core
    std::uint64_t wait_aborts = 0;       // waits skipped: the re-check found messages
    std::uint64_t ring_retries = 0;      // refused MSG_RINGs sent again (replies, doorbells)
    std::uint64_t enters = 0;            // loop iterations that entered the kernel
    std::uint64_t blocking_enters = 0;   // ... of which waited for a completion
    std::uint64_t abandoned = 0; // still in flight when shutdown gave up
    std::size_t leases_at_close = 0;        // fixed-buffer leases still held at close
    std::size_t provided_held_at_close = 0; // provided buffers still held at close
    std::size_t in_flight = 0;
    std::size_t peak_in_flight = 0;
    std::size_t tasks = 0;
};

// Cancellation scope. Operations started with a source are cancelled with
// IORING_OP_ASYNC_CANCEL when it is requested. A cancelled operation's caller
// sees either -ECANCELED or the operation's real result: the kernel may finish
// the operation before the cancel reaches it, and that race is part of the
// contract. Acknowledgement of the cancel never releases the operation's
// buffers; only the operation's own completion does.
class cancel_source {
public:
    struct report {
        unsigned requested = 0;
        unsigned ok = 0;
        unsigned not_found = 0;
        unsigned already = 0;
        unsigned other = 0;
    };

    explicit cancel_source(reactor& r) noexcept : r_(&r) {}
    cancel_source(const cancel_source&) = delete;
    cancel_source& operator=(const cancel_source&) = delete;
    ~cancel_source();

    void request();
    bool requested() const noexcept { return requested_; }
    std::size_t registered() const noexcept { return ops_.size(); }

    class cancel_awaiter {
    public:
        explicit cancel_awaiter(cancel_source& s) noexcept : s_(&s) {}
        cancel_awaiter(const cancel_awaiter&) = delete;
        cancel_awaiter& operator=(const cancel_awaiter&) = delete;
        ~cancel_awaiter() {
            if (waiting_ && s_->ack_waiter_ == h_) s_->ack_waiter_ = {};
        }
        bool await_ready() {
            s_->request();
            return s_->acks_pending_ == 0;
        }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            h_ = h;
            waiting_ = true;
            s_->ack_waiter_ = h;
        }
        report await_resume() noexcept {
            waiting_ = false;
            return s_->report_;
        }

    private:
        cancel_source* s_;
        std::coroutine_handle<> h_;
        bool waiting_ = false;
    };
    // Requests cancellation and waits until the kernel acknowledged every cancel.
    cancel_awaiter cancel() noexcept { return cancel_awaiter(*this); }

private:
    friend class reactor;
    void on_ack(int res) noexcept;
    reactor* r_;
    bool requested_ = false;
    std::vector<detail::slot_ref> ops_;
    unsigned acks_pending_ = 0;
    report report_{};
    std::coroutine_handle<> ack_waiter_;
};

// One entry of a reactor's registered file table. Move-only; destroying it
// closes the entry (IORING_OP_CLOSE on the fixed index). The index is reused
// only after that close has completed and no operation that named the index is
// still in flight. Must not outlive its reactor.
class direct_fd {
public:
    direct_fd() = default;
    direct_fd(direct_fd&& o) noexcept
        : r_(std::exchange(o.r_, nullptr)), index_(o.index_), gen_(o.gen_) {}
    direct_fd& operator=(direct_fd&& o) noexcept {
        if (this != &o) {
            reset();
            r_ = std::exchange(o.r_, nullptr);
            index_ = o.index_;
            gen_ = o.gen_;
        }
        return *this;
    }
    direct_fd(const direct_fd&) = delete;
    direct_fd& operator=(const direct_fd&) = delete;
    ~direct_fd() { reset(); }
    void reset() noexcept;
    explicit operator bool() const noexcept { return r_ != nullptr; }
    unsigned index() const noexcept { return index_; }
    std::uint32_t generation() const noexcept { return r_ ? gen_ : 0; }

private:
    friend class reactor;
    direct_fd(reactor* r, unsigned index, std::uint32_t gen) noexcept
        : r_(r), index_(index), gen_(gen) {}
    reactor* r_ = nullptr;
    unsigned index_ = 0;
    std::uint32_t gen_ = 0;
};

inline file_ref::file_ref(const direct_fd& d) noexcept
    : fd(static_cast<int>(d.index())), fixed(true), gen(d.generation()) {}

// A one-shot event for coordinating tasks on one reactor.
class event {
public:
    explicit event(reactor& r) noexcept : r_(&r) {}
    event(const event&) = delete;
    event& operator=(const event&) = delete;
    ~event();
    void set() noexcept;
    bool is_set() const noexcept { return set_; }

    class awaiter {
    public:
        explicit awaiter(event& e) noexcept : e_(&e) {}
        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;
        ~awaiter() { unlink(); }
        bool await_ready() const noexcept { return e_ == nullptr || e_->set_; }
        void await_suspend(std::coroutine_handle<> h) noexcept;
        void await_resume() const noexcept {}

    private:
        friend class event;
        void unlink() noexcept;
        event* e_;
        detail::ready_node node_;
        awaiter* prev_ = nullptr;
        awaiter* next_ = nullptr;
        bool linked_ = false;
    };
    awaiter wait() noexcept { return awaiter(*this); }

private:
    reactor* r_;
    bool set_ = false;
    awaiter* head_ = nullptr;
};

namespace detail {

struct unit {};

// Frame for a task handed to reactor::spawn. The reactor owns it.
struct spawn_promise {
    reactor* r = nullptr;
    ready_node node;
    spawn_promise* prev = nullptr;
    spawn_promise* next = nullptr;

    std::coroutine_handle<spawn_promise> get_return_object() noexcept {
        return std::coroutine_handle<spawn_promise>::from_promise(*this);
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    struct final_awaiter {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<spawn_promise> h) const noexcept;
        void await_resume() const noexcept {}
    };
    final_awaiter final_suspend() const noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept;
};
struct spawn_coro {
    using promise_type = spawn_promise;
    std::coroutine_handle<spawn_promise> h;
    spawn_coro(std::coroutine_handle<spawn_promise> handle) noexcept : h(handle) {}
};
spawn_coro spawn_body(task<void> t);

// Frame for block_on's root task.
template <class R>
struct root_promise {
    std::optional<R> value;
    std::exception_ptr error;
    ready_node node;
    std::coroutine_handle<root_promise> get_return_object() noexcept {
        return std::coroutine_handle<root_promise>::from_promise(*this);
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    std::suspend_always final_suspend() const noexcept { return {}; }
    template <class U>
    void return_value(U&& v) {
        value.emplace(std::forward<U>(v));
    }
    void unhandled_exception() noexcept { error = std::current_exception(); }
};
template <class R>
struct root_coro {
    using promise_type = root_promise<R>;
    std::coroutine_handle<root_promise<R>> h;
    root_coro(std::coroutine_handle<root_promise<R>> handle) noexcept : h(handle) {}
    root_coro(root_coro&& o) noexcept : h(std::exchange(o.h, {})) {}
    ~root_coro() {
        if (h) h.destroy();
    }
};
template <class T>
root_coro<T> root_body(task<T> t) {
    co_return co_await std::move(t);
}
inline root_coro<unit> root_body_void(task<void> t) {
    co_await std::move(t);
    co_return unit{};
}

template <class F>
struct post_node final : xnode {
    F fn;
    explicit post_node(F f) : fn(std::move(f)) {}
    void run(reactor& target) noexcept override;
};
template <class F, class R>
struct call_node final : xnode {
    F fn;
    std::optional<std::conditional_t<std::is_void_v<R>, unit, R>> result;
    std::exception_ptr error;
    explicit call_node(F f) : fn(std::move(f)) {}
    void run(reactor& target) noexcept override {
        try {
            if constexpr (std::is_void_v<R>) {
                fn(target);
                result.emplace();
            } else {
                result.emplace(fn(target));
            }
        } catch (...) {
            error = std::current_exception();
        }
    }
};

} // namespace detail

class recv_stream;

// One io_uring instance driven by one thread. Owns the operation slots, the
// registered buffers, the provided-buffer rings and every spawned task.
class reactor {
public:
    explicit reactor(reactor_config cfg = {});
    ~reactor();
    reactor(const reactor&) = delete;
    reactor& operator=(const reactor&) = delete;

    // Runs `t` to completion on the calling thread and returns its result.
    template <class T>
    T block_on(task<T> t);

    // Starts `t` on this reactor. The reactor owns it: it is destroyed when it
    // finishes or, at the latest, during shutdown. Throws Failure(closed) once
    // shutdown has begun.
    void spawn(task<void> t);

    // Stop accepting work, cancel every operation, destroy unfinished tasks,
    // drain to zero in-flight operations, then unregister buffers and close the
    // ring. Idempotent; the destructor calls it.
    void shutdown();
    bool closing() const noexcept { return closing_; }

    const reactor_stats& stats() const noexcept { return stats_; }
    std::size_t in_flight() const noexcept { return stats_.in_flight; }
    unsigned setup_flags() const noexcept { return setup_flags_; }
    int ring_fd() const noexcept { return ring_.ring_fd; }

    fixed_buffer_pool& fixed_buffers();

    // Direct descriptors (reactor_config::direct_files > 0). adopt_direct moves
    // `fd` into a free table entry (the plain descriptor is closed; the table
    // holds the file). Throws Failure(full) when no entry is free.
    direct_fd adopt_direct(unique_fd&& fd); // `fd` is left untouched if this throws
    enum class direct_state : std::uint8_t { free, open, closing };
    direct_state direct_state_of(unsigned index) const;
    std::uint32_t direct_pins(unsigned index) const;
    provided_buffer_ring& add_provided_ring(std::uint16_t group, unsigned entries,
                                            std::size_t buffer_size);

    // Cross-core (only on reactors created by a runtime).
    unsigned core() const noexcept { return core_; }
    unsigned cores() const noexcept;
    template <class F>
    void post(unsigned target, F fn);
    template <class F>
    auto call(unsigned target, F fn);
    // Calls several cores at once: calls[i].second runs on core calls[i].first
    // (functions are moved out of `calls`). Resumes once every reply is back
    // and returns the results in the order of `calls`. If calls fail, every
    // slot is still released first; then the first failure in that order is
    // thrown. One slot per call, its node built in the slot; no coroutine
    // frames. The second form puts the results into `results` (cleared
    // first; its contents are unspecified after a failure), so a caller that
    // keeps the vector allocates nothing per call_all.
    template <class F>
    auto call_all(std::span<std::pair<unsigned, F>> calls);
    template <class F, class Res>
    auto call_all(std::span<std::pair<unsigned, F>> calls, std::vector<Res>& results);

    // ---- operations ----
    template <class Prep, class Take>
    class op_awaiter;

    auto nop(op_options o = {});
    auto sleep(std::chrono::nanoseconds d, op_options o = {});
    auto recv(file_ref f, owned_buffer buf, op_options o = {});
    auto send(file_ref f, owned_buffer buf, std::size_t offset = 0, op_options o = {});
    task<send_all_result> send_all(file_ref f, owned_buffer buf, op_options o = {});
    auto read(file_ref f, owned_buffer buf, op_options o = {});
    auto read_fixed(file_ref f, fixed_lease lease, std::size_t len, op_options o = {});
    // Writes lease bytes [offset, offset + len).
    auto write_fixed(file_ref f, fixed_lease lease, std::size_t len, op_options o = {},
                     std::size_t offset = 0);
    // Zero-copy send. The awaiter resumes at the first CQE with the byte count;
    // the buffer stays with the operation until the notification CQE and only
    // then returns to its pool (lease) or is freed (owned buffer).
    auto send_zc(file_ref f, fixed_lease lease, std::size_t len, op_options o = {});
    auto send_zc(file_ref f, owned_buffer buf, op_options o = {});
    // Multishot receive into provided buffers. With max_queued > 0 the reactor
    // cancels the receive once that many results wait unconsumed, so a busy
    // consumer cannot drain the shared buffer ring; the data stays in the
    // socket and a new receive picks it up.
    recv_stream recv_multishot(file_ref f, provided_buffer_ring& ring, op_options o = {},
                               std::uint32_t max_queued = 0);
    auto accept(int fd, op_options o = {});
    auto connect(int fd, const sockaddr* addr, socklen_t len, op_options o = {});
    // The descriptors in `pass` are duplicated into the operation, so closing
    // the originals while the send is in flight cannot change what is sent.
    auto sendmsg(file_ref f, owned_buffer payload, std::span<const int> pass, op_options o = {});
    auto recvmsg(file_ref f, owned_buffer payload, op_options o = {});

    // ---- internals used by awaiters, buffers and the runtime ----
    void schedule(detail::ready_node* n) noexcept { ready_.push_back(n); }
    detail::op_slot& slot_at(detail::slot_ref ref) noexcept;
    template <class Prep>
    detail::slot_ref start(std::coroutine_handle<> h, const op_options& o, detail::op_kind kind,
                           Prep& prep, file_ref file = {});
    void close_direct(unsigned index, std::uint32_t gen) noexcept;
    void finish_take(detail::slot_ref ref) noexcept;
    void detach(detail::slot_ref ref) noexcept;
    bool request_cancel(detail::slot_ref ref, cancel_source* reporter) noexcept;
    void forget_source(cancel_source* s) noexcept;
    void close_stream(detail::slot_ref ref) noexcept;
    // Builds a call node of type Node from `fn` in a new slot (in the slot's
    // own storage when it fits) and queues it for `target`, which the caller
    // has checked with check_call_target.
    template <class Node, class F>
    detail::slot_ref start_call(std::coroutine_handle<> h, unsigned target, F&& fn);
    void check_call_target(unsigned target) const; // what start_call would refuse
    void send_post(unsigned target, std::unique_ptr<detail::xnode> node);
    void on_spawn_done(detail::spawn_promise* p) noexcept;
    void record_task_error(std::exception_ptr e) noexcept;

private:
    friend class runtime;
    reactor(reactor_config cfg, runtime* rt, unsigned core);

    template <class Pred>
    void run_until(Pred pred);
    // One loop iteration: run ready tasks, submit, wait (if allowed), reap.
    void step(bool may_block, std::chrono::milliseconds wait_limit);
    void check_progress() const;
    void rethrow_task_error();
    void reap() noexcept;
    void dispatch(const io_uring_cqe& c) noexcept;
    void on_op_cqe(const io_uring_cqe& c) noexcept;
    void on_timeout_cqe(const io_uring_cqe& c) noexcept;
    void on_cancel_cqe(const io_uring_cqe& c) noexcept;
    void on_multishot(detail::op_slot& s, detail::slot_ref ref, const io_uring_cqe& c) noexcept;
    void complete(detail::op_slot& s, detail::slot_ref ref) noexcept;
    void maybe_free(detail::op_slot& s, detail::slot_ref ref) noexcept;
    void free_slot(detail::slot_ref ref) noexcept;
    void free_slot_at(detail::op_slot& s, std::uint32_t index) noexcept;
    void release_resources(detail::op_slot& s) noexcept;
    void launch_call(detail::op_slot& s, detail::slot_ref ref, std::coroutine_handle<> h,
                     unsigned target) noexcept;
    void unregister_source(detail::op_slot& s, detail::slot_ref ref) noexcept {
        if (s.source) remove_from_source(s, ref);
    }
    void remove_from_source(detail::op_slot& s, detail::slot_ref ref) noexcept;
    void capture_rights(detail::op_slot& s) noexcept;
    bool direct_live(const file_ref& f) const noexcept;
    void unpin_direct(unsigned index) noexcept;
    void release_direct_if_unused(unsigned index) noexcept;
    detail::slot_ref acquire_slot(detail::op_kind kind);
    void add_slots(); // one more chunk of free slots
    io_uring_sqe* reserve_sqes(unsigned n) {
        if (io_uring_sq_space_left(&ring_) >= n) return io_uring_get_sqe(&ring_);
        return reserve_sqes_slow(n);
    }
    io_uring_sqe* reserve_sqes_slow(unsigned n);
    io_uring_sqe* reserve_sqes_noexcept(unsigned n) noexcept;
    void enqueue_message(unsigned target, detail::xnode* n) noexcept;
    void flush_messages() noexcept;
    void flush_target(unsigned target) noexcept;
    void push_chain(unsigned target, detail::xnode* head) noexcept;
    void ring_doorbell(unsigned target) noexcept;
    int message_ring_fd(unsigned target, bool doorbell) const;
    bool inbound_pending() const noexcept;
    void drain_inbound() noexcept;
    void resend_refused() noexcept;
    void on_message_chain(detail::xnode* head) noexcept;
    void on_request(detail::xnode* n) noexcept;
    void on_reply(detail::xnode* n) noexcept;
    void on_chain_failed(detail::xnode* head, int res) noexcept;
    void check_thread() const noexcept {
        ROL_INVARIANT(std::this_thread::get_id() == thread_,
                      "reactor used from a thread other than the one that created its ring");
    }
    void cancel_all() noexcept;
    void destroy_tasks() noexcept;
    void begin_shutdown() noexcept;
    bool drain(std::chrono::steady_clock::time_point deadline);
    void close_ring() noexcept;
    template <class Pred>
    void serve_until(Pred pred);

    reactor_config cfg_;
    io_uring ring_{};
    unsigned setup_flags_ = 0;
    bool ring_open_ = false;
    bool closing_ = false;
    bool closed_ = false;
    int loop_depth_ = 0;
    std::thread::id thread_;
    runtime* rt_ = nullptr;
    unsigned core_ = 0;
    // Operation slots in chunks of 64 that never move (the kernel and other
    // cores hold their addresses); an index names one for its whole life.
    static constexpr unsigned slot_chunk_bits = 6;
    static constexpr std::uint32_t slot_chunk_mask = (1u << slot_chunk_bits) - 1;
    detail::op_slot& slot_entry(std::uint32_t index) noexcept {
        return slot_chunks_[index >> slot_chunk_bits][index & slot_chunk_mask];
    }
    std::vector<std::unique_ptr<detail::op_slot[]>> slot_chunks_;
    std::uint32_t slot_count_ = 0;
    std::vector<std::uint32_t> free_slots_; // capacity always >= slot_count_
    detail::ready_list ready_;
    detail::spawn_promise* spawned_ = nullptr;
    std::unique_ptr<fixed_buffer_pool> fixed_;
    std::vector<std::unique_ptr<provided_buffer_ring>> provided_;
    std::exception_ptr task_error_;
    struct outbox {
        detail::xnode* head = nullptr;
        detail::xnode* tail = nullptr;
    };
    std::vector<outbox> outbox_; // per target core
    unsigned outbox_pending_ = 0; // targets with a non-empty outbox
    // Refused MSG_RINGs waiting to be sent again: reply nodes (linked through
    // `next`) and doorbells per target. Resent no sooner than resend_at_.
    detail::xnode* resend_head_ = nullptr;
    std::vector<std::uint8_t> resend_doorbell_;
    bool resend_pending_ = false;
    std::chrono::steady_clock::time_point resend_at_{};
    struct direct_entry {
        std::uint32_t gen = 0;
        std::uint32_t pins = 0; // operations in flight that name this index
        direct_state state = direct_state::free;
        bool close_pending = false; // the IORING_OP_CLOSE has not completed
    };
    std::vector<direct_entry> direct_;
    std::vector<unsigned> direct_free_;
    reactor_stats stats_;
};

// ---- slot bookkeeping on the hot path ----

inline detail::op_slot& reactor::slot_at(detail::slot_ref ref) noexcept {
    ROL_INVARIANT(ref.index < slot_count_, "slot index out of range");
    auto& s = slot_entry(ref.index);
    ROL_INVARIANT(s.active && s.gen == ref.gen, "CQE or awaiter refers to a slot that is not in flight");
    return s;
}

inline detail::slot_ref reactor::acquire_slot(detail::op_kind kind) {
    if (free_slots_.empty()) add_slots();
    const std::uint32_t index = free_slots_.back();
    free_slots_.pop_back();
    auto& s = slot_entry(index);
    s.gen = (s.gen + 1) & detail::gen_mask;
    if (s.gen == 0) s.gen = 1;
    s.kind = kind;
    s.active = true;
    if (++stats_.in_flight > stats_.peak_in_flight) stats_.peak_in_flight = stats_.in_flight;
    return {index, s.gen};
}

inline void reactor::maybe_free(detail::op_slot& s, detail::slot_ref ref) noexcept {
    if (s.pending != 0 || !(s.detached || s.taken)) return;
    if (s.detached) ++stats_.detached_released;
    free_slot_at(s, ref.index);
}

inline void reactor::finish_take(detail::slot_ref ref) noexcept {
    auto& s = slot_at(ref);
    s.taken = true;
    s.waiter = {};
    maybe_free(s, ref);
}

// ---- the awaitable for single-result operations ----

template <class Prep, class Take>
class reactor::op_awaiter {
public:
    op_awaiter(reactor& r, op_options o, detail::op_kind kind, Prep prep, Take take,
               file_ref file = {})
        : r_(&r), opts_(o), kind_(kind), prep_(std::move(prep)), take_(std::move(take)),
          file_(file) {}
    op_awaiter(const op_awaiter&) = delete;
    op_awaiter& operator=(const op_awaiter&) = delete;
    ~op_awaiter() {
        // Destroyed while the operation is still in flight (the coroutine frame
        // was destroyed): the slot keeps every resource until the kernel's CQE.
        if (ref_) r_->detach(ref_);
    }
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        ref_ = r_->start(h, opts_, kind_, prep_, file_);
        return !r_->slot_at(ref_).primary_done;
    }
    auto await_resume() {
        auto& s = r_->slot_at(ref_);
        auto out = take_(s);
        auto ref = std::exchange(ref_, {});
        r_->finish_take(ref);
        return out;
    }

private:
    reactor* r_;
    op_options opts_;
    detail::op_kind kind_;
    Prep prep_;
    Take take_;
    file_ref file_;
    detail::slot_ref ref_{};
};

// Multishot receive. Owns its slot; completions that arrive while nobody is
// waiting queue in the slot. Destroying the stream cancels the operation; the
// slot returns any buffers the kernel still hands out until the final CQE.
class recv_stream {
public:
    recv_stream(recv_stream&& o) noexcept : r_(o.r_), ref_(std::exchange(o.ref_, {})) {}
    recv_stream(const recv_stream&) = delete;
    recv_stream& operator=(const recv_stream&) = delete;
    recv_stream& operator=(recv_stream&&) = delete;
    ~recv_stream() {
        if (ref_) r_->close_stream(ref_);
    }

    class next_awaiter {
    public:
        explicit next_awaiter(recv_stream& s) noexcept : s_(&s) {}
        next_awaiter(const next_awaiter&) = delete;
        next_awaiter& operator=(const next_awaiter&) = delete;
        ~next_awaiter() {
            if (waiting_ && s_->ref_) s_->r_->slot_at(s_->ref_).waiter = {};
        }
        bool await_ready() const noexcept {
            if (!s_->ref_) return true;
            auto& slot = s_->r_->slot_at(s_->ref_);
            return !slot.queue.empty() || slot.ended;
        }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            waiting_ = true;
            s_->r_->slot_at(s_->ref_).waiter = h;
        }
        std::optional<recv_item> await_resume() {
            waiting_ = false;
            return s_->pop();
        }

    private:
        recv_stream* s_;
        bool waiting_ = false;
    };
    // nullopt once the final completion has been consumed.
    next_awaiter next() noexcept { return next_awaiter(*this); }
    bool ended() const noexcept { return !ref_ || r_->slot_at(ref_).ended; }

private:
    friend class reactor;
    recv_stream(reactor& r, detail::slot_ref ref) noexcept : r_(&r), ref_(ref) {}
    std::optional<recv_item> pop();
    reactor* r_;
    detail::slot_ref ref_;
};

// ---- template implementations ----

namespace detail {
inline __kernel_timespec to_timespec(std::chrono::nanoseconds d) noexcept {
    __kernel_timespec ts{};
    ts.tv_sec = static_cast<long long>(d.count() / 1000000000);
    ts.tv_nsec = static_cast<long long>(d.count() % 1000000000);
    return ts;
}
} // namespace detail

template <class Prep>
detail::slot_ref reactor::start(std::coroutine_handle<> h, const op_options& o,
                                detail::op_kind kind, Prep& prep, file_ref file) {
    check_thread();
    const bool stale = file.fixed && !direct_live(file);
    const bool short_circuit = closing_ || (o.cancel && o.cancel->requested()) || stale;
    const bool linked = o.timeout.count() > 0;
    // Reserve everything that can fail before any resource moves into the slot.
    auto ref = acquire_slot(kind);
    io_uring_sqe* sqe = nullptr;
    if (!short_circuit) {
        try {
            sqe = reserve_sqes(linked ? 2 : 1);
        } catch (...) {
            free_slot(ref);
            throw;
        }
    }
    auto& s = slot_at(ref);
    prep(s, sqe);
    ++stats_.ops_started;
    if (short_circuit) {
        s.res = stale ? -EBADF : -ECANCELED;
        s.primary_done = true;
        s.ended = true;
        if (kind == detail::op_kind::multishot) s.queue.push_back({s.res, 0});
        ++stats_.ops_short_circuited;
        return ref;
    }
    io_uring_sqe_set_data64(sqe, detail::encode(detail::ud_tag::op, ref));
    if (file.fixed) {
        // The index stays reserved until this operation's last CQE.
        sqe->flags |= IOSQE_FIXED_FILE;
        ++direct_[static_cast<unsigned>(file.fd)].pins;
        s.pinned_file = file.fd;
    }
    s.pending = 1;
    if (linked) {
        sqe->flags |= IOSQE_IO_LINK;
        s.ts = detail::to_timespec(o.timeout);
        io_uring_sqe* t = io_uring_get_sqe(&ring_);
        io_uring_prep_link_timeout(t, &s.ts, 0);
        io_uring_sqe_set_data64(t, detail::encode(detail::ud_tag::linked_timeout, ref));
        s.timeout_pending = true;
        s.pending = 2;
        ++stats_.sqes;
    }
    ++stats_.sqes;
    s.waiter = h;
    if (o.cancel) {
        s.source = o.cancel;
        o.cancel->ops_.push_back(ref);
    }
    return ref;
}

template <class Pred>
void reactor::run_until(Pred pred) {
    check_thread();
    ROL_INVARIANT(loop_depth_ == 0, "block_on called from inside a running reactor");
    ++loop_depth_;
    struct depth_guard {
        int& d;
        ~depth_guard() { --d; }
    } guard{loop_depth_};
    while (!pred()) {
        rethrow_task_error();
        if (!ready_.empty()) {
            step(false, std::chrono::milliseconds(0));
            continue;
        }
        if (!rt_) check_progress();
        step(true, std::chrono::milliseconds(0));
    }
    rethrow_task_error();
}

template <class Pred>
void reactor::serve_until(Pred pred) {
    ++loop_depth_;
    struct depth_guard {
        int& d;
        ~depth_guard() { --d; }
    } guard{loop_depth_};
    while (!pred()) step(ready_.empty(), std::chrono::milliseconds(1));
}

template <class T>
T reactor::block_on(task<T> t) {
    if constexpr (std::is_void_v<T>) {
        auto root = detail::root_body_void(std::move(t));
        root.h.promise().node.handle = root.h;
        schedule(&root.h.promise().node);
        run_until([&] { return root.h.done(); });
        if (root.h.promise().error) std::rethrow_exception(root.h.promise().error);
    } else {
        auto root = detail::root_body<T>(std::move(t));
        root.h.promise().node.handle = root.h;
        schedule(&root.h.promise().node);
        run_until([&] { return root.h.done(); });
        if (root.h.promise().error) std::rethrow_exception(root.h.promise().error);
        return std::move(*root.h.promise().value);
    }
}

inline auto reactor::nop(op_options o) {
    return op_awaiter(
        *this, o, detail::op_kind::generic,
        [](detail::op_slot&, io_uring_sqe* sqe) {
            if (sqe) io_uring_prep_nop(sqe);
        },
        [](detail::op_slot& s) { return io_result{s.res, s.flags}; });
}

inline auto reactor::sleep(std::chrono::nanoseconds d, op_options o) {
    return op_awaiter(
        *this, o, detail::op_kind::generic,
        [d](detail::op_slot& s, io_uring_sqe* sqe) {
            s.ts = detail::to_timespec(d);
            if (sqe) io_uring_prep_timeout(sqe, &s.ts, 0, 0);
        },
        [](detail::op_slot& s) { return io_result{s.res, s.flags}; });
}

inline auto reactor::recv(file_ref f, owned_buffer buf, op_options o) {
    const int fd = f.fd;
    return op_awaiter(
        *this, o, detail::op_kind::generic,
        [fd, buf = std::move(buf)](detail::op_slot& s, io_uring_sqe* sqe) mutable {
            s.owned = std::move(buf);
            if (sqe) {
                auto w = s.owned.storage();
                io_uring_prep_recv(sqe, fd, w.data(), w.size(), 0);
            }
        },
        [](detail::op_slot& s) {
            if (s.res >= 0) s.owned.resize(static_cast<std::size_t>(s.res));
            return buffer_result{s.res, std::move(s.owned)};
        },
        f);
}

inline auto reactor::send(file_ref f, owned_buffer buf, std::size_t offset, op_options o) {
    const int fd = f.fd;
    if (offset > buf.size()) throw Failure(Error::invalid, "send offset beyond buffer size");
    return op_awaiter(
        *this, o, detail::op_kind::generic,
        [fd, offset, buf = std::move(buf)](detail::op_slot& s, io_uring_sqe* sqe) mutable {
            s.owned = std::move(buf);
            if (sqe) {
                auto b = s.owned.bytes().subspan(offset);
                io_uring_prep_send(sqe, fd, b.data(), b.size(), MSG_NOSIGNAL);
            }
        },
        [](detail::op_slot& s) { return buffer_result{s.res, std::move(s.owned)}; },
        f);
}

inline auto reactor::read(file_ref f, owned_buffer buf, op_options o) {
    const int fd = f.fd;
    return op_awaiter(
        *this, o, detail::op_kind::generic,
        [fd, buf = std::move(buf)](detail::op_slot& s, io_uring_sqe* sqe) mutable {
            s.owned = std::move(buf);
            if (sqe) {
                auto w = s.owned.storage();
                io_uring_prep_read(sqe, fd, w.data(), static_cast<unsigned>(w.size()),
                                   static_cast<__u64>(-1));
            }
        },
        [](detail::op_slot& s) {
            if (s.res >= 0) s.owned.resize(static_cast<std::size_t>(s.res));
            return buffer_result{s.res, std::move(s.owned)};
        },
        f);
}

inline auto reactor::read_fixed(file_ref f, fixed_lease lease, std::size_t len, op_options o) {
    const int fd = f.fd;
    if (fixed_) fixed_->check_lease(lease);
    else throw Failure(Error::invalid, "reactor has no fixed buffers");
    if (len > lease.size()) throw Failure(Error::invalid, "read length exceeds the buffer");
    return op_awaiter(
        *this, o, detail::op_kind::generic,
        [fd, len, lease = std::move(lease)](detail::op_slot& s, io_uring_sqe* sqe) mutable {
            s.lease = std::move(lease);
            s.lease.pool()->begin_io(s.lease);
            if (sqe) {
                io_uring_prep_read_fixed(sqe, fd, s.lease.data().data(),
                                         static_cast<unsigned>(len), 0,
                                         static_cast<int>(s.lease.index()));
                // Sockets ignore the offset; -1 means "current position" for files.
                sqe->off = static_cast<__u64>(-1);
            }
        },
        [](detail::op_slot& s) {
            if (s.lease) s.lease.pool()->end_io(s.lease);
            return lease_result{s.res, std::move(s.lease)};
        },
        f);
}

inline auto reactor::write_fixed(file_ref f, fixed_lease lease, std::size_t len, op_options o,
                                 std::size_t offset) {
    const int fd = f.fd;
    if (fixed_) fixed_->check_lease(lease);
    else throw Failure(Error::invalid, "reactor has no fixed buffers");
    if (offset > lease.size() || len > lease.size() - offset)
        throw Failure(Error::invalid, "write range exceeds the buffer");
    return op_awaiter(
        *this, o, detail::op_kind::generic,
        [fd, len, offset, lease = std::move(lease)](detail::op_slot& s,
                                                    io_uring_sqe* sqe) mutable {
            s.lease = std::move(lease);
            s.lease.pool()->begin_io(s.lease);
            if (sqe) {
                io_uring_prep_write_fixed(sqe, fd, s.lease.data().data() + offset,
                                          static_cast<unsigned>(len), 0,
                                          static_cast<int>(s.lease.index()));
                sqe->off = static_cast<__u64>(-1);
            }
        },
        [](detail::op_slot& s) {
            if (s.lease) s.lease.pool()->end_io(s.lease);
            return lease_result{s.res, std::move(s.lease)};
        },
        f);
}

inline auto reactor::send_zc(file_ref f, fixed_lease lease, std::size_t len, op_options o) {
    const int fd = f.fd;
    if (fixed_) fixed_->check_lease(lease);
    else throw Failure(Error::invalid, "reactor has no fixed buffers");
    if (len > lease.size()) throw Failure(Error::invalid, "send length exceeds the buffer");
    return op_awaiter(
        *this, o, detail::op_kind::send_zc,
        [fd, len, lease = std::move(lease)](detail::op_slot& s, io_uring_sqe* sqe) mutable {
            s.lease = std::move(lease);
            s.lease.pool()->begin_io(s.lease);
            if (sqe) {
                io_uring_prep_send_zc_fixed(sqe, fd, s.lease.data().data(), len, MSG_NOSIGNAL,
                                            IORING_SEND_ZC_REPORT_USAGE, s.lease.index());
            }
        },
        // The lease is not handed back: it stays in the slot until the
        // notification CQE, then returns to the pool.
        [](detail::op_slot& s) { return io_result{s.res, s.flags}; },
        f);
}

inline auto reactor::send_zc(file_ref f, owned_buffer buf, op_options o) {
    const int fd = f.fd;
    return op_awaiter(
        *this, o, detail::op_kind::send_zc,
        [fd, buf = std::move(buf)](detail::op_slot& s, io_uring_sqe* sqe) mutable {
            s.owned = std::move(buf);
            if (sqe) {
                auto b = s.owned.bytes();
                io_uring_prep_send_zc(sqe, fd, b.data(), b.size(), MSG_NOSIGNAL,
                                      IORING_SEND_ZC_REPORT_USAGE);
            }
        },
        [](detail::op_slot& s) { return io_result{s.res, s.flags}; },
        f);
}

inline auto reactor::accept(int fd, op_options o) {
    return op_awaiter(
        *this, o, detail::op_kind::generic,
        [fd](detail::op_slot&, io_uring_sqe* sqe) {
            if (sqe) io_uring_prep_accept(sqe, fd, nullptr, nullptr, SOCK_CLOEXEC);
        },
        [](detail::op_slot& s) { return io_result{s.res, s.flags}; });
}

inline auto reactor::connect(int fd, const sockaddr* addr, socklen_t len, op_options o) {
    if (len > sizeof(sockaddr_storage)) throw Failure(Error::invalid, "address too long");
    sockaddr_storage copy{};
    std::memcpy(&copy, addr, len);
    return op_awaiter(
        *this, o, detail::op_kind::generic,
        [fd, copy, len](detail::op_slot& s, io_uring_sqe* sqe) {
            if (!s.msg) s.msg = std::make_unique<detail::msg_storage>();
            s.msg->addr = copy;
            s.msg->addrlen = len;
            if (sqe)
                io_uring_prep_connect(sqe, fd, reinterpret_cast<sockaddr*>(&s.msg->addr),
                                      s.msg->addrlen);
        },
        [](detail::op_slot& s) { return io_result{s.res, s.flags}; });
}

inline auto reactor::sendmsg(file_ref f, owned_buffer payload, std::span<const int> pass,
                             op_options o) {
    const int fd = f.fd;
    if (pass.size() > 8) throw Failure(Error::invalid, "at most 8 descriptors per message");
    std::vector<unique_fd> dups;
    dups.reserve(pass.size());
    for (int p : pass) {
        int d = ::fcntl(p, F_DUPFD_CLOEXEC, 0);
        if (d < 0) throw Failure(Error::invalid, "cannot duplicate descriptor to pass", errno);
        dups.emplace_back(d);
    }
    return op_awaiter(
        *this, o, detail::op_kind::generic,
        [fd, payload = std::move(payload), dups = std::move(dups)](detail::op_slot& s,
                                                                   io_uring_sqe* sqe) mutable {
            s.owned = std::move(payload);
            s.fds = std::move(dups);
            if (!s.msg) s.msg = std::make_unique<detail::msg_storage>();
            auto& m = *s.msg;
            m = detail::msg_storage{};
            m.iov.iov_base = s.owned.storage().data();
            m.iov.iov_len = s.owned.size();
            m.msg.msg_iov = &m.iov;
            m.msg.msg_iovlen = 1;
            if (!s.fds.empty()) {
                const auto bytes = s.fds.size() * sizeof(int);
                m.msg.msg_control = m.control;
                m.msg.msg_controllen = CMSG_SPACE(bytes);
                cmsghdr* c = CMSG_FIRSTHDR(&m.msg);
                c->cmsg_level = SOL_SOCKET;
                c->cmsg_type = SCM_RIGHTS;
                c->cmsg_len = CMSG_LEN(bytes);
                for (std::size_t i = 0; i < s.fds.size(); ++i) {
                    int v = s.fds[i].get();
                    std::memcpy(CMSG_DATA(c) + i * sizeof(int), &v, sizeof(int));
                }
            }
            if (sqe) io_uring_prep_sendmsg(sqe, fd, &m.msg, MSG_NOSIGNAL);
        },
        [](detail::op_slot& s) { return buffer_result{s.res, std::move(s.owned)}; },
        f);
}

inline auto reactor::recvmsg(file_ref f, owned_buffer payload, op_options o) {
    const int fd = f.fd;
    return op_awaiter(
        *this, o, detail::op_kind::recvmsg,
        [fd, payload = std::move(payload)](detail::op_slot& s, io_uring_sqe* sqe) mutable {
            s.owned = std::move(payload);
            if (!s.msg) s.msg = std::make_unique<detail::msg_storage>();
            auto& m = *s.msg;
            m = detail::msg_storage{};
            auto w = s.owned.storage();
            m.iov.iov_base = w.data();
            m.iov.iov_len = w.size();
            m.msg.msg_iov = &m.iov;
            m.msg.msg_iovlen = 1;
            m.msg.msg_control = m.control;
            m.msg.msg_controllen = sizeof(m.control);
            if (sqe) io_uring_prep_recvmsg(sqe, fd, &m.msg, MSG_CMSG_CLOEXEC);
        },
        [](detail::op_slot& s) {
            recvmsg_result out;
            out.res = s.res;
            if (s.res >= 0) s.owned.resize(static_cast<std::size_t>(s.res));
            out.payload = std::move(s.owned);
            if (s.msg && s.res >= 0) {
                out.msg_flags = s.msg->msg.msg_flags;
                const auto n = std::min<std::size_t>(s.msg->msg.msg_controllen,
                                                     sizeof(s.msg->control));
                out.control = owned_buffer::copy_of(
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.msg->control), n));
            }
            out.fds = std::move(s.fds);
            return out;
        },
        f);
}

template <class F>
void reactor::post(unsigned target, F fn) {
    send_post(target, std::make_unique<detail::post_node<F>>(std::move(fn)));
}

template <class F, class R>
class call_awaiter {
public:
    call_awaiter(reactor& r, unsigned target, F fn)
        : r_(&r), target_(target), fn_(std::move(fn)) {}
    call_awaiter(const call_awaiter&) = delete;
    call_awaiter& operator=(const call_awaiter&) = delete;
    ~call_awaiter() {
        // The target core may still write the result: the slot keeps the node
        // until the reply arrives.
        if (ref_) r_->detach(ref_);
    }
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) {
        r_->check_call_target(target_);
        ref_ = r_->template start_call<detail::call_node<F, R>>(h, target_, std::move(fn_));
        return !r_->slot_at(ref_).primary_done;
    }
    R await_resume() {
        auto& s = r_->slot_at(ref_);
        auto ref = std::exchange(ref_, {});
        struct release {
            reactor* r;
            detail::slot_ref ref;
            ~release() { r->finish_take(ref); }
        } guard{r_, ref};
        if (s.res < 0) throw Failure(Error::closed, "cross-core call could not be delivered", -s.res);
        auto* node = static_cast<detail::call_node<F, R>*>(s.xcall);
        ROL_INVARIANT(node->stage.load(std::memory_order_acquire) == 2,
                      "cross-core reply arrived before the result was published");
        if (node->error) std::rethrow_exception(node->error);
        if constexpr (std::is_void_v<R>) return;
        else return std::move(*node->result);
    }

private:
    reactor* r_;
    unsigned target_;
    F fn_;
    detail::slot_ref ref_{};
};

template <class F>
auto reactor::call(unsigned target, F fn) {
    using R = std::invoke_result_t<F&, reactor&>;
    return call_awaiter<F, R>(*this, target, std::move(fn));
}

// Into: the results go into a vector the caller owns instead of a new one.
template <class F, class R, bool Into>
class call_all_awaiter {
    using Res = std::conditional_t<std::is_void_v<R>, detail::unit, R>;

public:
    call_all_awaiter(reactor& r, std::span<std::pair<unsigned, F>> calls,
                     std::vector<Res>* into = nullptr)
        : r_(&r), calls_(calls), into_(into) {}
    call_all_awaiter(const call_all_awaiter&) = delete;
    call_all_awaiter& operator=(const call_all_awaiter&) = delete;
    ~call_all_awaiter() {
        // Destroyed with replies outstanding: each slot keeps its node until
        // its reply arrives, and no longer counts down `join_`.
        for (std::size_t i = 0; i < count_; ++i)
            if (refs_[i]) r_->detach(refs_[i]);
    }
    bool await_ready() const noexcept { return calls_.empty(); }
    bool await_suspend(std::coroutine_handle<> h) {
        for (auto& c : calls_) r_->check_call_target(c.first); // throws before anything starts
        join_.parent = h;
        join_.remaining = calls_.size();
        if (calls_.size() > inline_refs) {
            big_.resize(calls_.size());
            refs_ = big_.data();
        }
        for (auto& c : calls_) {
            const auto ref = r_->template start_call<detail::call_node<F, R>>({}, c.first,
                                                                            std::move(c.second));
            r_->slot_at(ref).join = &join_;
            refs_[count_++] = ref;
        }
        return true; // a reply always arrives in a later loop iteration
    }
    auto await_resume() {
        if constexpr (Into) {
            into_->clear();
            collect(*into_);
        } else {
            std::vector<Res> out;
            out.reserve(count_);
            collect(out);
            if constexpr (!std::is_void_v<R>) return out;
        }
    }

private:
    void collect(std::vector<Res>& out) {
        int failure = 0;
        std::exception_ptr error;
        for (std::size_t i = 0; i < count_; ++i) {
            auto& ref = refs_[i];
            auto& s = r_->slot_at(ref);
            ROL_INVARIANT(s.primary_done, "call_all resumed before every reply arrived");
            auto* node = static_cast<detail::call_node<F, R>*>(s.xcall);
            if (s.res < 0) {
                if (!failure && !error) failure = s.res;
            } else {
                ROL_INVARIANT(node->stage.load(std::memory_order_acquire) == 2,
                              "cross-core reply arrived before the result was published");
                if (node->error) {
                    if (!failure && !error) error = node->error;
                } else if (!failure && !error) {
                    out.push_back(std::move(*node->result));
                }
            }
            r_->finish_take(std::exchange(ref, {}));
        }
        count_ = 0;
        if (failure) throw Failure(Error::closed, "cross-core call could not be delivered", -failure);
        if (error) std::rethrow_exception(error);
    }

    static constexpr std::size_t inline_refs = 8;
    reactor* r_;
    std::span<std::pair<unsigned, F>> calls_;
    std::vector<Res>* into_;
    detail::slot_ref small_[inline_refs]{};
    detail::slot_ref* refs_ = small_;
    std::size_t count_ = 0; // slots started and not yet taken
    std::vector<detail::slot_ref> big_; // more than inline_refs calls
    detail::call_join join_;
};

template <class F>
auto reactor::call_all(std::span<std::pair<unsigned, F>> calls) {
    using R = std::invoke_result_t<F&, reactor&>;
    return call_all_awaiter<F, R, false>(*this, calls);
}

template <class F, class Res>
auto reactor::call_all(std::span<std::pair<unsigned, F>> calls, std::vector<Res>& results) {
    using R = std::invoke_result_t<F&, reactor&>;
    static_assert(std::is_same_v<Res, std::conditional_t<std::is_void_v<R>, detail::unit, R>>,
                  "results must hold what the functions return");
    return call_all_awaiter<F, R, true>(*this, calls, &results);
}

template <class Node, class F>
detail::slot_ref reactor::start_call(std::coroutine_handle<> h, unsigned target, F&& fn) {
    check_thread();
    const auto ref = acquire_slot(detail::op_kind::xcall);
    auto& s = slot_at(ref);
    try {
        if constexpr (sizeof(Node) <= detail::op_slot::node_space_size &&
                      alignof(Node) <= alignof(std::max_align_t)) {
            s.xcall = ::new (static_cast<void*>(s.node_space)) Node(std::forward<F>(fn));
            s.xcall_inline = true;
        } else {
            s.xcall = new Node(std::forward<F>(fn));
            ++stats_.call_nodes_on_heap;
        }
    } catch (...) {
        free_slot(ref);
        throw;
    }
    launch_call(s, ref, h, target);
    return ref;
}

template <class F>
void detail::post_node<F>::run(reactor& target) noexcept {
    try {
        fn(target);
    } catch (...) {
        target.record_task_error(std::current_exception());
    }
}

} // namespace rol
