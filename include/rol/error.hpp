#pragma once
#include <stdexcept>
#include <string>

namespace rol {

// Errors the runtime reports by exception. I/O results are not exceptions: an
// operation's kernel result (bytes or -errno) is returned to the caller as-is.
enum class Error { invalid, stale, full, busy, closed, allocation, system };

class Failure : public std::runtime_error {
public:
    Error code;
    int sys_errno;
    Failure(Error code_, const std::string& message, int sys_errno_ = 0)
        : std::runtime_error(message), code(code_), sys_errno(sys_errno_) {}
};

// Called when an internal ownership invariant is broken. Prints and aborts: a
// runtime that has lost track of who owns a buffer cannot continue safely.
[[noreturn]] void invariant_failure(const char* expr, const char* file, int line,
                                    const char* message) noexcept;

} // namespace rol

#define ROL_INVARIANT(expr, message)                                              \
    do {                                                                          \
        if (!(expr)) [[unlikely]]                                                 \
            ::rol::invariant_failure(#expr, __FILE__, __LINE__, message);         \
    } while (0)
