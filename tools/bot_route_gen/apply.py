"""apply.py -- replace authored city routes with generated ones (#140).

usage:
  python tools/bot_route_gen/apply.py --gen DIR --live results.json [--authored data/bot/authored] [--dry-run]

Takes the generated routes from `botnavsim poiroutes --emit DIR` that PASSED `live_check.py`
(results.json) and swaps each one's waypoint group in
data/bot/authored/city_route_waypoints/town_<id>.csv for the generated group, in place (same position
in the file, the `# --- name (N waypoints) ---` comment rewritten). city_routes.csv is untouched: the
route keeps its name, town and `enabled` flag, and its first and last waypoint rows (which the loader
turns into the POI positions) are the authored ones verbatim.

City routes have no manifest or skip_reimport flag (no importer writes them; BOT_SYSTEM_DOCS.md §6c),
so the only discipline is the one the loader enforces: every route's rows stay contiguous.
"""
import argparse, csv, glob, io, json, os, re


def read_groups(path):
    """-> (header line, [(name or None, [lines])]) keeping comments/blank lines attached to the next group."""
    with open(path, encoding="utf-8", newline="") as f:
        lines = f.read().split("\n")
    header, body = lines[0], lines[1:]
    groups, pending, cur, curName = [], [], [], None
    for line in body:
        if not line.strip() or line.startswith("#"):
            if cur:
                groups.append((curName, cur))
                cur, curName = [], None
            pending.append(line)
            continue
        name = next(csv.reader([line]))[0]
        if name != curName and cur:
            groups.append((curName, cur))
            cur = []
        if not cur:
            cur = pending + [line]
            pending = []
        else:
            cur.append(line)
        curName = name
    if cur:
        groups.append((curName, cur))
    if pending:
        groups.append((None, pending))
    return header, groups


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", required=True)
    ap.add_argument("--live", required=True)
    ap.add_argument("--authored", default=os.path.join("data", "bot", "authored"))
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    live = json.load(open(a.live, encoding="utf-8"))
    passed = {n for n, r in live.items() if r["failed"] == 0}
    applied = 0
    for gf in sorted(glob.glob(os.path.join(a.gen, "generated_town_*.csv"))):
        town = re.search(r"generated_town_(\d+)\.csv", gf).group(1)
        _, ggroups = read_groups(gf)
        gen = {}
        for name, lines in ggroups:
            if name in passed:
                rows = [l for l in lines if l.strip() and not l.startswith("#")]
                gen[name] = [f"# --- {name} ({len(rows)} waypoints) --- generated #140"] + rows
        if not gen:
            continue
        target = os.path.join(a.authored, "city_route_waypoints", f"town_{town}.csv")
        header, groups = read_groups(target)
        out = [header]
        for name, lines in groups:
            if name in gen:
                lead = []
                for l in lines:
                    if l.strip() and not l.startswith("#"):
                        break
                    lead.append(l)
                keep = [l for l in lead if not l.startswith("# --- ")]  # blank lines / other comments stay
                out += keep + gen.pop(name)
                applied += 1
                print(f"town {town}: replaced {name}")
            else:
                out += lines
        for name in gen:
            print(f"town {town}: {name} not found in {target} -- skipped")
        if not a.dry_run:
            with open(target, "w", encoding="utf-8", newline="") as f:
                f.write("\n".join(out))
    print(f"apply: {applied} route(s) {'would be ' if a.dry_run else ''}replaced")


if __name__ == "__main__":
    main()
