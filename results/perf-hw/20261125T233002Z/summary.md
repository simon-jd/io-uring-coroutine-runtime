# Hardware counters: io_uring vs epoll front end (20261125T233002Z)

Per request = counter over the whole kvload run (1 s warm-up + 5 s) divided by requests completed in it; perf stat attached to the server's threads (user + kernel), cpu_core PMU only. Medians over accepted runs.

| server cores | workload | arm | runs | req/s | CPU µs/req | cycles/req | instr/req | IPC | user instr/req | kernel instr/req | user IPC | kernel IPC | LLC refs/req | LLC misses/req | LLC miss ratio | L1D load misses/req | branch misses/req | branch miss % | GHz |
| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | get_p1 | uring | 4 | 325,982 | 3.048 | 16,055 | 25,608 | 1.60 | 2,231 | 23,377 | 1.65 | 1.59 | 194.87 | 20.817 | 0.106 | 194.3 | 56.83 | 1.16 | 5.25 |
| 1 | get_p1 | epoll | 4 | 316,368 | 3.194 | 16,715 | 24,574 | 1.47 | 1,376 | 23,198 | 0.83 | 1.54 | 163.20 | 14.016 | 0.086 | 182.4 | 64.46 | 1.37 | 5.26 |
| 1 | get_p16 | uring | 5 | 2,711,722 | 0.370 | 1,935 | 2,386 | 1.23 | 913 | 1,473 | 1.01 | 1.44 | 30.86 | 9.459 | 0.305 | 20.0 | 4.29 | 0.93 | 5.14 |
| 1 | get_p16 | epoll | 4 | 2,679,853 | 0.370 | 1,910 | 2,318 | 1.21 | 853 | 1,466 | 0.99 | 1.40 | 24.66 | 7.431 | 0.298 | 22.0 | 4.37 | 0.97 | 5.20 |
| 1 | set_p16 | uring | 3 | 2,575,824 | 0.391 | 2,043 | 2,401 | 1.18 | 934 | 1,467 | 0.90 | 1.45 | 31.55 | 11.202 | 0.353 | 18.0 | 3.93 | 0.82 | 5.28 |
| 1 | set_p16 | epoll | 3 | 2,713,472 | 0.370 | 1,916 | 2,342 | 1.22 | 881 | 1,461 | 0.98 | 1.44 | 24.20 | 6.500 | 0.270 | 21.2 | 4.59 | 0.99 | 5.28 |
| 2 | get_p16 | uring | 7 | 4,155,574 | 0.436 | 2,184 | 2,580 | 1.18 | 1,083 | 1,497 | 1.09 | 1.30 | 48.20 | 9.457 | 0.194 | 26.7 | 5.39 | 1.09 | 5.01 |
| 2 | get_p16 | epoll | 7 | 3,066,467 | 0.576 | 2,801 | 2,518 | 0.91 | 997 | 1,521 | 0.75 | 1.05 | 44.63 | 11.584 | 0.253 | 32.5 | 4.72 | 0.97 | 4.87 |
| 2 | set_p16 | uring | 7 | 3,111,082 | 0.498 | 2,436 | 2,639 | 1.09 | 1,139 | 1,500 | 0.99 | 1.17 | 53.21 | 12.380 | 0.233 | 27.3 | 5.31 | 1.03 | 4.94 |
| 2 | set_p16 | epoll | 7 | 1,680,224 | 0.760 | 3,587 | 2,633 | 0.74 | 1,061 | 1,570 | 0.59 | 0.88 | 44.70 | 13.782 | 0.303 | 37.3 | 6.17 | 1.19 | 4.83 |
| 4 | get_p16 | uring | 7 | 2,240,579 | 0.941 | 4,433 | 2,888 | 0.65 | 1,242 | 1,645 | 0.56 | 0.75 | 73.07 | 21.945 | 0.301 | 45.3 | 8.24 | 1.48 | 4.71 |
| 4 | get_p16 | epoll | 7 | 2,797,203 | 0.666 | 2,999 | 2,680 | 0.89 | 1,093 | 1,587 | 0.79 | 0.98 | 61.46 | 12.349 | 0.198 | 38.2 | 5.51 | 1.09 | 4.70 |
| 4 | set_p16 | uring | 7 | 5,366,752 | 0.652 | 3,103 | 2,866 | 0.92 | 1,312 | 1,553 | 0.84 | 1.01 | 77.64 | 14.991 | 0.197 | 37.7 | 5.90 | 1.05 | 4.76 |
| 4 | set_p16 | epoll | 7 | 2,957,350 | 0.663 | 3,084 | 2,748 | 0.89 | 1,175 | 1,573 | 0.81 | 0.97 | 58.17 | 12.103 | 0.199 | 37.1 | 5.36 | 1.01 | 4.75 |

io_uring / epoll, paired by repetition: median ratio (repetitions where io_uring is lower / pairs)

| cores | workload | CPU µs/req | cycles/req | instr/req | IPC | LLC misses/req | L1D misses/req | branch misses/req | req/s |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | get_p1 | 0.962 (2/2) | 0.959 (2/2) | 1.044 (0/2) | 1.089 (0/2) | 1.560 (0/2) | 1.074 (0/2) | 0.880 (2/2) | 1.037 (0/2) |
| 1 | get_p16 | 1.002 (2/4) | 1.013 (1/4) | 1.030 (0/4) | 1.015 (1/4) | 1.283 (0/4) | 0.900 (4/4) | 0.893 (3/4) | 0.998 (2/4) |
| 1 | set_p16 | 1.031 (0/3) | 1.036 (0/3) | 1.025 (0/3) | 0.990 (2/3) | 1.666 (0/3) | 0.850 (3/3) | 0.855 (3/3) | 0.998 (2/3) |
| 2 | get_p16 | 0.813 (5/7) | 0.823 (5/7) | 1.026 (0/7) | 1.242 (1/7) | 1.042 (3/7) | 0.888 (6/7) | 1.106 (0/7) | 1.094 (2/7) |
| 2 | set_p16 | 0.911 (4/7) | 0.922 (4/7) | 1.023 (2/7) | 1.109 (3/7) | 1.242 (2/7) | 0.945 (7/7) | 1.035 (2/7) | 1.363 (2/7) |
| 4 | get_p16 | 1.119 (1/7) | 1.160 (1/7) | 1.061 (0/7) | 0.911 (6/7) | 1.574 (0/7) | 1.044 (1/7) | 1.102 (0/7) | 0.770 (5/7) |
| 4 | set_p16 | 1.103 (1/7) | 1.123 (1/7) | 1.052 (0/7) | 0.940 (6/7) | 1.570 (1/7) | 1.013 (2/7) | 1.128 (1/7) | 0.870 (5/7) |

## Conditions

- stamp: 20261125T233002Z
- start: 2026-11-25T23:30:02Z
- commit: caa38ad
- kernel: 7.0.0-42-generic
- perf: perf version 7.0.14
- cpu: 13th Gen Intel(R) Core(TM) i9-13900K
- microcode: 0x133
- governor / no_turbo: powersave 0
- perf_event_paranoid / kptr_restrict / nmi_watchdog (unchanged): 4 1 1
- build: <repo>/build-release (Release, -O3)
- kv_server sha256: 884fc71de6329608
- server CPUs: first N of 2,4,6,0 (P-cores; SMT siblings [1, 3, 5, 7] kept idle by the gate)
- client CPUs: 16-23 (E-cores), kvload 8 threads, 50 connections per server core
- harness CPUs: [24, 25, 26, 27, 28, 29, 30, 31]
- workloads: closed loop, 100k keys preloaded, 64-byte values; 1 core(s): get_p1, get_p16, set_p16; 2 core(s): get_p16, set_p16; 4 core(s): get_p16, set_p16
- reps: 7, arm order rotated per repetition
- events: cpu_core/cycles/,cpu_core/instructions/,cpu_core/cycles/u,cpu_core/instructions/u,cpu_core/branches/,cpu_core/branch-misses/,cpu_core/cache-references/,cpu_core/cache-misses/,cpu_core/L1-dcache-load-misses/
- record: perf record -e cpu_core/cycles/ -F 999 --call-graph dwarf,8192 on 1 core(s) get_p1, 4 core(s) get_p16 for each arm
- gate: 10 s sample; CPUs [0, 1, 2, 3, 4, 5, 6, 7] mean <= 15% and each <= 35%, client CPUs mean <= 35%, machine mean <= 30%, loadavg <= 8.0; wait up to 43200 s at start, 7200 s per repetition
- rejection: 1-core runs with interference > 0.05 repeated up to 2 times
- dry run: False
- end: 2026-11-26T02:06:13Z
- loadavg (1 min) during runs: [5.52, 60.98]
- interference on server CPUs + SMT siblings: median 1.3681, max 6.5016; rejected and repeated 1-core runs: 65 of 144
- lowest perf 'percent running' (multiplexing): 100.0
- other processes' CPU time on CPUs [0, 1, 2, 3, 4, 5, 6, 7] during accepted runs (top 5 per run, summed; names kept off the repository): 856.17 s
- gate checks: 216 (205 busy)
- median busy fraction per CPU during accepted runs:

  0:0.65 1:0.37 2:0.87 3:0.45 4:0.82 5:0.54 6:0.65 7:0.26 8:0.55 9:0.64 10:0.64 11:0.64 12:0.37 13:0.29 14:0.48 15:0.24 16:0.45 17:0.45 18:0.44 19:0.44 20:0.43 21:0.45 22:0.43 23:0.43 24:0.28 25:0.30 26:0.25 27:0.27 28:0.30 29:0.32 30:0.32 31:0.32
