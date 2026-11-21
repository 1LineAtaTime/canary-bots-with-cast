#!/usr/bin/env python3
"""Bot test harness hub (library for runner.py + the Phase 0 acceptance scenario).

Loopback WebSocket server that OTClient instances running the `harness_ctl` mod connect to.
It routes verbs to clients, writes every client event as NDJSON (plan section 3.2 envelope)
to runs/<runId>/cli-<name>.ndjson, and runs the Phase 0 acceptance scenario:

  (a) player login via session key to the game port, goto, see_appear of a bot with
      cid == Player::firstID (0x10000000) + guid asserted against the DB
  (b) @cast camera login on that bot receiving a creature move
  (c) operator /cavebot "<bot>" status issued in-band, reply captured
  (d) client-event latency vs server time (lagmark round trips + journal joins)
  (e) packet records written, start epoch logged

Usage:
  python tools/bottest/hub.py phase0 [--run-id ID] [--fps 10] [--fps2 60]

Secrets come from tools/bottest/harness.local.json (gitignored) and .claude/secrets.local. In a
git worktree (gitignored files are per checkout) both fall back to the MAIN checkout's copies, so
secrets are never copied into a worktree.
Nothing secret is written into the run dir: login verbs are logged with the session key
redacted.
"""
from __future__ import annotations

import argparse
import asyncio
import ctypes
import itertools
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

import websockets

sys.path.insert(0, str(Path(__file__).resolve().parent))
from winproc import popen_kw  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SSH = r"C:\Windows\System32\OpenSSH\ssh.exe"
WSL = "wsl.exe"
MYSQL = r"C:\Program Files\MySQL\MySQL Workbench 8.0 CE\mysql.exe"
PLAYER_FIRST_ID = 0x10000000  # src/creatures/players/player.cpp:154
BOT_GUID_MIN, BOT_GUID_MAX = 65001, 66004
CHANNEL_CAST = 0x50  # src/utils/const.hpp:22

DEFAULTS = {
    "server_host": "127.0.0.1",
    "ssh_target": "root@127.0.0.1",
    "game_port": 7172,
    "world": "Canary",
    "client_dir": "~/Documents/otc-harness",
    "client_exe": "otclient_gl_x64.exe",
    "hub_port": 8765,
    "player_char": "Harness One",
    "client_version": 1525,
}


# The config load_config() returned last: load_db_secrets() takes the profile's db_name from it.
ACTIVE: dict = {}

LOCAL_OVERRIDE_KEYS = ("server_host", "ssh_target", "game_port", "world", "audit_path", "client_dir")


def load_config(path: Path | None = None, profile: str | None = None) -> dict:
    """Explicit harness config: DEFAULTS < tools/bottest/harness.json (non-secret, committed) < the
    machine-specific keys in harness.local.json (gitignored: the real server address and ssh target
    stay out of the repo) < the selected target profile (harness.json "profiles"; --profile or env
    HARNESS_PROFILE). A profile can repoint the server (server_host / game_port), the remote shell
    (ssh_target, which may be "wsl:<distro>[:<user>]"), the systemd unit read by journalctl
    (journal_unit), the database (db_name), the audit file and the client (client_version / client_dir)."""
    cfg = dict(DEFAULTS)
    p = path or (HERE / "harness.json")
    if p.exists():
        cfg.update(json.loads(p.read_text(encoding="utf-8")))
    try:
        local = load_local_secrets()
    except (OSError, ValueError):
        local = {}
    cfg.update({k: local[k] for k in LOCAL_OVERRIDE_KEYS if k in local})
    profile = profile or os.environ.get("HARNESS_PROFILE") or None
    profiles = cfg.pop("profiles", {})
    if profile:
        if profile not in profiles:
            raise SystemExit(f"harness profile {profile!r} not in harness.json (have: {', '.join(sorted(profiles))})")
        cfg.update({k: v for k, v in profiles[profile].items() if not k.startswith("_")})
    cfg["profile"] = profile or ""
    cfg["client_dir"] = os.path.expanduser(cfg["client_dir"])
    ACTIVE.clear()
    ACTIVE.update(cfg)
    return cfg


def remote_argv(target: str, cmd: str, keepalive: bool = False) -> list[str]:
    """argv that runs `cmd` with bash on the target, windowless via popen_kw() at the call site.

    target "user@host" -> OpenSSH (BatchMode). target "wsl:<distro>[:<user>]" -> hidden wsl.exe; --exec
    hands bash -c the command verbatim (wsl.exe's own shell would re-parse $ and quotes)."""
    if target.startswith("wsl:"):
        parts = target.split(":")
        distro = parts[1] if len(parts) > 1 and parts[1] else "Ubuntu-22.04"
        user = parts[2] if len(parts) > 2 and parts[2] else "root"
        return [WSL, "-d", distro, "-u", user, "--exec", "bash", "-c", cmd]
    opts = ["-T", "-o", "BatchMode=yes"]
    if keepalive:
        opts += ["-o", "ServerAliveInterval=15"]
    return [SSH, *opts, target, cmd]


def journal_unit(cfg: dict) -> str:
    """systemd unit of the server under test (production: canary; WSL / second-instance arms differ)."""
    return shlex.quote(str(cfg.get("journal_unit") or "canary"))


# ------------------------------------------------------------------ clocks
class _FT(ctypes.Structure):
    _fields_ = [("lo", ctypes.c_uint32), ("hi", ctypes.c_uint32)]


_k32 = ctypes.windll.kernel32 if os.name == "nt" else None


def epoch_ms() -> float:
    """Wall-clock epoch ms with sub-ms resolution (Python 3.12 time.time() on Windows is coarse)."""
    if _k32 is not None:
        ft = _FT()
        _k32.GetSystemTimePreciseAsFileTime(ctypes.byref(ft))
        return ((ft.hi << 32) | ft.lo) / 10000.0 - 11644473600000.0
    return time.time() * 1000.0


def main_checkout() -> Path | None:
    """The main working tree when REPO is a linked git worktree (else None)."""
    try:
        common = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, timeout=10, **popen_kw()).stdout.strip()
    except Exception:
        return None
    if not common:
        return None
    main = Path(common).resolve().parent
    return main if main != REPO.resolve() else None


def secret_path(rel: str) -> Path:
    """REPO/<rel>, or the main checkout's copy when running from a worktree that has none."""
    p = REPO / rel
    if p.exists():
        return p
    m = main_checkout()
    if m and (m / rel).exists():
        return m / rel
    return p


def load_local_secrets() -> dict:
    p = secret_path("tools/bottest/harness.local.json")
    return json.loads(p.read_text(encoding="utf-8"))


def load_db_secrets() -> dict:
    vals = {}
    for line in secret_path(".claude/secrets.local").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        k = k.strip()
        if k.startswith("export "):
            k = k[len("export "):].strip()
        vals[k] = v.strip().strip("\"'")
    if ACTIVE.get("db_name"):
        vals["DB_NAME"] = ACTIVE["db_name"]  # profile: a disposable copy, never the live DB by accident
    # profile: a disposable DB server (P5 WSL arms run a local MySQL in WSL, 127.0.0.1:3316)
    if ACTIVE.get("db_host"):
        vals["DB_HOST"] = str(ACTIVE["db_host"])
    if ACTIVE.get("db_port"):
        vals["DB_PORT"] = str(ACTIVE["db_port"])
    return vals


def db_query(sql: str) -> list[list[str]]:
    s = load_db_secrets()
    cmd = [MYSQL, "-h", s["DB_HOST"], "-P", s.get("DB_PORT", "3306"), "-u", s["DB_USER"],
           f"-p{s['DB_PASS']}", "-N", "-B", s.get("DB_NAME", "canary"), "-e", sql]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=30, **popen_kw())
    if out.returncode != 0:
        raise RuntimeError(out.stderr.strip())
    return [line.split("\t") for line in out.stdout.splitlines() if line]


class ServerClock:
    """PC<->LXC clock offset over ONE persistent ssh session (no per-sample ssh startup)."""

    def __init__(self, target: str):
        import threading
        self.lock = threading.Lock()
        self.p = subprocess.Popen(
            remote_argv(target, 'while read l; do if [ "$l" = z ]; then date +%z; else date +%s%N; fi; done'),
            stdout=subprocess.PIPE, text=True, bufsize=1, **popen_kw(subprocess.PIPE))

    def tz(self) -> str:
        with self.lock:
            # bytes: a Windows text-mode pipe would send "z\r\n" and the remote test fails
            self.p.stdin.buffer.write(b"z\n")
            self.p.stdin.buffer.flush()
            return self.p.stdout.readline().strip()

    def sample(self, n: int = 40) -> dict:
        with self.lock:
            return self._sample(n)

    def _sample(self, n: int) -> dict:
        rows = []
        for _ in range(n):
            t0 = epoch_ms()
            self.p.stdin.buffer.write(b"x\n")
            self.p.stdin.buffer.flush()
            line = self.p.stdout.readline()
            t1 = epoch_ms()
            srv = int(line.strip()) / 1e6
            rows.append({"t0": t0, "t1": t1, "srv": srv, "rtt": t1 - t0, "off": srv - (t0 + t1) / 2})
            time.sleep(0.02)
        rows.sort(key=lambda r: r["rtt"])
        best = rows[: max(3, n // 5)]
        off = sorted(r["off"] for r in best)[len(best) // 2]
        return {"offset_ms": off, "best_rtt_ms": rows[0]["rtt"], "median_rtt_ms": rows[len(rows) // 2]["rtt"],
                "n": n, "uncertainty_ms": rows[0]["rtt"] / 2, "at_pc_ms": epoch_ms()}

    def close(self):
        try:
            self.p.stdin.close()
            self.p.terminate()
        except Exception:
            pass


class ClockTracker:
    """Re-samples the PC<->LXC offset every `every_s` (<=10 min; drift is ~1.5 ms / 2.5 min)
    over the persistent ssh session and interpolates between samples for joins."""

    def __init__(self, hub: "Hub", target: str, every_s: float = 300.0):
        self.hub = hub
        self.clock = ServerClock(target)
        self.every_s = min(every_s, 600.0)
        self.samples: list[dict] = []
        self.task = None
        self.tz = None

    async def start(self):
        self.tz = await asyncio.to_thread(self.clock.tz)
        await self.resample("start")
        self.task = asyncio.create_task(self._loop())

    async def _loop(self):
        while True:
            await asyncio.sleep(self.every_s)
            try:
                await self.resample("periodic")
            except Exception as e:  # never kill the run over one bad sample
                self.hub.log("clock_error", err=str(e))

    async def resample(self, phase: str, n: int = 40) -> dict:
        s = await asyncio.to_thread(self.clock.sample, n)
        s["phase"] = phase
        self.samples.append(s)
        self.hub.log("clock_offset", **s)
        return s

    def offset_at(self, pc_ms: float | None = None) -> float:
        return offset_at(self.samples, pc_ms)

    async def stop(self):
        if self.task:
            self.task.cancel()
        try:
            await self.resample("end")
        except Exception as e:
            self.hub.log("clock_error", err=str(e))
        self.clock.close()


def offset_at(samples: list[dict], pc_ms: float | None = None) -> float:
    """Linear interpolation of offset_ms (server - pc) between samples; clamps at the ends."""
    if not samples:
        return 0.0
    ss = sorted(samples, key=lambda s: s["at_pc_ms"])
    if pc_ms is None:
        return ss[-1]["offset_ms"]
    if len(ss) == 1 or pc_ms <= ss[0]["at_pc_ms"]:
        return ss[0]["offset_ms"]
    for a, b in zip(ss, ss[1:]):
        if pc_ms <= b["at_pc_ms"]:
            f = (pc_ms - a["at_pc_ms"]) / max(1e-9, b["at_pc_ms"] - a["at_pc_ms"])
            return a["offset_ms"] + f * (b["offset_ms"] - a["offset_ms"])
    return ss[-1]["offset_ms"]


def strip_cam_login(path: Path) -> int:
    """Drop the login packet (client opcode 0x0A: RSA block with the session key) from an
    OTClient packet record. Lines are '<dir> <ms> <hex>'; only outgoing ('>') lines carry it.
    Returns the number of lines removed."""
    p = Path(path)
    if not p.exists():
        return 0
    kept, dropped = [], 0
    with open(p, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parts = line.split(" ", 2)
            if len(parts) == 3 and parts[0] == ">" and parts[2].lower().startswith("0a"):
                dropped += 1
                continue
            kept.append(line)
    if dropped:
        tmp = p.with_suffix(p.suffix + ".tmp")
        tmp.write_text("".join(kept), encoding="utf-8")
        os.replace(tmp, p)
    return dropped


# ------------------------------------------------------------------ hub
class Client:
    def __init__(self, name: str, ws, run_dir: Path):
        self.name = name
        self.ws = ws
        self.pending: dict[int, asyncio.Future] = {}
        self.events: list[dict] = []
        self.waiters: list[tuple] = []
        self.sync: list[dict] = []  # ping samples for cms->epoch
        self.ffi = False
        self.write_dir = None
        self.closed = False
        self.last_rx = epoch_ms()
        self.fh = open(run_dir / f"cli-{name}.ndjson", "a", encoding="utf-8")


class Hub:
    def __init__(self, run_dir: Path, port: int):
        self.run_dir = run_dir
        self.port = port
        self.clients: dict[str, Client] = {}
        self.hello_waiters: dict[str, asyncio.Future] = {}
        self.seq = itertools.count(1)
        self.ids = itertools.count(1)
        self.hublog = open(run_dir / "hub.ndjson", "a", encoding="utf-8")
        self.server = None
        self.echo = True

    def log(self, k: str, **fields):
        rec = {"v": 1, "ts": round(epoch_ms(), 1), "seq": next(self.seq), "src": "hub", "k": k, **fields}
        self.hublog.write(json.dumps(rec) + "\n")
        self.hublog.flush()
        if self.echo:
            print(f"[hub] {k} {json.dumps(fields)[:300]}", flush=True)

    async def start(self):
        self.server = await websockets.serve(self._handler, "127.0.0.1", self.port, max_size=8 * 1024 * 1024)
        self.log("hub_listen", port=self.port)

    async def stop(self):
        if self.server:
            self.server.close()
            await self.server.wait_closed()

    def _to_epoch(self, c: Client, m: dict) -> float:
        if "ems" in m:
            return float(m["ems"])
        # fallback: min-RTT ping fit of cms -> epoch
        if c.sync:
            best = min(c.sync, key=lambda s: s["rtt"])
            return m["cms"] + best["off"]
        return m.get("_rx", epoch_ms())

    async def _handler(self, ws):
        c = None
        try:
            async for raw in ws:
                rx = epoch_ms()
                m = json.loads(raw)
                t = m.get("t")
                if t == "hello":
                    name = m.get("name", "anon")
                    c = Client(name, ws, self.run_dir)
                    c.ffi = bool(m.get("ffi"))
                    c.write_dir = m.get("writeDir")
                    self.clients[name] = c
                    self.log("client_hello", name=name, ffi=c.ffi, version=m.get("version"))
                    fut = self.hello_waiters.get(name)
                    if fut and not fut.done():
                        fut.set_result(c)
                    continue
                if c is None:
                    continue
                c.last_rx = rx
                if t == "pong":
                    fut = c.pending.pop(m["id"], None)
                    if fut and not fut.done():
                        fut.set_result({**m, "_rx": rx})
                    continue
                if t == "reply":
                    rec = self._envelope(c, m, rx, kind="reply")
                    fut = c.pending.pop(m["id"], None)
                    if fut and not fut.done():
                        fut.set_result(rec)
                    continue
                if t == "ev":
                    rec = self._envelope(c, m, rx)
                    c.events.append(rec)
                    for w in list(c.waiters):
                        pred, fut = w
                        if not fut.done() and pred(rec):
                            fut.set_result(rec)
                            c.waiters.remove(w)
        except websockets.ConnectionClosed:
            pass
        finally:
            if c:
                c.closed = True
                for fut in c.pending.values():
                    if not fut.done():
                        fut.set_result({"ok": False, "text": "STUCK verb=? reason=client-down"})
                c.pending.clear()
                self.log("client_closed", name=c.name)

    def _envelope(self, c: Client, m: dict, rx: float, kind: str | None = None) -> dict:
        m = dict(m)
        m["_rx"] = rx
        ts = self._to_epoch(c, m)
        rec = {"v": 1, "ts": round(ts, 1), "seq": next(self.seq), "src": f"cli:{c.name}",
               "k": kind or m.pop("k", "?"), "rx": round(rx, 1)}
        for key, val in m.items():
            if key in ("t", "_rx", "ems"):
                continue
            rec["cseq" if key == "seq" else key] = val
        c.fh.write(json.dumps(rec) + "\n")
        c.fh.flush()
        return rec

    async def wait_hello(self, name: str, timeout: float) -> Client:
        if name in self.clients:
            return self.clients[name]
        fut = asyncio.get_running_loop().create_future()
        self.hello_waiters[name] = fut
        return await asyncio.wait_for(fut, timeout)

    async def cmd(self, name: str, verb: str, /, timeout: float = 30.0, redact: tuple = (), **args) -> dict:
        c = self.clients[name]
        if c.closed:
            return {"ok": False, "text": f"STUCK verb={verb} reason=client-down"}
        cid = next(self.ids)
        fut = asyncio.get_running_loop().create_future()
        c.pending[cid] = fut
        shown = {k: ("<redacted>" if k in redact else v) for k, v in args.items()}
        self.log("cmd", client=name, id=cid, verb=verb, args=shown)
        try:
            await c.ws.send(json.dumps({"t": "cmd", "id": cid, "verb": verb, "args": args}))
        except websockets.ConnectionClosed:
            c.pending.pop(cid, None)
            return {"ok": False, "text": f"STUCK verb={verb} reason=client-down"}
        try:
            rec = await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            c.pending.pop(cid, None)
            rec = {"ok": False, "text": f"STUCK verb={verb} reason=hub-timeout"}
        self.log("cmd_result", client=name, id=cid, verb=verb, ok=rec.get("ok"), text=rec.get("text"))
        return rec

    async def ping_sync(self, name: str, n: int = 15):
        c = self.clients[name]
        for _ in range(n):
            pid = next(self.ids)
            fut = asyncio.get_running_loop().create_future()
            c.pending[pid] = fut
            t0 = epoch_ms()
            await c.ws.send(json.dumps({"t": "ping", "id": pid}))
            try:
                m = await asyncio.wait_for(fut, 5)
            except asyncio.TimeoutError:
                continue
            t1 = m["_rx"]
            sample = {"rtt": t1 - t0, "off": (t0 + t1) / 2 - m["cms"]}
            if "ems" in m:
                sample["ems_minus_mid"] = m["ems"] - (t0 + t1) / 2
            c.sync.append(sample)
            await asyncio.sleep(0.05)
        best = min(c.sync, key=lambda s: s["rtt"]) if c.sync else None
        rtts = sorted(s["rtt"] for s in c.sync)
        self.log("ping_sync", client=name, n=len(c.sync), best=best,
                 rtt_p50=rtts[len(rtts) // 2] if rtts else None, rtt_max=rtts[-1] if rtts else None)

    async def wait_event(self, name: str, pred, timeout: float, since_seq: int = 0):
        c = self.clients[name]
        for rec in c.events:
            if rec["seq"] > since_seq and pred(rec):
                return rec
        fut = asyncio.get_running_loop().create_future()
        c.waiters.append((pred, fut))
        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            return None


# ------------------------------------------------------------------ client processes
def launch_client(cfg: dict, name: str, run_dir: Path) -> subprocess.Popen:
    env = dict(os.environ)
    env["OTC_HARNESS_HUB"] = f"ws://127.0.0.1:{cfg['hub_port']}"
    env["OTC_HARNESS_INSTANCE"] = name
    if not cfg.get("visible"):
        env["OTC_HARNESS_HIDDEN"] = "1"   # headless by default; --visible to debug
    out = open(run_dir / f"otc-{name}.stdout.log", "w", encoding="utf-8", errors="replace")
    exe = str(Path(cfg["client_dir"]) / cfg["client_exe"])
    # GUI-subsystem exe (no console of its own); no-window flags + DEVNULL stdin keep it that way
    return subprocess.Popen([exe], cwd=cfg["client_dir"], env=env, stdout=out, stderr=subprocess.STDOUT, **popen_kw())


def png_stats(path: Path) -> dict:
    """Cheap stdlib PNG check: size and the share of non-zero RGB bytes in the raw scanlines
    (an all-black capture decompresses to zeros apart from the alpha channel)."""
    import struct
    import zlib
    data = Path(path).read_bytes()
    if data[1:4] != b"PNG":
        return {"ok": False}
    pos, idat = 8, b""
    w = h = 0
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif typ == b"IDAT":
            idat += body
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4 + 1
    nz = tot = 0
    for r in range(0, min(h, 400), max(1, h // 200)):
        row = raw[r * stride + 1:(r + 1) * stride]
        rgb = bytes(b for i, b in enumerate(row) if i % 4 != 3)
        tot += len(rgb)
        nz += len(rgb) - rgb.count(0)
    return {"ok": True, "w": w, "h": h, "nonzero_rgb": round(nz / max(1, tot), 3)}


def stop_client(p: subprocess.Popen, timeout: float = 10.0):
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait(timeout=5)


def bot_cid_range(cid: int) -> bool:
    return BOT_GUID_MIN + PLAYER_FIRST_ID <= cid <= BOT_GUID_MAX + PLAYER_FIRST_ID


def pull_server_log(cfg: dict, run_dir: Path, since_epoch_s: float, until_epoch_s: float):
    cmd = (f"journalctl -u {journal_unit(cfg)} --since @{int(since_epoch_s)} --until @{int(until_epoch_s) + 1} "
           f"-o short-unix --no-pager; echo '#TZ' $(date +%z)")
    out = subprocess.run(remote_argv(cfg["ssh_target"], cmd),
                         capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120, **popen_kw())
    (run_dir / "server.log").write_text(out.stdout, encoding="utf-8")
    return out.stdout.count("\n")


# ------------------------------------------------------------------ server audit (Phase 2)
def audit_tail_cmd(cfg: dict) -> str | None:
    """Remote command that live-follows the engine audit file (survives the sink's rotation)."""
    path = cfg.get("audit_path")
    return f"tail -F -n 0 {shlex.quote(path)} 2>/dev/null" if path else None


def pull_audit(cfg: dict, run_dir: Path, since_srv_ms: float, until_srv_ms: float) -> int:
    """Authoritative copy of the audit window -> run_dir/audit.ndjson (server clock, raw records).

    Read-only: cat of <path>.1 then <path> (the sink keeps one rotated generation), filtered on
    the server by the record's own epoch-ms `ts` (the envelope always starts {"v":1,"ts":N,).
    Returns the line count, or -1 when harness.json has no audit_path."""
    path = cfg.get("audit_path")
    if not path:
        return -1
    q = shlex.quote(path)
    awk = ("awk -v lo=%d -v hi=%d -F'\"ts\":' "
           "'NF>1 { split($2, a, \",\"); t = a[1] + 0; if (t >= lo && t <= hi) print }'"
           % (int(since_srv_ms), int(until_srv_ms)))
    cmd = f"for f in {q}.1 {q}; do [ -f \"$f\" ] && cat \"$f\"; done | {awk}"
    out = subprocess.run(remote_argv(cfg["ssh_target"], cmd),
                         capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180, **popen_kw())
    (run_dir / "audit.ndjson").write_text(out.stdout, encoding="utf-8")
    return out.stdout.count("\n")


# ------------------------------------------------------------------ scenario
async def phase0(args):
    cfg = load_config(profile=getattr(args, "profile", None))
    sec = load_local_secrets()
    run_id = args.run_id or time.strftime("p0_%Y%m%d_%H%M%S")
    run_dir = REPO / "runs" / run_id
    (run_dir / "records").mkdir(parents=True, exist_ok=True)
    result: dict = {"run_id": run_id, "checks": {}, "notes": []}

    def check(name, ok, **detail):
        result["checks"][name] = {"ok": bool(ok), **detail}
        hub.log("check", name=name, ok=bool(ok), **detail)

    hub = Hub(run_dir, cfg["hub_port"])
    await hub.start()
    t_start = epoch_ms()
    clock = ServerClock(cfg["ssh_target"])
    clk_before = await asyncio.to_thread(clock.sample, 40)
    hub.log("clock_offset", phase="before", **clk_before)

    procs = {}
    try:
        # ---------------- (a) player
        procs["p1"] = launch_client(cfg, "p1", run_dir)
        hub.log("client_launch", name="p1", pid=procs["p1"].pid)
        await hub.wait_hello("p1", 90)
        await hub.ping_sync("p1")
        await hub.cmd("p1", "set_fps", fps=args.fps)
        rec_p1 = str(run_dir / "records" / "p1.cam")
        login_seq = next(hub.seq)
        r = await hub.cmd("p1", "login", timeout=45, redact=("sessionKey",), host=cfg["server_host"],
                          port=cfg["game_port"], world=cfg["world"], char=cfg["player_char"], version=int(cfg["client_version"]),
                          sessionKey=sec["account_email"] + "\n" + sec["account_password"], recordTo=rec_p1)
        ev = await hub.wait_event("p1", lambda e: e["k"] in ("login_ok", "login_error", "conn_error"), 45, login_seq)
        if not ev or ev["k"] != "login_ok":
            check("a_login", False, event=ev)
            return result
        check("a_login", True, cid=ev.get("cid"), pos=ev.get("pos"))
        self_cid = ev.get("cid")
        db_self = db_query(f"SELECT id FROM players WHERE name='{cfg['player_char']}'")
        check("a_self_cid", db_self and self_cid == PLAYER_FIRST_ID + int(db_self[0][0]),
              cid=self_cid, guid=int(db_self[0][0]) if db_self else None)
        sp = await hub.wait_event("p1", lambda e: e["k"] == "self_pos" and e.get("to"), 15, ev["seq"])
        if not sp:
            check("a_self_pos", False)
            return result
        start = sp["to"]
        await asyncio.sleep(2)

        # goto a nearby tile: try a few offsets around the temple spawn
        goto_ok = None
        for dx, dy in args.goto_offsets:
            dest = {"x": start["x"] + dx, "y": start["y"] + dy, "z": start["z"]}
            r = await hub.cmd("p1", "goto", timeout=40, x=dest["x"], y=dest["y"], z=dest["z"], range=0,
                              timeout_s=30, stuck=8)
            if r.get("ok"):
                goto_ok = {"dest": dest, "text": r.get("text")}
                break
        check("a_goto", goto_ok is not None, detail=goto_ok)

        # wait for a bot to appear
        def is_bot_appear(e):
            return e["k"] == "see_appear" and e.get("ctype") == "player" and bot_cid_range(int(e.get("cid", 0)))
        bot_ev = await hub.wait_event("p1", is_bot_appear, args.bot_wait, 0)
        if not bot_ev:
            check("a_bot_seen", False, waited_s=args.bot_wait)
            return result
        # give the area a little time so several bots show up, then pick the closest still visible
        await asyncio.sleep(args.settle)
        seen = [e for e in hub.clients["p1"].events if is_bot_appear(e)]
        names = sorted({e["name"] for e in seen})
        in_list = ",".join("'" + n.replace("'", "''") + "'" for n in names)
        db_rows = db_query(f"SELECT name, id, account_id, group_id FROM players WHERE name IN ({in_list})")
        db_map = {row[0]: row for row in db_rows}
        asserts = []
        for e in seen:
            row = db_map.get(e["name"])
            guid = int(row[1]) if row else None
            asserts.append({"name": e["name"], "cid": e["cid"], "guid": guid,
                            "match": guid is not None and e["cid"] == PLAYER_FIRST_ID + guid,
                            "account_id": int(row[2]) if row else None})
        uniq = {a["name"]: a for a in asserts}
        all_match = all(a["match"] for a in uniq.values())
        check("a_bot_seen", True, first=bot_ev["name"], first_cid=bot_ev["cid"], n_bots=len(uniq))
        check("a_cid_equals_firstid_plus_guid", all_match and len(uniq) > 0, bots=list(uniq.values())[:20])

        view = await hub.cmd("p1", "dump_view", multifloor=False)
        visible = [c for c in view.get("creatures", []) if c.get("ctype") == "player" and bot_cid_range(int(c["cid"]))]
        target = None
        if visible:
            me = view.get("pos") or start
            visible.sort(key=lambda c: max(abs(c["pos"]["x"] - me["x"]), abs(c["pos"]["y"] - me["y"])))
            target = visible[0]["name"]
        else:
            target = bot_ev["name"]
        hub.log("cam_target", bot=target, visible=[c["name"] for c in visible])

        # ---------------- (b) camera
        procs["c1"] = launch_client(cfg, "c1", run_dir)
        hub.log("client_launch", name="c1", pid=procs["c1"].pid)
        await hub.wait_hello("c1", 90)
        await hub.ping_sync("c1")
        await hub.cmd("c1", "set_fps", fps=args.fps)
        rec_c1 = str(run_dir / "records" / "c1.cam")
        cam_seq = next(hub.seq)
        await hub.cmd("c1", "login", timeout=45, redact=("sessionKey",), host=cfg["server_host"],
                      port=cfg["game_port"], world=cfg["world"], char=target, version=int(cfg["client_version"]),
                      sessionKey="@cast\n" + sec["cast_operator_password"], recordTo=rec_c1)
        ev = await hub.wait_event("c1", lambda e: e["k"] in ("login_ok", "login_error", "conn_error"), 45, cam_seq)
        if not ev or ev["k"] != "login_ok":
            check("b_cam_login", False, event=ev)
            return result
        check("b_cam_login", True, bot=target, cid=ev.get("cid"))
        op = await hub.wait_event("c1", lambda e: e["k"] == "see_talk" and "Operator session" in str(e.get("text")), 10, cam_seq)
        check("b_operator_session", op is not None, text=op and op.get("text"))
        # a real step: from AND to both known (the initial self_pos after login has from=null)
        mv = await hub.wait_event("c1", lambda e: e["k"] in ("see_move", "self_pos") and e.get("from") and e.get("to"),
                                  args.move_wait, ev["seq"])
        check("b_cam_creature_move", mv is not None, event=mv and {k: mv.get(k) for k in ("k", "name", "cid", "from", "to")})

        # ---------------- (c) operator /cavebot status
        cmd_text = f'/cavebot "{target}" status'
        pre = next(hub.seq)
        await hub.cmd("c1", "channel_say", channel=CHANNEL_CAST, text=cmd_text)
        rep = await hub.wait_event("c1", lambda e: e["k"] in ("see_msg", "see_text") and f"Bot '{target}'" in str(e.get("text")), 15, pre)
        echo = await hub.wait_event("c1", lambda e: e["k"] == "see_talk" and str(e.get("text", "")).startswith("> /cavebot"), 1, pre)
        check("c_cavebot_status_reply", rep is not None, reply=rep and rep.get("text"), echo=echo and echo.get("text"))

        # ---------------- (d) lagmark round trips at two fps caps
        for fps in (args.fps, args.fps2):
            await hub.cmd("c1", "set_fps", fps=fps)
            await asyncio.sleep(1.5)
            for i in range(args.lagmarks):
                s = await hub.cmd("c1", "channel_say", channel=CHANNEL_CAST, text="/lagmark")
                hub.log("lagmark_sent", fps=fps, i=i, client_ts=s.get("ts"))
                await asyncio.sleep(0.45)
            await asyncio.sleep(1.0)
        # p1 view of bot moves at fps2 for a short while (step-cadence sample)
        await asyncio.sleep(args.hold)

        # ---------------- wrap up
        await hub.cmd("c1", "logout", force=True)
        await hub.cmd("p1", "logout", force=False)
        await hub.wait_event("p1", lambda e: e["k"] == "game_end", 15, 0)
        await asyncio.sleep(1)
    finally:
        # g_app.exit() fail-fasts (0xC0000409) in this OTC build, so after the in-game logout
        # the process is simply terminated (we only ever kill PIDs we launched).
        await asyncio.sleep(2)
        for name, p in procs.items():
            if p.poll() is None:
                p.terminate()
            await asyncio.to_thread(stop_client, p, 15)
            hub.log("client_exit", name=name, rc=p.returncode)
        clk_after = await asyncio.to_thread(clock.sample, 40)
        hub.log("clock_offset", phase="after", **clk_after)
        clock.close()
        t_end = epoch_ms()
        # map the run window to server time and pull the journal
        off = clk_before["offset_ms"]
        n = await asyncio.to_thread(pull_server_log, cfg, run_dir, (t_start + off) / 1000 - 5,
                                    (t_end + off) / 1000 + 5)
        hub.log("server_log", lines=n)
        meta = {"run_id": run_id, "t_start_pc_ms": t_start, "t_end_pc_ms": t_end, "fps": args.fps,
                "fps2": args.fps2, "clock_before": clk_before, "clock_after": locals().get("clk_after"),
                "player_char": cfg["player_char"], "server": cfg["server_host"], "game_port": cfg["game_port"],
                "result": result}
        (run_dir / "run.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
        await hub.stop()
        mr = subprocess.run([sys.executable, str(HERE / "merge.py"), str(run_dir)], capture_output=True, text=True,
                            **popen_kw())
        (run_dir / "merge.out").write_text(mr.stdout + mr.stderr, encoding="utf-8")
    return result


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p0 = sub.add_parser("phase0")
    p0.add_argument("--run-id")
    p0.add_argument("--profile", help="harness.json target profile (default: env HARNESS_PROFILE, else the base config)")
    p0.add_argument("--fps", type=int, default=10)
    p0.add_argument("--fps2", type=int, default=60)
    p0.add_argument("--lagmarks", type=int, default=30)
    p0.add_argument("--bot-wait", type=float, default=240)
    p0.add_argument("--settle", type=float, default=20)
    p0.add_argument("--move-wait", type=float, default=90)
    p0.add_argument("--hold", type=float, default=10)
    p0.add_argument("--goto-offsets", type=lambda s: [tuple(int(v) for v in p.split(",")) for p in s.split(";")],
                    default=[(0, 4), (3, 3), (-3, 3), (4, 0), (-4, 0), (0, -4)])
    args = ap.parse_args()
    if args.cmd == "phase0":
        res = asyncio.run(phase0(args))
        print(json.dumps(res, indent=2))
        ok = res["checks"] and all(c["ok"] for c in res["checks"].values())
        sys.exit(0 if ok else 4)


if __name__ == "__main__":
    main()
