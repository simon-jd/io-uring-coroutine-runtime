# Window C: decision rules (written before the run)

Second session for `c3` (GET and SET hash their key once and carry the hash
to the owning core's table), at least an hour after window B. In window B,
`uring-c3 / uring-c2` was lower in 6 of 7 repetitions at 4 cores, SET depth
16 (0.991), and `epoll-c3 / epoll-main` in 7 of 7 at 4 cores, SET (0.947);
no cell was worse. Callgrind puts the change at 22 / 16 fewer instructions
per request at 2 / 4 cores (about 2%), so its effect on CPU time is
expected to be well under 3% and needs this second session.

`c2` is decided (passed in windows A and B) and `c4` is rejected (window B).

Arms (Release, separate source trees, as before): `uring-main`, `uring-c2`,
`uring-c3`, `epoll-main`, `epoll-c3`. 7 repetitions, rotating order; part
1 at 1 and 2 cores (GET depth 1, GET and SET depth 16, rejection rule at 1
core), part 2 at 4 cores (GET and SET depth 16, harness on CPU 11).

Rule: `c3` is kept if `uring-c3 / uring-c2` or `epoll-c3 / epoll-main` is
lower in at least 6 of 7 repetitions in at least one depth-16 cell, and no
cell of either is worse (median above 1.02 with at least 5 of 7 above 1).
Otherwise it is reverted and recorded with both sessions' numbers.
