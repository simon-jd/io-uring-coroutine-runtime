#pragma once
// The key-value service logic shared by every front end: command table,
// shard (one per core), the batch a connection sends to another core, and the
// per-read plan that routes commands, executes this core's share and puts the
// replies back in request order.
//
// The keyspace is shared-nothing: key k belongs to core owner(k) and only that
// core's thread touches its shard. A connection on core c executes its own
// core's commands directly and sends every other core one batch per read.
#include "kv/resp.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kv {

enum class cmd_id : std::uint8_t {
    ping, echo, get, set, del, exists, dbsize, flushall, config, command, info, quit, debug, unknown
};

// 64-bit hash of a key, computed once per key and used twice: its upper half
// picks the owning core, its lower bits the slot in that core's table.
std::uint64_t key_hash(std::string_view key) noexcept;

// Which core owns `key`.
inline unsigned owner_of_hash(std::uint64_t h, unsigned cores) noexcept {
    return static_cast<unsigned>(((h >> 32) * cores) >> 32);
}
inline unsigned owner(std::string_view key, unsigned cores) noexcept {
    return owner_of_hash(key_hash(key), cores);
}

// One core's part of the keyspace: an open-addressing table (linear probing,
// at most half full, deletion by backward shift, no tombstones). A slot holds
// the key's hash and a pointer to one heap block with the key and the value,
// so a lookup that hits reads the slot array once and that block once.
class shard {
public:
    shard() = default;
    shard(shard&& o) noexcept;
    shard& operator=(shard&& o) noexcept;
    shard(const shard&) = delete;
    shard& operator=(const shard&) = delete;
    ~shard();

    // Runs a shard command (get, set, del, exists, dbsize, flushall) on keys
    // this shard owns and appends the reply.
    void execute(cmd_id id, std::span<const std::string_view> args, std::string& out);
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }
    // Checks the table's invariants (tests): every entry is reachable from its
    // home slot without crossing an empty slot, and the count matches.
    bool consistent() const;

private:
    struct entry; // header, then the key bytes, then the value bytes
    struct slot {
        std::uint64_t hash = 0;
        entry* e = nullptr; // nullptr: empty
    };
    // The slot holding `key`, or the empty slot where it would go.
    std::size_t probe(std::string_view key, std::uint64_t h) const noexcept;
    void grow();
    void erase_at(std::size_t i) noexcept;
    void clear() noexcept;

    std::vector<slot> slots_; // size 0 or a power of two
    std::size_t size_ = 0;
};

// Commands for one other core, with their argument bytes copied (the input
// buffer they came from may be reused while the batch is away), and the
// replies that core appends. Moved to the target core and back; reused.
class batch {
public:
    void clear() noexcept;
    std::size_t size() const noexcept { return ids_.size(); }
    std::uint32_t add(cmd_id id, std::span<const std::string_view> args);
    void execute(shard& s); // on the owning core
    std::string_view reply(std::uint32_t i) const noexcept;

private:
    std::vector<cmd_id> ids_;
    std::vector<std::uint32_t> argc_;
    std::vector<std::size_t> arg_ends_;
    std::string bytes_;
    std::string replies_;
    std::vector<std::size_t> reply_ends_;
    std::vector<std::string_view> scratch_;
};

// Server-wide facts some commands report.
struct server_info {
    const char* io = "";
    unsigned cores = 1;
    std::atomic<long>* connections = nullptr; // one counter per core
    bool supports_sleep = false;
};

// What one connection's read turned into. Reused for every read.
class plan {
public:
    plan(unsigned core, unsigned cores, shard& own, const server_info& info);

    void begin() noexcept;
    // Routes one command. This core's share runs immediately; other cores'
    // shares go into their batches.
    void add(const command& cmd);
    // A protocol error: reply with it; the connection closes after this read.
    void add_protocol_error(const std::string& msg);

    bool close_after() const noexcept { return close_after_; }
    double sleep_seconds() const noexcept { return sleep_; } // DEBUG SLEEP
    std::size_t commands() const noexcept { return entries_.size(); }

    // Cores (other than this one) with a non-empty batch for this read.
    const std::vector<unsigned>& targets() const noexcept { return targets_; }
    std::unique_ptr<batch> take(unsigned core) noexcept { return std::move(batches_[core]); }
    void give_back(unsigned core, std::unique_ptr<batch> b) noexcept { batches_[core] = std::move(b); }

    // After every batch is back: appends all replies in request order.
    void assemble(std::string& out);

private:
    enum class combine : std::uint8_t { copy, sum, all_ok };
    struct part {
        unsigned core;
        std::uint32_t index;
    };
    struct entry {
        combine how;
        std::uint32_t first;
        std::uint32_t count;
    };
    void route(cmd_id id, unsigned core, std::span<const std::string_view> args);
    std::string& local_begin() noexcept { return local_; }
    void local_end(); // closes one local reply as a single-part entry
    std::string_view part_reply(const part& p) const noexcept;

    unsigned core_;
    unsigned cores_;
    shard* own_;
    const server_info* info_;
    std::vector<std::unique_ptr<batch>> batches_; // [core]; this core's slot is unused
    std::vector<unsigned> targets_;
    std::vector<part> parts_;
    std::vector<entry> entries_;
    std::string local_; // replies to this core's commands
    std::vector<std::size_t> local_ends_;
    std::vector<std::string_view> scratch_;
    bool close_after_ = false;
    double sleep_ = 0;
};

// Parses every complete command in `data` from `pos` into `p`. Returns false on
// a protocol error (already added to the plan as the last reply). Stops early
// after QUIT.
bool consume(parser& ps, command& cmd, plan& p, std::string_view data, std::size_t& pos);

} // namespace kv
