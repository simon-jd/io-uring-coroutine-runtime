#pragma once
// RESP request parsing and reply encoding. No I/O and no threads: every server
// front end (io_uring runtime, epoll, Asio) uses this code unchanged.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kv {

struct limits {
    std::size_t max_bulk = std::size_t{64} << 20; // bytes per argument
    std::size_t max_args = 1024 * 1024;          // arguments per command
    std::size_t max_inline = 64 * 1024;          // inline command line, count lines
};

enum class parse_status { complete, incomplete, error };

// One command as views into the input buffer. Valid until that buffer changes.
struct command {
    std::vector<std::string_view> args;
};

// Stateless request parser for RESP arrays of bulk strings (what client
// libraries and valkey-benchmark send) and inline commands (words separated by
// spaces on one line; no quoting). A command split across reads is parsed only
// once all its bytes are there: the caller keeps the unparsed tail and calls
// again with more data.
class parser {
public:
    explicit parser(limits l = {}) : lim_(l) {}

    // Parses one command starting at data[pos]. complete: `cmd` holds it and
    // `pos` points past it. Empty commands (`*0`, blank lines) are consumed and
    // skipped. incomplete: more bytes are needed and `pos` is unchanged.
    // error: a protocol error (error() says which); the connection must close.
    parse_status next(std::string_view data, std::size_t& pos, command& cmd);
    const std::string& error() const noexcept { return error_; }

private:
    parse_status fail(std::string msg);
    limits lim_;
    std::string error_;
};

// Strict decimal parse: optional '-', no '+', no leading zeros, no overflow.
bool parse_ll(std::string_view s, long long& out) noexcept;

// Reply encoders (append to `out`).
void reply_simple(std::string& out, std::string_view s);
void reply_error(std::string& out, std::string_view s);
void reply_int(std::string& out, long long v);
void reply_bulk(std::string& out, std::string_view s);
void reply_nil(std::string& out);
void reply_array(std::string& out, std::size_t n);

} // namespace kv
