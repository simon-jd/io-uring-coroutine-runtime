# Window 3: decision rules (written before the run)

Window 2 showed the largest io_uring/epoll gap at 4 cores, depth 16 (CPU per
request 1.23 / 1.21 for GET / SET, paired). With more than two cores a read
sends batches to several cores; the front end awaited them with `when_all`
over one `forward_one` coroutine per target (two coroutine frames per target
plus four vectors per read). The candidate starts every call from one
awaiter, `reactor::call_all`, with a join counter in the call slots.

Arms (Release, separate trees): `uring-p1` (perf at 3796efd), `uring-callall`
(perf + call_all, cand-callall), `epoll-p1` (perf). 7 repetitions, rotating
order, 2, 3 and 4 server cores (CPUs 2,4 / 2,4,6 / 2,4,6,8), closed loop GET
and SET at depth 16. 2 cores is a control: one target per read uses the
unchanged single-call path.

Rule: call_all is kept if server CPU per request is lower than `uring-p1` in
at least 6 of 7 paired repetitions in at least one 3- or 4-core cell, and no
cell (the 2-core controls included) is worse: a median paired ratio above
1.02 with at least 5 of 7 repetitions above 1 rejects it.
