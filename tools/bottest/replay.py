#!/usr/bin/env python3
"""Replay a scenario's server-side expects against an EXISTING run directory, offline.

    python tools/bottest/replay.py scenarios/fix_121_yalahar_no_travel_sink.steps <run_dir> [--var R=Name]

Every `@srv expect` / `@srv expect-none` step is evaluated with the runner's own filter code
(Runner.parse_filters / ev_match) over the events the run recorded: server.ndjson (journal),
audit.pc.ndjson (audit stream) and cli-*.ndjson (clients). `within=` / `wait=` / `settle=` are
ignored -- the whole run is the window -- so this answers "would this check have passed on that
build?", which is how a fix shows its scenario FAILS on the pre-fix baseline (runs/bl_*).
Client-side joins (`join=move`) are skipped. Nothing is sent anywhere; read-only.
Exit 0 = all evaluated checks pass, 4 = at least one fails.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runner  # noqa: E402


def load(run: Path) -> list[dict]:
    evs: list[dict] = []
    files = [run / "server.ndjson", run / "audit.pc.ndjson", *sorted(run.glob("cli-*.ndjson"))]
    for f in files:
        if not f.exists():
            continue
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("{"):
                try:
                    evs.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return evs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("steps")
    ap.add_argument("run_dir")
    ap.add_argument("--var", action="append", default=[], metavar="NAME=VALUE")
    a = ap.parse_args()
    env = dict(v.split("=", 1) for v in a.var)
    _, steps = runner.parse_steps(Path(a.steps).read_text(encoding="utf-8"))
    evs = load(Path(a.run_dir))
    r = runner.Runner.__new__(runner.Runner)  # filters only; no I/O
    n2c = {e["name"]: e["cid"] for e in evs if e.get("name") and e.get("cid")}
    bad = 0
    for st in steps:
        if st.actor != "srv" or st.verb not in ("expect", "expect-none") or st.kw.get("join"):
            continue
        kw = {k: runner.subst(v, env) for k, v in st.kw.items()}
        fl = r.parse_filters(kw)
        got = sum(1 for e in evs if r.ev_match(e, fl, n2c))
        if st.verb == "expect-none":
            ok, want = got == 0, "=0"
        else:
            lo, hi = int(kw.get("min", 1)), (int(kw["max"]) if "max" in kw else None)
            ok = got >= lo and (hi is None or got <= hi)
            want = f">={lo}" + (f",<={hi}" if hi is not None else "")
        opt = kw.get("optional") in ("1", "true", "yes")
        tag = "PASS" if ok else ("SKIP(optional)" if opt else "FAIL")
        bad += 0 if ok or opt else 1
        print(f"{tag:14} line {st.lineno:3} got={got} want{want}  {st.verb} " +
              " ".join(f"{k}={v}" for k, v in kw.items() if k not in ("wait", "settle")))
    print(f"{len(evs)} events from {a.run_dir}; {'ALL PASS' if not bad else f'{bad} FAIL'}")
    return 4 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
