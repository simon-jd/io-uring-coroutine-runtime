#pragma once
#include "rol/detail/ready.hpp"
#include "rol/error.hpp"
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

struct io_uring;
struct io_uring_buf_ring;

namespace rol {

class reactor;
class fixed_buffer_pool;
class provided_buffer_ring;

// A copyable name for one lease of one registered buffer. It stops resolving
// as soon as that lease ends: the slot's generation moves on.
struct buffer_handle {
    std::uint64_t pool = 0;
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
};

// Exclusive use of one registered (fixed) buffer. Move-only; destroying it
// returns the buffer to its pool. Operations take the lease by value, so while
// the kernel uses the buffer nobody outside the runtime holds a lease to it.
class fixed_lease {
public:
    fixed_lease() = default;
    fixed_lease(fixed_lease&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_), gen_(other.gen_) {}
    fixed_lease& operator=(fixed_lease&& other) noexcept {
        if (this != &other) {
            reset();
            pool_ = std::exchange(other.pool_, nullptr);
            index_ = other.index_;
            gen_ = other.gen_;
        }
        return *this;
    }
    fixed_lease(const fixed_lease&) = delete;
    fixed_lease& operator=(const fixed_lease&) = delete;
    ~fixed_lease() { reset(); }

    void reset() noexcept;
    // Gives up the lease without returning the buffer (the kernel may still use it).
    void leak() noexcept { pool_ = nullptr; }
    std::span<std::byte> data() const noexcept;
    std::size_t size() const noexcept;
    std::uint32_t index() const noexcept { return index_; }
    std::uint32_t generation() const noexcept { return gen_; }
    buffer_handle handle() const noexcept;
    fixed_buffer_pool* pool() const noexcept { return pool_; }
    explicit operator bool() const noexcept { return pool_ != nullptr; }

private:
    friend class fixed_buffer_pool;
    fixed_lease(fixed_buffer_pool* pool, std::uint32_t index, std::uint32_t gen) noexcept
        : pool_(pool), index_(index), gen_(gen) {}
    fixed_buffer_pool* pool_ = nullptr;
    std::uint32_t index_ = 0;
    std::uint32_t gen_ = 0;
};

// Equal-sized buffers registered with one reactor's ring (READ_FIXED,
// WRITE_FIXED, SEND_ZC with IORING_RECVSEND_FIXED_BUF).
class fixed_buffer_pool {
public:
    enum class buffer_state : std::uint8_t { free, leased, in_flight };

    fixed_buffer_pool(const fixed_buffer_pool&) = delete;
    fixed_buffer_pool& operator=(const fixed_buffer_pool&) = delete;
    ~fixed_buffer_pool();

    std::optional<fixed_lease> try_acquire();

    // Waits until a buffer is free. Waiters are served in FIFO order.
    class acquire_awaiter {
    public:
        explicit acquire_awaiter(fixed_buffer_pool& pool) noexcept : pool_(&pool) {}
        acquire_awaiter(const acquire_awaiter&) = delete;
        acquire_awaiter& operator=(const acquire_awaiter&) = delete;
        ~acquire_awaiter();
        bool await_ready();
        void await_suspend(std::coroutine_handle<> h);
        fixed_lease await_resume() noexcept { return std::move(lease_); }

    private:
        friend class fixed_buffer_pool;
        fixed_buffer_pool* pool_;
        fixed_lease lease_;
        detail::ready_node node_;
        acquire_awaiter* prev_ = nullptr;
        acquire_awaiter* next_ = nullptr;
        bool waiting_ = false;
    };
    acquire_awaiter acquire() noexcept { return acquire_awaiter(*this); }

    std::size_t capacity() const noexcept { return count_; }
    std::size_t buffer_size() const noexcept { return size_; }
    std::size_t available() const noexcept { return free_.size(); }
    std::size_t outstanding() const noexcept { return count_ - free_.size(); }
    std::uint64_t id() const noexcept { return id_; }
    buffer_state state(std::uint32_t index) const;

    // Read access through a handle. Throws Failure(stale) when that lease has
    // ended and Failure(busy) while an operation owns the buffer.
    std::span<const std::byte> view(buffer_handle h) const;

    // Used by the reactor when an operation takes or gives back a lease.
    void check_lease(const fixed_lease& lease) const;
    void begin_io(const fixed_lease& lease) noexcept;
    void end_io(const fixed_lease& lease) noexcept;
    std::byte* address(std::uint32_t index) const noexcept { return mem_ + index * size_; }

private:
    friend class reactor;
    friend class fixed_lease;
    fixed_buffer_pool(reactor& owner, io_uring* ring, unsigned count, std::size_t size);
    void release(std::uint32_t index, std::uint32_t gen) noexcept;
    fixed_lease make_lease(std::uint32_t index) noexcept;
    void unregister() noexcept;
    void orphan() noexcept;

    reactor* reactor_;
    io_uring* ring_;
    std::uint64_t id_;
    std::byte* mem_ = nullptr;
    std::size_t count_;
    std::size_t size_;
    std::size_t bytes_;
    bool registered_ = false;
    bool orphaned_ = false;
    std::vector<std::uint32_t> gen_;
    std::vector<buffer_state> state_;
    std::vector<std::uint32_t> free_;
    acquire_awaiter* wait_head_ = nullptr;
    acquire_awaiter* wait_tail_ = nullptr;
};

// One buffer the kernel picked from a provided-buffer ring. The holder must give
// it back (release() or destruction) before the kernel can use it again.
class provided_buffer {
public:
    provided_buffer() = default;
    provided_buffer(provided_buffer&& other) noexcept
        : ring_(std::exchange(other.ring_, nullptr)), bid_(other.bid_), len_(other.len_) {}
    provided_buffer& operator=(provided_buffer&& other) noexcept {
        if (this != &other) {
            release();
            ring_ = std::exchange(other.ring_, nullptr);
            bid_ = other.bid_;
            len_ = other.len_;
        }
        return *this;
    }
    provided_buffer(const provided_buffer&) = delete;
    provided_buffer& operator=(const provided_buffer&) = delete;
    ~provided_buffer() { release(); }

    std::span<std::byte> data() const noexcept;
    std::size_t size() const noexcept { return len_; }
    std::uint16_t id() const noexcept { return bid_; }
    void release() noexcept;
    explicit operator bool() const noexcept { return ring_ != nullptr; }

private:
    friend class provided_buffer_ring;
    provided_buffer(provided_buffer_ring* ring, std::uint16_t bid, std::uint32_t len) noexcept
        : ring_(ring), bid_(bid), len_(len) {}
    provided_buffer_ring* ring_ = nullptr;
    std::uint16_t bid_ = 0;
    std::uint32_t len_ = 0;
};

// A provided-buffer ring (IORING_REGISTER_PBUF_RING) for multishot receive.
class provided_buffer_ring {
public:
    provided_buffer_ring(const provided_buffer_ring&) = delete;
    provided_buffer_ring& operator=(const provided_buffer_ring&) = delete;
    ~provided_buffer_ring();

    std::uint16_t group() const noexcept { return group_; }
    unsigned entries() const noexcept { return entries_; }
    std::size_t buffer_size() const noexcept { return size_; }
    unsigned held() const noexcept { return held_count_; }
    unsigned in_ring() const noexcept { return entries_ - held_count_; }

    // Used by the reactor. kernel_returned: a CQE carried buffer `bid`, so the
    // kernel no longer owns it. wrap: hand it to a holder. recycle: give it back.
    void kernel_returned(std::uint16_t bid) noexcept;
    provided_buffer wrap(std::uint16_t bid, std::uint32_t len) noexcept;
    void recycle(std::uint16_t bid) noexcept;
    std::byte* address(std::uint16_t bid) const noexcept { return mem_ + bid * size_; }

private:
    friend class reactor;
    provided_buffer_ring(io_uring* ring, std::uint16_t group, unsigned entries, std::size_t size);
    void unregister() noexcept;

    io_uring* ring_;
    bool orphaned_ = false;
    io_uring_buf_ring* br_ = nullptr;
    std::byte* mem_ = nullptr;
    std::uint16_t group_;
    unsigned entries_;
    std::size_t size_;
    std::size_t bytes_;
    std::vector<std::uint8_t> held_;
    unsigned held_count_ = 0;
};

} // namespace rol
