#pragma once
#include "rol/buffer.hpp"
#include "rol/reactor.hpp"
#include "rol/task.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace rol {

// Immutable memfd snapshots passed between processes over AF_UNIX
// SOCK_SEQPACKET with SCM_RIGHTS. Linux, little endian, protocol v1.
struct WireHandle {
    std::uint32_t magic = 0x524f4c31;
    std::uint16_t version = 1;
    std::uint16_t size = 48;
    std::array<std::uint64_t, 2> identity{};
    std::uint64_t generation = 1;
    std::uint64_t count = 0;
    std::uint64_t reserved = 0;
};
static_assert(sizeof(WireHandle) == 48);

class SharedBlob {
public:
    static constexpr std::size_t max_elements = 1U << 20;
    // Creates, fills and seals a memfd, then maps it read-only.
    static SharedBlob create(std::span<const std::uint32_t> values, std::uint64_t generation = 1);
    // Checks seals, file type, exact length and the stored header against
    // `expected`, then maps read-only. Takes ownership of `fd` either way.
    static SharedBlob adopt(unique_fd fd, const WireHandle& expected);

    std::span<const std::uint32_t> values() const;
    WireHandle handle() const;
    int fd() const;
    explicit operator bool() const noexcept { return static_cast<bool>(mapping_); }
    SharedBlob() = default;

private:
    struct Mapping;
    std::shared_ptr<Mapping> mapping_;
    explicit SharedBlob(std::shared_ptr<Mapping> m) : mapping_(std::move(m)) {}
};

namespace ipc {

struct packet_verdict {
    bool ok = false;
    const char* reason = "";
    WireHandle header{};
    std::size_t rights = 0; // descriptors carried in SCM_RIGHTS
};

bool header_ok(const WireHandle& h) noexcept;

// The receive-side validator, kept free of system calls so it can be fuzzed.
// `payload` is the bytes received, `control` the control bytes the kernel
// returned (msg_controllen), `msg_flags` the returned flags. It never closes
// descriptors: the reactor already owns every received descriptor.
packet_verdict inspect_packet(std::span<const std::byte> payload,
                              std::span<const std::byte> control, int msg_flags) noexcept;

// Throws Failure(invalid) unless `fd` is a connected AF_UNIX SOCK_SEQPACKET socket.
void check_socket(int fd);

// Async send and receive through the reactor (IORING_OP_SENDMSG/RECVMSG).
// Kernel errors throw Failure(system); protocol violations Failure(invalid).
task<void> send_snapshot(reactor& r, int socket, SharedBlob blob, op_options o = {});
task<SharedBlob> receive_snapshot(reactor& r, int socket, op_options o = {});

} // namespace ipc
} // namespace rol
