#pragma once
#include "rol/reactor.hpp"
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace rol {

struct runtime_config {
    unsigned cores = 2;
    std::vector<int> cpus{}; // if not empty: pin core i's thread to cpus[i]
    reactor_config reactor{};
};

// Thread-per-core runtime: one thread and one reactor (one ring) per core.
// Cores talk through IORING_OP_MSG_RING (reactor::post / reactor::call).
//
// run() starts every core's root task, keeps every ring alive and serving
// messages until all roots have returned, then shuts the cores down together:
// new cross-core messages are refused, each core cancels and drains its own
// operations while still answering requests already in flight, and no ring is
// closed until no message is outstanding anywhere.
class runtime {
public:
    explicit runtime(runtime_config cfg = {});
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;
    ~runtime() = default;

    // Runs entry(core_reactor) on every core. Returns when every core has shut
    // down; rethrows the first exception a root task threw.
    void run(std::function<task<void>(reactor&)> entry);
    unsigned size() const noexcept { return cfg_.cores; }

    // Used by reactors.
    int ring_fd(unsigned core) const noexcept { return ring_fds_[core]; }
    bool accepting() const noexcept { return accepting_.load(); }
    // Shared-memory transport (reactor_config::doorbell): the queue from
    // `source` to `target` (a stack of chains, newest first; one producer and
    // one consumer), and each core's announcement that it is about to sleep.
    std::atomic<detail::xnode*>& inbox(unsigned source, unsigned target) const noexcept {
        return inboxes_[target * cfg_.cores + source].top;
    }
    std::atomic<bool>& sleeping(unsigned core) const noexcept { return sleep_flags_[core].sleeping; }

    // Cross-core messages not yet finished: posts until they have run, calls
    // until their reply has been reaped. Each core counts what it sent and what
    // it finished in its own cache line (one writer each, no shared atomic per
    // message). The sum is exact once no core can send any more, which is the
    // only time shutdown relies on it reaching zero.
    std::int64_t outstanding() const noexcept;
    void note_sent(unsigned core) noexcept { bump(counters_[core].sent); }
    void note_done(unsigned core) noexcept { bump(counters_[core].done); }

private:
    struct alignas(64) core_counters {
        std::atomic<std::uint64_t> sent{0};
        std::atomic<std::uint64_t> done{0};
    };
    struct alignas(64) queue_head {
        std::atomic<detail::xnode*> top{nullptr};
    };
    struct alignas(64) sleep_flag {
        std::atomic<bool> sleeping{false};
    };
    static void bump(std::atomic<std::uint64_t>& c) noexcept {
        c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    void core_main(unsigned core, const std::function<task<void>(reactor&)>& entry);
    void record(std::exception_ptr e);

    runtime_config cfg_;
    std::vector<int> ring_fds_;
    std::unique_ptr<core_counters[]> counters_;
    std::unique_ptr<queue_head[]> inboxes_;    // [target * cores + source]
    std::unique_ptr<sleep_flag[]> sleep_flags_; // [core]
    std::atomic<bool> accepting_{true};
    std::atomic<unsigned> started_{0};
    std::atomic<unsigned> roots_done_{0};
    std::atomic<unsigned> quiesced_{0};
    std::atomic<unsigned> finished_{0};
    std::atomic<bool> failed_start_{false};
    std::mutex error_mutex_;
    std::exception_ptr error_;
};

} // namespace rol
