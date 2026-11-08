// Thread-per-core runtime and IORING_OP_MSG_RING messaging. Run under TSan.
#include "rol/runtime.hpp"
#include "rol/when_all.hpp"
#include "test.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sys/eventfd.h>
#include <thread>
using namespace rol;
using namespace std::chrono_literals;

namespace {
runtime_config with_cores(runtime_config base, unsigned cores) {
    base.cores = cores;
    return base;
}

void spin_for(std::chrono::nanoseconds d) {
    const auto end = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < end) {
    }
}

// Watches a progress counter from its own thread. If the counter stops moving
// for `limit`, it counts a stall and wakes every core with a doorbell sent
// from its own ring, so a lost wake-up ends in a failed assertion instead of
// a hang.
class stall_watchdog {
public:
    stall_watchdog(std::atomic<long>& progress, std::vector<std::atomic<int>>& ring_fds,
                   std::chrono::milliseconds limit)
        : progress_(progress), fds_(ring_fds), limit_(limit) {
        CHECK(io_uring_queue_init(8, &ring_, 0) == 0);
        thread_ = std::thread([this] { watch(); });
    }
    stall_watchdog(const stall_watchdog&) = delete;
    stall_watchdog& operator=(const stall_watchdog&) = delete;
    ~stall_watchdog() {
        stop_.store(true);
        thread_.join();
        io_uring_queue_exit(&ring_);
    }
    long stalls() const { return stalls_.load(); }

private:
    void watch() {
        long last = progress_.load();
        auto since = std::chrono::steady_clock::now();
        while (!stop_.load()) {
            std::this_thread::sleep_for(10ms);
            const long now = progress_.load();
            if (now != last) {
                last = now;
                since = std::chrono::steady_clock::now();
                continue;
            }
            if (std::chrono::steady_clock::now() - since < limit_) continue;
            stalls_.fetch_add(1);
            std::fprintf(stderr, "watchdog: no progress for %lld ms at %ld; waking every core\n",
                         static_cast<long long>(limit_.count()), now);
            for (auto& f : fds_) {
                const int fd = f.load();
                if (fd < 0) continue;
                io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                io_uring_prep_msg_ring(sqe, fd, 0,
                                       std::uint64_t(detail::ud_tag::doorbell) << 56, 0);
                io_uring_submit(&ring_);
                io_uring_cqe* cqe = nullptr;
                if (io_uring_wait_cqe(&ring_, &cqe) == 0) io_uring_cqe_seen(&ring_, cqe);
            }
            since = std::chrono::steady_clock::now();
        }
    }
    std::atomic<long>& progress_;
    std::vector<std::atomic<int>>& fds_;
    std::chrono::milliseconds limit_;
    io_uring ring_{};
    std::atomic<bool> stop_{false};
    std::atomic<long> stalls_{0};
    std::thread thread_;
};

std::vector<std::pair<std::string, std::function<void(runtime_config)>>> cases() {
    return {
            {"call_every_core_with_payloads",
             [](runtime_config base) {
                 constexpr unsigned cores = 4;
                 constexpr int calls = 2000;
                 runtime rt(with_cores(base, cores));
                 std::vector<std::uint64_t> served(cores, 0); // each entry touched by one core only
                 std::atomic<int> verified{0};
                 rt.run([&](reactor& r) -> task<void> {
                     std::mt19937 rng(r.core() + 1);
                     for (int i = 0; i < calls; ++i) {
                         const unsigned target = (r.core() + 1 + rng() % (cores - 1)) % cores;
                         // A heap payload built on this core, read on the target core.
                         auto payload = std::make_shared<std::vector<std::uint32_t>>(1 + rng() % 64);
                         for (auto& x : *payload) x = static_cast<std::uint32_t>(rng());
                         const auto expect_sum =
                             std::accumulate(payload->begin(), payload->end(), std::uint64_t{0});
                         const auto got = co_await r.call(target, [payload, &served](reactor& t) {
                             ++served[t.core()];
                             return std::pair{t.core(), std::accumulate(payload->begin(),
                                                                        payload->end(),
                                                                        std::uint64_t{0})};
                         });
                         CHECK(got.first == target);
                         CHECK(got.second == expect_sum);
                         verified.fetch_add(1);
                     }
                 });
                 CHECK(verified.load() == static_cast<int>(cores) * calls);
                 CHECK(std::accumulate(served.begin(), served.end(), std::uint64_t{0}) ==
                       std::uint64_t{cores} * calls);
             }},
            {"posts_are_delivered_before_rings_close",
             [](runtime_config base) {
                 // Posts sent right before each root returns must still be run.
                 constexpr unsigned cores = 3;
                 constexpr int posts = 5000;
                 runtime rt(with_cores(base, cores));
                 // seen[target][sender * posts + i]: touched only by the target core.
                 std::vector<std::vector<std::uint8_t>> seen(
                     cores, std::vector<std::uint8_t>(cores * posts, 0));
                 rt.run([&](reactor& r) -> task<void> {
                     for (int i = 0; i < posts; ++i) {
                         const unsigned target = (r.core() + 1 + static_cast<unsigned>(i)) % cores;
                         const std::size_t id = r.core() * posts + static_cast<std::size_t>(i);
                         r.post(target, [&seen, id](reactor& t) { ++seen[t.core()][id]; });
                         if (i % 64 == 0) co_await r.nop(); // let submissions flow
                     }
                 });
                 std::size_t once = 0, twice = 0;
                 for (unsigned c = 0; c < cores; ++c)
                     for (auto v : seen[c]) {
                         once += v == 1;
                         twice += v > 1;
                     }
                 REQUIRE("every posted message runs exactly once",
                         once == std::size_t{cores} * posts && twice == 0);
             }},
            {"ping_pong_token_ring",
             [](runtime_config base) {
                 constexpr unsigned cores = 4;
                 constexpr int laps = 500;
                 runtime rt(with_cores(base, cores));
                 std::atomic<int> finished{0};
                 struct token {
                     int hops = 0;
                     std::vector<int> trail; // appended on whichever core holds the token
                 };
                 rt.run([&](reactor& r) -> task<void> {
                     if (r.core() != 0) co_return;
                     auto tok = std::make_unique<token>();
                     for (int lap = 0; lap < laps; ++lap) {
                         for (unsigned c = 1; c <= cores; ++c) {
                             const unsigned target = c % cores;
                             token* raw = tok.get();
                             co_await r.call(target, [raw](reactor& t) {
                                 raw->trail.push_back(static_cast<int>(t.core()));
                                 ++raw->hops;
                             });
                         }
                     }
                     CHECK(tok->hops == laps * static_cast<int>(cores));
                     for (std::size_t i = 0; i < tok->trail.size(); ++i)
                         CHECK(tok->trail[i] == static_cast<int>((i + 1) % cores));
                     finished.fetch_add(1);
                 });
                 CHECK(finished.load() == 1);
             }},
            {"shutdown_with_calls_in_flight",
             [](runtime_config base) {
                 // Spawned tasks keep calling other cores after their roots
                 // return. Shutdown refuses new messages, finishes the ones in
                 // flight and closes no ring while anything is outstanding.
                 constexpr unsigned cores = 4;
                 runtime rt(with_cores(base, cores));
                 std::atomic<long> answered{0}, refused{0};
                 rt.run([&](reactor& r) -> task<void> {
                     for (int k = 0; k < 8; ++k)
                         r.spawn([](reactor& re, std::atomic<long>& ans,
                                    std::atomic<long>& ref) -> task<void> {
                             for (unsigned i = 0;; ++i) {
                                 try {
                                     const unsigned target = (re.core() + 1 + i) % re.cores();
                                     const auto v = co_await re.call(target, [](reactor& t) {
                                         return t.core();
                                     });
                                     CHECK(v == target);
                                     ans.fetch_add(1);
                                 } catch (const Failure& f) {
                                     CHECK(f.code == Error::closed);
                                     ref.fetch_add(1);
                                     co_return;
                                 }
                             }
                         }(r, answered, refused));
                     co_await r.sleep(20ms);
                 });
                 CHECK(answered.load() > 0);
                 std::printf("calls answered %ld, refused after shutdown began %ld\n",
                             answered.load(), refused.load());
                 CHECK(rt.outstanding() == 0);
             }},
            {"concurrent_calls_share_message_rings",
             [](runtime_config base) {
                 // 64 calls started in one loop iteration: batched, they leave
                 // core 0 as one MSG_RING; unbatched, as 64.
                 runtime rt(with_cores(base, 2));
                 std::uint64_t rings = 0;
                 bool ok = true;
                 rt.run([&](reactor& r) -> task<void> {
                     if (r.core() != 0) co_return;
                     std::vector<task<int>> calls;
                     for (int i = 0; i < 64; ++i)
                         calls.push_back([](reactor& re, int v) -> task<int> {
                             co_return co_await re.call(1, [v](reactor& t) {
                                 return v * 2 + static_cast<int>(t.core());
                             });
                         }(r, i));
                     const auto before = r.stats().message_rings + r.stats().queue_pushes;
                     auto results = co_await when_all(std::move(calls));
                     rings = r.stats().message_rings + r.stats().queue_pushes - before;
                     for (int i = 0; i < 64; ++i) ok &= results[static_cast<std::size_t>(i)] == i * 2 + 1;
                 });
                 REQUIRE("call results arrive in order", ok);
                 std::printf("%s for 64 concurrent calls: %llu\n",
                             base.reactor.doorbell ? "queue pushes" : "MSG_RINGs",
                             static_cast<unsigned long long>(rings));
                 if (base.reactor.doorbell)
                     REQUIRE("concurrent calls to one core share one queue push", rings == 1);
                 else if (base.reactor.batch_messages)
                     REQUIRE("concurrent calls to one core share one MSG_RING", rings == 1);
                 else
                     REQUIRE("one MSG_RING per call when batching is off", rings == 64);
             }},
            {"sleeping_core_is_woken_for_every_message",
             [](runtime_config base) {
                 // Core 0 posts a job that keeps core 1 busy for a random
                 // time, waits a random time, then calls core 1. The call
                 // often lands while core 1 is finishing the job and about to
                 // sleep: the window in which a doorbell transport can lose a
                 // wake-up. Nothing else wakes core 1, so a lost wake-up would
                 // stall the test; the watchdog turns that into a count.
                 constexpr int rounds = 4000;
                 runtime rt(with_cores(base, 2));
                 std::atomic<long> progress{0};
                 std::vector<std::atomic<int>> fds(2);
                 for (auto& f : fds) f.store(-1);
                 long stalls = 0;
                 std::uint64_t aborts = 0, doorbells = 0;
                 std::optional<event> stop; // core 1's; created and set on core 1
                 {
                     stall_watchdog dog(progress, fds, 1000ms);
                     rt.run([&](reactor& r) -> task<void> {
                         fds[r.core()].store(r.ring_fd());
                         if (r.core() != 0) {
                             // Stay inside block_on, whose waits have no
                             // timeout, until core 0 is done.
                             stop.emplace(r);
                             co_await stop->wait();
                             co_return;
                         }
                         std::mt19937 rng(7);
                         for (int i = 0; i < rounds && dog.stalls() == 0; ++i) {
                             const auto busy = std::chrono::nanoseconds(rng() % 3000);
                             const auto gap = std::chrono::nanoseconds(rng() % 6000);
                             r.post(1, [busy](reactor&) { spin_for(busy); });
                             co_await r.nop(); // the post leaves now
                             spin_for(gap);
                             const auto v = co_await r.call(1, [](reactor& t) { return t.core(); });
                             CHECK(v == 1);
                             progress.fetch_add(1);
                         }
                         aborts = co_await r.call(1, [](reactor& t) { return t.stats().wait_aborts; });
                         doorbells = r.stats().doorbells;
                         r.post(1, [&stop](reactor&) { stop->set(); });
                     });
                     stalls = dog.stalls();
                 }
                 std::printf("rounds %ld of %d, stalls %ld, core 1 waits aborted by the re-check %llu, "
                             "core 0 doorbells %llu\n",
                             progress.load(), rounds, stalls, static_cast<unsigned long long>(aborts),
                             static_cast<unsigned long long>(doorbells));
                 REQUIRE("a sleeping core is woken for every message", stalls == 0);
             }},
            {"root_exception_is_rethrown",
             [](runtime_config base) {
                 runtime rt(with_cores(base, 2));
                 bool caught = false;
                 try {
                     rt.run([](reactor& r) -> task<void> {
                         co_await r.nop();
                         if (r.core() == 1) throw std::runtime_error("core 1 failed");
                     });
                 } catch (const std::runtime_error& e) {
                     caught = std::string(e.what()) == "core 1 failed";
                 }
                 CHECK(caught);
             }},
    };
}
} // namespace

// A descriptor that is not an io_uring: a MSG_RING aimed at it is refused by
// the kernel with -EBADFD.
struct not_a_ring {
    int fd = ::eventfd(0, EFD_CLOEXEC);
    not_a_ring() { CHECK(fd >= 0); }
    not_a_ring(const not_a_ring&) = delete;
    not_a_ring& operator=(const not_a_ring&) = delete;
    ~not_a_ring() { ::close(fd); }
};

// Refused MSG_RINGs (the chain_failed path), with real kernel refusals.
std::vector<Case> refusal_cases() {
    return {
        {"refused_request_fails_the_call",
         [] {
             // Core 0's first MSG_RING to core 1 is refused: that call fails
             // with the kernel's error, the target never runs it, and the
             // next call goes through.
             not_a_ring bad;
             std::atomic<int> refuse{1};
             runtime_config c;
             c.cores = 2;
             c.reactor.message_fault = [&](unsigned target, bool) {
                 return target == 1 && refuse.fetch_sub(1) > 0 ? bad.fd : -1;
             };
             runtime rt(c);
             std::atomic<int> ran{0};
             int err = 0;
             bool second = false;
             std::uint64_t failed = 0;
             rt.run([&](reactor& r) -> task<void> {
                 if (r.core() != 0) co_return;
                 try {
                     (void)co_await r.call(1, [&ran](reactor&) { return ran.fetch_add(1); });
                 } catch (const Failure& f) {
                     if (f.code == Error::closed) err = f.sys_errno;
                 }
                 second = co_await r.call(1, [](reactor& t) { return t.core() == 1; });
                 failed = r.stats().messages_failed;
             });
             REQUIRE("a refused request fails its call with the kernel's error", err == EBADFD);
             REQUIRE("a refused request is not run on the target", ran.load() == 0);
             REQUIRE("the next call is delivered", second && failed == 1);
         }},
        {"refused_post_is_dropped_and_counted",
         [] {
             not_a_ring bad;
             std::atomic<int> refuse{1};
             runtime_config c;
             c.cores = 2;
             c.reactor.message_fault = [&](unsigned target, bool) {
                 return target == 1 && refuse.fetch_sub(1) > 0 ? bad.fd : -1;
             };
             runtime rt(c);
             std::atomic<int> ran{0};
             std::uint64_t failed = 0;
             rt.run([&](reactor& r) -> task<void> {
                 if (r.core() != 0) co_return;
                 r.post(1, [&ran](reactor&) { ran.fetch_add(1); });
                 co_await r.nop();
                 co_await r.sleep(5ms); // the failure CQE arrives
                 r.post(1, [&ran](reactor&) { ran.fetch_add(10); });
                 (void)co_await r.call(1, [](reactor&) { return 0; });
                 failed = r.stats().messages_failed;
             });
             REQUIRE("a refused post is dropped and counted", ran.load() == 10 && failed == 1);
             CHECK(rt.outstanding() == 0);
         }},
        {"refused_reply_is_sent_again",
         [] {
             // Core 1's first three replies to core 0 are refused. The
             // replies belong to core 0's call slots, so core 1 sends them
             // again; every call completes with its own result.
             not_a_ring bad;
             std::atomic<int> refuse{3};
             runtime_config c;
             c.cores = 2;
             c.reactor.message_fault = [&](unsigned target, bool) {
                 return target == 0 && refuse.fetch_sub(1) > 0 ? bad.fd : -1;
             };
             runtime rt(c);
             bool ok = true;
             std::uint64_t retries = 0;
             rt.run([&](reactor& r) -> task<void> {
                 if (r.core() != 0) co_return;
                 for (int i = 0; i < 20; ++i)
                     ok &= co_await r.call(1, [i](reactor&) { return i * 3; }) == i * 3;
                 retries = co_await r.call(1, [](reactor& t) { return t.stats().ring_retries; });
             });
             std::printf("replies sent again: %llu\n", static_cast<unsigned long long>(retries));
             REQUIRE("a refused reply still completes its call", ok && refuse.load() < 0);
             REQUIRE("refused replies are sent again", retries >= 3);
             CHECK(rt.outstanding() == 0);
         }},
        {"refused_doorbell_is_rung_again",
         [] {
             // Doorbell transport: the first doorbells to core 1 are refused
             // while core 1 sleeps. The sender rings again; the call
             // completes.
             not_a_ring bad;
             std::atomic<int> refuse{2};
             runtime_config c;
             c.cores = 2;
             c.reactor.doorbell = true;
             c.reactor.message_fault = [&](unsigned target, bool doorbell) {
                 return doorbell && target == 1 && refuse.fetch_sub(1) > 0 ? bad.fd : -1;
             };
             runtime rt(c);
             int v = 0;
             std::uint64_t retries = 0, rung = 0;
             std::optional<event> stop;
             rt.run([&](reactor& r) -> task<void> {
                 if (r.core() != 0) {
                     stop.emplace(r); // core 1 waits in block_on (no timeout)
                     co_await stop->wait();
                     co_return;
                 }
                 co_await r.sleep(20ms); // core 1 is asleep by now
                 v = co_await r.call(1, [](reactor&) { return 42; });
                 retries = r.stats().ring_retries;
                 rung = r.stats().doorbells;
                 r.post(1, [&stop](reactor&) { stop->set(); });
             });
             std::printf("doorbells %llu, rung again %llu\n", static_cast<unsigned long long>(rung),
                         static_cast<unsigned long long>(retries));
             REQUIRE("a refused doorbell is rung again", v == 42 && retries >= 2 && refuse.load() < 0);
         }},
    };
}

int main(int argc, char** argv) {
    // Every case runs with batched cross-core messages over MSG_RING (the
    // default), with one MSG_RING per message, with busy-polling reactors,
    // and over the doorbell transport, sleeping and busy-polling.
    std::vector<Case> all;
    for (auto& [name, fn] : cases()) {
        all.emplace_back(name, [fn] { fn(runtime_config{}); });
        all.emplace_back(name + "/unbatched", [fn] {
            runtime_config c;
            c.reactor.batch_messages = false;
            fn(c);
        });
        all.emplace_back(name + "/busy_poll", [fn] {
            runtime_config c;
            c.reactor.busy_poll = true;
            fn(c);
        });
        all.emplace_back(name + "/doorbell", [fn] {
            runtime_config c;
            c.reactor.doorbell = true;
            fn(c);
        });
        all.emplace_back(name + "/doorbell_busy_poll", [fn] {
            runtime_config c;
            c.reactor.doorbell = true;
            c.reactor.busy_poll = true;
            fn(c);
        });
    }
    for (auto& c : refusal_cases()) all.push_back(std::move(c));
    return run(all, argc, argv);
}
