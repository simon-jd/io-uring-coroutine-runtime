# Hardware counters: io_uring vs epoll front end (20261126T034043Z)

Per request = counter over the whole kvload run (1 s warm-up + 5 s) divided by requests completed in it; perf stat attached to the server's threads (user + kernel), cpu_core PMU only. Medians over accepted runs.

| server cores | workload | arm | runs | req/s | CPU µs/req | cycles/req | instr/req | IPC | user instr/req | kernel instr/req | user IPC | kernel IPC | LLC refs/req | LLC misses/req | LLC miss ratio | L1D load misses/req | branch misses/req | branch miss % | GHz |
| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | get_p1 | uring | 7 | 340,571 | 2.916 | 15,594 | 25,641 | 1.64 | 2,231 | 23,410 | 2.02 | 1.61 | 190.27 | 12.941 | 0.068 | 193.4 | 58.11 | 1.19 | 5.33 |
| 1 | get_p1 | epoll | 7 | 324,506 | 3.045 | 16,370 | 24,572 | 1.50 | 1,376 | 23,196 | 0.88 | 1.56 | 164.54 | 11.374 | 0.069 | 180.4 | 58.70 | 1.25 | 5.33 |
| 1 | get_p16 | uring | 6 | 3,392,206 | 0.295 | 1,573 | 2,380 | 1.51 | 913 | 1,467 | 1.55 | 1.49 | 30.49 | 4.992 | 0.165 | 19.7 | 4.17 | 0.91 | 5.33 |
| 1 | get_p16 | epoll | 7 | 3,307,622 | 0.301 | 1,618 | 2,311 | 1.43 | 853 | 1,459 | 1.41 | 1.48 | 24.27 | 4.064 | 0.168 | 21.8 | 4.05 | 0.90 | 5.36 |
| 1 | set_p16 | uring | 7 | 3,017,277 | 0.327 | 1,731 | 2,403 | 1.39 | 934 | 1,469 | 1.25 | 1.48 | 31.53 | 6.891 | 0.218 | 18.0 | 4.10 | 0.86 | 5.32 |
| 1 | set_p16 | epoll | 7 | 3,038,221 | 0.329 | 1,701 | 2,338 | 1.37 | 881 | 1,457 | 1.24 | 1.45 | 24.12 | 4.067 | 0.169 | 21.2 | 4.58 | 0.99 | 5.30 |
| 2 | get_p16 | uring | 7 | 5,358,861 | 0.347 | 1,761 | 2,571 | 1.46 | 1,082 | 1,489 | 1.50 | 1.45 | 44.57 | 5.616 | 0.126 | 24.1 | 5.53 | 1.12 | 5.25 |
| 2 | get_p16 | epoll | 7 | 4,563,923 | 0.335 | 1,752 | 2,507 | 1.43 | 997 | 1,510 | 1.40 | 1.42 | 35.36 | 3.627 | 0.103 | 25.2 | 4.41 | 0.91 | 5.25 |
| 2 | set_p16 | uring | 7 | 5,290,963 | 0.346 | 1,817 | 2,625 | 1.45 | 1,139 | 1,486 | 1.46 | 1.42 | 47.08 | 7.578 | 0.164 | 21.8 | 5.36 | 1.04 | 5.24 |
| 2 | set_p16 | epoll | 7 | 5,155,296 | 0.355 | 1,816 | 2,565 | 1.41 | 1,061 | 1,504 | 1.34 | 1.46 | 36.51 | 4.161 | 0.116 | 24.1 | 4.25 | 0.84 | 5.25 |
| 4 | get_p16 | uring | 7 | 7,226,915 | 0.405 | 2,086 | 2,774 | 1.33 | 1,237 | 1,537 | 1.31 | 1.34 | 67.85 | 6.358 | 0.093 | 31.2 | 5.42 | 1.02 | 5.02 |
| 4 | get_p16 | epoll | 7 | 7,547,152 | 0.381 | 1,854 | 2,635 | 1.42 | 1,091 | 1,543 | 1.51 | 1.36 | 50.57 | 2.983 | 0.059 | 29.8 | 5.31 | 1.05 | 4.98 |
| 4 | set_p16 | uring | 7 | 7,740,819 | 0.425 | 2,146 | 2,835 | 1.32 | 1,312 | 1,523 | 1.35 | 1.30 | 70.37 | 8.389 | 0.118 | 29.5 | 5.41 | 0.97 | 5.05 |
| 4 | set_p16 | epoll | 7 | 7,939,187 | 0.383 | 1,958 | 2,709 | 1.39 | 1,173 | 1,535 | 1.41 | 1.36 | 52.77 | 3.432 | 0.065 | 29.4 | 5.30 | 0.99 | 5.15 |

io_uring / epoll, paired by repetition: median ratio (repetitions where io_uring is lower / pairs)

| cores | workload | CPU µs/req | cycles/req | instr/req | IPC | LLC misses/req | L1D misses/req | branch misses/req | req/s |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | get_p1 | 0.954 (6/7) | 0.946 (6/7) | 1.042 (0/7) | 1.101 (0/7) | 1.204 (1/7) | 1.070 (0/7) | 0.929 (4/7) | 1.051 (1/7) |
| 1 | get_p16 | 0.982 (4/6) | 0.979 (4/6) | 1.030 (0/6) | 1.053 (1/6) | 1.161 (1/6) | 0.908 (6/6) | 0.982 (3/6) | 1.024 (3/6) |
| 1 | set_p16 | 1.016 (3/7) | 1.032 (3/7) | 1.027 (0/7) | 0.994 (4/7) | 1.689 (0/7) | 0.837 (7/7) | 0.935 (6/7) | 0.982 (4/7) |
| 2 | get_p16 | 0.992 (4/7) | 1.009 (3/7) | 1.026 (0/7) | 1.019 (3/7) | 1.548 (0/7) | 0.976 (5/7) | 1.135 (0/7) | 1.031 (1/7) |
| 2 | set_p16 | 1.075 (3/7) | 1.063 (3/7) | 1.025 (0/7) | 0.964 (4/7) | 2.210 (0/7) | 0.924 (5/7) | 1.256 (0/7) | 1.017 (3/7) |
| 4 | get_p16 | 1.133 (3/7) | 1.164 (1/7) | 1.052 (0/7) | 0.906 (4/7) | 2.142 (0/7) | 1.084 (3/7) | 1.040 (0/7) | 0.904 (4/7) |
| 4 | set_p16 | 1.098 (1/7) | 1.076 (1/7) | 1.048 (0/7) | 0.973 (5/7) | 2.455 (0/7) | 1.021 (2/7) | 1.021 (2/7) | 0.934 (4/7) |

## Conditions

- stamp: 20261126T034043Z
- start: 2026-11-26T03:40:43Z
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
- gate: 10 s sample; CPUs [0, 1, 2, 3, 4, 5, 6, 7] mean <= 15% and each <= 35%, client CPUs mean <= 35%, machine mean <= 30%, loadavg <= 8.0; wait up to 1800 s at start, 600 s per repetition
- rejection: 1-core runs with interference > 0.05 repeated up to 2 times
- dry run: False
- end: 2026-11-26T03:58:06Z
- loadavg (1 min) during runs: [3.2, 9.28]
- interference on server CPUs + SMT siblings: median 0.1072, max 1.73; rejected and repeated 1-core runs: 18 of 115
- lowest perf 'percent running' (multiplexing): 100.0
- other processes' CPU time on CPUs [0, 1, 2, 3, 4, 5, 6, 7] during accepted runs (top 5 per run, summed; names kept off the repository): 311.6 s
- gate checks: 13 (2 busy)
- median busy fraction per CPU during accepted runs:

  0:0.13 1:0.04 2:0.83 3:0.02 4:0.65 5:0.06 6:0.22 7:0.05 8:0.22 9:0.08 10:0.18 11:0.06 12:0.15 13:0.02 14:0.12 15:0.02 16:0.35 17:0.35 18:0.33 19:0.33 20:0.32 21:0.31 22:0.32 23:0.31 24:0.12 25:0.11 26:0.11 27:0.09 28:0.13 29:0.11 30:0.10 31:0.10
