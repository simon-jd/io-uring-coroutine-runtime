#!/usr/bin/env python3
"""Summarize the raw kv-bench JSON lines into results/kv-bench/summary.md.

Every cell is the median over repetitions, with the minimum and maximum in
brackets where the spread matters. Usage: scripts/kv-summary.py [results-dir] [A:B ...]
(each A:B adds a paired comparison of arm A against arm B to the build comparison).
"""
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load(path, keep_rejected=False):
    """Raw lines; runs rejected by the interference rule are left out unless asked for."""
    if not path.exists():
        return []
    rows = [json.loads(l) for l in open(path) if l.strip()]
    return rows if keep_rejected else [r for r in rows if not r.get("rejected")]


def server_mhz(r):
    """Median sampled clock of the run's server CPUs (MHz), if recorded."""
    f = [v for v in r.get("env", {}).get("freq_mhz", {}).values() if v]
    return statistics.mean(f) if f else None


def kcycles(r):
    mhz = server_mhz(r)
    return r["server_cpu_us_per_req"] * mhz / 1000 if mhz else None


def fmt_rate(x):
    return f"{x / 1e6:.2f} M" if x >= 1e6 else f"{x / 1e3:.0f} k"


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def closed_table(rows, arms, cores, workloads, out):
    out.append(f"| workload ({cores} core{'s' if cores > 1 else ''}) | " +
               " | ".join(arms) + " |")
    out.append("| --- |" + " ---: |" * len(arms))
    for w in workloads:
        cells = []
        for a in arms:
            rs = [r for r in rows if r["arm"] == a and r["cores"] == cores and r["workload"] == w]
            if not rs:
                cells.append("—")
                continue
            v = [r["rps"] for r in rs]
            cells.append(f"{fmt_rate(med(v))} [{fmt_rate(min(v))}–{fmt_rate(max(v))}]")
        out.append(f"| {w} | " + " | ".join(cells) + " |")
    out.append("")
    out.append(f"Server CPU per request, µs (median), {cores} core{'s' if cores > 1 else ''}:")
    out.append("")
    out.append("| workload | " + " | ".join(arms) + " |")
    out.append("| --- |" + " ---: |" * len(arms))
    for w in workloads:
        cells = []
        for a in arms:
            rs = [r for r in rows if r["arm"] == a and r["cores"] == cores and r["workload"] == w]
            cells.append(f"{med([r['server_cpu_us_per_req'] for r in rs]):.3f}" if rs else "—")
        out.append(f"| {w} | " + " | ".join(cells) + " |")
    out.append("")
    if any(kcycles(r) for r in rows):
        out.append(f"Server cycles per request, thousands (CPU time x sampled clock; median), "
                   f"{cores} core{'s' if cores > 1 else ''}:")
        out.append("")
        out.append("| workload | " + " | ".join(arms) + " |")
        out.append("| --- |" + " ---: |" * len(arms))
        for w in workloads:
            cells = []
            for a in arms:
                v = [kcycles(r) for r in rows
                     if r["arm"] == a and r["cores"] == cores and r["workload"] == w and kcycles(r)]
                cells.append(f"{med(v):.2f}" if v else "—")
            out.append(f"| {w} | " + " | ".join(cells) + " |")
        out.append("")


def open_table(rows, arms, cores, out):
    ws = sorted({r["workload"] for r in rows if r["cores"] == cores and r["workload"].startswith("open_")},
                key=lambda w: int(w[5:-1]))
    if not ws:
        return
    out.append(f"| rate ({cores} core{'s' if cores > 1 else ''}), p50 / p99 / p99.9 µs | " +
               " | ".join(arms) + " |")
    out.append("| --- |" + " ---: |" * len(arms))
    for w in ws:
        cells = []
        for a in arms:
            rs = [r for r in rows if r["arm"] == a and r["cores"] == cores and r["workload"] == w]
            if not rs:
                cells.append("—")
                continue
            cells.append(f"{med([r['p50_us'] for r in rs]):.0f} / {med([r['p99_us'] for r in rs]):.0f} / "
                         f"{med([r['p999_us'] for r in rs]):.0f}")
        out.append(f"| {w[5:]} req/s | " + " | ".join(cells) + " |")
    out.append("")
    out.append("p99 spread over repetitions (min–max, µs) and requests not answered within 2 s:")
    out.append("")
    out.append("| rate | " + " | ".join(arms) + " |")
    out.append("| --- |" + " ---: |" * len(arms))
    for w in ws:
        cells = []
        for a in arms:
            rs = [r for r in rows if r["arm"] == a and r["cores"] == cores and r["workload"] == w]
            if not rs:
                cells.append("—")
                continue
            p99 = [r["p99_us"] for r in rs]
            to = sum(r["timeouts"] for r in rs)
            cells.append(f"{min(p99):.0f}–{max(p99):.0f}" + (f", {to} timeouts" if to else ""))
        out.append(f"| {w[5:]} | " + " | ".join(cells) + " |")
    out.append("")
    out.append("Server CPU per request at each rate, µs (median):")
    out.append("")
    out.append("| rate | " + " | ".join(arms) + " |")
    out.append("| --- |" + " ---: |" * len(arms))
    for w in ws:
        cells = []
        for a in arms:
            rs = [r for r in rows if r["arm"] == a and r["cores"] == cores and r["workload"] == w]
            cells.append(f"{med([r['server_cpu_us_per_req'] for r in rs]):.2f}" if rs else "—")
        out.append(f"| {w[5:]} | " + " | ".join(cells) + " |")
    out.append("")


def session_facts(rows, out, rejected=()):
    la = [x for r in rows if "loadavg_before" in r for x in (r["loadavg_before"][0], r["loadavg_after"][0])]
    reps = sorted({r["rep"] for r in rows if "rep" in r})
    exits = [r for r in rows if r.get("event") == "server_exit"]
    bad = [r for r in exits if r["code"] != 0]
    util = [r["client_cpu_s"] / (r["wall_s"] * 8) for r in rows if r.get("mode") == "closed"]
    out.append(f"- repetitions: {len(reps)}; measurements: {sum(1 for r in rows if 'workload' in r)}")
    if la:
        out.append(f"- 1-minute load average during the runs: {min(la):.2f}–{max(la):.2f}")
    out.append(f"- server shutdowns: {len(exits)}, non-zero exits: {len(bad)}")
    if util:
        out.append(f"- closed-loop client CPU use (of 8 load-generator CPUs): median {med(util):.0%}, "
                   f"max {max(util):.0%}")
    env = [r["env"] for r in rows if "env" in r]
    if env:
        mhz = [server_mhz(r) for r in rows if "env" in r and server_mhz(r)]
        intf = [e["interference_frac"] for e in env]
        busy = [e["busy_other_pcores"] for e in env]
        out.append(f"- server CPU clock (median of 0.1 s samples per run): median {med(mhz):.0f} MHz, "
                   f"range {min(mhz):.0f}–{max(mhz):.0f}")
        out.append(f"- interference (other work on the server CPUs and their SMT siblings, share of "
                   f"the window): median {med(intf):.3f}, max {max(intf):.3f}")
        out.append(f"- other physical P-cores more than half busy: median {med(busy):.0f}, max {max(busy)}")
    if rejected:
        by_arm = defaultdict(int)
        for r in rejected:
            by_arm[r["arm"]] += 1
        out.append(f"- runs rejected by the interference rule and repeated: {len(rejected)} (" +
                   ", ".join(f"{a} {n}" for a, n in sorted(by_arm.items())) + ")")
    elif env:
        out.append("- runs rejected by the interference rule: 0")
    out.append("")


def paired_ratios(rows, a, b, out):
    """Per repetition, arm a's CPU per request over arm b's (same rep, same core
    count, run minutes apart); median and range over repetitions."""
    closed = [r for r in rows if r.get("mode") == "closed"]
    keys = sorted({(r["cores"], r["workload"]) for r in closed})
    if not keys:
        return
    out.append(f"Server CPU per request, {a} / {b}, paired by repetition (median [min–max]; "
               f"below 1 means {a} needs less):")
    out.append("")
    out.append("| cores | workload | ratio | repetitions where it is below 1 |")
    out.append("| ---: | --- | ---: | ---: |")
    for cores, w in keys:
        ratios = []
        for rep in sorted({r["rep"] for r in closed}):
            ra = [r for r in closed if r["arm"] == a and r["cores"] == cores and r["workload"] == w
                  and r["rep"] == rep]
            rb = [r for r in closed if r["arm"] == b and r["cores"] == cores and r["workload"] == w
                  and r["rep"] == rep]
            if ra and rb:
                ratios.append(ra[-1]["server_cpu_us_per_req"] / rb[-1]["server_cpu_us_per_req"])
        if ratios:
            out.append(f"| {cores} | {w} | {med(ratios):.3f} [{min(ratios):.3f}–{max(ratios):.3f}] | "
                       f"{sum(1 for x in ratios if x < 1)} of {len(ratios)} |")
    out.append("")


def paired_metric(rows, a, b, metric, lower_better, out):
    """Per repetition, arm a's metric over arm b's, for every closed-loop cell."""
    closed = [r for r in rows if r.get("mode") == "closed"]
    keys = sorted({(r["cores"], r["workload"]) for r in closed})
    word = "below" if lower_better else "above"
    lines = []
    for cores, w in keys:
        ratios = []
        for rep in sorted({r["rep"] for r in closed}):
            ra = [r for r in closed if r["arm"] == a and r["cores"] == cores and r["workload"] == w
                  and r["rep"] == rep]
            rb = [r for r in closed if r["arm"] == b and r["cores"] == cores and r["workload"] == w
                  and r["rep"] == rep]
            if ra and rb:
                get = metric if callable(metric) else (lambda r, k=metric: r[k])
                ratios.append(get(ra[-1]) / get(rb[-1]))
        if ratios:
            better = sum(1 for x in ratios if (x < 1 if lower_better else x > 1))
            lines.append(f"| {cores} | {w} | {med(ratios):.3f} [{min(ratios):.3f}–{max(ratios):.3f}] | "
                         f"{better} of {len(ratios)} |")
    if lines:
        out.append(f"| cores | workload | {a} / {b} | repetitions {word} 1 |")
        out.append("| ---: | --- | ---: | ---: |")
        out += lines
        out.append("")


def client_us(r):
    return r["client_cpu_s"] * 1e6 / max(1, r["completed"])


EXTRA_PAIRS = [tuple(a.split(":", 1)) for a in sys.argv[2:] if ":" in a]


def compare_section(rows, rejected, out):
    arms = []
    for r in rows:
        if "workload" in r and r["arm"] not in arms:
            arms.append(r["arm"])
    out += ["## Build comparison", ""]
    session_facts(rows, out, rejected)
    closed = [r for r in rows if r.get("mode") == "closed"]
    workloads = []
    for r in closed:
        if r["workload"] not in workloads:
            workloads.append(r["workload"])
    for c in sorted({r["cores"] for r in closed}):
        closed_table(closed, arms, c, workloads, out)
        out.append(f"Load-generator CPU per request, µs (median), {c} core{'s' if c > 1 else ''}; "
                   f"share of its 8 CPUs in brackets:")
        out.append("")
        out.append("| workload | " + " | ".join(arms) + " |")
        out.append("| --- |" + " ---: |" * len(arms))
        for w in workloads:
            cells = []
            for a in arms:
                rs = [r for r in closed if r["arm"] == a and r["cores"] == c and r["workload"] == w]
                cells.append(f"{med([client_us(r) for r in rs]):.3f} "
                             f"[{med([r['client_cpu_s'] / (r['wall_s'] * 8) for r in rs]):.0%}]" if rs else "—")
            out.append(f"| {w} | " + " | ".join(cells) + " |")
        out.append("")
    # Each arm against its front end's published baseline (<io>-main), and
    # each io_uring arm against the epoll arm built from the same tree.
    pairs = []
    for a in arms:
        io, _, tag = a.partition("-")
        if tag and tag != "main" and f"{io}-main" in arms:
            pairs.append((a, f"{io}-main"))
    for a in arms:
        io, _, tag = a.partition("-")
        if io == "uring" and f"epoll-{tag}" in arms:
            pairs.append((a, f"epoll-{tag}"))
    for a in arms:
        if a.endswith("-newload") and a[:-len("-newload")] in arms:
            pairs.append((a, a[:-len("-newload")]))
    for a in arms:
        io, _, tag = a.partition("-")
        if io == "uring" and tag and tag != "main" and "uring-" + tag.split("-")[0] in arms \
                and "uring-" + tag.split("-")[0] != a and not a.endswith("-newload"):
            pairs.append((a, "uring-" + tag.split("-")[0]))
    pairs += [p for p in EXTRA_PAIRS if p[0] in arms and p[1] in arms]
    seen = set()
    pairs = [p for p in pairs if not (p in seen or seen.add(p))]
    for a, b in pairs:
        out += [f"### {a} against {b}", "",
                "Server CPU per request, paired by repetition (median [min–max]):", ""]
        paired_metric(closed, a, b, "server_cpu_us_per_req", True, out)
        out += ["Requests per second, paired by repetition:", ""]
        paired_metric(closed, a, b, "rps", False, out)
        if a.endswith("-newload"):
            out += ["Load-generator CPU per request, paired by repetition:", ""]
            paired_metric(closed, a, b, client_us, True, out)


def main():
    d = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "results" / "kv-bench"
    out = ["# kv-bench summary", "",
           "Generated by `scripts/kv-summary.py` from the raw JSON lines in this directory.",
           "Medians over repetitions; [min–max] across repetitions.", ""]
    main_rows = load(d / "main.jsonl")
    if main_rows:
        arms = ["uring", "epoll", "asio", "valkey"]
        out += ["## Main comparison", ""]
        session_facts(main_rows, out,
                      [r for r in load(d / "main.jsonl", True) if r.get("rejected")])
        paired_ratios(main_rows, "uring", "epoll", out)
        out += ["### Closed loop (requests/s)", ""]
        for c in sorted({r["cores"] for r in main_rows if "cores" in r}):
            closed_table([r for r in main_rows if r.get("mode") == "closed"], arms, c,
                         ["get_p1", "get_p16", "set_p1", "set_p16"], out)
        out += ["### Open loop, 90% GET / 10% SET (latency from scheduled send time)", ""]
        for c in sorted({r["cores"] for r in main_rows if "cores" in r}):
            open_table([r for r in main_rows if r.get("mode") == "open"], arms, c, out)
    cmp_rows = load(d / "compare.jsonl")
    if cmp_rows:
        compare_section(cmp_rows, [r for r in load(d / "compare.jsonl", True) if r.get("rejected")], out)
    var_rows = load(d / "variants.jsonl")
    if var_rows:
        arms = []
        for r in var_rows:
            if "workload" in r and r["arm"] not in arms:
                arms.append(r["arm"])
        out += ["## io_uring variants (2 cores)", ""]
        session_facts(var_rows, out,
                      [r for r in load(d / "variants.jsonl", True) if r.get("rejected")])
        closed_table([r for r in var_rows if r.get("mode") == "closed"], arms, 2, ["get_p1", "get_p16"], out)
        open_table([r for r in var_rows if r.get("mode") == "open"], arms, 2, out)
    cc_rows = load(d / "crosscheck.jsonl")
    if cc_rows:
        arms = ["uring", "epoll", "asio", "valkey"]
        out += ["## Cross-check with valkey-benchmark (1 core, GET, 50 connections, 4 threads)", ""]
        session_facts(cc_rows, out)
        closed_table([r for r in cc_rows if r.get("mode") == "valkey-benchmark"], arms, 1,
                     ["vb_get_p1", "vb_get_p16"], out)
    probe = load(d / "probe.jsonl")
    if probe:
        out += ["## Interference probe (1 core, GET, depth 1)", "",
                "Each arm with nothing added, with a spinning process on the server CPU's SMT "
                "sibling, and with spinning processes on three other P-cores.", ""]
        session_facts(probe, out)
        arms = []
        for r in probe:
            if "workload" in r and r["arm"] not in arms:
                arms.append(r["arm"])
        out.append("| condition | arm | requests/s | CPU µs/request | clock MHz | k cycles/request | "
                   "interference |")
        out.append("| --- | --- | ---: | ---: | ---: | ---: | ---: |")
        for cond in ("none", "pcores", "sibling"):
            for a in arms:
                rs = [r for r in probe if r.get("condition") == cond and r["arm"] == a]
                if not rs:
                    continue
                v = [r["rps"] for r in rs]
                out.append(f"| {cond} | {a} | {fmt_rate(med(v))} [{fmt_rate(min(v))}–{fmt_rate(max(v))}] | "
                           f"{med([r['server_cpu_us_per_req'] for r in rs]):.3f} | "
                           f"{med([server_mhz(r) for r in rs]):.0f} | "
                           f"{med([kcycles(r) for r in rs]):.2f} | "
                           f"{med([r['env']['interference_frac'] for r in rs]):.3f} |")
        out.append("")
        clean = [r for r in probe if r.get("condition") == "none" and r["env"]["interference_frac"] <= 0.05]
        if clean:
            out.append("Runs without an added spinner, split by measured interference (share of the "
                       "window), medians:")
            out.append("")
            out.append("| arm | interference <= 0.05: n, requests/s, CPU µs/request | > 0.05: n, requests/s, "
                       "CPU µs/request |")
            out.append("| --- | --- | --- |")
            for a in arms:
                lo = [r for r in probe if r.get("condition") == "none" and r["arm"] == a
                      and r["env"]["interference_frac"] <= 0.05]
                hi = [r for r in probe if r.get("condition") == "none" and r["arm"] == a
                      and r["env"]["interference_frac"] > 0.05]
                cell = lambda rs: (f"{len(rs)}, {fmt_rate(med([r['rps'] for r in rs]))}, "
                                   f"{med([r['server_cpu_us_per_req'] for r in rs]):.3f}") if rs else "0"
                out.append(f"| {a} | {cell(lo)} | {cell(hi)} |")
            out.append("")
    diag = load(d / "diag.jsonl")
    if diag:
        out += ["## I/O counters per 1,000 requests (2 cores, GET, medians of repetitions)", ""]
        keys = defaultdict(list)
        for r in diag:
            keys[(r["arm"], r["workload"])].append(r)
        counters = []
        for rs in keys.values():
            for k in rs[0]["per_1k_requests"]:
                if k not in counters:
                    counters.append(k)
        out.append("| arm | workload | requests/s | " + " | ".join(counters) + " |")
        out.append("| --- | --- | ---: |" + " ---: |" * len(counters))
        for (arm, w), rs in keys.items():
            cells = []
            for k in counters:
                vals = [r["per_1k_requests"][k] for r in rs if k in r["per_1k_requests"]]
                cells.append(f"{med(vals):.1f}" if vals else "—")
            out.append(f"| {arm} | {w} | {fmt_rate(med([r['rps'] for r in rs]))} | " + " | ".join(cells) + " |")
        out.append("")
    (d / "summary.md").write_text("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
