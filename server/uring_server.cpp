// The key-value server on the io_uring runtime.
//
// Per core: one reactor, one shard, one provided-buffer ring. Core 0 accepts
// and hands each connection to a core round-robin (a post). A connection is one
// coroutine: multishot receive into provided buffers, parse in place, run this
// core's commands, send every other core one batch with reactor::call (all
// batched into one MSG_RING per target per loop iteration by the runtime), then
// send the replies in request order.
//
// Shutdown: core 0 reads an eventfd; on a stop request it tells every core to
// drain. Draining stops accepting, cancels every multishot receive, lets each
// connection answer what it already received, then closes it. After
// config::drain_ms, sends that are still blocked (peers not reading) are
// cancelled.
#include "kv/engine.hpp"
#include "rol/runtime.hpp"
#include "rol/when_all.hpp"
#include "server.hpp"
#include <bit>
#include <cerrno>
#include <cstring>
#include <thread>
#include <csignal>
#include <cstdio>
#include <sys/eventfd.h>
#include <unistd.h>

namespace kvs {
namespace {

using namespace rol;
using namespace std::chrono_literals;

struct core_state {
    kv::shard store;
    std::atomic<long>* connections = nullptr;
    provided_buffer_ring* ring = nullptr;
    cancel_source* recv_cancel = nullptr;
    cancel_source* hard_cancel = nullptr;
    cancel_source* accept_cancel = nullptr;
    cancel_source* watchdog_cancel = nullptr;
    event* drained = nullptr;
    long live = 0; // connections and core 0's accept/stop tasks
    bool draining = false;
    bool watchdog_running = false;
};

struct shared {
    config cfg;
    int listen_fd = -1;
    int stop_fd = -1;
    std::unique_ptr<std::atomic<long>[]> connection_counts;
    kv::server_info info;
    std::vector<std::unique_ptr<core_state>> cores;
};

void maybe_drained(core_state& cs) {
    if (!cs.draining || cs.live > 0) return;
    if (cs.watchdog_running) cs.watchdog_cancel->request();
    else cs.drained->set();
}

task<void> watchdog(reactor& r, shared& sh, core_state& cs) {
    const auto res =
        co_await r.sleep(std::chrono::milliseconds(sh.cfg.drain_ms), {.cancel = cs.watchdog_cancel});
    cs.watchdog_running = false;
    if (res.res == -ETIME) cs.hard_cancel->request(); // peers that did not read their replies
    maybe_drained(cs);
}

void begin_drain(reactor& r, shared& sh, core_state& cs) {
    if (cs.draining) return;
    cs.draining = true;
    if (cs.accept_cancel) cs.accept_cancel->request();
    cs.recv_cancel->request();
    cs.watchdog_running = true;
    r.spawn(watchdog(r, sh, cs));
    maybe_drained(cs);
}

task<std::unique_ptr<kv::batch>> forward_one(reactor& r, shared& sh, unsigned target,
                                             std::unique_ptr<kv::batch> b) {
    co_return co_await r.call(target, [b = std::move(b), &sh](reactor& t) mutable {
        b->execute(sh.cores[t.core()]->store);
        return std::move(b);
    });
}

task<void> connection(reactor& r, shared& sh, core_state& cs, int raw_fd) {
    unique_fd sock(raw_fd);
    const config& cfg = sh.cfg;
    // With --direct the socket moves into this core's registered file table
    // and every operation names the table entry (a full table falls back to
    // the plain descriptor).
    direct_fd direct;
    if (cfg.direct) {
        try {
            direct = r.adopt_direct(std::move(sock));
        } catch (const Failure&) {
        }
    }
    const file_ref fd = direct ? file_ref(direct) : file_ref(sock.get());
    kv::parser parser(cfg.limits);
    kv::command cmd;
    kv::plan plan(r.core(), cfg.cores, cs.store, sh.info);
    std::string in, out;
    owned_buffer sendbuf = owned_buffer::allocate(16 * 1024);
    const bool use_fixed = cfg.send == "fixed";
    bool open = true;
    bool starved = false;
    while (open) {
        if (starved) (void)co_await r.sleep(1ms, {.cancel = cs.recv_cancel});
        starved = false;
        auto stream = r.recv_multishot(fd, *cs.ring, {.cancel = cs.recv_cancel}, cfg.max_queued);
        bool rearm = false;
        while (open) {
            auto item = co_await stream.next();
            if (!item) break;
            const bool more = item->more;
            if (item->res > 0) {
                const std::string_view chunk(reinterpret_cast<const char*>(item->buffer.data().data()),
                                             static_cast<std::size_t>(item->res));
                // Parse straight from the provided buffer unless a partial
                // command is waiting for these bytes.
                const bool buffered = !in.empty();
                if (buffered) in.append(chunk);
                const std::string_view view = buffered ? std::string_view(in) : chunk;
                std::size_t pos = 0;
                plan.begin();
                const bool ok = kv::consume(parser, cmd, plan, view, pos);
                if (!ok || plan.close_after()) {
                    open = false;
                } else if (buffered) {
                    in.erase(0, pos);
                } else {
                    in.assign(view.substr(pos));
                }
                // Nothing refers to the buffer any more: this core's commands
                // have run and other cores' arguments were copied.
                item->buffer.release();
                if (plan.commands() == 0) {
                    if (!more) {
                        rearm = open;
                        break;
                    }
                    continue;
                }
                const auto& targets = plan.targets();
                if (targets.size() == 1) {
                    const unsigned t = targets[0];
                    auto b = co_await r.call(t, [b = plan.take(t), &sh](reactor& tr) mutable {
                        b->execute(sh.cores[tr.core()]->store);
                        return std::move(b);
                    });
                    plan.give_back(t, std::move(b));
                } else if (targets.size() > 1) {
                    std::vector<task<std::unique_ptr<kv::batch>>> calls;
                    calls.reserve(targets.size());
                    for (unsigned t : targets) calls.push_back(forward_one(r, sh, t, plan.take(t)));
                    auto back = co_await when_all(std::move(calls));
                    for (std::size_t i = 0; i < targets.size(); ++i)
                        plan.give_back(targets[i], std::move(back[i]));
                }
                if (plan.sleep_seconds() > 0)
                    (void)co_await r.sleep(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::duration<double>(plan.sleep_seconds())));
                out.clear();
                plan.assemble(out);
                bool sent = false;
                if (use_fixed && out.size() <= r.fixed_buffers().buffer_size()) {
                    if (auto lease = r.fixed_buffers().try_acquire()) {
                        std::memcpy(lease->data().data(), out.data(), out.size());
                        fixed_lease l = std::move(*lease);
                        std::size_t off = 0;
                        while (off < out.size()) {
                            auto w = co_await r.write_fixed(fd, std::move(l), out.size() - off,
                                                            {.cancel = cs.hard_cancel}, off);
                            l = std::move(w.lease);
                            if (w.res <= 0) {
                                open = false;
                                break;
                            }
                            off += static_cast<std::size_t>(w.res);
                        }
                        sent = true;
                    }
                }
                if (!sent) {
                    if (sendbuf.capacity() < out.size())
                        sendbuf = owned_buffer::allocate(std::bit_ceil(out.size()));
                    std::memcpy(sendbuf.storage().data(), out.data(), out.size());
                    sendbuf.resize(out.size());
                    std::size_t off = 0;
                    while (off < out.size()) {
                        auto s = co_await r.send(fd, std::move(sendbuf), off, {.cancel = cs.hard_cancel});
                        sendbuf = std::move(s.buffer);
                        if (s.res <= 0) {
                            open = false;
                            break;
                        }
                        off += static_cast<std::size_t>(s.res);
                    }
                    if (sendbuf.capacity() > (1u << 20)) sendbuf = owned_buffer::allocate(16 * 1024);
                }
            } else if (item->res == 0) {
                open = false; // peer closed
            } else if (item->res == -ENOBUFS) {
                starved = true; // the core's buffer ring ran dry; re-arm shortly
            } else if (item->res == -ECANCELED) {
                if (cs.draining) open = false; // otherwise paused by max_queued: re-arm
            } else {
                open = false; // connection error
            }
            if (!more) {
                rearm = open;
                break;
            }
        }
        if (!rearm) break;
    }
    direct.reset();
    sock.reset();
    cs.connections->fetch_sub(1, std::memory_order_relaxed);
    --cs.live;
    maybe_drained(cs);
}

void adopt(reactor& r, shared& sh, core_state& cs, int fd) {
    if (cs.draining) {
        ::close(fd);
        return;
    }
    ++cs.live;
    cs.connections->fetch_add(1, std::memory_order_relaxed);
    r.spawn(connection(r, sh, cs, fd));
}

task<void> accept_loop(reactor& r, shared& sh, core_state& cs) {
    unsigned next = 0;
    while (!cs.draining) {
        const auto a = co_await r.accept(sh.listen_fd, {.cancel = cs.accept_cancel});
        if (a.res < 0) {
            if (a.res == -ECANCELED || cs.draining) break;
            if (a.res == -EMFILE || a.res == -ENFILE || a.res == -ENOMEM)
                (void)co_await r.sleep(10ms, {.cancel = cs.accept_cancel});
            continue;
        }
        set_nodelay(a.res);
        const unsigned target = next++ % sh.cfg.cores;
        if (target == r.core()) adopt(r, sh, cs, a.res);
        else
            r.post(target, [&sh, fd = a.res](reactor& t) { adopt(t, sh, *sh.cores[t.core()], fd); });
    }
    --cs.live;
    maybe_drained(cs);
}

task<void> stop_watch(reactor& r, shared& sh, core_state& cs) {
    (void)co_await r.read(sh.stop_fd, owned_buffer::allocate(8), {.cancel = cs.accept_cancel});
    for (unsigned c = 0; c < sh.cfg.cores; ++c)
        if (c != r.core())
            r.post(c, [&sh](reactor& t) { begin_drain(t, sh, *sh.cores[t.core()]); });
    --cs.live;
    begin_drain(r, sh, cs);
}

task<void> core_root(reactor& r, shared& sh) {
    core_state& cs = *sh.cores[r.core()];
    cs.ring = &r.add_provided_ring(0, sh.cfg.recv_buffers, sh.cfg.recv_buffer_size);
    cancel_source recv_cancel(r), hard_cancel(r), accept_cancel(r), watchdog_cancel(r);
    event drained(r);
    cs.recv_cancel = &recv_cancel;
    cs.hard_cancel = &hard_cancel;
    cs.watchdog_cancel = &watchdog_cancel;
    cs.drained = &drained;
    if (r.core() == 0) {
        cs.accept_cancel = &accept_cancel;
        cs.live += 2;
        r.spawn(accept_loop(r, sh, cs));
        r.spawn(stop_watch(r, sh, cs));
    }
    co_await drained.wait();
    if (sh.cfg.stats) {
        const auto& s = r.stats();
        std::fprintf(stderr,
                     "stats core=%u io=uring enters=%llu blocking_enters=%llu sqes=%llu cqes=%llu "
                     "message_rings=%llu doorbells=%llu queue_pushes=%llu wait_aborts=%llu "
                     "messages_sent=%llu replies_sent=%llu multishot_paused=%llu\n",
                     r.core(), static_cast<unsigned long long>(s.enters),
                     static_cast<unsigned long long>(s.blocking_enters),
                     static_cast<unsigned long long>(s.sqes), static_cast<unsigned long long>(s.cqes),
                     static_cast<unsigned long long>(s.message_rings),
                     static_cast<unsigned long long>(s.doorbells),
                     static_cast<unsigned long long>(s.queue_pushes),
                     static_cast<unsigned long long>(s.wait_aborts),
                     static_cast<unsigned long long>(s.messages_sent),
                     static_cast<unsigned long long>(s.replies_sent),
                     static_cast<unsigned long long>(s.multishot_paused));
    }
    cs.recv_cancel = cs.hard_cancel = cs.accept_cancel = cs.watchdog_cancel = nullptr;
    cs.drained = nullptr;
}

class uring_frontend final : public frontend {
public:
    uring_frontend(const config& cfg, int listen_fd) {
        sh_.cfg = cfg;
        sh_.listen_fd = listen_fd;
        sh_.stop_fd = ::eventfd(0, EFD_CLOEXEC);
        if (sh_.stop_fd < 0) throw std::runtime_error("eventfd failed");
        sh_.connection_counts.reset(new std::atomic<long>[cfg.cores]);
        for (unsigned c = 0; c < cfg.cores; ++c) {
            sh_.connection_counts[c] = 0;
            sh_.cores.push_back(std::make_unique<core_state>());
            sh_.cores.back()->connections = &sh_.connection_counts[c];
        }
        sh_.info.io = "uring";
        sh_.info.cores = cfg.cores;
        sh_.info.connections = sh_.connection_counts.get();
        sh_.info.supports_sleep = true;

        runtime_config rc;
        rc.cores = cfg.cores;
        rc.cpus = cfg.cpus;
        rc.reactor.sq_entries = 512;
        rc.reactor.cq_entries = 8192;
        rc.reactor.setup_flags =
            cfg.ring == "coop"
                ? (IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL)
                : (IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL);
        rc.reactor.batch_messages = cfg.batch;
        rc.reactor.doorbell = cfg.xcore != "ring";
        rc.reactor.submit_before_announce = cfg.xcore != "doorbell-early";
        rc.reactor.busy_poll = cfg.busy_poll;
        if (cfg.direct) rc.reactor.direct_files = 4096;
        if (cfg.send == "fixed") {
            rc.reactor.fixed_buffers = 32;
            rc.reactor.fixed_buffer_size = 16 * 1024;
        }
        rt_ = std::make_unique<runtime>(rc);
        thread_ = std::thread([this] {
            try {
                rt_->run([this](reactor& r) { return core_root(r, sh_); });
            } catch (const std::exception& e) {
                std::fprintf(stderr, "uring server: %s\n", e.what());
                status_ = 1;
                ::kill(::getpid(), SIGTERM); // wake the main thread's sigwait
            }
        });
    }
    ~uring_frontend() override {
        if (thread_.joinable()) {
            stop();
            thread_.join();
        }
        if (sh_.stop_fd >= 0) ::close(sh_.stop_fd);
    }
    void stop() override {
        const std::uint64_t one = 1;
        (void)!::write(sh_.stop_fd, &one, sizeof(one));
    }
    int wait() override {
        if (thread_.joinable()) thread_.join();
        return status_;
    }

private:
    shared sh_;
    std::unique_ptr<runtime> rt_;
    std::thread thread_;
    int status_ = 0;
};

} // namespace

std::unique_ptr<frontend> start_uring(const config& cfg, int listen_fd) {
    return std::make_unique<uring_frontend>(cfg, listen_fd);
}

} // namespace kvs
