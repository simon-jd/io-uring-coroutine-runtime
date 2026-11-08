#pragma once
#include <cstddef>
#include <cstring>
#include <span>
#include <sys/socket.h>

namespace rol::detail {

struct cmsg_view {
    int level;
    int type;
    std::span<const std::byte> data;
};

// Walks the control messages in `control`, which may be arbitrary bytes. Every
// header and payload is checked against the buffer bounds before it is read
// (CMSG_FIRSTHDR alone does not check cmsg_len). Returns false if a header is
// malformed; messages before it have already been visited.
template <class Fn>
bool for_each_cmsg(std::span<const std::byte> control, Fn&& fn) {
    constexpr std::size_t header = CMSG_LEN(0); // aligned cmsghdr size
    std::size_t offset = 0;
    while (control.size() - offset >= sizeof(cmsghdr)) {
        cmsghdr h;
        std::memcpy(&h, control.data() + offset, sizeof(h));
        const std::size_t len = h.cmsg_len;
        if (len < header || len > control.size() - offset) return false;
        fn(cmsg_view{h.cmsg_level, h.cmsg_type, control.subspan(offset + header, len - header)});
        const std::size_t step = CMSG_ALIGN(len);
        if (step > control.size() - offset) break;
        offset += step;
    }
    return true;
}

// Calls fn(int) for every descriptor carried in SCM_RIGHTS messages.
template <class Fn>
bool for_each_right(std::span<const std::byte> control, Fn&& fn) {
    bool whole = true;
    bool ok = for_each_cmsg(control, [&](const cmsg_view& c) {
        if (c.level != SOL_SOCKET || c.type != SCM_RIGHTS) return;
        if (c.data.size() % sizeof(int)) whole = false;
        for (std::size_t i = 0; i + sizeof(int) <= c.data.size(); i += sizeof(int)) {
            int fd;
            std::memcpy(&fd, c.data.data() + i, sizeof(fd));
            fn(fd);
        }
    });
    return ok && whole;
}

} // namespace rol::detail
