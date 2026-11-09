#!/usr/bin/env python3
"""Break the ownership contract on purpose and check that a named test assertion fails.

Each mutation is applied to a copy of the source tree, built, and the one test
case that guards it is run. A mutation counts as caught only if the test exits
non-zero AND prints the expected assertion name. Build errors, crashes without
the assertion, and timeouts count as not caught.

Usage: scripts/deliberate-bugs.py [--out results/deliberate-bugs] [--work .cache/bugs]
"""
import argparse
import difflib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

MUTATIONS = [
    {
        "name": "a-release-at-cancel-ack",
        "what": "Release the operation's buffer when the ASYNC_CANCEL acknowledgement "
                "arrives instead of at the operation's own CQE.",
        "file": "src/reactor.cpp",
        "old": "    cancel_source* reporter = cs.reporter;\n    --cs.pending;\n",
        "new": "    cancel_source* reporter = cs.reporter;\n"
               "    if (res == 0 && slots_[cs.target.index].active &&\n"
               "        slots_[cs.target.index].gen == cs.target.gen)\n"
               "        slots_[cs.target.index].lease.reset();\n"
               "    --cs.pending;\n",
        "suite": "reactor",
        "case": "cancel_ack_does_not_release_buffer",
        "expect": "assertion 'buffer stays with the operation after cancel-ack' failed",
    },
    {
        "name": "b-zc-reuse-at-first-cqe",
        "what": "Give a SEND_ZC buffer back at the first CQE without waiting for the "
                "notification CQE.",
        "file": "src/reactor.cpp",
        "old": "        // until the notification CQE.\n        ++s.pending;\n",
        "new": "        // until the notification CQE.\n        ++s.pending;\n"
               "        s.lease.reset();\n",
        "suite": "buffer",
        "case": "send_zc_buffer_held_until_notification",
        "expect": "assertion 'zero-copy payload arrives unmodified' failed",
    },
    {
        "name": "c-release-on-coroutine-destroy",
        "what": "Release the operation's buffer when the awaiting coroutine is destroyed "
                "(detach) instead of at the operation's CQE.",
        "file": "src/reactor.cpp",
        "old": "    s.waiter = {};\n    s.join = nullptr;\n    s.detached = true;\n    ++stats_.detached;\n"
               "    unregister_source(s, ref);\n",
        "new": "    s.waiter = {};\n    s.join = nullptr;\n    s.detached = true;\n    s.lease.reset();\n"
               "    ++stats_.detached;\n    unregister_source(s, ref);\n",
        "suite": "reactor",
        "case": "destroyed_coroutine_keeps_fixed_buffer_until_cqe",
        "expect": "assertion 'a new lease holder's bytes are not overwritten by the kernel' failed",
    },
    {
        "name": "d-drop-rights-of-detached-recvmsg",
        "what": "Do not take ownership of descriptors received by a recvmsg whose "
                "coroutine was destroyed.",
        "file": "src/reactor.cpp",
        "old": "    if (s.kind == op_kind::recvmsg) capture_rights(s);\n",
        "new": "    if (s.kind == op_kind::recvmsg && !s.detached) capture_rights(s);\n",
        "suite": "ipc",
        "case": "detached_receive_closes_received_descriptor",
        "expect": "assertion 'received descriptor closed by the slot' failed",
    },
    {
        "name": "e-stale-direct-descriptor",
        "what": "Submit an operation through a direct-descriptor reference whose direct_fd "
                "was closed, without checking its generation.",
        "file": "include/rol/reactor.hpp",
        "old": "    const bool stale = file.fixed && !direct_live(file);\n",
        "new": "    const bool stale = false;\n",
        "suite": "reactor",
        "case": "stale_direct_descriptor_is_refused",
        "expect": "assertion 'stale direct descriptor does not reach the new connection' failed",
    },
    {
        "name": "f-direct-index-freed-under-operation",
        "what": "Hand a direct-descriptor index out again as soon as its close completes, "
                "although an operation that names it is still in flight.",
        "file": "src/reactor.cpp",
        "old": "    if (e.state != direct_state::closing || e.close_pending || e.pins > 0) return;\n",
        "new": "    if (e.state != direct_state::closing || e.close_pending) return;\n",
        "suite": "reactor",
        "case": "direct_index_reserved_while_operation_in_flight",
        "expect": "assertion 'an index stays reserved while an operation names it' failed",
    },
    {
        "name": "g-one-msg-ring-per-message",
        "what": "Send every cross-core message with its own MSG_RING even when batching "
                "is on.",
        "file": "src/reactor.cpp",
        "old": "    if (!cfg_.batch_messages) flush_target(target);\n",
        "new": "    flush_target(target);\n",
        "suite": "runtime",
        "case": "concurrent_calls_share_message_rings",
        "expect": "assertion 'concurrent calls to one core share one MSG_RING' failed",
    },
    {
        "name": "h-drain-cuts-off-replies",
        "what": "On shutdown, cancel replies still being sent together with the receives, "
                "instead of after the drain deadline.",
        "file": "server/uring_server.cpp",
        "old": "    cs.recv_cancel->request();\n    cs.watchdog_running = true;\n",
        "new": "    cs.recv_cancel->request();\n    cs.hard_cancel->request();\n"
               "    cs.watchdog_running = true;\n",
        "suite": "server",
        "targets": ["server_tests", "kv_server"],
        "args": ["--server", "{build}/kv_server", "--io", "uring"],
        "case": "shutdown_waits_for_requests_in_progress",
        "expect": "assertion 'requests received before shutdown are answered' failed",
    },
    {
        "name": "i-partial-bulk-string-accepted",
        "what": "Treat a bulk string as complete before all its bytes (and the CRLF after "
                "them) have arrived.",
        "file": "kv/resp.cpp",
        "old": "            if (d.size() - start < ulen + 2) return parse_status::incomplete;\n"
               "            if (d[start + ulen] != '\\r' || d[start + ulen + 1] != '\\n')\n"
               "                return fail(\"Protocol error: bulk string not terminated by CRLF\");\n",
        "new": "            if (d.size() < start) return parse_status::incomplete;\n",
        "suite": "kv",
        "case": "split_input_parses_identically",
        "expect": "assertion 'split input parses to the same commands' failed",
    },
    {
        "name": "j-sleep-without-recheck",
        "what": "Doorbell transport: announce the sleep and block without looking at the "
                "inbound queues again, so a chain published just before the announcement "
                "(whose sender saw this core awake and did not ring) waits unseen.",
        "file": "src/reactor.cpp",
        "old": "        if (inbound_pending()) {\n",
        "new": "        if (false) {\n",
        "suite": "runtime",
        "case": "sleeping_core_is_woken_for_every_message/doorbell",
        "expect": "assertion 'a sleeping core is woken for every message' failed",
    },
    {
        "name": "k-erase-without-backward-shift",
        "what": "Delete a key from the open-addressing shard table by emptying its slot, "
                "without moving later members of the probe run back: keys behind the hole "
                "become unreachable.",
        "file": "kv/engine.cpp",
        "old": "    for (std::size_t j = (i + 1) & mask; slots_[j].e; j = (j + 1) & mask) {\n",
        "new": "    for (std::size_t j = (i + 1) & mask; false && slots_[j].e; j = (j + 1) & mask) {\n",
        "suite": "kv",
        "case": "shard_matches_reference_map",
        "expect": "assertion 'shard replies match the reference map' failed",
    },
    {
        "name": "l-number-fast-path-accepts-leading-zero",
        "what": "Let the parser's one-pass number path accept a leading zero ($01), which "
                "the strict path rejects as an invalid length.",
        "file": "kv/resp.cpp",
        "old": "    if (digit != 0) {\n",
        "new": "    if (true) {\n",
        "suite": "kv",
        "case": "parser_matches_reference",
        "expect": "assertion 'parser matches the reference parser' failed",
    },
    {
        "name": "n-call-all-resumes-early",
        "what": "Resume a call_all's caller at the first reply instead of the last, so it "
                "reads results the other cores have not published yet.",
        "file": "src/reactor.cpp",
        "old": "        if (--s.join->remaining == 0) std::exchange(s.join->parent, {}).resume();\n",
        "new": "        if (--s.join->remaining, true) std::exchange(s.join->parent, {}).resume();\n",
        "suite": "runtime",
        "case": "call_all_returns_results_in_call_order",
        "expect": "rol invariant violated: call_all resumed before every reply arrived",
    },
]

COPY = ["CMakeLists.txt", "include", "src", "kv", "server", "tests", "bench", "fuzz"]


def run(cmd, cwd=None, timeout=None, env=None):
    p = subprocess.run(cmd, cwd=cwd, timeout=timeout, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def build(src: Path, build_dir: Path, targets, liburing: str):
    rc, out = run(["cmake", "-S", str(src), "-B", str(build_dir), "-G", "Ninja",
                   "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_CXX_COMPILER=clang++",
                   "-DCMAKE_C_COMPILER=clang", f"-DROL_LIBURING_ROOT={liburing}"])
    if rc:
        return rc, out
    return run(["nice", "cmake", "--build", str(build_dir), "-j16", "--target"] + list(targets))


def test_cmd(m, build_dir: Path):
    args = [a.replace("{build}", str(build_dir)) for a in m.get("args", [])]
    return [str(build_dir / f"{m['suite']}_tests")] + args + [m["case"]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "results" / "deliberate-bugs"))
    ap.add_argument("--work", default=str(ROOT / ".cache" / "bugs"))
    ap.add_argument("--liburing", default=os.environ.get(
        "ROL_LIBURING_ROOT", str(ROOT / ".cache" / "liburing-2.14" / "install")))
    args = ap.parse_args()
    out, work = Path(args.out), Path(args.work)
    out.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    summary = []
    all_caught = True
    for m in MUTATIONS:
        tree = work / m["name"] / "src"
        if tree.exists():
            shutil.rmtree(tree)
        tree.mkdir(parents=True)
        for item in COPY:
            s = ROOT / item
            (shutil.copytree if s.is_dir() else shutil.copy2)(s, tree / item)
        target = tree / m["file"]
        text = target.read_text()
        count = text.count(m["old"])
        report = [f"mutation: {m['name']}", f"what: {m['what']}",
                  f"test: {m['suite']}_tests {m['case']}", f"expected: {m['expect']}", ""]
        if count != 1:
            report.append(f"anchor found {count} times; mutation not applied")
            caught = False
        else:
            mutated = text.replace(m["old"], m["new"])
            report += list(difflib.unified_diff(text.splitlines(), mutated.splitlines(),
                                                m["file"], m["file"] + " (mutated)", lineterm=""))
            report.append("")
            # Baseline: the unmutated test must pass in the same build setup.
            targets = m.get("targets", [f"{m['suite']}_tests"])
            rc, log = build(tree, work / m["name"] / "build-base", targets, args.liburing)
            if rc:
                report += ["baseline build failed:", log[-3000:]]
                caught = False
            else:
                brc, blog = run(test_cmd(m, work / m["name"] / "build-base"), timeout=300)
                report.append(f"baseline (unmutated) exit code: {brc}")
                target.write_text(mutated)
                rc, log = build(tree, work / m["name"] / "build-mut", targets, args.liburing)
                if rc:
                    report += ["mutated build failed (does not count as caught):", log[-3000:]]
                    caught = False
                else:
                    try:
                        mrc, mlog = run(test_cmd(m, work / m["name"] / "build-mut"), timeout=300)
                    except subprocess.TimeoutExpired:
                        mrc, mlog = -1, "timeout (does not count as caught)"
                    report.append(f"mutated exit code: {mrc}")
                    report.append("mutated output (last lines):")
                    report += mlog.strip().splitlines()[-8:]
                    caught = brc == 0 and mrc != 0 and m["expect"] in mlog
        report.append("")
        report.append(f"result: {'CAUGHT' if caught else 'NOT CAUGHT'}")
        text_out = "\n".join(report) + "\n"
        # Keep reports free of machine-specific paths.
        text_out = text_out.replace(str(work), "<work>").replace(str(ROOT), ".")
        (out / f"{m['name']}.txt").write_text(text_out)
        summary.append(f"{m['name']}: {'CAUGHT' if caught else 'NOT CAUGHT'} "
                       f"by {m['suite']}_tests {m['case']}")
        all_caught &= caught
        print(summary[-1], flush=True)
    (out / "summary.txt").write_text("\n".join(summary) + "\n")
    return 0 if all_caught else 1


if __name__ == "__main__":
    sys.exit(main())
