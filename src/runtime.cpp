#include "rol/runtime.hpp"
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <thread>

namespace rol {

runtime::runtime(runtime_config cfg) : cfg_(cfg) {
    if (cfg.cores == 0 || cfg.cores > 1024) throw Failure(Error::invalid, "cores must be 1..1024");
    if (!cfg.cpus.empty() && cfg.cpus.size() != cfg.cores)
        throw Failure(Error::invalid, "cpus must be empty or list one CPU per core");
    ring_fds_.assign(cfg.cores, -1);
    counters_.reset(new core_counters[cfg.cores]);
    inboxes_.reset(new queue_head[std::size_t{cfg.cores} * cfg.cores]);
    sleep_flags_.reset(new sleep_flag[cfg.cores]);
}

std::int64_t runtime::outstanding() const noexcept {
    std::uint64_t sent = 0, done = 0;
    for (unsigned i = 0; i < cfg_.cores; ++i) {
        sent += counters_[i].sent.load(std::memory_order_acquire);
        done += counters_[i].done.load(std::memory_order_acquire);
    }
    return static_cast<std::int64_t>(sent - done);
}

void runtime::record(std::exception_ptr e) {
    std::lock_guard lock(error_mutex_);
    if (!error_) error_ = e;
}

namespace {
// Spin-then-yield wait on a shared counter. Used only at start-up and for the
// final barrier, when the reactor has no work left.
template <class Pred>
void wait_for(Pred pred) {
    while (!pred()) std::this_thread::yield();
}
} // namespace

void runtime::core_main(unsigned core, const std::function<task<void>(reactor&)>& entry) {
    const unsigned n = cfg_.cores;
    if (!cfg_.cpus.empty()) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<std::size_t>(cfg_.cpus[core]), &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
    std::unique_ptr<reactor> r;
    try {
        r.reset(new reactor(cfg_.reactor, this, core));
        ring_fds_[core] = r->ring_fd();
    } catch (...) {
        record(std::current_exception());
        failed_start_ = true;
    }
    started_.fetch_add(1);
    wait_for([&] { return started_.load() == n; });
    if (failed_start_) {
        // Every core sees the failure before anyone sends a message.
        finished_.fetch_add(1);
        return;
    }

    try {
        r->block_on(entry(*r));
    } catch (...) {
        record(std::current_exception());
    }
    roots_done_.fetch_add(1);
    try {
        // Keep serving other cores until every root task has returned.
        r->serve_until([&] { return roots_done_.load() == n; });
        accepting_ = false;
        r->begin_shutdown();
        r->drain(std::chrono::steady_clock::now() + cfg_.reactor.shutdown_timeout);
        quiesced_.fetch_add(1);
        // Answer requests that were already in flight until nothing is
        // outstanding anywhere; only then may rings close.
        r->serve_until([&] {
            return quiesced_.load() == n && outstanding() == 0 && r->in_flight() == 0;
        });
    } catch (...) {
        record(std::current_exception());
    }
    if (r->task_error_) record(r->task_error_);
    finished_.fetch_add(1);
    wait_for([&] { return finished_.load() == n; });
    r->close_ring();
}

void runtime::run(std::function<task<void>(reactor&)> entry) {
    std::vector<std::thread> threads;
    threads.reserve(cfg_.cores);
    for (unsigned i = 0; i < cfg_.cores; ++i)
        threads.emplace_back([this, i, &entry] { core_main(i, entry); });
    for (auto& t : threads) t.join();
    if (error_) std::rethrow_exception(error_);
}

} // namespace rol
