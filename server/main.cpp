// kv_server: a RESP key-value server (PING, ECHO, GET, SET [NX|XX], DEL,
// EXISTS, DBSIZE, FLUSHALL, INFO, QUIT, CONFIG GET, COMMAND) with three
// interchangeable I/O front ends. SIGINT/SIGTERM start a graceful shutdown.
#include "server.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

namespace {

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: kv_server [--io uring|epoll|asio] [--cores N] [--cpus a,b,...]\n"
                 "                 [--bind ADDR] [--port P] [--drain-ms MS] [--stats]\n"
                 "  io_uring only: [--ring defer|coop] [--xcore ring|doorbell|doorbell-early]\n"
                 "                 [--no-batch]\n"
                 "                 [--busy-poll] [--direct]\n"
                 "                 [--send owned|fixed] [--recv-buffers N] [--max-queued N]\n");
    std::exit(2);
}

std::vector<int> parse_cpus(const char* s) {
    std::vector<int> out;
    while (*s) {
        char* end = nullptr;
        out.push_back(static_cast<int>(std::strtol(s, &end, 10)));
        if (end == s) usage();
        s = *end == ',' ? end + 1 : end;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    kvs::config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (a == "--io") cfg.io = val();
        else if (a == "--cores") cfg.cores = static_cast<unsigned>(std::atoi(val()));
        else if (a == "--cpus") cfg.cpus = parse_cpus(val());
        else if (a == "--bind") cfg.bind = val();
        else if (a == "--port") cfg.port = std::atoi(val());
        else if (a == "--drain-ms") cfg.drain_ms = static_cast<unsigned>(std::atoi(val()));
        else if (a == "--stats") cfg.stats = true;
        else if (a == "--ring") cfg.ring = val();
        else if (a == "--no-batch") cfg.batch = false;
        else if (a == "--xcore") cfg.xcore = val();
        else if (a == "--busy-poll") cfg.busy_poll = true;
        else if (a == "--direct") cfg.direct = true;
        else if (a == "--send") cfg.send = val();
        else if (a == "--recv-buffers") cfg.recv_buffers = static_cast<unsigned>(std::atoi(val()));
        else if (a == "--max-queued") cfg.max_queued = static_cast<unsigned>(std::atoi(val()));
        else usage();
    }
    if (cfg.cores == 0 || (!cfg.cpus.empty() && cfg.cpus.size() != cfg.cores)) usage();
    if (cfg.ring != "defer" && cfg.ring != "coop") usage();
    if (cfg.xcore != "ring" && cfg.xcore != "doorbell" && cfg.xcore != "doorbell-early") usage();
    if (cfg.send != "owned" && cfg.send != "fixed") usage();

    std::signal(SIGPIPE, SIG_IGN);
    // Every thread inherits this mask; the main thread takes the signals with sigwait.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    rlimit nofile{};
    if (getrlimit(RLIMIT_NOFILE, &nofile) == 0 && nofile.rlim_cur < nofile.rlim_max) {
        nofile.rlim_cur = nofile.rlim_max;
        setrlimit(RLIMIT_NOFILE, &nofile);
    }

    int port = 0;
    int listen_fd = -1;
    std::unique_ptr<kvs::frontend> fe;
    try {
        listen_fd = kvs::make_listener(cfg.bind, cfg.port, port);
        if (cfg.io == "uring") fe = kvs::start_uring(cfg, listen_fd);
        else if (cfg.io == "epoll") fe = kvs::start_epoll(cfg, listen_fd);
#ifdef ROL_HAVE_ASIO
        else if (cfg.io == "asio") fe = kvs::start_asio(cfg, listen_fd);
#endif
        else usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "kv_server: %s\n", e.what());
        return 1;
    }
    std::printf("port %d\n", port);
    std::fflush(stdout);
    int sig = 0;
    sigwait(&sigs, &sig);
    fe->stop();
    const int rc = fe->wait();
    fe.reset();
    ::close(listen_fd);
    return rc;
}
