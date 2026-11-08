#include "rol/buffers.hpp"
#include "rol/buffer.hpp"
#include "rol/reactor.hpp"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace rol {

// ---------------------------------------------------------------- owned_buffer

owned_buffer owned_buffer::allocate(std::size_t capacity) {
    if (capacity == 0) throw Failure(Error::invalid, "zero-capacity buffer");
    owned_buffer b;
    b.data_.reset(new std::byte[capacity]());
    b.capacity_ = capacity;
    return b;
}

owned_buffer owned_buffer::copy_of(std::span<const std::byte> bytes) {
    auto b = allocate(bytes.empty() ? 1 : bytes.size());
    if (!bytes.empty()) std::memcpy(b.data_.get(), bytes.data(), bytes.size());
    b.size_ = bytes.size();
    return b;
}

owned_buffer owned_buffer::copy_of(std::string_view text) {
    return copy_of(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

void owned_buffer::resize(std::size_t n) {
    if (n > capacity_) throw Failure(Error::invalid, "resize beyond capacity");
    size_ = n;
}

void unique_fd::reset(int fd) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
}

// ---------------------------------------------------------------- fixed buffers

namespace {
std::atomic<std::uint64_t> next_pool_id{1};
}

void fixed_lease::reset() noexcept {
    if (pool_) std::exchange(pool_, nullptr)->release(index_, gen_);
}

std::span<std::byte> fixed_lease::data() const noexcept {
    if (!pool_) return {};
    return {pool_->address(index_), pool_->buffer_size()};
}

std::size_t fixed_lease::size() const noexcept { return pool_ ? pool_->buffer_size() : 0; }

buffer_handle fixed_lease::handle() const noexcept {
    if (!pool_) return {};
    return {pool_->id(), index_, gen_};
}

fixed_buffer_pool::fixed_buffer_pool(reactor& owner, io_uring* ring, unsigned count,
                                     std::size_t size)
    : reactor_(&owner), ring_(ring), id_(next_pool_id.fetch_add(1)), count_(count), size_(size) {
    if (count == 0 || count > 16384 || size == 0 || size > (1u << 30))
        throw Failure(Error::invalid, "fixed buffer pool: bad count or size");
    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    bytes_ = ((count * size) + page - 1) / page * page;
    void* m = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) throw Failure(Error::allocation, "fixed buffer pool: mmap failed", errno);
    mem_ = static_cast<std::byte*>(m);
    std::vector<iovec> iov(count);
    for (unsigned i = 0; i < count; ++i) iov[i] = {mem_ + i * size, size};
    const int rc = io_uring_register_buffers(ring_, iov.data(), count);
    if (rc < 0) {
        ::munmap(mem_, bytes_);
        throw Failure(Error::system,
                      "io_uring_register_buffers failed (RLIMIT_MEMLOCK too small?)", -rc);
    }
    registered_ = true;
    gen_.assign(count, 0);
    state_.assign(count, buffer_state::free);
    free_.reserve(count);
    for (unsigned i = count; i > 0; --i) free_.push_back(i - 1);
}

fixed_buffer_pool::~fixed_buffer_pool() {
    unregister();
    if (mem_) ::munmap(mem_, bytes_);
}

void fixed_buffer_pool::unregister() noexcept {
    if (registered_) io_uring_unregister_buffers(ring_);
    registered_ = false;
}

void fixed_buffer_pool::orphan() noexcept {
    unregister();
    orphaned_ = true;
    for (auto* w = wait_head_; w;) {
        auto* next = w->next_;
        w->waiting_ = false;
        w->pool_ = nullptr;
        w->prev_ = w->next_ = nullptr;
        w = next;
    }
    wait_head_ = wait_tail_ = nullptr;
}

fixed_lease fixed_buffer_pool::make_lease(std::uint32_t index) noexcept {
    auto& g = gen_[index];
    g = g + 1 == 0 ? 1 : g + 1;
    state_[index] = buffer_state::leased;
    return fixed_lease(this, index, g);
}

std::optional<fixed_lease> fixed_buffer_pool::try_acquire() {
    if (orphaned_) throw Failure(Error::closed, "fixed buffer pool outlived its reactor");
    if (free_.empty() || wait_head_) return std::nullopt;
    const auto index = free_.back();
    free_.pop_back();
    return make_lease(index);
}

void fixed_buffer_pool::release(std::uint32_t index, std::uint32_t gen) noexcept {
    ROL_INVARIANT(index < count_ && gen_[index] == gen && state_[index] != buffer_state::free,
                  "fixed buffer released twice or by a stale lease");
    state_[index] = buffer_state::free;
    if (orphaned_) return;
    if (acquire_awaiter* w = wait_head_) {
        wait_head_ = w->next_;
        if (wait_head_) wait_head_->prev_ = nullptr;
        else wait_tail_ = nullptr;
        w->prev_ = w->next_ = nullptr;
        w->waiting_ = false;
        w->lease_ = make_lease(index);
        reactor_->schedule(&w->node_);
        return;
    }
    free_.push_back(index);
}

fixed_buffer_pool::buffer_state fixed_buffer_pool::state(std::uint32_t index) const {
    if (index >= count_) throw Failure(Error::invalid, "buffer index out of range");
    return state_[index];
}

std::span<const std::byte> fixed_buffer_pool::view(buffer_handle h) const {
    if (h.pool != id_ || h.index >= count_ || h.generation == 0 || gen_[h.index] != h.generation ||
        state_[h.index] == buffer_state::free)
        throw Failure(Error::stale, "stale or foreign buffer handle");
    if (state_[h.index] == buffer_state::in_flight)
        throw Failure(Error::busy, "buffer is owned by an operation in flight");
    return {address(h.index), size_};
}

void fixed_buffer_pool::check_lease(const fixed_lease& lease) const {
    if (!lease) throw Failure(Error::invalid, "empty lease");
    if (lease.pool_ != this) throw Failure(Error::stale, "lease belongs to another reactor's pool");
    if (gen_[lease.index_] != lease.gen_ || state_[lease.index_] != buffer_state::leased)
        throw Failure(Error::stale, "lease is stale or already in flight");
}

void fixed_buffer_pool::begin_io(const fixed_lease& lease) noexcept {
    ROL_INVARIANT(state_[lease.index_] == buffer_state::leased && gen_[lease.index_] == lease.gen_,
                  "operation started on a buffer that is not leased");
    state_[lease.index_] = buffer_state::in_flight;
}

void fixed_buffer_pool::end_io(const fixed_lease& lease) noexcept {
    ROL_INVARIANT(state_[lease.index_] == buffer_state::in_flight && gen_[lease.index_] == lease.gen_,
                  "operation returned a buffer it did not own");
    state_[lease.index_] = buffer_state::leased;
}

fixed_buffer_pool::acquire_awaiter::~acquire_awaiter() {
    if (waiting_ && pool_) {
        if (prev_) prev_->next_ = next_;
        else pool_->wait_head_ = next_;
        if (next_) next_->prev_ = prev_;
        else pool_->wait_tail_ = prev_;
    }
}

bool fixed_buffer_pool::acquire_awaiter::await_ready() {
    if (auto l = pool_->try_acquire()) {
        lease_ = std::move(*l);
        return true;
    }
    return false;
}

void fixed_buffer_pool::acquire_awaiter::await_suspend(std::coroutine_handle<> h) {
    node_.handle = h;
    waiting_ = true;
    prev_ = pool_->wait_tail_;
    next_ = nullptr;
    if (pool_->wait_tail_) pool_->wait_tail_->next_ = this;
    else pool_->wait_head_ = this;
    pool_->wait_tail_ = this;
}

// ---------------------------------------------------------------- provided buffers

std::span<std::byte> provided_buffer::data() const noexcept {
    if (!ring_) return {};
    return {ring_->address(bid_), len_};
}

void provided_buffer::release() noexcept {
    if (ring_) std::exchange(ring_, nullptr)->recycle(bid_);
}

provided_buffer_ring::provided_buffer_ring(io_uring* ring, std::uint16_t group, unsigned entries,
                                           std::size_t size)
    : ring_(ring), group_(group), entries_(entries), size_(size) {
    if (entries == 0 || entries > 32768 || (entries & (entries - 1)) || size == 0 ||
        size > (1u << 24))
        throw Failure(Error::invalid, "provided ring: entries must be a power of two <= 32768");
    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    bytes_ = ((entries * size) + page - 1) / page * page;
    void* m = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) throw Failure(Error::allocation, "provided ring: mmap failed", errno);
    mem_ = static_cast<std::byte*>(m);
    int err = 0;
    br_ = io_uring_setup_buf_ring(ring_, entries, group, 0, &err);
    if (!br_) {
        ::munmap(mem_, bytes_);
        throw Failure(Error::system, "io_uring_setup_buf_ring failed", -err);
    }
    held_.assign(entries, 0);
    const int mask = io_uring_buf_ring_mask(entries);
    for (unsigned i = 0; i < entries; ++i)
        io_uring_buf_ring_add(br_, address(static_cast<std::uint16_t>(i)),
                              static_cast<unsigned>(size), static_cast<unsigned short>(i), mask,
                              static_cast<int>(i));
    io_uring_buf_ring_advance(br_, static_cast<int>(entries));
}

provided_buffer_ring::~provided_buffer_ring() {
    unregister();
    if (mem_) ::munmap(mem_, bytes_);
}

void provided_buffer_ring::unregister() noexcept {
    if (br_) io_uring_free_buf_ring(ring_, br_, entries_, group_);
    br_ = nullptr;
}

void provided_buffer_ring::kernel_returned(std::uint16_t bid) noexcept {
    ROL_INVARIANT(bid < entries_ && !held_[bid],
                  "kernel returned a provided buffer that was not in the ring");
    held_[bid] = 1;
    ++held_count_;
}

provided_buffer provided_buffer_ring::wrap(std::uint16_t bid, std::uint32_t len) noexcept {
    ROL_INVARIANT(bid < entries_ && held_[bid], "wrapping a provided buffer the kernel still owns");
    return provided_buffer(this, bid, len);
}

void provided_buffer_ring::recycle(std::uint16_t bid) noexcept {
    ROL_INVARIANT(bid < entries_ && held_[bid], "provided buffer returned twice");
    held_[bid] = 0;
    --held_count_;
    if (orphaned_ || !br_) return;
    io_uring_buf_ring_add(br_, address(bid), static_cast<unsigned>(size_), bid,
                          io_uring_buf_ring_mask(entries_), 0);
    io_uring_buf_ring_advance(br_, 1);
}

} // namespace rol
