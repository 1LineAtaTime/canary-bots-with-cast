"""Convert OTC cavebot 1.3 .cfg files into SQL inserts for bot_hunt_scripts/waypoints/targets.

Conversion rules:
- goto:X,Y,Z         -> node
- use:X,Y,Z          -> use_with (extra_data NULL = use whatever item is on the tile)
- usewith:ITEM,X,Y,Z -> use_with (extra_data = ITEM_ID)
- npc:NAME,X,Y,Z     -> npc_interact (extra_data = NAME) — bot says "hi" (boat/travel NPC)
- TELEPORT:X,Y,Z / teleport:X,Y,Z -> teleport (force internalTeleport on advance)
- function:action levitate_<dir>_<up|down>  -> levitate_<up|down> at PREV wp pos, extra_data=face_<dir>
- function:levitate: <up|down>, <dir>       -> same (new OTC format; note reversed arg order)
- function:function:action ... (double "function:" prefix from cfg) -> same as above
- Dedupe consecutive identical (type,x,y,z,extra_data) waypoints.
- Phase from label: travel_to / hunt_patrol / travel_from (also accepts patrol_hunt typo).
- label:level: N+ / levels:N -> min_level=N
- label:monsters: a, b, c -> targets (optional)

Second-pass promotions (per phase):
- node -> teleport when Chebyshev gap to NEXT same-phase wp > TELEPORT_GAP_THRESHOLD
  (real in-game teleporter — the bot cannot walk it; A* caps at 512 nodes).
- node -> stand when next wp is a teleport, a use_with, a levitate, or on a different z
  (so the bot stops precisely before a floor change / interaction tile).
- First wp of travel_to and last wp of travel_from -> stand (route endpoints). hunt_patrol
  endpoints are intentionally left as node — the engine's wake/walk-back placement only
  accepts NODE candidates.
"""

import re
import sys
from pathlib import Path

# Old TibiaBot format: "levitate_east_up"  -> group(1)=dir, group(2)=updown
LEVITATE_RE = re.compile(
    r"levitate(?:_([nsew]|north|south|east|west))?_?(up|down)",
    re.IGNORECASE,
)
# New OTC format: "levitate: up, east"   -> group(1)=updown, group(2)=dir
LEVITATE_NEW_RE = re.compile(
    r"levitate\s*:\s*(up|down)\s*,\s*([nsew]|north|south|east|west)",
    re.IGNORECASE,
)
# Accepts "level: 350+", "level:650", "levels:450+" (plural, no space — Cobra cfg).
LEVEL_RE = re.compile(r"levels?\s*:?\s*(\d+)", re.IGNORECASE)

# Chebyshev gap above which two consecutive same-phase waypoints are treated as an
# in-game teleporter (force-teleport) rather than a walkable step. Max legitimate
# ramp/stair gap observed in the cfgs is ~7; the smallest real teleporter gap is ~68,
# so 15 is a safe separator.
TELEPORT_GAP_THRESHOLD = 15

# Coordinates inside any label comment ("(33341, 31167, 7) should be STAND" etc.)
COORD_TUPLE_RE = re.compile(r"\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)")

DIR_TO_FACE = {
    "n": "face_north", "north": "face_north",
    "s": "face_south", "south": "face_south",
    "e": "face_east",  "east":  "face_east",
    "w": "face_west",  "west":  "face_west",
}


def parse_cfg(filepath: Path):
    """Parse a .cfg file and return (min_level, monsters_list, raw_steps, stand_hints).

    raw_steps is a flat ordered list of dicts:
        {kind:'phase', phase:'travel_to'|'hunt_patrol'|'travel_from'}
        {kind:'goto'|'use'|'usewith'|'teleport', x, y, z, item_id?}
        {kind:'levitate', dir:'east', updown:'down'}

    stand_hints is a set of (x,y,z) tuples extracted from any label comment
    that contains the word 'STAND' — those waypoints will be forced to STAND
    type regardless of neighbouring z-context.
    """
    min_level = 1
    monsters = []
    steps = []
    stand_hints: set[tuple[int, int, int]] = set()
    pending_teleport = False  # set by `label:TELEPORT`, applied to next coord step

    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            # label:level: 150+
            if line.lower().startswith("label:level"):
                m = LEVEL_RE.search(line)
                if m:
                    min_level = int(m.group(1))
                continue

            # label:monsters: ...
            if line.lower().startswith("label:monsters"):
                rest = line.split(":", 2)
                if len(rest) >= 3:
                    monster_str = rest[2].strip()
                    monsters = [m.strip() for m in monster_str.split(",") if m.strip()]
                continue

            # label:TELEPORT — next coord-step becomes a teleport waypoint
            if line.lower().startswith("label:") and line.split(":", 1)[1].strip().lower() == "teleport":
                pending_teleport = True
                continue

            # label:travel_to / label:hunt_patrol / label:patrol_hunt / label:travel_from
            if line.lower().startswith("label:"):
                label_body = line.split(":", 1)[1].strip()
                phase_raw = label_body.lower()
                phase = None
                if phase_raw in ("travel_to", "to_hunt", "go_hunt"):
                    phase = "travel_to"
                elif phase_raw in ("hunt_patrol", "patrol_hunt", "patrol", "hunt"):
                    phase = "hunt_patrol"
                elif phase_raw in ("travel_from", "leave", "back", "exit"):
                    phase = "travel_from"
                if phase:
                    steps.append({"kind": "phase", "phase": phase})
                else:
                    # Unknown label = comment. Scan it for STAND hints with (x,y,z) tuples.
                    if "stand" in phase_raw:
                        for m in COORD_TUPLE_RE.finditer(label_body):
                            stand_hints.add((int(m.group(1)), int(m.group(2)), int(m.group(3))))
                continue

            # goto:X,Y,Z   (or teleport target if pending_teleport flag was just set)
            if line.lower().startswith("goto:"):
                coords = line.split(":", 1)[1].split(",")
                if len(coords) == 3:
                    kind = "teleport" if pending_teleport else "goto"
                    steps.append({"kind": kind,
                                  "x": int(coords[0]), "y": int(coords[1]), "z": int(coords[2])})
                    pending_teleport = False
                continue

            # use:X,Y,Z   (no item — bot uses whatever is on the tile)
            if line.lower().startswith("use:"):
                coords = line.split(":", 1)[1].split(",")
                if len(coords) == 3:
                    kind = "teleport" if pending_teleport else "use"
                    steps.append({"kind": kind,
                                  "x": int(coords[0]), "y": int(coords[1]), "z": int(coords[2])})
                    pending_teleport = False
                continue

            # usewith:ITEM,X,Y,Z
            if line.lower().startswith("usewith:"):
                parts = line.split(":", 1)[1].split(",")
                if len(parts) == 4:
                    steps.append({"kind": "usewith",
                                  "item_id": int(parts[0]),
                                  "x": int(parts[1]), "y": int(parts[2]), "z": int(parts[3])})
                    pending_teleport = False
                continue

            # npc:NAME,X,Y,Z — bot says "hi" at this tile (boat captain / travel NPC).
            if line.lower().startswith("npc:"):
                parts = line.split(":", 1)[1].split(",")
                if len(parts) == 4:
                    steps.append({"kind": "npc",
                                  "name": parts[0].strip(),
                                  "x": int(parts[1]), "y": int(parts[2]), "z": int(parts[3])})
                    pending_teleport = False
                continue

            # Legacy TELEPORT:X,Y,Z (older cfg format — direct destination)
            if line.upper().startswith("TELEPORT:"):
                coords = line.split(":", 1)[1].split(",")
                if len(coords) == 3:
                    steps.append({"kind": "teleport",
                                  "x": int(coords[0]), "y": int(coords[1]), "z": int(coords[2])})
                pending_teleport = False
                continue

            # function:action levitate_<dir>_<up|down>
            #  also: function:function:action levitate_<dir>_<up|down>
            if line.lower().startswith("function:"):
                # strip all leading "function:" prefixes
                rest = line
                while rest.lower().startswith("function:"):
                    rest = rest[len("function:"):]
                rest = rest.strip()
                # New OTC format: "levitate: up, east"  (updown first, dir second)
                mnew = LEVITATE_NEW_RE.search(rest)
                if mnew:
                    updown = mnew.group(1).lower()
                    direction = mnew.group(2).lower()
                    steps.append({"kind": "levitate",
                                  "dir": direction,
                                  "updown": updown})
                    continue
                # Old TibiaBot format: "action levitate_east_up" (dir first, updown second)
                if rest.lower().startswith("action "):
                    action_str = rest[len("action "):].strip().lower()
                    m = LEVITATE_RE.search(action_str)
                    if m:
                        direction = m.group(1) or ""
                        updown = m.group(2).lower()
                        steps.append({"kind": "levitate",
                                      "dir": direction.lower(),
                                      "updown": updown})
                # silently skip other action types we don't handle
                continue

            # config:{...} / extensions:{...} — ignore
            if line.lower().startswith(("config:", "extensions:")):
                continue

    return min_level, monsters, steps, stand_hints


def to_waypoints(steps, stand_hints=None):
    """Convert raw steps into phase-tagged waypoints with phase-local seq.

    Returns list of dicts:
       {phase, seq, type, x, y, z, extra_data, item_id}
    """
    current_phase = "hunt_patrol"  # default if no phase label yet
    by_phase = {"travel_to": [], "hunt_patrol": [], "travel_from": []}
    seen_hunt_patrol = False  # for typo correction: travel_to after hunt_patrol is really travel_from

    last_kind_xyz = None  # for dedupe: (kind, x, y, z)

    # First pass: build a flat list per phase
    for i, step in enumerate(steps):
        kind = step["kind"]

        if kind == "phase":
            phase = step["phase"]
            # Heuristic: if we hit "travel_to" AFTER a hunt_patrol section, treat as travel_from
            # (handles cfg typos like Nightmare_Isles.cfg that mislabels the leave path).
            if phase == "travel_to" and seen_hunt_patrol:
                phase = "travel_from"
            if phase == "hunt_patrol":
                seen_hunt_patrol = True
            current_phase = phase
            last_kind_xyz = None  # reset dedupe across phases
            continue

        if kind == "levitate":
            # Apply to the most-recent wp in current phase
            phase_wps = by_phase[current_phase]
            if not phase_wps:
                continue  # nothing to apply to
            prev = phase_wps[-1]
            direction = step.get("dir", "")
            face = DIR_TO_FACE.get(direction)
            updown = step["updown"]
            wp_type = "levitate_down" if updown == "down" else "levitate_up"
            phase_wps.append({
                "type": wp_type,
                "x": prev["x"], "y": prev["y"], "z": prev["z"],
                "extra_data": face,
                "item_id": None,
            })
            last_kind_xyz = (wp_type, prev["x"], prev["y"], prev["z"])
            continue

        if kind == "npc":
            wp_type = "npc_interact"
            extra = step.get("name") or "npc"
            item_id = None
            signature = (wp_type, step["x"], step["y"], step["z"])
            if signature != last_kind_xyz:
                by_phase[current_phase].append({
                    "type": wp_type,
                    "x": step["x"], "y": step["y"], "z": step["z"],
                    "extra_data": extra,
                    "item_id": item_id,
                })
                last_kind_xyz = signature
            continue

        if kind == "goto":
            wp_type = "node"
            extra = None
            item_id = None
        elif kind == "use":
            wp_type = "use_with"
            extra = None
            item_id = None
        elif kind == "usewith":
            wp_type = "use_with"
            extra = str(step["item_id"])
            item_id = step["item_id"]
        elif kind == "teleport":
            wp_type = "teleport"
            extra = None
            item_id = None
        else:
            continue

        signature = (wp_type, step["x"], step["y"], step["z"])
        if signature == last_kind_xyz:
            continue  # dedupe consecutive duplicate

        by_phase[current_phase].append({
            "type": wp_type,
            "x": step["x"], "y": step["y"], "z": step["z"],
            "extra_data": extra,
            "item_id": item_id,
        })
        last_kind_xyz = signature

    # Pre-pass (per phase): promote a node/stand whose Chebyshev gap to the NEXT same-phase
    # waypoint exceeds the teleport threshold to a TELEPORT (real in-game teleporter the bot
    # cannot walk). Per-phase ONLY — never across phase boundaries: the travel_to -> hunt_patrol
    # jump is handled by the engine's teleport-to-patrol-start, and promoting hunt_patrol[0] to
    # teleport would make the bot teleport home on every patrol-loop wrap instead of walking.
    def cheb(a, b):
        return max(abs(a["x"] - b["x"]), abs(a["y"] - b["y"]))

    for phase, wps in by_phase.items():
        for j in range(len(wps) - 1):
            cur, nxt = wps[j], wps[j + 1]
            if nxt["type"] in ("node", "stand") and cheb(cur, nxt) > TELEPORT_GAP_THRESHOLD:
                nxt["type"] = "teleport"

    # Second pass: promote a node->stand so the bot stops precisely on the trigger tile before
    # a floor change / teleporter / interaction. Runs AFTER the teleport pre-pass so a node
    # preceding a freshly-promoted teleport also becomes a stand.
    stand_hints = stand_hints or set()
    for phase, wps in by_phase.items():
        for j in range(len(wps)):
            cur = wps[j]
            # User STAND-hint override: any node at hinted coords forced to stand.
            if cur["type"] == "node" and (cur["x"], cur["y"], cur["z"]) in stand_hints:
                cur["type"] = "stand"
                continue
            if cur["type"] != "node":
                continue
            if j + 1 >= len(wps):
                continue
            nxt = wps[j + 1]
            if nxt["type"] in ("teleport", "use_with", "npc_interact",
                               "levitate_up", "levitate_down"):
                # Stand on the trigger tile before teleporting / using / talking / levitating.
                cur["type"] = "stand"
            elif nxt["z"] != cur["z"] and nxt["type"] in ("node", "stand"):
                # Floor change (stairs / ramp / hole) — stop before stepping onto it.
                cur["type"] = "stand"

    # Route endpoints: first wp of travel_to and last wp of travel_from become stand.
    # hunt_patrol endpoints are deliberately left as node (the engine's wake/walk-back
    # placement only accepts NODE candidates — a stand there could strand a revived bot).
    if by_phase["travel_to"] and by_phase["travel_to"][0]["type"] == "node":
        by_phase["travel_to"][0]["type"] = "stand"
    if by_phase["travel_from"] and by_phase["travel_from"][-1]["type"] == "node":
        by_phase["travel_from"][-1]["type"] = "stand"

    # Build final result with phase-local seq numbers
    result = []
    for phase in ("travel_to", "hunt_patrol", "travel_from"):
        for seq, wp in enumerate(by_phase[phase]):
            result.append({
                "phase": phase,
                "seq": seq,
                "type": wp["type"],
                "x": wp["x"], "y": wp["y"], "z": wp["z"],
                "extra_data": wp["extra_data"],
                "item_id": wp.get("item_id"),
            })
    return result



# ============================================================================
# AUTHORED-CSV OUTPUT  (BOT_CFG_INGEST, 2026-08-29)
#
# This converter used to emit `out.sql`. Since the BOT_CSV migration (2026-08-14) the
# engine loads authored data from data/bot/authored/*.csv and never reads those tables
# (bot_data.cpp: "authored data comes from data/bot/authored/ (CSV), not MySQL"), so the
# SQL output was writing into a void. The parser above is unchanged and still good; only
# the destination moved.
#
# The bridge back from MySQL (tools/bot_csv/export_from_mysql.py) cannot be used for this:
# it refuses to run because a re-export would silently delete min_monsters, a column that
# exists only in the CSV. So the converter writes the tree directly.
# ============================================================================

import argparse
import csv
import io
import os
import shutil
import subprocess
import tempfile

_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "tools" / "bot_csv"))
from csv_common import write_csv  # noqa: E402

CFG_DIR = _REPO_ROOT / "tools" / "cfg_importer" / "cfg"
MANIFEST = _REPO_ROOT / "tools" / "cfg_importer" / "cfg_manifest.csv"
AUTHORED = _REPO_ROOT / "data" / "bot" / "authored"
VALIDATOR = _REPO_ROOT / "tools" / "bot_csv" / "validate.py"

MANIFEST_HEADER = [
    "cfg_file", "name", "town_id", "town_name", "vocation_mask", "min_level", "max_level",
    "script_category", "is_quest", "keep_distance_ek", "keep_distance_ms",
    "keep_distance_ed", "keep_distance_rp", "min_monsters", "enabled", "skip_reimport",
]

SCRIPTS_HEADER = [
    "id", "name", "town_id", "min_level", "max_level", "vocation_mask",
    "keep_distance_ek", "keep_distance_ms", "keep_distance_ed", "keep_distance_rp",
    "enabled", "is_quest", "script_category", "source", "source_file", "town_name",
    "script_type", "min_monsters",
]
WP_HEADER = ["phase", "waypoint_type", "pos_x", "pos_y", "pos_z", "label", "extra_data"]
PHASES = ("travel_to", "hunt_patrol", "travel_from")

# The engine's waypoint_type enum (bot_csv.cpp / validate.py). Anything outside this set is
# a hard load error, and a hard load error means /cavebot reload brings up an engine with no
# authored data and no bot activates. The sibling parser family emits "conditional", which
# is exactly such a value, so the whitelist is checked rather than assumed.
WAYPOINT_TYPES = {
    "node", "stand", "ladder", "rope", "hole", "shovel", "stairs_up", "stairs_down",
    "door", "action", "levitate_up", "levitate_down", "machete", "use_with",
    "npc_interact", "teleport",
}

# Refuse an INFERRED town whose nearest anchor POI is further than this from travel_to[0].
# followWaypoints aborts a route above 200 tiles (bot_waypoint.cpp), measured from wherever
# PREPARING left the bot -- somewhere in town, near the depot -- to travel_to[0]. Anchoring
# on the POI itself approximates that position, so the threshold keeps a margin under 200.
TOWN_ANCHOR_MAX_DIST = 150

# POI types that a hunt route plausibly starts from. RoshamuulLow-style scripts begin at a
# dock, nearer a boat than a temple, so a temple-only anchor mis-measures them.
ANCHOR_POI_TYPES = ("temple", "depot", "depot_outside", "boat")


def has_level_label(path):
    """Does the cfg state a level at all?

    parse_cfg defaults min_level to 1, which is indistinguishable from a real
    `label:level: 1`. That distinction matters now that a drop ships enabled=1 with an
    uncapped max_level: a cfg with no level line would enter the eligible set of EVERY bot
    at EVERY level and instantly become the most-picked script on the server.
    """
    with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.lower().startswith("label:") and LEVEL_RE.search(line):
                return True
    return False


class IngestError(Exception):
    """A fatal problem with one cfg. Never partially written -- see write_tree()."""


# ---------------------------------------------------------------- cfg kind detection

def classify_cfg(path):
    """Return ('hunt'|'city_route'|'poi', detail).

    The OTC cavebot_configs directory holds three kinds of .cfg that are indistinguishable
    by extension: hunt scripts, city routes (which belong in city_routes.csv +
    city_route_waypoints/, an entirely different destination), and label-less POI routes.

    This check is load-bearing rather than defensive. Downstream checks do NOT catch a
    route cfg: parse_cfg discards a non-phase label as a comment, and to_waypoints starts
    with current_phase = "hunt_patrol", so every waypoint of Krailos.cfg lands in the patrol
    phase and the non-empty-patrol check passes happily.
    """
    labels = []
    with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.lower().startswith("label:"):
                labels.append(line.split(":", 1)[1].strip())

    # Reuse parse_cfg's own synonym set. If the two ever disagree, a file would classify as
    # a hunt and then parse with no phases, or vice versa.
    phase_words = {"travel_to", "to_hunt", "go_hunt", "hunt_patrol", "patrol_hunt",
                   "patrol", "hunt", "travel_from", "leave", "back", "exit"}
    for lb in labels:
        if lb.lower() in phase_words:
            return "hunt", f"{len(labels)} label(s), phase labels present"

    if not labels:
        return "poi", "no labels at all"
    pairs = [lb for lb in labels if "-" in lb]
    if pairs:
        return "city_route", f"{len(pairs)} src-dst label(s), e.g. '{pairs[0]}'"
    return "poi", f"{len(labels)} label(s), none of them phases"


# ---------------------------------------------------------------- manifest

def load_manifest():
    if not MANIFEST.exists():
        return []
    with io.open(MANIFEST, "r", encoding="utf-8") as fh:
        rows = [r for r in csv.DictReader(fh) if r.get("cfg_file")]
    for r in rows:
        for col in MANIFEST_HEADER:
            r.setdefault(col, "")
    return rows


def save_manifest(rows):
    write_csv(str(MANIFEST), MANIFEST_HEADER,
              [[r.get(c, "") for c in MANIFEST_HEADER] for r in rows])


def manifest_defaults(cfg_file, name, min_level, town_id, town_name):
    """A new script's row. Mirrors what emit_sql used to hardcode, minus the 9999.

    max_level=0 rather than 9999: the engine treats levelMax==0 as "no ceiling"
    (bot_hunt.cpp checks `levelMax > 0` first), so 0 is the same behavior stated honestly.
    """
    return {
        "cfg_file": cfg_file, "name": name,
        "town_id": str(town_id), "town_name": town_name,
        "vocation_mask": "15", "min_level": str(min_level), "max_level": "0",
        "script_category": "hunt", "is_quest": "0",
        "keep_distance_ek": "0", "keep_distance_ms": "3",
        "keep_distance_ed": "3", "keep_distance_rp": "3",
        "min_monsters": "0", "enabled": "1", "skip_reimport": "0",
    }


# ---------------------------------------------------------------- authored tree I/O

class AuthoredTree:
    """The data/bot/authored/ files this converter touches, loaded whole and rewritten whole."""

    def __init__(self, root):
        self.root = Path(root)
        with io.open(self.root / "hunt_scripts.csv", encoding="utf-8") as fh:
            self.scripts = [r for r in csv.DictReader(fh) if r.get("id")]
        with io.open(self.root / "hunt_targets.csv", encoding="utf-8") as fh:
            self.targets = [r for r in csv.DictReader(fh) if r.get("script_id")]
        with io.open(self.root / "meta.csv", encoding="utf-8") as fh:
            self.meta = {r["key"]: r["value"] for r in csv.DictReader(fh) if r.get("key")}
        self.waypoints = {}          # script_id -> list of wp dicts, lazily loaded
        self.dirty_waypoints = set()  # script ids whose file must be rewritten

    def by_source_file(self, source_file):
        for r in self.scripts:
            if r.get("source_file") == source_file:
                return r
        return None

    def next_id(self):
        return int(self.meta["next_script_id"])

    def allocate_id(self):
        sid = self.next_id()
        self.meta["next_script_id"] = str(sid + 1)
        return sid

    def load_waypoints(self, sid):
        """Read a script's waypoint file as it stands on disk (for pinned-script checks)."""
        path = self.root / "hunt_waypoints" / f"{sid}.csv"
        if not path.exists():
            return []
        with io.open(path, encoding="utf-8") as fh:
            return list(csv.DictReader(fh))

    # -- mutation ---------------------------------------------------------

    def put_script(self, sid, row):
        for i, r in enumerate(self.scripts):
            if int(r["id"]) == sid:
                self.scripts[i] = row
                return
        self.scripts.append(row)

    def put_waypoints(self, sid, wps):
        self.waypoints[sid] = wps
        self.dirty_waypoints.add(sid)

    def put_targets(self, sid, monsters):
        self.targets = [t for t in self.targets if int(t["script_id"]) != sid]
        for m in monsters:
            self.targets.append({"script_id": str(sid), "monster_name": m})

    # -- output -----------------------------------------------------------

    def write(self):
        """Rewrite every file this converter owns, in the exporter's byte format.

        Row order and banners mirror export_from_mysql.py exactly. The in-game
        /cavebot csv* editor agrees with csv_common's quoting rule, so a file written here
        and a file written there are byte-identical for the same content -- otherwise every
        import would produce spurious diffs against the editor's output.
        """
        self.scripts.sort(key=lambda r: int(r["id"]))
        write_csv(str(self.root / "hunt_scripts.csv"), SCRIPTS_HEADER,
                  [[r.get(c, "") for c in SCRIPTS_HEADER] for r in self.scripts])

        self.targets.sort(key=lambda t: int(t["script_id"]))
        write_csv(str(self.root / "hunt_targets.csv"), ["script_id", "monster_name"],
                  [[t["script_id"], t["monster_name"]] for t in self.targets])

        wp_dir = self.root / "hunt_waypoints"
        wp_dir.mkdir(parents=True, exist_ok=True)
        for sid in sorted(self.dirty_waypoints):
            wps = self.waypoints[sid]
            rows, banners, seen = [], [], None
            for w in wps:
                if w["phase"] != seen:
                    seen = w["phase"]
                    n = sum(1 for x in wps if x["phase"] == seen)
                    banners.append((len(rows),
                                    f"--- {seen} ({n} waypoint{'s' if n != 1 else ''}) ---"))
                rows.append([w["phase"], w["type"], w["x"], w["y"], w["z"], "",
                             w["extra_data"] if w["extra_data"] is not None else ""])
            write_csv(str(wp_dir / f"{sid}.csv"), WP_HEADER, rows, banners=banners)

        write_csv(str(self.root / "meta.csv"), ["key", "value"],
                  [["format_version", self.meta.get("format_version", "1")],
                   ["next_script_id", self.meta["next_script_id"]]])


# ---------------------------------------------------------------- town inference

def load_anchor_pois(tree_root):
    """Town anchors: the temple/depot/boat POIs a hunt route plausibly departs from."""
    anchors = {}
    with io.open(Path(tree_root) / "city_pois.csv", encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            if not r.get("name") or r.get("enabled") != "1":
                continue
            if r["poi_type"] not in ANCHOR_POI_TYPES:
                continue
            anchors.setdefault(int(r["town_id"]), []).append(
                (r["name"], r["poi_type"], int(r["pos_x"]), int(r["pos_y"])))
    return anchors


def infer_town(waypoints, anchors):
    """Nearest anchor POI to the route's first waypoint. Returns (town_id, dist, detail).

    followWaypoints aborts above 200 tiles from the current waypoint, measured x/y Chebyshev
    (z-blind), and tryStartHunt never teleports the bot to town -- it travels, PREPARING walks
    the depot and shops, and TRAVEL_TO starts from there. So travel_to[0] is what has to be
    near the town, and a wrong town_id yields a script that loads clean and aborts every run.

    With an empty travel_to the guard does not apply (the engine teleports to the patrol
    entry), so patrol[0] is used instead and the caller is told the result is low-confidence.
    """
    head = [w for w in waypoints if w["phase"] == "travel_to"]
    confident = True
    if not head:
        head = [w for w in waypoints if w["phase"] == "hunt_patrol"]
        confident = False
    if not head:
        return None, None, "no waypoints at all", False
    w0 = head[0]

    best = None
    for town_id, pois in anchors.items():
        for name, ptype, px, py in pois:
            d = max(abs(px - w0["x"]), abs(py - w0["y"]))
            if best is None or d < best[1]:
                best = (town_id, d, f"{ptype} '{name}'")
    if best is None:
        return None, None, "no anchor POIs loaded", False
    return best[0], best[1], best[2], confident


# ---------------------------------------------------------------- fatal checks

def check_waypoints(name, category, waypoints):
    """The engine's own requirements, none of which validate.py checks."""
    for w in waypoints:
        if w["type"] not in WAYPOINT_TYPES:
            raise IngestError(
                f"{name}: waypoint type '{w['type']}' is not one of the engine's 16 -- "
                "the whole authored load would fail and no bot would activate")

    counts = {p: sum(1 for w in waypoints if w["phase"] == p) for p in PHASES}
    if category in ("hunt", "quest"):
        if counts["hunt_patrol"] == 0:
            raise IngestError(
                f"{name}: no hunt_patrol waypoints. Every selection path skips a script with an "
                "empty patrol (bot_hunt.cpp) and beginHuntPhase aborts it, so it would load "
                "clean and simply never be picked. validate.py does not catch this.")
    elif category == "traveling":
        if counts["travel_to"] == 0:
            raise IngestError(f"{name}: a traveling script needs travel_to waypoints")
    return counts


def check_row_coherence(row, tree, sid):
    """Cross-field rules the engine cares about but the validator does not."""
    cat, is_quest = row["script_category"], row["is_quest"]
    if (cat == "quest") != (is_quest == "1"):
        raise IngestError(
            f"{row['name']}: script_category='{cat}' but is_quest={is_quest}. The engine gates "
            "on `isQuest || scriptCategory == \"quest\"`, so the two must agree.")

    # spawnGroup is derived as lower(name) (bot_data.cpp). Two scripts sharing a name share a
    # single 1-bot-per-spawn reservation, silently -- the validator does not check names.
    key = row["name"].strip().lower()
    for other in tree.scripts:
        if int(other["id"]) != sid and other["name"].strip().lower() == key:
            raise IngestError(
                f"{row['name']}: script id {other['id']} already uses the name '{other['name']}'. "
                "The engine derives the spawn-reservation key from lower(name), so the two would "
                "silently share one reservation and only one could ever hunt.")


# ---------------------------------------------------------------- staged validation

def run_validator(staged_root):
    """Run tools/bot_csv/validate.py against a STAGED copy of the tree.

    A subprocess, not an import: validate.py does `from csv_common import AUTHORED_DIR`, so
    the value binds once at first import and a single process can only ever see one tree.
    """
    env = dict(os.environ)
    env["BOT_AUTHORED_DIR"] = str(Path(staged_root).resolve())
    proc = subprocess.run([sys.executable, str(VALIDATOR)], env=env, cwd=str(_REPO_ROOT),
                          capture_output=True, text=True)
    return proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")


# ---------------------------------------------------------------- the import itself

def ingest(cfg_paths, manifest, tree, anchors, report):
    """Apply each cfg to `tree` in memory. Raises IngestError before anything is written."""
    by_file = {r["cfg_file"]: r for r in manifest}

    for cfg_path in cfg_paths:
        cfg_path = Path(cfg_path)
        cfg_file = cfg_path.name
        source_file = f"cfg/{cfg_file}"

        kind, detail = classify_cfg(cfg_path)
        if kind != "hunt":
            what = ("a city-route cfg (labels are src-dst pairs; it belongs in city_routes.csv "
                    "+ city_route_waypoints/, which this tool does not write)"
                    if kind == "city_route" else
                    "a POI-route or bookkeeping cfg (no phase labels)")
            raise IngestError(f"{cfg_file} is {what} -- {detail}. Refusing: importing it as a "
                              "hunt would put every waypoint in hunt_patrol and look fine.")

        row = by_file.get(cfg_file)
        pinned = row is not None and row.get("skip_reimport") == "1"

        min_level_cfg, monsters, steps, stand_hints = parse_cfg(cfg_path)
        waypoints = to_waypoints(steps, stand_hints)

        if row is None:
            # New cfg: infer what we can, then refuse rather than guess a town.
            town_id, dist, anchor, confident = infer_town(waypoints, anchors)
            if town_id is None or dist is None or dist > TOWN_ANCHOR_MAX_DIST:
                raise IngestError(
                    f"{cfg_file}: cannot place this route in a town "
                    f"(nearest anchor {anchor}, {dist} tiles away; limit {TOWN_ANCHOR_MAX_DIST}). "
                    "followWaypoints aborts a route above 200 tiles from its waypoint, so a "
                    "guessed town would ship a script that loads clean and fails every run. "
                    "Add a row to tools/cfg_importer/cfg_manifest.csv with the right town_id "
                    "and re-run.")
            if not has_level_label(cfg_path):
                raise IngestError(
                    f"{cfg_file}: no `label:level: N+` line, so min_level would default to 1. "
                    "A new script ships enabled=1 with no level ceiling, so that would make it "
                    "eligible for every bot at every level and the most-picked script on the "
                    "server. Add a level label to the cfg (or a manifest row with min_level) "
                    "and re-run.")
            town_name = next((t for t in _town_names(tree, town_id)), str(town_id))
            name = cfg_path.stem.replace("_", " ").replace("-", " -").strip()
            row = manifest_defaults(cfg_file, name, min_level_cfg, town_id, town_name)
            manifest.append(row)
            by_file[cfg_file] = row
            report.append(f"  NEW  {cfg_file}: '{name}' -> town {town_id} ({town_name}), "
                          f"level {min_level_cfg}+, anchor {anchor} at {dist} tiles"
                          + ("" if confident else " [LOW CONFIDENCE: no travel_to, used patrol[0]]"))

        existing = tree.by_source_file(source_file)
        sid = int(existing["id"]) if existing else tree.allocate_id()

        # The cfg's own level label is a DEFAULT for a new import, never an override: five
        # live scripts were hand-tuned away from it (Library's cfg says 700+, the row says
        # 1800), and re-running must not revert that.
        script_row = {
            "id": str(sid), "name": row["name"], "town_id": row["town_id"],
            "min_level": row["min_level"] or str(min_level_cfg),
            "max_level": row["max_level"] or "0",
            "vocation_mask": row["vocation_mask"] or "15",
            "keep_distance_ek": row["keep_distance_ek"] or "0",
            "keep_distance_ms": row["keep_distance_ms"] or "3",
            "keep_distance_ed": row["keep_distance_ed"] or "3",
            "keep_distance_rp": row["keep_distance_rp"] or "3",
            "enabled": row["enabled"] or "1", "is_quest": row["is_quest"] or "0",
            "script_category": row["script_category"] or "hunt",
            "source": "manual", "source_file": source_file,
            "town_name": row["town_name"], "script_type": "",
            "min_monsters": row["min_monsters"] or "0",
        }

        # A pinned script's row still syncs, but its WAYPOINTS are frozen -- so the checks
        # that depend on waypoint content must run against the frozen file, which is what
        # the engine will actually walk, not against this fresh (and differing) parse.
        if pinned and existing:
            frozen = tree.load_waypoints(sid)
            counts = {p: sum(1 for w in frozen if w["phase"] == p) for p in PHASES}
            if script_row["script_category"] in ("hunt", "quest") and counts["hunt_patrol"] == 0:
                raise IngestError(f"{row['name']} (pinned): frozen waypoint file has no patrol")
            report.append(f"  PIN  {cfg_file}: id {sid} '{row['name']}' -- row synced, "
                          f"waypoints frozen ({sum(counts.values())} on disk)")
        else:
            counts = check_waypoints(row["name"], script_row["script_category"], waypoints)
            tree.put_waypoints(sid, waypoints)
            tree.put_targets(sid, monsters)
            report.append(
                f"  {'UPD' if existing else 'ADD'}  {cfg_file}: id {sid} '{row['name']}' "
                f"level {script_row['min_level']}+ "
                f"wp={counts['travel_to']}/{counts['hunt_patrol']}/{counts['travel_from']} "
                f"targets={len(monsters) or 'all'}")

            # Geometry sanity for a manifest-supplied town: warn, never override. The author
            # may be right and the route odd; only a human can tell which.
            town_id, dist, anchor, _ = infer_town(waypoints, anchors)
            if town_id is not None and int(row["town_id"]) != town_id:
                report.append(f"       [town?] manifest says {row['town_id']}, geometry says "
                              f"{town_id} ({anchor}, {dist} tiles). Keeping the manifest.")

        tree.put_script(sid, script_row)
        check_row_coherence(script_row, tree, sid)


def _town_names(tree, town_id):
    for r in tree.scripts:
        if r.get("town_id") == str(town_id) and r.get("town_name"):
            yield r["town_name"]


# ---------------------------------------------------------------- driver

def write_tree(cfg_paths, dry_run):
    """Stage -> validate -> publish. Nothing is touched unless the validator passes."""
    manifest = load_manifest()
    report = []

    staging = tempfile.mkdtemp(prefix="botcsv_")
    staged_root = Path(staging) / "authored"
    try:
        shutil.copytree(AUTHORED, staged_root)
        tree = AuthoredTree(staged_root)
        anchors = load_anchor_pois(staged_root)

        ingest(cfg_paths, manifest, tree, anchors, report)
        tree.write()

        ok, out = run_validator(staged_root)
        if not ok:
            print("\n".join(report))
            print("\nVALIDATION FAILED against the staged tree -- nothing was written.\n")
            print(out.strip())
            return 1, report

        if dry_run:
            changed = _diff_trees(AUTHORED, staged_root)
            print("\n".join(report))
            print(f"\n[dry-run] validator PASSED. {len(changed)} file(s) would change:")
            for c in changed:
                print(f"    {c}")
            if not changed:
                print("    (none -- byte-identical round trip)")
            return 0, report

        for rel in _diff_trees(AUTHORED, staged_root):
            src, dst = staged_root / rel, AUTHORED / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        save_manifest(manifest)
        print("\n".join(report))
        print("\nvalidator PASSED -- data/bot/authored/ updated.")
        # checkout alone is NOT enough: a NEW script's hunt_waypoints/<id>.csv is untracked,
        # so it survives a checkout and is then an ORPHAN (a row-less waypoint file), which
        # the validator rejects -- the tree fails to load until it is removed. Undoing an
        # import needs both halves.
        print("Undo with: git checkout -- data/bot/authored "
              "&& git clean -f data/bot/authored/hunt_waypoints")
        return 0, report
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def _diff_trees(a, b):
    """Relative paths whose bytes differ between two authored trees (b's view)."""
    out = []
    for root, _dirs, files in os.walk(b):
        for fn in files:
            if not fn.endswith(".csv"):
                continue
            rel = os.path.relpath(os.path.join(root, fn), b)
            pa, pb = Path(a) / rel, Path(b) / rel
            if not pa.exists() or pa.read_bytes() != pb.read_bytes():
                out.append(rel.replace(os.sep, "/"))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(
        description="Import OTC cavebot 1.3 hunt .cfg files into data/bot/authored/.")
    ap.add_argument("cfgs", nargs="*",
                    help="loose .cfg paths (drag-and-drop). Omit to run the manifest.")
    ap.add_argument("--cfg-dir", default=str(CFG_DIR),
                    help="directory for manifest runs (default: the repo's tools/cfg_importer/cfg)")
    ap.add_argument("--only", help="comma-separated cfg filenames to import")
    ap.add_argument("--regenerate", help="comma-separated cfg filenames whose skip_reimport pin to lift")
    ap.add_argument("--dry-run", action="store_true", help="report and validate, write nothing")
    args = ap.parse_args()

    dropped = bool(args.cfgs)
    manifest = load_manifest()

    if args.regenerate:
        wanted = {n.strip() for n in args.regenerate.split(",") if n.strip()}
        for r in manifest:
            if r["cfg_file"] in wanted:
                r["skip_reimport"] = "0"
        save_manifest(manifest)
        print(f"Lifted skip_reimport on: {', '.join(sorted(wanted))}")
        manifest = load_manifest()

    if dropped:
        paths = []
        for p in args.cfgs:
            p = Path(p)
            if not p.exists():
                print(f"FAIL: {p} does not exist")
                return 1
            # Classify BEFORE copying. A refused file must not be left behind in the repo:
            # the point of the copy is to version the cfgs an import actually used.
            kind, detail = classify_cfg(p)
            if kind != "hunt":
                what = ("a city-route cfg (labels are src-dst pairs; it belongs in "
                        "city_routes.csv + city_route_waypoints/, which this tool does not write)"
                        if kind == "city_route" else
                        "a POI-route or bookkeeping cfg (no phase labels)")
                print(f"\nREFUSED: {p.name} is {what} -- {detail}. Importing it as a hunt would "
                      "put every waypoint in hunt_patrol and look fine. Nothing was copied.")
                return 1
            paths.append(p)
    else:
        wanted = None
        if args.only:
            wanted = {n.strip() for n in args.only.split(",") if n.strip()}
        paths = []
        for r in manifest:
            if wanted is not None and r["cfg_file"] not in wanted:
                continue
            p = Path(args.cfg_dir) / r["cfg_file"]
            if not p.exists():
                print(f"[WARN] missing: {p}")
                continue
            paths.append(p)

    if not paths:
        print("Nothing to import.")
        return 1

    try:
        rc, _ = write_tree(paths, args.dry_run)
    except IngestError as e:
        print(f"\nREFUSED: {e}")
        rc = 1

    # Version the cfgs only once the import actually succeeded, so a refused or failed drop
    # leaves nothing behind in the repo. The copy exists to make an import reproducible from
    # a clean clone -- it should only ever hold cfgs that really produced a script.
    if dropped and rc == 0 and not args.dry_run:
        CFG_DIR.mkdir(parents=True, exist_ok=True)
        for p in paths:
            dest = CFG_DIR / p.name
            if p.resolve() != dest.resolve():
                shutil.copy2(p, dest)
                print(f"Versioned {p.name} -> tools/cfg_importer/cfg/")

    # Hold the console open after a drag-and-drop, where Explorer's console vanishes with the
    # process and the report would be unreadable. BOTH streams must be interactive: a piped or
    # redirected run (a script, CI) has at least one that is not, and pausing there would hang
    # with the prompt invisible -- which it did, the first time this was tested.
    if dropped and sys.stdin.isatty() and sys.stdout.isatty():
        try:
            input("\nPress Enter to close...")
        except (EOFError, OSError):
            pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
