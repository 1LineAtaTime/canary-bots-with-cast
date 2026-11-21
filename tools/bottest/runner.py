#!/usr/bin/env python3
"""Run a *.steps scenario with harness OTClient instances against the live server.

  python tools/bottest/runner.py tools/bottest/scenarios/smoke_login_see.steps [--run-id ID] [--label L]

Exit codes: 0 every verb OK and every expect passed; 3 a verb returned STUCK; 4 an expect
FAILed; 2 infrastructure failure (client never connected, ssh down, exception).

Everything lands in runs/<runId>/: run.log (one line per step + verdict, ends with DONE),
.watch (watch.py anomaly stream), result.md ("| Result |" row first), run.json, the steps
file copy, hub.ndjson, cli-<name>.ndjson, server.live.log (live journal tail used by expects),
server.log + server.ndjson + timeline.md + latency.json (merge.py), records/*.cam (login packet
stripped), shots/*.png, view-*.txt, cmd-*.txt.

Format (plan section 4.3):
  # SCENARIO: name   SEED: 1   CAMERAS: none   BOTCFG: auto|off
  @p1 login                       player actors @p<n> -> harness.json "players"
  @p1 goto 32369,32235,7 range=2 stuck=10
  @p1 wait-see any-bot timeout=60 as=B1     captures $B1 (name) and $B1_cid
  @cam:$B1 login                  @cast operator camera on that bot
  @srv cmd "$B1" status expect=state=       in-band via the camera, else bot_commands queue
  @srv expect src=cli:p1 k=see_move bot=$B1 min=1 within=120
  @srv expect join=move cli=p1 bot=any min=1 within=120 window=100
  @srv expect-none src=srv msg~=Lua\\sScript\\sError within=300
Any verb takes optional=1 (a STUCK becomes SKIP). `;` separates several verbs on one line.

Phase 2 server truth: with harness.json "audit_path" set, the engine audit file is tailed live
(src "srv:audit", PC clock) and pulled for the run window at the end (audit.ndjson ->
audit.pc.ndjson). `@srv audit on|off|watch <bot>|unwatch <bot>|kinds <list>|status|mark <text>`
is sugar for `@srv cmd _global audit ...`; expects match audit kinds directly, e.g.
  @srv expect src=srv:audit k=walkpause bot=$B1 tier=observed min=1 within=300
"""
from __future__ import annotations

import argparse
import asyncio
import fnmatch
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import hub as H  # noqa: E402
from winproc import popen_kw  # noqa: E402
import merge  # noqa: E402

REPO = H.REPO
CHANNEL_CAST = H.CHANNEL_CAST
EXIT_OK, EXIT_INFRA, EXIT_STUCK, EXIT_FAIL = 0, 2, 3, 4
# Keys start lowercase; camelCase after that (audit fields like census wakeDeferred60s, #115).
KW_RE = re.compile(r"^([a-z_][A-Za-z0-9_\-]*~?)=(.*)$", re.S)
VAR_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)")
HDR_RE = re.compile(r"([A-Z][A-Z_]+):\s*(.*?)(?=\s{2,}[A-Z][A-Z_]+:|$)")


class Infra(Exception):
    pass


# ------------------------------------------------------------------ parsing
@dataclass
class Step:
    lineno: int
    raw: str
    actor: str
    verb: str
    pos: list = field(default_factory=list)
    kw: dict = field(default_factory=dict)


def subst(s: str, env: dict) -> str:
    def rep(m):
        k = m.group(1) or m.group(2)
        if k not in env:
            raise Infra(f"unknown variable ${k}")
        return str(env[k])
    return VAR_RE.sub(rep, s)


def parse_steps(text: str):
    hdr, steps = {}, []
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s:
            continue
        if s.startswith("#"):
            for k, v in HDR_RE.findall(s.lstrip("#").strip()):
                hdr[k] = v.strip()
            continue
        toks = shlex.split(s, comments=True, posix=True)
        if not toks:
            continue
        if not toks[0].startswith("@"):
            raise Infra(f"line {n}: a step starts with an actor (@srv/@p1/@cam:Bot): {s}")
        actor = toks[0][1:]
        groups, cur = [], []
        for t in toks[1:]:
            if t == ";":
                if cur:
                    groups.append(cur)
                cur = []
            else:
                cur.append(t)
        if cur:
            groups.append(cur)
        for g in groups:
            st = Step(n, s, actor, g[0].lower())
            for t in g[1:]:
                m = KW_RE.match(t)
                if m:
                    st.kw[m.group(1)] = m.group(2)
                else:
                    st.pos.append(t)
            steps.append(st)
    return hdr, steps


def parse_xyz(s: str) -> tuple[int, int, int]:
    p = [int(v) for v in s.replace(" ", "").split(",")]
    if len(p) != 3:
        raise ValueError(s)
    return p[0], p[1], p[2]


def window_state(pid: int) -> list[dict]:
    import ctypes
    from ctypes import wintypes
    u32 = ctypes.windll.user32
    out = []
    proto = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, _):
        p = wintypes.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and not u32.GetWindow(hwnd, 4):  # GW_OWNER: top-level only
            r = wintypes.RECT()
            u32.GetWindowRect(hwnd, ctypes.byref(r))
            buf = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(hwnd, buf, 256)
            if buf.value:
                out.append({"visible": bool(u32.IsWindowVisible(hwnd)), "title": buf.value,
                            "rect": [r.left, r.top, r.right, r.bottom]})
        return True
    u32.EnumWindows(proto(cb), 0)
    return out


def git_info() -> dict:
    def g(*a):
        try:
            return subprocess.run(["git", *a], cwd=REPO, capture_output=True, text=True, timeout=20,
                                  **popen_kw()).stdout.strip()
        except Exception:
            return ""
    dirty = g("status", "--porcelain", "--untracked-files=no")
    return {"sha": g("rev-parse", "HEAD"), "branch": g("rev-parse", "--abbrev-ref", "HEAD"),
            "dirty_tracked_files": len(dirty.splitlines()) if dirty else 0}


# ------------------------------------------------------------------ runner
class Runner:
    def __init__(self, steps_path: Path, args):
        self.args = args
        self.cfg = H.load_config(profile=getattr(args, "profile", None))
        self.sec = H.load_local_secrets()
        self.steps_path = steps_path
        self.text = steps_path.read_text(encoding="utf-8")
        self.hdr, self.steps = parse_steps(self.text)
        if args.client_dir:
            self.cfg["client_dir"] = os.path.abspath(os.path.expanduser(args.client_dir))
        # headless by default: clients run hidden unless --visible or "# VISIBLE: true"
        self.cfg["visible"] = bool(args.visible or self.hdr.get("VISIBLE", "").lower() in ("1", "true", "yes"))
        name = self.hdr.get("SCENARIO") or steps_path.stem
        self.run_id = args.run_id or time.strftime(f"{re.sub(r'[^A-Za-z0-9_]+', '_', name)}_%Y%m%d_%H%M%S")
        self.run_dir = REPO / "runs" / self.run_id
        for sub in ("records", "shots"):
            (self.run_dir / sub).mkdir(parents=True, exist_ok=True)
        shutil.copy2(steps_path, self.run_dir / steps_path.name)
        self.log_fh = open(self.run_dir / "run.log", "a", encoding="utf-8")
        self.vars: dict = {}
        for kv in (getattr(args, "var", None) or []):
            k, eq, v = kv.partition("=")
            if not k or not eq:
                raise Infra(f"--var needs NAME=VALUE, got {kv!r}")
            self.vars[k.strip()] = v
        self.procs: dict[str, subprocess.Popen] = {}
        self.cmeta: dict[str, dict] = {}
        self.actor_client: dict[str, str] = {}
        self.srv_events: list[dict] = []
        self.verdicts: list[dict] = []
        self.expects: list[dict] = []
        self.watch_names: set[str] = set()
        self.notes: list[str] = []
        self.counter = 0
        self.stuck = self.failed = False
        self.infra = None
        self.botcfg = None
        self.stopping = False
        self.jproc = None
        self.aproc = None
        self.audit_pulled = None
        self.t_start = H.epoch_ms()

    # ---------------- logging
    def say(self, text: str):
        line = time.strftime("%H:%M:%S ") + text
        print(line, flush=True)
        self.log_fh.write(line + "\n")
        self.log_fh.flush()

    def write_clients(self):
        (self.run_dir / "clients.json").write_text(json.dumps(self.cmeta, indent=1), encoding="utf-8")

    # ---------------- infrastructure
    def start_watch(self):
        w = self.cfg.get("watch", {})
        cmd = [sys.executable, str(HERE / "watch.py"), str(self.run_dir), "--runner-pid", str(os.getpid()),
               "--ssh-target", self.cfg["ssh_target"], "--unit", str(self.cfg.get("journal_unit") or "canary"),
               "--poll", str(w.get("poll", 10)),
               "--logstall", str(w.get("logstall", 180)), "--heartbeat", str(w.get("heartbeat", 30)),
               "--slow-ms", str(w.get("slow_ms", 500)), "--slow-per-min", str(w.get("slow_per_min", 3)),
               "--max-age-min", str(self.args.watch_max_min or w.get("max_age_min", 120))]
        # NOT DETACHED_PROCESS: a console-less watcher makes every ssh.exe it spawns pop up a new
        # visible console. CREATE_NO_WINDOW = hidden console, inherited by its children.
        err = open(self.run_dir / "watch.stderr.log", "w", encoding="utf-8")
        self.watch_proc = subprocess.Popen(cmd, stdout=err, stderr=err, **popen_kw(new_group=True))
        wf = self.run_dir / ".watch"
        for _ in range(100):
            if wf.exists() and "WATCH-START" in wf.read_text(encoding="utf-8", errors="replace"):
                return
            time.sleep(0.2)
        self.say("NOTE watch.py did not report WATCH-START within 20 s")

    def _audit_thread(self):
        """Live tail of the Phase 2 audit file (tail -F follows the sink's rotation). Records are
        mapped onto the PC clock and appended to srv_events with src "srv:audit", so expects see
        them while the run is still going. The pull in finish() is the authoritative copy."""
        cmd = H.audit_tail_cmd(self.cfg)
        fh = open(self.run_dir / "audit.live.ndjson", "a", encoding="utf-8")
        while not self.stopping and cmd:
            p = subprocess.Popen(H.remote_argv(self.cfg["ssh_target"], cmd, keepalive=True),
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 text=True, encoding="utf-8", errors="replace", bufsize=1, **popen_kw())
            self.aproc = p
            for line in p.stdout:
                fh.write(line)
                fh.flush()
                for r in merge.parse_audit_text(line, self.clock.samples):
                    self.srv_events.append(r)
            if self.stopping:
                break
            self.hub.log("audit_tail_restart")
            time.sleep(2)
        fh.close()

    def _journal_thread(self):
        fh = open(self.run_dir / "server.live.log", "a", encoding="utf-8")
        cmd = f"journalctl -u {H.journal_unit(self.cfg)} -f -n 0 -o short-unix --no-pager"
        while not self.stopping:
            p = subprocess.Popen(H.remote_argv(self.cfg["ssh_target"], cmd, keepalive=True),
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 text=True, encoding="utf-8", errors="replace", bufsize=1, **popen_kw())
            self.jproc = p
            for line in p.stdout:
                fh.write(line)
                fh.flush()
                r = merge.parse_line(line, self.tz_min, self.clock.samples)
                if r:
                    self.srv_events.append(r)
            if self.stopping:
                break
            self.hub.log("journal_tail_restart")
            time.sleep(2)
        fh.close()

    async def ensure_client(self, actor: str) -> str:
        name = self.actor_client.get(actor)
        if name and name in self.procs and self.procs[name].poll() is None and \
                name in self.hub.clients and not self.hub.clients[name].closed:
            return name
        if not name:
            if actor.startswith("cam:"):
                name = f"c{sum(1 for a in self.actor_client if a.startswith('cam:')) + 1}"
            else:
                name = actor
            self.actor_client[actor] = name
        self.hub.clients.pop(name, None)
        p = H.launch_client(self.cfg, name, self.run_dir)
        self.procs[name] = p
        role = "cam" if actor.startswith("cam:") else "player"
        self.cmeta[name] = {"pid": p.pid, "role": role, "actor": actor, "state": "starting", "started": time.time(),
                            "char": actor[4:] if role == "cam" else self.cfg["players"].get(actor, "")}
        self.write_clients()
        self.hub.log("client_launch", name=name, pid=p.pid, actor=actor)
        try:
            await self.hub.wait_hello(name, 90)
        except asyncio.TimeoutError:
            raise Infra(f"client {name} (pid {p.pid}) never connected to the hub")
        self.cmeta[name]["state"] = "up"
        self.write_clients()
        await self.hub.ping_sync(name, 10)
        await self.hub.cmd(name, "set_fps", fps=int(self.cfg.get("fps", 10)))
        return name

    def client_of(self, actor: str) -> str | None:
        n = self.actor_client.get(actor)
        if n and n in self.hub.clients and not self.hub.clients[n].closed:
            return n
        return None

    def alive(self, name: str) -> bool:
        return name in self.procs and self.procs[name].poll() is None and \
            name in self.hub.clients and not self.hub.clients[name].closed

    async def stop_client(self, name: str, logout: bool = True):
        if name not in self.procs:
            return
        if self.procs[name].poll() is not None and self.cmeta[name].get("state") == "up":
            # died on its own (crash / killed): keep it visible to watch.py as CLIENTDOWN
            self.cmeta[name].update(state="lost", rc=self.procs[name].returncode, lost_at=time.time())
            self.write_clients()
            self.hub.log("client_lost", name=name, rc=self.procs[name].returncode)
            return
        if logout and self.alive(name):
            c = self.hub.clients[name]
            pre = next(self.hub.seq)
            await self.hub.cmd(name, "logout", force=self.cmeta[name]["role"] == "cam", timeout=10)
            ev = await self.hub.wait_event(name, lambda e: e["k"] == "game_end", 15, pre)
            if not ev and not c.closed:
                await self.hub.cmd(name, "logout", force=True, timeout=10)
                await self.hub.wait_event(name, lambda e: e["k"] == "game_end", 10, pre)
            await asyncio.sleep(1.0)
        # mark first so the watcher does not report an intended stop as CLIENTDOWN
        self.cmeta[name]["state"] = "stopped"
        self.write_clients()
        p = self.procs[name]
        if p.poll() is None:
            p.terminate()  # g_app.exit() fail-fasts in this OTC build; only PIDs we launched
        await asyncio.to_thread(H.stop_client, p, 15)
        self.hub.log("client_exit", name=name, rc=p.returncode)

    # ---------------- events
    def all_events(self) -> list[dict]:
        evs = []
        for c in list(self.hub.clients.values()):
            evs.extend(c.events)
        evs.extend(self.srv_events)
        return evs

    def name_to_cid(self) -> dict[str, int]:
        m = {}
        for c in list(self.hub.clients.values()):
            m.update(merge.name_cid_map(c.events))
        return m

    # ---------------- step execution
    async def run_step(self, st: Step) -> dict:
        pos = [subst(p, self.vars) for p in st.pos]
        kw = {k: subst(v, self.vars) for k, v in st.kw.items()}
        actor = subst(st.actor, self.vars)
        fn = getattr(self, "v_" + st.verb.replace("-", "_"), None)
        if fn is None:
            return {"ok": False, "kind": "infra", "text": f"unknown verb {st.verb}"}
        return await fn(actor, pos, kw)

    async def player_verb(self, actor, verb, timeout=30, **args) -> dict:
        name = self.client_of(actor)
        if not name:
            return {"ok": False, "text": f"STUCK verb={verb} reason=not-logged-in"}
        return await self.hub.cmd(name, verb, timeout=timeout, **args)

    # --- client verbs
    async def v_login(self, actor, pos, kw):
        name = await self.ensure_client(actor)
        c = self.hub.clients[name]
        if actor.startswith("cam:"):
            char = actor[4:]
            key = "@cast\n" + self.sec["cast_operator_password"]
            # P5 (#160): `desc=@livestream [pw=...]` logs the camera in as an ordinary viewer of
            # upstream's livestream (or `desc=@cast pw=x` as a plain cast viewer) instead.
            if kw.get("desc"):
                key = kw["desc"] + "\n" + kw.get("pw", "")
        else:
            char = kw.get("char") or (pos[0] if pos else self.cfg["players"].get(actor))
            if not char:
                return {"ok": False, "kind": "infra", "text": f"no character configured for {actor}"}
            key = self.sec["account_email"] + "\n" + self.sec["account_password"]
            if kw.get("acct"):   # P5 V11c: another account (disposable DBs only), e.g. acct=@god pw=$GODPW
                key = kw["acct"] + "\n" + kw.get("pw", "")
        self.cmeta[name]["char"] = char
        self.write_clients()
        pre = next(self.hub.seq)
        r = await self.hub.cmd(name, "login", timeout=45, redact=("sessionKey",), host=self.cfg["server_host"],
                               port=self.cfg["game_port"], world=self.cfg["world"], char=char, version=int(self.cfg["client_version"]),
                               sessionKey=key, recordTo=str(self.run_dir / "records" / f"{name}.cam"))
        del key
        if not r.get("ok"):
            return r
        ev = await self.hub.wait_event(name, lambda e: e["k"] in ("login_ok", "login_error", "conn_error"), 45, pre)
        if not ev:
            return {"ok": False, "text": "STUCK verb=login reason=no-answer"}
        if ev["k"] != "login_ok":
            # never retried: 5 wrong operator passwords lock the IP for 15 min
            return {"ok": False, "text": f"STUCK verb=login reason={ev['k']} {str(ev.get('err') or ev.get('msg'))[:120]}"}
        extra = ""
        if actor.startswith("cam:"):
            op = await self.hub.wait_event(name, lambda e: e["k"] == "see_talk" and "Operator session" in str(e.get("text")),
                                           10, pre)
            self.cmeta[name]["operator"] = op is not None
            self.write_clients()
            extra = f" operator={'yes' if op else 'no'}"
            # config hash in-band while a camera exists (no bot_commands row on production)
            if op and not self.botcfg and self.hdr.get("BOTCFG", "auto") != "off" and not self.args.no_botcfg:
                await self.take_botcfg("cam")
                if self.botcfg:
                    extra += f" botcfg={self.botcfg['sha256'][:12]}"
        else:
            sp = await self.hub.wait_event(name, lambda e: e["k"] == "self_pos" and e.get("to"), 10, ev["seq"])
            if sp:
                ev = dict(ev, pos=sp["to"])
        p = ev.get("pos") or {}
        # every harness client must be silent (overlay + harness_ctl audioOff); record the proof
        cs = await self.hub.cmd(name, "client_state", timeout=10)
        au = (cs.get("audio") or {}) if cs.get("ok") else {}
        self.cmeta[name]["audio_enabled"] = au.get("audio_enabled")
        self.write_clients()
        extra += f" audio_enabled={au.get('audio_enabled')}"
        self.vars[f"{actor.replace(':', '_').replace(' ', '_')}_cid"] = ev.get("cid")
        return {"ok": True, "text": f"OK login char={char} cid={ev.get('cid')} pos={p.get('x')},{p.get('y')},{p.get('z')}{extra}"}

    async def v_logout(self, actor, pos, kw):
        name = self.actor_client.get(actor)
        if not name:
            return {"ok": True, "text": "OK logout (never logged in)"}
        await self.stop_client(name)
        return {"ok": True, "text": f"OK logout client={name}"}

    async def v_goto(self, actor, pos, kw):
        try:
            x, y, z = parse_xyz(pos[0])
        except (IndexError, ValueError):
            return {"ok": False, "kind": "infra", "text": "goto needs x,y,z"}
        to = float(kw.get("timeout", 120))
        return await self.player_verb(actor, "goto", timeout=to + 15, x=x, y=y, z=z, range=int(kw.get("range", 0)),
                                      timeout_s=to, stuck=float(kw.get("stuck", 10)))

    async def v_walk(self, actor, pos, kw):
        n = int(pos[1]) if len(pos) > 1 else int(kw.get("n", 1))
        return await self.player_verb(actor, "walk", timeout=n * 2 + 10, dir=pos[0] if pos else kw.get("dir"), n=n)

    async def v_face(self, actor, pos, kw):
        return await self.player_verb(actor, "face", dir=pos[0] if pos else kw.get("dir"))

    async def v_stop(self, actor, pos, kw):
        return await self.player_verb(actor, "stop")

    async def v_follow(self, actor, pos, kw):
        return await self.player_verb(actor, "follow", name=" ".join(pos))

    async def v_say(self, actor, pos, kw):
        text = " ".join(pos) or kw.get("text", "")
        if actor.startswith("cam:"):
            return await self.player_verb(actor, "channel_say", channel=CHANNEL_CAST, text=text)
        if kw.get("npc") in ("1", "true", "yes"):   # NPC channel (after a greeting), like the real client
            return await self.player_verb(actor, "say", text=text, npc=True)
        return await self.player_verb(actor, "say", text=text)

    async def v_pm(self, actor, pos, kw):
        if len(pos) < 2:
            return {"ok": False, "kind": "infra", "text": "pm needs <name> <text>"}
        return await self.player_verb(actor, "pm", name=pos[0], text=" ".join(pos[1:]))

    async def v_attack(self, actor, pos, kw):
        """attack <name> | attack stop (safe-fight off, chase on)."""
        return await self.player_verb(actor, "attack", name=" ".join(pos))

    async def v_party(self, actor, pos, kw):
        """party invite <name> | party leave | party say <text> (party channel, #91 verbs)."""
        if not pos:
            return {"ok": False, "kind": "infra", "text": "party needs invite <name> | leave | say <text>"}
        if pos[0] == "say":
            return await self.player_verb(actor, "party", op="say", text=" ".join(pos[1:]))
        return await self.player_verb(actor, "party", op=pos[0], name=" ".join(pos[1:]))

    async def v_botpanel(self, actor, pos, kw):
        """botpanel open|close|dump|select <name>|click <action> [expect=re] [timeout=S] (#26 panel,
        modules.game_botcontrol in the harness client). With expect=, `dump` is retried every 0.5 s
        until the panel state matches or timeout (default 10 s)."""
        op = pos[0] if pos else "dump"
        arg = " ".join(pos[1:])
        r = await self.player_verb(actor, "botpanel", op=op, arg=arg)
        want = kw.get("expect")
        if not want or not r.get("ok"):
            return r
        deadline = time.time() + float(kw.get("timeout", 10))
        while not re.search(want, r.get("text", "")):
            if time.time() > deadline:
                return {"ok": False, "kind": "fail", "text": f'FAIL check=botpanel~{want} got="{r.get("text", "")[:400]}"'}
            await asyncio.sleep(0.5)
            r = await self.player_verb(actor, "botpanel", op="dump", arg="")
            if not r.get("ok"):
                return r
        return r

    async def v_trade(self, actor, pos, kw):
        """trade <name> [slot=1] | trade cancel. Server answer = client events trade_own / trade_close."""
        return await self.player_verb(actor, "trade", name=" ".join(pos), slot=int(kw.get("slot", 1)))

    async def v_wait_see(self, actor, pos, kw):
        target = pos[0] if pos else "any-bot"
        args = {"timeout_s": float(kw.get("timeout", 30))}
        if target.startswith("any-"):
            args["any"] = target[4:]
        else:
            args["name"] = target
        if kw.get("moving") in ("1", "true", "yes"):
            args["moving"] = True
        if kw.get("not"):
            args["exclude"] = [n.strip() for n in kw["not"].split(",") if n.strip()]
        r = await self.player_verb(actor, "wait_see", timeout=args["timeout_s"] + 10, **args)
        if r.get("ok") and kw.get("as"):
            self.vars[kw["as"]] = r.get("name")
            self.vars[kw["as"] + "_cid"] = r.get("cid")
            r = dict(r, text=r["text"] + f" as=${kw['as']}")
        return r

    async def v_wait_gone(self, actor, pos, kw):
        to = float(kw.get("timeout", 30))
        return await self.player_verb(actor, "wait_gone", timeout=to + 10, name=" ".join(pos), timeout_s=to)

    async def v_hold(self, actor, pos, kw):
        secs = float(pos[0]) if pos else float(kw.get("secs", 10))
        name = self.client_of(actor)
        if not name:
            return {"ok": False, "text": "STUCK verb=hold reason=not-logged-in"}
        watch = [n.strip() for n in kw.get("watch", "").split(",") if n.strip()]
        self.watch_names.update(watch)
        t0 = H.epoch_ms()
        end = time.monotonic() + secs
        while time.monotonic() < end:
            if not self.alive(name):
                return {"ok": False, "text": f"STUCK verb=hold reason=client-down client={name} after={int((H.epoch_ms() - t0) / 1000)}s"}
            await asyncio.sleep(min(1.0, max(0.0, end - time.monotonic())))
        evs = [e for e in self.hub.clients[name].events if e["ts"] >= t0]
        n2c = merge.name_cid_map(self.hub.clients[name].events)
        parts = []
        for w in watch:
            cid = n2c.get(w)
            mv = sum(1 for e in evs if e["k"] == "see_move" and (e.get("name") == w or (cid and e.get("cid") == cid)))
            parts.append(f"{w.replace(' ', '_')}:moves={mv}")
        allmv = sum(1 for e in evs if e["k"] == "see_move")
        return {"ok": True, "text": f"OK hold secs={secs:g} see_move={allmv} " + " ".join(parts)}

    async def v_dump_view(self, actor, pos, kw):
        r = await self.player_verb(actor, "dump_view", multifloor=kw.get("multifloor") == "1")
        if r.get("ok"):
            self.counter += 1
            f = self.run_dir / f"view-{self.counter}.txt"
            o = r.get("origin") or {}
            lines = [f"# {actor} {r.get('text')}  origin(top-left)={o.get('x')},{o.get('y')},{o.get('z')}",
                     "# legend: @ self  B bot  P player  M monster  N npc  C creature  . walkable  # blocked  = floor change"]
            lines += r.get("ascii") or []
            lines.append("# creatures:")
            for c in r.get("creatures") or []:
                p = c.get("pos") or {}
                lines.append(f"  {c.get('ctype'):8} {'bot' if c.get('bot') else '   '} cid={c.get('cid')} "
                             f"{p.get('x')},{p.get('y')},{p.get('z')}  {c.get('name')}")
            f.write_text("\n".join(lines) + "\n", encoding="utf-8")
            r = dict(r, text=r["text"] + f" file={f.name}")
        return r

    async def v_shot(self, actor, pos, kw):
        self.counter += 1
        fname = f"harness_{self.counter}_{int(time.time())}.png"
        args = {"file": fname, "mode": kw.get("mode", "window")}
        if kw.get("reveal") in ("1", "true", "yes"):
            args["reveal"] = True
        r = await self.player_verb(actor, "shot", timeout=20, **args)
        if not r.get("ok"):
            return r
        wd = r.get("writeDir") or self.hub.clients[self.client_of(actor)].write_dir
        src = Path(wd) / fname if wd else None
        dst = self.run_dir / "shots" / f"{(pos[0] if pos else actor.replace(':', '_'))}_{self.counter}.png"
        for _ in range(50):
            if src and src.exists() and src.stat().st_size > 0:
                await asyncio.sleep(0.3)
                shutil.move(str(src), dst)
                try:
                    ps = H.png_stats(dst)
                except Exception as e:
                    ps = {"err": str(e)}
                black = ps.get("nonzero_rgb", 1) < 0.01
                return {"ok": True, "text": f"OK shot file=shots/{dst.name} bytes={dst.stat().st_size} "
                                            f"{ps.get('w')}x{ps.get('h')} nonzero_rgb={ps.get('nonzero_rgb')}"
                                            f"{' BLACK' if black else ''} mode={args['mode']}"
                                            f"{' revealed' if args.get('reveal') else ''} hidden={r.get('hidden')}"}
            await asyncio.sleep(0.1)
        return {"ok": False, "text": f"STUCK verb=shot reason=no-file write_dir={wd}"}

    async def v_lagmark(self, actor, pos, kw):
        """Camera-only: N x /lagmark in the cast channel; merge.py pairs each server stamp with its
        send (+-50 ms) and reports up/down/rtt in latency.json."""
        name = self.client_of(actor)
        if not name or not actor.startswith("cam:"):
            return {"ok": False, "text": "STUCK verb=lagmark reason=needs-logged-in-camera"}
        n = int(pos[0]) if pos else int(kw.get("n", 20))
        pre = next(self.hub.seq)
        for i in range(n):
            s = await self.hub.cmd(name, "channel_say", channel=CHANNEL_CAST, text="/lagmark")
            self.hub.log("lagmark_sent", i=i, client_ts=s.get("ts"), client=name)
            await asyncio.sleep(float(kw.get("gap", 0.45)))
        await asyncio.sleep(1.5)
        sends = [h for h in self._hub_records() if h.get("k") == "lagmark_sent" and h.get("seq", 0) > pre]
        reps = [e for e in self.hub.clients[name].events if e["seq"] > pre and e["k"] == "see_talk"
                and str(e.get("text", "")).startswith("lagmark @ ")]
        rtts = []
        for e in reps:
            t_srv = int(e["text"].split("@", 1)[1].strip())
            t_pc = t_srv - self.clock.offset_at(t_srv)
            c = [s for s in sends if s.get("client_ts") and abs(t_pc - s["client_ts"]) <= merge.LAGMARK_WINDOW_MS]
            if c:
                s = min(c, key=lambda s: abs(t_pc - s["client_ts"]))
                rtts.append(e["ts"] - s["client_ts"])
        st = merge.summary(rtts)
        return {"ok": bool(rtts), "text": (f"OK lagmark n={n} replies={len(reps)} paired={len(rtts)} "
                                           f"rtt_p50={st.get('p50')} p95={st.get('p95')} max={st.get('max')}")
                if rtts else f"STUCK verb=lagmark reason=no-replies replies={len(reps)}"}

    def _hub_records(self) -> list[dict]:
        p = self.run_dir / "hub.ndjson"
        return [json.loads(l) for l in p.read_text(encoding="utf-8").splitlines() if l.strip()]

    async def v_window_state(self, actor, pos, kw):
        """Win32 truth about the client's top-level windows (visible flag + rect), so 'hidden' is
        measured from outside the client instead of trusted from its own Lua."""
        name = self.client_of(actor)
        if not name or os.name != "nt":
            return {"ok": False, "text": "STUCK verb=window-state reason=not-logged-in"}
        wins = window_state(self.procs[name].pid)
        vis = [w for w in wins if w["visible"]]
        txt = (f"OK window-state pid={self.procs[name].pid} windows={len(wins)} visible={len(vis)}"
               + "".join(f" [{w['title'][:30]!r} rect={w['rect']}]" for w in vis[:3]))
        want = kw.get("expect")
        if want == "hidden" and vis:
            return {"ok": False, "kind": "fail", "text": "FAIL check=window-state=hidden got=" + txt[3:]}
        if want == "visible" and not vis:
            return {"ok": False, "kind": "fail", "text": "FAIL check=window-state=visible got=" + txt[3:]}
        return {"ok": True, "text": txt}

    async def v_set_fps(self, actor, pos, kw):
        return await self.player_verb(actor, "set_fps", fps=int(pos[0]))

    async def v_sleep(self, actor, pos, kw):
        await asyncio.sleep(float(pos[0]))
        return {"ok": True, "text": f"OK sleep {pos[0]}"}

    async def v_note(self, actor, pos, kw):
        self.notes.append(" ".join(pos))
        return {"ok": True, "text": "OK note " + " ".join(pos)}

    # --- perception verbs (perceive.lua)
    @staticmethod
    def _onoff(v):
        return None if v is None else str(v).lower() in ("1", "on", "true", "yes")

    async def v_view(self, actor, pos, kw):
        """Full snapshot of what the client perceives: every creature (all known floors) with
        hp/skull/shield/emblem/outfit/dir/speed/walking, the own UI state, and the effects /
        missiles / texts tapped in the last `recent=` ms. Written to view-N.json."""
        args = {}
        if kw.get("recent"):
            args["recent_ms"] = int(kw["recent"])
        if kw.get("multifloor") == "0":
            args["multifloor"] = False
        r = await self.player_verb(actor, "view", **args)
        if r.get("ok"):
            self.counter += 1
            f = self.run_dir / f"view-{self.counter}.json"
            f.write_text(json.dumps({k: r.get(k) for k in ("text", "pos", "creatures", "recent", "self", "containers",
                                                           "perceive")}, indent=1), encoding="utf-8")
            r = dict(r, text=r["text"] + f" file={f.name}")
            if kw.get("expect") and not re.search(kw["expect"], r["text"]):
                return {"ok": False, "kind": "fail", "text": f'FAIL check=view~{kw["expect"]} got="{r["text"]}"'}
        return r

    async def v_self(self, actor, pos, kw):
        """Own character UI state (stats, skills, conditions, inventory, containers, cooldowns,
        target/follow, modes) -> self-N.json; `expect=<regex>` is checked against the reply line,
        `as=S` sets $S_<field> (e.g. $S_mp) for later steps."""
        r = await self.player_verb(actor, "self")
        if r.get("ok"):
            self.counter += 1
            f = self.run_dir / f"self-{self.counter}.json"
            f.write_text(json.dumps({k: r.get(k) for k in ("text", "pos", "self", "inventory", "containers",
                                                           "cooldowns")}, indent=1), encoding="utf-8")
            if kw.get("as"):
                for k, v in (r.get("self") or {}).items():
                    self.vars[f"{kw['as']}_{k}"] = v
            r = dict(r, text=r["text"] + f" file={f.name}")
            if kw.get("expect") and not re.search(kw["expect"], r["text"]):
                return {"ok": False, "kind": "fail", "text": f'FAIL check=self~{kw["expect"]} got="{r["text"][:300]}"'}
        return r

    async def v_perceive(self, actor, pos, kw):
        """Configure the perception layer and read its counters:
        `perceive [poll=100] [self_poll=500] [recent=3000] [tap=on|off] [creatures=on|off] [self=on|off] [reset=1]`"""
        args = {}
        for k, a in (("poll", "poll_ms"), ("self_poll", "self_poll_ms"), ("recent", "recent_ms")):
            if kw.get(k):
                args[a] = int(kw[k])
        for k in ("tap", "creatures", "self", "reset"):
            if kw.get(k) is not None:
                args[k] = self._onoff(kw[k])
        r = await self.player_verb(actor, "perceive", **args)
        if r.get("ok") and kw.get("expect") and not re.search(kw["expect"], r["text"]):
            return {"ok": False, "kind": "fail", "text": f'FAIL check=perceive~{kw["expect"]} got="{r["text"]}"'}
        return r

    async def v_use(self, actor, pos, kw):
        """`use <itemId> [with=self|<creature name>|none]` - hotkey-style use of an item the
        character carries (the server finds it in inventory / containers)."""
        if not pos:
            return {"ok": False, "kind": "infra", "text": "use needs <itemId>"}
        return await self.player_verb(actor, "use", id=int(pos[0]), **({"with": kw["with"]} if kw.get("with") else {}))

    async def v_uilogin(self, actor, pos, kw):
        """P5 V11c: `uilogin acct=<email|@cast|@livestream> pw=<pw> [url=http://127.0.0.1:8080/login.php]`
        - the REAL OTClient login UI path (EnterGame HTTP login -> character list), not loginWorld.
        `expect=<regex>` is checked against the reply (the listed names)."""
        name = await self.ensure_client(actor)
        url = kw.get("url", self.cfg.get("login_url", "http://127.0.0.1:8080/login.php"))
        r = await self.hub.cmd(name, "uilogin", timeout=45, redact=("password",), account=kw.get("acct", ""),
                               password=kw.get("pw", ""), url=url, port=int(kw.get("port", 8080)),
                               version=int(self.cfg["client_version"]))
        listed = r.get("text", "") + " " + " | ".join(r.get("names") or [])   # match the FULL list, not the 12 in the text
        if r.get("ok") and kw.get("expect") and not re.search(kw["expect"], listed):
            return {"ok": False, "kind": "fail", "text": f'FAIL check=uilogin~{kw["expect"]} got="{r["text"][:300]}"'}
        return r

    async def v_uichar(self, actor, pos, kw):
        """P5 V11c: `uichar <name>` - pick a character in the open character list and enter the game."""
        char = kw.get("name") or " ".join(p for p in pos if "=" not in p)
        name = self.client_of(actor) or await self.ensure_client(actor)
        pre = next(self.hub.seq)
        r = await self.hub.cmd(name, "uichar", timeout=20, name=char)
        if not r.get("ok") or kw.get("nowait") in ("1", "true", "yes"):
            return r
        ev = await self.hub.wait_event(name, lambda e: e["k"] in ("login_ok", "login_error", "conn_error"), 45, pre)
        if not ev or ev["k"] != "login_ok":
            return {"ok": False, "text": f"STUCK verb=uichar reason={ev['k'] if ev else 'no-answer'} "
                                         f"{str((ev or {}).get('err') or (ev or {}).get('msg'))[:120]}"}
        self.cmeta[name]["char"] = char
        self.write_clients()
        return {"ok": True, "text": f"OK uichar char={char} cid={ev.get('cid')} pos={ev.get('pos')}"}

    async def v_uicheck(self, actor, pos, kw):
        """P5 V11c: `uicheck [expect=re]` - game panels loaded/visible, chat tabs, npc trade window."""
        r = await self.player_verb(actor, "uicheck")
        if r.get("ok") and kw.get("expect") and not re.search(kw["expect"], r["text"]):
            return {"ok": False, "kind": "fail", "text": f'FAIL check=uicheck~{kw["expect"]} got="{r["text"][:300]}"'}
        return r

    async def v_client_state(self, actor, pos, kw):
        """`client-state [expect=re]` - audio (g_sounds + channels + persisted options), hidden, fps."""
        r = await self.player_verb(actor, "client_state")
        if r.get("ok") and kw.get("expect") and not re.search(kw["expect"], r["text"]):
            return {"ok": False, "kind": "fail", "text": f'FAIL check=client-state~{kw["expect"]} got="{r["text"]}"'}
        return r

    async def v_open(self, actor, pos, kw):
        """`open [slot]` - open the container in an inventory slot (default back = backpack)."""
        return await self.player_verb(actor, "open", slot=pos[0] if pos else kw.get("slot", "back"))

    async def v_walk_check(self, actor, pos, kw):
        """`walk-check [within=S] [end=x,y,z] [end-range=0] [max-revisits=N] [max-osc=N]
        [max-steps=N] [min-steps=N] [z-seq=7,6]` - the shape of the walk this client's OWN
        character made, from its `self_pos` stream (on a camera that is the watched bot, i.e. the
        bot's own view). A jump of Chebyshev > 1 (teleport) starts a new walk; the LAST walk is
        checked. revisits = steps onto a tile already stood on in that walk (back-and-forth),
        osc = A->B->A reversals, z-seq = the floors in order. Writes walk-N.json."""
        name = self.actor_client.get(actor, actor)
        c = self.hub.clients.get(name)
        if not c:
            return {"ok": False, "text": f"STUCK verb=walk-check reason=no-client actor={actor}"}
        t_lo = H.epoch_ms() - float(kw["within"]) * 1000 if "within" in kw else self.t_start
        evs = [e for e in c.events if e.get("k") == "self_pos" and e.get("ts", 0) >= t_lo and e.get("to")]
        walks, cur = [], []
        for e in evs:
            p = (int(e["to"]["x"]), int(e["to"]["y"]), int(e["to"]["z"]))
            if cur and max(abs(p[0] - cur[-1][0]), abs(p[1] - cur[-1][1])) > 1:
                walks.append(cur)
                cur = []
            if not cur and e.get("from"):
                f = (int(e["from"]["x"]), int(e["from"]["y"]), int(e["from"]["z"]))
                if max(abs(p[0] - f[0]), abs(p[1] - f[1])) <= 1:
                    cur.append(f)
            cur.append(p)
        if cur:
            walks.append(cur)
        w = walks[-1] if walks else []
        seen, revisits, osc = set(), 0, 0
        for i, p in enumerate(w):
            if p in seen:
                revisits += 1
            seen.add(p)
            if i >= 2 and w[i - 2] == p and w[i - 1] != p:
                osc += 1
        zs = []
        for p in w:
            if not zs or zs[-1] != p[2]:
                zs.append(p[2])
        steps = max(0, len(w) - 1)
        last = w[-1] if w else None
        self.counter += 1
        f = self.run_dir / f"walk-{self.counter}.json"
        f.write_text(json.dumps({"client": name, "walks": len(walks), "tiles": w, "revisits": revisits, "osc": osc,
                                 "z_seq": zs}, indent=1), encoding="utf-8")
        lp = f"{last[0]},{last[1]},{last[2]}" if last else "?"
        text = (f"walk-check client={name} walks={len(walks)} steps={steps} revisits={revisits} osc={osc} "
                f"z-seq={','.join(map(str, zs)) or '-'} start={','.join(map(str, w[0])) if w else '?'} end={lp} "
                f"file={f.name}")
        bad = []
        if "end" in kw:
            ex, ey, ez = parse_xyz(kw["end"])
            rng = int(kw.get("end-range", 0))
            if not last or last[2] != ez or max(abs(last[0] - ex), abs(last[1] - ey)) > rng:
                bad.append(f"end={kw['end']}~{rng}")
        for k, v in (("max-revisits", revisits), ("max-osc", osc), ("max-steps", steps)):
            if k in kw and v > int(kw[k]):
                bad.append(f"{k}={kw[k]}")
        if "min-steps" in kw and steps < int(kw["min-steps"]):
            bad.append(f"min-steps={kw['min-steps']}")
        if "z-seq" in kw and ",".join(map(str, zs)) != kw["z-seq"].replace(" ", ""):
            bad.append(f"z-seq={kw['z-seq']}")
        self.expects.append({"check": "walk-check " + " ".join(f"{k}={v}" for k, v in kw.items()),
                             "got": f"steps={steps} revisits={revisits} osc={osc} z={','.join(map(str, zs))} end={lp}",
                             "want": " ".join(bad) or "all", "ok": not bad})
        if bad:
            return {"ok": False, "kind": "fail", "text": f"FAIL check=walk-check {' '.join(bad)} got: {text}"}
        return {"ok": True, "text": "OK " + text}

    async def v_find_bot(self, actor, pos, kw):
        """`@srv find-bot [since=900] [not=a,b] [as=VAR]` - read-only journal scan for a bot whose
        LATEST hunt-phase transition entered PATROLLING (phase x->2) and that has not logged
        [HUNT:STUCK] since; picks the most recent such entry. Sets $VAR and $VAR_guid."""
        since = int(kw.get("since", 900))
        excl = {n.strip() for n in kw.get("not", "").split(",") if n.strip()}
        cmd = (f"journalctl -u {H.journal_unit(self.cfg)} --since '{since} seconds ago' -o cat --no-pager "
               "| grep -F '[HUNT' | tail -n 20000")
        out = await asyncio.to_thread(subprocess.run, H.remote_argv(self.cfg["ssh_target"], cmd),
                                      capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
                                      **popen_kw())
        phase_re = re.compile(r"guid=(\d+) name='([^']+)' phase (\d+)->(\d+)")
        stuck_re = re.compile(r"\[HUNT:STUCK\] bot guid=(\d+) name='([^']+)'")
        last = {}
        for i, line in enumerate(out.stdout.splitlines()):
            m = phase_re.search(line)
            if m:
                last[m.group(2)] = {"guid": int(m.group(1)), "to": int(m.group(4)), "i": i, "line": line.strip()}
                continue
            m = stuck_re.search(line)
            if m and m.group(2) in last:
                last[m.group(2)]["stuck"] = True
        cands = [(v["i"], n, v) for n, v in last.items() if v["to"] == 2 and not v.get("stuck") and n not in excl]
        if not cands:
            return {"ok": False, "text": f"STUCK verb=find-bot reason=none-patrolling since={since}s seen={len(last)}"}
        _, name, v = max(cands)
        if kw.get("as"):
            self.vars[kw["as"]] = name
            self.vars[kw["as"] + "_guid"] = v["guid"]
        self.watch_names.add(name)
        return {"ok": True, "text": f"OK find-bot name={name} guid={v['guid']} candidates={len(cands)} "
                                    f"last=\"{v['line'][-120:]}\"" + (f" as=${kw['as']}" if kw.get("as") else "")}

    # --- server verbs
    def cam_for(self, bot: str) -> str | None:
        n = self.client_of("cam:" + bot)
        if n and self.cmeta.get(n, {}).get("operator"):
            return n
        for actor, n in self.actor_client.items():
            if actor.startswith("cam:") and self.client_of(actor) and self.cmeta.get(n, {}).get("operator"):
                return n
        return None

    async def srv_command(self, bot: str, sub: str, via: str = "auto", timeout: float = 30) -> tuple[str, str]:
        cam = self.cam_for(bot) if via in ("auto", "cam") else None
        if via == "cam" and not cam:
            raise Infra("via=cam but no operator camera is connected")
        if cam:
            text = f"/cavebot {sub}" if bot == "_global" else f'/cavebot "{bot}" {sub}'
            pre = next(self.hub.seq)
            said = await self.hub.cmd(cam, "channel_say", channel=CHANNEL_CAST, text=text)
            c = self.hub.clients[cam]
            first = await self.hub.wait_event(cam, lambda e: e["k"] == "see_msg", min(timeout, 15), pre)
            if not first:
                return "cam:" + cam, ""
            if said.get("ts"):
                self.last_cmd_ms = round(first["ts"] - said["ts"], 1)
            # collect until 700 ms of quiet (multi-line replies arrive as one text message per line)
            last_n = -1
            while True:
                got = [e for e in c.events if e["seq"] > pre and e["k"] == "see_msg"]
                if len(got) == last_n:
                    break
                last_n = len(got)
                await asyncio.sleep(0.7)
            return "cam:" + cam, "\n".join(str(e.get("text", "")) for e in got)
        sys.path.insert(0, str(REPO / "tools" / "botperf"))
        from driver import BotCommandDriver, DbConfig  # queue protocol only
        s = H.load_db_secrets()
        drv = BotCommandDriver(DbConfig(host=s["DB_HOST"], user=s["DB_USER"], password=s["DB_PASS"],
                                        database=s.get("DB_NAME", "canary"), port=s.get("DB_PORT", "3306"),
                                        mysql_bin=H.MYSQL), timeout=timeout)
        out = await asyncio.to_thread(drv.send, bot, sub, timeout)
        return "queue", out

    async def v_cmd(self, actor, pos, kw):
        if len(pos) < 2:
            return {"ok": False, "kind": "infra", "text": 'cmd needs "<bot>" <subcommand>'}
        bot, sub = pos[0], " ".join(pos[1:])
        self.last_cmd_ms = None
        try:
            via, out = await self.srv_command(bot, sub, kw.get("via", "auto"), float(kw.get("timeout", 30)))
        except Infra:
            raise
        except Exception as e:
            return {"ok": False, "text": f"STUCK verb=cmd reason=channel-error {str(e)[:160]}"}
        self.counter += 1
        f = self.run_dir / f"cmd-{self.counter}.txt"
        f.write_text(f"# {bot} {sub} via={via}\n{out}\n", encoding="utf-8")
        if not out.strip():
            return {"ok": False, "text": f"STUCK verb=cmd reason=no-reply via={via}"}
        first = out.strip().splitlines()[0][:160]
        if kw.get("as"):
            self.vars[kw["as"]] = out.strip()
        if kw.get("capture"):  # capture=NAME:regex-with-one-group -> $NAME (e.g. a position in the reply)
            cname, _, crx = kw["capture"].partition(":")
            m = re.search(crx, out)
            if not m:
                return {"ok": False, "kind": "fail", "text": f'FAIL check=cmd-capture~{crx} got="{first}" file={f.name}'}
            self.vars[cname] = m.group(1)
        if kw.get("expect") and not re.search(kw["expect"], out):
            return {"ok": False, "kind": "fail",
                    "text": f'FAIL check=cmd-reply~{kw["expect"]} got="{first}" via={via} file={f.name}'}
        ms = f" reply_ms={self.last_cmd_ms}" if self.last_cmd_ms is not None else ""
        return {"ok": True, "text": f'OK cmd via={via}{ms} lines={len(out.strip().splitlines())} first="{first}" file={f.name}'}

    async def v_audit(self, actor, pos, kw):
        """`@srv audit <sub...>` = `@srv cmd _global audit <sub...>` (Phase 2 server audit toggles).
        `watch <bot>` also adds the bot to the run's watch_names (timeline + result.md)."""
        if not pos:
            pos = ["status"]
        if pos[0].lower() == "watch" and len(pos) > 1 and pos[1].lower() not in ("clear", "none", "*"):
            self.watch_names.add(" ".join(pos[1:]))
        return await self.v_cmd(actor, ["_global", "audit", *pos], kw)

    async def v_db(self, actor, pos, kw):
        """`@srv db "<SELECT|SHOW ...>" [expect=re] [none=re] [wait=S] [as=VAR]` (P5 #160): read-only
        query on the profile's DB (db_name / db_host). Rows are tab-joined, one per line; expect=
        must match, none= must not, retried for up to wait= seconds."""
        if not pos:
            return {"ok": False, "kind": "infra", "text": 'db needs "<SELECT ...>"'}
        sql = " ".join(pos)
        if not re.match(r"\s*(select|show)\b", sql, re.I) or ";" in sql.rstrip(" ;"):
            return {"ok": False, "kind": "infra", "text": "db: one SELECT/SHOW statement only"}
        rx, nrx = kw.get("expect"), kw.get("none")
        deadline = time.monotonic() + float(kw.get("wait", 0))
        while True:
            try:
                rows = await asyncio.to_thread(H.db_query, sql)
            except Exception as e:
                return {"ok": False, "text": f"STUCK verb=db reason=db-error {str(e)[:160]}"}
            text = "\n".join("\t".join(r) for r in rows)
            ok = (not rx or re.search(rx, text) is not None) and (not nrx or re.search(nrx, text) is None)
            if ok or time.monotonic() >= deadline:
                break
            await asyncio.sleep(1.0)
        self.counter += 1
        f = self.run_dir / f"db-{self.counter}.txt"
        f.write_text(f"# {sql}\n{text}\n", encoding="utf-8")
        if kw.get("as"):
            self.vars[kw["as"]] = rows[0][0] if rows and rows[0] else ""
        first = (text.splitlines() or [""])[0][:160]
        if not ok:
            return {"ok": False, "kind": "fail",
                    "text": f'FAIL check=db expect={rx!r} none={nrx!r} rows={len(rows)} got="{first}" file={f.name}'}
        return {"ok": True, "text": f'OK db rows={len(rows)} first="{first}" file={f.name}'}

    async def v_botcfg(self, actor, pos, kw):
        await self.take_botcfg(kw.get("via", "auto"))
        if not self.botcfg:
            return {"ok": False, "text": "STUCK verb=botcfg reason=no-reply"}
        return {"ok": True, "text": f"OK botcfg sha256={self.botcfg['sha256'][:16]} lines={self.botcfg['lines']} via={self.botcfg['via']}"}

    async def take_botcfg(self, via="auto"):
        try:
            v, out = await self.srv_command("_global", "botcfg", via, 30)
        except Exception as e:
            self.notes.append(f"botcfg unavailable: {e}")
            return
        lines = [l.rstrip() for l in out.splitlines() if l.strip()]
        if not lines:
            self.notes.append(f"botcfg: empty reply via {v}")
            return
        norm = "\n".join(lines)
        (self.run_dir / "botcfg.txt").write_text(norm + "\n", encoding="utf-8")
        self.botcfg = {"sha256": hashlib.sha256(norm.encode()).hexdigest(), "lines": len(lines), "via": v}

    # --- expects
    def parse_filters(self, kw: dict):
        skip = {"min", "max", "within", "wait", "join", "window", "cli", "pos", "settle", "optional"}
        fl = []
        for k, v in kw.items():
            if k in skip:
                continue
            if k.endswith("~"):
                fl.append((k[:-1], "~", v))
            else:
                fl.append((k, "=", v))
        return fl

    def ev_match(self, e: dict, fl, n2c: dict) -> bool:
        for key, op, val in fl:
            if key == "src":
                if not fnmatch.fnmatchcase(str(e.get("src", "")), val):
                    return False
                continue
            if key == "bot":
                cid = e.get("cid") or n2c.get(e.get("name"))
                if val in ("any", "any-bot"):
                    if not cid or not H.bot_cid_range(int(cid)):
                        return False
                else:
                    want = n2c.get(val)
                    if not (e.get("name") == val or (want and cid == want)):
                        return False
                continue
            got = e.get(key)
            if got is None and key == "text":
                got = e.get("msg")
            if op == "~":
                if got is None or not re.search(val, str(got)):
                    return False
            elif str(got) != val:
                return False
        return True

    def count_expect(self, kw: dict, t_lo: float) -> int:
        n2c = self.name_to_cid()
        if kw.get("join") == "move":
            cli = kw.get("cli", "p1")
            name = self.actor_client.get(cli, cli)
            c = self.hub.clients.get(name)
            if not c:
                return 0
            evs = [e for e in c.events if e["ts"] >= t_lo]
            bot = kw.get("bot", "any")
            if bot in ("any", "any-bot"):
                evs = [e for e in evs if H.bot_cid_range(int(e.get("cid", 0) or 0))]
                cids = None
            else:
                cids = {n2c[bot]} if bot in n2c else set()
            js = merge.join_moves(evs, [r for r in self.srv_events if r["ts"] >= t_lo - 1000],
                                  float(kw.get("window", merge.JOURNAL_WINDOW_MS)), cids, kw.get("pos") == "1")
            self.last_join = js
            return len(js)
        fl = self.parse_filters(kw)
        return sum(1 for e in self.all_events() if e.get("ts", 0) >= t_lo and self.ev_match(e, fl, n2c))

    async def v_expect(self, actor, pos, kw, none=False):
        now = H.epoch_ms()
        t_lo = now - float(kw["within"]) * 1000 if "within" in kw else self.t_start
        lo = 0 if none else int(kw.get("min", 1))
        hi = 0 if none else (int(kw["max"]) if "max" in kw else None)
        desc = " ".join(f"{k}={v}" for k, v in kw.items() if k not in ("wait",))
        # journal lines reach us up to ~3 s late (spdlog async flush_every 3 s, R4)
        wait = float(kw.get("wait", 5 if not none else 0))
        settle = float(kw.get("settle", 4 if none else 0))
        if settle:
            await asyncio.sleep(settle)
        deadline = time.monotonic() + wait
        while True:
            got = self.count_expect(kw, t_lo)
            ok = got >= lo and (hi is None or got <= hi)
            if ok or time.monotonic() >= deadline:
                break
            await asyncio.sleep(0.5)
        want = f"=0" if none else (f">={lo}" + (f",<={hi}" if hi is not None else ""))
        rec = {"check": ("expect-none " if none else "expect ") + desc, "got": got, "want": want, "ok": ok}
        if kw.get("join") == "move" and getattr(self, "last_join", None):
            j = self.last_join[0]
            rec["example"] = f"{j['name']} cid={j['cid']} dt={j['dt_ms']}ms srv[{j['srv_k']}] {j['msg'][:90]}"
        self.expects.append(rec)
        if ok:
            return {"ok": True, "text": f"OK {rec['check']} got={got}" + (f" e.g. {rec['example']}" if rec.get("example") else "")}
        return {"ok": False, "kind": "fail", "text": f"FAIL check={rec['check']!r} got={got} want{want}"}

    async def v_expect_none(self, actor, pos, kw):
        return await self.v_expect(actor, pos, kw, none=True)

    # ---------------- main flow
    async def main(self) -> int:
        self.say(f"RUN {self.run_id} scenario={self.hdr.get('SCENARIO', self.steps_path.stem)} steps={len(self.steps)} "
                 f"dir={self.run_dir}")
        self.start_watch()
        self.hub = H.Hub(self.run_dir, int(self.cfg["hub_port"]))
        self.hub.echo = False
        await self.hub.start()
        self.clock = H.ClockTracker(self.hub, self.cfg["ssh_target"], float(self.cfg.get("clock_resample_s", 300)))
        try:
            await self.clock.start()
        except Exception as e:
            self.infra = f"server clock/ssh unavailable: {e}"
        self.tz_min = merge.tz_minutes(self.clock.tz)
        jt = threading.Thread(target=self._journal_thread, daemon=True)
        at = threading.Thread(target=self._audit_thread, daemon=True)
        if not self.infra:
            jt.start()
            if H.audit_tail_cmd(self.cfg) and self.hdr.get("AUDIT", "auto").lower() != "off":
                at.start()
            await asyncio.sleep(1.0)
        try:
            if not self.infra:
                for i, st in enumerate(self.steps, 1):
                    self.say(f"step {i}/{len(self.steps)} L{st.lineno} @{st.actor} {st.verb} "
                             f"{' '.join(st.pos)} {' '.join(f'{k}={v}' for k, v in st.kw.items())}".rstrip())
                    self.hub.log("step", n=i, line=st.raw)
                    try:
                        r = await self.run_step(st)
                    except Infra:
                        raise
                    except Exception as e:
                        r = {"ok": False, "kind": "infra", "text": f"{type(e).__name__}: {e}"}
                        self.say("".join(traceback.format_exc()))
                    text = str(r.get("text", ""))
                    kind = r.get("kind") or ("ok" if r.get("ok") else ("stuck" if text.startswith("STUCK") else "fail"))
                    if not r.get("ok") and st.kw.get("optional") == "1":
                        kind, text = "skip", "SKIP " + text
                    self.verdicts.append({"n": i, "line": st.lineno, "actor": st.actor, "verb": st.verb,
                                          "verdict": kind.upper(), "text": text})
                    self.say("  " + text)
                    self.hub.log("verdict", n=i, text=text, kind=kind)
                    if kind == "infra":
                        self.infra = text
                        break
                    if kind == "fail":
                        self.failed = True
                    if kind == "stuck":
                        self.stuck = True
                        break
        except Infra as e:
            self.infra = str(e)
            self.say(f"INFRA {e}")
        except Exception as e:
            self.infra = f"{type(e).__name__}: {e}"
            self.say("Traceback (most recent call last): " + "".join(traceback.format_exc()))
        finally:
            await self.finish()
        code = EXIT_INFRA if self.infra else EXIT_STUCK if self.stuck else EXIT_FAIL if self.failed else EXIT_OK
        self.write_result(code)
        self.say(f"EXIT {code}")
        self.say("DONE")
        return code

    async def finish(self):
        try:
            if self.hdr.get("BOTCFG", "auto") != "off" and not self.botcfg and not self.args.no_botcfg:
                await self.take_botcfg("auto")
        except Exception as e:
            self.notes.append(f"botcfg failed: {e}")
        for name in sorted(self.procs, key=lambda n: not n.startswith("c")):  # cameras first
            try:
                await self.stop_client(name, logout=True)
            except Exception as e:
                self.notes.append(f"stop {name}: {e}")
        self.stopping = True
        await asyncio.sleep(4.0)  # let the last journal lines arrive (async logger flush)
        if self.jproc and self.jproc.poll() is None:
            self.jproc.terminate()
        if self.aproc and self.aproc.poll() is None:
            self.aproc.terminate()
        try:
            await self.clock.stop()
        except Exception:
            pass
        self.t_end = H.epoch_ms()
        # authoritative journal window for merge (live tail can miss lines across reconnects)
        try:
            off = self.clock.offset_at(self.t_start)
            n = await asyncio.to_thread(H.pull_server_log, self.cfg, self.run_dir, (self.t_start + off) / 1000 - 5,
                                        (self.t_end + off) / 1000 + 5)
            self.hub.log("server_log", lines=n)
        except Exception as e:
            self.notes.append(f"server.log pull failed: {e}")
        # Phase 2: authoritative audit window (server clock), read-only cat+awk over ssh
        if self.hdr.get("AUDIT", "auto").lower() != "off":
            try:
                off = self.clock.offset_at(self.t_start)
                self.audit_pulled = await asyncio.to_thread(H.pull_audit, self.cfg, self.run_dir,
                                                            self.t_start + off - 5000, self.t_end + off + 5000)
                self.hub.log("audit_pull", lines=self.audit_pulled)
            except Exception as e:
                self.notes.append(f"audit pull failed: {e}")
        self.stripped = {p.name: H.strip_cam_login(p) for p in (self.run_dir / "records").glob("*.cam")}
        self.db_pos = {}
        for actor, char in self.cfg["players"].items():
            if actor in self.actor_client:
                try:
                    rows = H.db_query("SELECT posx,posy,posz FROM players WHERE name='" + char.replace("'", "''") + "'")
                    self.db_pos[char] = ",".join(rows[0]) if rows else None
                except Exception as e:
                    self.db_pos[char] = f"error: {e}"
        self.write_meta()
        await self.hub.stop()
        try:
            self.lat = await asyncio.to_thread(merge.main, self.run_dir)
        except Exception as e:
            self.lat = None
            self.notes.append(f"merge failed: {type(e).__name__}: {e}")

    def write_meta(self, code=None):
        meta = {"run_id": self.run_id, "label": self.args.label, "scenario": self.hdr.get("SCENARIO"),
                "header": self.hdr, "steps_file": str(self.steps_path), "git": git_info(),
                "t_start_pc_ms": self.t_start, "t_end_pc_ms": getattr(self, "t_end", None),
                "clock_samples": self.clock.samples if hasattr(self, "clock") else [],
                "server_tz": getattr(self.clock, "tz", None) if hasattr(self, "clock") else None,
                "clients": self.cmeta, "vars": self.vars, "watch_names": sorted(self.watch_names),
                "player_char": self.cfg["players"].get("p1"), "server": self.cfg["server_host"],
                "game_port": self.cfg["game_port"], "botcfg": self.botcfg,
                "cam_login_lines_stripped": getattr(self, "stripped", {}), "db_final_pos": getattr(self, "db_pos", {}),
                "visible": self.cfg.get("visible"), "client_dir": self.cfg.get("client_dir"),
                "gm_assist": [], "notes": self.notes, "exit_code": code, "infra": self.infra}
        (self.run_dir / "run.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")

    def write_result(self, code: int):
        self.write_meta(code)
        verdict = {0: "PASS", 2: "INFRA", 3: "STUCK", 4: "FAIL"}[code]
        n_ok = sum(1 for v in self.verdicts if v["verdict"] == "OK")
        n_exp = sum(1 for e in self.expects if e["ok"])
        L = [f"# Result {self.run_id}", "",
             f"| Result | {verdict} | exit={code} | steps {n_ok}/{len(self.steps)} OK | expects {n_exp}/{len(self.expects)} |",
             "|---|---|---|---|---|", "",
             f"scenario `{self.hdr.get('SCENARIO', self.steps_path.stem)}` · git {git_info().get('sha', '')[:12]} · "
             f"botcfg {(self.botcfg or {}).get('sha256', 'n/a')[:16]} · label {self.args.label or '-'}", ""]
        if self.infra:
            L += [f"**INFRA**: {self.infra}", ""]
        L += ["## Steps", "", "| # | line | actor | verb | verdict | detail |", "|---:|---:|---|---|---|---|"]
        for v in self.verdicts:
            L.append(f"| {v['n']} | {v['line']} | {v['actor']} | {v['verb']} | {v['verdict']} | "
                     f"{v['text'].replace('|', '/')[:300]} |")
        L += ["", "## Expects", "", "| check | got | want | verdict |", "|---|---:|---|---|"]
        for e in self.expects:
            L.append(f"| {e['check'].replace('|', '/')} | {e['got']} | {e['want']} | {'PASS' if e['ok'] else 'FAIL'} |")
        if getattr(self, "lat", None):
            cj = self.lat.get("cid_joins", {})
            L += ["", "## Joins (merge.py, window +-100 ms journal / +-50 ms lagmark)", ""]
            for c, v in cj.items():
                L.append(f"- cli:{c}: see_move<->journal same-cid joins n={v['n']} (pos match {v['pos_match']}), dt {v['dt_ms']}")
            L.append(f"- clock samples: {self.lat['clock']['samples']}, drift {self.lat['clock']['drift_ms']:.1f} ms")
        if getattr(self, "lat", None) and self.lat.get("perception"):
            L += perception_section(self.lat["perception"])
        if getattr(self, "lat", None) and self.lat.get("audit") is not None:
            L += self.audit_section()
        L += ["", "## Final DB position (after logout)", ""]
        for k, v in (getattr(self, "db_pos", {}) or {}).items():
            L.append(f"- {k}: {v}")
        L += ["", "## GM-assist", "", "none", ""]
        if self.notes:
            L += ["## Notes", ""] + [f"- {n}" for n in self.notes] + [""]
        (self.run_dir / "result.md").write_text("\n".join(L), encoding="utf-8")


def _fmt_hist(d) -> str:
    if not d:
        return "-"
    return " ".join(f"{k}={v}" for k, v in d.items())


def _audit_section(self) -> list[str]:
    """result.md block for the Phase 2 server audit: per-kind counts + the census summary."""
    a = self.lat.get("audit") or {}
    c = self.lat.get("census") or {}
    L = ["", "## Server audit (Phase 2, src=srv:audit)", ""]
    if not a.get("records"):
        if self.audit_pulled == -1:
            why = "harness.json has no audit_path"
        else:
            why = "no records in the run window (is `/cavebot audit on` set? botAuditEnabled defaults OFF)"
        return L + [f"none - {why}"]
    L.append(f"- records: {a['records']} ({_fmt_hist(a.get('kinds'))})")
    if a.get("watched"):
        L.append(f"- probed bots: {', '.join(a['watched'])}")
    w = a.get("wake_to_p1_appear") or {}
    if w.get("n"):
        L.append(f"- wake -> p1 see_appear: n={w['n']} dt {w.get('dt_ms')}")
    if c.get("n"):
        L += ["", f"### Census ({c['n']} samples over {c['span_s']} s, all bots incl. hibernated)", "",
              "| metric | p50 | min | max |", "|---|---:|---:|---:|"]
        for key in ("active", "awake", "hib", "cast", "real_players"):
            v = c.get(key) or {}
            L.append(f"| {key} | {v.get('p50', '-')} | {v.get('min', '-')} | {v.get('max', '-')} |")
        L += ["", f"- state mean: {_fmt_hist(c.get('state_mean'))}",
              f"- state last: {_fmt_hist(c.get('state_last'))}",
              f"- hunt phases last [awake,hib]: {_fmt_hist(c.get('phase_last'))}",
              f"- virtual hunts last: {_fmt_hist(c.get('vhunt_last'))}; live hunts last: {_fmt_hist(c.get('lhunt_last'))}",
              f"- top towns last [awake,hib]: {_fmt_hist(c.get('town_top_last'))}",
              f"- wake window last: {_fmt_hist(c.get('wake_last'))}"]
    return L


Runner.audit_section = _audit_section


def perception_section(pc: dict) -> list[str]:
    """result.md block: what each hidden client perceived (perceive.lua)."""
    L = ["", "## Perception (client-side, perceive.lua)", "",
         "| client | effect | missile | text anim | text static | msg | creature | self | cooldown | sound | tap errors |",
         "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for c, v in sorted(pc.get("clients", {}).items()):
        k = v.get("kinds", {})
        L.append(f"| {c} | {k.get('see_effect', 0)} | {k.get('see_missile', 0)} | {v.get('text_anim', 0)} | "
                 f"{v.get('text_static', 0)} | {k.get('see_msg', 0)} | {k.get('see_creature', 0)} | "
                 f"{k.get('see_self', 0)} | {k.get('see_cooldown', 0)} | {k.get('see_sound', 0)} | {k.get('tap_error', 0)} |")
    for c, v in sorted(pc.get("clients", {}).items()):
        bits = []
        if v.get("effect_ids"):
            bits.append("effect ids " + _fmt_hist(v["effect_ids"]))
        if v.get("missile_ids"):
            bits.append("missile ids " + _fmt_hist(v["missile_ids"]))
        if v.get("anim_modes"):
            bits.append("anim " + _fmt_hist(v["anim_modes"]))
        if v.get("msg_modes"):
            bits.append("msg " + _fmt_hist(v["msg_modes"]))
        if v.get("effect_life_est_ms"):
            bits.append(f"effect life est ms {v['effect_life_est_ms']}")
        if v.get("missile_life_ms"):
            bits.append(f"missile life ms {v['missile_life_ms']}")
        if bits:
            L.append(f"- {c}: " + "; ".join(bits))
    for j in pc.get("say_to_self_effect", []):
        L.append(f"- {j['client']}: say -> own see_effect n={j['n']} of {j['says']} says, dt {j['dt_ms']}")
    for x in pc.get("cross", []):
        L.append(f"- cross-client {x['a']}->{x['b']}: of {x['n']} effects {x['a']} saw inside both views, "
                 f"{x['b']} saw {x['matched']} ({x['rate']})")
    return L


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("steps")
    ap.add_argument("--run-id")
    ap.add_argument("--profile", help="harness.json target profile (default: env HARNESS_PROFILE, else the base config)")
    ap.add_argument("--label")
    ap.add_argument("--no-botcfg", action="store_true")
    ap.add_argument("--watch-max-min", type=float)
    ap.add_argument("--visible", action="store_true", help="show the OTClient windows (debug); default hidden")
    ap.add_argument("--client-dir", help="harness client copy to launch (default: harness.json client_dir)")
    ap.add_argument("--var", action="append", default=[], metavar="NAME=VALUE",
                    help="pre-set a scenario variable ($NAME), repeatable")
    a = ap.parse_args()
    try:
        r = Runner(Path(a.steps).resolve(), a)
    except Infra as e:
        print(f"INFRA {e}")
        sys.exit(EXIT_INFRA)
    sys.exit(asyncio.run(r.main()))


if __name__ == "__main__":
    main()
