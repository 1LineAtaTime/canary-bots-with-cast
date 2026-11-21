#!/usr/bin/env python3
"""Per-town table for a p5_lxc_gm_town_tour run (P5 #160 V10).

    python tools/bottest/town_tour_report.py runs/<run_id> [--host-samples <tsv>] [--label prod] [--out file.md]

Per stop (from the audit mark "town <id> <name> (...)" to the next mark):
- census: total awake (median, max) and awake in the visited town (town[id][0], max) from audit `census`;
- engine: the perfstat JSON the stop's `@srv cmd _global perfstat` returned (ticks, tick/body ms, bot loop,
  slow ticks) - every key the engine reports is kept in the JSON output;
- journal: TICK_SLOW / GAP_SLOW / HB_STALL / PROCBOT_SLOW / CYCLE_SLOW counts and max ms in the window;
- host: process CPU cores and RSS from the server-side sampler TSV (epoch_ms, utime+stime ticks, rss_kb);
- client: received packets, max ping, and any game_end / conn_error in the window.
"""
from __future__ import annotations

import argparse
import json
import re
import statistics as st
from pathlib import Path

PAT = {
    "tick_slow": re.compile(r"\[TICK_SLOW\] body=(\d+)ms"),
    "gap_slow": re.compile(r"\[GAP_SLOW\] gap=(\d+)ms"),
    "hb_stall": re.compile(r"\[HB_STALL\].*delta=(\d+)ms"),
    "procbot_slow": re.compile(r"\[PROCBOT_SLOW\].*duration=(\d+)ms"),
    "cycle_slow": re.compile(r"\[CYCLE_SLOW\] total=(\d+)ms"),
}


def nd(p: Path):
    if not p.exists():
        return []
    out = []
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--host-samples")
    ap.add_argument("--label", default="")
    ap.add_argument("--out")
    a = ap.parse_args()
    r = Path(a.run)
    audit = nd(r / "audit.pc.ndjson") or nd(r / "audit.ndjson")
    srv = nd(r / "server.ndjson")
    cli = nd(r / "cli-p1.ndjson")
    ts = lambda e: e.get("srv_ts") or e.get("ts")
    marks = sorted([(ts(e), e["text"]) for e in audit if e.get("k") == "mark"])
    census = [e for e in audit if e.get("k") == "census"]
    perf = [(ts(e), e.get("reply", "")) for e in audit if e.get("k") == "cmd" and e.get("command") == "perfstat"]
    host = []
    if a.host_samples and Path(a.host_samples).exists():
        for line in Path(a.host_samples).read_text().splitlines():
            f = line.split()
            if len(f) >= 3 and f[0].isdigit():
                host.append((int(f[0]), int(f[1]), int(f[2])))
    # full perfstat JSON per stop: the cmd-N.txt files in order (the audit copy of the reply can be truncated)
    pfiles = sorted((f for f in r.glob("cmd-*.txt")), key=lambda f: int(f.stem.split("-")[1]))
    pjs = []
    for f in pfiles:
        t = f.read_text(encoding="utf-8", errors="replace")
        if '"cmd":"perfstat"' in t:
            try:
                pjs.append(json.loads(t[t.index("{"):].strip().splitlines()[0]))
            except ValueError:
                pjs.append({})
    rows = []
    stop = 0
    last_tid = 0
    for i, (t0, text) in enumerate(marks):
        m = re.match(r"town (\d+) (.*) \(", text)
        if m:
            tid, name = int(m.group(1)), m.group(2)
        elif text.startswith("settle tail"):
            tid, name = last_tid, "settle tail (last town, 60 s)"
        else:
            continue
        last_tid = tid
        t1 = marks[i + 1][0] if i + 1 < len(marks) else t0 + 120000
        cs = [c for c in census if t0 <= ts(c) < t1]
        if not cs:  # a 5 s stop is shorter than the census period (botAuditCensusSec): take the next record
            nxt = [c for c in census if t0 <= ts(c) < t0 + 12000]
            cs = nxt[:1]
        aw = [c.get("awake", 0) for c in cs]
        awt = [(c.get("town") or {}).get(str(tid), [0, 0])[0] for c in cs]
        pj = pjs[stop] if stop < len(pjs) else {}
        stop += 1
        jc = {k: [] for k in PAT}
        for e in srv:
            if t0 <= ts(e) < t1:
                for k, p in PAT.items():
                    mm = p.search(e.get("msg", ""))
                    if mm:
                        jc[k].append(int(mm.group(1)))
        # bracketing samples (the sampler runs every 5 s, a stop can be ~5 s long)
        before = [h for h in host if h[0] <= t0]
        after = [h for h in host if h[0] >= t1]
        hs = ([before[-1]] if before else []) + [h for h in host if t0 < h[0] < t1] + ([after[0]] if after else [])
        cores = ((hs[-1][1] - hs[0][1]) / 100) / ((hs[-1][0] - hs[0][0]) / 1000) if len(hs) >= 2 and hs[-1][0] > hs[0][0] else None
        rss = hs[-1][2] // 1024 if hs else None
        stats = [e for e in cli if e.get("k") == "stat" and t0 <= e["ts"] < t1]
        rx = [e.get("rx_packets") for e in stats if e.get("rx_packets") is not None]
        pings = [e.get("ping") for e in stats if isinstance(e.get("ping"), (int, float))]
        ends = [e.get("k") for e in cli if e.get("k") in ("game_end", "conn_error") and t0 <= e["ts"] < t1]
        rows.append({
            "town": tid, "name": name, "census_n": len(cs),
            "awake_gauge": (pj.get("gauges") or {}).get("awake"),
            "awake_med": st.median(aw) if aw else None, "awake_max": max(aw) if aw else None,
            "awake_town_max": max(awt) if awt else None,
            "perfstat": pj,
            "journal": {k: {"n": len(v), "max": max(v) if v else 0} for k, v in jc.items()},
            "cpu_cores": round(cores, 4) if cores is not None else None, "rss_mb": rss,
            "rx_packets": (rx[-1] - rx[0]) if len(rx) >= 2 else None, "ping_max": max(pings) if pings else None,
            "client_end": ends,
        })
    def h(p, name, field):
        v = (p.get(name) or {}).get(field)
        return "" if v is None else (f"{v:.2f}" if isinstance(v, float) else str(v))
    hdr = ("| town | name | awake (perfstat) | awake med/max (census) | awake in town (max) | body_cpu mean/p95/max ms | body_wall max ms | "
           "gap_wall max ms | virtual_tick mean ms | tick_slow/gap_slow (engine) | GAP_SLOW / HB_STALL max (journal) | "
           "CYCLE_SLOW | cpu cores | RSS MB | client rx | ping max | kick |")
    lines = [f"## GM town tour {a.label} ({r.name})", "", hdr, "|" + "---|" * 17]
    for x in rows:
        p = x["perfstat"]
        j = x["journal"]
        c = p.get("counters") or {}
        lines.append(
            f"| {x['town']} | {x['name']} | {x['awake_gauge']} | {x['awake_med']}/{x['awake_max']} | {x['awake_town_max']} | "
            f"{h(p, 'body_cpu', 'mean')}/{h(p, 'body_cpu', 'p95')}/{h(p, 'body_cpu', 'max')} | {h(p, 'body_wall', 'max')} | "
            f"{h(p, 'gap_wall', 'max')} | {h(p, 'virtual_tick', 'mean')} | {c.get('tick_slow', '')}/{c.get('gap_slow', '')} | "
            f"{j['gap_slow']['n']} / {j['hb_stall']['max'] or '-'} | {j['cycle_slow']['n']} | {x['cpu_cores']} | {x['rss_mb']} | "
            f"{x['rx_packets']} | {x['ping_max']} | {','.join(x['client_end']) or '-'} |")
    md = "\n".join(lines) + "\n"
    (Path(a.out) if a.out else r / "town_tour.md").write_text(md, encoding="utf-8")
    (r / "town_tour.json").write_text(json.dumps(rows, indent=1), encoding="utf-8")
    print(md)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
