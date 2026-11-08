// Operation lifetime, cancellation and shutdown: the ownership contract.
#include "test.hpp"
#include "rol/when_all.hpp"
#include <fcntl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <sys/socket.h>
using namespace rol;
using namespace std::chrono_literals;

namespace {

// A plain ring: no SINGLE_ISSUER/DEFER_TASKRUN. Task work then runs on any
// return to user space, so a completion can land between two lines of the
// test (here: inside a write(2) call), after a coroutine was destroyed and
// before the runtime's cancel reaches the kernel.
reactor_config plain_ring(reactor_config c = {}) {
    c.setup_flags = 0;
    return c;
}

bool all_bytes(std::span<const std::byte> b, char c) {
    return std::all_of(b.begin(), b.end(), [c](std::byte x) { return x == std::byte(c); });
}

eager recv_into(reactor& r, int fd, bool& resumed) {
    auto res = co_await r.recv(fd, owned_buffer::allocate(4096));
    (void)res;
    resumed = true; // must never run: the frame is destroyed first
}

eager read_fixed_into(reactor& r, int fd, fixed_lease lease, cancel_source* src, lease_result& out,
                      bool& done) {
    const auto len = lease.size();
    out = co_await r.read_fixed(fd, std::move(lease), len, {.cancel = src});
    done = true;
}

task<void> wait_released(reactor& r, std::uint64_t target) {
    while (r.stats().detached_released < target) co_await r.nop();
}

} // namespace

int main(int argc, char** argv) {
    return run(
        {
            {"nop_sleep_and_ring_flags",
             [] {
                 reactor r;
                 CHECK(r.setup_flags() & IORING_SETUP_SINGLE_ISSUER);
                 CHECK(r.setup_flags() & IORING_SETUP_DEFER_TASKRUN);
                 r.block_on([](reactor& re) -> task<void> {
                     CHECK((co_await re.nop()).res == 0);
                     const auto t0 = std::chrono::steady_clock::now();
                     CHECK((co_await re.sleep(20ms)).res == -ETIME);
                     CHECK(std::chrono::steady_clock::now() - t0 >= 19ms);
                 }(r));
                 CHECK(r.in_flight() == 0);
             }},
            {"send_recv_hand_buffers_back",
             [] {
                 socket_pair p;
                 reactor r;
                 r.block_on([](reactor& re, int a, int b) -> task<void> {
                     auto s = co_await re.send(a, text("hello"));
                     CHECK(s.res == 5);
                     CHECK(s.buffer.view() == "hello");
                     auto v = co_await re.recv(b, owned_buffer::allocate(64));
                     CHECK(v.res == 5);
                     CHECK(v.buffer.view() == "hello");
                     CHECK(v.buffer.capacity() == 64);
                 }(r, p.fd[0], p.fd[1]));
                 CHECK(r.in_flight() == 0);
             }},
            {"linked_timeout",
             [] {
                 socket_pair p;
                 reactor r;
                 r.block_on([](reactor& re, int a, int b) -> task<void> {
                     auto v = co_await re.recv(b, owned_buffer::allocate(64), {.timeout = 20ms});
                     CHECK(v.res == -ETIME);
                     CHECK(v.buffer.capacity() == 64); // buffer comes back on timeout too
                     write_all_fd(a, "x");
                     v = co_await re.recv(b, std::move(v.buffer), {.timeout = 5s});
                     CHECK(v.res == 1);
                 }(r, p.fd[0], p.fd[1]));
                 CHECK(r.in_flight() == 0);
             }},
            {"destroyed_coroutine_recv_in_flight",
             [] {
                 // Destroy a coroutine whose recv is in flight, then send data.
                 // The slot keeps the buffer; no code touches the dead frame.
                 // Run on both ring types: with DEFER_TASKRUN the runtime's
                 // cancel wins; on a plain ring the kernel completes the recv
                 // (writes the data) after the frame is gone.
                 for (bool plain : {false, true}) {
                     socket_pair p;
                     reactor r(plain ? plain_ring() : reactor_config{});
                     r.block_on([](reactor& re, int a, int b, bool plain_ring) -> task<void> {
                         bool resumed = false;
                         eager c = recv_into(re, b, resumed);
                         co_await re.nop(); // the recv is now armed in the kernel
                         CHECK(!c.done());
                         c.destroy();
                         CHECK(re.stats().detached == 1);
                         CHECK(re.in_flight() >= 1); // slot (and its cancel) still live
                         write_all_fd(a, "late data");
                         co_await wait_released(re, 1);
                         CHECK(!resumed);
                         if (plain_ring) {
                             REQUIRE("kernel completed the recv after the frame was destroyed",
                                     re.stats().cancel_not_found == 1);
                         } else {
                             CHECK(re.stats().cancel_ok == 1);
                         }
                     }(r, p.fd[0], p.fd[1], plain));
                     CHECK(r.in_flight() == 0);
                 }
             }},
            {"destroyed_coroutine_keeps_fixed_buffer_until_cqe",
             [] {
                 // Deliberate-bug target (c): releasing the lease when the
                 // awaiting coroutine is destroyed lets the kernel write into a
                 // buffer that a new owner already holds.
                 socket_pair p;
                 reactor r(plain_ring({.fixed_buffers = 1, .fixed_buffer_size = 4096}));
                 r.block_on([](reactor& re, int a, int b) -> task<void> {
                     auto& pool = re.fixed_buffers();
                     lease_result out;
                     bool done = false;
                     eager c = read_fixed_into(re, b, *pool.try_acquire(), nullptr, out, done);
                     co_await re.nop();
                     c.destroy();
                     auto early = pool.try_acquire(); // must be empty: the slot owns it
                     if (early) std::memset(early->data().data(), 'Z', early->size());
                     write_all_fd(a, "kernel-write"); // completes the read on return
                     co_await wait_released(re, 1);
                     if (early)
                         REQUIRE("a new lease holder's bytes are not overwritten by the kernel",
                                 all_bytes(early->data(), 'Z'));
                     REQUIRE("buffer not reusable while its operation is in flight", !early);
                     REQUIRE("kernel completed the read after the frame was destroyed",
                             re.stats().cancel_not_found == 1);
                     CHECK(pool.available() == 1);
                     CHECK(!done);
                 }(r, p.fd[0], p.fd[1]));
             }},
            {"cancel_ack_does_not_release_buffer",
             [] {
                 // Deliberate-bug target (a). The cancel acknowledgement can be
                 // reaped before the target's own CQE; in that window the
                 // buffer still belongs to the operation.
                 socket_pair p;
                 reactor r({.fixed_buffers = 1, .fixed_buffer_size = 4096});
                 int windows = 0;
                 r.block_on([](reactor& re, int b, int& win) -> task<void> {
                     auto& pool = re.fixed_buffers();
                     for (int i = 0; i < 100; ++i) {
                         cancel_source src(re);
                         lease_result out;
                         bool done = false;
                         auto lease = *pool.try_acquire();
                         const auto index = lease.index();
                         eager c = read_fixed_into(re, b, std::move(lease), &src, out, done);
                         co_await re.nop();
                         auto rep = co_await src.cancel();
                         CHECK(rep.requested == 1 && rep.ok == 1);
                         if (!done) {
                             ++win;
                             REQUIRE("buffer stays with the operation after cancel-ack",
                                     !pool.try_acquire().has_value());
                         }
                         while (!done) co_await re.nop();
                         REQUIRE("operation hands back its own lease",
                                 out.lease && out.lease.index() == index);
                         CHECK(out.res == -ECANCELED);
                     }
                 }(r, p.fd[1], windows));
                 // If the window never occurs the test proves nothing.
                 CHECK(windows > 0);
                 std::printf("cancel-ack before target CQE in %d/100 rounds\n", windows);
             }},
            {"cancel_vs_complete_race",
             [] {
                 // >= 10k rounds per ring type. A peer thread writes one byte at a
                 // random moment while the reactor cancels the recv at a random
                 // moment. Outcome is -ECANCELED or the byte; every byte is
                 // received exactly once and the lease always comes back.
                 constexpr int rounds = 10000;
                 for (bool plain : {false, true}) {
                     socket_pair p;
                     reactor_config cfg{.fixed_buffers = 1, .fixed_buffer_size = 64};
                     reactor r(plain ? plain_ring(cfg) : cfg);
                     std::atomic<int> go{-1};
                     std::atomic<bool> stop{false};
                     std::thread peer([&] {
                         std::mt19937 rng(7);
                         int last = -1;
                         while (!stop.load()) {
                             const int g = go.load();
                             if (g == last) continue;
                             last = g;
                             // Half the rounds write at once, half after up to
                             // ~100 us, so both outcomes stay frequent in fast
                             // and sanitizer builds alike.
                             const unsigned spin = (rng() & 1) ? rng() % 200000 : 0;
                             for (unsigned k = spin; k > 0; --k)
                                 std::atomic_signal_fence(std::memory_order_seq_cst);
                             const char byte = static_cast<char>(g & 0xff);
                             CHECK(::write(p.fd[0], &byte, 1) == 1);
                         }
                     });
                     int cancelled = 0, completed = 0;
                     try {
                         r.block_on([](reactor& re, int b, std::atomic<int>& g, int& nc,
                                       int& nd) -> task<void> {
                             auto& pool = re.fixed_buffers();
                             std::mt19937 rng(11);
                             auto lease = *pool.try_acquire();
                             const auto index = lease.index();
                             for (int i = 0; i < rounds; ++i) {
                                 cancel_source src(re);
                                 lease_result out;
                                 bool done = false;
                                 eager c = read_fixed_into(re, b, std::move(lease), &src, out, done);
                                 g.store(i);
                                 for (unsigned k = rng() % 4; k > 0; --k) co_await re.nop();
                                 (void)co_await src.cancel();
                                 while (!done) {
                                     c.rethrow();
                                     co_await re.nop();
                                 }
                                 REQUIRE("lease returned to the canceller's operation",
                                         out.lease && out.lease.index() == index);
                                 lease = std::move(out.lease);
                                 const char want = static_cast<char>(i & 0xff);
                                 if (out.res == 1) {
                                     ++nd;
                                     CHECK(static_cast<char>(lease.data()[0]) == want);
                                 } else {
                                     REQUIRE("cancelled read reports -ECANCELED",
                                             out.res == -ECANCELED);
                                     ++nc;
                                     bool got = false;
                                     eager d = read_fixed_into(re, b, std::move(lease), nullptr, out,
                                                               got);
                                     while (!got) {
                                         d.rethrow();
                                         co_await re.nop();
                                     }
                                     CHECK(out.res == 1);
                                     lease = std::move(out.lease);
                                     CHECK(static_cast<char>(lease.data()[0]) == want);
                                 }
                                 CHECK(pool.available() == 0);
                             }
                         }(r, p.fd[1], go, cancelled, completed));
                     } catch (...) {
                         stop = true;
                         peer.join();
                         throw;
                     }
                     stop = true;
                     peer.join();
                     std::printf("%s ring: %d cancelled, %d completed\n", plain ? "plain" : "defer",
                                 cancelled, completed);
                     CHECK(cancelled + completed == rounds);
                     CHECK(cancelled > 0 && completed > 0); // both sides of the race happened
                     CHECK(r.in_flight() == 0);
                 }
             }},
            {"cancel_before_submit_short_circuits",
             [] {
                 socket_pair p;
                 reactor r;
                 r.block_on([](reactor& re, int b) -> task<void> {
                     cancel_source src(re);
                     src.request();
                     auto v = co_await re.recv(b, owned_buffer::allocate(16), {.cancel = &src});
                     CHECK(v.res == -ECANCELED);
                     CHECK(v.buffer.capacity() == 16);
                     CHECK(re.stats().ops_short_circuited == 1);
                 }(r, p.fd[1]));
             }},
            {"short_writes_are_resumed",
             [] {
                 socket_pair p;
                 int small = 4096;
                 CHECK(::setsockopt(p.fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
                 CHECK(::setsockopt(p.fd[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
                 reactor r;
                 constexpr std::size_t total = 1 << 20;
                 auto payload = owned_buffer::allocate(total);
                 for (std::size_t i = 0; i < total; ++i)
                     payload.storage()[i] = std::byte(static_cast<unsigned char>(i * 31 + 7));
                 payload.resize(total);
                 bool reader_ok = true;
                 std::size_t received = 0;
                 event reader_done(r);
                 r.spawn([](reactor& re, int fd, bool& ok, std::size_t& got, event& ev) -> task<void> {
                     auto buf = owned_buffer::allocate(4096);
                     while (got < total) {
                         auto v = co_await re.recv(fd, std::move(buf));
                         buf = std::move(v.buffer);
                         if (v.res <= 0) break;
                         for (int i = 0; i < v.res; ++i, ++got)
                             if (buf.bytes()[static_cast<std::size_t>(i)] !=
                                 std::byte(static_cast<unsigned char>(got * 31 + 7)))
                                 ok = false;
                     }
                     ev.set();
                 }(r, p.fd[1], reader_ok, received, reader_done));
                 auto result = r.block_on([](reactor& re, int fd, owned_buffer b,
                                             event& ev) -> task<send_all_result> {
                     auto res = co_await re.send_all(fd, std::move(b));
                     co_await ev.wait();
                     co_return res;
                 }(r, p.fd[0], std::move(payload), reader_done));
                 CHECK(result.res == 0);
                 CHECK(result.sent == total);
                 CHECK(result.buffer.size() == total);
                 REQUIRE("short writes happened", result.submissions > 1);
                 CHECK(reader_ok && received == total);
                 std::printf("1 MiB over a 4 KiB socket buffer: %u send submissions\n",
                             result.submissions);
             }},
            {"peer_reset",
             [] {
                 tcp_pair t;
                 reactor r;
                 r.block_on([](reactor& re, int ours, int peer) -> task<void> {
                     buffer_result got;
                     event ev(re);
                     re.spawn([](reactor& r2, int fd, buffer_result& out, event& e) -> task<void> {
                         out = co_await r2.recv(fd, owned_buffer::allocate(128));
                         e.set();
                     }(re, ours, got, ev));
                     co_await re.nop();
                     linger lg{1, 0}; // close with RST
                     CHECK(::setsockopt(peer, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
                     ::close(peer);
                     co_await ev.wait();
                     CHECK(got.res == -ECONNRESET);
                     CHECK(got.buffer.capacity() == 128);
                     auto s = co_await re.send(ours, text("after reset"));
                     CHECK(s.res == -EPIPE || s.res == -ECONNRESET);
                     CHECK(s.buffer.view() == "after reset");
                 }(r, t.fd[0], t.fd[1]));
                 t.fd[1] = -1;
             }},
            {"shutdown_with_ops_in_flight",
             [] {
                 const auto baseline = fd_count();
                 {
                     socket_pair p1, p2, p3, p4(SOCK_SEQPACKET);
                     reactor r({.fixed_buffers = 4, .fixed_buffer_size = 4096});
                     auto& ring = r.add_provided_ring(3, 8, 512);
                     int live = 0;
                     struct marker {
                         int* n;
                         explicit marker(int* c) : n(c) { ++*n; }
                         marker(const marker&) = delete;
                         ~marker() { --*n; }
                     };
                     // A task that ignores cancellation and keeps retrying.
                     r.spawn([](reactor& re, int fd, int* l) -> task<void> {
                         marker m(l);
                         for (;;) {
                             auto v = co_await re.recv(fd, owned_buffer::allocate(64));
                             if (v.res > 0) continue;
                             if (re.closing()) co_return;
                         }
                     }(r, p1.fd[1], &live));
                     r.spawn([](reactor& re, int fd, int* l) -> task<void> {
                         marker m(l);
                         auto v = co_await re.read_fixed(fd, *re.fixed_buffers().try_acquire(), 4096);
                         (void)v;
                     }(r, p2.fd[1], &live));
                     r.spawn([](reactor& re, int fd, provided_buffer_ring& rg, int* l) -> task<void> {
                         marker m(l);
                         auto st = re.recv_multishot(fd, rg);
                         while (auto item = co_await st.next()) {
                         }
                     }(r, p3.fd[1], ring, &live));
                     r.spawn([](reactor& re, int fd, int* l) -> task<void> {
                         marker m(l);
                         auto v = co_await re.recvmsg(fd, owned_buffer::allocate(64));
                         (void)v;
                     }(r, p4.fd[1], &live));
                     r.spawn([](reactor& re, int* l) -> task<void> {
                         marker m(l);
                         (void)co_await re.sleep(std::chrono::hours(1));
                     }(r, &live));
                     r.block_on([](reactor& re) -> task<void> {
                         co_await re.nop();
                         co_await re.nop();
                     }(r));
                     CHECK(live == 5);
                     CHECK(r.in_flight() == 5);
                     CHECK(r.fixed_buffers().available() == 3);
                     CHECK(ring.in_ring() == 8);
                     r.shutdown();
                     CHECK(live == 0);
                     CHECK(r.in_flight() == 0);
                     CHECK(r.stats().abandoned == 0);
                     CHECK(r.stats().leases_at_close == 0);
                     CHECK(r.stats().provided_held_at_close == 0);
                     CHECK(r.stats().tasks == 0);
                 }
                 CHECK(fd_count() == baseline);
             }},
            {"read_op_reads_a_pipe",
             [] {
                 int fds[2];
                 CHECK(::pipe2(fds, O_CLOEXEC) == 0);
                 reactor r;
                 r.block_on([](reactor& re, int rd, int wr) -> task<void> {
                     write_all_fd(wr, "pipe bytes");
                     auto v = co_await re.read(rd, owned_buffer::allocate(64));
                     CHECK(v.res == 10);
                     CHECK(v.buffer.view() == "pipe bytes");
                 }(r, fds[0], fds[1]));
                 ::close(fds[0]);
                 ::close(fds[1]);
             }},
            {"when_all_returns_results_in_order",
             [] {
                 reactor r;
                 r.block_on([](reactor& re) -> task<void> {
                     std::vector<task<int>> ts;
                     for (int i = 0; i < 8; ++i)
                         ts.push_back([](reactor& x, int v) -> task<int> {
                             (void)co_await x.sleep(std::chrono::milliseconds(8 - v));
                             co_return v;
                         }(re, i));
                     auto out = co_await when_all(std::move(ts));
                     CHECK(out.size() == 8);
                     for (int i = 0; i < 8; ++i) CHECK(out[static_cast<std::size_t>(i)] == i);
                     auto none = co_await when_all(std::vector<task<int>>{});
                     CHECK(none.empty());
                 }(r));
                 CHECK(r.in_flight() == 0);
             }},
            {"when_all_rethrows_after_every_child_finished",
             [] {
                 reactor r;
                 int finished = 0;
                 r.block_on([](reactor& re, int& fin) -> task<void> {
                     std::vector<task<void>> ts;
                     ts.push_back([]() -> task<void> {
                         throw std::runtime_error("child failed");
                         co_return;
                     }());
                     for (int i = 0; i < 3; ++i)
                         ts.push_back([](reactor& x, int& f) -> task<void> {
                             (void)co_await x.sleep(5ms);
                             ++f;
                         }(re, fin));
                     bool caught = false;
                     try {
                         co_await when_all(std::move(ts));
                     } catch (const std::runtime_error& e) {
                         caught = std::string(e.what()) == "child failed";
                     }
                     CHECK(caught);
                 }(r, finished));
                 REQUIRE("siblings of a failed child run to completion", finished == 3);
             }},
            {"destroyed_when_all_detaches_children",
             [] {
                 // The awaiting frame is destroyed while two child receives are
                 // in flight: the children's slots keep their buffers until
                 // their CQEs (the detach cancels them).
                 socket_pair p1, p2;
                 reactor r;
                 r.block_on([](reactor& re, int f1, int f2) -> task<void> {
                     bool resumed = false;
                     auto parent = [](reactor& x, int a, int b, bool& res) -> eager {
                         std::vector<task<void>> ts;
                         for (int fd : {a, b})
                             ts.push_back([](reactor& y, int f) -> task<void> {
                                 auto v = co_await y.recv(f, owned_buffer::allocate(256));
                                 (void)v;
                             }(x, fd));
                         co_await when_all(std::move(ts));
                         res = true;
                     }(re, f1, f2, resumed);
                     co_await re.nop();
                     CHECK(re.in_flight() == 2);
                     parent.destroy();
                     CHECK(re.stats().detached == 2);
                     co_await wait_released(re, 2);
                     for (int i = 0; i < 100 && re.in_flight() > 0; ++i) co_await re.nop();
                     REQUIRE("detached child receives are released by their CQEs",
                             re.in_flight() == 0 && !resumed);
                 }(r, p1.fd[1], p2.fd[1]));
             }},
            {"direct_descriptors_carry_io",
             [] {
                 tcp_pair a;
                 reactor r({.direct_files = 4});
                 auto ring_ptr = &r.add_provided_ring(3, 8, 256);
                 r.block_on([](reactor& re, int local, int peer, provided_buffer_ring& ring) -> task<void> {
                     direct_fd d = re.adopt_direct(unique_fd(::fcntl(local, F_DUPFD_CLOEXEC, 0)));
                     CHECK(re.direct_state_of(d.index()) == reactor::direct_state::open);
                     auto s = co_await re.send(d, text("to peer"));
                     CHECK(s.res == 7);
                     char buf[16];
                     CHECK(::recv(peer, buf, sizeof(buf), 0) == 7);
                     write_all_fd(peer, "one");
                     auto v = co_await re.recv(d, owned_buffer::allocate(16));
                     CHECK(v.res == 3 && v.buffer.view() == "one");
                     auto st = re.recv_multishot(d, ring);
                     write_all_fd(peer, "two");
                     auto item = co_await st.next();
                     CHECK(item && item->res == 3);
                     CHECK(std::string_view(reinterpret_cast<const char*>(item->buffer.data().data()), 3) == "two");
                 }(r, a.fd[0], a.fd[1], *ring_ptr));
                 r.block_on([](reactor& re) -> task<void> {
                     for (int i = 0; i < 100 && re.in_flight() > 0; ++i) co_await re.nop();
                 }(r));
                 CHECK(r.in_flight() == 0);
                 CHECK(r.direct_state_of(0) == reactor::direct_state::free);
             }},
            {"stale_direct_descriptor_is_refused",
             [] {
                 // One table entry: closing A's descriptor frees index 0 and B
                 // gets it. A send through a kept reference to A must fail
                 // instead of reaching B's peer.
                 tcp_pair a, b;
                 reactor r({.direct_files = 1});
                 r.block_on([](reactor& re, int a0, int b0, int b1) -> task<void> {
                     direct_fd da = re.adopt_direct(unique_fd(::fcntl(a0, F_DUPFD_CLOEXEC, 0)));
                     const file_ref kept = da;
                     da.reset();
                     for (int i = 0; i < 100 && re.direct_state_of(0) != reactor::direct_state::free; ++i)
                         co_await re.nop();
                     CHECK(re.direct_state_of(0) == reactor::direct_state::free);
                     direct_fd db = re.adopt_direct(unique_fd(::fcntl(b0, F_DUPFD_CLOEXEC, 0)));
                     CHECK(db.index() == 0);
                     auto s = co_await re.send(kept, text("for A only"));
                     char buf[32];
                     const ssize_t got = ::recv(b1, buf, sizeof(buf), MSG_DONTWAIT);
                     REQUIRE("stale direct descriptor does not reach the new connection",
                             s.res == -EBADF && got < 0);
                 }(r, a.fd[0], b.fd[0], b.fd[1]));
             }},
            {"direct_index_reserved_while_operation_in_flight",
             [] {
                 // A receive names index 0 when its direct_fd is closed. The
                 // close completes, but the index is not handed out again until
                 // the receive's own CQE; the receive still gets its data.
                 tcp_pair a, b;
                 reactor r({.direct_files = 1});
                 r.block_on([](reactor& re, int a0, int a1, int b0) -> task<void> {
                     direct_fd da = re.adopt_direct(unique_fd(::fcntl(a0, F_DUPFD_CLOEXEC, 0)));
                     buffer_result got;
                     bool done = false;
                     auto receiver = [](reactor& x, file_ref f, buffer_result& out, bool& d) -> eager {
                         out = co_await x.recv(f, owned_buffer::allocate(64));
                         d = true;
                     }(re, da, got, done);
                     co_await re.nop();
                     CHECK(re.direct_pins(0) == 1);
                     da.reset();
                     for (int i = 0; i < 20; ++i) co_await re.nop(); // the close completes
                     bool refused = false;
                     try {
                         (void)re.adopt_direct(unique_fd(::fcntl(b0, F_DUPFD_CLOEXEC, 0)));
                     } catch (const Failure& f) {
                         refused = f.code == Error::full;
                     }
                     REQUIRE("an index stays reserved while an operation names it",
                             refused && re.direct_state_of(0) == reactor::direct_state::closing);
                     write_all_fd(a1, "late data");
                     while (!done) co_await re.nop();
                     REQUIRE("an operation started before the close completes normally",
                             got.res == 9 && got.buffer.view() == "late data");
                     CHECK(re.direct_state_of(0) == reactor::direct_state::free);
                     direct_fd db = re.adopt_direct(unique_fd(::fcntl(b0, F_DUPFD_CLOEXEC, 0)));
                     CHECK(db.index() == 0);
                 }(r, a.fd[0], a.fd[1], b.fd[0]));
             }},
            {"stale_and_foreign_leases_rejected",
             [] {
                 socket_pair p;
                 reactor r({.fixed_buffers = 2, .fixed_buffer_size = 4096});
                 reactor other({.fixed_buffers = 1, .fixed_buffer_size = 4096});
                 auto& pool = r.fixed_buffers();
                 auto a = *pool.try_acquire();
                 const auto h = a.handle();
                 CHECK(pool.view(h).size() == 4096);
                 a.reset();
                 expect(Error::stale, [&] { (void)pool.view(h); });
                 auto b = *pool.try_acquire();
                 auto c = *pool.try_acquire();
                 expect(Error::stale, [&] { (void)pool.view(h); }); // index reused, generation moved on
                 auto foreign = *other.fixed_buffers().try_acquire();
                 expect(Error::stale, [&] { (void)r.read_fixed(p.fd[1], std::move(foreign), 16); });
                 CHECK(other.fixed_buffers().available() == 1); // the rejected lease went home
                 r.block_on([](reactor& re, int a2, int b2, fixed_lease l) -> task<void> {
                     auto& pl = re.fixed_buffers();
                     const auto hb = l.handle();
                     lease_result out;
                     bool done = false;
                     eager e = read_fixed_into(re, b2, std::move(l), nullptr, out, done);
                     co_await re.nop();
                     expect(Error::busy, [&] { (void)pl.view(hb); }); // in flight: no access
                     write_all_fd(a2, "abc");
                     while (!done) co_await re.nop();
                     CHECK(out.res == 3);
                     CHECK(pl.view(out.lease.handle())[0] == std::byte('a'));
                 }(r, p.fd[0], p.fd[1], std::move(b)));
                 c.reset();
                 CHECK(pool.available() == 2);
             }},
        },
        argc, argv);
}
