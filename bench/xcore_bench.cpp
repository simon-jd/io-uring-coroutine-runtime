// Cross-core messaging study: why one-way posts to a DEFER_TASKRUN ring are
// slower, what batching MSG_RINGs per target per loop iteration changes, and
// what the doorbell transport (shared-memory queues, a MSG_RING only to wake a
// sleeping core) changes.
//
//   post_flood  core 0 posts n messages to core 1 (an await every 64 posts);
//               time until core 1 has run all of them
//   call_burst  32 coroutines on core 0 each make calls to core 1 in a loop
//               (the shape of a server forwarding requests); time per call
//   call_rtt    one coroutine making calls to core 1; round-trip time
//   post_flood is also run batched with post nodes freed on the target
//   (reactor_config::return_posts = false) to measure that choice.
//
// Each run reports core 1's (the target's) thread CPU time, context switches
// and io_uring_enter counts, measured on that thread with getrusage
// (RUSAGE_THREAD) and the reactor's own counters, plus core 0's MSG_RING count.
// Variants (ring flags x batching x idle mode) are interleaved per repetition.
// Output: one JSON object per line.
#include "rol/runtime.hpp"
#include "rol/when_all.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include <sys/resource.h>

using namespace rol;
using clock_type = std::chrono::steady_clock;

namespace {

constexpr unsigned defer_flags =
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL;
constexpr unsigned coop_flags =
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL;

struct variant {
    const char* ring;
    unsigned flags;
    bool batch;
    bool spin;
    bool return_posts = true;
    bool doorbell = false;
};

struct thread_usage {
    double cpu_ns = 0;
    long nvcsw = 0;
    long nivcsw = 0;
    static thread_usage now() {
        rusage u{};
        getrusage(RUSAGE_THREAD, &u);
        thread_usage t;
        t.cpu_ns = (static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1e9 +
                    static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e3);
        t.nvcsw = u.ru_nvcsw;
        t.nivcsw = u.ru_nivcsw;
        return t;
    }
};

struct measurement {
    double ns_per_op = 0;
    thread_usage target;       // deltas on core 1
    reactor_stats target_stats; // deltas on core 1 (enters, cqes)
    thread_usage sender;       // deltas on core 0
    std::uint64_t sender_rings = 0;
    std::uint64_t target_rings = 0;
    std::uint64_t sender_doorbells = 0;
    std::uint64_t target_doorbells = 0;
};

struct target_side {
    std::optional<event> stop;
};

reactor_config make_cfg(const variant& v) {
    reactor_config c;
    c.setup_flags = v.flags;
    c.batch_messages = v.batch;
    c.busy_poll = v.spin;
    c.return_posts = v.return_posts;
    c.doorbell = v.doorbell;
    return c;
}

// Runs `body` on core 0 and measures core 1 while it serves.
template <class Body>
measurement measure(const variant& v, const std::vector<int>& cpus, Body body) {
    runtime rt({.cores = 2, .cpus = cpus, .reactor = make_cfg(v)});
    measurement m;
    target_side ts;
    rt.run([&](reactor& r) -> task<void> {
        if (r.core() == 1) {
            ts.stop.emplace(r);
            const auto u0 = thread_usage::now();
            const auto s0 = r.stats();
            co_await ts.stop->wait();
            const auto u1 = thread_usage::now();
            const auto& s1 = r.stats();
            m.target.cpu_ns = u1.cpu_ns - u0.cpu_ns;
            m.target.nvcsw = u1.nvcsw - u0.nvcsw;
            m.target.nivcsw = u1.nivcsw - u0.nivcsw;
            m.target_stats.enters = s1.enters - s0.enters;
            m.target_stats.blocking_enters = s1.blocking_enters - s0.blocking_enters;
            m.target_stats.cqes = s1.cqes - s0.cqes;
            m.target_rings = s1.message_rings - s0.message_rings;
            m.target_doorbells = s1.doorbells - s0.doorbells;
            co_return;
        }
        const auto u0 = thread_usage::now();
        const auto rings0 = r.stats().message_rings;
        const auto bells0 = r.stats().doorbells;
        m.ns_per_op = co_await body(r);
        const auto u1 = thread_usage::now();
        m.sender.cpu_ns = u1.cpu_ns - u0.cpu_ns;
        m.sender.nvcsw = u1.nvcsw - u0.nvcsw;
        m.sender_rings = r.stats().message_rings - rings0;
        m.sender_doorbells = r.stats().doorbells - bells0;
        r.post(1, [&ts](reactor&) { ts.stop->set(); });
    });
    return m;
}

double ns_per(clock_type::duration d, long n) {
    return std::chrono::duration<double, std::nano>(d).count() / static_cast<double>(n);
}

task<double> post_flood(reactor& r, long n) {
    long received = 0; // touched only by core 1
    const auto t0 = clock_type::now();
    for (long i = 0; i < n; ++i) {
        r.post(1, [&received](reactor&) { ++received; });
        if ((i & 63) == 63) co_await r.nop();
    }
    for (;;) {
        const long seen = co_await r.call(1, [&received](reactor&) { return received; });
        if (seen == n) break;
    }
    co_return ns_per(clock_type::now() - t0, n);
}

task<double> call_burst(reactor& r, long n) {
    constexpr int workers = 32;
    std::vector<task<void>> ts;
    for (int w = 0; w < workers; ++w)
        ts.push_back([](reactor& re, long count) -> task<void> {
            for (long i = 0; i < count; ++i)
                (void)co_await re.call(1, [](reactor& t) { return t.core(); });
        }(r, n / workers));
    const auto t0 = clock_type::now();
    co_await when_all(std::move(ts));
    co_return ns_per(clock_type::now() - t0, n / workers * workers);
}

task<double> call_rtt(reactor& r, long n) {
    for (int i = 0; i < 1000; ++i) (void)co_await r.call(1, [](reactor&) { return 0; });
    const auto t0 = clock_type::now();
    for (long i = 0; i < n; ++i) (void)co_await r.call(1, [](reactor&) { return 0; });
    co_return ns_per(clock_type::now() - t0, n);
}

void emit(const char* bench, const variant& v, int rep, long n, const measurement& m) {
    const double per = static_cast<double>(n);
    std::printf(
        "{\"bench\":\"%s\",\"ring\":\"%s\",\"transport\":\"%s\",\"batch\":%s,"
        "\"return_posts\":%s,\"idle\":\"%s\","
        "\"rep\":%d,\"ops\":%ld,"
        "\"ns_per_op\":%.1f,\"target_cpu_ns_per_op\":%.1f,\"target_vcsw_per_1k\":%.2f,"
        "\"target_ivcsw_per_1k\":%.2f,\"target_enters_per_1k\":%.2f,"
        "\"target_blocking_enters_per_1k\":%.2f,\"target_cqes_per_1k\":%.2f,"
        "\"target_rings_per_1k\":%.2f,\"sender_cpu_ns_per_op\":%.1f,\"sender_rings_per_1k\":%.2f,"
        "\"sender_doorbells_per_1k\":%.2f,\"target_doorbells_per_1k\":%.2f}\n",
        bench, v.ring, v.doorbell ? "doorbell" : "msg_ring", v.batch ? "true" : "false",
        v.return_posts ? "true" : "false",
        v.spin ? "spin" : "sleep", rep, n, m.ns_per_op,
        m.target.cpu_ns / per, 1000.0 * static_cast<double>(m.target.nvcsw) / per,
        1000.0 * static_cast<double>(m.target.nivcsw) / per,
        1000.0 * static_cast<double>(m.target_stats.enters) / per,
        1000.0 * static_cast<double>(m.target_stats.blocking_enters) / per,
        1000.0 * static_cast<double>(m.target_stats.cqes) / per,
        1000.0 * static_cast<double>(m.target_rings) / per, m.sender.cpu_ns / per,
        1000.0 * static_cast<double>(m.sender_rings) / per,
        1000.0 * static_cast<double>(m.sender_doorbells) / per,
        1000.0 * static_cast<double>(m.target_doorbells) / per);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    int reps = 5;
    long n = 400000;
    std::vector<int> cpus;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--ops") && i + 1 < argc) n = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--cpus") && i + 1 < argc) {
            const char* p = argv[++i];
            cpus = {std::atoi(p)};
            if (const char* comma = std::strchr(p, ',')) cpus.push_back(std::atoi(comma + 1));
            if (cpus.size() != 2) return 2;
        }
    }
    std::vector<variant> variants;
    for (bool spin : {false, true}) {
        for (bool batch : {false, true}) {
            variants.push_back({"defer_taskrun", defer_flags, batch, spin});
            variants.push_back({"coop_taskrun", coop_flags, batch, spin});
        }
        variants.push_back({"defer_taskrun", defer_flags, true, spin, true, true});
        variants.push_back({"coop_taskrun", coop_flags, true, spin, true, true});
    }
    // Batched, but post nodes freed on the target (post_flood only).
    std::vector<variant> freed_on_target;
    for (bool spin : {false, true}) {
        freed_on_target.push_back({"defer_taskrun", defer_flags, true, spin, false});
        freed_on_target.push_back({"coop_taskrun", coop_flags, true, spin, false});
    }
    for (int rep = 0; rep < reps; ++rep) {
        for (const auto& v : variants) {
            emit("post_flood", v, rep, n,
                 measure(v, cpus, [n](reactor& r) { return post_flood(r, n); }));
            emit("call_burst", v, rep, n / 2,
                 measure(v, cpus, [n](reactor& r) { return call_burst(r, n / 2); }));
            emit("call_rtt", v, rep, n / 8,
                 measure(v, cpus, [n](reactor& r) { return call_rtt(r, n / 8); }));
        }
        for (const auto& v : freed_on_target)
            emit("post_flood", v, rep, n,
                 measure(v, cpus, [n](reactor& r) { return post_flood(r, n); }));
    }
    return 0;
}
