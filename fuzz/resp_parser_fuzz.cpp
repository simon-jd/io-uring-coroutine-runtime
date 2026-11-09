// libFuzzer target for the RESP request parser and the routing plan.
// Input: byte 0 picks a chunk size, the rest is the byte stream a connection
// receives. Checks that parsing it whole and parsing it in chunks (keeping the
// unparsed tail between reads, as the servers do) give the same commands and
// the same error outcome, and that the parser gives the same commands, outcome
// and error message as the reference parser (tests/kv_reference.hpp). Then it
// routes the commands over two shards and assembles the replies, and runs the
// shard commands on one shard and on the reference map, which must answer
// alike. Small limits make the limit checks reachable.
#include "kv/engine.hpp"
#include "tests/kv_reference.hpp"
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct outcome {
    std::vector<std::vector<std::string>> commands;
    bool error = false;
    bool operator==(const outcome&) const = default;
};

constexpr kv::limits fuzz_limits{512, 32, 128};

outcome parse_whole(std::string_view in) {
    kv::parser p(fuzz_limits);
    kv::command c;
    outcome out;
    std::size_t pos = 0;
    for (;;) {
        const auto st = p.next(in, pos, c);
        if (st == kv::parse_status::complete) {
            out.commands.emplace_back(c.args.begin(), c.args.end());
            continue;
        }
        out.error = st == kv::parse_status::error;
        return out;
    }
}

// Whole-stream parse with both parsers: commands, final status, error text.
bool matches_reference(std::string_view in) {
    kv::parser p(fuzz_limits);
    kv::command c, rc;
    std::size_t pos = 0, rpos = 0;
    std::string rerror;
    for (;;) {
        const auto st = p.next(in, pos, c);
        const auto rst = kv::reference::next(fuzz_limits, in, rpos, rc, rerror);
        if (st != rst || pos != rpos) return false;
        if (st == kv::parse_status::error) return p.error() == rerror;
        if (st == kv::parse_status::incomplete) return true;
        if (c.args != rc.args) return false;
    }
}

outcome parse_chunked(std::string_view in, std::size_t step) {
    kv::parser p(fuzz_limits);
    kv::command c;
    outcome out;
    std::string buf;
    for (std::size_t from = 0; from < in.size(); from += step) {
        buf.append(in.substr(from, step));
        std::size_t pos = 0;
        for (;;) {
            const auto st = p.next(buf, pos, c);
            if (st == kv::parse_status::complete) {
                out.commands.emplace_back(c.args.begin(), c.args.end());
                continue;
            }
            if (st == kv::parse_status::error) {
                out.error = true;
                return out;
            }
            break;
        }
        buf.erase(0, pos);
    }
    return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) return 0;
    const std::size_t step = 1 + data[0] % 64;
    const std::string_view in(reinterpret_cast<const char*>(data + 1), size - 1);
    const outcome whole = parse_whole(in);
    if (!(parse_chunked(in, step) == whole)) std::abort();
    if (!matches_reference(in)) std::abort();

    // Route everything through a plan on core 0 of two shards.
    kv::shard shards[2];
    std::atomic<long> conns[2] = {0, 0};
    kv::server_info info;
    info.io = "fuzz";
    info.cores = 2;
    info.connections = conns;
    kv::plan plan(0, 2, shards[0], info);
    kv::parser p(fuzz_limits);
    kv::command c;
    std::size_t pos = 0;
    plan.begin();
    kv::consume(p, c, plan, in, pos);
    for (unsigned t : plan.targets()) {
        auto b = plan.take(t);
        b->execute(shards[t]);
        plan.give_back(t, std::move(b));
    }
    std::string out;
    plan.assemble(out);
    if (plan.commands() > 0 && out.empty()) std::abort();

    // Shard commands straight on one shard and on the reference map.
    kv::shard fast;
    kv::reference::shard ref;
    for (const auto& args : whole.commands) {
        if (args.empty() || args.size() > 6) continue;
        std::string name = args[0];
        for (auto& ch : name) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        kv::cmd_id id;
        std::size_t min = 2;
        if (name == "GET") { id = kv::cmd_id::get; if (args.size() != 2) continue; }
        else if (name == "SET") { id = kv::cmd_id::set; min = 3; }
        else if (name == "DEL") id = kv::cmd_id::del;
        else if (name == "EXISTS") id = kv::cmd_id::exists;
        else if (name == "DBSIZE") { id = kv::cmd_id::dbsize; min = 1; }
        else if (name == "FLUSHALL") { id = kv::cmd_id::flushall; min = 1; }
        else continue;
        if (args.size() < min) continue;
        std::vector<std::string_view> views(args.begin(), args.end());
        std::string a, b;
        fast.execute(id, views, a);
        ref.execute(id, views, b);
        if (a != b || fast.size() != ref.size()) std::abort();
    }
    if (!fast.consistent()) std::abort();
    return 0;
}
