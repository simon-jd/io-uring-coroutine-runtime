# Check D: fresh clones, three more quiet windows

An independent rerun of the improvement round. `improve` (10c00f5) and the
published `main` (a48c76c) were cloned fresh into `~/.cache/rl-check-d/`
on the build machine and each was built in its own clone. Only the
pinned tarballs were copied from the repository's `.cache/downloads/`. Their
checksums matched, and nothing was downloaded again: `logs/tarballs-*.txt`
show the same inodes before and after the builds. Builds and tests ran on
CPUs 24-31 under `nice`. Timing ran at nice 0 inside quiet windows, and every
arm used `main`'s load generator.

## Commands

On the build machine, in each clone:

```sh
# both clones: the README's build commands
scripts/build-liburing.sh && scripts/fetch-asio.sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
# improve clone: tests (taskset -c 24-31 nice ...)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j && ctest --test-dir build --output-on-failure
scripts/test-matrix.sh <out>/test-matrix
python3 scripts/deliberate-bugs.py --out <out>/deliberate-bugs
git status --porcelain && cmake --build build -j && ctest --test-dir build --output-on-failure   # restored tree
scripts/run-fuzz.sh 300 <out>/fuzz                 # fresh corpora (the clone's own .cache)
scripts/build-valkey.sh
build-release/kv_server --io uring --cores 2 --cpus 26,28 --port 16379   # then valkey-cli set k v / get k, SIGINT
# both clones: callgrind (GET, depth 16, 8 s)
cmake -S . -B build-prof -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -g" && cmake --build build-prof -j16
scripts/callgrind-profile.sh <clone>/build-prof <uring|epoll> <1|2|4> 16 8 <out>/profile/<main|imp>-<io>-<n>c-p16
scripts/callgrind-summary.py main=<dir> imp=<dir> epoll-main=<dir> epoll-imp=<dir> > profile/summary-<n>c.md
scripts/callgrind-jobs.py "io_uring 1c=<dir>" ... > profile/jobs.md
# timing, from the improve clone, in a quiet window
A="--arm uring-main=uring@<main>/build-release --arm uring-imp=uring@<improve>/build-release
   --arm epoll-imp=epoll@<improve>/build-release --arm epoll-main=epoll@<main>/build-release"
python3 scripts/kv-bench.py --suite compare --reps 7 --cores 1,2 --workloads get_p1,get_p16,set_p16 \
  --max-interference 0.05 --retries 3 --kvload <main>/build-release/kvload $A --out session-N
python3 scripts/kv-bench.py --suite compare --reps 7 --cores 4 --harness-cpu 11 --workloads get_p16,set_p16 \
  --max-interference 0.05 --retries 3 --kvload <main>/build-release/kvload $A --out session-N/cores4
python3 scripts/kv-summary.py session-N uring-main:epoll-imp      # and session-N/cores4
```

All README commands worked as written. The server command ran on CPUs
26,28 and port 16379 instead of 2,4 and 6379, so it stayed off the
measurement CPUs. It answered SET and GET from `valkey-cli` and exited with 0
after SIGINT (`logs/smoke.log`).

## Files

- `session-1/`, `session-2/` (1 and 2 cores) and their `cores4/` (4 cores), and
  `session-3/cores4/`: `compare-meta.txt` and `summary.md` (from
  `scripts/kv-summary.py`). The suite prints one line per run, with
  interference, in `logs/s<N>-cores<12|4>.log`. The raw JSON lines, with each
  run's per-process interference record, are kept on the build machine in
  `~/.cache/rl-check-d/out/session-<N>/`.
- `RULES.md`: how the claims are judged. It was written after session 1 and before session 2.
- `test-matrix/`, `deliberate-bugs/`, `fuzz/`: the scripts' own outputs.
- `profile/`: callgrind dumps, `summary-<n>c.md` and `jobs*.md`.
- `logs/`: one log per step (`driver.log` has the timeline).

## Sessions (2026-11-10, PST)

| session | time | parts | interference, median share of the window (1 / 2 / 4 cores) | runs rejected and repeated (1 core) |
| --- | --- | --- | ---: | ---: |
| 1 | 15:16–15:42 | 1–2 cores, 4 cores | 0.008 / 0.080 / **1.13** | 17 |
| 2 | 16:49–17:14 | 1–2 cores, 4 cores | 0.006 / 0.033 / 0.125 | 11 |
| 3 | 18:21–18:27 | 4 cores | — / — / 0.178 | — |

In session 1's 4-core part, another project's process (`auto-app`) ran on the
server CPUs' SMT siblings, for 173 s of the part's runs in all. By
`RULES.md` that part cannot be used, so it was rerun alone as session 3, more
than an hour after session 2. Its numbers are shown in brackets below.

## Results

Server CPU per request, paired by repetition: the median, with the number of
repetitions below 1 out of 7 in brackets. The 1- and 2-core rows give sessions
1 / 2, and the 4-core rows give sessions 3 / 2.

| | io_uring / epoll | io_uring / `main` | `main` / epoll | epoll / epoll of `main` |
| --- | ---: | ---: | ---: | ---: |
| 1 core, GET depth 1 | 0.951 (7) / 0.967 (5) | 0.983 (5) / 0.990 (4) | 0.953 (6) / 0.972 (5) | 0.989 (6) / 0.991 (4) |
| 1 core, GET depth 16 | 0.973 (6) / 1.025 (2) | 0.991 (4) / 0.998 (4) | 0.982 (4) / 1.018 (2) | 1.021 (3) / 1.008 (3) |
| 1 core, SET depth 16 | 1.035 (1 of 6) / 1.075 (1) | 0.941 (7) / 0.974 (5) | 1.092 (1 of 6) / 1.069 (2) | 0.997 (3 of 6) / 1.020 (3) |
| 2 cores, GET depth 1 | 1.011 (3) / 0.977 (4) | 0.996 (4) / 1.002 (3) | 0.996 (4) / 0.999 (4) | 0.950 (6) / 0.982 (4) |
| 2 cores, GET depth 16 | 0.988 (4) / 1.001 (3) | 1.005 (3) / 0.940 (5) | 0.982 (4) / 1.046 (0) | 0.929 (4) / 0.989 (5) |
| 2 cores, SET depth 16 | 1.084 (1) / 1.108 (0) | 0.985 (4) / 1.011 (3) | 0.980 (4) / 1.128 (0) | 0.937 (4) / 0.979 (4) |
| 4 cores, GET depth 16 | 1.053 (2) / 1.096 (0) [0.961 (4)] | 0.943 (7) / 0.950 (6) [0.953 (6)] | 1.110 (1) / 1.130 (0) [1.013 (3)] | 1.003 (3) / 1.002 (3) [1.046 (2)] |
| 4 cores, SET depth 16 | 1.065 (3) / 1.106 (1) [1.034 (2)] | 0.885 (7) / 0.963 (7) [0.953 (5)] | 1.180 (1) / 1.149 (1) [1.085 (1)] | 0.980 (4) / 0.994 (4) [0.929 (5)] |

Requests per second, paired, at depth 16, with the number of repetitions above 1:

- io_uring / `main`: GET at 2 cores 0.869 (1) / 1.017 (5); SET at 2 cores
  0.926 (2) / 0.997 (3); GET at 4 cores 1.021 (5) / 1.034 (4) [1.020 (5)]; SET at 4
  cores 1.154 (7) / 1.034 (6) [1.111 (5)].
- io_uring / epoll, SET: 1 core 0.974 (2 of 6) / 0.928 (1); 2 cores 0.915
  (2) / 0.874 (1).

User / system time per GET at 4 cores, ns: io_uring 145 / 202 and
160 / 208, epoll 135 / 203 and 128 / 204, `main` 168 / 206 and 167 / 206
(sessions 3 and 2).

Callgrind, user-space instructions per GET at depth 16 (`profile/summary-*.md`),
for 1 / 2 / 4 cores: `main` 967 / 1,152 / 1,397; this round 913 / 1,083 /
1,241; epoll of `main` 886 / 1,022 / 1,114; epoll of this round 853 / 997 /
1,094. The round's profiles have 966 / 1,151 / 1,397, 1,083 and 1,242 (c3),
886 / 1,022 / 1,114 and 853 / 997 / 1,093. The front end's jobs
(`profile/jobs.md`) match the round's table exactly: 79 / 116 / 195 for io_uring
and 15 / 26 / 47 for epoll.

Tests in the improve clone:
- Test matrix: 156 cases pass in all 6 compiler × sanitizer configurations.
- Deliberate bugs: 15 of 15 caught by their named assertions, and every unmutated
  baseline passed.
- Afterwards the clone was clean and ctest passed 10 of 10.
- Fuzzing, 300 s per target from fresh corpora: IPC validator 147,508,500
  executions, RESP parser 1,732,461, no findings.

## Claims

The rule (`RULES.md`): a claim stays only if it holds in both usable check-D
sessions and in the round's own final sessions (`imp-session-1`,
`imp-session-2`). Every number was also checked against its committed file.
All the numbers in the README table and in Part 4's tables match their summaries.
Three sets of figures can only be recomputed from the raw lines, because no
summary prints them:
- the final sessions' `main` / epoll ratios;
- the correlations, busy shares and throughputs in Part 4's "GET throughput
  at 2 cores";
- the per-thread user and system time of window 1 in Part 3.

| claim | check D | verdict |
| --- | --- | --- |
| io_uring 4–8% less CPU than epoll, 1 core, depth 1 (GET 7 of 7 in both) | 0.951 (7), 0.967 (5) | holds; 3–8% over the four sessions |
| io_uring 3–9% more than epoll at 2 cores, depth 16 | GET 0.988 (4 below), 1.001 (3); SET 1.084, 1.108 | **GET does not hold**; SET holds (7–11% over the four sessions) |
| io_uring 7–12% more than epoll at 4 cores | GET 1.053, 1.096; SET 1.065 (4 of 7 above), 1.106 | holds as a loss; 5–12% over the four sessions |
| io_uring 3–8% more than epoll, SET at 1 core, depth 16 | 1.035, 1.075 | holds |
| `main` 12–15% more than epoll at 4 cores | 1.110 / 1.180, 1.130 / 1.149 | holds; 11–18% over the four sessions |
| this round 1–4% below `main` at 4 cores in the final sessions (4–6 of 7) | 0.943 / 0.885, 0.950 / 0.963 (6–7 of 7) | holds; larger here (4–12%) |
| no difference against `main` held in both final sessions at 1 and 2 cores | 1-core SET depth 16 lower in both check-D sessions (0.941, 0.974), but not in the round's | holds as stated |
| what is left is user time; system time the same (4 cores) | user 145 vs 135 and 160 vs 128 ns; system 202 vs 203, 208 vs 204 | holds |
| −6% / −11% user-space instructions at 2 / 4 cores (callgrind) | 6.0% / 11.2% | holds |
| 60 / 86 / 149 more instructions than epoll at 1 / 2 / 4 cores | 60 / 86 / 147 (the 1-core figure measured here for the first time; the round derived it from c2) | holds |
| within each session the gap to epoll at 4 cores is smaller than `main`'s | 5–7% against 11–18% (session 3), 10–11% against 13–15% (session 2) | holds |
| c2 served 5–6% more GETs per second than `main` at 4 cores (windows A and B, 7 of 7) | window C 1.005 (4 of 7); final sessions 0.953, 0.980 (3 of 7 above); check D 1.021, 1.034 (5 and 4 of 7) | **does not hold** outside windows A and B |
| one hash per key: the epoll build's CPU per request 1–2% lower | epoll / epoll of `main` 0.93–1.02 at depth 16, at most 5 of 7 | not resolved here (the text claims only the window rule and the instruction count) |
| left out: 2-core GET requests per second against `main` | 0.869 (1 of 7 above), 1.017 (5); final sessions 0.977 (2), 0.926 (0) | **missing loss**: lower in three of four sessions |
| left out: SET requests per second against epoll at depth 16 | 1 core 0.974, 0.928; 2 cores 0.915, 0.874; final sessions 0.936, 0.970 and 0.876, 0.871 | **missing loss**: lower in all four |
| window results (c1, c2, c3, c4 tables, rules) | not rerun (candidate builds) | match their files; window C's c2 result (0.990 and 0.972 at 4 cores, 5 of 7) was not in Part 4 |
| 156 cases × 6 configurations, 15 of 15 bugs, both fuzz targets clean | same | holds |
