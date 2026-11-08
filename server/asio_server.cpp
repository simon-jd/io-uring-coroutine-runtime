// The key-value server on standalone Asio (the non-Boost distribution of
// Boost.Asio, header-only, default epoll reactor): the same parser, shard,
// routing plan, threading model and batching rule as the other front ends,
// written the way Asio code usually is: one io_context per core thread,
// connections owned by shared_ptr, read -> process -> write -> read.
//
// Cross-core batches are collected per target while handlers run and handed
// over with one asio::post per target when this core's flush handler runs
// (posted when the first message of a round is queued).
#include "kv/engine.hpp"
#include "server.hpp"
#include <asio.hpp>
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

namespace kvs {
namespace {

using asio::ip::tcp;

class asio_frontend;
struct conn;

struct message {
    enum class kind : std::uint8_t { request, reply };
    kind k;
    unsigned from;
    std::shared_ptr<conn> c; // released only on the connection's own core
    std::unique_ptr<kv::batch> b;
};

struct core_ctx {
    explicit core_ctx(unsigned id_) : id(id_), io(1), work(asio::make_work_guard(io)), drain_timer(io) {}
    unsigned id;
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    kv::shard store;
    std::vector<std::vector<message>> outbox;
    bool flush_posted = false;
    std::unordered_set<conn*> conns;
    long live = 0; // connections, plus the acceptor on core 0
    bool draining = false;
    bool counted_drained = false;
    asio::steady_timer drain_timer;
    std::unique_ptr<tcp::acceptor> acceptor;
};

struct conn : std::enable_shared_from_this<conn> {
    conn(asio_frontend& f, core_ctx& k, tcp::socket s);
    void start() { read(); }
    void read();
    void on_read(const asio::error_code& ec, std::size_t n);
    void drive();
    bool route(std::string_view data, bool fresh);
    void write();
    void close();
    void on_reply(unsigned from, std::unique_ptr<kv::batch> b);

    asio_frontend& fe;
    core_ctx& core;
    tcp::socket sock;
    kv::parser parser;
    kv::command cmd;
    kv::plan plan;
    std::string in, out;
    char rbuf[16 * 1024];
    unsigned waiting = 0;
    bool reading = false, writing = false, closing = false, draining = false, closed = false;
};

class asio_frontend final : public frontend {
public:
    asio_frontend(const config& cfg, int listen_fd) : cfg_(cfg) {
        counts_.reset(new std::atomic<long>[cfg.cores]);
        info_.io = "asio";
        info_.cores = cfg.cores;
        info_.connections = counts_.get();
        info_.supports_sleep = false;
        for (unsigned c = 0; c < cfg.cores; ++c) {
            counts_[c] = 0;
            cores_.push_back(std::make_unique<core_ctx>(c));
            cores_.back()->outbox.resize(cfg.cores);
        }
        auto& k0 = *cores_[0];
        k0.acceptor = std::make_unique<tcp::acceptor>(k0.io, tcp::v4(), listen_fd);
        k0.live = 1;
        accept();
        for (unsigned c = 0; c < cfg.cores; ++c)
            threads_.emplace_back([this, c] {
                if (!cfg_.cpus.empty()) pin_thread(cfg_.cpus[c]);
                cores_[c]->io.run();
            });
    }
    ~asio_frontend() override {
        if (!threads_.empty()) {
            stop();
            wait();
        }
        if (cores_[0]->acceptor) {
            // The listening descriptor belongs to main(); do not close it here.
            asio::error_code ec;
            (void)cores_[0]->acceptor->release(ec);
        }
    }
    void stop() override {
        for (auto& k : cores_) asio::post(k->io, [this, kp = k.get()] { begin_drain(*kp); });
    }
    int wait() override {
        for (auto& t : threads_) t.join();
        threads_.clear();
        // A message can reach a core after its run() returned (a connection
        // cut off by the drain deadline while its batch was away). Deliver
        // what is left, single-threaded now, so every connection is released
        // on its own context before any context is destroyed.
        for (std::size_t ran = 1; ran > 0;) {
            ran = 0;
            for (auto& k : cores_) {
                k->io.restart();
                ran += k->io.poll();
            }
        }
        return 0;
    }

    const config& cfg() const { return cfg_; }
    const kv::server_info& info() const { return info_; }

    void send(core_ctx& from, unsigned target, message m) {
        from.outbox[target].push_back(std::move(m));
        if (!from.flush_posted) {
            from.flush_posted = true;
            asio::post(from.io, [this, &from] { flush(from); });
        }
    }

    void conn_closed(core_ctx& k) {
        counts_[k.id].fetch_sub(1, std::memory_order_relaxed);
        --k.live;
        check_drained(k);
    }

private:
    void flush(core_ctx& from) {
        from.flush_posted = false;
        for (unsigned t = 0; t < cfg_.cores; ++t) {
            auto& ob = from.outbox[t];
            if (ob.empty()) continue;
            asio::post(cores_[t]->io, [this, kp = cores_[t].get(), ms = std::move(ob)]() mutable {
                for (auto& m : ms) deliver(*kp, m);
            });
            ob = std::vector<message>();
        }
    }

    void deliver(core_ctx& k, message& m) {
        if (m.k == message::kind::request) {
            m.b->execute(k.store);
            message back{message::kind::reply, k.id, std::move(m.c), std::move(m.b)};
            send(k, m.from, std::move(back));
            return;
        }
        m.c->on_reply(m.from, std::move(m.b));
        m.c.reset(); // on the connection's own core
    }

    void accept() {
        auto& k0 = *cores_[0];
        const unsigned target = next_++ % cfg_.cores;
        core_ctx& tk = *cores_[target];
        k0.acceptor->async_accept(tk.io, [this, &tk](const asio::error_code& ec, tcp::socket s) {
            auto& k = *cores_[0];
            if (ec || k.draining) {
                if (!ec) s.close();
                if (k.draining || ec == asio::error::operation_aborted) {
                    --k.live;
                    check_drained(k);
                    return;
                }
                accept();
                return;
            }
            asio::error_code ignore;
            s.set_option(tcp::no_delay(true), ignore);
            asio::post(tk.io, [this, &tk, sock = std::move(s)]() mutable { adopt(tk, std::move(sock)); });
            accept();
        });
    }

    void adopt(core_ctx& k, tcp::socket s) {
        if (k.draining) return; // the socket closes as it goes out of scope
        ++k.live;
        counts_[k.id].fetch_add(1, std::memory_order_relaxed);
        auto c = std::make_shared<conn>(*this, k, std::move(s));
        k.conns.insert(c.get());
        c->start();
    }

    void begin_drain(core_ctx& k) {
        if (k.draining) return;
        k.draining = true;
        if (k.acceptor) {
            asio::error_code ec;
            k.acceptor->cancel(ec);
        }
        // Stop reading; answer what was read; then close.
        for (conn* c : std::vector<conn*>(k.conns.begin(), k.conns.end())) {
            c->draining = true;
            if (c->reading) {
                asio::error_code ec;
                c->sock.cancel(ec);
            }
        }
        k.drain_timer.expires_after(std::chrono::milliseconds(cfg_.drain_ms));
        k.drain_timer.async_wait([&k](const asio::error_code& ec) {
            if (ec) return;
            for (conn* c : std::vector<conn*>(k.conns.begin(), k.conns.end())) c->close();
        });
        check_drained(k);
    }

    void check_drained(core_ctx& k) {
        if (!k.draining || k.live > 0 || k.counted_drained) return;
        k.counted_drained = true;
        k.drain_timer.cancel();
        if (drained_.fetch_add(1) + 1 == cfg_.cores)
            for (auto& other : cores_) asio::post(other->io, [kp = other.get()] { kp->work.reset(); });
    }

    config cfg_;
    std::unique_ptr<std::atomic<long>[]> counts_;
    kv::server_info info_;
    std::vector<std::unique_ptr<core_ctx>> cores_;
    std::vector<std::thread> threads_;
    std::atomic<unsigned> drained_{0};
    unsigned next_ = 0; // core 0 only
    friend struct conn;
};

conn::conn(asio_frontend& f, core_ctx& k, tcp::socket s)
    : fe(f), core(k), sock(std::move(s)), parser(f.cfg().limits),
      plan(k.id, f.cfg().cores, k.store, f.info()) {}

void conn::read() {
    if (reading || writing || closing || draining || closed) return;
    reading = true;
    sock.async_read_some(asio::buffer(rbuf), [self = shared_from_this()](const asio::error_code& ec,
                                                                         std::size_t n) {
        self->on_read(ec, n);
    });
}

void conn::on_read(const asio::error_code& ec, std::size_t n) {
    reading = false;
    if (closed) return;
    if (ec) {
        if (ec == asio::error::operation_aborted) {
            drive(); // draining: answer what is buffered, then close
            return;
        }
        // EOF or error: close at once (once replies for other cores are back).
        closing = true;
        draining = true;
        if (!waiting && !writing) close();
        return;
    }
    const std::string_view chunk(rbuf, n);
    // Parse in place unless something is buffered or pending.
    if (in.empty() && !waiting && !writing && !closing) {
        if (!route(chunk, true)) drive();
    } else {
        in.append(chunk);
        drive();
    }
    read();
}

// Parses `data` and routes it: other cores' batches are sent, local replies
// written. false when no complete command was there.
bool conn::route(std::string_view data, bool fresh) {
    std::size_t pos = 0;
    plan.begin();
    const bool ok = kv::consume(parser, cmd, plan, data, pos);
    if (!ok || plan.close_after()) closing = true;
    else if (fresh) in.assign(data.substr(pos));
    else in.erase(0, pos);
    if (plan.commands() == 0) return false;
    const auto& targets = plan.targets();
    if (!targets.empty()) {
        for (unsigned t : targets)
            fe.send(core, t, message{message::kind::request, core.id, shared_from_this(), plan.take(t)});
        waiting = static_cast<unsigned>(targets.size());
        return true;
    }
    out.clear();
    plan.assemble(out);
    write();
    return true;
}

void conn::drive() {
    if (closed || waiting || writing) return;
    if (closing) {
        close();
        return;
    }
    if (!route(in, false) && draining && !reading) close();
}

void conn::on_reply(unsigned from, std::unique_ptr<kv::batch> b) {
    if (closed) return; // the batch is dropped with the message
    plan.give_back(from, std::move(b));
    if (--waiting == 0) {
        out.clear();
        plan.assemble(out);
        write();
    }
}

void conn::write() {
    writing = true;
    asio::async_write(sock, asio::buffer(out), [self = shared_from_this()](const asio::error_code& ec, std::size_t) {
        self->writing = false;
        if (self->closed) return;
        if (ec) {
            self->close();
            return;
        }
        self->drive();
        self->read();
    });
}

void conn::close() {
    if (closed) return;
    closed = true;
    asio::error_code ec;
    sock.close(ec);
    core.conns.erase(this);
    fe.conn_closed(core);
}

} // namespace

std::unique_ptr<frontend> start_asio(const config& cfg, int listen_fd) {
    return std::make_unique<asio_frontend>(cfg, listen_fd);
}

} // namespace kvs
