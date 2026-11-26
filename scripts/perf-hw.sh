#!/usr/bin/env bash
# Hardware-counter study of the key-value server: the io_uring and epoll front
# ends under perf stat (cycles, instructions, IPC, LLC references and misses,
# L1D load misses, branch misses per request) and perf record (call graphs).
#
# Run as root:  sudo bash scripts/perf-hw.sh
# Root is not limited by kernel.perf_event_paranoid, so no sysctl or other
# global setting is changed. Server and load generator run as the repository's
# owner (setpriv); only perf runs as root. Every file written is chowned back
# to the repository's owner.
#
# The run detaches from the terminal (setsid) and keeps going if ssh drops;
# Ctrl-C only stops the log view. Abort with: sudo kill -- -<pid printed>.
#
# Before the run and before every repetition it waits for the machine to be
# quiet (server CPUs and their SMT siblings, client CPUs, whole machine,
# loadavg) and gives up with the reason after PERF_HW_MAX_WAIT seconds.
#
# Knobs (environment): PERF_HW_REPS (7), PERF_HW_DURATION (5 s per run, after
# 1 s warm-up), PERF_HW_SERVER_CPUS (2,4,6,0: P-cores, first N for N cores),
# PERF_HW_CLIENT_CPUS (16-23), PERF_HW_CORES (1,2,4), PERF_HW_MAX_WAIT (1800 s),
# PERF_HW_MAX_WAIT_REP (600 s), PERF_HW_DRYRUN=1 (no perf, no root: checks the
# harness itself).
#
# Output: results/perf-hw/<UTC stamp>/ (summary.md, summary.json, runs.jsonl,
# gate.jsonl, raw perf stat CSVs, perf report text, run.log);
# perf.data files stay in .cache/perf-hw/<UTC stamp>/.
set -euo pipefail

REPO=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)
OWNER=$(stat -c %U "$REPO")

if [[ $EUID -ne 0 && "${PERF_HW_DRYRUN:-0}" != 1 ]]; then
  echo "perf-hw: run as root: sudo bash $REPO/scripts/perf-hw.sh" >&2
  exit 1
fi

if [[ "${1:-}" != --child ]]; then
  STAMP=$(date -u +%Y%m%dT%H%M%SZ)
  [[ "${PERF_HW_DRYRUN:-0}" == 1 ]] && STAMP="dryrun-$STAMP"
  OUT=$REPO/results/perf-hw/$STAMP
  CACHE=$REPO/.cache/perf-hw/$STAMP
  mkdir -p "$OUT" "$CACHE"
  chown "$OWNER:" "$REPO/results/perf-hw" "$OUT" "$REPO/.cache/perf-hw" "$CACHE"
  export PERF_HW_STAMP=$STAMP PERF_HW_OUT=$OUT PERF_HW_CACHE=$CACHE PERF_HW_REPO=$REPO PERF_HW_OWNER=$OWNER
  setsid bash "$0" --child </dev/null >"$OUT/run.log" 2>&1 &
  pid=$!
  echo "perf-hw: running in the background as pid $pid (keeps going if ssh drops)"
  echo "perf-hw: log $OUT/run.log"
  echo "perf-hw: Ctrl-C only closes this view; to abort the run: sudo kill -- -$pid"
  tail -n +1 -f --pid="$pid" "$OUT/run.log" || true
  wait "$pid" 2>/dev/null || true
  echo "perf-hw: exit status $(cat "$OUT/exit-status" 2>/dev/null || echo unknown); summary $OUT/summary.md"
  exit 0
fi

cd "$REPO"
exec 9>"$REPO/.cache/perf-hw/lock"
if ! flock -n 9; then
  echo "perf-hw: another run holds $REPO/.cache/perf-hw/lock"
  echo 4 >"$PERF_HW_OUT/exit-status"
  exit 4
fi
finish() {
  local st=$?
  echo "$st" >"$PERF_HW_OUT/exit-status"
  chown -R "$PERF_HW_OWNER:" "$PERF_HW_OUT" "$PERF_HW_CACHE" "$REPO/.cache/perf-hw/lock" 2>/dev/null || true
}
trap finish EXIT

python3 - <<'PY'
import json
import os
import pwd
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(os.environ["PERF_HW_REPO"])
OUT = Path(os.environ["PERF_HW_OUT"])
CACHE = Path(os.environ["PERF_HW_CACHE"])
OWNER = os.environ["PERF_HW_OWNER"]
DRY = os.environ.get("PERF_HW_DRYRUN") == "1"
REPS = int(os.environ.get("PERF_HW_REPS", "7"))
DURATION = float(os.environ.get("PERF_HW_DURATION", "5"))
WARMUP = 1
SERVER_CPUS = os.environ.get("PERF_HW_SERVER_CPUS", "2,4,6,0").split(",")
CLIENT_SPEC = os.environ.get("PERF_HW_CLIENT_CPUS", "16-23")
CORES = [int(c) for c in os.environ.get("PERF_HW_CORES", "1,2,4").split(",")]
MAX_WAIT = float(os.environ.get("PERF_HW_MAX_WAIT", "1800"))
MAX_WAIT_REP = float(os.environ.get("PERF_HW_MAX_WAIT_REP", "600"))
# Quiet-machine gate, judged on a 10 s /proc/stat sample.
GATE = {"watch_mean": 0.15, "watch_max": 0.35, "client_mean": 0.35, "machine_mean": 0.30, "load1": 8.0}
# 1-core closed-loop runs whose interference (other work on the server CPU and
# its SMT sibling, as a fraction of the window) exceeds this are kept, marked
# rejected and repeated (the kv-bench rule).
MAX_INTERFERENCE, RETRIES = 0.05, 2
BUILD = REPO / "build-release"
KV, KVLOAD = BUILD / "kv_server", BUILD / "kvload"
KEYS, VALUE, THREADS = 100_000, 64, 8
ARMS = ["uring", "epoll"]
WORKLOADS = {1: [("get", 1), ("get", 16), ("set", 16)], 2: [("get", 16), ("set", 16)],
             4: [("get", 16), ("set", 16)]}
RECORDS = [(1, "get", 1), (4, "get", 16)]
PERF = "/usr/bin/perf"
EVENTS = [
    ("cycles", "cpu_core/cycles/"),
    ("instructions", "cpu_core/instructions/"),
    ("cycles_u", "cpu_core/cycles/u"),
    ("instructions_u", "cpu_core/instructions/u"),
    ("branches", "cpu_core/branches/"),
    ("branch_misses", "cpu_core/branch-misses/"),
    ("llc_refs", "cpu_core/cache-references/"),
    ("llc_misses", "cpu_core/cache-misses/"),
    ("l1d_load_misses", "cpu_core/L1-dcache-load-misses/"),
]
HZ = os.sysconf("SC_CLK_TCK")
SYS_CPU = Path("/sys/devices/system/cpu")
PENV = dict(os.environ, DEBUGINFOD_URLS="", PAGER="cat")

pw = pwd.getpwnam(OWNER)
AS_OWNER = [] if os.geteuid() != 0 else [
    "setpriv", f"--reuid={pw.pw_uid}", f"--regid={pw.pw_gid}", "--init-groups", "--"]
RAW = OUT / "raw-stat"
RAW.mkdir(exist_ok=True)
REC_OUT = OUT / "record"
REC_OUT.mkdir(exist_ok=True)


def log(msg):
    print(time.strftime("%H:%M:%SZ ", time.gmtime()) + msg, flush=True)


def utc():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def cpu_list(spec):
    out = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out += range(int(a), int(b) + 1)
        elif part:
            out.append(int(part))
    return out


def siblings(c):
    return cpu_list((SYS_CPU / f"cpu{c}" / "topology" / "thread_siblings_list").read_text().strip())


CLIENT = cpu_list(CLIENT_SPEC)
USED_SERVER = [int(c) for c in SERVER_CPUS[:max(CORES)]]
WATCH = sorted({s for c in USED_SERVER for s in siblings(c)})
CORE_PMU_CPUS = set(cpu_list(Path("/sys/bus/event_source/devices/cpu_core/cpus").read_text().strip()))
assert set(USED_SERVER) <= CORE_PMU_CPUS, f"server CPUs {USED_SERVER} must be P-cores {sorted(CORE_PMU_CPUS)}"
os.sched_setaffinity(0, {c for c in range(os.cpu_count()) if c not in WATCH and c not in CLIENT
                         and c not in CORE_PMU_CPUS} or {os.cpu_count() - 1})


def loadavg():
    return [float(x) for x in open("/proc/loadavg").read().split()[:3]]


def stat_snapshot():
    snap = {}
    for line in open("/proc/stat"):
        if line.startswith("cpu") and line[3].isdigit():
            f = line.split()
            v = [int(x) for x in f[1:9]]
            snap[int(f[0][3:])] = ((v[0] + v[1] + v[2] + v[5] + v[6] + v[7]) / HZ, sum(v) / HZ)
    return snap


def busy_frac(s0, s1):
    return {c: (s1[c][0] - s0[c][0]) / max(1e-9, s1[c][1] - s0[c][1]) for c in s1}


def task_cpu_ns(pid):
    total = 0
    for t in Path(f"/proc/{pid}/task").iterdir():
        try:
            total += int((t / "schedstat").read_text().split()[0])
        except (OSError, ValueError, IndexError):
            pass
    return total


def task_ticks(pid):
    ut = st = 0
    for t in Path(f"/proc/{pid}/task").iterdir():
        try:
            s = (t / "stat").read_text()
            f = s[s.rindex(")") + 2:].split()
            ut += int(f[11])
            st += int(f[12])
        except (OSError, ValueError, IndexError):
            pass
    return ut, st


def proc_ticks():
    out = {}
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                s = open(f"/proc/{d}/stat").read()
                i = s.rindex(")")
                f = s[i + 2:].split()
                out[int(d)] = (s[s.index("(") + 1:i], int(f[36]), int(f[11]) + int(f[12]))
            except (OSError, ValueError, IndexError):
                pass
    return out


class Bystanders(threading.Thread):
    """Every 0.5 s: which other processes used CPU time while last seen on the
    watched CPUs (approximate: charged to the CPU a process was on at the sample)."""

    def __init__(self, cpus, skip):
        super().__init__(daemon=True)
        self.cpus, self.skip, self.seen = set(cpus), set(skip), {}
        self.halt = threading.Event()

    def run(self):
        prev = proc_ticks()
        while not self.halt.wait(0.5):
            cur = proc_ticks()
            for pid, (comm, cpu, t) in cur.items():
                if pid in self.skip or cpu not in self.cpus or pid not in prev:
                    continue
                d = t - prev[pid][2]
                if d > 0:
                    self.seen[f"{comm}[{pid}]"] = self.seen.get(f"{comm}[{pid}]", 0) + d
            prev = cur

    def stop(self):
        self.halt.set()
        self.join()
        return {k: round(v / HZ, 2) for k, v in sorted(self.seen.items(), key=lambda kv: -kv[1])[:5]}


class Freq(threading.Thread):
    def __init__(self, cpus):
        super().__init__(daemon=True)
        self.cpus, self.v = cpus, {c: [] for c in cpus}
        self.halt = threading.Event()

    def run(self):
        while not self.halt.is_set():
            for c in self.cpus:
                try:
                    self.v[c].append(int((SYS_CPU / f"cpu{c}/cpufreq/scaling_cur_freq").read_text()) / 1000)
                except (OSError, ValueError):
                    pass
            self.halt.wait(0.1)

    def stop(self):
        self.halt.set()
        self.join()
        return {str(c): (round(statistics.median(v)) if v else None) for c, v in self.v.items()}


def jsonl(name, rec):
    with open(OUT / name, "a") as f:
        f.write(json.dumps(rec) + "\n")


def gate(label, max_wait):
    """Waits for a quiet machine. Returns (ok, record)."""
    t0 = time.time()
    while True:
        s0 = stat_snapshot()
        time.sleep(10)
        b = busy_frac(s0, stat_snapshot())
        la = loadavg()
        w = [b[c] for c in WATCH]
        cl = [b[c] for c in CLIENT]
        m = statistics.mean(b.values())
        reasons = []
        if statistics.mean(w) > GATE["watch_mean"]:
            reasons.append(f"server CPUs and siblings {WATCH} mean busy {statistics.mean(w):.0%} > {GATE['watch_mean']:.0%}")
        if max(w) > GATE["watch_max"]:
            c = max(WATCH, key=lambda c: b[c])
            reasons.append(f"CPU {c} busy {b[c]:.0%} > {GATE['watch_max']:.0%}")
        if statistics.mean(cl) > GATE["client_mean"]:
            reasons.append(f"client CPUs {CLIENT_SPEC} mean busy {statistics.mean(cl):.0%} > {GATE['client_mean']:.0%}")
        if m > GATE["machine_mean"]:
            reasons.append(f"whole machine mean busy {m:.0%} > {GATE['machine_mean']:.0%}")
        if la[0] > GATE["load1"]:
            reasons.append(f"loadavg {la[0]:.2f} > {GATE['load1']}")
        waited = round(time.time() - t0)
        rec = {"label": label, "time": utc(), "ok": not reasons, "reasons": reasons, "waited_s": waited,
               "loadavg": la, "machine_busy": round(m, 3), "busy": {str(c): round(v, 3) for c, v in b.items()}}
        jsonl("gate.jsonl", rec)
        if not reasons:
            log(f"gate {label}: quiet (load {la[0]:.2f}, CPUs {WATCH} mean {statistics.mean(w):.0%}, "
                f"client {statistics.mean(cl):.0%}, machine {m:.0%}; waited {waited} s)")
            return True, rec
        if DRY:
            log(f"gate {label}: busy ({'; '.join(reasons)}) - dry run, not enforced")
            return True, rec
        if time.time() - t0 >= max_wait:
            log(f"gate {label}: still busy after {waited} s: {'; '.join(reasons)}")
            return False, rec
        log(f"gate {label}: busy ({'; '.join(reasons)}); waiting 30 s ({waited}/{int(max_wait)} s)")
        time.sleep(30)


def ping(port, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1) as s:
                s.sendall(b"PING\r\n")
                if s.recv(64).startswith(b"+PONG"):
                    return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError(f"server on port {port} did not answer PING")


PORT = [17300]


def start_server(arm, cores):
    PORT[0] += 1
    port = PORT[0]
    cpus = ",".join(SERVER_CPUS[:cores])
    cmd = AS_OWNER + ["taskset", "-c", cpus, str(KV), "--io", arm, "--cores", str(cores),
                      "--cpus", cpus, "--port", str(port)]
    err = open(CACHE / f"server-{arm}-{port}.stderr", "w")
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err)
    try:
        ping(port)
    except Exception:
        p.kill()
        raise
    return p, port, cpus


def stop_server(p):
    p.send_signal(signal.SIGTERM)
    try:
        return p.wait(timeout=20)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        return "killed"


def kvload(mode, port, extra):
    cmd = AS_OWNER + ["taskset", "-c", CLIENT_SPEC, str(KVLOAD), mode, "--port", str(port),
                      "--threads", str(THREADS), "--cpus", CLIENT_SPEC, "--keys", str(KEYS),
                      "--value-size", str(VALUE)] + extra
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=300)
    if r.returncode != 0:
        raise RuntimeError(f"kvload failed ({r.returncode}): {r.stderr.strip()}")
    return json.loads(r.stdout.strip().splitlines()[-1])


def closed_args(op, depth, cores):
    return ["--pipeline", str(depth), "--duration", str(DURATION), "--warmup", str(WARMUP),
            "--conns", str(50 * cores), "--get-ratio", "1" if op == "get" else "0"]


def parse_stat(path):
    """perf stat -x, lines: value,unit,event,run time,percent running,..."""
    names = {e: k for k, e in EVENTS}
    lines = [l for l in path.read_text().splitlines() if l.strip() and not l.startswith("#")]
    vals, pct = {}, {}
    for i, l in enumerate(lines):
        f = l.split(",")
        if len(f) < 3:
            continue
        key = names.get(f[2].strip()) or (EVENTS[i][0] if len(lines) == len(EVENTS) else None)
        if key is None:
            continue
        try:
            vals[key] = float(f[0])
        except ValueError:
            vals[key] = None
        try:
            pct[key] = float(f[4])
        except (IndexError, ValueError):
            pct[key] = None
    return vals, pct


def derive(v, n, cpu_s):
    def per(k):
        return v[k] / n if v.get(k) is not None else None

    def div(a, b):
        return a / b if a is not None and b else None

    d = {f"{k}_req": per(k) for k, _ in EVENTS}
    ck = v["cycles"] - v["cycles_u"] if v.get("cycles") is not None and v.get("cycles_u") is not None else None
    ik = (v["instructions"] - v["instructions_u"]
          if v.get("instructions") is not None and v.get("instructions_u") is not None else None)
    d.update({
        "cycles_k_req": ck / n if ck is not None else None,
        "instructions_k_req": ik / n if ik is not None else None,
        "ipc": div(v.get("instructions"), v.get("cycles")),
        "ipc_u": div(v.get("instructions_u"), v.get("cycles_u")),
        "ipc_k": div(ik, ck),
        "llc_miss_ratio": div(v.get("llc_misses"), v.get("llc_refs")),
        "branch_miss_pct": (100 * div(v.get("branch_misses"), v.get("branches"))
                            if div(v.get("branch_misses"), v.get("branches")) is not None else None),
        "branch_mpki": (1000 * div(v.get("branch_misses"), v.get("instructions"))
                        if div(v.get("branch_misses"), v.get("instructions")) is not None else None),
        "llc_mpki": (1000 * div(v.get("llc_misses"), v.get("instructions"))
                     if div(v.get("llc_misses"), v.get("instructions")) is not None else None),
        "ghz": div(v.get("cycles"), cpu_s * 1e9),
    })
    return d


def measure(rep, arm, cores, p, port, cpus, op, depth):
    server = cpu_list(cpus)
    sibs = sorted({s for c in server for s in siblings(c)} - set(server))
    for attempt in range(RETRIES + 1):
        tag = f"r{rep}-{arm}-c{cores}-{op}_p{depth}-a{attempt}"
        statf = RAW / f"{tag}.csv"
        la0 = loadavg()
        s0 = stat_snapshot()
        c0 = task_cpu_ns(p.pid)
        t0 = task_ticks(p.pid)
        by = Bystanders(WATCH, {p.pid, os.getpid()})
        fq = Freq(server)
        by.start()
        fq.start()
        w0 = time.time()
        perf = None
        if not DRY:
            perf = subprocess.Popen([PERF, "stat", "-x", ",", "--no-big-num", "-o", str(statf), "-e",
                                     ",".join(e for _, e in EVENTS), "-p", str(p.pid)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=PENV)
            time.sleep(0.5)
        try:
            res = kvload("closed", port, closed_args(op, depth, cores))
        finally:
            if perf:
                time.sleep(0.1)
                perf.send_signal(signal.SIGINT)
                perf_err = perf.communicate(timeout=30)[1].decode(errors="replace").strip()
        wall = time.time() - w0
        c1 = task_cpu_ns(p.pid)
        t1 = task_ticks(p.pid)
        s1 = stat_snapshot()
        la1 = loadavg()
        freq = fq.stop()
        byst = by.stop()
        b = busy_frac(s0, s1)
        busy_s = {c: s1[c][0] - s0[c][0] for c in s1}
        cpu_s = (c1 - c0) / 1e9
        foreign = max(0.0, sum(busy_s[c] for c in server) - cpu_s)
        intf = (foreign + sum(busy_s[c] for c in sibs)) / wall
        n = max(1, res["completed"])
        rec = {"rep": rep, "arm": arm, "cores": cores, "workload": f"{op}_p{depth}", "attempt": attempt,
               "time": utc(), "server_cpus": cpus, "sibling_cpus": sibs, "client_cpus": CLIENT_SPEC,
               "rps": res["rps"], "completed": res["completed"], "errors": res["errors"],
               "server_cpu_s": round(cpu_s, 4), "cpu_us_req": cpu_s * 1e6 / n,
               "server_utime_s": (t1[0] - t0[0]) / HZ, "server_stime_s": (t1[1] - t0[1]) / HZ,
               "loadavg_before": la0, "loadavg_after": la1, "window_s": round(wall, 3),
               "interference_frac": round(intf, 4),
               "cpu_busy": {str(c): round(v, 3) for c, v in b.items()},
               "freq_mhz": freq, "other_processes_top5_cpu_s": round(sum(byst.values()), 2)}
        with open(CACHE / "bystanders.jsonl", "a") as f:  # process names stay out of results/
            f.write(json.dumps({"tag": tag, "bystanders_s": byst}) + "\n")
        if not DRY:
            if not statf.exists():
                raise RuntimeError(f"perf stat wrote nothing: {perf_err}")
            vals, pct = parse_stat(statf)
            rec["counters"] = vals
            rec["pct_running"] = pct
            rec.update(derive(vals, n, cpu_s))
        rejected = cores == 1 and intf > MAX_INTERFERENCE
        if rejected:
            rec["rejected"] = True
        jsonl("runs.jsonl", rec)
        log(f"rep {rep} {arm:<5} c{cores} {op}_p{depth:<2} rps={res['rps']:>12,.0f} "
            f"cpu_us/req={rec['cpu_us_req']:.3f} "
            + (f"cyc/req={rec['cycles_req']:,.0f} ins/req={rec['instructions_req']:,.0f} "
               f"IPC={rec['ipc']:.2f} " if rec.get("cycles_req") and rec.get("ipc") else "")
            + f"load={la0[0]:.2f} intf={intf:.3f}" + (" REJECTED" if rejected else ""))
        if not rejected:
            return


def run_rep(rep):
    for cores in CORES:
        order = ARMS[rep % 2:] + ARMS[:rep % 2]
        for arm in order:
            p, port, cpus = start_server(arm, cores)
            try:
                kvload("preload", port, [])
                for op, depth in WORKLOADS[cores]:
                    measure(rep, arm, cores, p, port, cpus, op, depth)
            finally:
                code = stop_server(p)
            if code != 0:
                log(f"WARNING: {arm} c{cores} server exited with {code}")


def record(arm, cores, op, depth):
    name = f"{arm}-c{cores}-{op}_p{depth}"
    data = CACHE / f"{name}.perf.data"
    p, port, cpus = start_server(arm, cores)
    la0 = loadavg()
    s0 = stat_snapshot()
    try:
        kvload("preload", port, [])
        perf = None
        if not DRY:
            perf = subprocess.Popen([PERF, "record", "-N", "-e", "cpu_core/cycles/", "-F", "999",
                                     "--call-graph", "dwarf,8192", "-o", str(data), "-p", str(p.pid)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=PENV)
            time.sleep(0.5)
        try:
            res = kvload("closed", port, closed_args(op, depth, cores))
        finally:
            if perf:
                time.sleep(0.1)
                perf.send_signal(signal.SIGINT)
                perr = perf.communicate(timeout=120)[1].decode(errors="replace").strip()
                (REC_OUT / f"{name}-record-stderr.txt").write_text(perr.replace(str(REPO), "<repo>") + "\n")
    finally:
        stop_server(p)
    b = busy_frac(s0, stat_snapshot())
    jsonl("record.jsonl", {"name": name, "time": utc(), "server_cpus": cpus, "rps": res["rps"],
                           "loadavg_before": la0, "loadavg_after": loadavg(),
                           "cpu_busy": {str(c): round(v, 3) for c, v in b.items()}})
    log(f"record {name}: rps={res['rps']:,.0f}")
    if DRY:
        return
    reports = {
        "flat": ["--no-children", "--sort", "dso,sym", "-g", "none", "--percent-limit", "0.3"],
        "dso": ["--no-children", "--sort", "dso", "-g", "none"],
        "callers": ["--children", "--sort", "sym", "-g", "graph,0.5,caller", "--percent-limit", "3"],
    }
    for kind, args in reports.items():
        t = time.time()
        r = subprocess.run([PERF, "report", "-i", str(data), "--stdio"] + args,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=1200, env=PENV)
        lines = r.stdout.splitlines()
        if len(lines) > 4000:
            lines = lines[:4000] + [f"... cut at 4000 of {len(r.stdout.splitlines())} lines"]
        (REC_OUT / f"{name}-{kind}.txt").write_text("\n".join(lines) + "\n")
        log(f"  report {name}-{kind}: {len(lines)} lines, {time.time() - t:.0f} s")


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def summarize(meta, aborted):
    runs = []
    if (OUT / "runs.jsonl").exists():
        runs = [json.loads(l) for l in open(OUT / "runs.jsonl")]
    ok = [r for r in runs if not r.get("rejected")]
    gates = [json.loads(l) for l in open(OUT / "gate.jsonl")] if (OUT / "gate.jsonl").exists() else []
    cols = [("rps", "req/s", "{:,.0f}"), ("cpu_us_req", "CPU µs/req", "{:.3f}"),
            ("cycles_req", "cycles/req", "{:,.0f}"), ("instructions_req", "instr/req", "{:,.0f}"),
            ("ipc", "IPC", "{:.2f}"), ("instructions_u_req", "user instr/req", "{:,.0f}"),
            ("instructions_k_req", "kernel instr/req", "{:,.0f}"), ("ipc_u", "user IPC", "{:.2f}"),
            ("ipc_k", "kernel IPC", "{:.2f}"), ("llc_refs_req", "LLC refs/req", "{:.2f}"),
            ("llc_misses_req", "LLC misses/req", "{:.3f}"), ("llc_miss_ratio", "LLC miss ratio", "{:.3f}"),
            ("l1d_load_misses_req", "L1D load misses/req", "{:.1f}"),
            ("branch_misses_req", "branch misses/req", "{:.2f}"), ("branch_miss_pct", "branch miss %", "{:.2f}"),
            ("ghz", "GHz", "{:.2f}")]
    fmt = lambda f, v: f.format(v) if v is not None else "n/a"
    configs = [(c, f"{op}_p{d}") for c in CORES for op, d in WORKLOADS[c]]
    table, ratios, js = [], [], {"meta": meta, "aborted": aborted, "medians": {}, "paired": {}}
    for cores, wl in configs:
        for arm in ARMS:
            rs = [r for r in ok if r["cores"] == cores and r["workload"] == wl and r["arm"] == arm]
            if not rs:
                continue
            m = {k: med([r.get(k) for r in rs]) for k, _, _ in cols}
            js["medians"][f"c{cores}-{wl}-{arm}"] = dict(m, n=len(rs))
            table.append(f"| {cores} | {wl} | {arm} | {len(rs)} | " +
                         " | ".join(fmt(f, m[k]) for k, _, f in cols) + " |")
        pr = {}
        for rep in range(REPS):
            a = [r for r in ok if r["rep"] == rep and r["cores"] == cores and r["workload"] == wl]
            u = [r for r in a if r["arm"] == "uring"]
            e = [r for r in a if r["arm"] == "epoll"]
            if u and e:
                for k in ("cpu_us_req", "cycles_req", "instructions_req", "ipc", "llc_misses_req",
                          "l1d_load_misses_req", "branch_misses_req", "rps"):
                    if u[-1].get(k) is not None and e[-1].get(k):
                        pr.setdefault(k, []).append(u[-1][k] / e[-1][k])
        if pr:
            js["paired"][f"c{cores}-{wl}"] = {k: {"median": med(v), "lower": sum(x < 1 for x in v), "n": len(v)}
                                             for k, v in pr.items()}
            ratios.append(f"| {cores} | {wl} | " + " | ".join(
                f"{med(pr[k]):.3f} ({sum(x < 1 for x in pr[k])}/{len(pr[k])})" if k in pr else "n/a"
                for k in ("cpu_us_req", "cycles_req", "instructions_req", "ipc", "llc_misses_req",
                          "l1d_load_misses_req", "branch_misses_req", "rps")) + " |")
    las = [x for r in runs for x in (r["loadavg_before"][0], r["loadavg_after"][0])]
    busy = {str(c): med([r["cpu_busy"][str(c)] for r in ok]) for c in range(os.cpu_count())} if ok else {}
    intf = [r["interference_frac"] for r in ok]
    pcts = [p for r in ok for p in (r.get("pct_running") or {}).values() if p is not None]
    byst = round(sum(r.get("other_processes_top5_cpu_s", 0) for r in ok), 2)
    js["conditions"] = {"loadavg_1min_range": [min(las), max(las)] if las else None, "cpu_busy_median": busy,
                        "interference_median": med(intf), "interference_max": max(intf) if intf else None,
                        "rejected": sum(1 for r in runs if r.get("rejected")), "runs": len(runs),
                        "min_pct_running": min(pcts) if pcts else None,
                        "other_processes_on_watched_cpus_s": byst}
    (OUT / "summary.json").write_text(json.dumps(js, indent=1) + "\n")
    L = [f"# Hardware counters: io_uring vs epoll front end ({meta['stamp']})", ""]
    if aborted:
        L += [f"**Stopped early: {aborted}**", ""]
    L += ["Per request = counter over the whole kvload run (1 s warm-up + "
          f"{DURATION:g} s) divided by requests completed in it; perf stat attached to the server's "
          "threads (user + kernel), cpu_core PMU only. Medians over accepted runs.", "",
          "| server cores | workload | arm | runs | " + " | ".join(h for _, h, _ in cols) + " |",
          "| ---: | --- | --- | ---: |" + " ---: |" * len(cols)] + table
    L += ["", "io_uring / epoll, paired by repetition: median ratio (repetitions where io_uring is lower / pairs)", "",
          "| cores | workload | CPU µs/req | cycles/req | instr/req | IPC | LLC misses/req | L1D misses/req | branch misses/req | req/s |",
          "| ---: | --- |" + " ---: |" * 8] + ratios
    c = js["conditions"]
    L += ["", "## Conditions", ""] + [f"- {k}: {v}" for k, v in meta.items()]
    L += [f"- loadavg (1 min) during runs: {c['loadavg_1min_range']}",
          f"- interference on server CPUs + SMT siblings: median {c['interference_median']}, max {c['interference_max']}; "
          f"rejected and repeated 1-core runs: {c['rejected']} of {c['runs']}",
          f"- lowest perf 'percent running' (multiplexing): {c['min_pct_running']}",
          f"- other processes' CPU time on CPUs {WATCH} during accepted runs (top 5 per run, summed; "
          f"names in .cache/perf-hw/<stamp>/bystanders.jsonl): {c['other_processes_on_watched_cpus_s']} s",
          f"- gate checks: {len(gates)} ({sum(1 for g in gates if not g['ok'])} busy)",
          "- median busy fraction per CPU during accepted runs:", "",
          "  " + " ".join(f"{k}:{v:.2f}" for k, v in busy.items() if v is not None)]
    (OUT / "summary.md").write_text("\n".join(L) + "\n")


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=30).stdout.strip()
    except subprocess.SubprocessError:
        return "unavailable"


meta = {
    "stamp": os.environ["PERF_HW_STAMP"], "start": utc(),
    "commit": sh(f"git -C '{REPO}' rev-parse --short HEAD") +
              (" (uncommitted changes)" if sh(f"git -C '{REPO}' status --porcelain --untracked-files=no") else ""),
    "kernel": os.uname().release, "perf": sh(f"{PERF} --version"),
    "cpu": sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-").strip(),
    "microcode": sh("grep -m1 microcode /proc/cpuinfo | cut -d: -f2-").strip(),
    "governor / no_turbo": sh("cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor "
                              "/sys/devices/system/cpu/intel_pstate/no_turbo | tr '\\n' ' '"),
    "perf_event_paranoid / kptr_restrict / nmi_watchdog (unchanged)": sh(
        "cat /proc/sys/kernel/perf_event_paranoid /proc/sys/kernel/kptr_restrict /proc/sys/kernel/nmi_watchdog | tr '\\n' ' '"),
    "build": "build-release (Release, -O3)", "kv_server sha256": sh(f"sha256sum {KV} | cut -c1-16"),
    "server CPUs": f"first N of {','.join(SERVER_CPUS)} (P-cores; SMT siblings {sorted(set(WATCH) - set(USED_SERVER))} kept idle by the gate)",
    "client CPUs": f"{CLIENT_SPEC} (E-cores), kvload {THREADS} threads, 50 connections per server core",
    "harness CPUs": str(sorted(os.sched_getaffinity(0))),
    "workloads": "closed loop, 100k keys preloaded, 64-byte values; " +
                 "; ".join(f"{c} core(s): {', '.join(f'{o}_p{d}' for o, d in WORKLOADS[c])}" for c in CORES),
    "reps": f"{REPS}, arm order rotated per repetition",
    "events": ",".join(e for _, e in EVENTS),
    "record": "perf record -e cpu_core/cycles/ -F 999 --call-graph dwarf,8192 on " +
              ", ".join(f"{c} core(s) {o}_p{d}" for c, o, d in RECORDS) + " for each arm",
    "gate": f"10 s sample; CPUs {WATCH} mean <= {GATE['watch_mean']:.0%} and each <= {GATE['watch_max']:.0%}, "
            f"client CPUs mean <= {GATE['client_mean']:.0%}, machine mean <= {GATE['machine_mean']:.0%}, "
            f"loadavg <= {GATE['load1']}; wait up to {MAX_WAIT:.0f} s at start, {MAX_WAIT_REP:.0f} s per repetition",
    "rejection": f"1-core runs with interference > {MAX_INTERFERENCE} repeated up to {RETRIES} times",
    "dry run": DRY,
}
log("perf-hw: " + json.dumps(meta))
aborted = None
try:
    if not DRY:
        chk = subprocess.run([PERF, "stat", "-x", ",", "-e", ",".join(e for _, e in EVENTS), "--",
                              "taskset", "-c", SERVER_CPUS[0], "true"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=PENV)
        log("event check: " + " | ".join(l for l in chk.stderr.splitlines() if l.strip()))
        core = [l for l in chk.stderr.splitlines()
                if l.endswith(",cpu_core/cycles/,") or "cpu_core/cycles/," in l or "cpu_core/instructions/," in l]
        if chk.returncode != 0 or len(core) < 2 or any("<not" in l for l in core):
            raise SystemExit("event check failed (cycles/instructions not counted): " + chk.stderr.strip())
    ok, _ = gate("start", MAX_WAIT if not DRY else 0)
    if not ok:
        aborted = "machine busy at start (see gate.jsonl and run.log)"
        raise SystemExit(3)
    for rep in range(REPS):
        if rep:
            ok, _ = gate(f"rep {rep}", MAX_WAIT_REP)
            if not ok:
                aborted = f"machine busy before repetition {rep}; repetitions 0-{rep - 1} kept"
                break
        run_rep(rep)
    if not aborted:
        for arm in ARMS:
            for cores, op, depth in RECORDS:
                ok, _ = gate(f"record {arm} c{cores}", MAX_WAIT_REP)
                if not ok:
                    aborted = "machine busy before a perf record run; counter results complete"
                    break
                record(arm, cores, op, depth)
            if aborted:
                break
finally:
    meta["end"] = utc()
    summarize(meta, aborted)
    log(f"summary written: {OUT.relative_to(REPO) / 'summary.md'}")
if aborted and "busy at start" in aborted:
    sys.exit(3)
PY
