# Window A: decision rules (written before the run)

Question: does the io_uring front end need less CPU per request once the
runtime's per-read bookkeeping is lighter? Callgrind (GET, depth 16,
instructions per request, `.cache/imp/prof` on the build machine): `main`
1,151 / 1,397 at 2 / 4 cores, epoll 1,022 / 1,114.

- `c1` (cand-c1 5e954d4): call nodes built inside their operation slots,
  call_all without per-call allocations: 1,139 / 1,321.
- `c2` (cand-c2 f020b31, c1 plus): operation slots in fixed chunks, inline
  slot bookkeeping, release only what a slot holds: 1,105 / 1,256 (1 core:
  944 against main's 966 and epoll's 886).

Arms (Release, separate source trees, `.cache/imp/<name>/build-release`):
`uring-main` (published `main` a48c76c), `uring-c1`, `uring-c2`,
`epoll-main` (epoll front end of the `main` build). One load generator
(main's kvload) for all. 7 repetitions, arm order rotating. Part 1: 1 and 2
server cores, closed-loop GET depth 1, GET and SET depth 16, rejection rule
at 1 core (interference > 0.05, up to 3 repeats). Part 2: 4 server cores
(CPUs 2,4,6,8, harness on CPU 11), GET and SET depth 16, no rejection rule.

Server CPU per request, paired by repetition against `uring-main`:

- A candidate passes if it is lower in at least 6 of 7 repetitions in at
  least one depth-16 cell, and no cell is worse (a median ratio above 1.02
  with at least 5 of 7 repetitions above 1 fails it).
- The step from c1 to c2 is judged the same way on `uring-c2 / uring-c1`.
- A passing change whose median ratio in its best cell is above 0.97
  (an effect under 3%) is kept only if a second session at least an hour
  later passes the same rule.
- `uring / epoll-main` is reported for every cell; it decides nothing here.
