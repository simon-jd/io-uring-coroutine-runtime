# Hardware counters (2026-11-26)

`sudo bash scripts/perf-hw.sh` at caa38ad (Release build, kv_server sha256
884fc71de6329608). Root is not limited by `perf_event_paranoid`; no sysctl was
changed (`perf_event_paranoid` stayed 4, `kptr_restrict` 1, `nmi_watchdog` 1).
The server and the load generator ran as the normal user; only perf ran as
root.

| Directory | What it is |
| --- | --- |
| [20261126T034043Z/](20261126T034043Z) | **the run the numbers below come from**: 7 repetitions, 03:41–03:58 UTC, on a quiet machine by the script's gate |
| [20261125T233002Z/](20261125T233002Z) | an earlier run that waited up to 12 h for the gate and then measured on a busy machine (1-min load 5.5–61, another project's job on CPUs 0–7, 65 of 144 1-core runs rejected); counters only, no profiles. Used only as a direction check below |

A third start (20261125T210605Z) gave up at the gate after 30 minutes and
measured nothing.

## Setup

- perf 7.0.14 on Linux 7.0.0-42, i9-13900K. Only the P-core PMU (`cpu_core`)
  was used, and the server ran only on P-cores: 1 core on CPU 2, 2 cores on 2
  and 4, 4 cores on 2, 4, 6 and 0. Their SMT siblings (1, 3, 5, 7) were
  required to be idle by the gate. CPUs 8–15 were left to another project's
  timing tests, so these placements differ from kv-bench's 2,4,6,8. CPU 8 is
  one of the two 5.8 GHz cores; CPUs 0–7 reach 5.5 GHz. The server cores ran
  at a median of 5.3 GHz at 1 core and 5.0 GHz at 4 cores.
- The load generator (kvload, 8 threads) ran on E-cores 16–23, and the harness
  ran on 24–31. The workloads were kv-bench's closed loop: 100k keys, 64-byte
  values, 50 connections per server core, 1 s warm-up and 5 s measured.
- `perf stat -p <server>` counted all of the server's threads, user plus
  kernel, over the whole kvload run. Per request = count ÷ requests completed
  in that run, including the warm-up. The same division of schedstat CPU time
  gives "CPU µs/req", which is how kv-bench defines it.
- Events: `cycles`, `instructions`, `cycles:u`, `instructions:u`, `branches`,
  `branch-misses`, `cache-references`, `cache-misses` and
  `L1-dcache-load-misses`. On this PMU, cache-references and cache-misses
  count last-level-cache references and misses. Kernel counts are total minus
  `:u`. Every counter ran 100% of the time, with no multiplexing.
- **Gate.** Before the run and before every repetition, the script took a 10 s
  `/proc/stat` sample. It required CPUs 0–7 at ≤ 15% mean and ≤ 35% each,
  CPUs 16–23 at ≤ 35% mean, the machine at ≤ 30% and the 1-min load at ≤ 8.
  13 checks, 2 of them busy and waited out.
- **Interference.** Every run records other work on the server CPUs and their
  siblings, the per-CPU busy fraction for all 32 CPUs, and the load average.
  1-core runs above 5% interference were repeated (kv-bench's rule): 18 of 115
  were repeated. Multi-core runs have no rule. Across all runs the median
  interference was 0.107 and the maximum 1.73 CPU-seconds per second. The 1-min
  load was 3.2–9.3.
- **Profiles.** `perf record -e cpu_core/cycles/ -F 999 --call-graph
  dwarf,8192` was taken for each front end at 1 core GET depth 1 and at 4 cores
  GET depth 16, with no lost samples. In `record/`: `*-flat.txt` (self time by
  symbol), `*-dso.txt` and `*-callers.txt.gz` (call graph, children ≥ 3%,
  capped at 4,000 lines). The perf.data files stayed on the build machine.
- **What was taken out.** The committed files carry the total CPU time of other
  processes (`other_processes_top5_cpu_s`) instead of their names, and
  `<repo>` instead of the checkout's path. The script now writes them this way.

## Results (20261126T034043Z, medians of 7)

The full tables are in [summary.md](20261126T034043Z/summary.md). Ratios are
io_uring ÷ epoll, paired by repetition. The count in brackets is how many
repetitions had io_uring lower.

| | CPU µs/req | cycles/req | instr/req | IPC | LLC misses/req | L1D load misses/req | branch misses/req |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 core, GET depth 1 | 0.954 (6/7) | 0.946 (6/7) | 1.042 (0/7) | 1.101 (0/7) | 1.204 (1/7) | 1.070 (0/7) | 0.929 (4/7) |
| 1 core, GET depth 16 | 0.982 (4/6) | 0.979 (4/6) | 1.030 (0/6) | 1.053 (1/6) | 1.161 (1/6) | 0.908 (6/6) | 0.982 (3/6) |
| 1 core, SET depth 16 | 1.016 (3/7) | 1.032 (3/7) | 1.027 (0/7) | 0.994 (4/7) | 1.689 (0/7) | 0.837 (7/7) | 0.935 (6/7) |
| 2 cores, GET depth 16 | 0.992 (4/7) | 1.009 (3/7) | 1.026 (0/7) | 1.019 (3/7) | 1.548 (0/7) | 0.976 (5/7) | 1.135 (0/7) |
| 2 cores, SET depth 16 | 1.075 (3/7) | 1.063 (3/7) | 1.025 (0/7) | 0.964 (4/7) | 2.210 (0/7) | 0.924 (5/7) | 1.256 (0/7) |
| 4 cores, GET depth 16 | 1.133 (3/7) | 1.164 (1/7) | 1.052 (0/7) | 0.906 (4/7) | 2.142 (0/7) | 1.084 (3/7) | 1.040 (0/7) |
| 4 cores, SET depth 16 | 1.098 (1/7) | 1.076 (1/7) | 1.048 (0/7) | 0.973 (5/7) | 2.455 (0/7) | 1.021 (2/7) | 1.021 (2/7) |

**1 core, GET, depth 1.** io_uring used 2.92 µs per request and epoll 3.05.
That is 15,594 against 16,370 cycles. io_uring retires 4% *more* instructions
(25,641 against 24,572), but at a higher IPC (1.64 against 1.50).

About 91% of the instructions are the kernel's: 23,410 against 23,196 per
request, mostly loopback TCP. Kernel cycles are 3% lower for io_uring (14,524
against 14,799; 6 of 7). User mode accounts for most of the difference.
io_uring runs 2,231 user instructions per request against 1,376, but in 1,105
cycles against 1,571 (0.70; 7 of 7). Its user IPC is 2.02, epoll's 0.88.

epoll's server returns to user space from a read and a send per request,
plus an epoll_wait about every 17 requests. The io_uring server returns from
one io_uring_enter about every 17 requests. These are kv-bench's diag counts
at 2 cores, depth 1. Low user IPC right
after each return is the likely cause, but it was not measured per call.

The same direction appears in the busy run: 0.962 CPU, 0.959 cycles and user
IPC 1.9 times epoll's, but only 2 of its 1-core pairs were accepted. The CPU
ratio agrees with the kv-bench sessions (0.959 / 0.924).

**Depth 16 at 1 and 2 cores.** CPU per request does not separate the two front
ends in this run: 3–4 of 7 repetitions lower, median ratios 0.98–1.08. The
counters do separate them, in every repetition or all but one or two:
- io_uring retires 2.5–3% more instructions per request (0 of 6 or 7 lower).
- It has 1.2–2.2 times the LLC misses (0–1 lower).
- It has 9–16% fewer L1D load misses at 1 core (6 of 6, 7 of 7).
- At 2 cores it has 14–26% more branch misses (0 of 7).

**4 cores, depth 16.** io_uring used 13% (GET) and 10% (SET) more CPU per
request by median, but GET was lower in 3 of 7 repetitions. Cycles per
request: 1.164 (GET, 1 of 7 lower) and 1.076 (SET, 1 of 7).

The extra cost is in user mode:
- User cycles are 943 against 721 for GET (1.33; 0 of 7) and 973 against 831
  for SET.
- User instructions are 12–13% higher (0 of 7), at lower user IPC.
- Kernel instructions are the same (0.995 and 0.994; io_uring lower in 6 of
  7) and kernel cycles 4% higher.
- LLC misses per request are 2.1 times (GET, 6.36 against 2.98) and 2.5 times
  (SET, 8.39 against 3.43) epoll's, with 0 of 7 lower. LLC references are 1.3–1.4
  times.

The busy run agrees in direction: CPU 1.12 and 1.10, cycles 1.16 and 1.12, LLC
misses 1.57 times, user cycles 1.26 and 1.18. This matches the kv-bench
finding that the 4-core gap is user time with system time the same.

**Profiles.** These are samples of cycles. They show where time goes, not
where the misses happen.
- **1 core, depth 1.** 93% (io_uring) and 91% (epoll) of server cycles are in
  the kernel. Netfilter (`nft_do_chain`, conntrack lookup and the rule
  evaluation around them) takes about 18% in both (counting symbols above 0.3%). The host's firewall rules
  run on every loopback packet, so they are part of every per-request number
  here and in kv-bench.
- **4 cores, depth 16.** User space is 45% of io_uring's cycles and 42% of
  epoll's. The key-value engine costs the same in both (`kv::shard::probe`
  14.4% against 14.6%). The runtime's cross-core path shows up only in
  io_uring's profile, at about 3% of cycles: `reactor::on_message_chain` 1.3%,
  `on_reply`, `call_all_awaiter::await_suspend`, `flush_target` and
  `slot_at`. epoll's front-end loop takes 1.7%.
- The extra LLC misses also appear at 1 core, where nothing crosses cores
  (1.2–1.7 times). So they are not only cross-core message traffic. Which code
  takes them would need sampling on LLC-miss or memory-load events. That was
  not done.

## The busy run's 2-core cells

Its 2-core ratios point the other way: CPU 0.813 and 0.911, with LLC misses
only 1.04 and 1.24 times epoll's. Its 4-core throughput was erratic (0.77 and
0.87 times epoll's). They are not used.
