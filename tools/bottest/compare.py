"""Compare two baseline dirs (tools/bottest/baselines/<date>/B#.json), metric by metric.

    python tools/bottest/compare.py tools/bottest/baselines/2026-11-18 tools/bottest/baselines/<new>

Single-run baselines (user rule): no noise model, no p-values. Each metric declares its own change
threshold in the JSON (`thr`: {"abs": X} or {"rel": R, "min_abs": M}); a delta beyond it is
CHANGED, flagged WORSE/BETTER when the metric declares `better` (lower|higher). Non-numeric values
(maps, lists) are CHANGED when they differ at all and the threshold is abs 0, else reported as INFO.
Exit 0 = nothing CHANGED, 1 = something changed (read it; it is a flag, not a verdict), 2 = usage.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def judge(old, new, m):
    thr = m.get("thr") or {}
    if old is None and new is None:
        return "PASS", ""
    if old is None or new is None:
        return "CHANGED", "missing on one side"
    if isinstance(old, (int, float)) and isinstance(new, (int, float)) and not isinstance(old, bool):
        d = new - old
        lim = thr["abs"] if "abs" in thr else max(abs(old) * thr.get("rel", 0.5), thr.get("min_abs", 0))
        if abs(d) <= lim:
            return "PASS", f"|d|<={lim:g}"
        tag = "CHANGED"
        b = m.get("better")
        if b in ("lower", "higher"):
            tag += " (WORSE)" if (d > 0) == (b == "lower") else " (BETTER)"
        return tag, f"|d|>{lim:g}"
    if old == new:
        return "PASS", "identical"
    return ("CHANGED", "differs") if thr.get("abs", None) == 0 else ("INFO", "differs (not gated)")


def fmt(v):
    s = json.dumps(v) if not isinstance(v, str) else v
    return s if len(s) <= 28 else s[:25] + "..."


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    a, b = Path(argv[1]), Path(argv[2])
    changed = 0
    for fa in sorted(a.glob("B*.json")):
        fb = b / fa.name
        if not fb.exists():
            print(f"\n## {fa.stem}: missing in {b}")
            changed += 1
            continue
        ja, jb = json.loads(fa.read_text(encoding="utf-8")), json.loads(fb.read_text(encoding="utf-8"))
        print(f"\n## {fa.stem}  old={ja.get('verdict')} {ja.get('runs')}  new={jb.get('verdict')} {jb.get('runs')}")
        print(f"{'metric':40s} {'old':>28s} {'new':>28s} {'delta':>10s}  verdict")
        ma, mb = ja.get("metrics", {}), jb.get("metrics", {})
        for k in list(ma) + [k for k in mb if k not in ma]:
            m = ma.get(k) or mb.get(k)
            ov = (ma.get(k) or {}).get("value")
            nv = (mb.get(k) or {}).get("value")
            v, why = judge(ov, nv, m)
            d = ""
            if isinstance(ov, (int, float)) and isinstance(nv, (int, float)) and not isinstance(ov, bool):
                d = f"{nv - ov:+g}"
            if v.startswith("CHANGED"):
                changed += 1
            print(f"{k:40s} {fmt(ov):>28s} {fmt(nv):>28s} {d:>10s}  {v} {why}")
    print(f"\n{'ALL PASS' if not changed else f'{changed} metric(s) CHANGED'} (single-run baselines: a flag to read, not a verdict)")
    return 1 if changed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
