#include "server.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace kvs {

int make_listener(const std::string& bind, int port, int& bound_port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, bind.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("bad bind address " + bind);
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0 || ::listen(fd, 4096) < 0) {
        const int e = errno;
        ::close(fd);
        throw std::runtime_error(std::string("bind/listen: ") + std::strerror(e));
    }
    socklen_t len = sizeof(sa);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len);
    bound_port = ntohs(sa.sin_port);
    return fd;
}

void set_nodelay(int fd) {
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void pin_thread(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

} // namespace kvs
