// The key-value server on an epoll readiness loop: the baseline that differs
// from the io_uring front end only in the I/O interface.
//
// Same parser, shard and routing plan; one thread per core; core 0 accepts and
// hands connections out round-robin; cross-core batches go through a per-core
// inbox (mutex + vector) woken by an eventfd, collected per target during a
// loop iteration and handed over once at its end (the same batching rule as
// the io_uring runtime's MSG_RING chains). Level-triggered epoll, non-blocking
// sockets, read(2)/send(2).
#include "kv/engine.hpp"
#include "server.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kvs {
namespace {

constexpr std::uint64_t listener_tag = ~0ull;
constexpr std::uint64_t inbox_tag = ~0ull - 1;
constexpr std::size_t read_chunk = 16 * 1024;

struct message {
    enum class kind : std::uint8_t { request, reply, adopt, drain, wake };
    explicit message(kind kind_) : k(kind_) {}
    kind k;
    unsigned from = 0;
    std::uint64_t conn = 0; // origin core's connection id (generation << 32 | index)
    std::unique_ptr<kv::batch> b;
    int fd = -1;
};

struct inbox {
    std::mutex m;
    std::vector<message> q;
    int efd = -1;
};

struct conn {
    conn(unsigned core, unsigned cores, kv::shard& s, const kv::server_info& info, const kv::limits& l)
        : parser(l), plan(core, cores, s, info) {}
    int fd = -1;
    std::uint32_t gen = 0;
    std::string in;
    std::string out;
    std::size_t out_off = 0;
    kv::parser parser;
    kv::command cmd;
    kv::plan plan;
    unsigned waiting = 0;    // batches still on other cores
    bool closing = false;    // close once the replies are written
    bool draining = false;   // no more reads; answer what is buffered, then close
    bool want_write = false; // registered for EPOLLOUT (and not EPOLLIN)
};

class epoll_frontend;

struct core {
    unsigned id = 0;
    int epfd = -1;
    inbox box;
    kv::shard store;
    std::vector<std::unique_ptr<conn>> conns;
    std::vector<std::uint32_t> gens;
    std::vector<std::uint32_t> free_slots;
    std::vector<std::vector<message>> outbox;
    std::vector<message> work;
    char rbuf[read_chunk]; // reads land here; parsed in place or appended to the connection
    long live = 0;
    bool draining = false;
    bool counted_drained = false;
    std::uint64_t waits = 0, wakeups_sent = 0, handoffs = 0, messages_sent = 0;
    std::chrono::steady_clock::time_point drain_deadline{};
};

class epoll_frontend final : public frontend {
public:
    epoll_frontend(const config& cfg, int listen_fd) : cfg_(cfg), listen_fd_(listen_fd) {
        counts_.reset(new std::atomic<long>[cfg.cores]);
        info_.io = "epoll";
        info_.cores = cfg.cores;
        info_.connections = counts_.get();
        info_.supports_sleep = false;
        for (unsigned c = 0; c < cfg.cores; ++c) {
            counts_[c] = 0;
            auto k = std::make_unique<core>();
            k->id = c;
            k->outbox.resize(cfg.cores);
            k->epfd = ::epoll_create1(EPOLL_CLOEXEC);
            k->box.efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (k->epfd < 0 || k->box.efd < 0) throw std::runtime_error("epoll/eventfd setup failed");
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u64 = inbox_tag;
            ::epoll_ctl(k->epfd, EPOLL_CTL_ADD, k->box.efd, &ev);
            cores_.push_back(std::move(k));
        }
        // Core 0 accepts: non-blocking, level-triggered.
        const int fl = ::fcntl(listen_fd_, F_GETFL);
        ::fcntl(listen_fd_, F_SETFL, fl | O_NONBLOCK);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u64 = listener_tag;
        ::epoll_ctl(cores_[0]->epfd, EPOLL_CTL_ADD, listen_fd_, &ev);
        for (unsigned c = 0; c < cfg.cores; ++c)
            threads_.emplace_back([this, c] {
                if (!cfg_.cpus.empty()) pin_thread(cfg_.cpus[c]);
                run(*cores_[c]);
            });
    }
    ~epoll_frontend() override {
        if (!threads_.empty()) {
            stop();
            wait();
        }
        for (auto& k : cores_) {
            for (auto& c : k->conns)
                if (c && c->fd >= 0) ::close(c->fd);
            ::close(k->epfd);
            ::close(k->box.efd);
        }
    }
    void stop() override {
        for (auto& k : cores_) deliver(*k, message{message::kind::drain});
    }
    int wait() override {
        for (auto& t : threads_) t.join();
        threads_.clear();
        return 0;
    }

private:
    void deliver(core& target, message m) {
        // Used outside the loop (stop, wake): not counted.
        bool was_empty;
        {
            std::lock_guard lock(target.box.m);
            was_empty = target.box.q.empty();
            target.box.q.push_back(std::move(m));
        }
        if (was_empty) wake(target);
    }
    static void wake(core& target) {
        const std::uint64_t one = 1;
        (void)!::write(target.box.efd, &one, sizeof(one));
    }
    void flush_outboxes(core& k) {
        for (unsigned t = 0; t < cfg_.cores; ++t) {
            auto& ob = k.outbox[t];
            if (ob.empty()) continue;
            core& target = *cores_[t];
            bool was_empty;
            {
                std::lock_guard lock(target.box.m);
                was_empty = target.box.q.empty();
                for (auto& m : ob) target.box.q.push_back(std::move(m));
            }
            ++k.handoffs;
            k.messages_sent += ob.size();
            ob.clear();
            if (was_empty) {
                wake(target);
                ++k.wakeups_sent;
            }
        }
    }

    static std::uint64_t id_of(const conn& c, std::uint32_t index) {
        return (std::uint64_t(c.gen) << 32) | index;
    }
    conn* find(core& k, std::uint64_t id) {
        const auto index = static_cast<std::uint32_t>(id);
        if (index >= k.conns.size() || !k.conns[index]) return nullptr;
        if (k.conns[index]->gen != static_cast<std::uint32_t>(id >> 32)) return nullptr;
        return k.conns[index].get();
    }

    void adopt(core& k, int fd) {
        if (k.draining) {
            ::close(fd);
            return;
        }
        std::uint32_t index;
        if (!k.free_slots.empty()) {
            index = k.free_slots.back();
            k.free_slots.pop_back();
        } else {
            index = static_cast<std::uint32_t>(k.conns.size());
            k.conns.emplace_back();
            k.gens.push_back(0);
        }
        auto c = std::make_unique<conn>(k.id, cfg_.cores, k.store, info_, cfg_.limits);
        c->fd = fd;
        c->gen = ++k.gens[index];
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u64 = id_of(*c, index);
        ::epoll_ctl(k.epfd, EPOLL_CTL_ADD, fd, &ev);
        k.conns[index] = std::move(c);
        ++k.live;
        counts_[k.id].fetch_add(1, std::memory_order_relaxed);
    }

    void close_conn(core& k, std::uint32_t index) {
        auto& c = k.conns[index];
        ::close(c->fd); // also removes it from the epoll set
        c.reset();      // batches still away are dropped when their reply arrives
        k.free_slots.push_back(index);
        --k.live;
        counts_[k.id].fetch_sub(1, std::memory_order_relaxed);
    }

    void set_events(core& k, conn& c, std::uint32_t index, std::uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.u64 = id_of(c, index);
        ::epoll_ctl(k.epfd, EPOLL_CTL_MOD, c.fd, &ev);
    }

    // Writes pending replies. false: blocked (EPOLLOUT armed) or closed.
    bool flush(core& k, conn& c, std::uint32_t index) {
        while (c.out_off < c.out.size()) {
            const ssize_t n =
                ::send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
            if (n > 0) {
                c.out_off += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) {
                if (!c.want_write) {
                    // Stop reading while the peer is not reading its replies.
                    set_events(k, c, index, EPOLLOUT);
                    c.want_write = true;
                }
                return false;
            }
            close_conn(k, index);
            return false;
        }
        c.out.clear();
        c.out_off = 0;
        if (c.want_write) {
            c.want_write = false;
            set_events(k, c, index, c.draining ? 0u : static_cast<std::uint32_t>(EPOLLIN));
        }
        return true;
    }

    // Parses `data` (the connection's buffer, or a fresh read when nothing is
    // buffered) and routes it: other cores' batches go to the outbox, local
    // replies to `out`. false when no complete command was there.
    bool route(core& k, std::uint32_t index, conn& c, std::string_view data, bool fresh) {
        std::size_t pos = 0;
        c.plan.begin();
        const bool ok = kv::consume(c.parser, c.cmd, c.plan, data, pos);
        if (!ok || c.plan.close_after()) c.closing = true;
        else if (fresh) c.in.assign(data.substr(pos));
        else c.in.erase(0, pos);
        if (c.plan.commands() == 0) return false;
        const auto& targets = c.plan.targets();
        if (!targets.empty()) {
            for (unsigned t : targets) {
                message m{message::kind::request};
                m.from = k.id;
                m.conn = id_of(c, index);
                m.b = c.plan.take(t);
                k.outbox[t].push_back(std::move(m));
            }
            c.waiting = static_cast<unsigned>(targets.size());
            return true;
        }
        c.plan.assemble(c.out);
        return true;
    }

    // Runs the connection until it waits for input, for other cores or for
    // the socket to drain.
    void drive(core& k, std::uint32_t index) {
        for (;;) {
            conn* c = k.conns[index].get();
            if (!c || c->waiting) return;
            if (!c->out.empty() && !flush(k, *c, index)) return;
            if (c->closing) {
                close_conn(k, index);
                return;
            }
            if (!route(k, index, *c, c->in, false)) {
                if (c->draining) close_conn(k, index);
                return;
            }
        }
    }

    void on_readable(core& k, std::uint32_t index) {
        conn& c = *k.conns[index];
        const ssize_t n = ::read(c.fd, k.rbuf, read_chunk);
        if (n > 0) {
            const std::string_view chunk(k.rbuf, static_cast<std::size_t>(n));
            // Parse in place unless something is buffered or pending.
            if (c.in.empty() && !c.waiting && c.out.empty() && !c.closing) route(k, index, c, chunk, true);
            else c.in.append(chunk);
        } else {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
            // EOF or error: Valkey closes at once; so do we.
            if (c.waiting == 0) {
                close_conn(k, index);
                return;
            }
            c.closing = true;
            c.draining = true;
            set_events(k, c, index, 0);
            return;
        }
        drive(k, index);
    }

    void on_message(core& k, message& m) {
        switch (m.k) {
        case message::kind::request:
            m.b->execute(k.store);
            m.k = message::kind::reply;
            {
                const unsigned back = m.from;
                m.from = k.id;
                k.outbox[back].push_back(std::move(m));
            }
            return;
        case message::kind::reply: {
            conn* c = find(k, m.conn);
            if (!c) return; // the connection is gone; drop its batch
            c->plan.give_back(m.from, std::move(m.b));
            if (--c->waiting == 0) {
                c->plan.assemble(c->out);
                drive(k, static_cast<std::uint32_t>(m.conn));
            }
            return;
        }
        case message::kind::adopt:
            adopt(k, m.fd);
            return;
        case message::kind::drain:
            begin_drain(k);
            return;
        case message::kind::wake:
            return;
        }
    }

    void begin_drain(core& k) {
        if (k.draining) return;
        k.draining = true;
        k.drain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.drain_ms);
        if (k.id == 0) ::epoll_ctl(k.epfd, EPOLL_CTL_DEL, listen_fd_, nullptr);
        for (std::uint32_t i = 0; i < k.conns.size(); ++i) {
            conn* c = k.conns[i].get();
            if (!c) continue;
            c->draining = true;
            if (!c->want_write) set_events(k, *c, i, 0);
            drive(k, i); // answers complete requests already read, then closes
        }
    }

    void accept_all(core& k) {
        for (;;) {
            const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) return;
            set_nodelay(fd);
            const unsigned target = next_++ % cfg_.cores;
            if (target == k.id) {
                adopt(k, fd);
            } else {
                message m{message::kind::adopt};
                m.fd = fd;
                k.outbox[target].push_back(std::move(m));
            }
        }
    }

    void run(core& k) {
        epoll_event events[256];
        for (;;) {
            int timeout = -1;
            if (k.draining && k.live > 0) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      k.drain_deadline - std::chrono::steady_clock::now())
                                      .count();
                if (left <= 0) {
                    // Peers that did not read their replies: cut them off.
                    for (std::uint32_t i = 0; i < k.conns.size(); ++i)
                        if (k.conns[i]) close_conn(k, i);
                } else {
                    timeout = static_cast<int>(left);
                }
            }
            if (k.draining && k.live == 0 && !k.counted_drained) {
                k.counted_drained = true;
                if (drained_.fetch_add(1) + 1 == cfg_.cores)
                    for (auto& other : cores_) deliver(*other, message{message::kind::wake});
            }
            if (drained_.load() == cfg_.cores) {
                flush_outboxes(k);
                if (cfg_.stats)
                    std::fprintf(stderr,
                                 "stats core=%u io=epoll epoll_waits=%llu handoffs=%llu "
                                 "eventfd_wakeups=%llu messages_sent=%llu\n",
                                 k.id, static_cast<unsigned long long>(k.waits),
                                 static_cast<unsigned long long>(k.handoffs),
                                 static_cast<unsigned long long>(k.wakeups_sent),
                                 static_cast<unsigned long long>(k.messages_sent));
                return;
            }
            ++k.waits;
            const int n = ::epoll_wait(k.epfd, events, 256, timeout);
            for (int i = 0; i < n; ++i) {
                const std::uint64_t tag = events[i].data.u64;
                if (tag == listener_tag) {
                    accept_all(k);
                } else if (tag == inbox_tag) {
                    std::uint64_t v;
                    (void)!::read(k.box.efd, &v, sizeof(v));
                    {
                        std::lock_guard lock(k.box.m);
                        k.work.swap(k.box.q);
                    }
                    for (auto& m : k.work) on_message(k, m);
                    k.work.clear();
                } else {
                    const auto index = static_cast<std::uint32_t>(tag);
                    conn* c = find(k, tag);
                    if (!c) continue; // closed earlier in this batch of events
                    if (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                        if (c->want_write || c->draining) {
                            if (events[i].events & (EPOLLERR | EPOLLHUP)) close_conn(k, index);
                            else drive(k, index);
                        } else {
                            on_readable(k, index);
                        }
                    } else if (events[i].events & EPOLLOUT) {
                        drive(k, index);
                    }
                }
            }
            flush_outboxes(k);
        }
    }

    config cfg_;
    int listen_fd_;
    std::unique_ptr<std::atomic<long>[]> counts_;
    kv::server_info info_;
    std::vector<std::unique_ptr<core>> cores_;
    std::vector<std::thread> threads_;
    std::atomic<unsigned> drained_{0};
    unsigned next_ = 0; // used by core 0 only
};

} // namespace

std::unique_ptr<frontend> start_epoll(const config& cfg, int listen_fd) {
    return std::make_unique<epoll_frontend>(cfg, listen_fd);
}

} // namespace kvs
