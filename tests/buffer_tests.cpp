// Fixed-buffer pool, provided-buffer rings and zero-copy send.
#include "test.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
using namespace rol;
using namespace std::chrono_literals;

namespace {
std::string_view as_text(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

task<void> drain_and_check(reactor& r, int fd, std::size_t total, char expect_byte, bool& intact,
                           std::size_t& got, event& done) {
    auto buf = owned_buffer::allocate(65536);
    while (got < total) {
        auto v = co_await r.recv(fd, std::move(buf));
        buf = std::move(v.buffer);
        if (v.res <= 0) break;
        for (auto b : buf.bytes())
            if (b != std::byte(expect_byte)) intact = false;
        got += static_cast<std::size_t>(v.res);
    }
    done.set();
}
} // namespace

int main(int argc, char** argv) {
    return run(
        {
            {"fixed_pool_fifo_waiters",
             [] {
                 reactor r({.fixed_buffers = 1, .fixed_buffer_size = 1024});
                 r.block_on([](reactor& re) -> task<void> {
                     auto& pool = re.fixed_buffers();
                     auto first = co_await pool.acquire();
                     CHECK(pool.available() == 0);
                     std::vector<int> order;
                     event done(re);
                     for (int i = 0; i < 3; ++i)
                         re.spawn([](fixed_buffer_pool& p, std::vector<int>& o, int id,
                                     event& d) -> task<void> {
                             auto l = co_await p.acquire();
                             o.push_back(id);
                             if (o.size() == 3) d.set();
                         }(pool, order, i, done));
                     co_await re.nop();
                     CHECK(order.empty());
                     first.reset(); // hands the buffer to waiter 0, then 1, then 2
                     co_await done.wait();
                     CHECK((order == std::vector<int>{0, 1, 2}));
                     CHECK(pool.available() == 1);
                 }(r));
             }},
            {"destroyed_waiter_returns_handed_lease",
             [] {
                 reactor r({.fixed_buffers = 1, .fixed_buffer_size = 1024});
                 r.block_on([](reactor& re) -> task<void> {
                     auto& pool = re.fixed_buffers();
                     auto held = *pool.try_acquire();
                     auto waiter = [](fixed_buffer_pool& p) -> eager { auto l = co_await p.acquire(); }(pool);
                     held.reset();      // lease handed to the waiter, which is queued to run
                     waiter.destroy();  // destroyed before it ran: its lease goes back
                     CHECK(pool.available() == 1);
                     co_await re.nop(); // the ready queue must not resume the dead frame
                 }(r));
             }},
            {"multishot_recv_with_explicit_return",
             [] {
                 socket_pair p(SOCK_SEQPACKET);
                 reactor r;
                 auto& ring = r.add_provided_ring(1, 4, 256);
                 r.block_on([](reactor& re, int a, int b, provided_buffer_ring& rg) -> task<void> {
                     {
                     auto st = re.recv_multishot(b, rg);
                     for (int i = 0; i < 10; ++i) {
                         const std::string msg = "message " + std::to_string(i);
                         write_all_fd(a, msg);
                         auto item = co_await st.next();
                         CHECK(item && item->more);
                         CHECK(item->res == static_cast<int>(msg.size()));
                         CHECK(as_text(item->buffer.data()) == msg);
                         CHECK(rg.held() == 1);
                         item->buffer.release(); // explicit return to the ring
                         CHECK(rg.held() == 0);
                     }
                     } // the stream's destructor cancels the multishot
                     while (re.stats().detached_released < 1) co_await re.nop();
                 }(r, p.fd[0], p.fd[1], ring));
                 CHECK(r.in_flight() == 0);
                 CHECK(ring.in_ring() == 4);
             }},
            {"enobufs_when_provided_ring_is_empty",
             [] {
                 socket_pair p(SOCK_SEQPACKET);
                 reactor r;
                 auto& ring = r.add_provided_ring(2, 2, 256);
                 write_all_fd(p.fd[0], "m0");
                 write_all_fd(p.fd[0], "m1");
                 write_all_fd(p.fd[0], "m2");
                 r.block_on([](reactor& re, int b, provided_buffer_ring& rg) -> task<void> {
                     auto st = re.recv_multishot(b, rg);
                     auto i0 = co_await st.next();
                     auto i1 = co_await st.next();
                     CHECK(i0 && as_text(i0->buffer.data()) == "m0" && i0->more);
                     CHECK(i1 && as_text(i1->buffer.data()) == "m1" && i1->more);
                     auto i2 = co_await st.next();
                     REQUIRE("empty ring ends the multishot with -ENOBUFS",
                             i2 && i2->res == -ENOBUFS && !i2->more && !i2->buffer);
                     CHECK(!(co_await st.next()));
                     CHECK(rg.in_ring() == 0);
                     i0->buffer.release();
                     i1->buffer.release();
                     CHECK(rg.in_ring() == 2);
                     {
                         auto again = re.recv_multishot(b, rg);
                         auto j = co_await again.next();
                         CHECK(j && as_text(j->buffer.data()) == "m2"); // nothing was lost
                     }
                     while (re.stats().detached_released < 1) co_await re.nop();
                 }(r, p.fd[1], ring));
                 CHECK(r.in_flight() == 0);
             }},
            {"destroyed_stream_returns_buffers",
             [] {
                 socket_pair p(SOCK_SEQPACKET);
                 reactor r;
                 auto& ring = r.add_provided_ring(3, 4, 256);
                 r.block_on([](reactor& re, int a, int b, provided_buffer_ring& rg) -> task<void> {
                     {
                         auto st = re.recv_multishot(b, rg);
                         write_all_fd(a, "one");
                         write_all_fd(a, "two");
                         co_await re.nop();
                         co_await re.nop();
                         CHECK(rg.held() == 2); // queued in the slot, not yet consumed
                     }
                     CHECK(rg.held() == 0); // returned when the stream was destroyed
                     while (re.in_flight() > 1) co_await re.nop();
                 }(r, p.fd[0], p.fd[1], ring));
                 CHECK(ring.in_ring() == 4);
             }},
            {"send_zc_buffer_held_until_notification",
             [] {
                 // Deliberate-bug target (b). The receiver's window is small, so
                 // most of the 256 KiB is still queued in the sender's socket
                 // (referencing our pages) when the first CQE arrives. Reusing
                 // the buffer then would change bytes that are not yet sent.
                 constexpr std::size_t total = 256 * 1024;
                 tcp_pair t(4096);
                 int snd = 1 << 20;
                 CHECK(::setsockopt(t.fd[0], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd)) == 0);
                 reactor r({.fixed_buffers = 1, .fixed_buffer_size = total});
                 bool intact = true;
                 std::size_t got = 0;
                 r.block_on([](reactor& re, int out, int in, bool& ok, std::size_t& n) -> task<void> {
                     auto& pool = re.fixed_buffers();
                     auto lease = *pool.try_acquire();
                     std::memset(lease.data().data(), 'A', total);
                     event done(re);
                     re.spawn(drain_and_check(re, in, total, 'A', ok, n, done));
                     auto res = co_await re.send_zc(out, std::move(lease), total);
                     CHECK(res.res == static_cast<int>(total));
                     CHECK(res.flags & IORING_CQE_F_MORE); // a notification will follow
                     // Reuse the buffer as soon as the pool hands it out.
                     auto again = co_await pool.acquire();
                     std::memset(again.data().data(), 'B', total);
                     co_await done.wait();
                 }(r, t.fd[0], t.fd[1], intact, got));
                 REQUIRE("zero-copy payload arrives unmodified", intact && got == total);
                 CHECK(r.stats().zc_notifications == 1);
                 std::printf("SEND_ZC over loopback: %llu notification(s), %llu reported copied\n",
                             static_cast<unsigned long long>(r.stats().zc_notifications),
                             static_cast<unsigned long long>(r.stats().zc_copied));
             }},
            {"send_zc_owned_buffer",
             [] {
                 tcp_pair t;
                 reactor r;
                 r.block_on([](reactor& re, int out, int in) -> task<void> {
                     auto res = co_await re.send_zc(out, text("zero-copy hello"));
                     CHECK(res.res == 15);
                     auto v = co_await re.recv(in, owned_buffer::allocate(64));
                     CHECK(v.buffer.view() == "zero-copy hello");
                     while (re.stats().zc_notifications == 0) co_await re.nop();
                 }(r, t.fd[0], t.fd[1]));
                 CHECK(r.in_flight() == 0);
             }},
            {"shutdown_waits_for_zc_notification",
             [] {
                 constexpr std::size_t total = 256 * 1024;
                 tcp_pair t(4096);
                 int snd = 1 << 20;
                 CHECK(::setsockopt(t.fd[0], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd)) == 0);
                 std::chrono::steady_clock::duration took{};
                 {
                     reactor r({.fixed_buffers = 1, .fixed_buffer_size = total});
                     r.block_on([](reactor& re, int out) -> task<void> {
                         auto res = co_await re.send_zc(out, *re.fixed_buffers().try_acquire(), total);
                         CHECK(res.res == static_cast<int>(total));
                     }(r, t.fd[0]));
                     CHECK(r.in_flight() == 1); // waiting for the notification
                     std::thread reader([fd = t.fd[1]] {
                         std::this_thread::sleep_for(50ms);
                         std::vector<char> buf(65536);
                         std::size_t n = 0;
                         while (n < total) {
                             ssize_t k = ::read(fd, buf.data(), buf.size());
                             if (k <= 0) break;
                             n += static_cast<std::size_t>(k);
                         }
                     });
                     const auto t0 = std::chrono::steady_clock::now();
                     r.shutdown();
                     took = std::chrono::steady_clock::now() - t0;
                     reader.join();
                     CHECK(r.stats().zc_notifications == 1);
                     CHECK(r.stats().abandoned == 0);
                     CHECK(r.stats().leases_at_close == 0);
                 }
                 CHECK(took >= 40ms); // shutdown waited for the peer to drain
             }},
            {"shutdown_timeout_abandons_instead_of_freeing",
             [] {
                 constexpr std::size_t total = 256 * 1024;
                 tcp_pair t(4096);
                 int snd = 1 << 20;
                 CHECK(::setsockopt(t.fd[0], SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd)) == 0);
                 reactor r({.fixed_buffers = 1, .fixed_buffer_size = total,
                            .shutdown_timeout = std::chrono::milliseconds(100)});
                 r.block_on([](reactor& re, int out) -> task<void> {
                     auto res = co_await re.send_zc(out, *re.fixed_buffers().try_acquire(), total);
                     CHECK(res.res == static_cast<int>(total));
                 }(r, t.fd[0]));
                 r.shutdown(); // the peer never reads: the notification cannot arrive
                 CHECK(r.stats().abandoned == 1);
                 CHECK(r.stats().leases_at_close == 1); // the slot's lease, kept, not freed
             }},
        },
        argc, argv);
}
