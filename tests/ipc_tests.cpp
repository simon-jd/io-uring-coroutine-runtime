// Sealed-memfd snapshots over SCM_RIGHTS, sent and received through the reactor.
#include "rol/ipc.hpp"
#include "test.hpp"
#include <array>
#include <csignal>
#include <fcntl.h>
#include <numeric>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
using namespace rol;
using namespace std::chrono_literals;

namespace {
WireHandle valid_wire() {
    WireHandle h;
    h.identity = {42, 84};
    h.count = 3;
    return h;
}

int make_fd(WireHandle h, bool sealed = true, bool truncate = false) {
    int fd = ::memfd_create("rol-malformed-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd >= 0);
    CHECK(::write(fd, &h, sizeof(h)) == sizeof(h));
    std::array<std::uint32_t, 3> values{10, 20, 30};
    if (!truncate) CHECK(::write(fd, values.data(), sizeof(values)) == sizeof(values));
    if (sealed)
        CHECK(::fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == 0);
    return fd;
}

// A hostile or buggy peer: raw sendmsg with arbitrary header and descriptors.
void raw_send(int socket, WireHandle wire, std::span<const int> fds, bool oversized = false) {
    std::array<char, sizeof(WireHandle) + 1> packet{};
    std::memcpy(packet.data(), &wire, sizeof(wire));
    iovec iov{packet.data(), sizeof(WireHandle) + (oversized ? 1U : 0U)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (!fds.empty()) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(fds.size_bytes());
        auto* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(fds.size_bytes());
        std::memcpy(CMSG_DATA(c), fds.data(), fds.size_bytes());
    }
    CHECK(::sendmsg(socket, &msg, MSG_NOSIGNAL) == static_cast<ssize_t>(iov.iov_len));
}

// Receive through the runtime and expect rejection without a descriptor leak.
void rejects(reactor& r, WireHandle wire, int fd, std::size_t copies = 1, bool oversized = false) {
    socket_pair p(SOCK_SEQPACKET);
    std::vector<int> fds(copies, fd);
    const auto baseline = fd_count();
    raw_send(p.fd[0], wire, fds, oversized);
    expect(Error::invalid, [&] { (void)r.block_on(ipc::receive_snapshot(r, p.fd[1])); });
    CHECK(fd_count() == baseline);
}

std::uint64_t sum(std::span<const std::uint32_t> v) {
    return std::accumulate(v.begin(), v.end(), std::uint64_t{0});
}
} // namespace

int main(int argc, char** argv) {
    return run(
        {
            {"sealed_snapshot_roundtrip",
             [] {
                 const auto baseline = fd_count();
                 {
                     socket_pair p(SOCK_SEQPACKET);
                     reactor r;
                     auto blob = SharedBlob::create(std::vector<std::uint32_t>{1, 2, UINT32_MAX}, 7);
                     auto received = r.block_on([](reactor& re, int a, int b,
                                                   SharedBlob bl) -> task<SharedBlob> {
                         co_await ipc::send_snapshot(re, a, bl);
                         co_return co_await ipc::receive_snapshot(re, b);
                     }(r, p.fd[0], p.fd[1], blob));
                     CHECK(received.handle().generation == 7);
                     CHECK(received.handle().identity == blob.handle().identity);
                     CHECK(std::equal(blob.values().begin(), blob.values().end(),
                                      received.values().begin()));
                 }
                 CHECK(fd_count() == baseline);
             }},
            {"producer_crash_before_consumer_maps",
             [] {
                 socket_pair p(SOCK_SEQPACKET);
                 auto pid = ::fork();
                 CHECK(pid >= 0);
                 if (pid == 0) {
                     ::close(p.fd[1]);
                     try {
                         reactor child;
                         auto blob = SharedBlob::create(std::vector<std::uint32_t>{4, 8, 15, 16, 23, 42});
                         child.block_on(ipc::send_snapshot(child, p.fd[0], blob));
                     } catch (...) {
                         ::_exit(9);
                     }
                     // Abrupt death; the parent has not mapped the data yet.
                     ::raise(SIGKILL);
                     ::_exit(10);
                 }
                 p.close_end(0);
                 int status = 0;
                 CHECK(::waitpid(pid, &status, 0) == pid);
                 CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
                 reactor r;
                 auto imported = r.block_on(ipc::receive_snapshot(r, p.fd[1]));
                 CHECK(sum(imported.values()) == 108);
             }},
            {"producer_exit_without_export",
             [] {
                 socket_pair p(SOCK_SEQPACKET);
                 auto pid = ::fork();
                 CHECK(pid >= 0);
                 if (pid == 0) {
                     ::close(p.fd[1]);
                     ::_exit(0);
                 }
                 p.close_end(0);
                 int status = 0;
                 CHECK(::waitpid(pid, &status, 0) == pid);
                 CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
                 reactor r;
                 expect(Error::invalid, [&] { (void)r.block_on(ipc::receive_snapshot(r, p.fd[1])); });
             }},
            {"identity_generation_version_and_length",
             [] {
                 reactor r;
                 auto h = valid_wire();
                 int fd = make_fd(h);
                 auto altered = h;
                 altered.identity[0]++;
                 rejects(r, altered, fd);
                 altered = h;
                 altered.generation++;
                 rejects(r, altered, fd);
                 altered = h;
                 altered.version++;
                 rejects(r, altered, fd);
                 altered = h;
                 altered.magic++;
                 rejects(r, altered, fd);
                 altered = h;
                 altered.count++;
                 rejects(r, altered, fd);
                 altered = h;
                 altered.count = UINT64_MAX;
                 rejects(r, altered, fd);
                 altered = h;
                 altered.size--;
                 rejects(r, altered, fd);
                 altered = h;
                 altered.reserved = 1;
                 rejects(r, altered, fd);
                 altered = h;
                 altered.generation = 0;
                 rejects(r, altered, fd);
                 ::close(fd);
             }},
            {"malformed_ancillary_data_has_no_fd_leak",
             [] {
                 reactor r;
                 auto h = valid_wire();
                 int fd = make_fd(h);
                 for (int i = 0; i < 100; ++i) {
                     rejects(r, h, fd, 0);
                     rejects(r, h, fd, 2);
                     rejects(r, h, fd, 12); // more than the control buffer holds: MSG_CTRUNC
                     rejects(r, h, fd, 1, true);
                 }
                 ::close(fd);
             }},
            {"reject_unsealed_truncated_and_non_memfd",
             [] {
                 reactor r;
                 auto h = valid_wire();
                 int fd = make_fd(h, false);
                 rejects(r, h, fd);
                 ::close(fd);
                 fd = make_fd(h, true, true);
                 rejects(r, h, fd);
                 ::close(fd);
                 fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                 CHECK(fd >= 0);
                 rejects(r, h, fd);
                 ::close(fd);
             }},
            {"seals_prevent_mutation_and_resize",
             [] {
                 auto h = valid_wire();
                 int fd = make_fd(h);
                 std::uint32_t value = 999;
                 CHECK(::pwrite(fd, &value, sizeof(value), sizeof(WireHandle)) == -1);
                 CHECK(errno == EPERM);
                 CHECK(::ftruncate(fd, 0) == -1);
                 CHECK(errno == EPERM);
                 void* address = ::mmap(nullptr, 60, PROT_WRITE, MAP_SHARED, fd, 0);
                 CHECK(address == MAP_FAILED);
                 ::close(fd);
             }},
            {"invalid_creation_socket_and_closed_peer",
             [] {
                 expect(Error::invalid, [] { SharedBlob::create({}); });
                 expect(Error::invalid, [] { SharedBlob::create(std::vector<std::uint32_t>{1}, 0); });
                 SharedBlob empty;
                 expect(Error::invalid, [&] { (void)empty.values(); });
                 reactor r;
                 auto blob = SharedBlob::create(std::vector<std::uint32_t>{1});
                 expect(Error::invalid, [&] { r.block_on(ipc::send_snapshot(r, -1, blob)); });
                 socket_pair stream(SOCK_STREAM);
                 expect(Error::invalid, [&] { r.block_on(ipc::send_snapshot(r, stream.fd[0], blob)); });
                 socket_pair p(SOCK_SEQPACKET);
                 p.close_end(1);
                 try {
                     r.block_on(ipc::send_snapshot(r, p.fd[0], blob));
                     CHECK(false);
                 } catch (const Failure& f) {
                     CHECK(f.code == Error::system && f.sys_errno == EPIPE);
                 }
             }},
            {"repeated_import_release_returns_fd_baseline",
             [] {
                 const auto baseline = fd_count();
                 {
                     socket_pair p(SOCK_SEQPACKET);
                     reactor r;
                     r.block_on([](reactor& re, int a, int b) -> task<void> {
                         for (unsigned i = 0; i < 200; ++i) {
                             auto blob = SharedBlob::create(std::vector<std::uint32_t>(256, i));
                             co_await ipc::send_snapshot(re, a, blob);
                             auto got = co_await ipc::receive_snapshot(re, b);
                             CHECK(sum(got.values()) == 256ull * i);
                         }
                     }(r, p.fd[0], p.fd[1]));
                 }
                 CHECK(fd_count() == baseline);
             }},
            {"detached_receive_closes_received_descriptor",
             [] {
                 // The snapshot is already queued, so the recvmsg completes (and
                 // the kernel installs the descriptor) during the submit that
                 // also carries the cancel. The receiving coroutine is gone by
                 // then; the slot owns the descriptor and closes it.
                 socket_pair p(SOCK_SEQPACKET);
                 reactor r;
                 const auto baseline = fd_count();
                 r.block_on([](reactor& re, int a, int b) -> task<void> {
                     co_await ipc::send_snapshot(re, a,
                                                 SharedBlob::create(std::vector<std::uint32_t>{9}));
                     auto c = [](reactor& r2, int fd) -> eager {
                         (void)co_await ipc::receive_snapshot(r2, fd);
                         CHECK(false); // never resumed
                     }(re, b);
                     c.destroy();
                     while (re.stats().detached_released < 1) co_await re.nop();
                     CHECK(re.stats().cancel_not_found == 1); // it completed, not cancelled
                 }(r, p.fd[0], p.fd[1]));
                 REQUIRE("received descriptor closed by the slot", fd_count() == baseline);
             }},
            {"sendmsg_owns_descriptor_copies",
             [] {
                 // Close the descriptor and reuse its number before the send is
                 // submitted: the operation sends its own duplicate.
                 socket_pair p(SOCK_SEQPACKET);
                 reactor r;
                 auto original = SharedBlob::create(std::vector<std::uint32_t>{5, 6, 7});
                 int fd = ::fcntl(original.fd(), F_DUPFD_CLOEXEC, 0);
                 struct stat want{};
                 CHECK(::fstat(fd, &want) == 0);
                 r.block_on([](reactor& re, int a, int b, int& f, WireHandle w,
                               ino_t ino) -> task<void> {
                     auto send = re.sendmsg(a, owned_buffer::copy_of(std::as_bytes(
                                                   std::span<const WireHandle>(&w, 1))),
                                            std::span<const int>(&f, 1));
                     ::close(f);
                     f = ::memfd_create("impostor", MFD_CLOEXEC); // likely the same number
                     auto sent = co_await send;
                     CHECK(sent.res == static_cast<int>(sizeof(WireHandle)));
                     auto got = co_await ipc::receive_snapshot(re, b);
                     struct stat st{};
                     CHECK(::fstat(got.fd(), &st) == 0);
                     REQUIRE("the original file was sent", st.st_ino == ino);
                 }(r, p.fd[0], p.fd[1], fd, original.handle(), want.st_ino));
                 ::close(fd);
             }},
        },
        argc, argv);
}
