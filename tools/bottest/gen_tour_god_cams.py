#!/usr/bin/env python3
"""Generate the realistic-human live soak: a GM tours the 14 bot cities while @cast cameras watch bots.

    python tools/bottest/gen_tour_god_cams.py --parties "Bot A,Bot B,Bot C" [--hunters "Bot D,Bot E"]
                                              [--dwell 30] [--out tools/bottest/scenarios/tour_god_cams.steps]

What a real player + a few cast viewers would do, nothing synthetic:
  * p1 = the GOD character (group 6). Per city: `/town <id>` to the temple, then it WALKS (client goto,
    same floor) or `/pos`-teleports (other floor) to the depot and to a street/shop spot taken from the
    authored city-route endpoints (data/bot/authored/city_route_waypoints), then `/goto` the city cam's bot.
  * camC = one @cast camera per city on a bot the GM actually SEES moving near the temple (wait-see).
  * camA = a party-hunt member (--parties, one per rotation of ~1/3 of the tour; pick monks/healers).
  * camB = a solo hunter: `find-bot` (journal: last hunt phase entered PATROLLING, no HUNT:STUCK since),
    or the next --hunters name if given; rotated every 2 cities.
  Each sample is a `view / seen / view` triple per camera (cmp_seen-style ghost/missing diff), map shots at
  every stop, `walk-check` before every camera leaves (walk-N.json: revisits, A->B->A reversals), and a
  per-city perfreset/perfstat window. Audit: census + low-rate kinds for every bot (`watch *`), full kinds
  for the cam'd bots (named watches). Every engine-side step is optional: observation only.
Run: runner.py <out> --profile lxc-port-dropin --client-dir <Redemption 4.2 harness copy> --hidden
       --var GMPW=<GM account password>
"""
from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
# the 14 cities bots live in (players.town_id of account 65000): id -> (name, temple x,y,z)
CITIES = [
    (8, "Thais", 32369, 32241, 7), (6, "Carlin", 32360, 31782, 7), (5, "AbDendriel", 32732, 31634, 7),
    (9, "Venore", 32957, 32076, 7), (7, "Kazordoon", 32649, 31925, 11), (11, "Edron", 33217, 31814, 8),
    (13, "Darashia", 33213, 32454, 7), (10, "Ankrahmun", 33194, 32853, 8), (15, "Port Hope", 32594, 32745, 7),
    (14, "Liberty Bay", 32317, 32826, 7), (16, "Svargrond", 32212, 31132, 7), (17, "Yalahar", 32787, 31276, 7),
    (20, "Rathleton", 33594, 31899, 6), (22, "Issavi", 33921, 31477, 5),
]
STREET_PREF = ("potions", "runes", "bank", "tools", "food", "loot", "ammo", "trainers", "boat")


def route_endpoints(town_id: int) -> dict[str, tuple[int, int, int]]:
    f = REPO / f"data/bot/authored/city_route_waypoints/town_{town_id}.csv"
    routes: dict[str, list[tuple[int, int, int]]] = {}
    if f.exists():
        for r in csv.DictReader(open(f, encoding="utf-8")):
            try:
                routes.setdefault(r["source_name"], []).append((int(r["pos_x"]), int(r["pos_y"]), int(r["pos_z"])))
            except (KeyError, TypeError, ValueError):
                pass
    ends: dict[str, tuple[int, int, int]] = {}
    for name, wps in routes.items():
        m = re.match(r"[^|]+\|([^~:]+)~([^:]+):", name)
        if m and wps:
            ends.setdefault(m.group(2).lower(), wps[-1])
            ends.setdefault(m.group(1).lower(), wps[0])
    return ends


def move(L: list[str], label: str, here: tuple[int, int, int], to: tuple[int, int, int]) -> None:
    """A human walks when it can (same floor, < 60 tiles); otherwise the GM teleports."""
    d = max(abs(to[0] - here[0]), abs(to[1] - here[1]))
    if to[2] == here[2] and d <= 60:
        L.append(f"@p1 goto {to[0]},{to[1]},{to[2]} range=2 stuck=10 timeout=75 optional=1")
    L.append(f"@p1 say /pos {to[0]}, {to[1]}, {to[2]}")   # lands exactly (no-op distance if the walk arrived)
    L.append(f"@p1 note GM at {label} ({to[0]},{to[1]},{to[2]}) walk_d={d}")


def q(actor: str) -> str:
    return '"' + actor.replace('"', "") + '"'


def sample(L: list[str], cams: list[str], shot: str | None = None) -> None:
    """One cmp_seen triple per live camera: view / seen / view, back to back."""
    for c in cams:
        L.append(f"{q('@cam:' + c)} view optional=1")
        L.append(f"@srv cmd {q(c)} seen expect=\"SEEN\" optional=1")
        L.append(f"{q('@cam:' + c)} view optional=1")
        if shot:
            L.append(f"{q('@cam:' + c)} shot {shot} mode=map optional=1")


def cam_on(L: list[str], c: str) -> None:
    L.append(f"{q('@cam:' + c)} login optional=1")
    L.append(f"{q('@cam:' + c)} perceive turns=on items=on optional=1")
    L.append(f"@srv audit watch {q(c)} optional=1")
    L.append(f"@srv cmd {q(c)} status optional=1")


def cam_off(L: list[str], c: str, within: int) -> None:
    L.append(f"{q('@cam:' + c)} walk-check within={within} optional=1")
    L.append(f"@srv cmd {q(c)} status optional=1")
    L.append(f"{q('@cam:' + c)} perceive optional=1")
    L.append(f"{q('@cam:' + c)} logout")
    L.append(f"@srv audit unwatch {q(c)} optional=1")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--parties", required=True, help="comma list: party members for camA, one per rotation")
    ap.add_argument("--hunters", default="", help="comma list for camB (default: find-bot per rotation)")
    ap.add_argument("--dwell", type=int, default=30, help="hold per GM spot (temple/depot/street/bot)")
    ap.add_argument("--start", type=int, default=1, help="resume: first city number (1-based); rotations restart there")
    ap.add_argument("--cities", type=int, default=0, help="tour only this many cities (0 = all from --start)")
    ap.add_argument("--out", default=str(REPO / "tools/bottest/scenarios/tour_god_cams.steps"))
    a = ap.parse_args()
    parties = [p.strip() for p in a.parties.split(",") if p.strip()]
    hunters = [h.strip() for h in a.hunters.split(",") if h.strip()]
    n_city = len(CITIES)
    todo = CITIES[a.start - 1:]
    if a.cities > 0:
        todo = todo[:a.cities]
    per_party = -(-len(todo) // len(parties))
    L = [
        "# SCENARIO: tour_god_cams   SEED: 0   CAMERAS: auto   BOTCFG: auto",
        "# GENERATED by tools/bottest/gen_tour_god_cams.py - regenerate, do not hand-edit.",
        "# $CB<k> / $HB<k> are picked live (wait-see / find-bot); pre-set a fallback for each with runner --var,",
        "# or a missed pick aborts the run as INFRA (unknown variable).",
        f"# GM (GOD) tours {n_city} cities; camA party member ({', '.join(parties)}), camB solo hunter "
        "(find-bot / --hunters, every 2 cities), camC a bot the GM sees moving in each city. Observation only.",
        "@srv audit kinds probe,path,pathout,state,wake,hib,decide,cmd,census",
        "@srv audit watch clear",
        "@srv audit watch *",
        "@srv audit on expect=\"enabled=on\"",
        "@srv audit mark \"tour start\"",
        "@p1 login char=GOD acct=@god pw=$GMPW",
        # A group-6 character logs in as a ghost (stock login): bots and cameras would not see the GM at
        # all, so the tour would observe nothing a real player provokes. One toggle makes it visible.
        "@p1 say /ghost",
        "@p1 hold 2",
        "@p1 perceive optional=1",
    ]
    camA = camB = None
    hb_i = 0
    for i, (tid, name, tx, ty, tz) in enumerate(todo):
        k = a.start + i
        L += [f"# ===== city {k}/{n_city}: {name} (town {tid})", f"@srv audit mark \"city {k} {name}\"",
              "@srv cmd _global perfreset optional=1"]
        # camera rotations
        if i % per_party == 0:
            if camA:
                cam_off(L, camA, per_party * 300)
            camA = parties[min(i // per_party, len(parties) - 1)]
            L.append(f"@p1 note camA -> {camA}")
            cam_on(L, camA)
        if i % 2 == 0:
            if camB:
                cam_off(L, camB, 600)
            if hb_i < len(hunters):
                camB = hunters[hb_i]
            else:
                camB = f"$HB{k}"
                excl = ",".join(x for x in (camA,) if x)
                L.append(f"@srv find-bot since=1200 not={q(excl)} as=HB{k} optional=1")
            hb_i += 1
            L.append(f"@p1 note camB -> {camB}")
            cam_on(L, camB)
        ends = route_endpoints(tid)
        temple = (tx, ty, tz)
        depot = ends.get("depot")
        street = next((ends[s] for s in STREET_PREF if s in ends and ends[s] != depot
                       and max(abs(ends[s][0] - tx), abs(ends[s][1] - ty)) >= 8), None)
        if not street:   # towns with only temple/depot/boat routes: a street 12 tiles off the temple
            street = (tx + 12, ty, tz)
        # temple
        L += [f"@p1 say /town {tid}", f"@p1 note GM at temple {name} ({tx},{ty},{tz})", "@p1 hold 12",
              f"@p1 wait-see any-bot moving=1 timeout=40 not={q(','.join(x for x in (camA, camB) if x and not x.startswith('$')))} as=CB{k} optional=1",
              f"@p1 shot c{k:02d}_temple mode=map optional=1"]
        cb = f"$CB{k}"
        cam_on(L, cb)
        L.append(f"@p1 hold {a.dwell}")
        sample(L, [x for x in (cb, camA, camB) if x])
        here = temple
        if depot:
            move(L, "depot", here, depot)
            here = depot
            L.append(f"@p1 hold {a.dwell}")
            L.append(f"@p1 shot c{k:02d}_depot mode=map optional=1")
            sample(L, [cb])
        move(L, "street", here, street)
        L.append(f"@p1 hold {a.dwell}")
        L.append(f"@p1 shot c{k:02d}_street mode=map optional=1")
        sample(L, [x for x in (cb, camA, camB) if x], shot=f"c{k:02d}")
        # follow the city bot like a curious player
        L += [f"@p1 say /goto {cb}", f"@p1 note GM visits {cb}", f"@p1 hold {a.dwell}",
              f"@p1 shot c{k:02d}_bot mode=map optional=1"]
        sample(L, [cb])
        cam_off(L, cb, 300)
        L.append("@srv cmd _global perfstat optional=1")
    for c in (camA, camB):
        if c:
            cam_off(L, c, 600)
    L += ["@srv audit mark \"tour end\"", "@p1 logout", "@srv audit watch clear", "@srv audit off",
          "@srv expect-none src=srv msg~=\"Lua Script Error\" optional=1"]
    Path(a.out).write_text("\n".join(L) + "\n", encoding="utf-8", newline="\n")
    est = len(todo) * (12 + 25 + 4 * a.dwell + 60 + 25) // 60
    print(f"wrote {a.out}: {n_city} cities, {len(L)} steps, ~{est} min")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
