// task<T>, spawn, block_on and event.
#include "test.hpp"
#include <chrono>
using namespace rol;
using namespace std::chrono_literals;

namespace {
task<int> value(int v) { co_return v; }
task<int> add(int a, int b) { co_return co_await value(a) + co_await value(b); }
task<void> thrower() {
    throw std::runtime_error("boom");
    co_return;
}
task<int> deep(int n) {
    if (n == 0) co_return 0;
    co_return 1 + co_await deep(n - 1);
}
struct tracked {
    int* live;
    explicit tracked(int* l) : live(l) { ++*live; }
    tracked(const tracked&) = delete;
    ~tracked() { --*live; }
};
task<void> holds(int* live, event& e) {
    tracked t(live);
    co_await e.wait();
}
} // namespace

int main(int argc, char** argv) {
    return run(
        {
            {"value_and_nesting",
             [] {
                 reactor r;
                 CHECK(r.block_on(add(2, 3)) == 5);
                 CHECK(r.block_on(deep(1000)) == 1000);
             }},
            {"exception_propagates",
             [] {
                 reactor r;
                 bool caught = false;
                 try {
                     r.block_on(thrower());
                 } catch (const std::runtime_error& e) {
                     caught = std::string(e.what()) == "boom";
                 }
                 CHECK(caught);
             }},
            {"unstarted_task_is_destroyed",
             [] {
                 int live = 0;
                 reactor r;
                 event e(r);
                 {
                     auto t = holds(&live, e); // lazy: never started
                     CHECK(live == 0);
                 }
                 CHECK(live == 0);
             }},
            {"spawn_and_event",
             [] {
                 reactor r;
                 event e(r);
                 int order = 0, a = 0, b = 0;
                 r.spawn([](event& ev, int& ord, int& slot) -> task<void> {
                     co_await ev.wait();
                     slot = ++ord;
                 }(e, order, a));
                 r.spawn([](event& ev, int& ord, int& slot) -> task<void> {
                     co_await ev.wait();
                     slot = ++ord;
                 }(e, order, b));
                 r.block_on([](reactor& re, event& ev) -> task<void> {
                     co_await re.nop();
                     ev.set();
                     co_await re.nop();
                 }(r, e));
                 CHECK(a > 0 && b > 0 && a != b);
                 CHECK(r.stats().tasks == 0);
             }},
            {"spawned_exception_fails_block_on",
             [] {
                 reactor r;
                 r.spawn(thrower());
                 bool caught = false;
                 try {
                     r.block_on([](reactor& re) -> task<void> {
                         co_await re.sleep(50ms);
                     }(r));
                 } catch (const std::runtime_error&) {
                     caught = true;
                 }
                 CHECK(caught);
             }},
            {"deadlock_is_reported",
             [] {
                 reactor r;
                 event e(r);
                 expect(Error::invalid, [&] { r.block_on([](event& ev) -> task<void> {
                                                  co_await ev.wait();
                                              }(e)); });
             }},
            {"shutdown_destroys_blocked_tasks",
             [] {
                 int live = 0;
                 {
                     reactor r;
                     event never(r);
                     r.spawn(holds(&live, never));
                     r.spawn(holds(&live, never));
                     r.block_on([](reactor& re) -> task<void> { co_await re.nop(); }(r));
                     CHECK(live == 2);
                     r.shutdown();
                     CHECK(live == 0);
                     CHECK(r.stats().tasks == 0);
                     expect(Error::closed, [&] { r.spawn([]() -> task<void> { co_return; }()); });
                 }
                 CHECK(live == 0);
             }},
        },
        argc, argv);
}
