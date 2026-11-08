// End-to-end tests of kv_server over loopback TCP. Each case starts the server
// binary (built with the same sanitizer as this test), talks RESP to it with
// plain blocking sockets, then stops it with SIGTERM and requires a clean exit
// (a sanitizer report makes the exit code non-zero).
//
// Usage: server_tests --server <kv_server> --io uring|epoll|asio
//                     [--server-arg ARG]... [case]
// Every --server-arg is passed to each server the cases start.
#include "test.hpp"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <random>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>

extern char** environ;

namespace {

using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

std::string g_server;
std::string g_io;
std::vector<std::string> g_server_args;

std::string resp(const std::vector<std::string>& args) {
    std::string s = "*" + std::to_string(args.size()) + "\r\n";
    for (auto& a : args) s += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    return s;
}
std::string bulk(const std::string& v) { return "$" + std::to_string(v.size()) + "\r\n" + v + "\r\n"; }

class server_proc {
public:
    explicit server_proc(std::vector<std::string> extra = {}) {
        int out[2];
        CHECK(::pipe2(out, O_CLOEXEC) == 0);
        log_ = "/tmp/kv_server_test." + std::to_string(::getpid()) + "." + std::to_string(++seq_) + ".log";
        std::vector<std::string> args = {g_server, "--io", g_io, "--port", "0", "--drain-ms", "1500"};
        args.insert(args.end(), g_server_args.begin(), g_server_args.end());
        args.insert(args.end(), extra.begin(), extra.end());
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_addopen(&fa, 2, log_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(posix_spawn(&pid_, g_server.c_str(), &fa, nullptr, argv.data(), environ) == 0);
        posix_spawn_file_actions_destroy(&fa);
        ::close(out[1]);
        // First line on stdout: "port N".
        std::string line;
        const auto deadline = clock_type::now() + 20s;
        while (line.find('\n') == std::string::npos) {
            pollfd p{out[0], POLLIN, 0};
            CHECK(clock_type::now() < deadline);
            if (::poll(&p, 1, 100) <= 0) continue;
            char c[64];
            const ssize_t n = ::read(out[0], c, sizeof(c));
            if (n <= 0) {
                ::close(out[0]);
                throw std::runtime_error("server exited during start-up:\n" + log_text());
            }
            line.append(c, static_cast<std::size_t>(n));
        }
        ::close(out[0]);
        CHECK(std::sscanf(line.c_str(), "port %d", &port_) == 1);
    }
    server_proc(const server_proc&) = delete;
    server_proc& operator=(const server_proc&) = delete;
    ~server_proc() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
        ::unlink(log_.c_str());
    }
    int port() const { return port_; }
    // SIGTERM, then wait up to `limit` for the exit. Returns the elapsed time.
    std::chrono::milliseconds stop(std::chrono::milliseconds limit = 10000ms) {
        const auto t0 = clock_type::now();
        CHECK(::kill(pid_, SIGTERM) == 0);
        int status = 0;
        for (;;) {
            const pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_) break;
            if (clock_type::now() - t0 > limit) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &status, 0);
                pid_ = -1;
                throw std::runtime_error("server did not exit after SIGTERM:\n" + log_text());
            }
            std::this_thread::sleep_for(5ms);
        }
        pid_ = -1;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            throw std::runtime_error("server exit status " + std::to_string(status) + ":\n" + log_text());
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0);
    }
    std::string log_text() const {
        std::string s;
        if (FILE* f = std::fopen(log_.c_str(), "r")) {
            char buf[4096];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
            std::fclose(f);
        }
        return s.substr(0, 20000);
    }

private:
    pid_t pid_ = -1;
    int port_ = 0;
    std::string log_;
    static inline int seq_ = 0;
};

class client {
public:
    explicit client(int port, int rcvbuf = 0) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(fd_ >= 0);
        if (rcvbuf) ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<std::uint16_t>(port));
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
        const int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        timeval tv{10, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    client(const client&) = delete;
    client& operator=(const client&) = delete;
    ~client() {
        if (fd_ >= 0) ::close(fd_);
    }
    int fd() const { return fd_; }
    void send(std::string_view s) { write_all_fd(fd_, s); }
    // One complete reply, as raw bytes.
    std::string reply() {
        std::size_t pos = 0;
        while (true) {
            pos = 0;
            if (complete(pos)) break;
            fill();
        }
        std::string r = buf_.substr(0, pos);
        buf_.erase(0, pos);
        return r;
    }
    std::string call(const std::vector<std::string>& args) {
        send(resp(args));
        return reply();
    }
    // true when the server closed the connection (after any buffered replies).
    bool at_eof() {
        if (!buf_.empty()) return false;
        char c;
        const ssize_t n = ::recv(fd_, &c, 1, 0);
        if (n > 0) buf_.push_back(c);
        return n == 0 || (n < 0 && errno == ECONNRESET);
    }
    void reset_close() {
        linger l{1, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        ::close(fd_);
        fd_ = -1;
    }

private:
    void fill() {
        char tmp[65536];
        const ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
        if (n <= 0) throw std::runtime_error("connection closed or timed out while waiting for a reply");
        buf_.append(tmp, static_cast<std::size_t>(n));
    }
    bool line(std::size_t& pos, std::string& out) {
        const auto e = buf_.find("\r\n", pos);
        if (e == std::string::npos) return false;
        out = buf_.substr(pos, e - pos);
        pos = e + 2;
        return true;
    }
    bool complete(std::size_t& pos) {
        if (pos >= buf_.size()) return false;
        const char type = buf_[pos];
        std::string l;
        if (!line(pos, l)) return false;
        if (type == '+' || type == '-' || type == ':') return true;
        const long long n = std::stoll(l.substr(1));
        if (type == '$') {
            if (n < 0) return true;
            if (buf_.size() < pos + static_cast<std::size_t>(n) + 2) return false;
            pos += static_cast<std::size_t>(n) + 2;
            return true;
        }
        if (type == '*') {
            for (long long i = 0; i < n; ++i)
                if (!complete(pos)) return false;
            return true;
        }
        throw std::runtime_error("bad reply type");
    }
    int fd_ = -1;
    std::string buf_;
};

long connected_clients(client& c) {
    const auto r = c.call({"INFO"});
    const auto at = r.find("connected_clients:");
    CHECK(at != std::string::npos);
    return std::stol(r.substr(at + 18));
}

template <class Pred>
bool eventually(Pred pred, std::chrono::milliseconds limit = 3000ms) {
    const auto t0 = clock_type::now();
    while (clock_type::now() - t0 < limit) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

// A pipeline that touches every core: SET/GET/DEL/EXISTS on 50 keys,
// multi-key commands, an inline PING and an empty command.
std::string mixed_pipeline() {
    std::string p = resp({"FLUSHALL"});
    for (int i = 0; i < 50; ++i) {
        const std::string k = "key:" + std::to_string(i);
        p += resp({"SET", k, std::string(static_cast<std::size_t>(i * 3 + 1), static_cast<char>('a' + i % 26))});
        p += resp({"GET", k});
        if (i % 5 == 0) p += "PING split\r\n";
        if (i % 7 == 0) p += "*0\r\n";
        if (i % 3 == 0) p += resp({"DEL", k, "key:" + std::to_string(i / 2), "nope"});
        p += resp({"EXISTS", k, "key:1", "key:2"});
    }
    p += resp({"DBSIZE"});
    return p;
}

std::size_t count_commands(const std::string& pipeline) {
    std::size_t n = 0;
    for (std::size_t i = 0; i + 1 < pipeline.size(); ++i)
        if (pipeline[i] == '*' && pipeline[i + 1] != '0' && (i == 0 || pipeline[i - 1] == '\n')) ++n;
    for (std::size_t at = 0; (at = pipeline.find("PING split", at)) != std::string::npos; ++at) ++n;
    return n;
}

std::vector<std::string> read_replies(client& c, std::size_t n) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < n; ++i) out.push_back(c.reply());
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<char*> rest = {argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--server" && i + 1 < argc) g_server = argv[++i];
        else if (a == "--io" && i + 1 < argc) g_io = argv[++i];
        else if (a == "--server-arg" && i + 1 < argc) g_server_args.push_back(argv[++i]);
        else rest.push_back(argv[i]);
    }
    if (g_server.empty() || g_io.empty()) {
        std::fprintf(stderr, "usage: server_tests --server PATH --io uring|epoll|asio [case]\n");
        return 2;
    }
    std::signal(SIGPIPE, SIG_IGN);
    std::vector<Case> cases = {
        {"basic_commands",
         [] {
             server_proc s;
             client c(s.port());
             CHECK(c.call({"PING"}) == "+PONG\r\n");
             CHECK(c.call({"PING", "hello"}) == bulk("hello"));
             CHECK(c.call({"ECHO", "x y"}) == bulk("x y"));
             CHECK(c.call({"GET", "missing"}) == "$-1\r\n");
             CHECK(c.call({"SET", "a", "1"}) == "+OK\r\n");
             CHECK(c.call({"SET", "a", "2", "NX"}) == "$-1\r\n");
             CHECK(c.call({"SET", "b", "2", "XX"}) == "$-1\r\n");
             CHECK(c.call({"GET", "a"}) == bulk("1"));
             CHECK(c.call({"EXISTS", "a", "b", "a"}) == ":2\r\n");
             CHECK(c.call({"DEL", "a", "b"}) == ":1\r\n");
             CHECK(c.call({"DBSIZE"}) == ":0\r\n");
             CHECK(c.call({"GET"}) == "-ERR wrong number of arguments for 'get' command\r\n");
             CHECK(c.call({"FOO"}) == "-ERR unknown command 'FOO', with args beginning with: \r\n");
             CHECK(c.call({"CONFIG", "GET", "save"}) == "*0\r\n");
             CHECK(c.call({"COMMAND", "DOCS"}) == "*0\r\n");
             c.send("PING\r\n");
             CHECK(c.reply() == "+PONG\r\n");
             CHECK(connected_clients(c) == 1);
             CHECK(c.call({"QUIT"}) == "+OK\r\n");
             CHECK(c.at_eof());
             s.stop();
         }},
        {"pipeline_split_across_reads",
         [] {
             // The same pipeline sent in one write, one byte per write, and in
             // random chunks must get byte-identical replies.
             server_proc s({"--cores", "3"});
             const std::string p = mixed_pipeline();
             const std::size_t n = count_commands(p);
             client whole(s.port());
             whole.send(p);
             const auto expect = read_replies(whole, n);
             CHECK(expect.back() != ":0\r\n");
             client bytes(s.port());
             for (std::size_t i = 0; i < p.size(); ++i) {
                 bytes.send(std::string_view(p).substr(i, 1));
                 if (i % 8 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
             }
             REQUIRE("split pipeline gets the same replies", read_replies(bytes, n) == expect);
             std::mt19937 rng(11);
             for (int round = 0; round < 5; ++round) {
                 client chunks(s.port());
                 for (std::size_t i = 0; i < p.size();) {
                     const std::size_t len = std::min<std::size_t>(1 + rng() % 700, p.size() - i);
                     chunks.send(std::string_view(p).substr(i, len));
                     i += len;
                 }
                 REQUIRE("split pipeline gets the same replies", read_replies(chunks, n) == expect);
             }
             s.stop();
         }},
        {"large_values",
         [] {
             server_proc s({"--cores", "2"});
             client c(s.port());
             std::string v(3 << 20, 'x');
             for (std::size_t i = 0; i < v.size(); i += 4093) v[i] = static_cast<char>('a' + i % 26);
             CHECK(c.call({"SET", "big", v}) == "+OK\r\n");
             REQUIRE("a 3 MiB value comes back intact", c.call({"GET", "big"}) == bulk(v));
             // Ten pipelined GETs of it: 30 MiB of replies through short writes.
             std::string p;
             for (int i = 0; i < 10; ++i) p += resp({"GET", "big"});
             c.send(p);
             for (int i = 0; i < 10; ++i) CHECK(c.reply() == bulk(v));
             s.stop();
         }},
        {"protocol_error_closes_after_reply",
         [] {
             server_proc s;
             for (std::string bad : {std::string("*1\r\n$abc\r\n"), std::string("*2\r\n+x\r\n"),
                                     std::string("*1\r\n$999999999999\r\n")}) {
                 client c(s.port());
                 c.send("PING\r\n" + bad);
                 CHECK(c.reply() == "+PONG\r\n");
                 const auto err = c.reply();
                 CHECK(err.rfind("-ERR Protocol error:", 0) == 0);
                 REQUIRE("the server closes after a protocol error", c.at_eof());
             }
             s.stop();
         }},
        {"slow_reader_does_not_block_other_connections",
         [] {
             server_proc s({"--cores", "1"});
             client a(s.port(), 4096);
             const std::string v(16 * 1024, 'v');
             CHECK(a.call({"SET", "v", v}) == "+OK\r\n");
             // a asks for ~16 MiB of replies and keeps sending while not reading.
             std::string p;
             for (int i = 0; i < 1000; ++i) p += resp({"GET", "v"});
             a.send(p);
             const int fl = ::fcntl(a.fd(), F_GETFL);
             ::fcntl(a.fd(), F_SETFL, fl | O_NONBLOCK);
             std::size_t extra = 0;
             const std::string more = resp({"GET", "v"});
             for (int i = 0; i < 4000; ++i) {
                 if (::send(a.fd(), more.data(), more.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(more.size())) break;
                 ++extra;
             }
             ::fcntl(a.fd(), F_SETFL, fl);
             client b(s.port());
             const auto t0 = clock_type::now();
             CHECK(b.call({"PING"}) == "+PONG\r\n");
             REQUIRE("other connections are served while one peer does not read",
                     clock_type::now() - t0 < 2s);
             for (std::size_t i = 0; i < 1000 + extra; ++i)
                 REQUIRE("every reply reaches the slow reader", a.reply() == bulk(v));
             CHECK(a.call({"PING"}) == "+PONG\r\n");
             s.stop();
         }},
        {"abrupt_disconnects_are_cleaned_up",
         [] {
             server_proc s({"--cores", "2"});
             client watcher(s.port());
             CHECK(watcher.call({"SET", "v", std::string(256 * 1024, 'q')}) == "+OK\r\n");
             for (int round = 0; round < 20; ++round) {
                 client a(s.port(), 4096);
                 std::string p;
                 for (int i = 0; i < 50; ++i) p += resp({"GET", "v"});
                 a.send(p);
                 if (round % 2) a.send("*3\r\n$3\r\nSET\r\n$7\r\npartial\r\n$5\r\nab");
                 a.reset_close(); // RST with replies still being written
             }
             // The server copies 20 x 50 replies of 256 KiB before it sees the resets; under
             // TSan that takes about 2.5 s on the development machine and longer on CI runners.
             REQUIRE("connection count returns to the clients still connected",
                     eventually([&] { return connected_clients(watcher) == 1; }, 15000ms));
             CHECK(watcher.call({"EXISTS", "partial"}) == ":0\r\n");
             s.stop();
         }},
        {"many_clients_match_a_sequential_model",
         [] {
             server_proc s({"--cores", "4"});
             constexpr int clients = 24;
             std::vector<std::unique_ptr<client>> cs;
             std::vector<std::map<std::string, std::string>> model(clients);
             for (int i = 0; i < clients; ++i) cs.push_back(std::make_unique<client>(s.port()));
             std::mt19937 rng(5);
             bool all_match = true;
             for (int round = 0; round < 60; ++round) {
                 std::vector<std::vector<std::string>> expect(clients);
                 for (int i = 0; i < clients; ++i) {
                     std::string p;
                     const int ops = 1 + static_cast<int>(rng() % 20);
                     for (int k = 0; k < ops; ++k) {
                         const std::string key = "c" + std::to_string(i) + ":k" + std::to_string(rng() % 16);
                         const std::string key2 = "c" + std::to_string(i) + ":k" + std::to_string(rng() % 16);
                         auto& m = model[static_cast<std::size_t>(i)];
                         switch (rng() % 4) {
                         case 0: {
                             const std::string v = std::to_string(rng());
                             p += resp({"SET", key, v});
                             m[key] = v;
                             expect[static_cast<std::size_t>(i)].push_back("+OK\r\n");
                             break;
                         }
                         case 1:
                             p += resp({"GET", key});
                             expect[static_cast<std::size_t>(i)].push_back(m.count(key) ? bulk(m[key]) : "$-1\r\n");
                             break;
                         case 2: {
                             p += resp({"DEL", key, key2});
                             long n = static_cast<long>(m.erase(key));
                             if (key2 != key) n += static_cast<long>(m.erase(key2));
                             expect[static_cast<std::size_t>(i)].push_back(":" + std::to_string(n) + "\r\n");
                             break;
                         }
                         default: {
                             p += resp({"EXISTS", key, key2});
                             const long n = static_cast<long>(m.count(key) + m.count(key2));
                             expect[static_cast<std::size_t>(i)].push_back(":" + std::to_string(n) + "\r\n");
                         }
                         }
                     }
                     cs[static_cast<std::size_t>(i)]->send(p);
                 }
                 for (int i = 0; i < clients; ++i)
                     for (auto& e : expect[static_cast<std::size_t>(i)]) all_match &= cs[static_cast<std::size_t>(i)]->reply() == e;
             }
             REQUIRE("replies match a sequential model per client", all_match);
             s.stop();
         }},
        {"shutdown_answers_received_requests",
         [] {
             // Requests already received when SIGTERM arrives are answered;
             // then every connection, idle ones included, sees EOF.
             server_proc s({"--cores", "2"});
             client idle(s.port());
             CHECK(idle.call({"PING"}) == "+PONG\r\n");
             client busy(s.port());
             std::string p;
             for (int i = 0; i < 20000; ++i) p += resp({"SET", "k" + std::to_string(i % 100), "v"});
             busy.send(p);
             CHECK(busy.reply() == "+OK\r\n"); // the server is working on the pipeline
             std::exception_ptr stop_error;
             std::thread killer([&] {
                 try {
                     s.stop();
                 } catch (...) {
                     stop_error = std::current_exception();
                 }
             });
             std::size_t answered = 1;
             bool all_ok = true;
             try {
                 while (true) {
                     const auto r = busy.reply();
                     all_ok &= r == "+OK\r\n";
                     ++answered;
                 }
             } catch (const std::exception&) {
             }
             killer.join();
             if (stop_error) std::rethrow_exception(stop_error);
             std::printf("answered %zu of 20000 pipelined requests before the connection closed\n", answered);
             REQUIRE("every reply sent during shutdown is complete and correct", all_ok);
             REQUIRE("idle connections are closed at shutdown", idle.at_eof());
         }},
        {"shutdown_completes_when_a_peer_never_reads",
         [] {
             server_proc s({"--cores", "1"});
             client a(s.port(), 4096);
             CHECK(a.call({"SET", "v", std::string(1 << 20, 'z')}) == "+OK\r\n");
             std::string p;
             for (int i = 0; i < 64; ++i) p += resp({"GET", "v"});
             a.send(p); // 64 MiB of replies nobody reads
             std::this_thread::sleep_for(100ms);
             const auto took = s.stop(8000ms);
             std::printf("shutdown took %lld ms (drain limit 1500 ms)\n", static_cast<long long>(took.count()));
             REQUIRE("shutdown completes when a peer never reads", took < 6000ms);
         }},
    };
    if (g_io == "uring") {
        cases.push_back(
            {"shutdown_waits_for_requests_in_progress",
             [] {
                 // DEBUG SLEEP holds a request in progress while SIGTERM arrives.
                 server_proc s({"--cores", "2"});
                 client c(s.port());
                 client idle(s.port());
                 CHECK(idle.call({"PING"}) == "+PONG\r\n");
                 c.send(resp({"DEBUG", "SLEEP", "0.4"}) + resp({"SET", "k", "v"}) + resp({"GET", "k"}));
                 std::this_thread::sleep_for(100ms);
                 std::exception_ptr stop_error;
                 std::thread killer([&] {
                     try {
                         s.stop();
                     } catch (...) {
                         stop_error = std::current_exception();
                     }
                 });
                 std::string r1, r2, r3;
                 try {
                     r1 = c.reply();
                     r2 = c.reply();
                     r3 = c.reply();
                 } catch (const std::exception&) {
                 }
                 killer.join();
                 if (stop_error) std::rethrow_exception(stop_error);
                 REQUIRE("requests received before shutdown are answered",
                         r1 == "+OK\r\n" && r2 == "+OK\r\n" && r3 == bulk("v"));
                 CHECK(c.at_eof());
                 CHECK(idle.at_eof());
             }});
        cases.push_back(
            {"direct_descriptors",
             [] {
                 server_proc s({"--cores", "2", "--direct"});
                 std::vector<std::unique_ptr<client>> cs;
                 for (int i = 0; i < 16; ++i) cs.push_back(std::make_unique<client>(s.port()));
                 for (int round = 0; round < 3; ++round)
                     for (int i = 0; i < 16; ++i) {
                         const std::string k = "d" + std::to_string(i);
                         CHECK(cs[static_cast<std::size_t>(i)]->call({"SET", k, std::to_string(round)}) == "+OK\r\n");
                         CHECK(cs[static_cast<std::size_t>(i)]->call({"GET", k}) == bulk(std::to_string(round)));
                     }
                 cs.clear(); // closes; the server frees and reuses table entries
                 for (int i = 0; i < 16; ++i) {
                     client c(s.port());
                     CHECK(c.call({"GET", "d" + std::to_string(i)}) == bulk("2"));
                 }
                 client w(s.port());
                 REQUIRE("connection count returns to the clients still connected",
                         eventually([&] { return connected_clients(w) == 1; }));
                 s.stop();
             }});
        cases.push_back(
            {"fixed_buffer_replies",
             [] {
                 server_proc s({"--cores", "2", "--send", "fixed"});
                 client c(s.port());
                 std::string p;
                 for (int i = 0; i < 200; ++i) p += resp({"SET", "k" + std::to_string(i), std::to_string(i)});
                 for (int i = 0; i < 200; ++i) p += resp({"GET", "k" + std::to_string(i)});
                 c.send(p);
                 for (int i = 0; i < 200; ++i) CHECK(c.reply() == "+OK\r\n");
                 for (int i = 0; i < 200; ++i) CHECK(c.reply() == bulk(std::to_string(i)));
                 const std::string v(100 * 1024, 'f'); // larger than a fixed buffer: heap path
                 CHECK(c.call({"SET", "v", v}) == "+OK\r\n");
                 CHECK(c.call({"GET", "v"}) == bulk(v));
                 s.stop();
             }});
    }
    return run(cases, static_cast<int>(rest.size()), rest.data());
}
