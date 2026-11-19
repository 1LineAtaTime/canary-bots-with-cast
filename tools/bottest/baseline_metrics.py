"""Phase 3 (lean) baseline metrics: one small JSON per scenario from a finished run dir.

    python tools/bottest/baseline_metrics.py b2 runs/<run_id> [--out tools/bottest/baselines/<date>/B2.json]

Reads only what the runner/merge.py already wrote (cli-*.ndjson, audit.pc.ndjson, server.ndjson,
hub.ndjson, run.json) plus, for b6, a botperf result JSON (pass its path instead of a run dir).
Single run per scenario by design (user rule): every metric carries a change threshold for
compare.py, not a noise model. A metric is {value, unit, thr: {abs|rel[,min_abs]}, better, spec}.
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

BOT_CID_MIN, BOT_CID_MAX = 0x10000000 + 65000, 0x10000000 + 69999


def nd(path: Path):
    if not path.exists():
        return []
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out


def M(value, unit="", thr=None, better="none", spec="", note=""):
    d = {"value": value, "unit": unit, "thr": thr or {"rel": 0.5, "min_abs": 1}, "better": better}
    if spec:
        d["spec"] = spec
    if note:
        d["note"] = note
    return d


def pct(xs, q):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    k = (len(xs) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return round(xs[lo] + (xs[hi] - xs[lo]) * (k - lo), 1)


class Run:
    def __init__(self, d: Path):
        self.d = d
        self.meta = json.loads((d / "run.json").read_text(encoding="utf-8")) if (d / "run.json").exists() else {}
        self.audit = nd(d / "audit.pc.ndjson")
        self.srv = nd(d / "server.ndjson")
        self.hub = nd(d / "hub.ndjson")
        self.cli = {p.stem[4:]: nd(p) for p in d.glob("cli-*.ndjson")}
        self.vars = self.meta.get("vars", {})

    def aud(self, k, **flt):
        return [e for e in self.audit if e.get("k") == k and all(e.get(a) == b for a, b in flt.items())]

    def marks(self):
        return [(e["ts"], e.get("text", "")) for e in self.audit if e.get("k") == "mark"]

    def verdicts(self):
        steps = {e["n"]: e for e in self.hub if e.get("k") == "step"}
        return [(steps.get(e["n"], {}).get("line", ""), e) for e in self.hub if e.get("k") == "verdict"]

    def journal(self, rx):
        r = re.compile(rx)
        return [e for e in self.srv if r.search(e.get("msg", ""))]

    def window(self):
        return self.meta.get("t_start_pc_ms"), self.meta.get("t_end_pc_ms")

    def result_line(self):
        f = self.d / "result.md"
        if not f.exists():
            return "?"
        m = re.search(r"\| Result \| (\w+) \| exit=(\d+)", f.read_text(encoding="utf-8"))
        return f"{m.group(1)} exit={m.group(2)}" if m else "?"


def is_bot_ev(e):
    cid = e.get("cid") or 0
    return e.get("bot") is True or BOT_CID_MIN <= cid <= BOT_CID_MAX


# ------------------------------------------------------------------ B1 walk-pause
def b1(r: Run):
    names = [r.vars.get(f"W{i}") for i in range(1, 6) if r.vars.get(f"W{i}")]
    wp = [e for e in r.aud("walkpause") if not names or e.get("bot") in names]
    obs = [e for e in wp if e.get("tier") == "observed"]
    uno = [e for e in wp if e.get("tier") == "unobserved"]
    bad = [e for e in wp if e.get("st") not in ("IDLE", "DWELLING", "TRAVELING")]
    wb = {e.get("bot"): e.get("walkBias") for e in wp}
    # client: see_move gaps per watched bot (moving, then still >= 400 ms, then moving again)
    moves = defaultdict(list)
    for e in r.cli.get("p1", []):
        if e.get("k") == "see_move" and e.get("name") in names:
            moves[e["name"]].append(e["ts"])
    gaps = []
    for n, ts in moves.items():
        ts.sort()
        for a, b in zip(ts, ts[1:]):
            if 400 <= b - a <= 25000:
                gaps.append((n, a, b - a))
    # join each server pause to a client gap of the same bot starting within -300..+800 ms
    matched, visible = 0, 0
    dur_err = []
    for e in wp:
        ts, n = e["ts"], e.get("bot")
        seen_before = [t for t in moves.get(n, []) if ts - 2000 <= t <= ts + 300]
        if not seen_before:
            continue
        visible += 1
        g = [x for x in gaps if x[0] == n and -300 <= x[1] - ts <= 800]
        if g:
            matched += 1
            dur_err.append(g[0][2] - e.get("ms", 0))
    big_gaps = [g for g in gaps if g[2] >= 1500]
    return {
        "walkpause_total": M(len(wp), "n", better="none", spec="n/a (rate depends on walks issued)"),
        "walkpause_observed": M(len(obs), "n", spec="12%*wb/15 per walk issue, cap 8/route"),
        "walkpause_unobserved": M(len(uno), "n", spec="2%, cap 3/route"),
        "walkpause_disallowed_state": M(len(bad), "n", {"abs": 0}, "lower", "0 outside IDLE/DWELLING/TRAVELING"),
        "observed_ms_p50": M(pct([e["ms"] for e in obs], .5), "ms", {"rel": 0.5, "min_abs": 1000}, spec="uniform 500-20000"),
        "observed_ms_max": M(max([e["ms"] for e in obs], default=None), "ms", {"rel": 0.5, "min_abs": 2000}),
        "server_pauses_while_visible": M(visible, "n"),
        "client_matched_pauses": M(matched, "n", spec="every visible server pause should be a client-seen gap"),
        "client_match_rate": M(round(matched / visible, 2) if visible else None, "ratio", {"abs": 0.25}, "higher"),
        "client_gap_minus_server_ms_p50": M(pct(dur_err, .5), "ms", {"abs": 500}, "lower"),
        "client_gaps_ge_1500ms": M(len(big_gaps), "n", note="all >=1.5 s stills seen by p1 on the 5 bots, any cause"),
        "bots_walkbias": M(wb, "map", {"abs": 0}, note="personality; wb<=3 never pauses in observed tier"),
    }


# ------------------------------------------------------------------ B2 wake realism
def b2(r: Run):
    wk = r.aud("wake")
    ok = [e for e in wk if e.get("result") == "ok"]
    gated = [e for e in wk if e.get("result") == "gated"]
    first = [e for e in wk if e.get("event") == "first_act"]
    vis = [e for e in ok if e.get("visible") == 1]
    vis_nospark = [e for e in vis if e.get("sparkle") != 1]
    paths = Counter(e.get("path") for e in ok)
    # gated attempts on the same guid in the 15 s before each ok wake
    gb = defaultdict(list)
    for e in gated:
        gb[e.get("guid")].append(e["ts"])
    pre_gated = [sum(1 for t in gb.get(e.get("guid"), []) if e["ts"] - 15000 <= t < e["ts"]) for e in ok]
    # client pop-ins: bot see_appear inside the inner viewport (|dx|<=6,|dy|<=4, same z) with no
    # login sparkle (see_effect id=11 on that tile within +-600 ms) => appeared out of nowhere
    p1 = sorted(r.cli.get("p1", []), key=lambda e: e["ts"])
    me, sparks, appears = None, [], []
    for e in p1:
        k = e.get("k")
        if k == "self_pos" and e.get("to"):
            me = e["to"]
        elif k == "see_effect" and e.get("id") == 11:
            sparks.append((e["ts"], e.get("x"), e.get("y"), e.get("z")))
        elif k == "see_appear" and e.get("ctype") == "player" and is_bot_ev(e) and me and e.get("pos"):
            p = e["pos"]
            dx, dy = p["x"] - me["x"], p["y"] - me["y"]
            appears.append((e["ts"], e.get("name"), p, dx, dy, p["z"] == me["z"]))
    t0 = min((e["ts"] for e in p1 if e.get("k") == "login_ok"), default=0)
    inner, popins, sparkle_in = 0, [], 0
    for ts, n, p, dx, dy, samez in appears:
        if ts - t0 < 3000 or not samez:
            continue  # the login screen fill is not a pop-in
        if abs(dx) <= 6 and abs(dy) <= 4:
            inner += 1
            if any(abs(s[0] - ts) <= 600 and (s[1], s[2], s[3]) == (p["x"], p["y"], p["z"]) for s in sparks):
                sparkle_in += 1
            else:
                popins.append({"name": n, "dx": dx, "dy": dy, "pos": [p["x"], p["y"], p["z"]], "ts": ts})
    edge = sum(1 for a in appears if a[0] - t0 >= 3000 and a[5] and not (abs(a[3]) <= 6 and abs(a[4]) <= 4))
    # wake -> first client-seen step of that bot (first_act audit is NAMED-watch only, so with
    # `watch *` the client is the only first-move source); visible wakes only = on screen at wake
    mv, apc = defaultdict(list), defaultdict(list)
    for e in p1:
        if e.get("k") == "see_move" and e.get("cid"):
            mv[e["cid"]].append(e["ts"])
        elif e.get("k") == "see_appear" and e.get("cid"):
            apc[e["cid"]].append(e["ts"])
    w2m = []
    for e in vis:
        # the staging->landing teleport shows up as a see_move within ms of see_appear: skip it,
        # the first REAL step is a move >= 150 ms after the bot appeared
        ap = [x for x in apc.get(e.get("cid"), []) if e["ts"] - 1000 < x < e["ts"] + 5000]
        a0 = min(ap) if ap else e["ts"]
        t = [x for x in mv.get(e.get("cid"), []) if a0 + 150 < x < e["ts"] + 30000]
        if t:
            w2m.append(min(t) - e["ts"])
    nonvis_appear = []
    ap_by = defaultdict(list)
    for a in appears:
        ap_by[a[1]].append(a[0])
    for e in ok:
        if e.get("visible") != 1:
            t = [x for x in ap_by.get(e.get("bot"), []) if e["ts"] < x < e["ts"] + 30000]
            if t:
                nonvis_appear.append(min(t) - e["ts"])
    return {
        "wake_to_first_client_move_ms_p50": M(pct(w2m, .5), "ms", {"rel": 0.5, "min_abs": 300}, "lower",
                                              "prox quiet 200-800 ms", note=f"n={len(w2m)} (visible wakes)"),
        "wake_to_first_client_move_ms_max": M(max(w2m, default=None), "ms", {"rel": 0.5, "min_abs": 500}, "lower"),
        "offscreen_wake_to_walkin_ms_p50": M(pct(nonvis_appear, .5), "ms", {"rel": 0.5, "min_abs": 2000},
                                             note=f"n={len(nonvis_appear)}"),
        "gated_bots_distinct": M(len({e.get("guid") for e in gated}), "n"),
        "gated_per_sec": M(round(len(gated) / ((max(e["ts"] for e in gated) - min(e["ts"] for e in gated)) / 1000), 1)
                           if len(gated) > 1 else None, "1/s", {"rel": 0.5, "min_abs": 10}, "lower"),
        "wake_ok": M(len(ok), "n"),
        "wake_ok_prox": M(paths.get("prox", 0), "n"),
        "wake_ok_burst": M(paths.get("burst", 0), "n"),
        "wake_ok_explicit": M(paths.get("explicit", 0), "n"),
        "wake_gated_records": M(len(gated), "n", {"rel": 0.5, "min_abs": 50}, "lower"),
        "gated_per_ok_wake": M(round(len(gated) / len(ok), 1) if ok else None, "ratio", {"rel": 0.5, "min_abs": 5}, "lower",
                               note="Phase 2 saw 5-10 gated per VISIBLE wake"),
        "gated_before_ok_p50": M(pct(pre_gated, .5), "n", {"abs": 5}, "lower", note="same guid, 15 s before its ok wake"),
        "gated_before_ok_max": M(max(pre_gated, default=None), "n", {"rel": 0.5, "min_abs": 10}, "lower"),
        "wake_visible": M(len(vis), "n"),
        "wake_sparkle": M(sum(1 for e in ok if e.get("sparkle") == 1), "n"),
        "wake_visible_no_sparkle": M(len(vis_nospark), "n", {"abs": 0}, "lower", "0: an on-screen wake must sparkle"),
        "first_act_ms_p50": M(pct([e.get("firstActMs") for e in first], .5), "ms", {"rel": 0.5, "min_abs": 200}, "lower",
                              "prox wakes 200-800 ms quiet"),
        "first_act_ms_p95": M(pct([e.get("firstActMs") for e in first], .95), "ms", {"rel": 0.5, "min_abs": 300}, "lower"),
        "client_bot_appears_after_login": M(sum(1 for a in appears if a[0] - t0 >= 3000), "n"),
        "client_edge_appears": M(edge, "n", note="appeared at the viewport edge = walk-in"),
        "client_inner_appears": M(inner, "n"),
        "client_inner_with_sparkle": M(sparkle_in, "n"),
        "client_popins": M(len(popins), "n", {"abs": 1}, "lower", "0: inner-viewport appear without a sparkle",
                           note=json.dumps(popins[:8])),
    }


# ------------------------------------------------------------------ B3 proximity weighting
def b3(r: Run):
    summ = r.journal(r"\[PROXBIAS\] summary")
    near = mid = far = elig = resv = 0
    for e in summ:
        d = dict(re.findall(r"(\w+)=(\d+)", e["msg"]))
        near += int(d.get("sel_near", 0)); mid += int(d.get("sel_mid", 0)); far += int(d.get("sel_far", 0))
        elig += int(d.get("hunt_near_eligible", 0)); resv += int(d.get("hunt_near_reserved", 0))
    sel = r.journal(r"\[PROXBIAS\] kind=")
    kinds = Counter(re.search(r"kind=(\w+)", e["msg"]).group(1) for e in sel)
    hib = r.journal(r"\[PROXBIAS\] hib_by_town")

    def t8(e):
        m = re.search(r"\bt8=(\d+)", e["msg"])
        return int(m.group(1)) if m else 0
    dens = r.journal(r"\[DENSITY\] periodic")

    def ring(e):
        m = re.search(r"counts_now=\[(\d+),(\d+),(\d+)\]", e["msg"])
        return [int(x) for x in m.groups()] if m else None
    cen = r.aud("census")

    def tw(e, k):
        v = (e.get("town") or {}).get("8") or (e.get("town") or {}).get(8)
        return v[k] if isinstance(v, list) and len(v) > k else None
    seen = {e.get("name") for e in r.cli.get("p1", []) if e.get("k") == "see_appear" and is_bot_ev(e)}
    tot = near + mid + far
    return {
        "proxbias_summaries": M(len(summ), "n"),
        "sel_near": M(near, "n"), "sel_mid": M(mid, "n"), "sel_far": M(far, "n"),
        "near_share": M(round(near / tot, 3) if tot else None, "ratio", {"abs": 0.15}, "higher",
                        "near-tier selections rise when an anchor exists"),
        "hunt_near_eligible": M(elig, "n"), "hunt_near_reserved": M(resv, "n"),
        "proxbias_selection_lines": M(dict(kinds), "map", {"abs": 0}),
        "hib_thais_first": M(t8(hib[0]) if hib else None, "n"),
        "hib_thais_last": M(t8(hib[-1]) if hib else None, "n"),
        "hib_thais_drift": M((t8(hib[-1]) - t8(hib[0])) if len(hib) > 1 else None, "n", {"abs": 15}, "none",
                             "mass-migration watch: Thais must not balloon"),
        "density_ring_first": M(ring(dens[0]) if dens else None, "list", {"abs": 0}),
        "density_ring_last": M(ring(dens[-1]) if dens else None, "list", {"abs": 0}),
        "census_thais_awake_first_last": M([tw(cen[0], 0), tw(cen[-1], 0)] if cen else None, "list", {"abs": 0}),
        "census_thais_hib_first_last": M([tw(cen[0], 1), tw(cen[-1], 1)] if cen else None, "list", {"abs": 0}),
        "unique_bots_seen": M(len(seen), "n", {"rel": 0.3, "min_abs": 5}, "higher"),
        "census_top_town_last": M(top_town(cen[-1]) if cen else None, "list", {"abs": 0},
                                  note="[town, awake+hib, share of reg]; uniform would be ~1/22"),
        "census_top_town_first": M(top_town(cen[0]) if cen else None, "list", {"abs": 0}),
    }


def top_town(e):
    t = e.get("town") or {}
    if not t:
        return None
    k, v = max(t.items(), key=lambda kv: sum(kv[1]))
    return [int(k), sum(v), round(sum(v) / max(1, e.get("reg") or 1), 3)]


# ------------------------------------------------------------------ B4 lure / kite
def b4(r: Run):
    h = r.vars.get("H", "")
    lure = [e for e in r.journal(r"LURE ENGAGE") if f"ENGAGE {h} " in e["msg"]]
    kite = [e for e in r.journal(r"\] KITE ") if f"KITE {h} " in e["msg"]]
    legs = [int(m.group(1)) for e in kite if (m := re.search(r"legs=(\d+)", e["msg"]))]
    reasons = Counter(re.search(r"reason=(\w+)", e["msg"]).group(1) for e in kite if "reason=" in e["msg"])
    packs = [int(m.group(1)) for e in lure if (m := re.search(r"pack=(\d+)", e["msg"]))]
    pr = [e for e in r.aud("probe") if e.get("bot") == h]
    patrol = [e for e in pr if e.get("ph") == "PATROLLING"]
    mins = (patrol[-1]["ts"] - patrol[0]["ts"]) / 60000 if len(patrol) > 1 else 0
    cam = r.cli.get("c1", [])
    hp = [e.get("hp") for e in cam if e.get("k") == "see_self" and e.get("hp") is not None]
    hpmax = max([e.get("hpmax") for e in cam if e.get("hpmax")], default=None)
    phases = Counter(e.get("ph") for e in pr)
    return {
        "probe_samples": M(len(pr), "n"),
        "phase_hist": M(dict(phases), "map", {"abs": 0}),
        "patrolling_minutes": M(round(mins, 2), "min", {"rel": 0.5, "min_abs": 1}),
        "lure_engagements": M(len(lure), "n", {"rel": 0.5, "min_abs": 2}),
        "lure_pack_p50": M(pct(packs, .5), "n", {"abs": 2}),
        "kite_runs": M(len(kite), "n", {"rel": 0.5, "min_abs": 2}),
        "kite_legs_total": M(sum(legs), "n", {"rel": 0.5, "min_abs": 3}),
        "kite_reversals_per_min": M(round(sum(legs) / mins, 2) if mins else None, "1/min", {"rel": 0.5, "min_abs": 1}, "lower",
                                    "ping-pong watch (memory: legs=7 in 4 s seen before)"),
        "kite_reasons": M(dict(reasons), "map", {"abs": 0}),
        "cam_hp_min": M(min(hp, default=None), "hp", {"rel": 0.5, "min_abs": 100}, "higher"),
        "cam_hpmax": M(hpmax, "hp", {"abs": 0}),
        "cam_effects_seen": M(sum(1 for e in cam if e.get("k") == "see_effect"), "n"),
        "cam_missiles_seen": M(sum(1 for e in cam if e.get("k") == "see_missile"), "n"),
        "cam_damage_numbers": M(sum(1 for e in cam if e.get("k") == "see_text" and e.get("kind") == "anim"), "n"),
    }


# ------------------------------------------------------------------ B5 routing
ARRIVE_R = 3  # goto lands on a random free tile near the goal (bot_nav random offset): Chebyshev <= 3 on the goal floor
GOAL_RE = re.compile(r"B5 (G\d) (\S+) (\d+),(\d+),(\d+)")


def b5(r: Run):
    bot = r.vars.get("R", "")
    marks = [(ts, GOAL_RE.search(t)) for ts, t in r.marks()]
    goals = [(ts, m) for ts, m in marks if m]
    end_ts = next((ts for ts, t in r.marks() if t.startswith("B5 end")), None) or (r.window()[1] or 9e18)
    pr = sorted((e for e in r.aud("probe") if e.get("bot") == bot), key=lambda e: e["ts"])
    paths = [e for e in r.aud("path") if e.get("bot") == bot]
    outs = [e for e in r.aud("pathout") if e.get("bot") == bot]
    per, arrived = {}, 0
    for i, (ts, m) in enumerate(goals):
        nxt = goals[i + 1][0] if i + 1 < len(goals) else end_ts
        g = (int(m.group(3)), int(m.group(4)), int(m.group(5)))
        arr = None
        moves, last = 0, None
        for e in pr:
            if not ts <= e["ts"] < nxt:
                continue
            p = e.get("pos") or {}
            q = (p.get("x"), p.get("y"), p.get("z"))
            if last and q != last:
                moves += 1
            last = q
            if arr is None and q[2] == g[2] and max(abs(q[0] - g[0]), abs(q[1] - g[1])) <= ARRIVE_R:
                arr = e["ts"]
        pw = [e for e in paths if ts <= e["ts"] < nxt]
        ow = [e for e in outs if ts <= e["ts"] < nxt]
        final = last
        d = None
        if final:
            d = max(abs(final[0] - g[0]), abs(final[1] - g[1])) + (100 * abs(final[2] - g[2]))
        per[m.group(1)] = {
            "goal": m.group(2), "dst": list(g), "arrived": arr is not None,
            "t_ms": round(arr - ts) if arr else None, "final": list(final) if final else None, "final_dist": d,
            "paths": len(pw), "path_fail": sum(1 for e in pw if str(e.get("result", "")).startswith("fail")),
            "planners": dict(Counter(e.get("planner") for e in pw)),
            "path_ms_max": max([e.get("ms") or 0 for e in pw], default=None),
            "pathout": dict(Counter(e.get("outcome") for e in ow)),
            "osc": sum(1 for e in ow if e.get("outcome") == "osc"),
            "tile_changes": moves,
        }
        arrived += 1 if arr else 0
    allms = [e.get("ms") for e in paths if e.get("ms") is not None]
    out = {
        "goals": M(len(goals), "n", {"abs": 0}),
        "arrived": M(arrived, "n", {"abs": 0}, "higher"),
        "arrival_rate": M(round(arrived / len(goals), 2) if goals else None, "ratio", {"abs": 0.01}, "higher"),
        "path_searches": M(len(paths), "n"),
        "path_fail": M(sum(1 for e in paths if str(e.get("result", "")).startswith("fail")), "n", {"abs": 1}, "lower"),
        "planner_ms_p50": M(pct(allms, .5), "ms", {"rel": 1.0, "min_abs": 1}, "lower"),
        "planner_ms_max": M(max(allms, default=None), "ms", {"rel": 1.0, "min_abs": 5}, "lower"),
        "pathout_osc": M(sum(1 for e in outs if e.get("outcome") == "osc"), "n", {"abs": 0}, "lower"),
        "pathout_stuck": M(sum(1 for e in outs if e.get("outcome") == "stuck"), "n", {"abs": 0}, "lower"),
        "pathout_abandoned": M(sum(1 for e in outs if e.get("outcome") == "abandoned"), "n", {"abs": 0}, "lower"),
    }
    for k, v in per.items():
        out[f"{k}_{v['goal']}_t_ms"] = M(v["t_ms"], "ms", {"rel": 0.5, "min_abs": 5000}, "lower", note=json.dumps(v))
    return out


# ------------------------------------------------------------------ B6 perf (botperf result JSON)
def b6(paths: list[Path]):
    """Run-level medians over the botperf windows (same reduction as the Phase 2 perf-gate) + the
    worst ticks from perfphases. Thresholds = the measured run-level noise (perf-gate SKILL: cpu_cores
    relSD 8.2 %, body_cpu mean 40 %; Phase 2: S1 cpu_cores bimodal 0.012 vs 0.035 across restarts)."""
    def med(v):
        v = sorted(x for x in v if x is not None)
        return v[len(v) // 2] if v else None

    def g(d, *ks):
        for k in ks:
            if not isinstance(d, dict) or k not in d:
                return None
            d = d[k]
        return d
    out = {}
    for p in paths:
        j = json.loads(p.read_text(encoding="utf-8"))
        sc = j.get("scenario", "S?")
        ws = j.get("windows", [])
        ph = lambda w, n, f: next((x.get(f) for x in g(w, "perfphases", "phases") or [] if x.get("name") == n), None)
        worst = []
        for i, w in enumerate(ws):
            for t in g(w, "perfphases", "worst_ticks") or []:
                worst.append(dict(t, window=i + 1))
        worst.sort(key=lambda t: -(t.get("body_wall") or 0))
        cnt = Counter()
        for w in ws:
            for k, v in (g(w, "perfstat", "counters") or {}).items():
                cnt[k] += v
        jc = Counter()
        for w in ws:
            for k, v in (g(w, "journal", "counts") or {}).items():
                jc[k] += v if isinstance(v, int) else 0
        out.update({
            f"{sc}_windows": M(len(ws), "n", {"abs": 0}),
            f"{sc}_awake_med": M(med([g(w, "perfstat", "gauges", "awake") for w in ws]), "n", {"rel": 0.3, "min_abs": 3},
                                  note="population: compare only at similar awake counts"),
            f"{sc}_real_players": M(g(j, "achieved", "real_players"), "n", {"abs": 0}),
            f"{sc}_body_cpu_mean_ms": M(med([g(w, "perfstat", "body_cpu", "mean") for w in ws]), "ms/tick",
                                        {"rel": 0.4, "min_abs": 0.05}, "lower", note="run relSD ~40% (perf-gate)"),
            f"{sc}_body_cpu_max_ms": M(max([g(w, "perfstat", "body_cpu", "max") or 0 for w in ws], default=None), "ms",
                                       {"rel": 1.0, "min_abs": 5}, "lower"),
            f"{sc}_body_wall_mean_ms": M(med([g(w, "perfstat", "body_wall", "mean") for w in ws]), "ms/tick",
                                         {"rel": 0.5, "min_abs": 0.1}, "lower"),
            f"{sc}_body_wall_p95_ms": M(med([g(w, "perfstat", "body_wall", "p95") for w in ws]), "ms (bucket edge)",
                                        {"rel": 1.0, "min_abs": 3}, "lower"),
            f"{sc}_body_wall_max_ms": M(max([g(w, "perfstat", "body_wall", "max") or 0 for w in ws], default=None), "ms",
                                        {"rel": 1.0, "min_abs": 20}, "lower"),
            f"{sc}_cpu_cores_med": M(med([w.get("cpu_cores") for w in ws]), "cores", {"rel": 0.35, "min_abs": 0.01}, "lower",
                                     note="S1 is bimodal across restarts (#112)"),
            f"{sc}_virtual_tick_mean_ms": M(med([ph(w, "virtual_tick", "mean") for w in ws]), "ms", {"rel": 0.5, "min_abs": 0.1}, "lower"),
            f"{sc}_bot_loop_mean_ms": M(med([ph(w, "bot_loop", "mean") for w in ws]), "ms", {"rel": 0.5, "min_abs": 0.05}, "lower"),
            f"{sc}_bot_loop_max_ms": M(max([ph(w, "bot_loop", "max") or 0 for w in ws], default=None), "ms", {"rel": 1.0, "min_abs": 10}, "lower"),
            f"{sc}_tick_slow": M(cnt.get("tick_slow", 0), "n", {"rel": 1.0, "min_abs": 3}, "lower"),
            f"{sc}_gap_slow": M(cnt.get("gap_slow", 0), "n", {"rel": 1.0, "min_abs": 3}, "lower"),
            f"{sc}_budget_deferred": M(cnt.get("budget_deferred", 0), "n", {"rel": 1.0, "min_abs": 10}),
            f"{sc}_rss_mb_end": M(round((ws[-1].get("rss_kb_end") or 0) / 1024) if ws else None, "MB", {"rel": 0.15, "min_abs": 50}, "lower"),
            f"{sc}_journal_counts": M(dict(jc), "map", {"rel": 1.0}, note="threshold-log counts summed over windows"),
            f"{sc}_worst_ticks_top5": M(worst[:5], "list", {"rel": 1.0}, note="perfphases worst_ticks, by body_wall"),
        })
    return out


# ------------------------------------------------------------------ B7 chat
def b7(r: Run):
    p1 = sorted(r.cli.get("p1", []), key=lambda e: e["ts"])
    vs = r.verdicts()
    says, pms = [], []
    for line, v in vs:
        t = v.get("text", "")
        if re.match(r"@p1 say ", line) and t.startswith("OK"):
            says.append((v["ts"], line[8:]))
        elif re.match(r"@p1 pm ", line) and t.startswith("OK"):
            m = re.search(r"to=(.+)$", t)
            pms.append((v["ts"], m.group(1).strip() if m else "?"))
    talk = [e for e in p1 if e.get("k") == "see_talk" and e.get("name") != "Harness One"]
    # see_talk carries no cid: a speaker is a bot when p1 saw that name appear as a bot
    botnames = {e.get("name") for e in p1 if e.get("k") in ("see_appear", "see_creature") and is_bot_ev(e)}
    for e in talk:
        if e.get("name") in botnames:
            e["bot"] = True
    lat, texts, rep_who = [], [], []
    for i, (ts, txt) in enumerate(says):
        nxt = says[i + 1][0] if i + 1 < len(says) else ts + 15000
        rs = [e for e in talk if ts < e["ts"] < min(nxt, ts + 9000) and e.get("channel", 0) == 0 and is_bot_ev(e)
              and e.get("mode") in (1, None)]
        if rs:
            lat.append(rs[0]["ts"] - ts)
            texts.append(rs[0].get("text"))
            rep_who.append(rs[0].get("name"))
    plat, ptexts, pans = [], [], 0
    for i, (ts, who) in enumerate(pms):
        rs = [e for e in talk if e.get("name") == who and ts < e["ts"] < ts + 11000]
        if rs:
            pans += 1
            plat.append(rs[0]["ts"] - ts)
            ptexts.append(rs[0].get("text"))
    hibst = [v.get("text", "") for line, v in vs if re.match(r'@srv cmd "\$PM\d" status', line)]
    in_spec = sum(1 for x in lat + plat if 1300 <= x <= 5600)
    allt = texts + ptexts
    return {
        "local_says": M(len(says), "n", {"abs": 0}),
        "local_replies": M(len(lat), "n"),
        "local_reply_rate": M(round(len(lat) / len(says), 2) if says else None, "ratio", {"abs": 0.3}, "none",
                              "replyChancePct=60 when an idle bot is within 7 tiles; 60 s per-bot cooldown"),
        "local_latency_ms_p50": M(pct(lat, .5), "ms", {"abs": 1500}, spec="1500-5000 (+ client/net)"),
        "local_latency_ms_max": M(max(lat, default=None), "ms", {"abs": 2000}),
        "pm_targets_hibernated": M(sum(1 for t in hibst if "HIBERNATED" in t), "n", {"abs": 2},
                                   note=f"of {len(hibst)} status checks just before the PMs"),
        "pm_sent": M(len(pms), "n", {"abs": 0}),
        "pm_replies": M(pans, "n"),
        "pm_reply_rate": M(round(pans / len(pms), 2) if pms else None, "ratio", {"abs": 0.3}, "higher", "pmReplyChancePct=90"),
        "pm_latency_ms_p50": M(pct(plat, .5), "ms", {"abs": 1500}, spec="1500-5000"),
        "replies_latency_in_spec": M(round(in_spec / len(lat + plat), 2) if lat + plat else None, "ratio", {"abs": 0.2}, "higher"),
        "reply_texts_distinct_ratio": M(round(len(set(allt)) / len(allt), 2) if allt else None, "ratio", {"abs": 0.3}, "higher",
                                        note=json.dumps(allt)[:600]),
        "distinct_local_responders": M(len(set(rep_who)), "n"),
        "bot_local_talk_total": M(sum(1 for e in talk if is_bot_ev(e) and e.get("channel", 0) == 0), "n",
                                  note="all bot local speech heard (banter + spells + replies)"),
    }


# ------------------------------------------------------------------ B8 party + trade
def b8(r: Run):
    det = r.journal(r"\[PINVITE\] detected")
    acc = r.journal(r"\[PINVITE\] accepted")
    dec = r.journal(r"\[PINVITE\] declined")
    lat = [int(m.group(1)) for e in acc if (m := re.search(r"latencyMs=(\d+)", e["msg"]))]
    vs = r.verdicts()
    inv = [(l, v) for l, v in vs if l.startswith("@p1 party invite")]
    tr = [(l, v) for l, v in vs if re.match(r"@p1 trade \$", l)]
    p1 = r.cli.get("p1", [])
    msgs = [e.get("text", "") for e in p1 if e.get("k") == "see_msg"]
    joined = [t for t in msgs if re.search(r"joined", t, re.I)]
    town = sum(1 for e in p1 if e.get("k") == "trade_own")
    tclose = sum(1 for e in p1 if e.get("k") == "trade_close")
    # client-side accept latency: invite verdict -> "has joined the party" message
    clat = []
    for l, v in inv:
        j = [e for e in p1 if e.get("k") == "see_msg" and re.search(r"joined", e.get("text", ""), re.I) and v["ts"] < e["ts"] < v["ts"] + 30000]
        if j:
            clat.append(j[0]["ts"] - v["ts"])
    return {
        "invites_sent": M(sum(1 for _, v in inv if v.get("text", "").startswith("OK")), "n", {"abs": 0}),
        "pinvite_detected": M(len(det), "n"),
        "pinvite_accepted": M(len(acc), "n", {"abs": 1}, "higher"),
        "pinvite_declined": M(len(dec), "n", note="; ".join(re.sub(r".*\[PINVITE\] ", "", e["msg"]) for e in dec)[:300]),
        "accept_latency_ms_p50": M(pct(lat, .5), "ms", {"rel": 0.5, "min_abs": 2000}, "none", "botPartyInviteAccept{Min,Max}Ms"),
        "client_joined_msgs": M(len(joined), "n", {"abs": 1}, "higher"),
        "client_invite_to_joined_ms_p50": M(pct(clat, .5), "ms", {"rel": 0.5, "min_abs": 2000}),
        "trade_requests": M(sum(1 for _, v in tr if v.get("text", "").startswith("OK")), "n", {"abs": 0}),
        "trade_windows_opened": M(town, "n", {"abs": 0}, "higher", "own offer window on each request"),
        "trade_closed": M(tclose, "n", {"abs": 0}, "higher", "closed on cancel"),
        "party_msgs": M(len([t for t in msgs if re.search(r"part", t, re.I)]), "n", note=json.dumps([t for t in msgs if re.search(r"part|trade", t, re.I)])[:600]),
    }


# ------------------------------------------------------------------ B9 PvP poke
def b9(r: Run):
    pokes = []
    for ts, t in r.marks():
        m = re.match(r"B9 poke (\d+) (.+)$", t)
        if m:
            pokes.append((ts, int(m.group(1)), m.group(2).strip()))
    vs = r.verdicts()
    att = {}
    for l, v in vs:
        if l.startswith("@p1 attack $V"):
            k = int(re.search(r"\$V(\d+)", l).group(1))
            att[k] = v.get("text", "")
    st = r.aud("state")
    p1 = r.cli.get("p1", [])
    selfev = [e for e in p1 if e.get("k") == "see_self" and e.get("cond") is not None]
    fight = flee = ign = pk = 0
    rows = []
    for ts, k, name in pokes:
        if name == "none" or not att.get(k, "").startswith("OK"):
            rows.append({"k": k, "bot": name, "result": "not-attacked", "attack": att.get(k, "")[:60]})
            continue
        tr = [e for e in st if e.get("bot") == name and ts <= e["ts"] <= ts + 13000
              and e.get("to") in ("COMBAT", "FLEEING", "PK_ATTACK")]
        locked = any(ts <= e["ts"] <= ts + 13000 and re.search(r"swords|pzblock|pz_block|fight", str(e.get("cond"))) for e in selfev)
        to = tr[0]["to"] if tr else None
        res = {"COMBAT": "fight", "FLEEING": "flee", "PK_ATTACK": "pk_attack"}.get(to, "ignore")
        if res == "pk_attack":
            pk += 1
        elif res == "fight":
            fight += 1
        elif res == "flee":
            flee += 1
        else:
            ign += 1
        rows.append({"k": k, "bot": name, "result": res, "attacker_locked_seen": locked})
    relog = sum(1 for l, v in vs if l.startswith("@p1 login") and v.get("text", "").startswith("OK login"))
    n = fight + flee + ign + pk
    return {
        "pk_attack": M(pk, "n", note="bot went IDLE->PK_ATTACK: the vigilante/PK path (attacker now skulled), not onAttacked's COMBAT branch"),
        "retaliate_rate": M(round((fight + pk) / n, 2) if n else None, "ratio", {"abs": 0.25}, "none",
                            "outlevel spec: 0.17 fight"),
        "pokes_planned": M(len(pokes), "n", {"abs": 0}),
        "pokes_attacked": M(n, "n", {"abs": 3}, "higher"),
        "fight": M(fight, "n"), "flee": M(flee, "n"), "ignore": M(ign, "n"),
        "fight_rate": M(round(fight / n, 2) if n else None, "ratio", {"abs": 0.25}, "none",
                        "outlevel (all bots >= lv16 vs lv8): 0.17; else 0.50"),
        "flee_rate": M(round(flee / n, 2) if n else None, "ratio", {"abs": 0.25}, "none", "outlevel: 0.00; else 0.50"),
        "ignore_rate": M(round(ign / n, 2) if n else None, "ratio", {"abs": 0.25}, "none", "outlevel: 0.83"),
        "harness_logins": M(relog, "n", note="1 + re-logins after deaths"),
        "pokes_detail": M(rows, "list", {"abs": 0}),
    }


FN = {"b1": b1, "b2": b2, "b3": b3, "b4": b4, "b5": b5, "b7": b7, "b8": b8, "b9": b9}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario")
    ap.add_argument("src", nargs="+", help="run dir (b6: botperf result json(s))")
    ap.add_argument("--out")
    ap.add_argument("--verdict", default=None)
    ap.add_argument("--note", action="append", default=[])
    ap.add_argument("--build", default="canary b64cf2755c2e / libbot_engine.so f18493b79202 (251b94e99 + b5b16a8ec tools)")
    a = ap.parse_args()
    sc = a.scenario.lower()
    if sc == "b6":
        metrics = b6([Path(p) for p in a.src])
        runs, res = [Path(p).name for p in a.src], "botperf"
    else:
        r = Run(Path(a.src[0]))
        metrics = FN[sc](r)
        runs, res = [r.d.name], r.result_line()
    doc = {"scenario": sc.upper(), "date": "2026-11-18", "build": a.build, "runs": runs, "runner_result": res,
           "single_run": True, "verdict": a.verdict, "notes": a.note, "metrics": metrics}
    txt = json.dumps(doc, indent=1, default=str)
    if a.out:
        Path(a.out).write_text(txt + "\n", encoding="utf-8")
    print(txt)


if __name__ == "__main__":
    sys.exit(main())
