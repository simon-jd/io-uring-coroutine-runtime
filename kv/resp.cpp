#include "kv/resp.hpp"
#include <charconv>
#include <cstdint>
#include <cstring>

namespace kv {

bool parse_ll(std::string_view s, long long& out) noexcept {
    if (s.empty() || s.size() > 20) return false;
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (s.size() == 1) return false;
    }
    if (s[i] == '0' && s.size() > i + 1) return false; // leading zero
    if (neg && s[i] == '0') return false;             // "-0"
    unsigned long long v = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        const unsigned d = static_cast<unsigned>(c - '0');
        if (v > (~0ull - d) / 10) return false;
        v = v * 10 + d;
    }
    if (neg) {
        if (v > 9223372036854775808ull) return false;
        out = v == 9223372036854775808ull ? static_cast<long long>(-9223372036854775807ll - 1)
                                           : -static_cast<long long>(v);
    } else {
        if (v > 9223372036854775807ull) return false;
        out = static_cast<long long>(v);
    }
    return true;
}

parse_status parser::fail(std::string msg) {
    error_ = std::move(msg);
    return parse_status::error;
}

namespace {
// Finds "\r\n" at or after `from`; returns the position of '\r' or npos.
std::size_t find_crlf(std::string_view d, std::size_t from) {
    while (from < d.size()) {
        const void* p = std::memchr(d.data() + from, '\r', d.size() - from);
        if (!p) return std::string_view::npos;
        const std::size_t cr = static_cast<std::size_t>(static_cast<const char*>(p) - d.data());
        if (cr + 1 >= d.size()) return std::string_view::npos; // '\n' not here yet
        if (d[cr + 1] == '\n') return cr;
        from = cr + 1;
    }
    return std::string_view::npos;
}

// The common case of a count or length line, in one pass: a lone "0" or 1 to
// 18 digits without a leading zero, then "\r\n". Anything else (a sign, a
// leading zero, more digits, other bytes, a line not complete yet) returns
// false and goes through find_crlf and parse_ll, which accept and reject
// exactly as before, with the same error messages.
bool short_number(std::string_view d, std::size_t p, long long& value, std::size_t& next) noexcept {
    const std::size_t n = d.size();
    if (p >= n) return false;
    unsigned digit = static_cast<unsigned char>(d[p]) - unsigned{'0'};
    if (digit > 9) return false;
    std::uint64_t v = digit;
    std::size_t i = p + 1;
    if (digit != 0) {
        const std::size_t stop = n < p + 18 ? n : p + 18;
        for (; i < stop; ++i) {
            digit = static_cast<unsigned char>(d[i]) - unsigned{'0'};
            if (digit > 9) break;
            v = v * 10 + digit;
        }
    }
    if (i + 1 >= n || d[i] != '\r' || d[i + 1] != '\n') return false;
    value = static_cast<long long>(v);
    next = i + 2;
    return true;
}
} // namespace

parse_status parser::next(std::string_view d, std::size_t& pos, command& cmd) {
    for (;;) {
        if (pos >= d.size()) return parse_status::incomplete;
        std::size_t p = pos;
        if (d[p] != '*') {
            // Inline command: one line, words separated by spaces or tabs.
            const void* nl = std::memchr(d.data() + p, '\n', d.size() - p);
            if (!nl) {
                if (d.size() - p > lim_.max_inline)
                    return fail("Protocol error: too big inline request");
                return parse_status::incomplete;
            }
            const std::size_t end = static_cast<std::size_t>(static_cast<const char*>(nl) - d.data());
            if (end - p > lim_.max_inline) return fail("Protocol error: too big inline request");
            std::size_t stop = end;
            if (stop > p && d[stop - 1] == '\r') --stop;
            cmd.args.clear();
            std::size_t i = p;
            while (i < stop) {
                while (i < stop && (d[i] == ' ' || d[i] == '\t')) ++i;
                const std::size_t w = i;
                while (i < stop && d[i] != ' ' && d[i] != '\t') ++i;
                if (i > w) cmd.args.push_back(d.substr(w, i - w));
            }
            pos = end + 1;
            if (cmd.args.empty()) continue; // blank line
            return parse_status::complete;
        }
        // Array of bulk strings: *<n>\r\n then n x $<len>\r\n<bytes>\r\n
        long long n = 0;
        std::size_t after = 0;
        if (short_number(d, p + 1, n, after)) {
            if (n > static_cast<long long>(lim_.max_args))
                return fail("Protocol error: invalid multibulk length");
            p = after;
        } else {
            const std::size_t cr = find_crlf(d, p + 1);
            if (cr == std::string_view::npos) {
                if (d.size() - p > lim_.max_inline) return fail("Protocol error: too big mbulk count string");
                return parse_status::incomplete;
            }
            if (!parse_ll(d.substr(p + 1, cr - p - 1), n) ||
                n > static_cast<long long>(lim_.max_args))
                return fail("Protocol error: invalid multibulk length");
            p = cr + 2;
        }
        if (n <= 0) {
            pos = p;
            continue; // empty command
        }
        cmd.args.clear();
        for (long long k = 0; k < n; ++k) {
            if (p >= d.size()) return parse_status::incomplete;
            if (d[p] != '$') {
                const char c = d[p];
                return fail(std::string("Protocol error: expected '$', got '") + c + "'");
            }
            long long len = 0;
            std::size_t start = 0;
            if (short_number(d, p + 1, len, start)) {
                if (static_cast<unsigned long long>(len) > lim_.max_bulk)
                    return fail("Protocol error: invalid bulk length");
            } else {
                const std::size_t lcr = find_crlf(d, p + 1);
                if (lcr == std::string_view::npos) {
                    if (d.size() - p > lim_.max_inline)
                        return fail("Protocol error: too big bulk count string");
                    return parse_status::incomplete;
                }
                if (!parse_ll(d.substr(p + 1, lcr - p - 1), len) || len < 0 ||
                    static_cast<unsigned long long>(len) > lim_.max_bulk)
                    return fail("Protocol error: invalid bulk length");
                start = lcr + 2;
            }
            const std::size_t ulen = static_cast<std::size_t>(len);
            if (d.size() - start < ulen + 2) return parse_status::incomplete;
            if (d[start + ulen] != '\r' || d[start + ulen + 1] != '\n')
                return fail("Protocol error: bulk string not terminated by CRLF");
            cmd.args.push_back(d.substr(start, ulen));
            p = start + ulen + 2;
        }
        pos = p;
        return parse_status::complete;
    }
}

// Each encoder appends its framing in as few appends as it can: the type byte,
// the number and the CRLF are built on the stack and appended once.

void reply_simple(std::string& out, std::string_view s) {
    if (s.size() <= 30) {
        char buf[33];
        buf[0] = '+';
        std::memcpy(buf + 1, s.data(), s.size());
        buf[s.size() + 1] = '\r';
        buf[s.size() + 2] = '\n';
        out.append(buf, s.size() + 3);
        return;
    }
    out += '+';
    out += s;
    out += "\r\n";
}

void reply_error(std::string& out, std::string_view s) {
    out += '-';
    out += s;
    out += "\r\n";
}

namespace {
template <class T>
void reply_number(std::string& out, char type, T v) {
    char buf[24];
    buf[0] = type;
    auto r = std::to_chars(buf + 1, buf + sizeof(buf) - 2, v);
    r.ptr[0] = '\r';
    r.ptr[1] = '\n';
    out.append(buf, static_cast<std::size_t>(r.ptr + 2 - buf));
}
} // namespace

void reply_int(std::string& out, long long v) { reply_number(out, ':', v); }

void reply_bulk(std::string& out, std::string_view s) {
    reply_number(out, '$', s.size());
    out.append(s.data(), s.size());
    out.append("\r\n", 2);
}

void reply_nil(std::string& out) { out.append("$-1\r\n", 5); }

void reply_array(std::string& out, std::size_t n) { reply_number(out, '*', n); }

} // namespace kv
