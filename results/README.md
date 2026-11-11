# Results

All runs on one shared machine: Intel Core i9-13900K (CPUs 0–15 are 8 P-cores ×
2 SMT threads, 16–31 are E-cores), Linux 7.0.0 (-38-generic in round 3,
-42-generic in the performance round), GCC 15.2, clang 21.1.8, liburing 2.14
(pinned, static). Every file below was produced by a script in `scripts/`; the
commit each run used is recorded in the file or in its `*-meta.txt`. Round-3
commits belong to the development history, which was squashed into a single
commit when the repository was published; performance-round commits are on
this branch, except the candidates' own commits (their diffs are in
[perf/candidates/](perf/candidates)). Other projects ran on the machine at the same time (a model
training job that takes one full CPU and is not pinned, container services,
another project's builds and VMs). Numbers from different sessions are never
put in one comparison.

## Improvement round (2026-11-09 and 11-10)

What was changed, what was dropped and why: [design-tradeoffs.md, Part
4](../docs/design-tradeoffs.md). Each candidate was built from its own
source tree (`.cache/imp/<name>` on the build machine; `SOURCE` in it names
the branch and commit) and interleaved with the published `main` (a48c76c)
in one quiet window; each window's rules were committed before it ran.
Candidates: c1 (call nodes in their slots), c2 (c1 + chunked slot table),
c3 (c2 + one hash per key), c4 (c3 + inline receive path).

| Directory | Command | Builds | What it shows |
| --- | --- | --- | --- |
| [improve/wA/](improve/wA) | `scripts/kv-bench.py --suite compare --reps 7 --cores 1,2 --workloads get_p1,get_p16,set_p16 --max-interference 0.05 --retries 3 --kvload <main>/kvload --arm NAME=IO@BUILD ...`, then `--cores 4 --workloads get_p16,set_p16 --harness-cpu 11 --out .../cores4` | main; c1; c2; epoll of main | c1 alone did not pass, and neither did c2 (a worse cell: 2 cores, SET depth 16, 1.0203 with 5 of 7 higher, printed as 1.020 in the summary); 2026-11-09 23:50 – 11-10 00:16 PST |
| [improve/wB/](improve/wB) | the same | main; c2; c3; c4; epoll of main and of c3 | c2 passed and is kept on this window (best cell 0.968), c3 passed, c4 rejected; 04:52–05:30 |
| [improve/wC/](improve/wC) | the same | main; c2; c3; epoll of main and of c3 | c3 passed a second time; 06:48–07:19 |
| [improve/profile/](improve/profile) | `scripts/callgrind-profile.sh <build> <io> <cores> 16 8 <dir>`, `scripts/callgrind-summary.py`, `scripts/callgrind-jobs.py` | main, c1–c4, epoll of main and of c3 | user-space instructions per request by module and by job (GET depth 16, 1, 2 and 4 cores); `sim-*`: c3 and c4 with callgrind's cache and branch simulation |
| [improve/candidates/](improve/candidates) | `git diff` against the build it was compared with | | the rejected c4 |
| [kv-bench/imp-session-1/](kv-bench/imp-session-1) | `scripts/kv-bench.py --suite main --reps 7 --cores 1,2 --max-interference 0.05 --retries 3 --build <final> --arm uring-main=uring@<main>`, then `--cores 4 --only get_p16,set_p16 --harness-cpu 11 --out .../cores4` (`.cache/imp/final-session.sh`) | c9bf7bb (`git archive`) against main a48c76c, epoll, Asio, Valkey | the final comparison; 2026-11-10 07:44–08:58 PST |
| [kv-bench/imp-session-2/](kv-bench/imp-session-2) | the same | the same | the same comparison, 11:13–12:28 |
| [test-matrix/](test-matrix) | `taskset -c 10-15,24-31 nice scripts/test-matrix.sh` | 660d3d7 (code of c9bf7bb) | 10 suites, 156 cases, GCC and clang, each plain, ASan + UBSan and TSan: 6 of 6 pass |
| [deliberate-bugs/](deliberate-bugs) | `taskset -c 10-15,24-31 nice python3 scripts/deliberate-bugs.py` | 660d3d7 | 15 contract violations (new: n, a call node destroyed at detach; o, the wrong hash carried to the owner), each caught by its named assertion |
| [improve/check-d/](improve/check-d) | the README build commands in fresh clones, the test scripts, `scripts/callgrind-profile.sh`, and `scripts/kv-bench.py --suite compare --reps 7` (exact commands in its README) | fresh clones of main a48c76c and improve 10c00f5, built separately | an independent check of the round; 2026-11-10 15:16–18:27 PST |
| [fuzz/](fuzz) | `scripts/run-fuzz.sh 600` | the c9bf7bb sources (`fuzz/SOURCE.txt`) | IPC validator 512,780,559 executions, RESP parser 4,511,062; no findings |

### Final comparison: imp-session-1 / imp-session-2

Server CPU per request, paired by repetition (median; repetitions below 1
of 7), the io_uring build of this round against epoll and against the
published `main`:

| | io_uring / epoll, GET | io_uring / epoll, SET | io_uring / `main`, GET | io_uring / `main`, SET |
| --- | ---: | ---: | ---: | ---: |
| 1 core, depth 1 | 0.959 (7) / 0.924 (7) | 0.964 (7) / 0.962 (6) | 1.013 (3) / 0.985 (5) | 0.986 (6) / 0.997 (4) |
| 1 core, depth 16 | 0.972 (5) / 0.970 (5) | 1.075 (1) / 1.033 (1) | 0.990 (5) / 0.957 (6) | 1.006 (2) / 0.976 (5) |
| 2 cores, depth 1 | 0.974 (4) / 0.975 (6) | 1.005 (2) / 0.995 (4) | 0.999 (4) / 1.003 (3) | 1.007 (3) / 1.000 (3) |
| 2 cores, depth 16 | 1.030 (0) / 1.091 (2) | 1.082 (1) / 1.075 (3) | 0.926 (6) / 1.070 (3) | 0.961 (6) / 0.984 (4) |
| 4 cores, depth 16 | 1.116 (1) / 1.073 (1) | 1.085 (2) / 1.108 (0) | 0.989 (5) / 0.972 (4) | 0.966 (6) / 0.967 (5) |

- **In both final sessions:** against epoll, 4–8% less CPU per request at 1
  core and depth 1 (GET and SET, 6–7 of 7), and more at depth 16 for GET at 2
  cores (3% and 9%), at 4 cores (7–12%) and for SET at 1 core (3–8%). SET at 2
  cores, depth 16, needed more by median in both (1.082 and 1.075) but at only
  4 of 7 in session 2. In the same sessions `main` / epoll was 1.131 / 1.153
  (GET) and 1.124 / 1.130 (SET) at 4 cores. Check D (below) did not run SET at
  depth 1, so the README claims GET only there; it did not confirm the 2-core
  GET loss; in its session 3 the 4-core GET (5.3%) is outside 7–12% and the
  4-core SET was higher at only 4 of 7 (the four-session ranges are below).
- **Losses in throughput** (requests per second, paired by repetition from
  `main.jsonl`; repetitions higher of 7): against `main` at 2 cores, GET
  0.977 / 0.926 (2 and 0) and SET 0.967 / 0.985 (1 and 2); against epoll at
  depth 16, SET 0.936 / 0.970 at 1 core (1 and 1) and 0.876 / 0.871 at 2
  cores (0 and 3).
- **Not resolved here:** the change against `main`. At 4 cores it was 1–4%
  less in both sessions (4–6 of 7); at 2 cores it changed sign; at 1 core, GET
  depth 16 was lower in both (0.990 and 0.957, 5 and 6 of 7), in check D only
  at 4 of 7. The decision windows above measured 3–5% at 4 cores with 7 of 7
  in at least one cell each.
- Session 1's 4-core runs had another project's job on the server CPUs' SMT
  siblings (interference median 0.87 of the window; session 2: 0.05; the
  recorded bystander was a process named `auto-app`). Check D's test for an
  unusable part (median above 0.5) would discard them; they are kept, for
  the reason given in check D's README. At 1 and 2 cores the rejection rule
  repeated 5 runs in session 1 and 13 in session 2.

### Check D: fresh clones, three more quiet windows

`main` and `improve` were cloned fresh and built separately. Every arm used
`main`'s load generator, and the build-comparison suite ran at 7 repetitions
with the epoll build of both trees. Session 1 (15:16–15:42 PST) and session 2
(16:49–17:14) each ran 1, 2 and 4 cores. In session 1's 4-core part,
another project's job ran on the server CPUs' SMT siblings (interference
median 1.13), so by the rules written before session 2
([check-d/RULES.md](improve/check-d/RULES.md)) that part was rerun alone as
session 3 (18:21–18:27). The commands, all tables and a claim-by-claim list
are in [improve/check-d/](improve/check-d).

The test matrix passed in all 6 configurations (156 cases), 15 of 15
deliberate bugs were caught, the restored tree passed, and both fuzz
targets ran 300 s clean. Callgrind on the fresh builds reproduced the round's
instruction counts within 2 per request. Server CPU per request, paired
(median; repetitions below 1 of 7), with 1 and 2 cores from sessions 1 / 2
and 4 cores from sessions 3 / 2:

| | io_uring / epoll, GET | io_uring / epoll, SET | io_uring / `main`, GET | io_uring / `main`, SET |
| --- | ---: | ---: | ---: | ---: |
| 1 core, depth 1 | 0.951 (7) / 0.967 (5) | | 0.983 (5) / 0.990 (4) | |
| 1 core, depth 16 | 0.973 (6) / 1.025 (2) | 1.035 (1 of 6) / 1.075 (1) | 0.991 (4) / 0.998 (4) | 0.941 (7) / 0.974 (5) |
| 2 cores, depth 16 | 0.988 (4) / 1.001 (3) | 1.084 (1) / 1.108 (0) | 1.005 (3) / 0.940 (5) | 0.985 (4) / 1.011 (3) |
| 4 cores, depth 16 | 1.053 (2) / 1.096 (0) | 1.065 (3) / 1.106 (1) | 0.943 (7) / 0.950 (6) | 0.885 (7) / 0.963 (7) |

By its rules two claims hold over the four sessions: less CPU per GET than
epoll at 1 core, depth 1 (3–8%), and more for SET at 1 core, depth 16 (3–8%).
The losses to epoll at 2 cores for SET and at 4 cores were on the losing side
by median in all four sessions but do not hold as stated: 2-core SET was 10.8%
in session 2 (claimed 3–9%; 7–11% over the four) and at 4 of 7 in
imp-session-2; at 4 cores GET was 5.3% in session 3 (claimed 7–12%; 5–12% over
the four) and SET at 4 of 7 there. `main`'s 12–15% over epoll at 4 cores was
18.0% for SET in session 3 (11–18% over the four). The 2-core GET loss to
epoll does not hold (0.99 and 1.00). The gain over `main` at 4 cores, 1–4% in
the final sessions (GET at 4 of 7 in imp-session-2), was 4–12% here (6–7 of
7). At 1 and 2 cores no CPU difference against `main` met the rule in all four
sessions: 1-core GET depth 16 was lower in both final sessions but at only 4
of 7 here (0.991, 0.998), and 1-core SET depth 16 was lower here (0.941,
0.974) and in imp-session-2 (0.976) but not in imp-session-1 (1.006).

Three losses were missing from the text before check D (requests per second,
paired by repetition): GET at 2 cores against `main`, lower in three of the
four sessions (0.977, 0.926, 0.869; 1.017 in check D's session 2); SET at 2
cores against `main`, lower by median in all four (0.967, 0.985, 0.926,
0.997; 4 of 7 in check D's session 2); and SET against epoll at 1 and 2
cores, depth 16, lower in all four (1 core 3–7%, 4 of 6 in check D's session
1; 2 cores 8–13%, 4 of 7 in imp-session-2). Window C's c2 result was missing
too: against `main` at 4 cores it was 0.990 and 0.972 (5 of 7), with GET
throughput 1.005 (4 of 7 higher). In window A, c2 failed its rule's test for
a worse cell (2 cores, SET depth 16: 1.0203 with 5 of 7 higher; the summary
prints 1.020); it is kept on window B, whose best cell (0.968) is an effect
of 3% or more. The README and design-tradeoffs Part 4 were corrected after
check D and its review, including the claim that c2's saving showed as GET
throughput at 4 cores.


## Performance round (2026-11-08 and 11-09)

What was optimized, what was dropped and why: [design-tradeoffs.md, Part
3](../docs/design-tradeoffs.md). Every comparison below interleaves builds
from separate source trees in one quiet window; each window's decision rules
were committed before it ran (`RULES.md` in its directory).

| Directory | Command | Builds | What it shows |
| --- | --- | --- | --- |
| [perf/w1/](perf/w1) | `scripts/kv-bench.py --suite compare --reps 7 --cores 1,2 --workloads get_p1,get_p16,set_p16 --max-interference 0.05 --retries 3 --arm NAME=IO@BUILD ...` | main 3eb4121; main + one change each: engine (kept), reactor bookkeeping, replies into the send buffer, registered ring descriptor | engine change kept, the other three rejected; 2026-11-08 22:25–22:59 PST |
| [perf/w2/](perf/w2) | the same with `--cores 2,4`, no rejection rule | 3796efd (main + engine), + spin before sleeping, + load-generator change; main | spin (both transports) removed, load-generator change not kept; at 4 cores, depth 16, the generator uses 39–59% of its CPUs; 11-09 03:36–04:08 |
| [perf/w3/](perf/w3) | `--cores 2,3,4 --workloads get_p16,set_p16` | 3796efd, + `call_all` | `call_all` kept; 04:55–05:08 |
| [perf/candidates/](perf/candidates) | `git diff` against the build each was compared with | | the rejected changes, to measure again |
| [profile/](profile) | `scripts/callgrind-profile.sh`, `scripts/callgrind-summary.py` | main, 3796efd, 5d86cbd, the window-1 candidates | user-space instructions per request by module (`summary.md`) |
| [kv-bench/perf-session-a/](kv-bench/perf-session-a) | `scripts/kv-bench.py --suite main --reps 7 --cores 1,2 --max-interference 0.05 --retries 3 --build <5d86cbd build> --arm uring-main=uring@<main build>`, then `--cores 4 --only get_p16,set_p16 --harness-cpu 11 --out .../cores4` | 5d86cbd against main 3eb4121 | the final comparison; 11-09 08:27–09:42 |
| [kv-bench/perf-session-b/](kv-bench/perf-session-b) | the same | the same | the same comparison, 11-09 10:44–11:58 |
| [kv-bench/perf-check-c/](kv-bench/perf-check-c) | `scripts/kv-bench.py --suite compare --reps 5 --cores 1,2 --workloads get_p1,get_p16,set_p16 --max-interference 0.05 --retries 3 --kvload <main build>/kvload --arm uring-main=uring@<main build> --arm uring-perf=uring@<perf build> --arm epoll-perf=epoll@<perf build>`, then `--cores 4 --harness-cpu 11 --workloads get_p16,set_p16 --out .../cores4` | fresh clones of main 3eb4121 and perf 932122c, built separately | an independent check of the final comparison; 11-09 13:37–13:50 |
| test-matrix/ (in git history at a48c76c; the directory now holds the improvement round's run) | `taskset -c 10-15,24-31 nice scripts/test-matrix.sh` | 5d86cbd | 10 suites, 151 cases, GCC and clang, each plain, ASan + UBSan and TSan (Debug): 6 of 6 pass |
| deliberate-bugs/ (git history at a48c76c) | `taskset -c 10-15,24-31 nice python3 scripts/deliberate-bugs.py` | 5d86cbd (script 8d6b3be) | 13 contract violations, each caught by a named assertion |
| fuzz/ (git history at a48c76c) | `taskset -c 10-15,24-31 nice scripts/run-fuzz.sh 600` | 5d86cbd | 600 s per target: IPC validator 302,901,197 executions, RESP parser 1,818,354 (each input now also checked against the reference parser and map); no findings |

Both final sessions ran the servers built from an archive of 5d86cbd
(`.cache/final-5d86cbd` on the build machine). Session A's meta file says
"5d86cbd (uncommitted changes)": the test-matrix logs in the working tree had
just been rewritten. Session B's says 120f2ca, the branch head by then; the
commits in between changed only scripts and results. Changes to the protocol since round 3: server and load generator
at nice 0 (nice +10 let other jobs crowd the measured CPUs), every summary
splits server CPU into user and system time, and the 4-core cells (CPUs
2,4,6,8) run the harness on CPU 11 instead of 9, the sibling of CPU 8. The
rejection rule is unchanged (1 core only). The 4-core cells are depth 16 only:
at depth 1 the load generator is the limit (window 2: 86–89% of its CPUs).

### Final comparison: perf-session-a / perf-session-b (GET, medians of 7)

| | io_uring | io_uring, `main` | epoll | Asio | Valkey |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 core, depth 1: requests/s | 346 k / 351 k | 338 k / 338 k | 327 k / 334 k | 325 k / 325 k | 296 k / 298 k |
| 1 core, depth 1: CPU per request, µs | 2.88 / 2.86 | 2.91 / 2.96 | 3.02 / 2.99 | 3.03 / 3.05 | 3.41 / 3.34 |
| 1 core, depth 16: requests/s | 3.72 M / 3.70 M | 2.68 M / 2.96 M | 3.56 M / 3.54 M | 3.55 M / 3.48 M | 2.14 M / 2.16 M |
| 1 core, depth 16: CPU per request, µs | 0.269 / 0.271 | 0.370 / 0.335 | 0.286 / 0.280 | 0.285 / 0.290 | 0.466 / 0.463 |
| 2 cores, depth 1: requests/s | 639 k / 633 k | 623 k / 613 k | 628 k / 634 k | 569 k / 575 k | 292 k / 290 k |
| 2 cores, depth 16: requests/s | 5.47 M / 5.22 M | 5.49 M / 5.37 M | 4.94 M / 5.07 M | 3.64 M / 3.77 M | 3.16 M / 3.15 M |
| 2 cores, depth 16: CPU per request, µs | 0.303 / 0.315 | 0.364 / 0.367 | 0.295 / 0.290 | 0.328 / 0.318 | 0.650 / 0.654 |
| 4 cores, depth 16: requests/s | 8.92 M / 8.36 M | 8.88 M / 8.80 M | 9.11 M / 8.99 M | 7.28 M / 7.18 M | 3.99 M / 3.94 M |
| 4 cores, depth 16: CPU per request, µs | 0.363 / 0.368 | 0.436 / 0.436 | 0.318 / 0.320 | 0.350 / 0.352 | 0.976 / 0.988 |

Server CPU per request, paired by repetition (median; repetitions below 1 of 7):

| | io_uring / epoll, GET | io_uring / epoll, SET | io_uring / `main`, GET | io_uring / `main`, SET |
| --- | ---: | ---: | ---: | ---: |
| 1 core, depth 1 | 0.950 (7) / 0.936 (7) | 0.944 (6) / 0.959 (6) | 0.971 (6) / 0.950 (6) | 0.969 (5) / 1.001 (3) |
| 1 core, depth 16 | 0.948 (7) / 0.943 (6) | 1.043 (1) / 1.023 (1) | 0.741 (7) / 0.796 (7) | 0.815 (7) / 0.805 (7) |
| 2 cores, depth 1 | 0.982 (5) / 1.009 (2) | 0.997 (5) / 0.988 (5) | 0.975 (7) / 0.978 (5) | 0.999 (5) / 0.970 (7) |
| 2 cores, depth 16 | 1.036 (1) / 1.059 (0) | 1.080 (1) / 1.051 (1) | 0.841 (6) / 0.842 (7) | 0.815 (7) / 0.807 (7) |
| 4 cores, depth 16 | 1.117 (0) / 1.114 (0) | 1.133 (0) / 1.123 (0) | 0.833 (7) / 0.844 (7) | 0.815 (7) / 0.823 (7) |

- **Holds in both sessions:** against the published code, 16–26% less CPU per
  request at depth 16 on 1, 2 and 4 cores (6 or 7 of 7 repetitions in every
  cell), 2–5% at depth 1 for GET. Against epoll, less CPU per request at 1
  core (depth 1 GET and SET, depth 16 GET), more at 2 cores and depth 16
  (4–8%) and at 4 cores (11–13%, no repetition the other way). Check C
  (below) did not reproduce the 1-core depth-16 GET result. Valkey needs
  1.7 times (1 core) to 2.7 times (4 cores) the io_uring server's CPU per GET
  at depth 16.
- **Not consistent or small:** 2 cores, depth 1 (within 2%; GET either sign);
  SET at 1 core, depth 16
  (io_uring 2–4% more, 1 of 7 the other way in both). At 2 and 4 cores the
  lower CPU per GET did not raise GET throughput over `main` (cores idle
  more; not investigated), while SET throughput rose (2 cores: 5.72 / 5.84 M
  against 5.13 / 5.06 M). In check C it did rise at 2 cores (1.14, 5 of 5),
  so this observation is not stable either.
- The extra io_uring cost is user time: at 4 cores, depth 16, GET 166 / 163 ns
  user against epoll's 123 / 123, system 198 / 204 against 196 / 197 (tables
  "split into user and system time").
- The 4-core cells used 48–49% of the load generator's 8 CPUs (median; at
  most 56%). Rejected and repeated at 1 core: 12 runs in A, 5 in B; every
  cell has 7 accepted repetitions. Open-loop medians agree across the front
  ends (1 core, 200 k requests/s: 15–16 µs in both sessions); tails are in
  the summaries without a ranking.

### Check C: fresh clones, one more quiet window

Main and perf were cloned fresh and built separately (perf 932122c has the
same code as 5d86cbd). All three arms used main's load generator, and the
build-comparison suite ran at 5 repetitions (13:37–13:50 PST). Its meta file
says "(uncommitted changes)" because the test matrix, deliberate bugs and a
60 s fuzz run had just rewritten result logs in that clone; all of them
passed, with 13 of 13 bugs caught. Server CPU per request, paired by
repetition (median; repetitions below 1 of 5):

| | io_uring / `main`, GET | io_uring / `main`, SET | io_uring / epoll, GET | io_uring / epoll, SET |
| --- | ---: | ---: | ---: | ---: |
| 1 core, depth 1 | 0.964 (3) | | 0.955 (5) | |
| 1 core, depth 16 | 0.749 (5) | 0.814 (5) | 1.021 (2) | 1.050 (0) |
| 2 cores, depth 1 | 0.898 (5) | | 0.995 (3) | |
| 2 cores, depth 16 | 0.823 (5) | 0.814 (5) | 1.083 (0) | 1.103 (0) |
| 4 cores, depth 16 | 0.842 (5) | 0.810 (5) | 1.172 (0) | 1.130 (0) |

It confirms the gain over the published code and the losses to epoll at 2
and 4 cores (here somewhat larger), but not the 1-core depth-16 GET result
(1.02 instead of 0.95), which the README therefore does not claim.

### Tests at 5d86cbd

The improvement round added 5 cases (`call_nodes_live_in_their_slots` on
each of the 5 transports; 156 in all) and mutations n and o (above).

| Suite | Cases | Covers |
| --- | ---: | --- |
| task | 7 | lazy tasks, ready list |
| reactor | 19 | operation lifetime, cancellation, shutdown, `when_all`, direct descriptors |
| buffer | 9 | fixed pool, provided rings, SEND_ZC ownership |
| runtime | 54 | 10 cross-core cases (3 of them `call_all`) × MSG_RING batched / one MSG_RING per message / busy-poll / doorbell / doorbell busy-poll, plus 4 refused-MSG_RING cases |
| ipc | 11 | sealed snapshots, descriptor ownership |
| kv | 9 | RESP parser (every split point; against the reference parser), routing plan, shard table against the reference map |
| server_uring, server_uring_doorbell | 12 each | end to end against `kv_server --io uring` (1 to 4 cores), over each transport |
| server_epoll, server_asio | 9 each | the same end-to-end cases against the baselines |

The server suites start the `kv_server` binary built with the same sanitizer
and require exit status 0 after SIGTERM, so a sanitizer report inside the
server fails the test. From the logs (all six configurations): the cancel
acknowledgement was reaped before the target's own CQE in 100 of 100 rounds;
the cancel-vs-complete race produced both outcomes on both ring types (for
example 4,880 cancelled / 5,120 completed on the DEFER_TASKRUN ring under clang
+ TSan); the lost-wake-up stress case had 0 stalls in 4,000 rounds on every
transport, and over the doorbell transport the target's second look at its
queues found a message and skipped the wait between 4 and 1,313 times per
configuration; refused replies were sent again and refused doorbells rung
again.

### Deliberate bugs at 5d86cbd

| Mutation | Caught by (named assertion) |
| --- | --- |
| a: release the buffer at the cancel acknowledgement | `cancel_ack_does_not_release_buffer`: *buffer stays with the operation after cancel-ack* |
| b: give a SEND_ZC buffer back at the first CQE | `send_zc_buffer_held_until_notification`: *zero-copy payload arrives unmodified* |
| c: release the buffer when the awaiting coroutine is destroyed | `destroyed_coroutine_keeps_fixed_buffer_until_cqe`: *a new lease holder's bytes are not overwritten by the kernel* |
| d: skip ownership of descriptors received by a detached recvmsg | `detached_receive_closes_received_descriptor`: *received descriptor closed by the slot* |
| e: submit through a stale direct-descriptor reference | `stale_direct_descriptor_is_refused`: *stale direct descriptor does not reach the new connection* |
| f: reuse a direct-descriptor index while an operation names it | `direct_index_reserved_while_operation_in_flight`: *an index stays reserved while an operation names it* |
| g: one MSG_RING per message despite batching | `concurrent_calls_share_message_rings`: *concurrent calls to one core share one MSG_RING* |
| h: cancel replies at the start of a drain | `shutdown_waits_for_requests_in_progress`: *requests received before shutdown are answered* |
| i: accept a bulk string before all its bytes arrived | `split_input_parses_identically`: *split input parses to the same commands* |
| j: doorbell transport, sleep without looking at the queues again | `sleeping_core_is_woken_for_every_message/doorbell`: *a sleeping core is woken for every message* (the watchdog counts the stall; no hang) |
| k: delete from the shard table without the backward shift | `shard_matches_reference_map`: *shard replies match the reference map* |
| l: let the parser's one-pass number path accept a leading zero | `parser_matches_reference`: *parser matches the reference parser* |
| m: resume a `call_all` caller at the first reply | `call_all_returns_results_in_call_order`: invariant *call_all resumed before every reply arrived* (named n in 5d86cbd's commit message) |

(b), (c) and (e) fail on observed wrong data (bytes on the wire or in a new
holder's buffer, a send reaching another connection); (h) on missing replies;
(i), (k) and (l) on differently parsed commands or different replies; (j) on a
stalled call; (m) on the runtime's own invariant; (a), (d), (f) and (g) on the
ownership or batching state the test inspects. The first run at 5d86cbd
reported (c) as not caught because its anchor in the source no longer matched
(`detach()` gained a line); the mutation was updated to the current code and
the suite run again (`results/deliberate-bugs`).

# Round 3 (2026-11-07)

## Index (round 3)

| Directory | Command | Commit | What it shows |
| --- | --- | --- | --- |
| [kv-bench/session-a/](kv-bench/session-a) | `scripts/kv-bench.py --suite main --reps 7 --cores 1,2 --max-interference 0.05 --retries 3 --out results/kv-bench/session-a` | a7ed06b | the four servers, closed and open loop, 1 and 2 cores; 2026-11-07 11:07–12:02 PST |
| [kv-bench/session-b/](kv-bench/session-b) | the same, `--out results/kv-bench/session-b` | 07ce1f1 | the same comparison, 2026-11-07 13:03–13:59 PST |
| [kv-bench/variants/](kv-bench/variants) | `scripts/kv-bench.py --suite variants --reps 7 --cores 2 --arms uring,uring-doorbell,uring-doorbell-early,uring-coop,uring-coop-doorbell`; `--suite diag --reps 2 --cores 2` | c653dc8 | MSG_RING vs. doorbell transport, ring flags (`variants.jsonl`); the servers' own I/O counters per 1,000 requests (`diag.jsonl`) |
| [probe/](probe) | `scripts/kv-bench.py --suite probe --reps 6 --out results/probe` | 3386a66 | what moves the 1-core depth-1 result: a busy SMT sibling, busy P-cores |
| [xcore/](xcore) | `REPS=7 OPS=400000 scripts/xcore-study.sh 4,6` | c653dc8 | cross-core messaging: ring flags × batching / doorbell × idle mode |
| [kv-bench/round2/](kv-bench/round2) | round 2's suites (main, variants, diag, crosscheck, 4-core calibration) | c562e3b | kept for the results round 3 did not repeat: registered-buffer replies, direct descriptors, the valkey-benchmark cross-check, and why 4 server cores are not compared |
| [perf-sanity/](perf-sanity) | `scripts/perf-sanity.sh 4,6` | 1fc1573 | round 1's quick check (kept for the 188 vs. 104 ns post-flood figure the study explains) |

Each `summary.md` is generated by `scripts/kv-summary.py <dir>` from the raw
JSON lines next to it. a7ed06b, c653dc8, df7ae97 and 07ce1f1 build the same
server code: between them only comments, docs, scripts and results changed, and
`kv_server` built at df7ae97 has the same SHA-256 as the build session A used
(`10e94ece…c57f`). The servers' stderr logs are in `.cache/kv-bench-logs/` on
the build machine (not committed).

## Quiet windows

Every publishable benchmark ran inside a quiet window agreed with the kernel
driver project then being developed on the same machine (it started no new builds or VM runs and kept to CPUs
10–15 and 24–31): the probe 06:45–06:51, the cross-core study and variants
07:31–08:01, session A 11:07–12:02 and session B 13:03–13:59 (all PST,
2026-11-07). Jobs of other projects kept running.

The variants directory also holds `attempt-with-rejection.jsonl`: the first
start of the variants suite with the interference rule applied at 2 cores. 77
of its 90 runs exceeded the limit, even after repeats, so it was stopped after
two repetitions and restarted without the rule; one of its runs also
overlapped a manual check on the same CPUs. It is not used for any comparison.

## Setup of the server comparison

- Servers: `kv_server --io uring|epoll|asio` (Release, GCC 15.2) and Valkey
  9.1.3 (`scripts/build-valkey.sh`; `--save "" --appendonly no --io-threads
  <cores> --server-cpulist <cpus>`). All pinned to the first 1 or 2 of CPUs
  2,4 (separate physical P-cores), under `nice`. The io_uring front end uses
  its defaults: DEFER_TASKRUN, batched messages over MSG_RING.
- Load: `bench/kvload`, 8 threads on E-cores 16–23 under `nice`, 50
  connections per server core, 100,000 keys preloaded, 64-byte values.
  - Closed loop: each connection sends a pipeline of 1 or 16 requests and waits
    for all replies; 1 s warm-up, 5 s measured. GET-only or SET-only.
  - Open loop: 90% GET / 10% SET at 50, 100, 200 k requests/s per server core;
    2 s warm-up, 8 s measured; latency from each request's scheduled send time;
    requests unanswered 2 s after the end count as timeouts.
- Every arm restarts its server, preloads, runs all workloads, and must exit 0
  on SIGTERM. Arm order rotates per repetition. The harness runs on CPU 9.
- CPU per request: the server's CPU time over the run (sum of
  `/proc/<pid>/task/*/schedstat`) divided by the requests completed. Cycles
  per request: that times the median of the server CPUs' `scaling_cur_freq`,
  sampled every 0.1 s. On loopback each side's CPU time includes part of the
  other side's TCP work.
- Interference, recorded for every run: busy time of the server CPUs that is
  not the server's own, plus busy time of their SMT siblings (`/proc/stat`),
  as a share of the window. Also recorded: context switches, run-queue wait,
  load on the other P-cores, and (session B only) which other processes used
  CPU time on the server CPUs or their siblings.

**Protocol, fixed in a7ed06b before session A:** 7 repetitions; a closed-loop
run on one server core whose interference exceeds 0.05 is kept in the raw file
marked `rejected` and repeated, up to 3 times, for every arm alike. The rule is
not applied at 2 cores or to open-loop runs (see
[design-tradeoffs.md](../docs/design-tradeoffs.md), "CPU sets, core counts and
noise").

## Server comparison: session A / session B (medians of 7, [min–max] in the summaries)

| GET, closed loop, requests/s | io_uring | epoll | Asio | Valkey |
| --- | ---: | ---: | ---: | ---: |
| 1 core, depth 1 | 336 k / 315 k | 316 k / 305 k | 316 k / 306 k | 292 k / 277 k |
| 1 core, depth 16 | 2.69 M / 2.55 M | 2.71 M / 2.30 M | 2.67 M / 2.52 M | 2.10 M / 1.96 M |
| 2 cores, depth 1 | 604 k / 563 k | 581 k / 535 k | 534 k / 511 k | 279 k / 272 k |
| 2 cores, depth 16 | 4.69 M / 4.40 M | 5.14 M / 4.56 M | 3.01 M / 2.83 M | 3.03 M / 2.90 M |

| Server CPU per GET (µs) | io_uring | epoll | Asio | Valkey |
| --- | ---: | ---: | ---: | ---: |
| 1 core, depth 1 | 2.94 / 3.13 | 3.15 / 3.29 | 3.16 / 3.26 | 3.39 / 3.57 |
| 1 core, depth 16 | 0.376 / 0.391 | 0.370 / 0.437 | 0.371 / 0.396 | 0.474 / 0.505 |
| 2 cores, depth 1 | 3.31 / 3.50 | 3.38 / 3.74 | 3.75 / 3.87 | 6.97 / 7.21 |
| 2 cores, depth 16 | 0.417 / 0.449 | 0.383 / 0.418 | 0.424 / 0.464 | 0.683 / 0.712 |

io_uring / epoll server CPU per request, paired by repetition (median; repetitions
below 1):

| | GET, A | GET, B | SET, A | SET, B |
| --- | ---: | ---: | ---: | ---: |
| 1 core, depth 1 | 0.927 (7 of 7) | 0.939 (7 of 7) | 0.941 (7 of 7) | 0.944 (7 of 7) |
| 1 core, depth 16 | 1.007 (3 of 7) | 0.992 (4 of 6) | 1.000 (3 of 7) | 1.037 (2 of 7) |
| 2 cores, depth 1 | 0.959 (5 of 7) | 0.940 (5 of 7) | 0.980 (4 of 7) | 1.006 (3 of 7) |
| 2 cores, depth 16 | 1.038 (2 of 7) | 1.077 (1 of 7) | 1.029 (1 of 7) | 1.028 (3 of 7) |

- **Holds in both sessions:** the 1-core, depth-1 advantage (6–7% less CPU per
  request, every repetition, GET and SET) and the 2-core, depth-16 loss (3–8%
  more CPU per request than epoll, at most 3 of 7 repetitions the other way).
- **Does not hold or moved:** 1 core, depth 16 has no consistent sign. At 2
  cores, depth 1, GET favours io_uring in 5 of 7 repetitions in both sessions
  and SET is even. Absolute throughput was lower in B for every arm (server
  clock median 5.27 GHz in B vs. 5.38 in A; interference median 0.058 vs.
  0.012); the depth-1 throughput lead was 6% in A and 3% in B. Round 2's
  single-session 11% (336 k vs. 302 k) is not reproduced.
- Open-loop median latencies agree within 1–2 µs across the front ends at every
  rate in both sessions (1 core, 200 k requests/s: 15 / 16 / 15 / 16 µs in A,
  16 / 17 / 17 / 17 µs in B), except Valkey at 2 cores and 400 k requests/s
  (408 and 422 µs), a rate above its 2-core closed-loop throughput. p99 and
  p99.9 swing between repetitions of the same arm and are listed in the
  summaries without a ranking.
- Rejected and repeated (1 core, closed loop): 18 runs in A, 33 in B. In B one
  cell (repetition 2, 1 core, io_uring, GET depth 16) has no accepted run after
  three repeats, so that median is over 6 repetitions. Session B also recorded
  which other processes ran on the server CPUs and their siblings: processes
  named `python` (the machine's training job runs as python; about 250 s of CPU
  time on those CPUs over the session) and a QEMU VM named `td-legacy-vm`
  (about 146 s). `python` was the largest bystander in 11 of the 33 rejected
  runs. Session A has no such record (added between the sessions; the protocol
  did not change).

## Interference probe (1 core, GET, depth 1; medians of 6)

| condition | io_uring | epoll | io_uring k cycles/req | epoll k cycles/req |
| --- | ---: | ---: | ---: | ---: |
| nothing added | 334 k | 308 k | 16.17 | 17.67 |
| spinners on three other P-cores (clock −3%) | 326 k | 302 k | 16.06 | 17.30 |
| a spinner on the server CPU's SMT sibling | 232 k | 220 k | 22.38 | 23.78 |

A busy sibling costs about 30%; the 1-core server never sleeps at depth 1
(0.0 voluntary context switches per 1,000 requests).

## io_uring variants (2 cores, GET; medians of 7)

| | depth 1 | depth 16 | CPU per request, depth 16 |
| --- | ---: | ---: | ---: |
| default: DEFER_TASKRUN, batched messages over MSG_RING | 599 k | 5.24 M | 0.377 µs |
| doorbell transport | 598 k | 4.78 M | 0.377 µs |
| doorbell, announcing sleep before submitting | 611 k | 3.48 M | 0.387 µs |
| COOP_TASKRUN | 617 k | 3.26 M | 0.379 µs |
| COOP_TASKRUN, doorbell | 634 k | 3.37 M | 0.384 µs |

## Cross-core study (xcore; two P-cores, medians of 7, ns per operation)

| | DEFER, one MSG_RING each | DEFER, batched | COOP, batched | doorbell |
| --- | ---: | ---: | ---: | ---: |
| one-way posts, receiver sleeps when idle | 192.9 | 54.1 | 47.4 | 52.4 |
| 32 concurrent callers, receiver sleeps | 357.2 | 188.2 | 199.9 | 219.9 |
| one caller, round trip, both cores spin | 932.9 | 907.9 | 743.9 | 294.2 |
| one caller, round trip, receiver sleeps | 2,863 | 2,844 | 2,945 | 3,033 |

Explanations in [design-tradeoffs.md](../docs/design-tradeoffs.md).

Round 3's test matrix, deliberate-bug and fuzz results (df7ae97: 134 cases, 10
of 10 mutations, 482,464,087 and 10,346,156 fuzz executions) are in the git
history of `results/test-matrix`, `results/deliberate-bugs` and
`results/fuzz`; those directories now hold the performance round's runs.

# Not verified

- Tail latencies on an idle machine: every run here shared the host with other
  jobs.
- Why the doorbell transport's cores idle more at depth 16 with the same work
  per request, and why COOP_TASKRUN stalls at depth 16 (round 3; no profiler
  or scheduler tracing on this host).
- Why a 20 µs spin before sleeping cut depth-16 throughput by a quarter or
  more on both transports (window 2).
- Cache behaviour: callgrind counts instructions, not cache misses, so the
  shard table's fewer memory reads per lookup follow from its layout and were
  not measured on the hardware.
- This branch has not run on the GitHub CI runners (Linux 6.8). It uses no
  io_uring feature that the published `main` did not already use.
