#!/usr/bin/env python3
"""Join harness client events with the server journal for one run.

  python tools/bottest/merge.py runs/<runId>

Inputs (run dir): run.json (clock samples), hub.ndjson, cli-*.ndjson, server.log
(journalctl -o short-unix, last line "#TZ <+-HHMM>"), audit.ndjson (Phase 2 engine audit
stream, raw server records pulled for the run window; optional).
Outputs: server.ndjson (parsed journal lines, PC-clock ts), audit.pc.ndjson (audit records
mapped onto the PC clock, src "srv:audit"), latency.json (incl. "audit" kind counts and the
"census" summary), timeline.md.

Audit records (plan section 3.2) carry their own epoch-ms `ts` from the server's system_clock, so
unlike journal lines they need no timezone and no spdlog parsing: pc = ts - offset_ms(t). They are
kept apart from journal lines by src ("srv:audit" vs "srv"); `bot` is mirrored into `name` and
`pos` [x,y,z] becomes {x,y,z}, so the runner's existing bot=/pos filters work on them unchanged.

Clock model: every server time is mapped onto the PC clock with
    pc = srv - offset_ms(t)   (offset_ms = server_clock - pc_clock, re-sampled every <=10 min
                               over one persistent ssh session; interpolated between samples)
The embedded spdlog timestamp ([%Y-%d-%m %H:%M:%S.%e], server local tz) is the time the
line was *logged*; the journald receipt time is kept as `jts` for comparison.

Join windows (measured in Phase 0: server->client p95 ~20 ms, clock uncertainty ~2 ms):
    JOURNAL_WINDOW_MS = 100   client event <-> journal line about the same creature
    LAGMARK_WINDOW_MS = 50    /lagmark send <-> server stamp
"""
from __future__ import annotations

import datetime as dt
import json
import re
import statistics
import sys
from pathlib import Path

PLAYER_FIRST_ID = 0x10000000
BOT_GUID_MIN, BOT_GUID_MAX = 65001, 66004
JOURNAL_WINDOW_MS = 100.0
LAGMARK_WINDOW_MS = 50.0

LINE_RE = re.compile(r"^(\d+\.\d+) \S+ canary\[\d+\]: \[(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{3})\] \[(\w+)\] (.*)$")
PATTERNS = [
    ("wake", re.compile(r"Woke bot '([^']+)' at \((\d+),(\d+),(\d+)\)")),
    ("login", re.compile(r"^(.+?) has logged in\.")),
    ("logout", re.compile(r"^(.+?) has logged out\.")),
    ("cast_connect", re.compile(r"\[Cast\] (.+?) connected to (.+?) \(")),
    ("cast_op_open", re.compile(r"\[Cast\]\[OP\] operator session opened: (.+?) ip=\S+ watching=(.+?) bot=")),
    ("cast_op_cmd", re.compile(r"\[Cast\]\[OP\] (.+?) ip=\S+ as=(.+?) group=\S+ cmd='(.*)'")),
    ("cast_op_done", re.compile(r"\[Cast\]\[OP\] (.+?) finished cmd='(.*)' handled=(\w+)")),
    ("lagmark", re.compile(r"\[LAGMARK\] viewer=(.+?) watching=(.+?) t=(\d+)")),
    ("roam_arrived", re.compile(r"\[ROAM\] arrived bot='([^']+)'")),
    ("roam_leg", re.compile(r"\[ROAM\] leg_start bot='([^']+)'")),
    ("route", re.compile(r"\[BotEngine\] ([^:\[\]]+?): ROUTE: ")),
]
TAG_RE = re.compile(r"^\[([A-Z][A-Z0-9_:]+)\]")
GUID_RE = re.compile(r"\bguid=(\d+)")
NAME_RES = [re.compile(r"\b(?:name|bot)='([^']+)'"), re.compile(r"Woke bot '([^']+)'"),
            re.compile(r"^\[BotEngine\] ([^:\[\]']+?): ")]
POS_RE = re.compile(r"\((\d{4,5}),(\d{4,5}),(\d{1,2})\)")


def tz_minutes(tz: str | None, default: int = -300) -> int:
    m = re.match(r"([+-])(\d{2})(\d{2})", tz or "")
    if not m:
        return default
    return (1 if m.group(1) == "+" else -1) * (int(m.group(2)) * 60 + int(m.group(3)))


def offset_at(samples: list[dict], pc_ms: float | None) -> float:
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


def parse_line(line: str, tz_min: int, samples: list[dict]) -> dict | None:
    """One journalctl -o short-unix line -> server event with PC-clock ts and creature refs."""
    m = LINE_RE.match(line.rstrip("\n"))
    if not m:
        return None
    jts = float(m.group(1)) * 1000.0
    y, d, mo, hh, mi, ss, ms = (int(m.group(i)) for i in (2, 3, 4, 5, 6, 7, 8))
    try:
        local = dt.datetime(y, mo, d, hh, mi, ss, ms * 1000, tzinfo=dt.timezone(dt.timedelta(minutes=tz_min)))
    except ValueError:
        return None
    lts = local.timestamp() * 1000.0
    msg = m.group(10)
    off = offset_at(samples, lts - (samples[0]["offset_ms"] if samples else 0.0))
    rec = {"src": "srv", "srv_ts": lts, "jts": jts, "ts": lts - off, "lvl": m.group(9), "msg": msg}
    for kind, rx in PATTERNS:
        mm = rx.search(msg)
        if mm:
            rec["k"] = kind
            rec["g"] = list(mm.groups())
            break
    if "k" not in rec:
        mm = TAG_RE.match(msg)
        rec["k"] = mm.group(1).lower() if mm else "log"
    mm = GUID_RE.search(msg)
    if mm:
        rec["guid"] = int(mm.group(1))
        if BOT_GUID_MIN <= rec["guid"] <= BOT_GUID_MAX or rec["guid"] >= 70001:
            rec["cid"] = PLAYER_FIRST_ID + rec["guid"]
    for rx in NAME_RES:
        mm = rx.search(msg)
        if mm:
            rec["name"] = mm.group(1).strip()
            break
    mm = POS_RE.search(msg)
    if mm:
        rec["pos"] = {"x": int(mm.group(1)), "y": int(mm.group(2)), "z": int(mm.group(3))}
    return rec


def parse_server_text(text: str, samples: list[dict]) -> list[dict]:
    m = re.search(r"^#TZ (\S+)", text, re.M)
    tz = tz_minutes(m.group(1) if m else None)
    out = []
    for line in text.splitlines():
        r = parse_line(line, tz, samples)
        if r:
            out.append(r)
    return out


AUDIT_SRC = "srv:audit"
AUDIT_TIMELINE_SKIP = {"probe", "census"}  # high-rate kinds: summarised, not listed row by row


def normalize_audit(rec: dict, samples: list[dict]) -> dict | None:
    """One raw audit record (server epoch-ms ts) -> PC-clock event usable by expects/joins."""
    try:
        srv = float(rec["ts"])
    except (KeyError, TypeError, ValueError):
        return None
    off = offset_at(samples, srv - (samples[0]["offset_ms"] if samples else 0.0))
    r = dict(rec)
    r["srv_ts"] = srv
    r["ts"] = srv - off
    r["src"] = AUDIT_SRC
    if r.get("bot") and "name" not in r:
        r["name"] = r["bot"]
    p = r.get("pos")
    if isinstance(p, list) and len(p) == 3:
        r["pos"] = {"x": p[0], "y": p[1], "z": p[2]}
    return r


def parse_audit_text(text: str, samples: list[dict]) -> list[dict]:
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            r = normalize_audit(json.loads(line), samples)
        except json.JSONDecodeError:
            continue  # a torn line from a live tail; the pulled file is whole lines only
        if r:
            out.append(r)
    out.sort(key=lambda r: (r.get("seq", 0), r["ts"]))
    return out


def census_summary(audit: list[dict]) -> dict:
    """Aggregate the 10 s `census` records of one run (all bots, hibernated included)."""
    cs = [r for r in audit if r.get("k") == "census"]
    if not cs:
        return {"n": 0}
    cs.sort(key=lambda r: r["ts"])

    def ser(key):
        return [float(c.get(key, 0) or 0) for c in cs]

    states = sorted({k for c in cs for k in (c.get("state") or {})})
    state_mean = {k: round(statistics.fmean([(c.get("state") or {}).get(k, 0) for c in cs]), 1) for k in states}
    last = cs[-1]
    towns = sorted(((t, ah[0] + ah[1], ah) for t, ah in (last.get("town") or {}).items()), key=lambda x: -x[1])
    return {"n": len(cs), "span_s": round((cs[-1]["ts"] - cs[0]["ts"]) / 1000.0, 1),
            "active": summary(ser("active")), "awake": summary(ser("awake")), "hib": summary(ser("hibn")),
            "cast": summary(ser("cast")), "real_players": summary(ser("realPlayers")),
            "state_mean": state_mean, "state_first": cs[0].get("state"), "state_last": last.get("state"),
            "phase_last": last.get("phase"), "vhunt_last": last.get("vhunt"), "lhunt_last": last.get("lhunt"),
            "town_top_last": {t: ah for t, _, ah in towns[:6]},
            "wake_last": {k: last.get(k) for k in ("wakeTried60s", "wakeGated60s", "wakeGranted60s")}}


def audit_kind_counts(audit: list[dict]) -> dict:
    out: dict = {}
    for r in audit:
        out[r.get("k", "?")] = out.get(r.get("k", "?"), 0) + 1
    return dict(sorted(out.items()))


def name_cid_map(cli_events: list[dict]) -> dict[str, int]:
    out = {}
    for e in cli_events:
        if e.get("name") and e.get("cid") and e.get("k") in ("see_appear", "see_move", "see_disappear"):
            out[e["name"]] = int(e["cid"])
    return out


def srv_cid(r: dict, name_to_cid: dict[str, int]) -> int | None:
    if r.get("cid"):
        return r["cid"]
    if r.get("name") and r["name"] in name_to_cid:
        return name_to_cid[r["name"]]
    return None


def join_moves(cli_events: list[dict], srv: list[dict], window_ms: float = JOURNAL_WINDOW_MS,
               cids: set[int] | None = None, need_pos: bool = False) -> list[dict]:
    """Client see_move events matched to a journal line about the SAME creature id within
    +-window_ms. A server line refers to a creature through guid=N (cid = firstID + guid) or a
    bot name resolved through the client's own name->cid map. need_pos additionally requires the
    line's first (x,y,z) to equal the move's destination."""
    n2c = name_cid_map(cli_events)
    by_cid: dict[int, list[dict]] = {}
    for r in srv:
        c = srv_cid(r, n2c)
        if c:
            by_cid.setdefault(c, []).append(r)
    out = []
    for e in cli_events:
        if e.get("k") != "see_move":
            continue
        c = int(e.get("cid", 0))
        if cids is not None and c not in cids:
            continue
        best = None
        for r in by_cid.get(c, ()):
            d = e["ts"] - r["ts"]
            if abs(d) > window_ms:
                continue
            if need_pos and r.get("pos") != e.get("to"):
                continue
            if best is None or abs(d) < abs(best[0]):
                best = (d, r)
        if best:
            d, r = best
            out.append({"cid": c, "name": e.get("name"), "cli_ts": e["ts"], "srv_ts": r["ts"],
                        "dt_ms": round(d, 1), "to": e.get("to"), "srv_k": r.get("k"),
                        "pos_match": r.get("pos") == e.get("to"), "msg": r["msg"][:200]})
    return out


def pct(vals, p):
    if not vals:
        return None
    s = sorted(vals)
    k = (len(s) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return round(s[f] + (s[c] - s[f]) * (k - f), 1)


def summary(vals):
    if not vals:
        return {"n": 0}
    return {"n": len(vals), "p50": pct(vals, 50), "p95": pct(vals, 95), "min": round(min(vals), 1),
            "max": round(max(vals), 1), "mean": round(statistics.fmean(vals), 1),
            "stdev": round(statistics.pstdev(vals), 1)}


def load_ndjson(p: Path):
    return [json.loads(l) for l in p.read_text(encoding="utf-8").splitlines() if l.strip()]


def clock_samples(meta: dict) -> list[dict]:
    if meta.get("clock_samples"):
        return meta["clock_samples"]
    return [s for s in (meta.get("clock_before"), meta.get("clock_after")) if s]


# ------------------------------------------------------------------ perception (perceive.lua)
PERCEPTION_KINDS = ("see_effect", "see_effect_end", "see_missile", "see_sound", "see_text", "see_msg",
                    "see_creature", "see_self", "see_cooldown", "tap_error")
CROSS_WINDOW_MS = 500.0
CORE_VIEW = (6, 4)   # |dx|, |dy| well inside the 15x11 game view of BOTH clients


def _hist(vals, top=12) -> dict:
    d: dict = {}
    for v in vals:
        d[v] = d.get(v, 0) + 1
    return dict(sorted(d.items(), key=lambda kv: -kv[1])[:top])


def _first_in(evts: list[dict], pred, lo: float, hi: float) -> dict | None:
    for e in evts:
        if lo <= e["ts"] <= hi and pred(e):
            return e
    return None


def _positions(evs: list[dict]) -> list[tuple[float, dict]]:
    out = []
    for e in evs:
        if e["k"] == "self_pos" and e.get("to"):
            out.append((e["ts"], e["to"]))
        elif e["k"] == "login_ok" and e.get("pos"):
            out.append((e["ts"], e["pos"]))
        elif e["k"] == "stat" and e.get("pos"):
            out.append((e["ts"], e["pos"]))
    out.sort(key=lambda x: x[0])
    return out


def _pos_at(track: list[tuple[float, dict]], ts: float) -> dict | None:
    best = None
    for t, p in track:
        if t > ts:
            break
        best = p
    return best


def perception_summary(cli: dict[str, list[dict]], hub: list[dict], audit: list[dict]) -> dict:
    """Per-client perception counts, say -> own effect latency, cross-client effect agreement and
    audit wake (sparkle) -> see_effect joins."""
    out: dict = {"clients": {}, "say_to_self_effect": [], "cross": [], "wake_sparkle": {}}
    for c, evs in cli.items():
        kinds = _hist([e["k"] for e in evs if e["k"] in PERCEPTION_KINDS], top=99)
        if not kinds:
            continue
        eff = [e for e in evs if e["k"] == "see_effect"]
        mis = [e for e in evs if e["k"] == "see_missile"]
        txt = [e for e in evs if e["k"] == "see_text"]
        lives = [e["life_est_ms"] for e in eff if e.get("life_est_ms")]
        out["clients"][c] = {
            "kinds": kinds,
            "effect_ids": _hist([e.get("id") for e in eff]),
            "missile_ids": _hist([e.get("id") for e in mis]),
            "text_anim": sum(1 for e in txt if e.get("kind") == "anim"),
            "text_static": sum(1 for e in txt if e.get("kind") == "static"),
            "anim_modes": _hist([e.get("mode") for e in txt if e.get("kind") == "anim"]),
            "msg_modes": _hist([e.get("mode_name") or e.get("mode") for e in evs if e["k"] == "see_msg"]),
            "effect_life_est_ms": summary(lives) if lives else None,
            "effect_life_lt_100ms": sum(1 for v in lives if v < 100),
            "missile_life_ms": summary([e["life_ms"] for e in mis if e.get("life_ms")]) if mis else None,
            "missile_life_lt_100ms": sum(1 for e in mis if (e.get("life_ms") or 999) < 100),
        }
    # say (spell words) -> first own-tile see_effect on the same client
    for c, evs in cli.items():
        says = [h for h in hub if h.get("k") == "cmd" and h.get("client") == c and h.get("verb") == "say"]
        if not says:
            continue
        dts = []
        for h in says:
            e = _first_in(evs, lambda e: e["k"] == "see_effect" and e.get("dx") == 0 and e.get("dy") == 0
                          and e.get("dz") == 0, h["ts"], h["ts"] + 3000)
            if e:
                dts.append(round(e["ts"] - h["ts"], 1))
        out["say_to_self_effect"].append({"client": c, "says": len(says), "n": len(dts), "dt_ms": summary(dts)})
    # cross-client agreement: effects A saw inside the core view of A AND of B (B's own position at
    # that moment) -> did B see the same id on the same tile within +-500 ms?
    names = sorted(c for c in cli if any(e["k"] == "see_effect" for e in cli[c]))
    for a in names:
        for b in names:
            if a == b:
                continue
            btrack = _positions(cli[b])
            beff = [e for e in cli[b] if e["k"] == "see_effect"]
            n = matched = 0
            for e in cli[a]:
                if e["k"] != "see_effect" or abs(e.get("dx", 99)) > CORE_VIEW[0] or abs(e.get("dy", 99)) > CORE_VIEW[1] \
                        or e.get("dz") != 0:
                    continue
                bp = _pos_at(btrack, e["ts"])
                if not bp or bp.get("z") != e.get("z") or abs(e["x"] - bp["x"]) > CORE_VIEW[0] \
                        or abs(e["y"] - bp["y"]) > CORE_VIEW[1]:
                    continue
                n += 1
                if any(x.get("id") == e.get("id") and x.get("at") == e.get("at") and abs(x["ts"] - e["ts"]) <= CROSS_WINDOW_MS
                       for x in beff):
                    matched += 1
            if n:
                out["cross"].append({"a": a, "b": b, "n": n, "matched": matched, "rate": f"{100.0 * matched / n:.1f}%"})
    # server audit wake (sparkle) -> see_effect on that tile (B2's client-side source)
    wk = [r for r in audit if r.get("k") == "wake" and r.get("result") == "ok" and isinstance(r.get("pos"), dict)]
    joins = []
    for r in wk:
        at = f"{r['pos'].get('x')},{r['pos'].get('y')},{r['pos'].get('z')}"
        for c, evs in cli.items():
            e = _first_in(evs, lambda e: e["k"] == "see_effect" and e.get("at") == at, r["ts"] - 500, r["ts"] + 3000)
            if e:
                joins.append({"client": c, "bot": r.get("name"), "sparkle": r.get("sparkle"), "id": e.get("id"),
                              "dt_ms": round(e["ts"] - r["ts"], 1)})
    out["wake_sparkle"] = {"wakes": len(wk), "sparkle_wakes": sum(1 for r in wk if r.get("sparkle")),
                           "joined": len(joins), "sample": joins[:20]}
    return out


def perception_row(e: dict) -> str | None:
    """timeline.md detail for a perception event (None = omit the row)."""
    k = e["k"]
    on = f" on {e['name']}" if e.get("name") else ""
    if k == "see_effect":
        return f"id={e.get('id')} at={e.get('at')} d={e.get('dx')},{e.get('dy')},{e.get('dz')}{on}"
    if k == "see_missile":
        return (f"id={e.get('id')} {e.get('from')}->{e.get('to')} {e.get('name') or '?'}->{e.get('to_name') or '?'} "
                f"life={e.get('life_ms')}ms")
    if k == "see_text":
        return f"{e.get('kind')} {e.get('mode')} '{str(e.get('text'))[:120]}' at={e.get('at')}{on}"
    if k == "see_msg":
        return f"{e.get('mode_name') or e.get('mode')}: {str(e.get('text'))[:200]}"
    if k == "see_self":
        ch = str(e.get("changed", "")).split(",")
        return f"[{e.get('why')}] " + " ".join(f"{c}={e.get(c)}" for c in ch if c)[:200]
    if k == "see_creature":
        if e.get("why") in ("dir", "walk", "dir,walk"):
            return None
        ch = str(e.get("changed", "")).split(",")
        return f"{e.get('name')} [{e.get('why')}] " + " ".join(f"{c}={e.get(c)}" for c in ch if c)[:200]
    if k == "see_cooldown":
        return f"{e.get('kind')} id={e.get('id')} ms={e.get('ms')}"
    return None


def main(run_dir: Path):
    meta = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
    samples = clock_samples(meta)
    offs = [s["offset_ms"] for s in samples] or [0.0]
    srv = parse_server_text((run_dir / "server.log").read_text(encoding="utf-8", errors="replace"), samples) \
        if (run_dir / "server.log").exists() else []
    with open(run_dir / "server.ndjson", "w", encoding="utf-8") as fh:
        for r in srv:
            fh.write(json.dumps(r) + "\n")

    audit_path = run_dir / "audit.ndjson"
    audit = (parse_audit_text(audit_path.read_text(encoding="utf-8", errors="replace"), samples)
             if audit_path.exists() else [])
    with open(run_dir / "audit.pc.ndjson", "w", encoding="utf-8") as fh:
        for r in audit:
            fh.write(json.dumps(r) + "\n")

    cli = {}
    for p in sorted(run_dir.glob("cli-*.ndjson")):
        cli[p.stem[4:]] = load_ndjson(p)
    hub = load_ndjson(run_dir / "hub.ndjson") if (run_dir / "hub.ndjson").exists() else []

    lat: dict = {"clock": {"samples": len(samples), "offset_first_ms": offs[0], "offset_last_ms": offs[-1],
                           "drift_ms": offs[-1] - offs[0],
                           "uncertainty_ms": max((s.get("uncertainty_ms", 0) for s in samples), default=None)},
                 "windows_ms": {"journal": JOURNAL_WINDOW_MS, "lagmark": LAGMARK_WINDOW_MS}}

    # journald receipt lag vs embedded log timestamp (how late journal lines can be)
    lat["journald_minus_logged_ms"] = summary([r["jts"] - r["srv_ts"] for r in srv])

    # ---- lagmark round trips: client send -> server handle (t=OTSYS_TIME) -> client rx.
    # A reply is paired with the send whose client ts lies within +-LAGMARK_WINDOW_MS of the
    # server stamp (mapped to PC clock); unpaired replies are counted, not guessed.
    lag = {}
    unpaired = 0
    sends = [h for h in hub if h["k"] == "lagmark_sent"]
    fps_changes = sorted((h["ts"], h["args"]["fps"]) for h in hub
                         if h["k"] == "cmd" and h.get("verb") == "set_fps" and h.get("args"))

    def fps_at(ts):
        cur = None
        for t, f in fps_changes:
            if t <= ts:
                cur = f
        return cur

    used = set()
    for cname, evs in cli.items():
        for e in evs:
            if e["k"] != "see_talk" or not str(e.get("text", "")).startswith("lagmark @ "):
                continue
            t_srv = int(e["text"].split("@", 1)[1].strip())
            t_srv_pc = t_srv - offset_at(samples, t_srv - offs[0])
            cand = [s for s in sends if s.get("client_ts") and id(s) not in used
                    and abs(t_srv_pc - s["client_ts"]) <= LAGMARK_WINDOW_MS]
            if not cand:
                unpaired += 1
                continue
            s = min(cand, key=lambda s: abs(t_srv_pc - s["client_ts"]))
            used.add(id(s))
            d = lag.setdefault(str(fps_at(s["ts"])), {"up": [], "down": [], "rtt": [], "hub_rx_minus_client": []})
            d["up"].append(t_srv_pc - s["client_ts"])
            d["down"].append(e["ts"] - t_srv_pc)
            d["rtt"].append(e["ts"] - s["client_ts"])
            d["hub_rx_minus_client"].append(e["rx"] - e["ts"])
    lat["lagmark"] = {f: {k: summary(v) for k, v in d.items()} for f, d in lag.items()}
    lat["lagmark_unpaired"] = unpaired

    # ---- semantic joins on name (these are cause->effect, so their window is seconds)
    joins = []

    def first_after(evts, pred, t0, window_ms=120000):
        for e in evts:
            if t0 - 5000 <= e["ts"] <= t0 + window_ms and pred(e):
                return e
        return None

    p1 = cli.get("p1", [])
    for r in srv:
        k = r.get("k")
        if k == "wake":
            name = r["g"][0]
            e = first_after(p1, lambda e: e["k"] == "see_appear" and e.get("name") == name, r["ts"])
            if e:
                joins.append({"join": "wake->p1 see_appear", "name": name, "dt_ms": round(e["ts"] - r["ts"], 1),
                              "cid": e.get("cid")})
        elif k == "login":
            name = r["g"][0]
            for cname, evs in cli.items():
                e = first_after(evs, lambda e: e["k"] == "login_ok" and e.get("char") == name, r["ts"], 20000)
                if e:
                    joins.append({"join": f"login->{cname} login_ok", "name": name, "dt_ms": round(e["ts"] - r["ts"], 1)})
        elif k == "cast_op_cmd":
            bot = r["g"][1]
            for cname, evs in cli.items():
                e = first_after(evs, lambda e: e["k"] in ("see_msg", "see_text") and str(e.get("text", "")).startswith("[cavebot]"),
                                r["ts"], 20000)
                if e and cname.startswith("c"):
                    joins.append({"join": f"op cmd log->{cname} reply text", "name": bot,
                                  "dt_ms": round(e["ts"] - r["ts"], 1)})
                    break
    lat["joins"] = joins
    by = {}
    for j in joins:
        by.setdefault(j["join"], []).append(j["dt_ms"])
    lat["joins_summary"] = {k: summary(v) for k, v in by.items()}

    # ---- cid joins: client see_move <-> journal line about the same creature, +-100 ms
    cid_joins = {}
    for cname, evs in cli.items():
        js = join_moves(evs, srv, JOURNAL_WINDOW_MS)
        cid_joins[cname] = {"n": len(js), "pos_match": sum(1 for j in js if j["pos_match"]),
                            "dt_ms": summary([j["dt_ms"] for j in js]), "sample": js[:25]}
    lat["cid_joins"] = cid_joins

    # ---- client step cadence (jitter of see_move intervals per creature) on p1
    steps = {}
    for e in p1:
        if e["k"] == "see_move":
            steps.setdefault(e["cid"], []).append(e["ts"])
    intervals = []
    for ts in steps.values():
        intervals += [b - a for a, b in zip(ts, ts[1:]) if b - a < 2000]
    lat["p1_see_move_interval_ms"] = summary(intervals)
    lat["p1_see_move_count"] = sum(len(v) for v in steps.values())

    # ---- packet records
    recs = {}
    for cname, evs in cli.items():
        for e in evs:
            if e["k"] == "record_start" and e.get("file"):
                b = e["before"]
                start_epoch = (b.get("ems") or e["ts"]) - (b["cms"] - b["frame_ms"])
                p = Path(e["file"])
                n = 0
                if p.exists():
                    with open(p, encoding="utf-8", errors="replace") as fh:
                        n = sum(1 for _ in fh)
                recs[cname] = {"file": str(p), "start_epoch_ms": round(start_epoch, 1), "exists": p.exists(),
                               "bytes": p.stat().st_size if p.exists() else 0, "lines": n}
    lat["records"] = recs

    # ---- Phase 2 server audit: per-kind counts, census summary, wake -> client see_appear
    lat["audit"] = {"records": len(audit), "kinds": audit_kind_counts(audit),
                    "watched": sorted({r["name"] for r in audit if r.get("name") and r.get("k") == "probe"})}
    lat["census"] = census_summary(audit)
    wake_joins = []
    for r in audit:
        if r.get("k") == "wake" and r.get("result") == "ok" and r.get("name"):
            e = first_after(p1, lambda e, n=r["name"]: e["k"] == "see_appear" and e.get("name") == n, r["ts"], 30000)
            if e:
                wake_joins.append({"name": r["name"], "path": r.get("path"), "visible": r.get("visible"),
                                   "sparkle": r.get("sparkle"), "dt_ms": round(e["ts"] - r["ts"], 1)})
    lat["audit"]["wake_to_p1_appear"] = {"n": len(wake_joins), "dt_ms": summary([j["dt_ms"] for j in wake_joins]),
                                         "sample": wake_joins[:20]}
    lat["perception"] = perception_summary(cli, hub, audit)
    (run_dir / "latency.json").write_text(json.dumps(lat, indent=2), encoding="utf-8")

    # ---- timeline.md
    watch = set(meta.get("watch_names") or [])
    all_joined = {}
    for cname, evs in cli.items():
        for j in join_moves(evs, srv, JOURNAL_WINDOW_MS):
            all_joined[(cname, j["cid"], j["cli_ts"])] = j
    names = {meta.get("player_char", "")} | watch
    for evs in cli.values():
        for e in evs:
            if e["k"] == "see_appear" and e.get("ctype") == "player":
                names.add(e.get("name"))
    names.discard("")
    names.discard(None)
    rows = []
    t0 = meta.get("t_start_pc_ms") or (hub[0]["ts"] if hub else 0)
    for h in hub:
        if h["k"] in ("cmd", "cmd_result", "check", "client_hello", "client_exit", "cam_target", "step", "verdict"):
            txt = h.get("text") or h.get("verb") or h.get("name") or ""
            if h["k"] == "check":
                txt = f"{h['name']} ok={h['ok']}"
            elif h["k"] == "cmd":
                txt = f"{h['client']} {h['verb']} {json.dumps(h.get('args'))[:120]}"
            elif h["k"] == "cmd_result":
                txt = f"{h['client']} {h['verb']} -> {h.get('text')}"
            elif h["k"] in ("step", "verdict"):
                txt = h.get("line") or h.get("text") or ""
            rows.append((h["ts"], "hub", h["k"], txt))
    for cname, evs in cli.items():
        for e in evs:
            k = e["k"]
            if k in ("stat", "self_pos"):
                continue
            if k == "see_move":
                j = all_joined.get((cname, int(e.get("cid", 0)), e["ts"]))
                if not j and e.get("name") not in watch:
                    continue
                txt = f"name={e.get('name')} cid={e.get('cid')} to={e.get('to')}"
                if j:
                    txt += f" JOIN dt={j['dt_ms']}ms srv[{j['srv_k']}]{' pos=match' if j['pos_match'] else ''}"
            elif k in ("see_appear", "see_disappear"):
                txt = f"{e.get('ctype')} name={e.get('name')} cid={e.get('cid')} pos={e.get('pos')}"
            elif k == "see_talk":
                txt = f"{e.get('name')} ch={e.get('channel')} mode={e.get('mode')}: {str(e.get('text'))[:160]}"
            elif k in ("see_effect", "see_missile", "see_text", "see_msg", "see_self", "see_creature", "see_cooldown"):
                txt = perception_row(e)
                if txt is None:
                    continue
            elif k == "reply":
                txt = str(e.get("text"))[:200]
            else:
                txt = json.dumps({kk: vv for kk, vv in e.items() if kk not in ("v", "ts", "seq", "src", "k", "rx", "cms", "cseq")})[:200]
            rows.append((e["ts"], f"cli:{cname}", k, txt))
    for r in srv:
        if r.get("k") in ("wake", "login", "logout", "cast_connect", "cast_op_open", "cast_op_cmd", "cast_op_done",
                          "roam_arrived", "roam_leg", "route") or any(n in r["msg"] for n in names):
            rows.append((r["ts"], "srv", r.get("k", "log"), r["msg"][:220]))
    for r in audit:
        k = r.get("k", "?")
        if k in AUDIT_TIMELINE_SKIP:
            continue
        detail = {kk: vv for kk, vv in r.items()
                  if kk not in ("v", "ts", "seq", "src", "k", "srv_ts", "guid", "cid", "bot", "hib")}
        rows.append((r["ts"], AUDIT_SRC, k, json.dumps(detail, separators=(",", ":"))[:220]))
    rows.sort(key=lambda x: x[0])
    lines = [f"# Timeline {meta['run_id']}", "",
             f"t0 = {t0:.0f} PC epoch ms; server times mapped to PC clock with {len(samples)} offset samples "
             f"({offs[0]:.1f} .. {offs[-1]:.1f} ms, server - pc). Join window +-{JOURNAL_WINDOW_MS:.0f} ms "
             f"(journal), +-{LAGMARK_WINDOW_MS:.0f} ms (lagmark).", "",
             f"see_move events on p1: {lat['p1_see_move_count']}; shown only when joined to a server line or "
             f"for watched bots ({', '.join(sorted(watch)) or 'none'}). stat/self_pos omitted.", "",
             "| t+ms | src | kind | detail |", "|---:|---|---|---|"]
    for ts, src, k, txt in rows:
        txt = str(txt).replace("|", "\\|").replace("\n", " ")
        lines.append(f"| {ts - t0:.0f} | {src} | {k} | {txt} |")
    (run_dir / "timeline.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({k: lat[k] for k in ("clock", "lagmark", "joins_summary", "p1_see_move_interval_ms", "records",
                                          "journald_minus_logged_ms", "audit")}, indent=2))
    print(json.dumps({c: {"n": v["n"], "pos_match": v["pos_match"], "dt_ms": v["dt_ms"]} for c, v in cid_joins.items()}))
    return lat


if __name__ == "__main__":
    main(Path(sys.argv[1]))
