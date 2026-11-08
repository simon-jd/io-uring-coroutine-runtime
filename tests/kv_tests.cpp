// RESP parser and routing plan, without I/O.
#include "kv/engine.hpp"
#include "test.hpp"
#include <limits>
#include <random>
#include <string>
#include <vector>
using namespace kv;

namespace {

std::string resp(std::initializer_list<std::string_view> args) {
    std::string s = "*" + std::to_string(args.size()) + "\r\n";
    for (auto a : args) s += "$" + std::to_string(a.size()) + "\r\n" + std::string(a) + "\r\n";
    return s;
}

using cmd_list = std::vector<std::vector<std::string>>;

// Parses everything in `data` at once.
cmd_list parse_all(std::string_view data, parse_status* last = nullptr) {
    parser p;
    command c;
    cmd_list out;
    std::size_t pos = 0;
    for (;;) {
        auto st = p.next(data, pos, c);
        if (st != parse_status::complete) {
            if (last) *last = st;
            return out;
        }
        out.emplace_back(c.args.begin(), c.args.end());
    }
}

// Feeds `data` the way a connection receives it: in chunks, keeping the
// unparsed tail between reads.
cmd_list parse_chunked(std::string_view data, const std::vector<std::size_t>& cuts) {
    parser p;
    command c;
    cmd_list out;
    std::string buf;
    std::size_t from = 0;
    auto feed = [&](std::string_view chunk) {
        buf.append(chunk);
        std::size_t pos = 0;
        while (p.next(buf, pos, c) == parse_status::complete) out.emplace_back(c.args.begin(), c.args.end());
        buf.erase(0, pos);
    };
    for (auto cut : cuts) {
        feed(data.substr(from, cut - from));
        from = cut;
    }
    feed(data.substr(from));
    CHECK(buf.empty());
    return out;
}

std::string error_of(std::string_view data, limits l = {}) {
    parser p(l);
    command c;
    std::size_t pos = 0;
    while (true) {
        auto st = p.next(data, pos, c);
        if (st == parse_status::error) return p.error();
        if (st == parse_status::incomplete) return "incomplete";
    }
}

struct cluster {
    explicit cluster(unsigned n) : shards(n), counts(new std::atomic<long>[n]) {
        info.io = "test";
        info.cores = n;
        for (unsigned i = 0; i < n; ++i) counts[i] = 0;
        info.connections = counts.get();
    }
    std::vector<shard> shards;
    std::unique_ptr<std::atomic<long>[]> counts;
    server_info info;
    // Runs one read's worth of input through a plan on `core`, executing
    // other cores' batches on their shards the way a front end would.
    std::string run(plan& p, std::string_view input) {
        parser ps;
        command c;
        std::size_t pos = 0;
        p.begin();
        consume(ps, c, p, input, pos);
        for (unsigned t : p.targets()) {
            auto b = p.take(t);
            b->execute(shards[t]);
            p.give_back(t, std::move(b));
        }
        std::string out;
        p.assemble(out);
        return out;
    }
};

} // namespace

int main(int argc, char** argv) {
    return run(
        {
            {"parses_arrays_and_inline_commands",
             [] {
                 const std::string data = resp({"SET", "key", "va\r\nlue"}) + "PING\r\n" +
                                          "  get   key \n" + "*0\r\n" + "\r\n" + resp({"DEL", ""});
                 parse_status last{};
                 const auto cmds = parse_all(data, &last);
                 CHECK(last == parse_status::incomplete);
                 CHECK(cmds.size() == 4);
                 CHECK((cmds[0] == std::vector<std::string>{"SET", "key", "va\r\nlue"}));
                 CHECK((cmds[1] == std::vector<std::string>{"PING"}));
                 CHECK((cmds[2] == std::vector<std::string>{"get", "key"}));
                 CHECK((cmds[3] == std::vector<std::string>{"DEL", ""}));
             }},
            {"split_input_parses_identically",
             [] {
                 std::string data;
                 for (int i = 0; i < 40; ++i) {
                     data += resp({"SET", "k" + std::to_string(i), std::string(static_cast<std::size_t>(i * 7), 'x')});
                     data += "PING hello\r\n";
                     data += resp({"GET", "k" + std::to_string(i)});
                     data += "*0\r\n";
                 }
                 const auto whole = parse_all(data);
                 CHECK(whole.size() == 120);
                 // Every single cut point, then many random multi-cut splits.
                 for (std::size_t cut = 1; cut < data.size(); ++cut)
                     REQUIRE("split input parses to the same commands",
                             parse_chunked(data, {cut}) == whole);
                 std::mt19937 rng(7);
                 for (int round = 0; round < 300; ++round) {
                     std::vector<std::size_t> cuts;
                     std::size_t at = 0;
                     while (true) {
                         at += 1 + rng() % 40;
                         if (at >= data.size()) break;
                         cuts.push_back(at);
                     }
                     REQUIRE("split input parses to the same commands",
                             parse_chunked(data, cuts) == whole);
                 }
             }},
            {"incomplete_input_waits",
             [] {
                 for (std::string partial : {"*", "*3", "*3\r", "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nval",
                                             "*1\r\n$4\r\nPING\r", "PING"}) {
                     parser p;
                     command c;
                     std::size_t pos = 0;
                     CHECK(p.next(partial, pos, c) == parse_status::incomplete);
                     CHECK(pos == 0);
                 }
             }},
            {"protocol_errors",
             [] {
                 CHECK(error_of("*abc\r\n") == "Protocol error: invalid multibulk length");
                 CHECK(error_of("*2\r\n+x\r\n") == "Protocol error: expected '$', got '+'");
                 CHECK(error_of("*1\r\n$-5\r\n") == "Protocol error: invalid bulk length");
                 CHECK(error_of("*1\r\n$01\r\nx\r\n") == "Protocol error: invalid bulk length");
                 CHECK(error_of("*1\r\n$3\r\nabcXY") ==
                       "Protocol error: bulk string not terminated by CRLF");
                 limits small;
                 small.max_bulk = 16;
                 small.max_args = 4;
                 small.max_inline = 32;
                 CHECK(error_of("*1\r\n$17\r\n", small) == "Protocol error: invalid bulk length");
                 CHECK(error_of("*5\r\n", small) == "Protocol error: invalid multibulk length");
                 CHECK(error_of(std::string(40, 'a'), small) == "Protocol error: too big inline request");
                 CHECK(error_of("*" + std::string(40, '1'), small) ==
                       "Protocol error: too big mbulk count string");
                 long long v = 0;
                 CHECK(parse_ll("-9223372036854775808", v) && v == std::numeric_limits<long long>::min());
                 CHECK(!parse_ll("9223372036854775808", v));
                 CHECK(!parse_ll("+1", v) && !parse_ll("", v) && !parse_ll("-", v) && !parse_ll("1a", v));
             }},
            {"local_commands_and_errors",
             [] {
                 cluster cl(1);
                 plan p(0, 1, cl.shards[0], cl.info);
                 CHECK(cl.run(p, "PING\r\n") == "+PONG\r\n");
                 CHECK(cl.run(p, resp({"ping", "hi"})) == "$2\r\nhi\r\n");
                 CHECK(cl.run(p, resp({"PING", "a", "b"})) ==
                       "-ERR wrong number of arguments for 'ping' command\r\n");
                 CHECK(cl.run(p, resp({"GET"})) == "-ERR wrong number of arguments for 'get' command\r\n");
                 CHECK(cl.run(p, resp({"NOPE", "x"})) ==
                       "-ERR unknown command 'NOPE', with args beginning with: 'x' \r\n");
                 CHECK(cl.run(p, resp({"SET", "k", "v", "EX", "10"})) == "-ERR syntax error\r\n");
                 CHECK(cl.run(p, resp({"SET", "k", "v", "XX"})) == "$-1\r\n");
                 CHECK(cl.run(p, resp({"SET", "k", "v", "NX"})) == "+OK\r\n");
                 CHECK(cl.run(p, resp({"SET", "k", "w", "NX"})) == "$-1\r\n");
                 CHECK(cl.run(p, resp({"GET", "k"})) == "$1\r\nv\r\n");
                 CHECK(cl.run(p, resp({"CONFIG", "GET", "save"})) == "*0\r\n");
                 CHECK(cl.run(p, resp({"DEBUG", "SLEEP", "0"})) ==
                       "-ERR DEBUG SLEEP is not supported by this server\r\n");
                 CHECK(cl.run(p, resp({"QUIT"}) + "PING\r\n") == "+OK\r\n");
                 CHECK(p.close_after());
             }},
            {"routes_keys_to_their_cores",
             [] {
                 constexpr unsigned cores = 4;
                 cluster cl(cores);
                 plan p(1, cores, cl.shards[1], cl.info);
                 // Keys whose owners cover every core.
                 std::vector<std::string> keys;
                 for (int i = 0; keys.size() < 16; ++i) keys.push_back("key:" + std::to_string(i));
                 std::string in, expect;
                 for (auto& k : keys) {
                     in += resp({"SET", k, "v-" + k});
                     expect += "+OK\r\n";
                 }
                 for (auto& k : keys) {
                     in += resp({"GET", k});
                     expect += "$" + std::to_string(2 + k.size()) + "\r\nv-" + k + "\r\n";
                 }
                 CHECK(cl.run(p, in) == expect);
                 std::vector<unsigned> seen(cores, 0);
                 for (auto& k : keys) ++seen[owner(k, cores)];
                 for (unsigned c = 0; c < cores; ++c) {
                     CHECK(seen[c] > 0);
                     CHECK(cl.shards[c].size() == seen[c]); // each key stored on its owner only
                 }
                 // Multi-key DEL across cores sums the parts; DBSIZE sums every shard.
                 CHECK(cl.run(p, resp({"DBSIZE"})) == ":16\r\n");
                 CHECK(cl.run(p, resp({"DEL", keys[0], keys[1], keys[2], keys[3], "missing"})) == ":4\r\n");
                 CHECK(cl.run(p, resp({"EXISTS", keys[4], keys[4], keys[0]})) == ":2\r\n");
                 CHECK(cl.run(p, resp({"FLUSHALL"})) == "+OK\r\n");
                 CHECK(cl.run(p, resp({"DBSIZE"})) == ":0\r\n");
             }},
            {"protocol_error_ends_the_read_with_a_reply",
             [] {
                 cluster cl(2);
                 plan p(0, 2, cl.shards[0], cl.info);
                 const auto out = cl.run(p, "PING\r\n*1\r\n$x\r\nPING\r\n");
                 CHECK(out == "+PONG\r\n-ERR Protocol error: invalid bulk length\r\n");
                 CHECK(p.close_after());
             }},
        },
        argc, argv);
}
