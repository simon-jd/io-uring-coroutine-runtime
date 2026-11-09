// RESP parser and routing plan, without I/O.
#include "kv/engine.hpp"
#include "kv_reference.hpp"
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


// Random byte streams made of RESP fragments, biased towards the number lines
// the parser's fast path handles: lone zeros, leading zeros, signs, 17 to 21
// digits, a CR without LF, lines cut short.
std::string random_stream(std::mt19937& rng) {
    static const char* const frags[] = {
        "*", "$", "\r\n", "\r", "\n", "0", "00", "01", "1", "2", "3", "9", "12", "-", "-1", "+",
        "x", "GET", "SET", "k", " ", "999999999999999999", "1000000000000000000",
        "99999999999999999999", "123456789012345678", "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
        "*1\r\n$0\r\n\r\n", "*0\r\n", "*-1\r\n", "PING\r\n"};
    std::string s;
    const int parts = 1 + static_cast<int>(rng() % 24);
    for (int i = 0; i < parts; ++i) s += frags[rng() % (sizeof(frags) / sizeof(frags[0]))];
    return s;
}

struct parse_trace {
    std::vector<std::vector<std::string>> commands;
    parse_status last = parse_status::incomplete;
    std::string error;
    std::size_t pos = 0;
    bool operator==(const parse_trace&) const = default;
};

parse_trace trace_fast(std::string_view d, const limits& l) {
    parser p(l);
    command c;
    parse_trace t;
    for (;;) {
        t.last = p.next(d, t.pos, c);
        if (t.last != parse_status::complete) break;
        t.commands.emplace_back(c.args.begin(), c.args.end());
    }
    if (t.last == parse_status::error) t.error = p.error();
    return t;
}

parse_trace trace_reference(std::string_view d, const limits& l) {
    command c;
    parse_trace t;
    for (;;) {
        t.last = reference::next(l, d, t.pos, c, t.error);
        if (t.last != parse_status::complete) break;
        t.commands.emplace_back(c.args.begin(), c.args.end());
    }
    if (t.last != parse_status::error) t.error.clear();
    return t;
}

// Executes one shard command on both tables and compares the replies.
bool same_reply(shard& fast, reference::shard& ref, cmd_id id, std::vector<std::string_view> args) {
    std::string a, b;
    fast.execute(id, args, a);
    ref.execute(id, args, b);
    return a == b && fast.size() == ref.size();
}
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
            {"parser_matches_reference",
             [] {
                 std::mt19937 rng(11);
                 limits small;
                 small.max_bulk = 64;
                 small.max_args = 8;
                 small.max_inline = 48;
                 for (int round = 0; round < 40000; ++round) {
                     const std::string d = random_stream(rng);
                     const limits& l = round % 2 ? small : limits{};
                     REQUIRE("parser matches the reference parser", trace_fast(d, l) == trace_reference(d, l));
                     // Every prefix too: a line cut short must wait, not fail early.
                     const std::size_t cut = rng() % (d.size() + 1);
                     const std::string_view head(d.data(), cut);
                     REQUIRE("parser matches the reference parser",
                             trace_fast(head, l) == trace_reference(head, l));
                 }
             }},
            {"shard_matches_reference_map",
             [] {
                 std::mt19937 rng(5);
                 shard fast;
                 reference::shard ref;
                 // A small keyspace: probe runs collide and wrap, DEL shifts entries back.
                 std::vector<std::string> keys;
                 for (int i = 0; i < 96; ++i) keys.push_back("k" + std::to_string(i * 7919 % 1000));
                 keys.push_back("");
                 keys.push_back(std::string(40, 'z'));
                 std::vector<std::string> values = {"", "v", std::string(7, 'a'), std::string(8, 'b'),
                                                    std::string(9, 'c'), std::string(300, 'd')};
                 for (int step = 0; step < 60000; ++step) {
                     const std::string& k = keys[rng() % keys.size()];
                     const std::string& k2 = keys[rng() % keys.size()];
                     const std::string& v = values[rng() % values.size()];
                     bool ok = true;
                     switch (rng() % 16) {
                     case 0: case 1: case 2: case 3: case 4:
                         ok = same_reply(fast, ref, cmd_id::set, {"SET", k, v});
                         break;
                     case 5:
                         ok = same_reply(fast, ref, cmd_id::set, {"SET", k, v, "NX"});
                         break;
                     case 6:
                         ok = same_reply(fast, ref, cmd_id::set, {"SET", k, v, "xx"});
                         break;
                     case 7: case 8: case 9:
                         ok = same_reply(fast, ref, cmd_id::del, {"DEL", k, k2});
                         break;
                     case 10:
                         ok = same_reply(fast, ref, cmd_id::exists, {"EXISTS", k, k2, k});
                         break;
                     case 11:
                         ok = same_reply(fast, ref, cmd_id::dbsize, {"DBSIZE"});
                         break;
                     case 12:
                         if (rng() % 400 == 0) ok = same_reply(fast, ref, cmd_id::flushall, {"FLUSHALL"});
                         break;
                     default:
                         ok = same_reply(fast, ref, cmd_id::get, {"GET", k});
                         break;
                     }
                     REQUIRE("shard replies match the reference map", ok);
                     if (step % 64 == 0) REQUIRE("shard table invariants hold", fast.consistent());
                 }
                 for (const auto& k : keys)
                     REQUIRE("shard replies match the reference map",
                             same_reply(fast, ref, cmd_id::get, {"GET", k}));
                 REQUIRE("shard table invariants hold", fast.consistent());
                 // Growth keeps every key.
                 shard big;
                 reference::shard bigref;
                 for (int i = 0; i < 20000; ++i) {
                     const std::string key = "key:" + std::to_string(i);
                     REQUIRE("shard replies match the reference map",
                             same_reply(big, bigref, cmd_id::set, {"SET", key, key}));
                 }
                 REQUIRE("shard table invariants hold", big.consistent() && big.size() == 20000);
                 for (int i = 0; i < 20000; i += 3)
                     REQUIRE("shard replies match the reference map",
                             same_reply(big, bigref, cmd_id::del, {"DEL", "key:" + std::to_string(i)}));
                 REQUIRE("shard table invariants hold", big.consistent());
                 for (int i = 0; i < 20000; ++i)
                     REQUIRE("shard replies match the reference map",
                             same_reply(big, bigref, cmd_id::get, {"GET", "key:" + std::to_string(i)}));
                 shard moved(std::move(big));
                 REQUIRE("shard table invariants hold", moved.consistent() && big.size() == 0);
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
