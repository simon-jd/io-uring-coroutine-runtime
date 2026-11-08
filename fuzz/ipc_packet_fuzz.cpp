// libFuzzer target for the IPC receive-side validator (rol::ipc::inspect_packet).
// Input layout: [flags byte][payload length byte][payload][control bytes].
// The control bytes are arbitrary, including cmsg_len values that point past
// the end of the buffer.
#include "rol/detail/cmsg.hpp"
#include "rol/ipc.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) return 0;
    int msg_flags = 0;
    if (data[0] & 1) msg_flags |= MSG_TRUNC;
    if (data[0] & 2) msg_flags |= MSG_CTRUNC;
    const std::size_t payload_len = std::min<std::size_t>(data[1], size - 2);
    // Copy into exact-size heap buffers so ASan sees any out-of-bounds read.
    std::vector<std::byte> payload(payload_len);
    if (payload_len) std::memcpy(payload.data(), data + 2, payload_len);
    const std::size_t control_len = size - 2 - payload_len;
    // operator new returns 16-byte aligned memory, as cmsghdr needs; the size is
    // exact so ASan flags any read past the end.
    std::vector<std::byte> control(control_len);
    if (control_len) std::memcpy(control.data(), data + 2 + payload_len, control_len);
    std::span<const std::byte> ctl(control.data(), control_len);

    const auto v = rol::ipc::inspect_packet(payload, ctl, msg_flags);

    // Independent count of descriptors the reactor would take ownership of.
    std::size_t owned = 0;
    rol::detail::for_each_right(ctl, [&](int) { ++owned; });
    if (v.ok) {
        // Accepted packets: one complete handle, exactly one descriptor,
        // nothing truncated, and a header the validator itself accepts.
        if (payload_len != sizeof(rol::WireHandle) || v.rights != 1 || owned != 1 ||
            (msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || !rol::ipc::header_ok(v.header) ||
            std::memcmp(&v.header, payload.data(), sizeof(rol::WireHandle)) != 0)
            std::abort();
    }
    return 0;
}
