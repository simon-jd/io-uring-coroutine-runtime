#!/usr/bin/env python3
"""Instructions per request by module from callgrind-profile.sh outputs.

Usage: scripts/callgrind-summary.py <label>=<out-dir> [...] > table.md

Each function's exclusive instruction count (callgrind_annotate; inlined code
counts for the function it was inlined into) is divided by the requests kvload
completed while the counters ran, and summed into modules by name. The thread
entry frames (clone, start_thread, the runtime's core loop entry) are listed by
callgrind_annotate with the whole dump's cost because they were active when the
counters were zeroed and dumped; entries at 100% are left out, and the module
sum is checked against PROGRAM TOTALS.
"""
import json
import re
import subprocess
import sys
from collections import Counter

MODULES = [
    ("RESP parser", r"kv::parser::|kv::parse_ll|find_crlf|short_number|kv::consume|__memchr"),
    ("reply encoders", r"kv::reply_"),
    ("engine: plan, batch, shard, hashing",
     r"kv::plan::|kv::batch::|kv::shard::|kv::owner|kv::key_hash|classify|_Hash_bytes|_Hashtable|"
     r"__memcmp|kv::\(anonymous namespace\)::ieq|_M_append|_M_replace|_M_mutate"),
    ("byte copies (memcpy/memmove)", r"__memcpy|__memmove|memmove"),
    ("allocator", r"^malloc$|^free$|_int_malloc|_int_free|operator new|operator delete|"
                  r"cfree|tcache|node_pool"),
    ("runtime (reactor, slots, cross-core)", r"rol::"),
    ("front end", r"kvs::"),
]


def load(d):
    completed = json.load(open(f"{d}/kvload.json"))["completed"]
    text = subprocess.run(["callgrind_annotate", "--threshold=100", "--inclusive=no", "--auto=no",
                           f"{d}/callgrind.out.1"], capture_output=True, text=True, check=True).stdout
    per = Counter()
    total = None
    for line in text.splitlines():
        m = re.match(r"\s*([\d,]+) \(\s*([\d.]+)%\)\s+(.*)", line)
        if not m:
            continue
        if "PROGRAM TOTALS" in m.group(3):
            total = int(m.group(1).replace(",", "")) / completed
            continue
        if float(m.group(2)) >= 99.95:
            continue
        m = re.match(r"\s*([\d,]+) \(\s*[\d.]+%\)\s+(.*)", line)
        name = re.sub(r"\s*\[[^\]]*\]$", "", m.group(2))
        name = name.split(":", 1)[1] if ":" in name else name
        per[name] += int(m.group(1).replace(",", "")) / completed
    out = Counter()
    for name, v in per.items():
        for mod, pat in MODULES:
            if re.search(pat, name):
                out[mod] += v
                break
        else:
            out["other (libc, kernel entry stubs, threads)"] += v
    if total is None or abs(sum(out.values()) - total) > 0.01 * total:
        sys.exit(f"{d}: module sum {sum(out.values()):.0f} does not match PROGRAM TOTALS {total}")
    return out, completed


def main():
    cols = []
    for arg in sys.argv[1:]:
        label, d = arg.split("=", 1)
        cols.append((label, *load(d)))
    mods = [m for m, _ in MODULES] + ["other (libc, kernel entry stubs, threads)"]
    print("| module | " + " | ".join(c[0] for c in cols) + " |")
    print("| --- |" + " ---: |" * len(cols))
    for m in mods:
        print(f"| {m} | " + " | ".join(f"{c[1].get(m, 0):.0f}" for c in cols) + " |")
    print("| **total** | " + " | ".join(f"**{sum(c[1].values()):.0f}**" for c in cols) + " |")
    print("| requests counted | " + " | ".join(f"{c[2]:,}" for c in cols) + " |")


if __name__ == "__main__":
    main()
