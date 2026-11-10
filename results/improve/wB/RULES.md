# Window B: decision rules (written before the run)

Window A: `c2` (c1 plus the chunked slot table) passed against `main` at 4
cores (0.954 GET, 6 of 7; 0.964 SET, 7 of 7); `c1` alone did not pass (no
cell at 6 of 7; 1 core GET depth 16 at 1.061, 6 of 7 higher). c1 is
therefore kept only inside c2, and c2 gets a second session here. Two more
candidates on top of `c2`.

Callgrind, GET, depth 16, instructions per request (`.cache/imp/prof`;
1 / 2 / 4 cores): `main` 966 / 1,151 / 1,397, `c2` 944 / 1,105 / 1,256,
`c3` — / 1,083 / 1,242, `c4` 901 / 1,068 / 1,225; epoll of `main` 886 /
1,022 / 1,114.

- `c3` (cand-c3 2846ac9, c2 plus): GET and SET hash their key once, when
  routed, and carry the hash to the owning core's table. Both front ends
  share this code, so it should lower io_uring and epoll alike.
- `c4` (cand-c4 cdb889f, c3 plus): the receive path (multishot result
  queue, `recv_stream::pop`, the provided-buffer ring's bookkeeping) and the
  call-target check inline; the slot helpers always inlined.

Arms (Release, separate source trees): `uring-main` (published `main`
a48c76c), `uring-c2`, `uring-c3`, `uring-c4`, `epoll-main`, `epoll-c3`
(epoll front end of the c3 build, to see the hash change on its own front
end). Same protocol as window A: 7 repetitions, rotating order; part 1 at 1
and 2 cores (GET depth 1, GET and SET depth 16, rejection rule at 1 core),
part 2 at 4 cores (GET and SET depth 16, harness on CPU 11).

Server CPU per request, paired by repetition:

- `c3` is kept if `uring-c3 / uring-c2` or `epoll-c3 / epoll-main` is
  lower in at least 6 of 7 repetitions in at least one depth-16 cell, and
  no cell of either is worse (median above 1.02 with at least 5 of 7
  above 1).
- `c4` is kept if `uring-c4 / uring-c3` passes the same rule.
- `c2` against `uring-main`: the rule of window A, as its second session.
- Any kept effect under 3% (median ratio of the best cell above 0.97)
  needs a third session at least an hour later that passes the same rule.
