#include "rol/ipc.hpp"
#include "rol/detail/cmsg.hpp"
#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace rol {
static_assert(std::endian::native == std::endian::little, "IPC v1 requires little endian");

namespace {
constexpr int required_seals = F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;

[[noreturn]] void invalid(const char* message) { throw Failure(Error::invalid, message); }

void write_all(int fd, const void* buffer, std::size_t len, off_t offset) {
    auto* bytes = static_cast<const char*>(buffer);
    while (len) {
        ssize_t n = ::pwrite(fd, bytes, len, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) invalid("memfd write failed");
        auto count = static_cast<std::size_t>(n);
        bytes += count;
        len -= count;
        offset += n;
    }
}
} // namespace

bool ipc::header_ok(const WireHandle& h) noexcept {
    return h.magic == 0x524f4c31 && h.version == 1 && h.size == sizeof(WireHandle) &&
           h.generation != 0 && h.count != 0 && h.count <= SharedBlob::max_elements &&
           h.reserved == 0 && (h.identity[0] != 0 || h.identity[1] != 0);
}

struct SharedBlob::Mapping {
    unique_fd fd;
    void* address = MAP_FAILED;
    std::size_t bytes = 0;
    WireHandle wire{};
    ~Mapping() {
        if (address != MAP_FAILED) ::munmap(address, bytes);
    }
};

SharedBlob SharedBlob::adopt(unique_fd fd, const WireHandle& expected) {
    if (!ipc::header_ok(expected)) invalid("invalid IPC v1 handle");
    const auto bytes =
        sizeof(WireHandle) + static_cast<std::size_t>(expected.count) * sizeof(std::uint32_t);
    const int seals = ::fcntl(fd.get(), F_GET_SEALS);
    if (seals < 0 || (seals & required_seals) != required_seals)
        invalid("IPC requires immutable size and write seals");
    struct stat st{};
    if (::fstat(fd.get(), &st) || !S_ISREG(st.st_mode) || st.st_size != static_cast<off_t>(bytes))
        invalid("IPC memfd length mismatch");
    WireHandle actual{};
    ssize_t count;
    do {
        count = ::pread(fd.get(), &actual, sizeof(actual), 0);
    } while (count < 0 && errno == EINTR);
    if (count != sizeof(actual) || std::memcmp(&actual, &expected, sizeof(actual)))
        invalid("IPC identity, generation or header mismatch");
    auto m = std::make_shared<Mapping>();
    m->bytes = bytes;
    m->wire = actual;
    m->fd = std::move(fd);
    m->address = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, m->fd.get(), 0);
    if (m->address == MAP_FAILED) throw Failure(Error::allocation, "IPC mmap failed", errno);
    return SharedBlob(std::move(m));
}

SharedBlob SharedBlob::create(std::span<const std::uint32_t> values, std::uint64_t generation) {
    if (values.empty() || values.size() > max_elements || !generation)
        invalid("invalid shared blob size/generation");
    WireHandle wire;
    wire.count = values.size();
    wire.generation = generation;
    // A random object identity distinguishes a recycled fd number from the original object.
    auto* id_bytes = reinterpret_cast<char*>(wire.identity.data());
    std::size_t left = sizeof(wire.identity);
    while (left) {
        ssize_t n = ::getrandom(id_bytes, left, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) invalid("object identity generation failed");
        id_bytes += n;
        left -= static_cast<std::size_t>(n);
    }
    unique_fd fd(::memfd_create("rol-snapshot-v1", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!fd) throw Failure(Error::allocation, "memfd_create failed", errno);
    const auto bytes = sizeof(wire) + values.size_bytes();
    if (::ftruncate(fd.get(), static_cast<off_t>(bytes))) invalid("memfd resize failed");
    write_all(fd.get(), &wire, sizeof(wire), 0);
    write_all(fd.get(), values.data(), values.size_bytes(), sizeof(wire));
    if (::fcntl(fd.get(), F_ADD_SEALS, required_seals)) invalid("memfd sealing failed");
    return adopt(std::move(fd), wire);
}

std::span<const std::uint32_t> SharedBlob::values() const {
    if (!mapping_) invalid("empty shared blob");
    auto* data = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(mapping_->address) +
                                                        sizeof(WireHandle));
    return {data, static_cast<std::size_t>(mapping_->wire.count)};
}

WireHandle SharedBlob::handle() const {
    if (!mapping_) invalid("empty shared blob");
    return mapping_->wire;
}

int SharedBlob::fd() const {
    if (!mapping_) invalid("empty shared blob");
    return mapping_->fd.get();
}

void ipc::check_socket(int fd) {
    int type = 0;
    socklen_t len = sizeof(type);
    sockaddr_un address{};
    socklen_t address_len = sizeof(address);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) || type != SOCK_SEQPACKET ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_len) ||
        address.sun_family != AF_UNIX)
        invalid("IPC requires a UNIX SEQPACKET socket");
}

ipc::packet_verdict ipc::inspect_packet(std::span<const std::byte> payload,
                                        std::span<const std::byte> control,
                                        int msg_flags) noexcept {
    packet_verdict v;
    bool unexpected = false;
    const bool well_formed = detail::for_each_cmsg(control, [&](const detail::cmsg_view& c) {
        if (c.level != SOL_SOCKET || c.type != SCM_RIGHTS || c.data.size() % sizeof(int)) {
            unexpected = true;
            return;
        }
        v.rights += c.data.size() / sizeof(int);
    });
    if (!well_formed) {
        v.reason = "malformed control data";
        return v;
    }
    if (msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        v.reason = "truncated packet or control data";
        return v;
    }
    if (unexpected) {
        v.reason = "unexpected ancillary data";
        return v;
    }
    if (payload.size() != sizeof(WireHandle)) {
        v.reason = "packet is not exactly one handle";
        return v;
    }
    if (v.rights != 1) {
        v.reason = "packet must carry exactly one descriptor";
        return v;
    }
    std::memcpy(&v.header, payload.data(), sizeof(WireHandle));
    if (!header_ok(v.header)) {
        v.reason = "invalid IPC v1 handle";
        return v;
    }
    v.ok = true;
    v.reason = "ok";
    return v;
}

task<void> ipc::send_snapshot(reactor& r, int socket, SharedBlob blob, op_options o) {
    if (!blob) invalid("empty shared blob");
    check_socket(socket);
    const WireHandle wire = blob.handle();
    auto payload = owned_buffer::copy_of(std::as_bytes(std::span<const WireHandle>(&wire, 1)));
    const int fd = blob.fd();
    auto res = co_await r.sendmsg(socket, std::move(payload), std::span<const int>(&fd, 1), o);
    if (res.res < 0) throw Failure(Error::system, "IPC send failed", -res.res);
    if (res.res != static_cast<int>(sizeof(WireHandle))) invalid("IPC send was truncated");
}

task<SharedBlob> ipc::receive_snapshot(reactor& r, int socket, op_options o) {
    check_socket(socket);
    auto res = co_await r.recvmsg(socket, owned_buffer::allocate(sizeof(WireHandle)), o);
    if (res.res < 0) throw Failure(Error::system, "IPC receive failed", -res.res);
    // Every received descriptor is in res.fds and is closed if we throw.
    const auto v = inspect_packet(res.payload.bytes(), res.control.bytes(), res.msg_flags);
    if (!v.ok) invalid(v.reason);
    if (res.fds.size() != 1) invalid("descriptor count does not match the control data");
    co_return SharedBlob::adopt(std::move(res.fds[0]), v.header);
}

} // namespace rol
