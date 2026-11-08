#pragma once
// Minimal test harness: each case is a function; the first failing check ends
// the suite with a message naming the check, file and line.
#include "rol/reactor.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

#define CHECK(x)                                                                              \
    do {                                                                                      \
        if (!(x))                                                                             \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                     ": " #x);                                                \
    } while (0)

// A named assertion. The deliberate-bug script looks for "assertion '<name>' failed".
#define REQUIRE(name, x)                                                                   \
    do {                                                                                   \
        if (!(x))                                                                          \
            throw std::runtime_error(std::string("assertion '") + (name) + "' failed: " #x \
                                     " at " __FILE__ ":" + std::to_string(__LINE__));      \
    } while (0)

template <class Fn>
void expect(rol::Error e, Fn fn) {
    try {
        fn();
    } catch (const rol::Failure& failure) {
        if (failure.code != e)
            throw std::runtime_error(std::string("wrong error code: ") + failure.what());
        return;
    }
    throw std::runtime_error("expected error was not thrown");
}

using Case = std::pair<std::string, std::function<void()>>;
inline int run(const std::vector<Case>& cases, int argc, char** argv) {
    std::size_t count = 0;
    for (auto& [name, test] : cases) {
        if (argc > 1 && name != argv[1]) continue;
        try {
            test();
            std::cout << "PASS " << name << std::endl;
            ++count;
        } catch (const std::exception& e) {
            std::cerr << "FAIL " << name << ": " << e.what() << std::endl;
            return 1;
        }
    }
    if (!count) {
        std::cerr << "No matching tests\n";
        return 2;
    }
    std::cout << "Passed " << count << " cases\n";
    return 0;
}

inline std::size_t fd_count() {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                                                  std::filesystem::directory_iterator{}));
}

struct socket_pair {
    int fd[2] = {-1, -1};
    explicit socket_pair(int type = SOCK_STREAM) {
        CHECK(::socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, fd) == 0);
    }
    socket_pair(const socket_pair&) = delete;
    socket_pair& operator=(const socket_pair&) = delete;
    ~socket_pair() {
        for (int f : fd)
            if (f >= 0) ::close(f);
    }
    void close_end(int i) {
        if (fd[i] >= 0) ::close(fd[i]);
        fd[i] = -1;
    }
};

// A connected loopback TCP pair: fd[0] accepted side, fd[1] connecting side.
struct tcp_pair {
    int fd[2] = {-1, -1};
    explicit tcp_pair(int rcvbuf_on_1 = 0) {
        int l = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(l >= 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::bind(l, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
        CHECK(::listen(l, 1) == 0);
        socklen_t len = sizeof(sa);
        CHECK(::getsockname(l, reinterpret_cast<sockaddr*>(&sa), &len) == 0);
        fd[1] = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(fd[1] >= 0);
        if (rcvbuf_on_1)
            CHECK(::setsockopt(fd[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf_on_1, sizeof(int)) == 0);
        CHECK(::connect(fd[1], reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
        fd[0] = ::accept4(l, nullptr, nullptr, SOCK_CLOEXEC);
        CHECK(fd[0] >= 0);
        ::close(l);
        int one = 1;
        for (int f : fd) ::setsockopt(f, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    tcp_pair(const tcp_pair&) = delete;
    tcp_pair& operator=(const tcp_pair&) = delete;
    ~tcp_pair() {
        for (int f : fd)
            if (f >= 0) ::close(f);
    }
};

// A coroutine type that starts eagerly and can be destroyed by the test at any
// suspension point, the way any user-written coroutine type can be. The runtime
// must stay safe when that happens with I/O in flight.
struct eager {
    struct promise_type {
        std::exception_ptr error;
        eager get_return_object() noexcept {
            return eager{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() const noexcept { return {}; }
        std::suspend_always final_suspend() const noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };
    std::coroutine_handle<promise_type> h;
    explicit eager(std::coroutine_handle<promise_type> handle) noexcept : h(handle) {}
    eager(eager&& o) noexcept : h(std::exchange(o.h, {})) {}
    eager(const eager&) = delete;
    ~eager() { destroy(); }
    void destroy() {
        if (h) h.destroy();
        h = {};
    }
    bool done() const { return h && h.done(); }
    void rethrow() const {
        if (h && h.promise().error) std::rethrow_exception(h.promise().error);
    }
};

inline rol::owned_buffer text(std::string_view s) { return rol::owned_buffer::copy_of(s); }

inline void write_all_fd(int fd, std::string_view s) {
    while (!s.empty()) {
        ssize_t n = ::write(fd, s.data(), s.size());
        CHECK(n > 0);
        s.remove_prefix(static_cast<std::size_t>(n));
    }
}
