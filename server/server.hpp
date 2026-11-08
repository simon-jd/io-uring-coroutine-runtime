#pragma once
// The key-value server's front ends. All three use the same parser, shard,
// routing plan, threading model (one thread per core, a shared-nothing
// keyspace, connections handed out round-robin by core 0's accept loop) and
// the same batching rule for cross-core traffic (one hand-off per target core
// per loop iteration). They differ only in the I/O interface: the io_uring
// runtime, an epoll readiness loop, or Asio.
#include "kv/resp.hpp"
#include <memory>
#include <string>
#include <vector>

namespace kvs {

struct config {
    std::string io = "uring"; // uring | epoll | asio
    unsigned cores = 1;
    std::vector<int> cpus;    // pin core i's thread to cpus[i]
    std::string bind = "127.0.0.1";
    int port = 6379;          // 0: pick a free port (printed as "port N")
    unsigned drain_ms = 2000; // graceful shutdown: then cut off unfinished sends
    bool stats = false;       // print per-core I/O counters to stderr at exit
    kv::limits limits;
    // io_uring front end only
    std::string ring = "defer";      // defer (DEFER_TASKRUN) | coop (COOP_TASKRUN)
    bool batch = true;               // batch cross-core messages per loop iteration
    std::string xcore = "ring";      // ring (chains in MSG_RING) | doorbell (shared-memory
                                     // queues) | doorbell-early (announce before submitting)
    bool busy_poll = false;
    bool direct = false;             // registered (direct) descriptors for connections
    std::string send = "owned";      // owned (heap buffer) | fixed (registered buffer)
    unsigned recv_buffers = 2048;    // provided buffers per core
    unsigned recv_buffer_size = 4096;
    unsigned max_queued = 8;         // results a busy connection may hold before receive pauses
};

// A running front end.
class frontend {
public:
    virtual ~frontend() = default;
    virtual void stop() = 0; // begin graceful shutdown; callable from any thread
    virtual int wait() = 0;  // join every core; 0 on a clean shutdown
};

std::unique_ptr<frontend> start_uring(const config& cfg, int listen_fd);
std::unique_ptr<frontend> start_epoll(const config& cfg, int listen_fd);
#ifdef ROL_HAVE_ASIO
std::unique_ptr<frontend> start_asio(const config& cfg, int listen_fd);
#endif

// Shared helpers (net.cpp).
int make_listener(const std::string& bind, int port, int& bound_port);
void set_nodelay(int fd);
void pin_thread(int cpu);

} // namespace kvs
