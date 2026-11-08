// libFuzzer target for the RESP request parser and the routing plan.
// Input: byte 0 picks a chunk size, the rest is the byte stream a connection
// receives. Checks that parsing it whole and parsing it in chunks (keeping the
// unparsed tail between reads, as the servers do) give the same commands and
// the same error outcome, then routes the commands over two shards and
// assembles the replies. Small limits make the limit checks reachable.
#include "kv/engine.hpp"
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
    return 0;
}
