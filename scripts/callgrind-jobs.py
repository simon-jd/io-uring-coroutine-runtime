#!/usr/bin/env python3
"""The io_uring front end's own instructions per request, grouped by job.

Usage: scripts/callgrind-jobs.py <label>=<out-dir> [...] > table.md

Takes the same callgrind-profile.sh directories as callgrind-summary.py and
sums the exclusive counts of the runtime's and the front end's functions
(by name, first matching group wins) per completed request: submitting the
send, reaping and dispatching completions and resuming the coroutine,
acquiring and releasing operation slots, cross-core calls (both the calling
and the called core's share), and the connection coroutine itself (which
also holds every awaiter the compiler inlined into it).
"""
import json, re, subprocess, sys
from collections import Counter
CATS = [
 ("send submission", r"reactor::start<rol::reactor::send|reserve_sqes|owned_buffer::resize"),
 ("completions: reap, dispatch, resume", r"reactor::reap|reactor::dispatch|reactor::on_op_cqe|reactor::on_multishot|reactor::complete|reactor::step|recv_stream|provided_buffer"),
 ("slot acquire and release", r"free_slot|release_resources|finish_take|maybe_free|acquire_slot|slot_at"),
 ("cross-core calls", r"call_all_awaiter|launch_call|start_call|on_reply|on_request|on_message_chain|call_node|flush_target|flush_messages|enqueue_message|check_call_target|refuse_call"),
 ("front end (the connection coroutine)", r"kvs::"),
 ("other runtime", r"rol::"),
]
print("| | " + " | ".join(a.split("=")[0] for a in sys.argv[1:]) + " |")
print("| --- |" + " ---: |" * (len(sys.argv) - 1))
cols = []
for a in sys.argv[1:]:
    d = a.split("=", 1)[1]
    n = json.load(open(f"{d}/kvload.json"))["completed"]
    t = subprocess.run(["callgrind_annotate", "--threshold=100", "--inclusive=no", "--auto=no", f"{d}/callgrind.out.1"], capture_output=True, text=True).stdout
    c = Counter()
    for line in t.splitlines():
        m = re.match(r"\s*([\d,]+) \(\s*([\d.]+)%\)\s+(.*)", line)
        if not m or float(m.group(2)) >= 99.95 or "PROGRAM TOTALS" in m.group(3): continue
        name = re.sub(r"\s*\[[^\]]*\]$", "", m.group(3)); name = name.split(":", 1)[1] if ":" in name else name
        for cat, pat in CATS:
            if re.search(pat, name): c[cat] += int(m.group(1).replace(",", "")) / n; break
    cols.append(c)
for cat, _ in CATS:
    print(f"| {cat} | " + " | ".join(f"{c.get(cat, 0):.0f}" for c in cols) + " |")
print("| total | " + " | ".join(f"{sum(c.values()):.0f}" for c in cols) + " |")
