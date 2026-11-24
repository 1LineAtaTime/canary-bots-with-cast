"""live_check.py -- re-check generated city routes against the LIVE walker (#140).

The offline generator (`botnavsim poiroutes --emit DIR`) works on a static world dump. Before a
generated route replaces an authored one, every consecutive waypoint pair of it is walked through
`/cavebot route a b` (the z-planner + the same leg tracer goTo uses, on the live map: live doors,
houses, creatures), queued through the bot_commands table.

usage:
  python tools/bot_route_gen/live_check.py --gen DIR --db NAME --secrets FILE [--sample N] [--seed S]
         [--routes name1;name2] [--out results.json]

  --gen      directory holding generated_town_<id>.csv (from poiroutes --emit)
  --db       the game database whose bot_commands queue the server polls (NEVER a production DB
             you do not mean to drive)
  --secrets  file with DB_HOST / DB_USER / DB_PASS lines (e.g. .claude/secrets.local)
  --sample   check N routes picked at random (default: all)

A route PASSES when every pair reports `0 failed`. A `ladder` waypoint is the hop itself: the pair
(ladder -> landing) is the graph's portal and is skipped; the walk TO the ladder is checked.
Needs one awake bot on the server (the route command's pathfinding context).
"""
import argparse, csv, glob, json, os, random, re, subprocess, sys, time

MYSQL = os.environ.get("MYSQL_EXE", r"C:\Program Files\MySQL\MySQL Workbench 8.0 CE\mysql.exe")
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def load_env(path):
    env = {}
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            env[k.replace("export ", "").strip()] = v.strip().strip('"').strip("'")
    return env


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--secrets", required=True)
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--seed", type=int, default=140)
    ap.add_argument("--routes", default="")
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    env = load_env(a.secrets)

    def q(sql):
        r = subprocess.run([MYSQL, "-h", env["DB_HOST"], "-u", env["DB_USER"], f"-p{env['DB_PASS']}", a.db, "-N", "-B", "-e", sql],
                           capture_output=True, text=True, creationflags=NOWIN)
        if r.returncode != 0:
            raise SystemExit(r.stderr)
        return r.stdout

    routes = {}
    for f in sorted(glob.glob(os.path.join(a.gen, "generated_town_*.csv"))):
        with open(f, encoding="utf-8", newline="") as fh:
            for row in csv.reader(line for line in fh if line.strip() and not line.startswith("#")):
                if row[0] == "source_name" or len(row) < 5:
                    continue
                routes.setdefault(row[0], []).append((row[1], int(row[2]), int(row[3]), int(row[4])))
    names = sorted(routes)
    if a.routes:
        want = set(a.routes.split(";"))
        names = [n for n in names if n in want]
    if a.sample and a.sample < len(names):
        names = sorted(random.Random(a.seed).sample(names, a.sample))
    results = {}
    for name in names:
        wps = routes[name]
        pairs = []
        for (ta, xa, ya, za), (tb, xb, yb, zb) in zip(wps, wps[1:]):
            if ta == "ladder":
                continue  # the hop itself (portal ladder -> landing)
            pairs.append(f"{xa},{ya},{za} {xb},{yb},{zb}")
        if not pairs:
            continue
        ids = []
        for p in pairs:
            ids.append(int(q(f"INSERT INTO bot_commands (bot_name, command) VALUES ('_global', 'route {p}'); "
                             "SELECT LAST_INSERT_ID();").strip()))
        t0 = time.time()
        while time.time() - t0 < 300:
            n = int(q(f"SELECT COUNT(*) FROM bot_commands WHERE id BETWEEN {ids[0]} AND {ids[-1]} AND processed=1").strip())
            if n >= len(ids):
                break
            time.sleep(1)
        failed, tiles, detail = 0, 0, []
        for i, p in zip(ids, pairs):
            res = q(f"SELECT IFNULL(result,'') FROM bot_commands WHERE id={i}")
            m = re.search(r"TOTAL (\d+) tiles across (\d+) leg\(s\), (\d+) failed", res)
            if not m:
                failed += 1
                detail.append(f"{p}: {res.strip()[:160]}")
                continue
            tiles += int(m.group(1))
            if int(m.group(3)) > 0:
                failed += 1
                detail.append(f"{p}: {m.group(3)} failed leg(s)")
        results[name] = {"pairs": len(pairs), "failed": failed, "tiles": tiles, "detail": detail}
        print(f"{'PASS' if failed == 0 else 'FAIL'} {name} pairs={len(pairs)} liveTiles={tiles}" +
              ("".join("\n    " + d for d in detail)), flush=True)
    ok = sum(1 for r in results.values() if r["failed"] == 0)
    print(f"live_check: {ok}/{len(results)} routes pass")
    if a.out:
        json.dump(results, open(a.out, "w", encoding="utf-8"), indent=1)


if __name__ == "__main__":
    main()
