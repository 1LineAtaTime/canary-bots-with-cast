"""Movement-realism metrics (#198), from the harness streams OR from botnavsim shapepaths output.

One implementation for both, so the offline proof and the live @cast acceptance measure the same
thing. Definitions follow research report 70 (2026-11-23) section 3:
  - walk: consecutive same-floor adjacent moves, gaps <= 2.5 s, at least 6 tiles;
  - staircase index: turns on a walk that is monotone on both axes with >= 4 tiles on each
    (an L needs 1);
  - straight run: maximal run of steps in one direction;
  - corner clip: a 90-degree turn whose inside-corner tile (pos - v1 + v2) is blocked in the dump;
  - avoidable wall contact: a straight step on a tile with a blocked 8-neighbour while a
    perpendicular neighbour tile is walkable and wall-free;
  - same-OD overlap: walk pairs with origin and destination within 1 tile; identical tile
    sequence, or > 80 % shared edges.

usage:
  python -I movement_metrics.py --dump NAVDUMP --runs RUNDIR [RUNDIR ...] [--names a,b] [--json]
  python -I movement_metrics.py --dump NAVDUMP --navsim SHAPEPATHS.txt [--json]
  python -I movement_metrics.py --dump NAVDUMP --runs ... --extract-routes OUT.txt
"""
import argparse
import collections
import glob
import json
import math
import os
import struct
import sys

GAP_SPLIT_MS = 2500
DIRS = {(0, -1): 'N', (1, 0): 'E', (0, 1): 'S', (-1, 0): 'W', (1, -1): 'NE', (1, 1): 'SE', (-1, 1): 'SW', (-1, -1): 'NW'}
ANG = {'N': 0, 'NE': 45, 'E': 90, 'SE': 135, 'S': 180, 'SW': 225, 'W': 270, 'NW': 315}
VEC = {'N': (0, -1), 'S': (0, 1), 'E': (1, 0), 'W': (-1, 0)}


class Dump:
    def __init__(self, p):
        b = open(p, 'rb').read()
        _magic, _ver, self.x1, self.y1, self.z1, self.x2, self.y2, self.z2, n = struct.unpack_from('<IIiiiiiiI', b, 0)
        self.w, self.h = self.x2 - self.x1 + 1, self.y2 - self.y1 + 1
        self.flags = b[36:36 + 2 * n:2]

    def inside(self, x, y, z):
        return self.x1 <= x <= self.x2 and self.y1 <= y <= self.y2 and self.z1 <= z <= self.z2

    def walk(self, x, y, z):
        if not self.inside(x, y, z):
            return None
        i = (z - self.z1) * self.w * self.h + (y - self.y1) * self.w + (x - self.x1)
        return bool(self.flags[i] & 1)

    def blocked_nbrs(self, x, y, z):
        c = 0
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if dx == dy == 0:
                    continue
                w = self.walk(x + dx, y + dy, z)
                if w is None:
                    return None
                if not w:
                    c += 1
        return c


def walks_of(ms):
    ms = sorted(set(ms))
    cur, out = [], []
    for a in ms:
        if cur:
            p = cur[-1]
            adj = a[3] == p[3] and max(abs(a[1] - p[1]), abs(a[2] - p[2])) == 1
            if not adj or a[0] - p[0] > GAP_SPLIT_MS:
                if len(cur) >= 6:
                    out.append(cur)
                cur = []
        cur.append(a)
    if len(cur) >= 6:
        out.append(cur)
    return out


def load_runs(dirs, names=None):
    """-> list of (group, tiles[(x,y,z)], times[rx], sds) ; group = (run, name)."""
    streams = {}
    for d in dirs:
        for f in glob.glob(os.path.join(d, 'cli-*.ndjson')):
            run = os.path.basename(os.path.dirname(f))
            per = collections.defaultdict(list)
            for line in open(f, encoding='utf-8', errors='replace'):
                if '"see_move"' not in line and '"self_pos"' not in line:
                    continue
                try:
                    e = json.loads(line)
                except Exception:
                    continue
                if e['k'] == 'see_move' and e.get('ctype') != 'player':
                    continue
                t = e.get('to')
                n = e.get('name')
                if not t or not n or n.startswith('Harness') or (names and n not in names):
                    continue
                per[n].append((e['rx'], t['x'], t['y'], t['z'], e.get('sd') or 0))
            for n, ms in per.items():
                k = (run, n)
                if k not in streams or len(ms) > len(streams[k]):
                    streams[k] = ms
    W = []
    for (run, name), ms in streams.items():
        for w in walks_of(ms):
            W.append(((run, name), [(a[1], a[2], a[3]) for a in w], [a[0] for a in w], [a[4] for a in w]))
    return W


def load_navsim(p):
    W = []
    stats = None
    for line in open(p, encoding='utf-8', errors='replace'):
        if line.startswith('shapepaths:'):
            stats = line.strip()
        if not line.startswith('W '):
            continue
        head, _, body = line.partition(' : ')
        f = head.split()
        route, bot, ok = f[1], f[2], f[4]
        if ok != '1':
            continue
        tiles = [tuple(int(v) for v in t.split(',')) for t in body.split()]
        if len(tiles) >= 7:
            W.append(((route, bot), tiles, None, None))
    return W, stats


def q(a, p):
    a = sorted(a)
    return a[min(len(a) - 1, int(p * len(a)))] if a else float('nan')


def metrics(W, dump):
    R = collections.defaultdict(list)
    for grp, tiles, times, sds in W:
        steps = [DIRS[(b[0] - a[0], b[1] - a[1])] for a, b in zip(tiles, tiles[1:])]
        n = len(steps)
        R['steps'].append(n)
        R['diag'].append(sum(1 for d in steps if len(d) == 2))
        R['turns'].append(sum(1 for a, b in zip(steps, steps[1:]) if a != b))
        run = 1
        for a, b in zip(steps, steps[1:]):
            if a == b:
                run += 1
            else:
                R['runs'].append(run)
                run = 1
        R['runs'].append(run)
        dx, dy = tiles[-1][0] - tiles[0][0], tiles[-1][1] - tiles[0][1]
        if abs(dx) >= 4 and abs(dy) >= 4:
            sx = 1 if dx > 0 else -1
            sy = 1 if dy > 0 else -1
            mono = all((b[0] - a[0]) * sx >= 0 and (b[1] - a[1]) * sy >= 0 for a, b in zip(tiles, tiles[1:]))
            if mono:
                R['stair'].append(sum(1 for a, b in zip(steps, steps[1:]) if a != b))
        for i in range(1, len(steps)):
            d = abs(ANG[steps[i - 1]] - ANG[steps[i]]) % 360
            d = min(d, 360 - d)
            if d == 180:
                instant = True
                if times and sds and sds[i]:
                    instant = (times[i + 1] - times[i]) < 1.5 * sds[i]
                R['rev_inst' if instant else 'rev_pause'].append(1)
        if dump is None:
            continue
        for i in range(1, len(tiles) - 1):
            x, y, z = tiles[i]
            b = dump.blocked_nbrs(x, y, z)
            d = steps[i - 1]
            if b is None or len(d) != 1:
                continue
            vx, vy = VEC[d]
            px, py = -vy, vx
            R['wall_tiles'].append(b > 0)
            if b > 0:
                better = False
                for s in (1, -1):
                    t = (x + px * s, y + py * s, z)
                    if dump.walk(*t):
                        bb = dump.blocked_nbrs(*t)
                        if bb is not None and bb == 0:
                            better = True
                R['wall_avoidable'].append(better)
        for i, d in enumerate(steps):
            if len(d) == 2:
                x, y, z = tiles[i]
                ddx, ddy = tiles[i + 1][0] - x, tiles[i + 1][1] - y
                a1, a2 = dump.walk(x + ddx, y, z), dump.walk(x, y + ddy, z)
                if a1 is not None and a2 is not None:
                    R['diag_shave'].append((not a1) or (not a2))
        for i in range(1, len(steps)):
            d1, d2 = steps[i - 1], steps[i]
            if len(d1) == 1 and len(d2) == 1 and abs(ANG[d1] - ANG[d2]) % 180 == 90:
                x, y, z = tiles[i]
                v1, v2 = VEC[d1], VEC[d2]
                wc = dump.walk(x - v1[0] + v2[0], y - v1[1] + v2[1], z)
                if wc is not None:
                    R['corner'].append(not wc)
    # same-OD overlap
    def near(a, b):
        return a[2] == b[2] and max(abs(a[0] - b[0]), abs(a[1] - b[1])) <= 1
    long_w = [(g, tuple(t)) for g, t, _, _ in W if len(t) >= 11]
    same = [0, 0, 0]
    cross = [0, 0, 0]
    bystart = collections.defaultdict(list)
    for i, (_g, t) in enumerate(long_w):
        bystart[t[0]].append(i)
    cand = set()
    for i, (_g, t) in enumerate(long_w):
        x, y, z = t[0]
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for j in bystart.get((x + dx, y + dy, z), ()):
                    if j > i:
                        cand.add((i, j))
    for i, j in cand:
        if True:
            (ga, a), (gb, b) = long_w[i], long_w[j]
            if not (near(a[0], b[0]) and near(a[-1], b[-1])):
                continue
            e1, e2 = set(zip(a, a[1:])), set(zip(b, b[1:]))
            ov = len(e1 & e2) / max(1, min(len(e1), len(e2)))
            tgt = same if ga[1] == gb[1] else cross
            tgt[0] += 1
            tgt[1] += ov > 0.8
            tgt[2] += a == b
    nsteps = sum(R['steps'])
    out = collections.OrderedDict()
    out['walks'] = len(R['steps'])
    out['steps'] = nsteps
    out['diag_pct'] = 100.0 * sum(R['diag']) / max(1, nsteps)
    out['walks_with_diag_pct'] = 100.0 * sum(1 for d in R['diag'] if d) / max(1, len(R['diag']))
    out['turns_per_step'] = sum(R['turns']) / max(1, nsteps)
    out['stair_n'] = len(R['stair'])
    out['stair_median'] = q(R['stair'], .5)
    out['stair_mean'] = sum(R['stair']) / max(1, len(R['stair']))
    out['run1_pct'] = 100.0 * sum(1 for r in R['runs'] if r == 1) / max(1, len(R['runs']))
    out['run_median'] = q(R['runs'], .5)
    out['run_p90'] = q(R['runs'], .9)
    out['rev_inst_per_1k'] = 1000.0 * len(R['rev_inst']) / max(1, nsteps)
    out['corner_n'] = len(R['corner'])
    out['corner_clip_pct'] = 100.0 * sum(R['corner']) / max(1, len(R['corner']))
    out['diag_shave_pct'] = 100.0 * sum(R['diag_shave']) / max(1, len(R['diag_shave']))
    out['wall_avoidable_pct'] = 100.0 * sum(R['wall_avoidable']) / max(1, len(R['wall_avoidable']))
    out['same_od_pairs'] = same[0]
    out['same_identical_pct'] = 100.0 * same[2] / max(1, same[0])
    out['same_overlap80_pct'] = 100.0 * same[1] / max(1, same[0])
    out['cross_od_pairs'] = cross[0]
    out['cross_identical_pct'] = 100.0 * cross[2] / max(1, cross[0])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dump')
    ap.add_argument('--runs', nargs='*')
    ap.add_argument('--navsim')
    ap.add_argument('--names')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--extract-routes')
    ap.add_argument('--label', default='')
    a = ap.parse_args()
    dump = Dump(a.dump) if a.dump else None
    stats = None
    if a.navsim:
        W, stats = load_navsim(a.navsim)
    else:
        W = load_runs(a.runs or [], set(a.names.split(',')) if a.names else None)
    if a.extract_routes:
        seen = []
        with open(a.extract_routes, 'w', newline='\n') as fo:
            for g, t, _, _ in W:
                s, e = t[0], t[-1]
                if s[2] != e[2] or len(t) < 11 or not dump or not all(dump.inside(*p) for p in t):
                    continue
                if any(max(abs(s[0] - a0[0]), abs(s[1] - a0[1]), abs(e[0] - b0[0]), abs(e[1] - b0[1])) <= 1 and s[2] == a0[2] for a0, b0 in seen):
                    continue
                seen.append((s, e))
                fo.write('%d,%d,%d %d,%d,%d\n' % (s + e))
        print('routes written:', len(seen))
        return
    m = metrics(W, dump)
    if stats:
        m['navsim'] = stats
    if a.json:
        print(json.dumps(m))
    else:
        print(a.label + ' ' + ' '.join('%s=%s' % (k, ('%.2f' % v) if isinstance(v, float) else v) for k, v in m.items()))


if __name__ == '__main__':
    main()
