#include "rol/reactor.hpp"
#include "rol/detail/cmsg.hpp"
#include "rol/runtime.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>

namespace rol {

using detail::op_kind;
using detail::op_slot;
using detail::slot_ref;
using detail::ud_tag;

void invariant_failure(const char* expr, const char* file, int line, const char* message) noexcept {
    std::fprintf(stderr, "rol invariant violated: %s [%s] at %s:%d\n", message, expr, file, line);
    std::fflush(stderr);
    std::abort();
}

namespace {
constexpr std::uint64_t pointer_mask = (std::uint64_t(1) << 56) - 1;

// Memory the kernel may still touch after a shutdown gave up waiting. It is
// kept reachable for the life of the process instead of being freed.
std::mutex grave_mutex;
void park(std::shared_ptr<void> p) {
    static auto* graves = new std::vector<std::shared_ptr<void>>();
    std::lock_guard lock(grave_mutex);
    graves->push_back(std::move(p));
}

std::uint64_t tagged_pointer(ud_tag tag, const void* p) {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    ROL_INVARIANT((v & ~pointer_mask) == 0, "pointer does not fit in 56 bits of user_data");
    return (std::uint64_t(tag) << 56) | v;
}
std::uint64_t tagged_value(ud_tag tag, std::uint64_t v) { return (std::uint64_t(tag) << 56) | v; }
} // namespace

// ---------------------------------------------------------------- lifecycle

reactor::reactor(reactor_config cfg) : reactor(cfg, nullptr, 0) {}

reactor::reactor(reactor_config cfg, runtime* rt, unsigned core)
    : cfg_(cfg), thread_(std::this_thread::get_id()), rt_(rt), core_(core) {
    if (cfg.sq_entries == 0 || cfg.sq_entries > 32768)
        throw Failure(Error::invalid, "sq_entries must be 1..32768");
    io_uring_params p{};
    p.flags = cfg.setup_flags | IORING_SETUP_CQSIZE;
    p.cq_entries = cfg.cq_entries ? cfg.cq_entries : cfg.sq_entries * 4;
    const int rc = io_uring_queue_init_params(cfg.sq_entries, &ring_, &p);
    if (rc < 0) throw Failure(Error::system, "io_uring_queue_init_params failed", -rc);
    ring_open_ = true;
    setup_flags_ = p.flags;
    if (rt_) {
        outbox_.resize(rt_->size());
        resend_doorbell_.assign(rt_->size(), 0);
    }
    if (!(p.features & IORING_FEAT_NODROP)) {
        io_uring_queue_exit(&ring_);
        throw Failure(Error::system, "kernel lacks IORING_FEAT_NODROP");
    }
    if (cfg.fixed_buffers) {
        try {
            fixed_.reset(new fixed_buffer_pool(*this, &ring_, cfg.fixed_buffers,
                                               cfg.fixed_buffer_size));
        } catch (...) {
            io_uring_queue_exit(&ring_);
            throw;
        }
    }
    if (cfg.direct_files) {
        const int r = io_uring_register_files_sparse(&ring_, cfg.direct_files);
        if (r < 0) {
            fixed_.reset();
            io_uring_queue_exit(&ring_);
            throw Failure(Error::system, "io_uring_register_files_sparse failed", -r);
        }
        direct_.resize(cfg.direct_files);
        for (unsigned i = cfg.direct_files; i > 0; --i) direct_free_.push_back(i - 1);
    }
}

reactor::~reactor() { shutdown(); }

void reactor::check_thread() const noexcept {
    ROL_INVARIANT(std::this_thread::get_id() == thread_,
                  "reactor used from a thread other than the one that created its ring");
}

unsigned reactor::cores() const noexcept { return rt_ ? rt_->size() : 1; }

fixed_buffer_pool& reactor::fixed_buffers() {
    if (!fixed_) throw Failure(Error::invalid, "reactor was created without fixed buffers");
    return *fixed_;
}

provided_buffer_ring& reactor::add_provided_ring(std::uint16_t group, unsigned entries,
                                                 std::size_t buffer_size) {
    check_thread();
    if (closing_) throw Failure(Error::closed, "reactor is shutting down");
    for (auto& r : provided_)
        if (r->group() == group) throw Failure(Error::invalid, "buffer group already registered");
    provided_.emplace_back(new provided_buffer_ring(&ring_, group, entries, buffer_size));
    return *provided_.back();
}

// ---------------------------------------------------------------- direct descriptors

direct_fd reactor::adopt_direct(unique_fd&& fd) {
    check_thread();
    if (direct_.empty()) throw Failure(Error::invalid, "reactor has no direct descriptor table");
    if (closing_) throw Failure(Error::closed, "reactor is shutting down");
    if (direct_free_.empty()) throw Failure(Error::full, "every direct descriptor is in use");
    const unsigned index = direct_free_.back();
    int raw = fd.get();
    const int r = io_uring_register_files_update(&ring_, index, &raw, 1);
    if (r < 0) throw Failure(Error::system, "io_uring_register_files_update failed", -r);
    direct_free_.pop_back();
    fd.reset(); // the table holds the file now
    auto& e = direct_[index];
    e.gen = e.gen + 1 == 0 ? 1 : e.gen + 1;
    e.state = direct_state::open;
    return direct_fd(this, index, e.gen);
}

reactor::direct_state reactor::direct_state_of(unsigned index) const {
    if (index >= direct_.size()) throw Failure(Error::invalid, "no such direct descriptor");
    return direct_[index].state;
}

std::uint32_t reactor::direct_pins(unsigned index) const {
    if (index >= direct_.size()) throw Failure(Error::invalid, "no such direct descriptor");
    return direct_[index].pins;
}

bool reactor::direct_live(const file_ref& f) const noexcept {
    if (f.fd < 0 || static_cast<std::size_t>(f.fd) >= direct_.size()) return false;
    const auto& e = direct_[static_cast<unsigned>(f.fd)];
    return e.state == direct_state::open && e.gen == f.gen;
}

void direct_fd::reset() noexcept {
    if (r_) std::exchange(r_, nullptr)->close_direct(index_, gen_);
}

void reactor::close_direct(unsigned index, std::uint32_t gen) noexcept {
    auto& e = direct_[index];
    if (e.state != direct_state::open || e.gen != gen) return;
    e.state = direct_state::closing; // references with this generation are refused from now on
    if (!ring_open_) return;
    // Remove the file from the table. Operations already submitted keep their
    // own reference to it and finish normally.
    slot_ref ref;
    try {
        ref = acquire_slot(op_kind::generic);
    } catch (...) {
        ROL_INVARIANT(false, "out of memory while allocating a close slot");
    }
    auto& s = slots_[ref.index];
    s.detached = true;
    s.pending = 1;
    s.closes_file = static_cast<std::int32_t>(index);
    e.close_pending = true;
    io_uring_sqe* sqe = reserve_sqes_noexcept(1);
    io_uring_prep_close_direct(sqe, index);
    io_uring_sqe_set_data64(sqe, detail::encode(ud_tag::op, ref));
    ++stats_.ops_started;
    ++stats_.sqes;
}

void reactor::unpin_direct(unsigned index) noexcept {
    auto& e = direct_[index];
    ROL_INVARIANT(e.pins > 0, "direct descriptor unpinned more often than pinned");
    --e.pins;
    release_direct_if_unused(index);
}

void reactor::release_direct_if_unused(unsigned index) noexcept {
    auto& e = direct_[index];
    if (e.state != direct_state::closing || e.close_pending || e.pins > 0) return;
    e.state = direct_state::free;
    direct_free_.push_back(index);
}

// ---------------------------------------------------------------- tasks

detail::spawn_coro detail::spawn_body(task<void> t) { co_await std::move(t); }

void detail::spawn_promise::final_awaiter::await_suspend(
    std::coroutine_handle<spawn_promise> h) const noexcept {
    h.promise().r->on_spawn_done(&h.promise());
    h.destroy();
}

void detail::spawn_promise::unhandled_exception() noexcept {
    r->record_task_error(std::current_exception());
}

void reactor::spawn(task<void> t) {
    check_thread();
    if (closing_) throw Failure(Error::closed, "reactor is shutting down");
    if (!t.valid()) throw Failure(Error::invalid, "spawn of an empty task");
    auto c = detail::spawn_body(std::move(t));
    auto& p = c.h.promise();
    p.r = this;
    p.node.handle = c.h;
    p.next = spawned_;
    if (spawned_) spawned_->prev = &p;
    spawned_ = &p;
    ++stats_.tasks;
    schedule(&p.node);
}

void reactor::on_spawn_done(detail::spawn_promise* p) noexcept {
    if (p->prev) p->prev->next = p->next;
    else spawned_ = p->next;
    if (p->next) p->next->prev = p->prev;
    p->prev = p->next = nullptr;
    --stats_.tasks;
}

void reactor::record_task_error(std::exception_ptr e) noexcept {
    if (!task_error_) task_error_ = e;
}

void reactor::rethrow_task_error() {
    if (task_error_) std::rethrow_exception(std::exchange(task_error_, nullptr));
}

void reactor::check_progress() const {
    if (ready_.empty() && stats_.in_flight == 0)
        throw Failure(Error::invalid,
                      "block_on: the root task is waiting, but no operation is in flight and no "
                      "task is ready");
}

// ---------------------------------------------------------------- loop

void reactor::step(bool may_block, std::chrono::milliseconds wait_limit) {
    for (int budget = 256; budget > 0 && !ready_.empty(); --budget) {
        detail::ready_node* n = ready_.pop_front();
        n->handle.resume();
    }
    if (!ready_.empty() || cfg_.busy_poll) may_block = false;
    if (resend_pending_) {
        resend_refused();
        // Do not sleep past the next resend.
        if (resend_pending_ && (wait_limit.count() == 0 || wait_limit > std::chrono::milliseconds(1)))
            wait_limit = std::chrono::milliseconds(1);
    }
    flush_messages();
    // Doorbell transport: announce the sleep, then look at the queues once
    // more. A sender publishes its chain and then reads the announcement; this
    // core announces and then reads the queues. All four accesses are
    // sequentially consistent, so at least one side sees the other: either the
    // sender rings the doorbell, or this core finds the chain and stays awake.
    bool announced = false;
    if (may_block && cfg_.doorbell && rt_) {
        // Submit this iteration's work first. Its sends can keep the kernel
        // busy for tens of microseconds; announcing before them made senders
        // ring doorbells that waited behind those sends (design-tradeoffs.md).
        if (cfg_.submit_before_announce && io_uring_sq_ready(&ring_) > 0) {
            ++stats_.enters;
            const int s = io_uring_submit(&ring_);
            if (s < 0 && s != -EINTR && s != -EBUSY && s != -EAGAIN)
                throw Failure(Error::system, "io_uring_enter failed", -s);
        }
        auto& flag = rt_->sleeping(core_);
        flag.store(true, std::memory_order_seq_cst);
        if (inbound_pending()) {
            flag.store(false, std::memory_order_seq_cst);
            may_block = false;
            ++stats_.wait_aborts;
        } else {
            announced = true;
        }
    }
    ++stats_.enters;
    if (may_block) ++stats_.blocking_enters;
    int rc;
    if (may_block && wait_limit.count() > 0) {
        __kernel_timespec ts = detail::to_timespec(wait_limit);
        io_uring_cqe* cqe = nullptr;
        rc = io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, &ts, nullptr);
    } else if (may_block) {
        rc = io_uring_submit_and_wait(&ring_, 1);
    } else {
        rc = io_uring_submit_and_get_events(&ring_);
    }
    if (announced) rt_->sleeping(core_).store(false, std::memory_order_seq_cst);
    if (rc < 0 && rc != -EINTR && rc != -ETIME && rc != -EBUSY && rc != -EAGAIN)
        throw Failure(Error::system, "io_uring_enter failed", -rc);
    reap();
    if (cfg_.doorbell && rt_) drain_inbound();
}

void reactor::reap() noexcept {
    io_uring_cqe* cqe = nullptr;
    while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
        const io_uring_cqe c = *cqe;
        io_uring_cqe_seen(&ring_, cqe);
        ++stats_.cqes;
        dispatch(c);
    }
}

void reactor::dispatch(const io_uring_cqe& c) noexcept {
    switch (detail::tag_of(c.user_data)) {
    case ud_tag::message:
        on_message_chain(
            reinterpret_cast<detail::xnode*>(static_cast<std::uintptr_t>(c.user_data)));
        return;
    case ud_tag::op:
        on_op_cqe(c);
        return;
    case ud_tag::linked_timeout:
        on_timeout_cqe(c);
        return;
    case ud_tag::cancel:
        on_cancel_cqe(c);
        return;
    case ud_tag::chain_failed:
        on_chain_failed(reinterpret_cast<detail::xnode*>(
                            static_cast<std::uintptr_t>(c.user_data & pointer_mask)),
                        c.res);
        return;
    case ud_tag::doorbell:
        return; // a wake-up; the queues are drained after every reap
    case ud_tag::doorbell_failed: {
        // The target may be asleep with messages in its queue, and nobody
        // else will ring (this core cleared its announcement): ring again.
        const auto target = static_cast<std::size_t>(c.user_data & pointer_mask);
        ROL_INVARIANT(target < resend_doorbell_.size(), "refused doorbell for an unknown core");
        resend_doorbell_[target] = 1;
        resend_pending_ = true;
        resend_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
        return;
    }
    }
    ROL_INVARIANT(false, "CQE with an unknown user_data tag");
}

// ---------------------------------------------------------------- slots

detail::op_slot& reactor::slot_at(slot_ref ref) noexcept {
    ROL_INVARIANT(ref.index < slots_.size(), "slot index out of range");
    auto& s = slots_[ref.index];
    ROL_INVARIANT(s.active && s.gen == ref.gen, "CQE or awaiter refers to a slot that is not in flight");
    return s;
}

slot_ref reactor::acquire_slot(op_kind kind) {
    std::uint32_t index;
    if (!free_slots_.empty()) {
        index = free_slots_.back();
        free_slots_.pop_back();
    } else {
        if (slots_.size() >= std::numeric_limits<std::uint32_t>::max())
            throw Failure(Error::full, "operation slot table exhausted");
        free_slots_.reserve(slots_.size() + 1);
        slots_.emplace_back();
        index = static_cast<std::uint32_t>(slots_.size() - 1);
    }
    auto& s = slots_[index];
    s.gen = (s.gen + 1) & detail::gen_mask;
    if (s.gen == 0) s.gen = 1;
    s.kind = kind;
    s.active = true;
    ++stats_.in_flight;
    if (stats_.in_flight > stats_.peak_in_flight) stats_.peak_in_flight = stats_.in_flight;
    return {index, s.gen};
}

void reactor::release_resources(op_slot& s) noexcept {
    s.owned = owned_buffer{};
    s.lease.reset();
    if (s.pring) {
        for (const auto& q : s.queue)
            if (q.flags & IORING_CQE_F_BUFFER)
                s.pring->recycle(static_cast<std::uint16_t>(q.flags >> IORING_CQE_BUFFER_SHIFT));
    }
    s.queue.clear();
    s.pring = nullptr;
    s.fds.clear();
    s.xcall.reset();
}

void reactor::free_slot(slot_ref ref) noexcept {
    auto& s = slot_at(ref);
    release_resources(s);
    if (s.pinned_file >= 0) unpin_direct(static_cast<unsigned>(std::exchange(s.pinned_file, -1)));
    if (s.closes_file >= 0) {
        const auto index = static_cast<unsigned>(std::exchange(s.closes_file, -1));
        direct_[index].close_pending = false;
        release_direct_if_unused(index);
    }
    s.active = false;
    s.primary_done = s.timeout_pending = s.detached = s.taken = s.cancel_sent = s.ended = false;
    s.pending = 0;
    s.max_queued = 0;
    s.res = s.timeout_res = 0;
    s.flags = 0;
    s.waiter = {};
    s.source = nullptr;
    s.reporter = nullptr;
    s.target = {};
    free_slots_.push_back(ref.index);
    --stats_.in_flight;
}

void reactor::maybe_free(op_slot& s, slot_ref ref) noexcept {
    if (s.pending != 0 || !(s.detached || s.taken)) return;
    if (s.detached) ++stats_.detached_released;
    free_slot(ref);
}

void reactor::finish_take(slot_ref ref) noexcept {
    auto& s = slot_at(ref);
    s.taken = true;
    s.waiter = {};
    maybe_free(s, ref);
}

void reactor::detach(slot_ref ref) noexcept {
    auto& s = slot_at(ref);
    s.waiter = {};
    s.detached = true;
    ++stats_.detached;
    unregister_source(s, ref);
    if (s.pending == 0) {
        free_slot(ref);
        return;
    }
    // The kernel still owns the buffers: they stay in the slot until the
    // operation's last CQE. Ask the kernel to finish early.
    if (!s.primary_done) request_cancel(ref, nullptr);
}

void reactor::unregister_source(op_slot& s, slot_ref ref) noexcept {
    if (!s.source) return;
    auto& ops = s.source->ops_;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].index == ref.index && ops[i].gen == ref.gen) {
            ops[i] = ops.back();
            ops.pop_back();
            break;
        }
    }
    s.source = nullptr;
}

void reactor::forget_source(cancel_source* src) noexcept {
    for (auto ref : src->ops_) {
        if (ref.index < slots_.size() && slots_[ref.index].active && slots_[ref.index].gen == ref.gen)
            slots_[ref.index].source = nullptr;
    }
    src->ops_.clear();
    for (auto& s : slots_)
        if (s.active && s.kind == op_kind::cancel && s.reporter == src) s.reporter = nullptr;
}

io_uring_sqe* reactor::reserve_sqes(unsigned n) {
    if (io_uring_sq_space_left(&ring_) < n) {
        const int rc = io_uring_submit(&ring_);
        if (rc < 0 && rc != -EBUSY && rc != -EAGAIN && rc != -EINTR)
            throw Failure(Error::system, "io_uring_submit failed", -rc);
        if (io_uring_sq_space_left(&ring_) < n)
            throw Failure(Error::busy, "submission queue is full", EBUSY);
    }
    return io_uring_get_sqe(&ring_);
}

io_uring_sqe* reactor::reserve_sqes_noexcept(unsigned n) noexcept {
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (io_uring_sq_space_left(&ring_) >= n) return io_uring_get_sqe(&ring_);
        io_uring_submit(&ring_);
    }
    ROL_INVARIANT(false, "submission queue stayed full while submitting a cancel or reply");
}

// ---------------------------------------------------------------- completions

void reactor::on_op_cqe(const io_uring_cqe& c) noexcept {
    const auto ref = detail::decode(c.user_data);
    auto& s = slot_at(ref);
    if (c.flags & IORING_CQE_F_NOTIF) {
        ROL_INVARIANT(s.kind == op_kind::send_zc && s.primary_done && s.pending > 0,
                      "zero-copy notification for a slot that does not expect one");
        ++stats_.zc_notifications;
        if (static_cast<std::uint32_t>(c.res) & IORING_NOTIF_USAGE_ZC_COPIED) ++stats_.zc_copied;
        --s.pending;
        maybe_free(s, ref);
        return;
    }
    if (s.kind == op_kind::multishot) {
        on_multishot(s, ref, c);
        return;
    }
    ROL_INVARIANT(!s.primary_done && s.pending > 0, "second result CQE for one operation");
    s.res = c.res;
    s.flags = c.flags;
    s.primary_done = true;
    --s.pending;
    if (s.kind == op_kind::send_zc && (c.flags & IORING_CQE_F_MORE)) {
        // The kernel still references the pages: the buffer stays in the slot
        // until the notification CQE.
        ++s.pending;
    }
    if (s.kind == op_kind::recvmsg) capture_rights(s);
    unregister_source(s, ref);
    ++stats_.ops_completed;
    complete(s, ref);
}

void reactor::on_timeout_cqe(const io_uring_cqe& c) noexcept {
    const auto ref = detail::decode(c.user_data);
    auto& s = slot_at(ref);
    ROL_INVARIANT(s.timeout_pending, "linked-timeout CQE for a slot without a pending timeout");
    s.timeout_pending = false;
    s.timeout_res = c.res;
    --s.pending;
    complete(s, ref);
}

void reactor::complete(op_slot& s, slot_ref ref) noexcept {
    if (!s.primary_done || s.timeout_pending) return;
    if (s.timeout_res == -ETIME && s.res == -ECANCELED) s.res = -ETIME;
    s.timeout_res = 0;
    if (s.waiter) {
        // Resume inline. The slot may be freed or reused by the time resume()
        // returns, so nothing touches `s` afterwards.
        std::exchange(s.waiter, {}).resume();
        return;
    }
    maybe_free(s, ref);
}

void reactor::on_multishot(op_slot& s, slot_ref ref, const io_uring_cqe& c) noexcept {
    const bool more = (c.flags & IORING_CQE_F_MORE) != 0;
    const bool has_buffer = (c.flags & IORING_CQE_F_BUFFER) != 0;
    const auto bid = static_cast<std::uint16_t>(c.flags >> IORING_CQE_BUFFER_SHIFT);
    if (has_buffer) s.pring->kernel_returned(bid);
    if (!more) {
        --s.pending;
        s.ended = true;
        s.primary_done = true;
        s.res = c.res;
        unregister_source(s, ref);
        ++stats_.ops_completed;
    }
    if (s.detached) {
        if (has_buffer) s.pring->recycle(bid);
        maybe_free(s, ref);
        return;
    }
    s.queue.push_back({c.res, c.flags});
    if (more && s.max_queued && s.queue.size() >= s.max_queued && !s.cancel_sent) {
        ++stats_.multishot_paused;
        request_cancel(ref, nullptr);
    }
    if (s.waiter) std::exchange(s.waiter, {}).resume();
}

void reactor::capture_rights(op_slot& s) noexcept {
    if (!s.msg || s.res < 0) return;
    const auto n = std::min<std::size_t>(s.msg->msg.msg_controllen, sizeof(s.msg->control));
    std::span<const std::byte> control(reinterpret_cast<const std::byte*>(s.msg->control), n);
    detail::for_each_right(control, [&](int fd) {
        if (fd >= 0) s.fds.emplace_back(fd);
    });
}

std::optional<recv_item> recv_stream::pop() {
    auto& s = r_->slot_at(ref_);
    if (s.queue.empty()) return std::nullopt;
    const io_result q = s.queue.front();
    s.queue.pop_front();
    recv_item item;
    item.res = q.res;
    item.more = (q.flags & IORING_CQE_F_MORE) != 0;
    if (q.flags & IORING_CQE_F_BUFFER)
        item.buffer = s.pring->wrap(static_cast<std::uint16_t>(q.flags >> IORING_CQE_BUFFER_SHIFT),
                                    q.res > 0 ? static_cast<std::uint32_t>(q.res) : 0u);
    return item;
}

void reactor::close_stream(slot_ref ref) noexcept {
    auto& s = slot_at(ref);
    s.waiter = {};
    if (s.pending == 0) {
        s.taken = true;
        free_slot(ref); // recycles buffers still queued
        return;
    }
    s.detached = true;
    ++stats_.detached;
    for (const auto& q : s.queue)
        if (q.flags & IORING_CQE_F_BUFFER)
            s.pring->recycle(static_cast<std::uint16_t>(q.flags >> IORING_CQE_BUFFER_SHIFT));
    s.queue.clear();
    unregister_source(s, ref);
    request_cancel(ref, nullptr);
}

recv_stream reactor::recv_multishot(file_ref f, provided_buffer_ring& ring, op_options o,
                                    std::uint32_t max_queued) {
    const int fd = f.fd;
    if (o.timeout.count() > 0)
        throw Failure(Error::invalid, "linked timeouts are not supported on multishot receive");
    bool ours = false;
    for (auto& r : provided_)
        if (r.get() == &ring) ours = true;
    if (!ours) throw Failure(Error::invalid, "provided-buffer ring belongs to another reactor");
    auto prep = [&](op_slot& s, io_uring_sqe* sqe) {
        s.pring = &ring;
        s.max_queued = max_queued;
        if (sqe) {
            io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
            sqe->flags |= IOSQE_BUFFER_SELECT;
            sqe->buf_group = ring.group();
        }
    };
    const auto ref = start({}, o, op_kind::multishot, prep, f);
    return recv_stream(*this, ref);
}

task<send_all_result> reactor::send_all(file_ref fd, owned_buffer buf, op_options o) {
    send_all_result out;
    std::size_t offset = 0;
    while (offset < buf.size()) {
        auto r = co_await send(fd, std::move(buf), offset, o);
        buf = std::move(r.buffer);
        ++out.submissions;
        if (r.res <= 0) {
            out.res = r.res < 0 ? r.res : -EIO;
            break;
        }
        offset += static_cast<std::size_t>(r.res);
    }
    out.sent = offset;
    out.buffer = std::move(buf);
    co_return out;
}

// ---------------------------------------------------------------- cancellation

bool reactor::request_cancel(slot_ref ref, cancel_source* reporter) noexcept {
    if (ref.index >= slots_.size()) return false;
    auto& s = slots_[ref.index];
    if (!s.active || s.gen != ref.gen) return false;
    if (s.kind == op_kind::cancel || s.kind == op_kind::xcall) return false;
    if (s.kind == op_kind::multishot ? s.ended : s.primary_done) return false;
    if (s.cancel_sent) return false;
    s.cancel_sent = true;
    slot_ref cref;
    try {
        cref = acquire_slot(op_kind::cancel);
    } catch (...) {
        ROL_INVARIANT(false, "out of memory while allocating a cancel slot");
    }
    auto& cs = slots_[cref.index];
    cs.detached = true;
    cs.target = ref;
    cs.reporter = reporter;
    cs.pending = 1;
    io_uring_sqe* sqe = reserve_sqes_noexcept(1);
    io_uring_prep_cancel64(sqe, detail::encode(ud_tag::op, ref), 0);
    io_uring_sqe_set_data64(sqe, detail::encode(ud_tag::cancel, cref));
    ++stats_.cancels;
    ++stats_.sqes;
    if (reporter) ++reporter->acks_pending_;
    return true;
}

void reactor::on_cancel_cqe(const io_uring_cqe& c) noexcept {
    const auto ref = detail::decode(c.user_data);
    auto& cs = slot_at(ref);
    const int res = c.res;
    if (res == 0) ++stats_.cancel_ok;
    else if (res == -ENOENT) ++stats_.cancel_not_found;
    else if (res == -EALREADY) ++stats_.cancel_already;
    else ++stats_.cancel_other;
    // The acknowledgement says nothing about the target's buffers: they are
    // released by the target's own CQE, which may come before or after this.
    cancel_source* reporter = cs.reporter;
    --cs.pending;
    free_slot(ref);
    if (reporter) reporter->on_ack(res);
}

cancel_source::~cancel_source() { r_->forget_source(this); }

void cancel_source::request() {
    requested_ = true;
    const auto ops = ops_;
    for (auto ref : ops)
        if (r_->request_cancel(ref, this)) ++report_.requested;
}

void cancel_source::on_ack(int res) noexcept {
    if (res == 0) ++report_.ok;
    else if (res == -ENOENT) ++report_.not_found;
    else if (res == -EALREADY) ++report_.already;
    else ++report_.other;
    if (acks_pending_ > 0) --acks_pending_;
    if (acks_pending_ == 0 && ack_waiter_) std::exchange(ack_waiter_, {}).resume();
}

// ---------------------------------------------------------------- event

event::~event() {
    for (awaiter* a = head_; a;) {
        awaiter* next = a->next_;
        a->linked_ = false;
        a->e_ = nullptr;
        a->prev_ = a->next_ = nullptr;
        a = next;
    }
}

void event::set() noexcept {
    set_ = true;
    while (head_) {
        awaiter* a = head_;
        a->unlink();
        r_->schedule(&a->node_);
    }
}

void event::awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    node_.handle = h;
    linked_ = true;
    prev_ = nullptr;
    next_ = e_->head_;
    if (e_->head_) e_->head_->prev_ = this;
    e_->head_ = this;
}

void event::awaiter::unlink() noexcept {
    if (!linked_) return;
    if (prev_) prev_->next_ = next_;
    else e_->head_ = next_;
    if (next_) next_->prev_ = prev_;
    prev_ = next_ = nullptr;
    linked_ = false;
}

// ---------------------------------------------------------------- cross-core
//
// A post or call node is appended to the outbox of its target core. Each loop
// iteration, before it submits, every non-empty outbox leaves as one chain: in
// one IORING_OP_MSG_RING, or pushed onto the (sender, target) queue with a
// doorbell only for a target that announced sleep (batch_messages off: one node
// at a time). The target walks the chain: requests (stage 1) are run; a call
// node then goes back, as a reply (stage 2), in the target's outbox for the
// calling core, whose slot completes when it arrives; the node belongs to that
// slot throughout. A post node, when batching, travels back the same way to be
// freed by the core that allocated it. runtime::outstanding() counts a post
// until it has run and a call until its reply has been reaped.

void reactor::enqueue_message(unsigned target, detail::xnode* n) noexcept {
    n->next = nullptr;
    auto& ob = outbox_[target];
    if (ob.tail) ob.tail->next = n;
    else {
        ob.head = n;
        ++outbox_pending_;
    }
    ob.tail = n;
    if (!cfg_.batch_messages) flush_target(target);
}

void reactor::flush_messages() noexcept {
    if (outbox_pending_ == 0) return;
    for (unsigned t = 0; t < outbox_.size(); ++t)
        if (outbox_[t].head) flush_target(t);
}

void reactor::flush_target(unsigned target) noexcept {
    auto& ob = outbox_[target];
    detail::xnode* head = ob.head;
    if (!head) return;
    ob = {};
    --outbox_pending_;
    if (cfg_.doorbell) {
        push_chain(target, head);
        return;
    }
    // Every `next` link is final now. Publish each node last: once its stage
    // is stored, this core does not touch the node again.
    for (detail::xnode* n = head; n;) {
        detail::xnode* next = n->next;
        n->stage.store(n->reply ? 2u : 1u, std::memory_order_release);
        n = next;
    }
    io_uring_sqe* sqe = reserve_sqes_noexcept(1);
    io_uring_prep_msg_ring(sqe, message_ring_fd(target, false), 0,
                           tagged_pointer(ud_tag::message, head), 0);
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
    io_uring_sqe_set_data64(sqe, tagged_pointer(ud_tag::chain_failed, head));
    ++stats_.sqes;
    ++stats_.message_rings;
}

int reactor::message_ring_fd(unsigned target, bool doorbell) const {
    if (cfg_.message_fault) {
        const int fd = cfg_.message_fault(target, doorbell);
        if (fd >= 0) return fd;
    }
    return rt_->ring_fd(target);
}

void reactor::push_chain(unsigned target, detail::xnode* head) noexcept {
    // Publish every node and reverse the chain: the queue is a stack of
    // chains, newest first, and the target reverses it back. `head`, the
    // oldest node, ends up last and is linked to the chains already queued.
    detail::xnode* newest = nullptr;
    for (detail::xnode* n = head; n;) {
        detail::xnode* next = n->next;
        n->stage.store(n->reply ? 2u : 1u, std::memory_order_release);
        n->next = newest;
        newest = n;
        n = next;
    }
    auto& top = rt_->inbox(core_, target);
    detail::xnode* below = top.load(std::memory_order_relaxed);
    do {
        head->next = below;
    } while (!top.compare_exchange_weak(below, newest, std::memory_order_seq_cst,
                                        std::memory_order_relaxed));
    ++stats_.queue_pushes;
    // Ring only if the target announced that it is going to sleep. The
    // exchange makes one sender ring per announcement.
    auto& flag = rt_->sleeping(target);
    if (flag.load(std::memory_order_seq_cst) && flag.exchange(false, std::memory_order_seq_cst))
        ring_doorbell(target);
}

void reactor::ring_doorbell(unsigned target) noexcept {
    io_uring_sqe* sqe = reserve_sqes_noexcept(1);
    io_uring_prep_msg_ring(sqe, message_ring_fd(target, true), 0,
                           tagged_value(ud_tag::doorbell, core_), 0);
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
    io_uring_sqe_set_data64(sqe, tagged_value(ud_tag::doorbell_failed, target));
    ++stats_.sqes;
    ++stats_.doorbells;
}

// Every source, this core included: a core may post to itself.
bool reactor::inbound_pending() const noexcept {
    for (unsigned s = 0, n = rt_->size(); s < n; ++s)
        if (rt_->inbox(s, core_).load(std::memory_order_seq_cst) != nullptr) return true;
    return false;
}

void reactor::drain_inbound() noexcept {
    for (unsigned s = 0, n = rt_->size(); s < n; ++s) {
        auto& top = rt_->inbox(s, core_);
        if (top.load(std::memory_order_relaxed) == nullptr) continue;
        detail::xnode* list = top.exchange(nullptr, std::memory_order_acquire);
        detail::xnode* fifo = nullptr; // back to the order the sender queued them in
        while (list) {
            detail::xnode* next = list->next;
            list->next = fifo;
            fifo = list;
            list = next;
        }
        on_message_chain(fifo);
    }
}

void reactor::resend_refused() noexcept {
    if (std::chrono::steady_clock::now() < resend_at_) return;
    resend_pending_ = false;
    for (detail::xnode* n = std::exchange(resend_head_, nullptr); n;) {
        detail::xnode* next = n->next;
        ++stats_.ring_retries;
        enqueue_message(n->origin, n);
        n = next;
    }
    for (unsigned t = 0; t < resend_doorbell_.size(); ++t) {
        if (!std::exchange(resend_doorbell_[t], std::uint8_t{0})) continue;
        ++stats_.ring_retries;
        ring_doorbell(t);
    }
}

void reactor::send_post(unsigned target, std::unique_ptr<detail::xnode> node) {
    check_thread();
    if (!rt_) throw Failure(Error::invalid, "post() needs a reactor created by a runtime");
    if (target >= rt_->size()) throw Failure(Error::invalid, "no such core");
    if (!rt_->accepting()) throw Failure(Error::closed, "runtime is shutting down");
    detail::xnode* n = node.release();
    n->is_call = false;
    n->reply = false;
    n->origin = core_;
    rt_->note_sent(core_);
    ++stats_.messages_sent;
    enqueue_message(target, n);
}

slot_ref reactor::start_call(std::coroutine_handle<> h, unsigned target,
                             std::unique_ptr<detail::xnode> node) {
    check_thread();
    if (!rt_) throw Failure(Error::invalid, "call() needs a reactor created by a runtime");
    if (target >= rt_->size()) throw Failure(Error::invalid, "no such core");
    if (!rt_->accepting()) throw Failure(Error::closed, "runtime is shutting down");
    const auto ref = acquire_slot(op_kind::xcall);
    auto& s = slot_at(ref);
    s.xcall = std::move(node);
    detail::xnode* n = s.xcall.get();
    n->is_call = true;
    n->reply = false;
    n->origin = core_;
    n->reply_ud = detail::encode(ud_tag::op, ref);
    rt_->note_sent(core_);
    s.pending = 1;
    s.waiter = h;
    ++stats_.ops_started;
    ++stats_.messages_sent;
    enqueue_message(target, n);
    return ref;
}

void reactor::on_message_chain(detail::xnode* head) noexcept {
    ROL_INVARIANT(head != nullptr && rt_ != nullptr, "message CQE on a reactor without a runtime");
    for (detail::xnode* n = head; n;) {
        const auto stage = n->stage.load(std::memory_order_acquire);
        ROL_INVARIANT(stage == 1 || stage == 2,
                      "cross-core message delivered before it was published");
        detail::xnode* next = n->next; // the node may be freed or re-linked below
        if (stage == 1) on_request(n);
        else if (n->is_call) on_reply(n);
        else {
            // A post node coming home after it ran on its target.
            delete n;
            rt_->note_done(core_);
        }
        n = next;
    }
}

void reactor::on_request(detail::xnode* n) noexcept {
    ++stats_.messages_received;
    if (!n->is_call) {
        n->run(*this);
        if (cfg_.batch_messages && cfg_.return_posts) {
            // Send the node back to be freed by the core that allocated it,
            // in that core's next chain: freeing it here would hand the
            // allocator a cross-thread free per message.
            n->reply = true;
            enqueue_message(n->origin, n);
        } else {
            delete n;
            rt_->note_done(core_);
        }
        return;
    }
    n->run(*this);
    // From here the node travels back; it belongs to the caller's slot.
    n->reply = true;
    ++stats_.replies_sent;
    enqueue_message(n->origin, n);
}

void reactor::on_reply(detail::xnode* n) noexcept {
    const auto ref = detail::decode(n->reply_ud);
    auto& s = slot_at(ref);
    ROL_INVARIANT(s.kind == op_kind::xcall && !s.primary_done && s.xcall.get() == n,
                  "cross-core reply for a slot that is not waiting for it");
    s.res = 0;
    s.primary_done = true;
    --s.pending;
    rt_->note_done(core_);
    ++stats_.ops_completed;
    complete(s, ref);
}

void reactor::on_chain_failed(detail::xnode* head, int res) noexcept {
    // The kernel refused the MSG_RING: the chain never reached the target and
    // is still ours. Requests fail here (a call completes with the error, a
    // post is dropped and counted). A reply, or a post node on its way home,
    // belongs to the other core and must arrive: it is sent again after a
    // short delay, until the kernel accepts it.
    for (detail::xnode* n = head; n;) {
        detail::xnode* next = n->next;
        if (n->reply) {
            n->next = resend_head_;
            resend_head_ = n;
            resend_pending_ = true;
            resend_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
            n = next;
            continue;
        }
        ++stats_.messages_failed;
        if (!n->is_call) {
            delete n;
            rt_->note_done(core_);
        } else {
            const auto ref = detail::decode(n->reply_ud);
            auto& s = slot_at(ref);
            ROL_INVARIANT(s.kind == op_kind::xcall && !s.primary_done,
                          "call failure for a slot that is not waiting for a reply");
            s.res = res < 0 ? res : -EIO;
            s.primary_done = true;
            --s.pending;
            rt_->note_done(core_);
            complete(s, ref);
        }
        n = next;
    }
}

// ---------------------------------------------------------------- shutdown

void reactor::cancel_all() noexcept {
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        auto& s = slots_[i];
        if (s.active && s.kind != op_kind::cancel && s.kind != op_kind::xcall)
            request_cancel({i, s.gen}, nullptr);
    }
}

void reactor::destroy_tasks() noexcept {
    while (spawned_) {
        detail::spawn_promise* p = spawned_;
        on_spawn_done(p);
        std::coroutine_handle<detail::spawn_promise>::from_promise(*p).destroy();
    }
}

void reactor::begin_shutdown() noexcept {
    closing_ = true;
    cancel_all();
}

bool reactor::drain(std::chrono::steady_clock::time_point deadline) {
    using clock = std::chrono::steady_clock;
    const auto tick = std::chrono::milliseconds(1);
    // 1. Spawned tasks see -ECANCELED and get a chance to finish.
    while ((spawned_ || !ready_.empty()) && clock::now() < deadline) {
        if (ready_.empty() && stats_.in_flight == 0) break; // blocked on nothing
        step(ready_.empty(), tick);
    }
    // 2. Destroy tasks that did not finish. Their awaiters detach their
    //    operations; the slots keep the buffers.
    destroy_tasks();
    cancel_all();
    // 3. Reap until no operation is in flight.
    while (stats_.in_flight > 0 && clock::now() < deadline) step(ready_.empty(), tick);
    if (stats_.in_flight > 0) {
        std::fprintf(stderr, "rol: shutdown timed out with %zu operations in flight\n",
                     stats_.in_flight);
        return false;
    }
    return true;
}

void reactor::close_ring() noexcept {
    if (closed_) return;
    // Nothing should be left unsent (the runtime closes rings only when no
    // message is outstanding). Posts still here are ours to free; call nodes
    // belong to slots and are handled with them.
    for (auto& ob : outbox_) {
        for (detail::xnode* n = ob.head; n;) {
            detail::xnode* next = n->next;
            if (!n->is_call) delete n;
            n = next;
        }
        ob = {};
    }
    outbox_pending_ = 0;
    if (stats_.in_flight > 0) {
        // The kernel may still read or write these slots' memory. Leak it.
        stats_.abandoned += stats_.in_flight;
        for (auto& s : slots_) {
            if (!s.active) continue;
            if (s.owned) park(std::make_shared<owned_buffer>(std::move(s.owned)));
            if (s.msg) park(std::shared_ptr<detail::msg_storage>(std::move(s.msg)));
            if (s.xcall) park(std::shared_ptr<detail::xnode>(std::move(s.xcall)));
            s.lease.leak();
            s.queue.clear();
            s.pring = nullptr;
            s.waiter = {};
            s.active = false;
        }
    }
    if (fixed_) stats_.leases_at_close = fixed_->outstanding();
    for (auto& r : provided_) stats_.provided_held_at_close += r->held();
    if (fixed_) {
        if (fixed_->outstanding() > 0 || stats_.abandoned > 0) {
            fixed_->orphan(); // leases still exist: keep the memory, stop serving
            park(std::shared_ptr<fixed_buffer_pool>(std::move(fixed_)));
        } else {
            fixed_->unregister();
            fixed_.reset();
        }
    }
    for (auto& r : provided_) {
        if (r->held() > 0 || stats_.abandoned > 0) {
            r->orphaned_ = true;
            park(std::shared_ptr<provided_buffer_ring>(std::move(r)));
        } else {
            r->unregister();
            r.reset();
        }
    }
    provided_.clear();
    if (!direct_.empty() && ring_open_) io_uring_unregister_files(&ring_);
    if (ring_open_) io_uring_queue_exit(&ring_);
    ring_open_ = false;
    closed_ = true;
}

void reactor::shutdown() {
    if (closed_) return;
    check_thread();
    begin_shutdown();
    ++loop_depth_;
    try {
        drain(std::chrono::steady_clock::now() + cfg_.shutdown_timeout);
    } catch (...) {
        // io_uring_enter failed during shutdown; close anyway.
    }
    --loop_depth_;
    close_ring();
}

} // namespace rol
