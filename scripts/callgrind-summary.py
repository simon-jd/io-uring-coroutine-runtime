#!/usr/bin/env python3
"""Instructions per request by module from callgrind-profile.sh outputs.

Usage: scripts/callgrind-summary.py <label>=<out-dir> [...] > table.md

Each function's exclusive instruction count (callgrind_annotate, no inlining
attribution beyond what the compiler left) is divided by the requests kvload
completed while the counters ran, and summed into modules by name.
"""
import json
import re
import subprocess
import sys
from collections import Counter

MODULES = [
    ("RESP parser", r"kv::parser::|kv::parse_ll|find_crlf|short_number|kv::consume|__memchr"),
    ("reply encoders", r"kv::reply_"),
    ("std::string appends (replies, batch arguments)", r"_M_append|_M_replace|basic_string.*append|_M_mutate"),
    ("engine: plan, batch, shard, hashing",
     r"kv::plan::|kv::batch::|kv::shard::|kv::owner|kv::key_hash|classify|_Hash_bytes|_Hashtable|"
     r"__memcmp|kv::\(anonymous namespace\)::ieq"),
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
    for line in text.splitlines():
        m = re.match(r"\s*([\d,]+) \(\s*[\d.]+%\)\s+(.*)", line)
        if not m or "PROGRAM TOTALS" in m.group(2):
            continue
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
