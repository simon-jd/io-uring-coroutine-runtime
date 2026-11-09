# Window 2: decision rules (written before the run)

Base: `perf` at 3796efd (main + the window-1 engine change), built Release
from a separate tree. 7 repetitions, arms interleaved in rotating order, 2 and
4 server cores (CPUs 2,4 and 2,4,6,8), closed loop GET depth 1, GET depth 16,
SET depth 16. No rejection rule (it applies to 1 core only).

- `uring-p1`: perf, defaults (MSG_RING transport).
- `uring-p1-spin`: perf + optional spin before sleeping (cand-spin), `--spin-us 20`.
- `uring-p1-doorbell`: perf, `--xcore doorbell`.
- `uring-p1-dbspin`: cand-spin, `--xcore doorbell --spin-us 20`.
- `uring-p1-newload`: perf server, load generator from cand-kvload (replies
  parsed where they were received).
- `epoll-p1`: perf, epoll front end.
- `uring-main`: the published baseline (main, 3eb4121).

Rules (2-core cells decide; 4-core cells are descriptive):
- Spin on the MSG_RING transport becomes the default if depth-16 GET and SET
  throughput is higher than `uring-p1` in at least 6 of 7 paired repetitions
  and depth-1 CPU per request is not higher by more than 2% (median paired
  ratio <= 1.02). It stays as an option if it wins depth-16 throughput in at
  least 6 of 7 repetitions in one of the two cells. Otherwise it is removed.
- Doorbell with spin becomes the default transport only if its median
  depth-16 throughput (GET and SET) is higher than `uring-p1`'s and its median
  depth-1 throughput is at most 2% lower (the round-3 rule). It stays as an
  option if its depth-16 throughput beats `uring-p1-doorbell` in at least 6 of
  7 repetitions in one cell; otherwise the spin is removed.
- The new load generator is kept if its CPU per request is lower than the
  old one's in at least 6 of 7 paired repetitions in every depth-16 cell at 2
  cores, and the server's median paired requests/s ratio stays within
  0.97-1.03 in every 2-core cell.
- 4 cores: a cell is reported as a server comparison only if the load
  generator's median CPU share is below 75% for every arm in it.
