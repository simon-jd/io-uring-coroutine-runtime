#!/usr/bin/env python3
"""Key-value server benchmark: the io_uring front end against the epoll and Asio
front ends and Valkey, on one machine, in one session, arms interleaved.

For every repetition the arms run in a rotated order. Each arm: start the server
pinned to SERVER_CPUS (nice), preload the keys, run the closed-loop workloads and
the open-loop rates with kvload pinned to CLIENT_CPUS (nice), stop the server
with SIGTERM and require a clean exit. Every measurement records the load
average before and after, the server's CPU time (sum of
/proc/<pid>/task/*/schedstat over the run) and the client's CPU time.

Suites: main (the four servers, 1 and 2 cores), variants (io_uring options,
2 cores), crosscheck (valkey-benchmark, 1 core), diag (one run per arm with the
servers' own I/O counters, 2 cores), compare (only the arms given with --arm,
closed-loop workloads from --workloads: one build against another).

--arm NAME=IO[@BUILD][,ARG...] adds an arm whose kv_server comes from BUILD
(default --build), e.g. --arm uring-main=uring@.cache/baseline-main/build-release.
The load generator is --kvload, the same binary for every arm, unless an arm
names its own with the argument kvload=PATH (to compare load generators).
Server and load generator run at --nice (default 0: timing runs happen in
quiet windows, where a positive nice value let other jobs crowd the measured
CPUs).

Usage: scripts/kv-bench.py [--suite main|variants|crosscheck|diag|compare] [--reps 5]
                           [--out results/kv-bench] [--quick] [--arm SPEC ...]
"""
import argparse
import json
import os
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

KEYS = 100_000
VALUE = 64
CLIENT_THREADS = 8


def loadavg():
    return [float(x) for x in open("/proc/loadavg").read().split()[:3]]


# ---- machine state around each measurement (the interference record) ----

HZ = os.sysconf("SC_CLK_TCK")
SYS_CPU = Path("/sys/devices/system/cpu")


def cpu_list(spec):
    out = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out += list(range(int(a), int(b) + 1))
        elif part:
            out.append(int(part))
    return out


def siblings(cpu):
    return cpu_list((SYS_CPU / f"cpu{cpu}" / "topology" / "thread_siblings_list").read_text().strip())


def smt_cpus():
    """CPUs that have an SMT sibling (the P-cores on this machine)."""
    return [c for c in range(os.cpu_count()) if len(siblings(c)) > 1]


def stat_snapshot():
    """Per-CPU busy and total time in seconds from /proc/stat."""
    snap = {}
    for line in open("/proc/stat"):
        if not line.startswith("cpu") or not line[3].isdigit():
            continue
        f = line.split()
        v = [int(x) for x in f[1:9]]  # user nice system idle iowait irq softirq steal
        busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7]
        snap[int(f[0][3:])] = (busy / HZ, sum(v) / HZ)
    return snap


def task_counters(pid):
    """Sum over the process's threads: CPU time and run-queue wait (schedstat),
    context switches (status), user and system time (stat)."""
    c = {"cpu_ns": 0, "run_delay_ns": 0, "vcsw": 0, "ivcsw": 0, "utime": 0, "stime": 0}
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            ss = (task / "schedstat").read_text().split()
            c["cpu_ns"] += int(ss[0])
            c["run_delay_ns"] += int(ss[1])
            for line in (task / "status").read_text().splitlines():
                if line.startswith("voluntary_ctxt_switches"):
                    c["vcsw"] += int(line.split()[1])
                elif line.startswith("nonvoluntary_ctxt_switches"):
                    c["ivcsw"] += int(line.split()[1])
            st = (task / "stat").read_text()
            fields = st[st.rindex(")") + 2:].split()
            c["utime"] += int(fields[11])
            c["stime"] += int(fields[12])
        except (OSError, ValueError, IndexError):
            pass
    return c


class FreqSampler(threading.Thread):
    """Samples scaling_cur_freq (and cpuinfo_avg_freq where present) of some
    CPUs every `period` seconds while a measurement runs."""

    def __init__(self, cpus, period=0.1):
        super().__init__(daemon=True)
        self.cpus = cpus
        self.period = period
        self.halt = threading.Event()
        self.cur = {c: [] for c in cpus}
        self.avg = {c: [] for c in cpus}

    def run(self):
        while not self.halt.is_set():
            for c in self.cpus:
                d = SYS_CPU / f"cpu{c}" / "cpufreq"
                try:
                    self.cur[c].append(int((d / "scaling_cur_freq").read_text()) / 1000)
                    self.avg[c].append(int((d / "cpuinfo_avg_freq").read_text()) / 1000)
                except (OSError, ValueError):
                    pass
            self.halt.wait(self.period)

    def stop(self):
        self.halt.set()
        self.join()
        # Medians: the window starts and ends with the server idle while the
        # load generator starts and stops.
        med = lambda xs: round(statistics.median(xs), 1) if xs else None
        return ({str(c): med(v) for c, v in self.cur.items()},
                {str(c): (round(min(v), 1) if v else None) for c, v in self.cur.items()},
                {str(c): med(v) for c, v in self.avg.items()})


def proc_cpu_ticks():
    """{pid: (comm, cpu it last ran on, utime + stime ticks)} for every process."""
    out = {}
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            st = open(f"/proc/{d}/stat").read()
        except OSError:
            continue
        i = st.rindex(")")
        f = st[i + 2:].split()
        try:
            out[int(d)] = (st[st.index("(") + 1:i], int(f[36]), int(f[11]) + int(f[12]))
        except (ValueError, IndexError):
            pass
    return out


class Bystanders(threading.Thread):
    """Every 0.5 s, notes which other processes used CPU time while last seen
    on the watched CPUs (the server CPUs and their siblings). Attribution is
    approximate: a process is charged to the CPU it was on at the sample."""

    def __init__(self, cpus, skip_pids, period=0.5):
        super().__init__(daemon=True)
        self.cpus = set(cpus)
        self.skip = set(skip_pids)
        self.period = period
        self.halt = threading.Event()
        self.seen = {}

    def run(self):
        prev = proc_cpu_ticks()
        while not self.halt.wait(self.period):
            cur = proc_cpu_ticks()
            for pid, (comm, cpu, t) in cur.items():
                if pid in self.skip or cpu not in self.cpus or pid not in prev:
                    continue
                d = t - prev[pid][2]
                if d > 0:
                    key = f"{comm}[{pid}]"
                    self.seen[key] = self.seen.get(key, 0) + d
            prev = cur

    def stop(self):
        self.halt.set()
        self.join()
        top = sorted(self.seen.items(), key=lambda kv: -kv[1])[:4]
        return {k: round(v / HZ, 2) for k, v in top}


class EnvProbe:
    """Records, for one measurement window, what else ran on the server's CPUs
    and their SMT siblings, the server CPUs' clock, the server's context
    switches and run-queue wait, and the load on the other P-cores."""

    def __init__(self, pid, server_cpus, client_cpus):
        self.pid = pid
        self.server = server_cpus
        self.sibs = sorted({s for c in server_cpus for s in siblings(c)} - set(server_cpus))
        self.client = client_cpus
        smt = smt_cpus()
        self.other_p = [c for c in smt if c not in server_cpus and c not in self.sibs]

    def __enter__(self):
        self.t0 = time.time()
        self.s0 = stat_snapshot()
        self.c0 = task_counters(self.pid)
        self.freq = FreqSampler(self.server)
        self.freq.start()
        self.others = Bystanders(self.server + self.sibs, {self.pid, os.getpid()})
        self.others.start()
        return self

    def __exit__(self, *exc):
        self.cur, self.cur_min, self.avg = self.freq.stop()
        self.bystanders = self.others.stop()
        self.s1 = stat_snapshot()
        self.c1 = task_counters(self.pid)
        self.wall = time.time() - self.t0
        return False

    def record(self, requests, client_cpu_s):
        d = {k: self.c1[k] - self.c0[k] for k in self.c0}
        busy = {c: self.s1[c][0] - self.s0[c][0] for c in self.s1}
        server_cpu = d["cpu_ns"] / 1e9
        on_server = sum(busy[c] for c in self.server)
        foreign = max(0.0, on_server - server_cpu)
        sib = sum(busy[c] for c in self.sibs)
        per_core = {}
        for c in self.other_p:
            core = min(siblings(c))
            per_core[core] = max(per_core.get(core, 0.0), busy[c] / self.wall)
        other_busy = [busy[c] / self.wall for c in self.other_p]
        client_busy = sum(busy[c] for c in self.client)
        n = max(1, requests)
        return {
            "window_s": round(self.wall, 3),
            "freq_mhz": self.cur, "freq_min_mhz": self.cur_min, "avg_freq_mhz": self.avg,
            "server_cpus": self.server, "sibling_cpus": self.sibs,
            "server_cpus_busy_s": round(on_server, 3),
            "server_cpus_foreign_s": round(foreign, 3),
            "sibling_busy_s": round(sib, 3),
            "interference_s": round(foreign + sib, 3),
            "interference_frac": round((foreign + sib) / self.wall, 4),
            "other_pcpu_busy_frac": round(statistics.mean(other_busy), 4) if other_busy else None,
            "busy_other_pcores": sum(1 for v in per_core.values() if v > 0.5),
            "server_run_delay_s": round(d["run_delay_ns"] / 1e9, 4),
            "server_vcsw": d["vcsw"], "server_ivcsw": d["ivcsw"],
            "server_vcsw_per_1k": round(1000 * d["vcsw"] / n, 3),
            "server_ivcsw_per_1k": round(1000 * d["ivcsw"] / n, 3),
            "server_utime_s": round(d["utime"] / HZ, 2), "server_stime_s": round(d["stime"] / HZ, 2),
            "client_cpus_busy_s": round(client_busy, 3),
            "client_foreign_s": round(max(0.0, client_busy - client_cpu_s), 3),
            "bystanders_s": self.bystanders,
        }


class Perturb:
    """Spinning processes pinned to given CPUs, for the interference probe."""

    def __init__(self, cpus):
        self.cpus = cpus
        self.procs = []

    def __enter__(self):
        for c in self.cpus:
            self.procs.append(subprocess.Popen(
                ["taskset", "-c", str(c), "nice", sys.executable, "-c", "while True: pass"]))
        time.sleep(0.5)
        return self

    def __exit__(self, *exc):
        for p in self.procs:
            p.kill()
            p.wait()
        return False


def server_cpu_ns(pid):
    total = 0
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            total += int((task / "schedstat").read_text().split()[0])
        except (OSError, ValueError, IndexError):
            pass
    return total


def ping(port, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1) as s:
                s.sendall(b"PING\r\n")
                if s.recv(64).startswith(b"+PONG"):
                    return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError(f"server on port {port} did not answer PING")


class Bench:
    def __init__(self, args):
        self.args = args
        self.build = Path(args.build)
        self.valkey = Path(args.valkey)
        self.out = Path(args.out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.raw = open(self.out / f"{args.suite}.jsonl", "a")
        self.logs = Path(args.logs)
        self.logs.mkdir(parents=True, exist_ok=True)
        self.kvload_bin = Path(args.kvload) if args.kvload else self.build / "kvload"
        self.port = 17101
        self.write_meta()

    def write_meta(self):
        def sh(cmd):
            try:
                return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                      timeout=30).stdout.strip()
            except (OSError, subprocess.SubprocessError):
                return "unavailable"
        a = self.args
        asio_ver = "unknown"
        asio_hdr = ROOT / ".cache" / "asio-1-36-0" / "include" / "asio" / "version.hpp"
        if asio_hdr.exists():
            for line in asio_hdr.read_text().splitlines():
                if line.startswith("#define ASIO_VERSION "):
                    asio_ver = line.split("//")[-1].strip()
        dirty = " (uncommitted changes)" if sh(["git", "-C", str(ROOT), "status", "--porcelain",
                                                "--untracked-files=no"]) else ""
        lines = [
            f"date: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}",
            f"commit: {sh(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'])}{dirty}",
            f"kernel: {os.uname().release}",
            f"cpu: {sh(['sh', '-c', 'grep -m1 \"model name\" /proc/cpuinfo | cut -d: -f2-']).strip()}",
            f"compiler: {sh(['g++', '--version']).splitlines()[0]}",
            f"kv_server/kvload build: {a.build} (CMAKE_BUILD_TYPE=Release)",
            f"valkey: {sh([str(self.valkey / 'valkey-server'), '--version'])}",
            f"asio: {asio_ver}",
            f"suite: {a.suite}, reps {a.reps}, cores {a.cores}, measured window {a.duration} s "
            f"(closed loop, after 1 s warm-up; open loop {a.duration + 3} s after 2 s)",
            f"server cpus: first <cores> of {a.server_cpus}; client cpus: {a.client_cpus} "
            f"({CLIENT_THREADS} kvload threads); both under taskset at nice {a.nice}",
            f"kvload: {self.kvload_bin}",
            f"keys: {KEYS} preloaded, values {VALUE} bytes; connections: 50 per server core",
            f"loadavg at start: {' '.join(map(str, loadavg()))}",
            f"rejection rule: " + (f"interference fraction > {a.max_interference} in closed-loop runs on "
                                   f"<= {a.reject_max_cores} server core(s), up to {a.retries} repeats "
                                   f"per run" if a.max_interference is not None else "none"),
            f"harness cpu: {a.harness_cpu}; frequency sampled every 0.1 s (scaling_cur_freq, "
            f"cpuinfo_avg_freq)",
        ]
        for arm in a.arm_list:
            src = ""
            if arm.get("build"):
                f = Path(arm["build"]).parent / "SOURCE"
                if f.exists():
                    src = " (" + f.read_text().strip().splitlines()[0] + ")"
            lines.append(f"arm {arm['name']}: --io {arm['io']} {' '.join(arm.get('args', []))} "
                         f"from {arm.get('build') or a.build}{src}" +
                         (f", load generator {arm['kvload']}" if arm.get("kvload") else ""))
        (self.out / f"{a.suite}-meta.txt").write_text("\n".join(lines) + "\n")

    def niced(self, cmd):
        return (["nice", "-n", str(self.args.nice)] if self.args.nice else []) + cmd

    def server_cmd(self, arm, cores, cpus, port):
        if arm["kind"] == "valkey":
            log = self.logs / f"valkey-{port}.log"
            return [str(self.valkey / "valkey-server"), "--port", str(port), "--bind", "127.0.0.1",
                    "--save", "", "--appendonly", "no", "--io-threads", str(cores),
                    "--protected-mode", "no", "--server-cpulist", cpus, "--logfile", str(log)]
        build = Path(arm["build"]) if arm.get("build") else self.build
        return [str(build / "kv_server"), "--io", arm["io"], "--cores", str(cores),
                "--cpus", cpus, "--port", str(port)] + arm.get("args", [])

    def start(self, arm, cores, cpus, extra=()):
        self.port += 1
        port = self.port
        cmd = self.niced(["taskset", "-c", cpus] + self.server_cmd(arm, cores, cpus, port) + list(extra))
        self.last_stderr = self.logs / f"{arm['name']}-{port}.stderr"
        err = open(self.last_stderr, "w")
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err)
        ping(port)
        return p, port, cmd

    def run_diag(self, rep, arm, cores, op, pipe):
        cpus = self.cpus(cores)
        p, port, cmd = self.start(arm, cores, cpus, ["--stats"])
        try:
            self.kvload("preload", port, [])
            res, ccmd = self.kvload("closed", port, [
                "--pipeline", str(pipe), "--duration", str(self.args.duration), "--warmup", "1",
                "--conns", str(50 * cores), "--get-ratio", "1" if op == "get" else "0"])
        finally:
            code = self.stop(p)
        totals = {}
        for line in open(self.last_stderr):
            if not line.startswith("stats "):
                continue
            for kv in line.split()[1:]:
                k, v = kv.split("=")
                if k in ("core", "io"):
                    continue
                totals[k] = totals.get(k, 0) + int(v)
        requests = res["completed"] + KEYS  # the preload is counted too
        rec = {"rep": rep, "arm": arm["name"], "cores": cores, "workload": f"{op}_p{pipe}",
               "rps": res["rps"], "requests_counted": requests, "server_exit": code,
               "server_cmd": " ".join(cmd), "client_cmd": " ".join(ccmd[ccmd.index("taskset") + 3:]),
               "counters": totals,
               "per_1k_requests": {k: round(1000 * v / requests, 3) for k, v in totals.items()}}
        self.raw.write(json.dumps(rec) + "\n")
        self.raw.flush()
        print(f"  {arm['name']:<22} {op}_p{pipe:<4} rps={res['rps']:,.0f} " +
              " ".join(f"{k}={v}" for k, v in rec["per_1k_requests"].items()), flush=True)

    def stop(self, p):
        p.send_signal(signal.SIGTERM)
        try:
            return p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            return "killed"

    def kvload(self, mode, port, extra, binary=None):
        cmd = self.niced(["taskset", "-c", self.args.client_cpus, str(binary or self.kvload_bin), mode,
               "--port", str(port), "--threads", str(CLIENT_THREADS), "--cpus", self.args.client_cpus,
               "--keys", str(KEYS), "--value-size", str(VALUE)] + extra)
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=300)
        if r.returncode != 0:
            raise RuntimeError(f"kvload failed ({r.returncode}): {r.stderr.strip()}")
        return json.loads(r.stdout.strip().splitlines()[-1]), cmd

    def valkey_benchmark(self, port, pipeline, requests):
        cmd = self.niced(["taskset", "-c", self.args.client_cpus, str(self.valkey / "valkey-benchmark"),
                          "-h", "127.0.0.1", "-p", str(port), "-c", "50", "-n", str(requests),
                          "-P", str(pipeline), "-t", "get", "-r", str(KEYS), "-d", str(VALUE),
                          "--threads", "4", "--csv", "-q"])
        t0 = os.times()
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=300)
        t1 = os.times()
        lines = [l for l in r.stdout.splitlines() if l.startswith('"GET"')]
        if r.returncode != 0 or not lines:
            raise RuntimeError(f"valkey-benchmark failed: {r.stderr.strip()}")
        f = [x.strip('"') for x in lines[-1].split(",")]
        out = {"mode": "valkey-benchmark", "pipeline": pipeline, "rps": float(f[1]),
               "avg_ms": float(f[2]), "p50_ms": float(f[4]), "p99_ms": float(f[6]),
               "completed": requests,
               "client_cpu_s": (t1.children_user - t0.children_user) + (t1.children_system - t0.children_system)}
        return out, cmd

    def measure(self, rep, arm, cores, pid, workload, fn, extra_fields=None):
        """One measurement. With --max-interference, a closed-loop run on at
        most --reject-max-cores server cores whose interference (other work on
        the server CPUs and their SMT siblings, as a fraction of the window)
        exceeds the limit is kept in the raw file marked rejected and repeated,
        up to --retries times; the rule is the same for every arm. It is not
        applied with more server cores or to open-loop runs: there the server
        is partly idle, other jobs fill its siblings, and the tick-sampled
        /proc/stat times are too coarse to separate them."""
        for attempt in range(self.args.retries + 1):
            la0 = loadavg()
            c0 = server_cpu_ns(pid)
            w0 = time.time()
            with EnvProbe(pid, cpu_list(self.cpus(cores)), cpu_list(self.args.client_cpus)) as env:
                res, cmd = fn()
            wall = time.time() - w0
            c1 = server_cpu_ns(pid)
            la1 = loadavg()
            e = env.record(res["completed"], res.get("client_cpu_s", 0.0))
            rec = {"rep": rep, "arm": arm["name"], "cores": cores, "workload": workload,
                   "server_cpus": self.cpus(cores), "client_cpus": self.args.client_cpus,
                   "loadavg_before": la0, "loadavg_after": la1, "wall_s": round(wall, 3),
                   "server_cpu_s": round((c1 - c0) / 1e9, 4),
                   "server_cpu_us_per_req": round((c1 - c0) / 1e3 / max(1, res["completed"]), 4),
                   "client_cmd": " ".join(cmd[cmd.index("taskset") + 3:]), **res, "env": e,
                   "attempt": attempt}
            if extra_fields:
                rec.update(extra_fields)
            limit = self.args.max_interference
            applies = cores <= self.args.reject_max_cores and res.get("mode") == "closed"
            rejected = limit is not None and applies and e["interference_frac"] > limit
            if rejected:
                rec["rejected"] = True
            self.raw.write(json.dumps(rec) + "\n")
            self.raw.flush()
            key = res.get("rps") or res.get("p99_us")
            freq = "/".join(f"{v:.0f}" for v in e["freq_mhz"].values() if v)
            print(f"  rep {rep} {arm['name']:<22} cores={cores} {workload:<16} "
                  f"{'rps' if 'rps' in res else 'p99_us'}={key:,.1f} "
                  f"cpu_us/req={rec['server_cpu_us_per_req']:.3f} load={la0[0]:.2f} "
                  f"MHz={freq} intf={e['interference_frac']:.3f} "
                  f"vcsw/1k={e['server_vcsw_per_1k']:.1f}" + (" REJECTED" if rejected else ""),
                  flush=True)
            if not rejected:
                return

    def cpus(self, cores):
        return ",".join(self.args.server_cpus.split(",")[:cores])

    def run_arm(self, rep, arm, cores, workloads):
        cpus = self.cpus(cores)
        p, port, cmd = self.start(arm, cores, cpus)
        gen = arm.get("kvload")
        try:
            self.kvload("preload", port, [], gen)
            conns = str(50 * cores)
            for w in workloads:
                kind = w[0]
                if kind == "closed":
                    _, op, pipe = w
                    extra = ["--pipeline", str(pipe), "--duration", str(self.args.duration),
                             "--warmup", "1", "--conns", conns,
                             "--get-ratio", "1" if op == "get" else "0"]
                    self.measure(rep, arm, cores, p.pid, f"{op}_p{pipe}",
                                 lambda: self.kvload("closed", port, extra, gen))
                elif kind == "open":
                    _, rate = w
                    extra = ["--rate", str(rate * cores), "--duration", str(self.args.duration + 3),
                             "--warmup", "2", "--conns", conns, "--get-ratio", "0.9",
                             "--seed", str(rep + 1)]
                    self.measure(rep, arm, cores, p.pid, f"open_{rate * cores // 1000}k",
                                 lambda: self.kvload("open", port, extra, gen))
                elif kind == "vb":
                    _, pipe, n = w
                    self.measure(rep, arm, cores, p.pid, f"vb_get_p{pipe}",
                                 lambda: self.valkey_benchmark(port, pipe, n))
        finally:
            code = self.stop(p)
        self.raw.write(json.dumps({"rep": rep, "arm": arm["name"], "cores": cores,
                                   "event": "server_exit", "code": code,
                                   "server_cmd": " ".join(cmd)}) + "\n")
        self.raw.flush()
        if code != 0:
            print(f"  WARNING: {arm['name']} exited with {code}", flush=True)


def run_probe(b, args):
    """Interference probe: 1 server core, closed-loop GET at depth 1, each arm
    under three conditions: nothing added, a spinning process on the server
    CPU's SMT sibling, and spinning processes on three other P-cores."""
    server = cpu_list(b.cpus(1))[0]
    sib = [c for c in siblings(server) if c != server]
    others = [c for c in cpu_list(args.probe_pcores)]
    conditions = [("none", []), ("sibling", sib), ("pcores", others)]
    arms = [a for a in PROBE_ARMS if not args.arms or a["name"] in set(args.arms.split(","))]
    extra = ["--pipeline", "1", "--duration", str(args.duration), "--warmup", "1",
             "--conns", "50", "--get-ratio", "1"]
    for rep in range(args.reps):
        corder = conditions[rep % len(conditions):] + conditions[:rep % len(conditions)]
        for cond, cpus in corder:
            order = arms[rep % len(arms):] + arms[:rep % len(arms)]
            for arm in order:
                p, port, cmd = b.start(arm, 1, b.cpus(1))
                try:
                    b.kvload("preload", port, [])
                    with Perturb(cpus):
                        b.measure(rep, arm, 1, p.pid, "get_p1",
                                  lambda: b.kvload("closed", port, extra),
                                  {"condition": cond, "perturb_cpus": cpus})
                finally:
                    code = b.stop(p)
                b.raw.write(json.dumps({"rep": rep, "arm": arm["name"], "cores": 1,
                                        "event": "server_exit", "code": code,
                                        "server_cmd": " ".join(cmd)}) + "\n")
                b.raw.flush()


PROBE_ARMS = [
    {"name": "uring", "kind": "kv", "io": "uring"},
    {"name": "epoll", "kind": "kv", "io": "epoll"},
    {"name": "uring-busy-poll", "kind": "kv", "io": "uring", "args": ["--busy-poll"]},
]


MAIN_ARMS = [
    {"name": "uring", "kind": "kv", "io": "uring"},
    {"name": "epoll", "kind": "kv", "io": "epoll"},
    {"name": "asio", "kind": "kv", "io": "asio"},
    {"name": "valkey", "kind": "valkey"},
]

VARIANT_ARMS = [
    {"name": "uring", "kind": "kv", "io": "uring"},
    {"name": "uring-coop", "kind": "kv", "io": "uring", "args": ["--ring", "coop"]},
    {"name": "uring-unbatched", "kind": "kv", "io": "uring", "args": ["--no-batch"]},
    {"name": "uring-coop-unbatched", "kind": "kv", "io": "uring", "args": ["--ring", "coop", "--no-batch"]},
    {"name": "uring-fixed-send", "kind": "kv", "io": "uring", "args": ["--send", "fixed"]},
    {"name": "uring-direct", "kind": "kv", "io": "uring", "args": ["--direct"]},
    {"name": "uring-doorbell", "kind": "kv", "io": "uring", "args": ["--xcore", "doorbell"]},
    {"name": "uring-coop-doorbell", "kind": "kv", "io": "uring",
     "args": ["--ring", "coop", "--xcore", "doorbell"]},
    {"name": "uring-doorbell-early", "kind": "kv", "io": "uring",
     "args": ["--xcore", "doorbell-early"]},
]


def parse_arm(spec):
    """NAME=IO[@BUILD][,ARG...] -> arm dict."""
    name, rest = spec.split("=", 1)
    parts = rest.split(",")
    io, _, build = parts[0].partition("@")
    arm = {"name": name, "kind": "kv", "io": io, "args": [a for a in parts[1:] if not a.startswith("kvload=")]}
    for a in parts[1:]:
        if a.startswith("kvload="):
            p = Path(a[len("kvload="):])
            arm["kvload"] = str(p if p.is_absolute() else ROOT / p)
    if build:
        p = Path(build)
        arm["build"] = str(p if p.is_absolute() else ROOT / p)
    return arm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--suite", default="main",
                    choices=["main", "variants", "crosscheck", "diag", "probe", "compare"])
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--duration", type=float, default=5.0)
    ap.add_argument("--cores", default="1,2")
    ap.add_argument("--server-cpus", default="2,4,6,8")
    ap.add_argument("--client-cpus", default="16-23")
    ap.add_argument("--out", default=str(ROOT / "results" / "kv-bench"))
    ap.add_argument("--build", default=str(ROOT / "build-release"))
    ap.add_argument("--valkey", default=str(ROOT / ".cache" / "valkey-9.1.3" / "bin"))
    ap.add_argument("--logs", default=str(ROOT / ".cache" / "kv-bench-logs"))
    ap.add_argument("--arms", default="", help="comma-separated subset of arm names")
    ap.add_argument("--quick", action="store_true", help="1 rep, 2 s runs (smoke test)")
    ap.add_argument("--max-interference", type=float, default=None,
                    help="reject and repeat a run whose interference fraction exceeds this")
    ap.add_argument("--retries", type=int, default=0, help="repeats allowed per rejected run")
    ap.add_argument("--reject-max-cores", type=int, default=1,
                    help="the rejection rule applies to closed-loop runs on at most this many cores")
    ap.add_argument("--harness-cpu", type=int, default=9,
                    help="CPU for this script itself (not a server or client CPU)")
    ap.add_argument("--arm", action="append", default=[],
                    help="extra arm NAME=IO[@BUILD][,ARG...] (repeatable)")
    ap.add_argument("--workloads", default="get_p1,get_p16,set_p16",
                    help="compare suite: closed-loop workloads <get|set>_p<depth>")
    ap.add_argument("--kvload", default="", help="load generator binary (default: <build>/kvload)")
    ap.add_argument("--nice", type=int, default=0, help="nice value for server and load generator")
    ap.add_argument("--probe-pcores", default="4,6,8",
                    help="probe suite: CPUs that get a spinning process in the 'pcores' condition")
    args = ap.parse_args()
    if args.quick:
        args.reps, args.duration = 1, 2.0
    args.arm_list = [parse_arm(spec) for spec in args.arm]
    if args.harness_cpu >= 0:
        os.sched_setaffinity(0, {args.harness_cpu})
    b = Bench(args)
    if args.suite == "probe":
        run_probe(b, args)
        return 0
    cores_list = [int(c) for c in args.cores.split(",")]
    if args.suite == "compare":
        arms = list(args.arm_list)
        workloads = []
        for w in args.workloads.split(","):
            op, depth = w.split("_p")
            workloads.append(("closed", op, int(depth)))
        if args.arms:
            keep = set(args.arms.split(","))
            arms = [a for a in arms if a["name"] in keep]
        for rep in range(args.reps):
            for cores in cores_list:
                order = arms[rep % len(arms):] + arms[:rep % len(arms)]
                for arm in order:
                    b.run_arm(rep, arm, cores, workloads)
        return 0
    if args.suite == "main":
        arms = MAIN_ARMS + args.arm_list
        workloads = [("closed", "get", 1), ("closed", "get", 16), ("closed", "set", 1),
                     ("closed", "set", 16), ("open", 50_000), ("open", 100_000), ("open", 200_000)]
    elif args.suite == "variants":
        arms = VARIANT_ARMS
        cores_list = [c for c in cores_list if c > 1]
        workloads = [("closed", "get", 1), ("closed", "get", 16), ("open", 100_000)]
    elif args.suite == "crosscheck":
        arms = MAIN_ARMS
        cores_list = [1]
        workloads = [("vb", 1, 1_500_000), ("vb", 16, 10_000_000)]
    else:
        arms = [a for a in VARIANT_ARMS if a["name"] in
                ("uring", "uring-coop", "uring-unbatched", "uring-coop-unbatched",
                 "uring-doorbell", "uring-coop-doorbell", "uring-doorbell-early")] + \
               [a for a in MAIN_ARMS if a["name"] == "epoll"]
        if args.arms:
            keep = set(args.arms.split(","))
            arms = [a for a in arms if a["name"] in keep]
        for rep in range(args.reps):
            for cores in [c for c in cores_list if c > 1]:
                for arm in arms:
                    for pipe in (1, 16):
                        b.run_diag(rep, arm, cores, "get", pipe)
        return 0
    if args.arms:
        keep = set(args.arms.split(","))
        arms = [a for a in arms if a["name"] in keep]
    for rep in range(args.reps):
        for cores in cores_list:
            order = arms[rep % len(arms):] + arms[:rep % len(arms)]
            for arm in order:
                b.run_arm(rep, arm, cores, workloads)
    return 0


if __name__ == "__main__":
    sys.exit(main())
