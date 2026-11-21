#!/usr/bin/env python3
"""Per-town delta of two town_tour.json files (town_tour_report.py output), e.g. prod (M0) vs port (P5 #160 V10).

    python tools/bottest/town_tour_delta.py runs/<a>/town_tour.json runs/<b>/town_tour.json --a prod --b port
"""
import argparse
import json
import statistics as st


def f(x, k1, k2=None):
    v = x.get(k1)
    if k2 is not None:
        v = (v or {}).get(k2)
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--a-label", "--a", dest="al", default="A")
    ap.add_argument("--b-label", "--b", dest="bl", default="B")
    x = ap.parse_args()
    A = {(r["town"], r["name"]): r for r in json.load(open(x.a, encoding="utf-8"))}
    B = {(r["town"], r["name"]): r for r in json.load(open(x.b, encoding="utf-8"))}
    cols = [("awake", lambda r: r.get("awake_gauge")),
            ("body_cpu mean ms", lambda r: f(r["perfstat"], "body_cpu", "mean")),
            ("body_cpu max ms", lambda r: f(r["perfstat"], "body_cpu", "max")),
            ("CYCLE_SLOW", lambda r: r["journal"]["cycle_slow"]["n"]),
            ("cpu cores", lambda r: r.get("cpu_cores")),
            ("RSS MB", lambda r: r.get("rss_mb"))]
    hdr = "| town | name | " + " | ".join(f"{c} {x.al} / {x.bl}" for c, _ in cols) + " |"
    out = [hdr, "|" + "---|" * (len(cols) + 2)]
    sums = {c: ([], []) for c, _ in cols}
    for k in sorted(set(A) | set(B), key=lambda k: (k[0], "settle" in k[1])):
        ra, rb = A.get(k), B.get(k)
        cells = []
        for c, g in cols:
            va = g(ra) if ra else None
            vb = g(rb) if rb else None
            if isinstance(va, (int, float)) and isinstance(vb, (int, float)):
                sums[c][0].append(va)
                sums[c][1].append(vb)
            fa = f"{va:.3g}" if isinstance(va, float) else str(va)
            fb = f"{vb:.3g}" if isinstance(vb, float) else str(vb)
            cells.append(f"{fa} / {fb}")
        out.append(f"| {k[0]} | {k[1]} | " + " | ".join(cells) + " |")
    out.append("")
    out.append("| summary (paired stops) | " + " | ".join(
        f"{c}: median {st.median(a):.3g} / {st.median(b):.3g}, max {max(a):.3g} / {max(b):.3g}" for c, (a, b) in sums.items() if a) + " |")
    print("\n".join(out))


if __name__ == "__main__":
    main()
