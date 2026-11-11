# Check D: rules for judging the claims (written 2026-11-10 15:44 PST, before session 2)

- Sessions: the build-comparison suite, 7 repetitions, arms uring-main, uring-imp, epoll-imp and
  epoll-main, with main's kvload. Part 1 is 1 and 2 cores, part 2 is 4 cores.
- A part (1-2 cores, or 4 cores) of a session is unusable if its median interference is above 0.5
  of the window. An unusable part is rerun, alone, in a further quiet window at least an hour after the
  previous session. Session 1's 4-core part (median 1.13; the 1-2 core part 0.019) is unusable by
  this rule, so a session 3 with the 4-core part only follows session 2.
- A comparison claim holds in a session if its median ratio is on the claimed side of 1 and within the
  claimed range, or within 1 point of it. A claim of "less" or "more" also needs at least 5 of 7
  repetitions on that side.
- A claim stays in the docs only if it holds in both usable check-D sessions and in the round's own
  two final sessions (imp-session-1 and imp-session-2). Claims about the decision windows or callgrind
  are checked against their files only; callgrind is rerun on fresh builds.
