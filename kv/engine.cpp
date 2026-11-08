#include "kv/engine.hpp"
#include <cerrno>
#include <cstdlib>
#include <string>

namespace kv {

unsigned owner(std::string_view key, unsigned cores) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const char ch : key) {
        h ^= static_cast<unsigned char>(ch);
        h *= 1099511628211ull;
    }
    return static_cast<unsigned>((h ^ (h >> 32)) % cores);
}

namespace {

char upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

bool ieq(std::string_view a, std::string_view upper_word) noexcept {
    if (a.size() != upper_word.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper_word[i]) return false;
    return true;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

cmd_id classify(std::string_view n) noexcept {
    switch (n.size()) {
    case 3:
        if (ieq(n, "GET")) return cmd_id::get;
        if (ieq(n, "SET")) return cmd_id::set;
        if (ieq(n, "DEL")) return cmd_id::del;
        break;
    case 4:
        if (ieq(n, "PING")) return cmd_id::ping;
        if (ieq(n, "ECHO")) return cmd_id::echo;
        if (ieq(n, "INFO")) return cmd_id::info;
        if (ieq(n, "QUIT")) return cmd_id::quit;
        break;
    case 5:
        if (ieq(n, "DEBUG")) return cmd_id::debug;
        break;
    case 6:
        if (ieq(n, "EXISTS")) return cmd_id::exists;
        if (ieq(n, "DBSIZE")) return cmd_id::dbsize;
        if (ieq(n, "CONFIG")) return cmd_id::config;
        break;
    case 7:
        if (ieq(n, "COMMAND")) return cmd_id::command;
        break;
    case 8:
        if (ieq(n, "FLUSHALL")) return cmd_id::flushall;
        break;
    }
    return cmd_id::unknown;
}

// Arity as Valkey states it: n > 0 exactly n arguments, n < 0 at least -n.
int arity(cmd_id id) noexcept {
    switch (id) {
    case cmd_id::ping: return -1;
    case cmd_id::echo: return 2;
    case cmd_id::get: return 2;
    case cmd_id::set: return -3;
    case cmd_id::del: return -2;
    case cmd_id::exists: return -2;
    case cmd_id::dbsize: return 1;
    case cmd_id::flushall: return -1;
    case cmd_id::config: return -2;
    case cmd_id::command: return -1;
    case cmd_id::info: return -1;
    case cmd_id::quit: return -1;
    case cmd_id::debug: return -2;
    case cmd_id::unknown: return -1;
    }
    return -1;
}

void wrong_arity(std::string& out, std::string_view name) {
    reply_error(out, "ERR wrong number of arguments for '" + lower(name) + "' command");
}

} // namespace

// ---------------------------------------------------------------- shard

void shard::execute(cmd_id id, std::span<const std::string_view> a, std::string& out) {
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

// ---------------------------------------------------------------- batch

void batch::clear() noexcept {
    ids_.clear();
    argc_.clear();
    arg_ends_.clear();
    bytes_.clear();
    replies_.clear();
    reply_ends_.clear();
}

std::uint32_t batch::add(cmd_id id, std::span<const std::string_view> args) {
    ids_.push_back(id);
    argc_.push_back(static_cast<std::uint32_t>(args.size()));
    for (auto a : args) {
        bytes_.append(a);
        arg_ends_.push_back(bytes_.size());
    }
    return static_cast<std::uint32_t>(ids_.size() - 1);
}

void batch::execute(shard& s) {
    replies_.clear();
    reply_ends_.clear();
    const std::string_view all(bytes_);
    std::size_t arg = 0, off = 0;
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        scratch_.clear();
        for (std::uint32_t k = 0; k < argc_[i]; ++k) {
            const std::size_t end = arg_ends_[arg++];
            scratch_.push_back(all.substr(off, end - off));
            off = end;
        }
        s.execute(ids_[i], scratch_, replies_);
        reply_ends_.push_back(replies_.size());
    }
}

std::string_view batch::reply(std::uint32_t i) const noexcept {
    const std::size_t start = i ? reply_ends_[i - 1] : 0;
    return std::string_view(replies_).substr(start, reply_ends_[i] - start);
}

// ---------------------------------------------------------------- plan

plan::plan(unsigned core, unsigned cores, shard& own, const server_info& info)
    : core_(core), cores_(cores), own_(&own), info_(&info), batches_(cores) {
    for (unsigned c = 0; c < cores; ++c)
        if (c != core) batches_[c] = std::make_unique<batch>();
}

void plan::begin() noexcept {
    for (unsigned t : targets_) batches_[t]->clear();
    targets_.clear();
    parts_.clear();
    entries_.clear();
    local_.clear();
    local_ends_.clear();
    close_after_ = false;
    sleep_ = 0;
}

void plan::route(cmd_id id, unsigned core, std::span<const std::string_view> args) {
    if (core == core_) {
        own_->execute(id, args, local_);
        local_ends_.push_back(local_.size());
        parts_.push_back({core_, static_cast<std::uint32_t>(local_ends_.size() - 1)});
        return;
    }
    batch& b = *batches_[core];
    if (b.size() == 0) targets_.push_back(core);
    parts_.push_back({core, b.add(id, args)});
}

void plan::local_end() {
    local_ends_.push_back(local_.size());
    const auto first = static_cast<std::uint32_t>(parts_.size());
    parts_.push_back({core_, static_cast<std::uint32_t>(local_ends_.size() - 1)});
    entries_.push_back({combine::copy, first, 1});
}

void plan::add_protocol_error(const std::string& msg) {
    reply_error(local_begin(), "ERR " + msg);
    local_end();
    close_after_ = true;
}

void plan::add(const command& cmd) {
    const auto& a = cmd.args;
    const cmd_id id = classify(a[0]);
    const int ar = arity(id);
    if (id != cmd_id::unknown &&
        ((ar > 0 && a.size() != static_cast<std::size_t>(ar)) ||
         (ar < 0 && a.size() < static_cast<std::size_t>(-ar)) || (id == cmd_id::ping && a.size() > 2))) {
        wrong_arity(local_begin(), a[0]);
        local_end();
        return;
    }
    const auto first = static_cast<std::uint32_t>(parts_.size());
    switch (id) {
    case cmd_id::get:
    case cmd_id::set:
        route(id, owner(a[1], cores_), a);
        entries_.push_back({combine::copy, first, 1});
        return;
    case cmd_id::del:
    case cmd_id::exists: {
        const unsigned o = owner(a[1], cores_);
        bool one_owner = true;
        for (std::size_t i = 2; i < a.size() && one_owner; ++i) one_owner = owner(a[i], cores_) == o;
        if (one_owner) {
            route(id, o, a);
            entries_.push_back({combine::copy, first, 1});
            return;
        }
        // Keys on several cores: one single-key command per key, replies summed.
        for (std::size_t i = 1; i < a.size(); ++i) {
            const std::string_view pair[2] = {a[0], a[i]};
            route(id, owner(a[i], cores_), pair);
        }
        entries_.push_back({combine::sum, first, static_cast<std::uint32_t>(a.size() - 1)});
        return;
    }
    case cmd_id::dbsize:
    case cmd_id::flushall: {
        const std::string_view name[1] = {a[0]};
        for (unsigned c = 0; c < cores_; ++c) route(id, c, name);
        entries_.push_back(
            {id == cmd_id::dbsize ? combine::sum : combine::all_ok, first, cores_});
        return;
    }
    case cmd_id::ping:
        if (a.size() == 1) reply_simple(local_begin(), "PONG");
        else reply_bulk(local_begin(), a[1]);
        local_end();
        return;
    case cmd_id::echo:
        reply_bulk(local_begin(), a[1]);
        local_end();
        return;
    case cmd_id::config:
        if (ieq(a[1], "GET")) reply_array(local_begin(), 0);
        else reply_error(local_begin(), "ERR unknown subcommand '" + std::string(a[1]) + "'. Try CONFIG HELP.");
        local_end();
        return;
    case cmd_id::command:
        reply_array(local_begin(), 0);
        local_end();
        return;
    case cmd_id::info: {
        long clients = 0;
        if (info_->connections)
            for (unsigned c = 0; c < info_->cores; ++c)
                clients += info_->connections[c].load(std::memory_order_relaxed);
        std::string text = "# Server\r\nserver:rol-kv\r\nio:" + std::string(info_->io) +
                           "\r\ncores:" + std::to_string(info_->cores) +
                           "\r\n\r\n# Clients\r\nconnected_clients:" + std::to_string(clients) + "\r\n";
        reply_bulk(local_begin(), text);
        local_end();
        return;
    }
    case cmd_id::quit:
        reply_simple(local_begin(), "OK");
        local_end();
        close_after_ = true;
        return;
    case cmd_id::debug: {
        if (ieq(a[1], "SLEEP") && a.size() == 3) {
            if (!info_->supports_sleep) {
                reply_error(local_begin(), "ERR DEBUG SLEEP is not supported by this server");
            } else {
                const std::string arg(a[2]);
                char* end = nullptr;
                errno = 0;
                const double s = std::strtod(arg.c_str(), &end);
                if (errno || end != arg.c_str() + arg.size() || !(s >= 0) || s > 60) {
                    reply_error(local_begin(), "ERR value is not a valid float");
                } else {
                    sleep_ += s;
                    reply_simple(local_begin(), "OK");
                }
            }
        } else {
            reply_error(local_begin(), "ERR unknown subcommand '" + std::string(a[1]) + "'");
        }
        local_end();
        return;
    }
    case cmd_id::unknown: {
        std::string args;
        for (std::size_t i = 1; i < a.size() && args.size() < 128; ++i)
            args += "'" + std::string(a[i].substr(0, 128 - args.size())) + "' ";
        std::string msg = "ERR unknown command '" + std::string(a[0].substr(0, 128)) +
                          "', with args beginning with: " + args;
        for (auto& c : msg)
            if (c == '\r' || c == '\n') c = ' ';
        reply_error(local_begin(), msg);
        local_end();
        return;
    }
    }
}

std::string_view plan::part_reply(const part& p) const noexcept {
    if (p.core == core_) {
        const std::size_t start = p.index ? local_ends_[p.index - 1] : 0;
        return std::string_view(local_).substr(start, local_ends_[p.index] - start);
    }
    return batches_[p.core]->reply(p.index);
}

void plan::assemble(std::string& out) {
    for (const auto& e : entries_) {
        if (e.how == combine::copy) {
            out += part_reply(parts_[e.first]);
            continue;
        }
        long long total = 0;
        bool failed = false;
        for (std::uint32_t i = 0; i < e.count && !failed; ++i) {
            const auto r = part_reply(parts_[e.first + i]);
            if (!r.empty() && r[0] == '-') {
                out += r;
                failed = true;
            } else if (e.how == combine::sum) {
                long long v = 0;
                if (r.size() >= 3 && r[0] == ':' && parse_ll(r.substr(1, r.size() - 3), v)) total += v;
            }
        }
        if (failed) continue;
        if (e.how == combine::sum) reply_int(out, total);
        else reply_simple(out, "OK");
    }
}

bool consume(parser& ps, command& cmd, plan& p, std::string_view data, std::size_t& pos) {
    for (;;) {
        switch (ps.next(data, pos, cmd)) {
        case parse_status::complete:
            p.add(cmd);
            if (p.close_after()) return true;
            break;
        case parse_status::incomplete:
            return true;
        case parse_status::error:
            p.add_protocol_error(ps.error());
            return false;
        }
    }
}

} // namespace kv
