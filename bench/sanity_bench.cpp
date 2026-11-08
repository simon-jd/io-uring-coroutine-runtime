// Quick performance sanity checks (not the performance study):
//   nop_rtt     one coroutine awaiting IORING_OP_NOP in a loop
//   nop_batch   32 coroutines doing the same concurrently
//   call_rtt    reactor::call round trip between two cores (two MSG_RINGs)
//   post_flood  one core posting to another; time until all are run
// Ring setup variants are interleaved within one process run.
// Output: one JSON object per line.
#include "rol/runtime.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace rol;
using clock_type = std::chrono::steady_clock;

namespace {
struct variant {
    const char* name;
    unsigned flags;
};
const variant variants[] = {
    {"defer_taskrun", IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL},
    {"coop_taskrun", IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL},
    {"plain", IORING_SETUP_SUBMIT_ALL},
};

double ns_per(clock_type::duration d, long n) {
    return std::chrono::duration<double, std::nano>(d).count() / static_cast<double>(n);
}

void emit(const char* bench, const char* var, int rep, long n, double ns) {
    std::printf("{\"bench\":\"%s\",\"variant\":\"%s\",\"rep\":%d,\"ops\":%ld,\"ns_per_op\":%.1f}\n",
                bench, var, rep, n, ns);
    std::fflush(stdout);
}

double nop_rtt(unsigned flags, long n) {
    reactor r({.setup_flags = flags});
    return r.block_on([](reactor& re, long count) -> task<double> {
        for (int i = 0; i < 1000; ++i) co_await re.nop();
        const auto t0 = clock_type::now();
        for (long i = 0; i < count; ++i) co_await re.nop();
        co_return ns_per(clock_type::now() - t0, count);
    }(r, n));
}

double nop_batch(unsigned flags, long n) {
    reactor r({.setup_flags = flags});
    constexpr int workers = 32;
    event done(r);
    int left = workers;
    const auto t0 = clock_type::now();
    for (int w = 0; w < workers; ++w)
        r.spawn([](reactor& re, long count, int& l, event& d) -> task<void> {
            for (long i = 0; i < count; ++i) co_await re.nop();
            if (--l == 0) d.set();
        }(r, n / workers, left, done));
    r.block_on([](event& d) -> task<void> { co_await d.wait(); }(done));
    return ns_per(clock_type::now() - t0, n / workers * workers);
}

double call_rtt(unsigned flags, long n, const std::vector<int>& cpus) {
    runtime rt({.cores = 2, .cpus = cpus, .reactor = {.setup_flags = flags}});
    double result = 0;
    rt.run([&](reactor& r) -> task<void> {
        if (r.core() != 0) co_return;
        for (int i = 0; i < 1000; ++i) (void)co_await r.call(1, [](reactor&) { return 0; });
        const auto t0 = clock_type::now();
        for (long i = 0; i < n; ++i) (void)co_await r.call(1, [](reactor&) { return 0; });
        result = ns_per(clock_type::now() - t0, n);
    });
    return result;
}

double post_flood(unsigned flags, long n, const std::vector<int>& cpus) {
    runtime rt({.cores = 2, .cpus = cpus, .reactor = {.setup_flags = flags}});
    long received = 0; // touched only by core 1
    double result = 0;
    rt.run([&](reactor& r) -> task<void> {
        if (r.core() != 0) co_return;
        const auto t0 = clock_type::now();
        for (long i = 0; i < n; ++i) {
            r.post(1, [&received](reactor&) { ++received; });
            if ((i & 63) == 63) co_await r.nop();
        }
        // Wait until core 1 has run every post.
        for (;;) {
            const long seen = co_await r.call(1, [&received](reactor&) { return received; });
            if (seen == n) break;
        }
        result = ns_per(clock_type::now() - t0, n);
    });
    return result;
}
} // namespace

int main(int argc, char** argv) {
    int reps = 5;
    long n = 200000;
    std::vector<int> cpus; // --cpus a,b pins the two runtime cores
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
    for (int rep = 0; rep < reps; ++rep) {
        for (const auto& v : variants) {
            emit("nop_rtt", v.name, rep, n, nop_rtt(v.flags, n));
            emit("nop_batch", v.name, rep, n, nop_batch(v.flags, n));
            emit("call_rtt", v.name, rep, n / 4, call_rtt(v.flags, n / 4, cpus));
            emit("post_flood", v.name, rep, n, post_flood(v.flags, n, cpus));
        }
    }
    return 0;
}
