#pragma once
// Reference implementations for differential tests (kv_tests, the RESP fuzz
// target): the straightforward parser and shard the server used before their
// faster replacements. The tests and the fuzz target require the replacements
// to give the same commands, outcomes, error messages and replies.
#include "kv/engine.hpp"
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kv::reference {

inline std::size_t find_crlf(std::string_view d, std::size_t from) {
    while (from < d.size()) {
        const void* p = std::memchr(d.data() + from, '\r', d.size() - from);
        if (!p) return std::string_view::npos;
        const std::size_t cr = static_cast<std::size_t>(static_cast<const char*>(p) - d.data());
        if (cr + 1 >= d.size()) return std::string_view::npos;
        if (d[cr + 1] == '\n') return cr;
        from = cr + 1;
    }
    return std::string_view::npos;
}

// The parser: every count and length line goes through find_crlf and parse_ll.
inline parse_status next(const limits& lim, std::string_view d, std::size_t& pos, command& cmd,
                         std::string& error) {
    auto fail = [&](std::string msg) {
        error = std::move(msg);
        return parse_status::error;
    };
    for (;;) {
        if (pos >= d.size()) return parse_status::incomplete;
        std::size_t p = pos;
        if (d[p] != '*') {
            const void* nl = std::memchr(d.data() + p, '\n', d.size() - p);
            if (!nl) {
                if (d.size() - p > lim.max_inline) return fail("Protocol error: too big inline request");
                return parse_status::incomplete;
            }
            const std::size_t end = static_cast<std::size_t>(static_cast<const char*>(nl) - d.data());
            if (end - p > lim.max_inline) return fail("Protocol error: too big inline request");
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
            if (cmd.args.empty()) continue;
            return parse_status::complete;
        }
        const std::size_t cr = find_crlf(d, p + 1);
        if (cr == std::string_view::npos) {
            if (d.size() - p > lim.max_inline) return fail("Protocol error: too big mbulk count string");
            return parse_status::incomplete;
        }
        long long n = 0;
        if (!parse_ll(d.substr(p + 1, cr - p - 1), n) || n > static_cast<long long>(lim.max_args))
            return fail("Protocol error: invalid multibulk length");
        p = cr + 2;
        if (n <= 0) {
            pos = p;
            continue;
        }
        cmd.args.clear();
        for (long long k = 0; k < n; ++k) {
            if (p >= d.size()) return parse_status::incomplete;
            if (d[p] != '$') {
                const char c = d[p];
                return fail(std::string("Protocol error: expected '$', got '") + c + "'");
            }
            const std::size_t lcr = find_crlf(d, p + 1);
            if (lcr == std::string_view::npos) {
                if (d.size() - p > lim.max_inline) return fail("Protocol error: too big bulk count string");
                return parse_status::incomplete;
            }
            long long len = 0;
            if (!parse_ll(d.substr(p + 1, lcr - p - 1), len) || len < 0 ||
                static_cast<unsigned long long>(len) > lim.max_bulk)
                return fail("Protocol error: invalid bulk length");
            const std::size_t start = lcr + 2;
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

// The shard: a node-based std::unordered_map.
class shard {
public:
    void execute(cmd_id id, std::span<const std::string_view> a, std::string& out) {
        auto ieq = [](std::string_view x, std::string_view upper) {
            if (x.size() != upper.size()) return false;
            for (std::size_t i = 0; i < x.size(); ++i) {
                const char c = (x[i] >= 'a' && x[i] <= 'z') ? static_cast<char>(x[i] - 32) : x[i];
                if (c != upper[i]) return false;
            }
            return true;
        };
        switch (id) {
        case cmd_id::get: {
            auto it = map_.find(a[1]);
            if (it == map_.end()) reply_nil(out);
            else reply_bulk(out, it->second);
            return;
        }
        case cmd_id::set: {
            bool nx = false, xx = false;
            for (std::size_t i = 3; i < a.size(); ++i) {
                if (ieq(a[i], "NX")) nx = true;
                else if (ieq(a[i], "XX")) xx = true;
                else {
                    reply_error(out, "ERR syntax error");
                    return;
                }
            }
            if (nx && xx) {
                reply_error(out, "ERR syntax error");
                return;
            }
            auto it = map_.find(a[1]);
            if ((nx && it != map_.end()) || (xx && it == map_.end())) {
                reply_nil(out);
                return;
            }
            if (it != map_.end()) it->second.assign(a[2]);
            else map_.emplace(std::string(a[1]), std::string(a[2]));
            reply_simple(out, "OK");
            return;
        }
        case cmd_id::del: {
            long long n = 0;
            for (std::size_t i = 1; i < a.size(); ++i) {
                auto it = map_.find(a[i]);
                if (it != map_.end()) {
                    map_.erase(it);
                    ++n;
                }
            }
            reply_int(out, n);
            return;
        }
        case cmd_id::exists: {
            long long n = 0;
            for (std::size_t i = 1; i < a.size(); ++i) n += map_.find(a[i]) != map_.end();
            reply_int(out, n);
            return;
        }
        case cmd_id::dbsize:
            reply_int(out, static_cast<long long>(map_.size()));
            return;
        case cmd_id::flushall:
            map_.clear();
            reply_simple(out, "OK");
            return;
        default:
            reply_error(out, "ERR not a shard command");
            return;
        }
    }
    std::size_t size() const noexcept { return map_.size(); }

private:
    struct hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    std::unordered_map<std::string, std::string, hash, std::equal_to<>> map_;
};

} // namespace kv::reference
