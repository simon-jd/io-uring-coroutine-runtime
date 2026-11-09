# Window 1: decision rules (written before the run)

Arms, all built Release from separate source trees (`SOURCE` per tree in the
meta file), interleaved in rotating order, 7 repetitions, 1 and 2 server cores,
closed loop GET depth 1, GET depth 16, SET depth 16; harness rejection rule as
in round 3 (1-core runs with interference > 0.05 repeated up to 3 times):

- `uring-main`, `epoll-main`: the published baseline (`main`, 3eb4121).
- `uring-kv`, `epoll-kv`: main + one-pass count/length lines in the parser,
  fewer appends per reply, an open-addressing shard table, one key hash for
  core and slot (cand-kv).
- `uring-reactor`: main + chunked slot table with inline lookup, results
  queue as a vector, release only what a slot holds, per-thread pool for
  message nodes (cand-reactor).
- `uring-front`: main + replies assembled straight into the send buffer
  (cand-front).
- `uring-setup`: main + the ring descriptor registered with its thread
  (cand-setup).

Metric: server CPU per request, paired by repetition against the same front
end from `main`.

- kv: kept if CPU per request is lower than main in at least 6 of 7 paired
  repetitions in each depth-16 cell (1 core GET, 2 cores GET and SET) for both
  `uring-kv` and `epoll-kv`.
- reactor: kept if lower than main in at least 6 of 7 repetitions at 2 cores,
  GET depth 16 (the cross-core path, the headline loss).
- front: kept if lower than main in at least 6 of 7 repetitions in at least
  one depth-16 cell.
- setup: kept if lower than main in at least 6 of 7 repetitions in at least
  one cell.
- For every candidate, no cell may be worse: a cell whose median paired ratio
  is above 1.02 with at least 5 of 7 repetitions above 1 rejects it.
