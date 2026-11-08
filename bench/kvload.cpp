// kvload: load generator for RESP servers (independent of the runtime).
//
//   kvload preload --port P --keys N --value-size D
//       SET key:%012d for every key, pipelined, checking each +OK.
//   kvload closed --port P --pipeline D --duration S [--warmup W] [--conns C]
//                 [--threads T] [--cpus a,b,..] [--get-ratio F] [--keys N]
//                 [--value-size D] [--seed X]
//       Closed loop, the valkey-benchmark -P model: each connection sends D
//       requests, waits for all D replies, then sends the next D. Reports the
//       requests completed per second during the measured window and the
//       round-trip time of each pipeline.
//   kvload open --port P --rate R --duration S [--warmup W] [--conns C]
//               [--threads T] [--cpus a,b,..] [--get-ratio F] [--keys N]
//               [--value-size D] [--seed X]
//       Open loop at a fixed rate. Every connection sends on its own fixed
//       schedule (rate R/C, phases spread evenly) whether or not earlier replies
//       have arrived; requests that fall due together are sent back to back.
//       Latency is measured from each request's scheduled send time to the
//       arrival of its reply, so a stalled server is charged for the whole
//       stall (no coordinated omission). Requests still unanswered 2 s after
//       the run are counted as timeouts and enter the latency data with their
//       age at that point.
//
// Output: one JSON object on stdout.
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <random>
#include <sched.h>
#include <string>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch())
        .count();
}

struct options {
    std::string mode;
    int port = 6379;
    double rate = 10000;
    double duration = 10;
    double warmup = 2;
    int conns = 50;
    int threads = 2;
    std::vector<int> cpus;
    double get_ratio = 0.9;
    int keys = 100000;
    int value_size = 64;
    unsigned seed = 1;
    int pipeline = 1;
};

[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "kvload: %s: %s\n", what, std::strerror(errno));
    std::exit(1);
}

int connect_to(int port, bool nonblocking) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) die("socket");
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<std::uint16_t>(port));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) die("connect");
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (nonblocking) {
        const int fl = ::fcntl(fd, F_GETFL);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    return fd;
}

std::string key_of(int k) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key:%012d", k);
    return buf;
}

void append_get(std::string& out, int k) {
    out += "*2\r\n$3\r\nGET\r\n$16\r\n";
    out += key_of(k);
    out += "\r\n";
}

void append_set(std::string& out, int k, const std::string& value) {
    out += "*3\r\n$3\r\nSET\r\n$16\r\n";
    out += key_of(k);
    out += "\r\n$" + std::to_string(value.size()) + "\r\n";
    out += value;
    out += "\r\n";
}

// Length of the first complete reply in `b` from `pos`, or 0. Sets `error`
// for '-' replies.
std::size_t reply_len(const std::string& b, std::size_t pos, bool& error) {
    if (pos >= b.size()) return 0;
    const auto e = b.find("\r\n", pos);
    if (e == std::string::npos) return 0;
    const char t = b[pos];
    error = t == '-';
    if (t == '+' || t == '-' || t == ':') return e + 2 - pos;
    if (t == '$') {
        const long n = std::strtol(b.c_str() + pos + 1, nullptr, 10);
        if (n < 0) return e + 2 - pos;
        const std::size_t end = e + 2 + static_cast<std::size_t>(n) + 2;
        return end <= b.size() ? end - pos : 0;
    }
    std::fprintf(stderr, "kvload: unexpected reply type '%c'\n", t);
    std::exit(1);
}

void pin(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

int preload(const options& o) {
    const int fd = connect_to(o.port, false);
    const std::string value(static_cast<std::size_t>(o.value_size), 'v');
    const int batch = 1000;
    std::string in;
    for (int base = 0; base < o.keys; base += batch) {
        std::string out;
        const int n = std::min(batch, o.keys - base);
        for (int i = 0; i < n; ++i) append_set(out, base + i, value);
        for (std::size_t off = 0; off < out.size();) {
            const ssize_t w = ::send(fd, out.data() + off, out.size() - off, MSG_NOSIGNAL);
            if (w <= 0) die("send");
            off += static_cast<std::size_t>(w);
        }
        int got = 0;
        std::size_t pos = 0;
        while (got < n) {
            bool err = false;
            const std::size_t len = reply_len(in, pos, err);
            if (len == 0) {
                in.erase(0, pos);
                pos = 0;
                char buf[65536];
                const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
                if (r <= 0) die("recv");
                in.append(buf, static_cast<std::size_t>(r));
                continue;
            }
            if (err || in.compare(pos, len, "+OK\r\n") != 0) {
                std::fprintf(stderr, "kvload: preload got an unexpected reply\n");
                return 1;
            }
            pos += len;
            ++got;
        }
        in.erase(0, pos);
    }
    ::close(fd);
    std::printf("{\"mode\":\"preload\",\"keys\":%d,\"value_size\":%d}\n", o.keys, o.value_size);
    return 0;
}

struct conn_state {
    int fd = -1;
    std::int64_t next_send = 0;
    std::string out;
    std::size_t out_off = 0;
    std::string in;
    std::deque<std::int64_t> pending; // scheduled send times, in order
    std::mt19937_64 rng;
};

struct thread_result {
    std::vector<std::int64_t> lat;  // ns, measured window only
    std::vector<std::int64_t> late; // send lateness, ns (sampled)
    std::uint64_t sent = 0, completed = 0, errors = 0, timeouts = 0, in_window = 0;
};

void run_open_thread(const options& o, int tid, std::int64_t t0, std::atomic<bool>& failed,
                     thread_result& res) {
    if (!o.cpus.empty()) pin(o.cpus[static_cast<std::size_t>(tid) % o.cpus.size()]);
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
    const int per = o.conns / o.threads + (tid < o.conns % o.threads ? 1 : 0);
    const int first = tid * (o.conns / o.threads) + std::min(tid, o.conns % o.threads);
    const double interval = 1e9 * o.conns / o.rate; // per connection
    const std::int64_t warm_end = t0 + static_cast<std::int64_t>(o.warmup * 1e9);
    const std::int64_t end = warm_end + static_cast<std::int64_t>(o.duration * 1e9);
    const std::string value(static_cast<std::size_t>(o.value_size), 's');
    const int ep = ::epoll_create1(EPOLL_CLOEXEC);
    std::vector<conn_state> cs(static_cast<std::size_t>(per));
    for (int i = 0; i < per; ++i) {
        auto& c = cs[static_cast<std::size_t>(i)];
        c.fd = connect_to(o.port, true);
        c.next_send = t0 + static_cast<std::int64_t>((first + i) * (1e9 / o.rate));
        c.rng.seed(o.seed * 1000003u + static_cast<unsigned>(first + i));
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u32 = static_cast<std::uint32_t>(i);
        ::epoll_ctl(ep, EPOLL_CTL_ADD, c.fd, &ev);
    }
    std::uniform_real_distribution<double> coin(0, 1);
    std::uniform_int_distribution<int> key(0, o.keys - 1);
    res.lat.reserve(static_cast<std::size_t>(o.rate * o.duration / o.threads * 1.1) + 1024);
    std::uint64_t sends_seen = 0;
    epoll_event evs[64];
    const std::int64_t grace_end = end + 2'000'000'000;
    for (;;) {
        std::int64_t now = now_ns();
        std::size_t outstanding = 0;
        std::int64_t earliest = INT64_MAX;
        for (auto& c : cs) {
            while (c.next_send <= now && c.next_send < end) {
                if (coin(c.rng) < o.get_ratio) append_get(c.out, key(c.rng));
                else append_set(c.out, key(c.rng), value);
                c.pending.push_back(c.next_send);
                if ((sends_seen++ & 63) == 0) res.late.push_back(now - c.next_send);
                ++res.sent;
                c.next_send += static_cast<std::int64_t>(interval);
            }
            while (c.out_off < c.out.size()) {
                const ssize_t w = ::send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
                if (w > 0) c.out_off += static_cast<std::size_t>(w);
                else if (w < 0 && errno == EAGAIN) break;
                else {
                    failed = true;
                    return;
                }
            }
            if (c.out_off == c.out.size()) {
                c.out.clear();
                c.out_off = 0;
            }
            outstanding += c.pending.size();
            if (c.next_send < end) earliest = std::min(earliest, c.next_send);
        }
        if (now >= end && outstanding == 0) break;
        if (now >= grace_end) break;
        int timeout_ms = 0;
        if (earliest == INT64_MAX) timeout_ms = 1;
        else if (earliest - now > 1'500'000) timeout_ms = static_cast<int>((earliest - now) / 1'000'000) - 1;
        const int n = ::epoll_wait(ep, evs, 64, timeout_ms);
        now = now_ns();
        for (int i = 0; i < n; ++i) {
            auto& c = cs[evs[i].data.u32];
            char buf[65536];
            for (;;) {
                const ssize_t r = ::recv(c.fd, buf, sizeof(buf), 0);
                if (r > 0) {
                    c.in.append(buf, static_cast<std::size_t>(r));
                    if (static_cast<std::size_t>(r) < sizeof(buf)) break;
                    continue;
                }
                if (r < 0 && errno == EAGAIN) break;
                failed = true;
                return;
            }
            std::size_t pos = 0;
            for (;;) {
                bool err = false;
                const std::size_t len = reply_len(c.in, pos, err);
                if (len == 0) break;
                pos += len;
                if (c.pending.empty()) {
                    failed = true;
                    return;
                }
                const std::int64_t sched = c.pending.front();
                c.pending.pop_front();
                ++res.completed;
                if (err) ++res.errors;
                if (sched >= warm_end && sched < end) {
                    res.lat.push_back(now - sched);
                    ++res.in_window;
                }
            }
            c.in.erase(0, pos);
        }
    }
    const std::int64_t stop = now_ns();
    for (auto& c : cs) {
        for (auto sched : c.pending) {
            ++res.timeouts;
            if (sched >= warm_end && sched < end) res.lat.push_back(stop - sched);
        }
        ::close(c.fd);
    }
    ::close(ep);
}

// Requests prebuilt as one string: batch j is bytes [ends[j*D-1], ends[(j+1)*D-1]).
struct request_pool {
    std::string bytes;
    std::vector<std::size_t> batch_end; // end offset of each batch
};

request_pool build_pool(const options& o, unsigned seed) {
    request_pool p;
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> coin(0, 1);
    std::uniform_int_distribution<int> key(0, o.keys - 1);
    const std::string value(static_cast<std::size_t>(o.value_size), 's');
    const int batches = std::max(1, 8192 / o.pipeline);
    for (int b = 0; b < batches; ++b) {
        for (int i = 0; i < o.pipeline; ++i) {
            if (coin(rng) < o.get_ratio) append_get(p.bytes, key(rng));
            else append_set(p.bytes, key(rng), value);
        }
        p.batch_end.push_back(p.bytes.size());
    }
    return p;
}

struct closed_conn {
    int fd = -1;
    std::size_t batch = 0;
    std::size_t out_off = 0, out_end = 0;
    int pending = 0;
    std::int64_t sent_at = 0;
    std::string in;
};

void run_closed_thread(const options& o, int tid, std::int64_t t0, std::atomic<bool>& failed,
                       thread_result& res) {
    if (!o.cpus.empty()) pin(o.cpus[static_cast<std::size_t>(tid) % o.cpus.size()]);
    const int per = o.conns / o.threads + (tid < o.conns % o.threads ? 1 : 0);
    const std::int64_t warm_end = t0 + static_cast<std::int64_t>(o.warmup * 1e9);
    const std::int64_t end = warm_end + static_cast<std::int64_t>(o.duration * 1e9);
    const request_pool pool = build_pool(o, o.seed * 7919u + static_cast<unsigned>(tid));
    const int ep = ::epoll_create1(EPOLL_CLOEXEC);
    std::vector<closed_conn> cs(static_cast<std::size_t>(per));
    for (int i = 0; i < per; ++i) {
        auto& c = cs[static_cast<std::size_t>(i)];
        c.fd = connect_to(o.port, true);
        c.batch = static_cast<std::size_t>(i * 37) % pool.batch_end.size();
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u32 = static_cast<std::uint32_t>(i);
        ::epoll_ctl(ep, EPOLL_CTL_ADD, c.fd, &ev);
    }
    while (now_ns() < t0) {
    }
    auto start_batch = [&](closed_conn& c, std::int64_t now) {
        c.out_off = c.batch ? pool.batch_end[c.batch - 1] : 0;
        c.out_end = pool.batch_end[c.batch];
        c.batch = (c.batch + 1) % pool.batch_end.size();
        c.pending = o.pipeline;
        c.sent_at = now;
        ++res.sent;
    };
    auto push = [&](closed_conn& c) {
        while (c.out_off < c.out_end) {
            const ssize_t w = ::send(c.fd, pool.bytes.data() + c.out_off, c.out_end - c.out_off, MSG_NOSIGNAL);
            if (w > 0) c.out_off += static_cast<std::size_t>(w);
            else if (w < 0 && errno == EAGAIN) return true;
            else return false;
        }
        return true;
    };
    for (auto& c : cs) {
        start_batch(c, now_ns());
        if (!push(c)) {
            failed = true;
            return;
        }
    }
    epoll_event evs[128];
    const std::int64_t grace_end = end + 2'000'000'000;
    std::size_t busy = cs.size();
    while (busy > 0) {
        const int n = ::epoll_wait(ep, evs, 128, 10);
        const std::int64_t now = now_ns();
        if (now > grace_end) break;
        for (int i = 0; i < n; ++i) {
            auto& c = cs[evs[i].data.u32];
            char buf[65536];
            for (;;) {
                const ssize_t r = ::recv(c.fd, buf, sizeof(buf), 0);
                if (r > 0) {
                    c.in.append(buf, static_cast<std::size_t>(r));
                    if (static_cast<std::size_t>(r) < sizeof(buf)) break;
                    continue;
                }
                if (r < 0 && errno == EAGAIN) break;
                failed = true;
                return;
            }
            std::size_t pos = 0;
            for (;;) {
                bool err = false;
                const std::size_t len = reply_len(c.in, pos, err);
                if (len == 0) break;
                pos += len;
                if (err) ++res.errors;
                if (--c.pending == 0) break;
            }
            c.in.erase(0, pos);
            if (c.pending > 0) {
                if (!push(c)) {
                    failed = true;
                    return;
                }
                continue;
            }
            res.completed += static_cast<std::uint64_t>(o.pipeline);
            if (c.sent_at >= warm_end && now <= end) {
                res.in_window += static_cast<std::uint64_t>(o.pipeline);
                res.lat.push_back(now - c.sent_at);
            }
            if (now >= end) {
                --busy;
                continue;
            }
            start_batch(c, now);
            if (!push(c)) {
                failed = true;
                return;
            }
        }
    }
    for (auto& c : cs) ::close(c.fd);
    ::close(ep);
}

double pct(const std::vector<std::int64_t>& v, double p) {
    if (v.empty()) return 0;
    const std::size_t i = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(v.size()))) - 1;
    return static_cast<double>(v[std::min(i, v.size() - 1)]) / 1000.0;
}

int closed_loop(const options& o) {
    std::vector<thread_result> res(static_cast<std::size_t>(o.threads));
    std::atomic<bool> failed{false};
    const std::int64_t t0 = now_ns() + 300'000'000;
    std::vector<std::thread> ts;
    for (int t = 0; t < o.threads; ++t)
        ts.emplace_back([&, t] { run_closed_thread(o, t, t0, failed, res[static_cast<std::size_t>(t)]); });
    for (auto& t : ts) t.join();
    if (failed) {
        std::fprintf(stderr, "kvload: a connection failed\n");
        return 1;
    }
    std::vector<std::int64_t> lat;
    std::uint64_t completed = 0, errors = 0, in_window = 0;
    for (auto& r : res) {
        lat.insert(lat.end(), r.lat.begin(), r.lat.end());
        completed += r.completed;
        errors += r.errors;
        in_window += r.in_window;
    }
    std::sort(lat.begin(), lat.end());
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const double cpu = static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
                       static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
    std::printf(
        "{\"mode\":\"closed\",\"pipeline\":%d,\"rps\":%.1f,\"duration_s\":%.1f,\"conns\":%d,"
        "\"threads\":%d,\"get_ratio\":%.2f,\"keys\":%d,\"value_size\":%d,\"completed\":%llu,"
        "\"errors\":%llu,\"batches\":%zu,\"rtt_p50_us\":%.1f,\"rtt_p99_us\":%.1f,"
        "\"rtt_p999_us\":%.1f,\"client_cpu_s\":%.3f}\n",
        o.pipeline, static_cast<double>(in_window) / o.duration, o.duration, o.conns, o.threads,
        o.get_ratio, o.keys, o.value_size, static_cast<unsigned long long>(completed),
        static_cast<unsigned long long>(errors), lat.size(), pct(lat, 50), pct(lat, 99),
        pct(lat, 99.9), cpu);
    return errors ? 1 : 0;
}

int open_loop(const options& o) {
    std::vector<thread_result> res(static_cast<std::size_t>(o.threads));
    std::atomic<bool> failed{false};
    const std::int64_t t0 = now_ns() + 200'000'000; // connections are set up first
    std::vector<std::thread> ts;
    for (int t = 0; t < o.threads; ++t)
        ts.emplace_back([&, t] { run_open_thread(o, t, t0, failed, res[static_cast<std::size_t>(t)]); });
    for (auto& t : ts) t.join();
    if (failed) {
        std::fprintf(stderr, "kvload: a connection failed\n");
        return 1;
    }
    std::vector<std::int64_t> lat, late;
    std::uint64_t sent = 0, completed = 0, errors = 0, timeouts = 0, in_window = 0;
    for (auto& r : res) {
        lat.insert(lat.end(), r.lat.begin(), r.lat.end());
        late.insert(late.end(), r.late.begin(), r.late.end());
        sent += r.sent;
        completed += r.completed;
        errors += r.errors;
        timeouts += r.timeouts;
        in_window += r.in_window;
    }
    std::sort(lat.begin(), lat.end());
    std::sort(late.begin(), late.end());
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const double cpu = static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
                       static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
    std::printf(
        "{\"mode\":\"open\",\"target_rate\":%.0f,\"achieved_rate\":%.1f,\"duration_s\":%.1f,"
        "\"conns\":%d,\"threads\":%d,\"get_ratio\":%.2f,\"keys\":%d,\"value_size\":%d,"
        "\"sent\":%llu,\"completed\":%llu,\"errors\":%llu,\"timeouts\":%llu,\"samples\":%zu,"
        "\"p50_us\":%.1f,\"p90_us\":%.1f,\"p99_us\":%.1f,\"p999_us\":%.1f,\"max_us\":%.1f,"
        "\"send_late_p50_us\":%.1f,\"send_late_p99_us\":%.1f,\"send_late_max_us\":%.1f,"
        "\"client_cpu_s\":%.3f}\n",
        o.rate, static_cast<double>(in_window) / o.duration, o.duration, o.conns, o.threads,
        o.get_ratio, o.keys, o.value_size, static_cast<unsigned long long>(sent),
        static_cast<unsigned long long>(completed), static_cast<unsigned long long>(errors),
        static_cast<unsigned long long>(timeouts), lat.size(), pct(lat, 50), pct(lat, 90),
        pct(lat, 99), pct(lat, 99.9), lat.empty() ? 0.0 : static_cast<double>(lat.back()) / 1000.0,
        pct(late, 50), pct(late, 99), late.empty() ? 0.0 : static_cast<double>(late.back()) / 1000.0,
        cpu);
    return 0;
}

std::vector<int> parse_cpus(const char* s) {
    std::vector<int> out;
    while (*s) {
        char* end = nullptr;
        const long a = std::strtol(s, &end, 10);
        long b = a;
        if (*end == '-') b = std::strtol(end + 1, &end, 10);
        for (long c = a; c <= b; ++c) out.push_back(static_cast<int>(c));
        s = *end == ',' ? end + 1 : end;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: kvload preload|open [options]\n");
        return 2;
    }
    options o;
    o.mode = argv[1];
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        const char* v = argv[i + 1];
        if (a == "--port") o.port = std::atoi(v);
        else if (a == "--rate") o.rate = std::atof(v);
        else if (a == "--duration") o.duration = std::atof(v);
        else if (a == "--warmup") o.warmup = std::atof(v);
        else if (a == "--conns") o.conns = std::atoi(v);
        else if (a == "--threads") o.threads = std::atoi(v);
        else if (a == "--cpus") o.cpus = parse_cpus(v);
        else if (a == "--get-ratio") o.get_ratio = std::atof(v);
        else if (a == "--keys") o.keys = std::atoi(v);
        else if (a == "--value-size") o.value_size = std::atoi(v);
        else if (a == "--seed") o.seed = static_cast<unsigned>(std::atoi(v));
        else if (a == "--pipeline") o.pipeline = std::atoi(v);
        else {
            std::fprintf(stderr, "kvload: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    rlimit nofile{};
    if (getrlimit(RLIMIT_NOFILE, &nofile) == 0) {
        nofile.rlim_cur = nofile.rlim_max;
        setrlimit(RLIMIT_NOFILE, &nofile);
    }
    if (o.mode == "preload") return preload(o);
    if (o.mode == "closed") {
        if (o.threads < 1 || o.conns < o.threads || o.pipeline < 1) return 2;
        return closed_loop(o);
    }
    if (o.mode == "open") {
        if (o.threads < 1 || o.conns < o.threads || o.rate <= 0) return 2;
        return open_loop(o);
    }
    std::fprintf(stderr, "kvload: unknown mode %s\n", o.mode.c_str());
    return 2;
}
