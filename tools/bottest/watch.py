#!/usr/bin/env python3
"""Tokenless anomaly watcher for one harness run (MoP watch-run.sh contract).

  python tools/bottest/watch.py runs/<runId> [--runner-pid N] [--poll 10] [--max-age-min 120]

Started by runner.py before step 1. Writes ONE line per anomaly to runs/<runId>/.watch
("[HH:MM:SS] KIND detail") and re-arms each detector after recovery, so Claude can follow the
file with the Monitor tool instead of polling logs.

Detectors (all READ-ONLY on the server: one persistent ssh session runs `systemctl show` and
`journalctl --after-cursor`; nothing is ever started, stopped or signalled):
  CRASH      canary not active, or ExecMainPID / NRestarts changed since the last poll
  ABORT?     journal line matching lua error|Lua Script Error|FATAL|assert
  LOGSTALL   no journal output for --logstall seconds (default 180)
  SLOW       more than --slow-per-min lines of [*_SLOW] with a duration >= --slow-ms in a minute
  CLIENTDOWN a client in clients.json (state=up) exited, or its cli-<name>.ndjson heartbeat
             (stat event every 2 s) is older than --heartbeat seconds (default 30)
  RUNFAIL    run.log gained a STUCK / Traceback line, or the runner process died without DONE
End markers: RESULT-READY (result.md has a "| Result" row), RUN-ENDED (run.log has DONE),
then DONE (watcher exits). WATCH-STOPPED when <run>/.stop exists, WATCH-EXPIRED at max age.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from winproc import popen_kw  # noqa: E402

SSH = r"C:\Windows\System32\OpenSSH\ssh.exe"
ABORT_RE = re.compile(r"lua error|Lua Script Error|FATAL|assert", re.I)
SLOW_RE = re.compile(r"\[([A-Z_]*_SLOW)\][^\n]*?(?:total|duration|body)=(\d+)ms")
STUCK_RE = re.compile(r"\bSTUCK verb=|Traceback \(most recent call last\)")
DONE_RE = re.compile(r"(?:^|\s)DONE\s*$", re.M)   # "12:49:38 DONE" (runner prefixes a time)
JLINE_RE = re.compile(r"^(\d+\.\d+) ")

# Remote side of the persistent session: one request line -> one framed reply.
REMOTE_LOOP = (
    'while read cur; do '
    'systemctl show canary -p ExecMainPID,NRestarts,ActiveState,SubState --no-pager; '
    'echo "@@J"; '
    'if [ "$cur" = "-" ]; then journalctl -u canary -n 0 --show-cursor --no-pager -o short-unix; '
    'else journalctl -u canary --after-cursor="$cur" --show-cursor --no-pager -o short-unix; fi; '
    'echo "@@END"; done'
)


# ------------------------------------------------------------------ pure detectors (unit-tested)
def parse_show(text: str) -> dict:
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            out[k.strip()] = v.strip()
    return out


class CrashDetector:
    """Feed `systemctl show canary -p ExecMainPID,NRestarts,ActiveState,SubState` output.
    Returns an anomaly string once per incident; re-arms when the service is active again
    (the new PID becomes the baseline)."""

    def __init__(self):
        self.base = None
        self.armed = True

    def feed(self, show: dict) -> str | None:
        st = show.get("ActiveState", "?")
        pid = show.get("ExecMainPID", "?")
        nr = show.get("NRestarts", "?")
        if self.base is None:
            if st == "active":
                self.base = {"pid": pid, "nr": nr}
                return None
            if self.armed:
                self.armed = False
                return f"CRASH service not active at watch start: state={st}/{show.get('SubState', '?')}"
            return None
        if st != "active":
            if self.armed:
                self.armed = False
                return f"CRASH canary state={st}/{show.get('SubState', '?')} pid={pid} (was {self.base['pid']})"
            return None
        if pid != self.base["pid"] or nr != self.base["nr"]:
            if self.armed:
                msg = (f"CRASH canary restarted: pid {self.base['pid']}->{pid} "
                       f"NRestarts {self.base['nr']}->{nr}")
            else:  # the down event was already reported; this is the recovery
                msg = f"RECOVERED canary active again pid={pid} NRestarts={nr}"
            self.base = {"pid": pid, "nr": nr}
            self.armed = True
            return msg
        if not self.armed:
            self.armed = True
            return f"RECOVERED canary active pid={pid}"
        return None


class SlowDetector:
    def __init__(self, slow_ms: int, per_min: int):
        self.slow_ms = slow_ms
        self.per_min = per_min
        self.times: list[tuple[float, str, int]] = []
        self.armed = True

    def feed(self, line: str, ts_s: float):
        m = SLOW_RE.search(line)
        if m and int(m.group(2)) >= self.slow_ms:
            self.times.append((ts_s, m.group(1), int(m.group(2))))

    def check(self, now_s: float) -> str | None:
        self.times = [t for t in self.times if now_s - t[0] <= 60]
        n = len(self.times)
        if n > self.per_min and self.armed:
            self.armed = False
            worst = max(self.times, key=lambda t: t[2])
            return f"SLOW {n} lines >= {self.slow_ms}ms in the last 60 s (worst [{worst[1]}] {worst[2]}ms)"
        if n <= self.per_min and not self.armed:
            self.armed = True
        return None


def pid_alive(pid: int) -> bool:
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong()
    ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return bool(ok) and code.value == 259  # STILL_ACTIVE


# ------------------------------------------------------------------ server session
class Remote:
    def __init__(self, target: str):
        self.target = target
        self.p = None
        self.cursor = "-"
        self.q: list[str] = []
        self.lock = threading.Lock()
        self._start()

    def _start(self):
        self.p = subprocess.Popen([SSH, "-T", "-o", "BatchMode=yes", "-o", "ServerAliveInterval=15", self.target,
                                   REMOTE_LOOP], stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace", bufsize=1,
                                  **popen_kw(subprocess.PIPE))

    def poll(self, timeout: float = 30.0):
        """-> (show dict, [journal lines]) or raises."""
        if self.p.poll() is not None:
            self._start()
        # bytes, not text: a Windows text-mode pipe appends \r, which breaks --after-cursor
        self.p.stdin.buffer.write((self.cursor + "\n").encode())
        self.p.stdin.buffer.flush()
        lines: list[str] = []
        done = threading.Event()

        def rd():
            while True:
                l = self.p.stdout.readline()
                if not l:
                    break
                l = l.rstrip("\n")
                lines.append(l)
                if l == "@@END":
                    break
            done.set()

        t = threading.Thread(target=rd, daemon=True)
        t.start()
        if not done.wait(timeout) or not lines or lines[-1] != "@@END":
            try:
                self.p.kill()
            except Exception:
                pass
            raise RuntimeError("ssh poll timed out or session ended")
        j = lines.index("@@J")
        show = parse_show("\n".join(lines[:j]))
        jl = []
        for l in lines[j + 1:-1]:
            if l.startswith("-- cursor: "):
                self.cursor = l[len("-- cursor: "):].strip()
            elif l.startswith("-- No entries --"):
                continue
            else:
                jl.append(l)
        return show, jl

    def close(self):
        try:
            self.p.stdin.close()
            self.p.kill()
        except Exception:
            pass


# ------------------------------------------------------------------ watcher
class Watcher:
    def __init__(self, run_dir: Path, args):
        self.run = run_dir
        self.a = args
        self.out = open(run_dir / ".watch", "a", encoding="utf-8")
        self.crash = CrashDetector()
        self.slow = SlowDetector(args.slow_ms, args.slow_per_min)
        self.last_journal = time.time()
        self.logstall_armed = True
        self.abort_reported = 0
        self.runlog_off = 0
        self.client_down: dict[str, bool] = {}
        self.result_ready = False
        self.ssh_fail = 0

    def emit(self, line: str):
        self.out.write(time.strftime("[%H:%M:%S] ") + line + "\n")
        self.out.flush()

    def check_server(self, remote: Remote):
        try:
            show, jl = remote.poll()
            self.ssh_fail = 0
        except Exception as e:
            self.ssh_fail += 1
            if self.ssh_fail == 2:
                self.emit(f"CRASH? server unreachable over ssh twice ({e})")
            return
        msg = self.crash.feed(show)
        if msg:
            self.emit(msg)
        now = time.time()
        if jl:
            self.last_journal = now
            if not self.logstall_armed:
                self.logstall_armed = True
                self.emit("RECOVERED journal output resumed")
        for l in jl:
            if ABORT_RE.search(l):
                self.abort_reported += 1
                if self.abort_reported <= 50:
                    self.emit("ABORT? " + l[:300])
                elif self.abort_reported == 51:
                    self.emit("ABORT? (further ABORT? lines suppressed)")
            m = JLINE_RE.match(l)
            self.slow.feed(l, float(m.group(1)) if m else now)
        smsg = self.slow.check(now)
        if smsg:
            self.emit(smsg)
        if self.logstall_armed and now - self.last_journal > self.a.logstall:
            self.logstall_armed = False
            self.emit(f"LOGSTALL no journal output for {int(now - self.last_journal)} s")

    def check_clients(self):
        p = self.run / "clients.json"
        if not p.exists():
            return
        try:
            clients = json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            return
        now = time.time()
        for name, c in clients.items():
            st = c.get("state")
            if st not in ("up", "lost"):  # starting / stopped on purpose by the runner
                self.client_down.pop(name, None)
                continue
            why = None
            if st == "lost":
                why = f"pid {c['pid']} exited on its own (rc={c.get('rc')}, runner marked it lost)"
            elif not pid_alive(int(c["pid"])):
                why = f"pid {c['pid']} exited"
            else:
                f = self.run / f"cli-{name}.ndjson"
                age = now - (f.stat().st_mtime if f.exists() else c.get("started", now))
                if age > self.a.heartbeat:
                    why = f"no heartbeat for {int(age)} s (pid {c['pid']} alive)"
            if why and not self.client_down.get(name):
                self.client_down[name] = True
                self.emit(f"CLIENTDOWN {name} ({c.get('role', '?')} {c.get('char', '')}) {why}")
            elif not why and self.client_down.get(name):
                self.client_down[name] = False
                self.emit(f"RECOVERED client {name}")

    def check_run(self) -> bool:
        """-> True when the run has ended (RUN-ENDED written)."""
        # sample runner liveness BEFORE reading run.log: a runner that wrote DONE and exited
        # between the two reads must not be reported as "exited without DONE"
        gone = bool(self.a.runner_pid) and not pid_alive(self.a.runner_pid)
        rl = self.run / "run.log"
        text = ""
        if rl.exists():
            with open(rl, "rb") as fh:
                fh.seek(self.runlog_off)
                data = fh.read()
            self.runlog_off += len(data)
            text = data.decode("utf-8", errors="replace")
            for line in text.splitlines():
                if STUCK_RE.search(line):
                    self.emit("RUNFAIL " + line.strip()[:300])
        res = self.run / "result.md"
        if not self.result_ready and res.exists():
            if any(l.startswith("| Result") for l in res.read_text(encoding="utf-8", errors="replace").splitlines()):
                self.result_ready = True
                self.emit("RESULT-READY " + str(res))
        if text and DONE_RE.search(text):
            self.emit("RUN-ENDED")
            return True
        if gone:
            # run.log was fully drained above after the runner was already gone, and had no DONE
            self.emit(f"RUNFAIL runner pid {self.a.runner_pid} exited without DONE")
            self.emit("RUN-ENDED (runner gone)")
            return True
        return False

    def loop(self):
        remote = Remote(self.a.ssh_target)
        t0 = time.time()
        self.emit(f"WATCH-START run={self.run.name} poll={self.a.poll}s runner_pid={self.a.runner_pid}")
        try:
            while True:
                if (self.run / ".stop").exists():
                    self.emit("WATCH-STOPPED")
                    break
                if time.time() - t0 > self.a.max_age_min * 60:
                    self.emit("WATCH-EXPIRED")
                    break
                self.check_server(remote)
                self.check_clients()
                if self.check_run():
                    break
                time.sleep(self.a.poll)
        finally:
            remote.close()
            self.emit("DONE")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--runner-pid", type=int, default=0)
    ap.add_argument("--ssh-target", default="root@127.0.0.1")
    ap.add_argument("--poll", type=float, default=10.0)
    ap.add_argument("--logstall", type=float, default=180.0)
    ap.add_argument("--heartbeat", type=float, default=30.0)
    ap.add_argument("--slow-ms", type=int, default=500)
    ap.add_argument("--slow-per-min", type=int, default=3)
    ap.add_argument("--max-age-min", type=float, default=120.0)
    a = ap.parse_args()
    run = Path(a.run_dir)
    pidf = run / ".watch.pid"
    if pidf.exists():
        try:
            other = int(pidf.read_text().strip())
            if other != os.getpid() and pid_alive(other):
                print(f"already watched by pid {other}", file=sys.stderr)
                sys.exit(1)
        except ValueError:
            pass
    pidf.write_text(str(os.getpid()))
    try:
        Watcher(run, a).loop()
    finally:
        try:
            pidf.unlink()
        except OSError:
            pass


if __name__ == "__main__":
    main()
