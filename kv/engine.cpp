#include "kv/engine.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>

namespace kv {

namespace {

__extension__ typedef unsigned __int128 u128;

std::uint64_t load64(const unsigned char* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
std::uint64_t load32(const unsigned char* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
// 64 x 64 -> 128-bit multiply, folded.
std::uint64_t fold(std::uint64_t a, std::uint64_t b) noexcept {
    const u128 r = static_cast<u128>(a) * b;
    return static_cast<std::uint64_t>(r) ^ static_cast<std::uint64_t>(r >> 64);
}
constexpr std::uint64_t k0 = 0xa0761d6478bd642full, k1 = 0xe7037ed1a0b428dbull,
                        k2 = 0x8ebc6af09c88c6e3ull;

} // namespace

// Multiply-fold hash in the style of wyhash: keys up to 16 bytes take two
// (possibly overlapping) loads and two multiplies.
std::uint64_t key_hash(std::string_view key) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(key.data());
    const std::size_t n = key.size();
    std::uint64_t seed = k0 ^ static_cast<std::uint64_t>(n);
    std::uint64_t a = 0, b = 0;
    if (n <= 16) {
        if (n >= 8) {
            a = load64(p);
            b = load64(p + n - 8);
        } else if (n >= 4) {
            a = load32(p);
            b = load32(p + n - 4);
        } else if (n > 0) {
            a = (std::uint64_t{p[0]} << 16) | (std::uint64_t{p[n >> 1]} << 8) | p[n - 1];
        }
    } else {
        std::size_t i = 0;
        for (; n - i > 16; i += 16) seed = fold(load64(p + i) ^ k1, load64(p + i + 8) ^ seed);
        a = load64(p + n - 16);
        b = load64(p + n - 8);
    }
    return fold(k1 ^ static_cast<std::uint64_t>(n), fold(a ^ k1, b ^ seed) ^ k2);
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

// One heap block per key: this header, the key bytes, then the value bytes
// (capacity cap; a SET that fits overwrites in place).
struct shard::entry {
    std::size_t key_len;
    std::size_t val_len;
    std::size_t val_cap;
    char* key() noexcept { return reinterpret_cast<char*>(this + 1); }
    char* val() noexcept { return key() + key_len; }
    std::string_view key_view() noexcept { return {key(), key_len}; }
    std::string_view val_view() noexcept { return {val(), val_len}; }

    static entry* make(std::string_view k, std::string_view v) {
        const std::size_t cap = (v.size() + 7) & ~std::size_t{7};
        void* mem = ::operator new(sizeof(entry) + k.size() + cap);
        auto* e = static_cast<entry*>(mem);
        e->key_len = k.size();
        e->val_len = v.size();
        e->val_cap = cap;
        if (!k.empty()) std::memcpy(e->key(), k.data(), k.size());
        if (!v.empty()) std::memcpy(e->val(), v.data(), v.size());
        return e;
    }
    static void destroy(entry* e) noexcept { ::operator delete(static_cast<void*>(e)); }
};

shard::shard(shard&& o) noexcept : slots_(std::move(o.slots_)), size_(std::exchange(o.size_, 0)) {
    o.slots_.clear();
}

shard& shard::operator=(shard&& o) noexcept {
    if (this != &o) {
        clear();
        slots_ = std::move(o.slots_);
        size_ = std::exchange(o.size_, 0);
        o.slots_.clear();
    }
    return *this;
}

shard::~shard() { clear(); }

void shard::clear() noexcept {
    for (auto& s : slots_)
        if (s.e) entry::destroy(std::exchange(s.e, nullptr));
    slots_.clear();
    size_ = 0;
}

std::size_t shard::probe(std::string_view key, std::uint64_t h) const noexcept {
    const std::size_t mask = slots_.size() - 1;
    for (std::size_t i = static_cast<std::size_t>(h) & mask;; i = (i + 1) & mask) {
        const slot& s = slots_[i];
        if (!s.e) return i;
        if (s.hash == h && s.e->key_view() == key) return i;
    }
}

void shard::grow() {
    std::vector<slot> bigger(slots_.empty() ? 16 : slots_.size() * 2);
    const std::size_t mask = bigger.size() - 1;
    for (const auto& s : slots_) {
        if (!s.e) continue;
        std::size_t i = static_cast<std::size_t>(s.hash) & mask;
        while (bigger[i].e) i = (i + 1) & mask;
        bigger[i] = s;
    }
    slots_.swap(bigger);
}

void shard::erase_at(std::size_t i) noexcept {
    entry::destroy(slots_[i].e);
    --size_;
    // Backward shift: pull later members of the probe run into the hole when
    // the hole lies between their home slot and where they are now.
    const std::size_t mask = slots_.size() - 1;
    for (std::size_t j = (i + 1) & mask; slots_[j].e; j = (j + 1) & mask) {
        const std::size_t home = static_cast<std::size_t>(slots_[j].hash) & mask;
        if (((j - home) & mask) >= ((j - i) & mask)) {
            slots_[i] = slots_[j];
            i = j;
        }
    }
    slots_[i] = slot{};
}

bool shard::consistent() const {
    std::size_t n = 0;
    const std::size_t mask = slots_.empty() ? 0 : slots_.size() - 1;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].e) continue;
        ++n;
        if (slots_[i].hash != key_hash(slots_[i].e->key_view())) return false;
        for (std::size_t j = static_cast<std::size_t>(slots_[i].hash) & mask; j != i; j = (j + 1) & mask)
            if (!slots_[j].e) return false; // an empty slot between home and entry
    }
    return n == size_ && size_ * 2 <= slots_.size();
}

void shard::execute(cmd_id id, std::span<const std::string_view> a, std::string& out) {
    if (id == cmd_id::get || id == cmd_id::set) execute_hashed(id, a, key_hash(a[1]), out);
    else execute_other(id, a, out);
}

void shard::execute_hashed(cmd_id id, std::span<const std::string_view> a, std::uint64_t h,
                           std::string& out) {
    switch (id) {
    case cmd_id::get: {
        if (size_ == 0) {
            reply_nil(out);
            return;
        }
        const slot& s = slots_[probe(a[1], h)];
        if (!s.e) reply_nil(out);
        else reply_bulk(out, s.e->val_view());
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
        if ((size_ + 1) * 2 > slots_.size()) grow();
        slot& s = slots_[probe(a[1], h)];
        if ((nx && s.e) || (xx && !s.e)) {
            reply_nil(out);
            return;
        }
        const std::string_view v = a[2];
        if (!s.e) {
            s.e = entry::make(a[1], v);
            s.hash = h;
            ++size_;
        } else if (v.size() <= s.e->val_cap) {
            if (!v.empty()) std::memmove(s.e->val(), v.data(), v.size());
            s.e->val_len = v.size();
        } else {
            entry* bigger = entry::make(a[1], v);
            entry::destroy(std::exchange(s.e, bigger));
        }
        reply_simple(out, "OK");
        return;
    }
    default:
        execute_other(id, a, out);
        return;
    }
}

void shard::execute_other(cmd_id id, std::span<const std::string_view> a, std::string& out) {
    switch (id) {
    case cmd_id::del: {
        long long n = 0;
        for (std::size_t i = 1; i < a.size() && size_ > 0; ++i) {
            const std::size_t at = probe(a[i], key_hash(a[i]));
            if (slots_[at].e) {
                erase_at(at);
                ++n;
            }
        }
        reply_int(out, n);
        return;
    }
    case cmd_id::exists: {
        long long n = 0;
        for (std::size_t i = 1; i < a.size() && size_ > 0; ++i)
            n += slots_[probe(a[i], key_hash(a[i]))].e != nullptr;
        reply_int(out, n);
        return;
    }
    case cmd_id::dbsize:
        reply_int(out, static_cast<long long>(size_));
        return;
    case cmd_id::flushall:
        clear();
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
    hashes_.clear();
    argc_.clear();
    arg_ends_.clear();
    bytes_.clear();
    replies_.clear();
    reply_ends_.clear();
}

std::uint32_t batch::add(cmd_id id, std::span<const std::string_view> args, std::uint64_t h) {
    ids_.push_back(id);
    hashes_.push_back(h);
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
        if (ids_[i] == cmd_id::get || ids_[i] == cmd_id::set)
            s.execute_hashed(ids_[i], scratch_, hashes_[i], replies_);
        else
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

void plan::route_hashed(cmd_id id, std::uint64_t h, std::span<const std::string_view> args) {
    const unsigned core = owner_of_hash(h, cores_);
    if (core == core_) {
        own_->execute_hashed(id, args, h, local_);
        local_ends_.push_back(local_.size());
        parts_.push_back({core_, static_cast<std::uint32_t>(local_ends_.size() - 1)});
        return;
    }
    batch& b = *batches_[core];
    if (b.size() == 0) targets_.push_back(core);
    parts_.push_back({core, b.add(id, args, h)});
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
        route_hashed(id, key_hash(a[1]), a);
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
