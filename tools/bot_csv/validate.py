#!/usr/bin/env python3
"""Offline validator for data/bot/authored/ — mirrors the C++ parser in bot_csv.cpp.

This is the PRIMARY defense for the CSV migration. Because /cavebot reload
destroys the engine before the CSVs are parsed, a malformed file cannot fall
back to previous data: it poisons the engine and no bot activates until it is
fixed. Run this before every deploy, and after every hand-edit.

    python tools/bot_csv/validate.py

Exit 0 = every file parses and every cross-file rule holds. Exit 1 = the first
failure, reported as file:line[:col]: reason, in the same shape the engine uses.

Lenient (normalized silently): UTF-8 BOM, CRLF/LF/lone-CR/mixed, missing final
terminator, blank lines, '#' comment lines, whitespace around unquoted fields,
header order/case, '+' on integers, one trailing delimiter.
Strict (hard error): missing/unknown/duplicate header column, wrong field count,
non-numeric or out-of-range integer, unknown enum, unterminated quote, duplicate
key, orphan/missing per-script file, non-canonical or non-consecutive phase
blocks, route waypoint group with no route row.
"""

import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csv_common import AUTHORED_DIR, announce_authored_dir  # noqa: E402

WAYPOINT_TYPES = {
    "node", "stand", "ladder", "rope", "hole", "shovel", "stairs_up", "stairs_down",
    "door", "action", "levitate_up", "levitate_down", "machete", "use_with",
    "npc_interact", "teleport",
}
PHASES = ["travel_to", "hunt_patrol", "travel_from"]
POI_TYPES = {"depot", "depot_outside", "temple", "boat", "shop", "npc", "adventurer_stone"}


class CsvError(Exception):
    def __init__(self, path, line, col, reason):
        parts = [path]
        if line:
            parts.append(str(line))
            if col:
                parts.append(str(col))
        super().__init__(": ".join([":".join(parts), reason]))


def lex(path):
    """Yield (line_number, [fields]) records. Mirrors lexRecord() in bot_csv.cpp."""
    with io.open(path, "rb") as fh:
        raw = fh.read()
    if raw[:3] == b"\xef\xbb\xbf":
        raw = raw[3:]
    text = raw.decode("utf-8")
    n, pos, line = len(text), 0, 1

    while pos < n:
        # skip blank + '#' comment lines
        while pos < n:
            p = pos
            while p < n and text[p] in " \t":
                p += 1
            if p >= n:
                return
            if text[p] == "#":
                while p < n and text[p] not in "\r\n":
                    p += 1
                if p >= n:
                    return
            if text[p] in "\r\n":
                if text[p] == "\r" and p + 1 < n and text[p + 1] == "\n":
                    p += 1
                pos, line = p + 1, line + 1
                continue
            pos = p
            break
        if pos >= n:
            return

        fields, rec_line = [], line
        while True:
            ws = pos
            while ws < n and text[ws] in " \t":
                ws += 1
            if ws < n and text[ws] == '"':
                pos, field, closed = ws + 1, [], False
                while pos < n:
                    c = text[pos]
                    if c == '"':
                        if pos + 1 < n and text[pos + 1] == '"':
                            field.append('"')
                            pos += 2
                            continue
                        pos += 1
                        closed = True
                        break
                    if c == "\r":
                        field.append("\n")
                        pos += 1
                        if pos < n and text[pos] == "\n":
                            pos += 1
                        line += 1
                        continue
                    if c == "\n":
                        line += 1
                    field.append(c)
                    pos += 1
                if not closed:
                    raise CsvError(path, rec_line, len(fields) + 1, "unterminated quote")
                while pos < n and text[pos] in " \t":
                    pos += 1
                if pos < n and text[pos] not in ",\r\n":
                    raise CsvError(path, rec_line, len(fields) + 1,
                                   "content after closing quote (only whitespace is allowed there)")
                fields.append("".join(field))
            else:
                pos = ws
                start = pos
                while pos < n and text[pos] not in ",\r\n":
                    if text[pos] == '"' and pos != start:
                        raise CsvError(path, rec_line, len(fields) + 1,
                                       "quote inside unquoted field (quote the whole field instead)")
                    pos += 1
                fields.append(text[start:pos].strip(" \t"))
            if pos >= n:
                line += 1
                yield rec_line, fields
                return
            if text[pos] == ",":
                pos += 1
                continue
            if text[pos] == "\r" and pos + 1 < n and text[pos + 1] == "\n":
                pos += 2
            else:
                pos += 1
            line += 1
            break
        yield rec_line, fields


class Table:
    def __init__(self, path, required, optional=()):
        self.path, self.rows, self.col = path, [], {}
        recs = lex(path)
        try:
            hdr_line, hdr = next(recs)
        except StopIteration:
            raise CsvError(path, 0, 0, "empty file — expected a header row")
        if len(hdr) > 1 and hdr[-1] == "":
            hdr.pop()
        # Check the whole-header shape FIRST. A ';'- or tab-delimited file lands as one
        # giant cell, and the per-column loop below would report it as "unrecognized
        # header column 'phase;waypoint_type;...'" — technically true, useless to a human.
        # Delimiter sniffing stays rejected; a precise error is the substitute for it.
        if len(hdr) == 1 and (";" in hdr[0] or "\t" in hdr[0]):
            raise CsvError(path, hdr_line, 0,
                           "the header looks ';'- or tab-delimited; the field separator must be a comma")
        for i, name in enumerate(hdr):
            name = name.strip().lower()
            if not name:
                raise CsvError(path, hdr_line, i + 1, "empty header column name")
            if name.startswith("_"):
                continue
            if name in self.col:
                raise CsvError(path, hdr_line, i + 1, f"duplicate header column '{name}'")
            if name not in required and name not in optional:
                raise CsvError(path, hdr_line, i + 1,
                               f"unrecognized header column '{name}' "
                               "(prefix with '_' if it is a deliberate annotation column)")
            self.col[name] = i
        for r in required:
            if r not in self.col:
                extra = ""
                if len(hdr) == 1 and (";" in hdr[0] or "\t" in hdr[0]):
                    extra = " — the header looks ';'- or tab-delimited; the separator must be a comma"
                raise CsvError(path, hdr_line, 0, f"missing required header column '{r}'{extra}")
        expected = len(hdr)
        for ln, fields in recs:
            if len(fields) == expected + 1 and fields[-1] == "":
                fields.pop()
            if len(fields) != expected:
                raise CsvError(path, ln, 0, f"expected {expected} fields, got {len(fields)}")
            self.rows.append((ln, fields))

    def raw(self, row, col):
        i = self.col.get(col)
        return "" if i is None else self.rows[row][1][i]

    def line(self, row):
        return self.rows[row][0]

    def num(self, row, col, lo, hi):
        v = self.raw(row, col).strip()
        if not v:
            raise CsvError(self.path, self.line(row), 0, f"column '{col}': empty value in required integer column")
        body = v[1:] if v[0] in "+-" else v
        if not body or not body.isdigit():
            raise CsvError(self.path, self.line(row), 0, f"column '{col}': non-numeric value '{v}'")
        x = int(v)
        if x < lo or x > hi:
            raise CsvError(self.path, self.line(row), 0, f"column '{col}': value {x} out of range [{lo}..{hi}]")
        return x

    def num_or(self, row, col, lo, hi, default):
        """Optional integer: empty (a DB NULL on export) takes `default`.

        Parity: the SQL loaders call result->getNumber<uint16_t>(col), which
        yields 0 for NULL. 463 bot_equipment rows have a NULL slot_left
        (two-handed weapons, no shield), so requiring a value here would reject
        good production data.
        """
        if not self.raw(row, col).strip():
            return default
        return self.num(row, col, lo, hi)

    def enum(self, row, col, allowed):
        v = self.raw(row, col).strip().lower()
        if v not in allowed:
            raise CsvError(self.path, self.line(row), 0, f"column '{col}': unknown value '{v}'")
        return v

    def __len__(self):
        return len(self.rows)


def p(*parts):
    return os.path.join(AUTHORED_DIR, *parts)


def validate():
    checked = {}

    meta = Table(p("meta.csv"), ["key", "value"])
    kv = {meta.raw(i, "key"): meta.raw(i, "value") for i in range(len(meta))}
    for k in ("format_version", "next_script_id"):
        if k not in kv:
            raise CsvError(p("meta.csv"), 0, 0, f"missing required key '{k}'")
    next_script_id = int(kv["next_script_id"])
    checked["meta.csv"] = len(meta)

    scripts = Table(p("hunt_scripts.csv"),
                    ["id", "name", "town_id", "min_level", "max_level", "vocation_mask",
                     "keep_distance_ek", "keep_distance_ms", "keep_distance_ed",
                     "keep_distance_rp", "enabled", "is_quest", "script_category"],
                    # min_monsters (BOT_LURE_KITE) is CSV-only: it has no MySQL column,
                    # so it is optional here and absent from most rows.
                    ["source", "source_file", "town_name", "script_type", "min_monsters"])
    ids = set()
    for i in range(len(scripts)):
        sid = scripts.num(i, "id", 1, 2 ** 31)
        if sid in ids:
            raise CsvError(scripts.path, scripts.line(i), 0, f"duplicate script id {sid}")
        if sid >= next_script_id:
            raise CsvError(scripts.path, scripts.line(i), 0,
                           f"script id {sid} >= meta.csv next_script_id {next_script_id}")
        ids.add(sid)
        scripts.num(i, "town_id", 0, 2 ** 31)
        scripts.num(i, "min_level", 0, 100000)
        scripts.num(i, "max_level", 0, 100000)
        scripts.num(i, "vocation_mask", 0, 255)
        for c in ("keep_distance_ek", "keep_distance_ms", "keep_distance_ed", "keep_distance_rp"):
            scripts.num(i, c, 0, 255)
        if scripts.raw(i, "min_monsters").strip():
            scripts.num(i, "min_monsters", 0, 20)
        scripts.num(i, "enabled", 0, 1)
        scripts.num(i, "is_quest", 0, 1)
        scripts.enum(i, "script_category", {"hunt", "quest", "traveling"})
        if not scripts.raw(i, "name").strip():
            raise CsvError(scripts.path, scripts.line(i), 0, "empty script name")
    checked["hunt_scripts.csv"] = len(scripts)

    # per-script waypoint files: one file per script, no orphans either way
    wp_dir = p("hunt_waypoints")
    on_disk = {f for f in os.listdir(wp_dir) if f.endswith(".csv")}
    expected_files = {f"{i}.csv" for i in ids}
    for orphan in sorted(on_disk - expected_files):
        raise CsvError(os.path.join(wp_dir, orphan), 0, 0,
                       "orphan waypoint file — no matching row in hunt_scripts.csv")
    for missing in sorted(expected_files - on_disk):
        raise CsvError(os.path.join(wp_dir, missing), 0, 0,
                       "missing waypoint file for a script in hunt_scripts.csv")
    total_wp = 0
    for sid in sorted(ids):
        t = Table(p("hunt_waypoints", f"{sid}.csv"),
                  ["phase", "waypoint_type", "pos_x", "pos_y", "pos_z"], ["label", "extra_data"])
        seen, current = [], None
        for i in range(len(t)):
            ph = t.enum(i, "phase", set(PHASES))
            if ph != current:
                if ph in seen:
                    raise CsvError(t.path, t.line(i), 0,
                                   f"phase block '{ph}' is non-consecutive (block restarted)")
                if seen and PHASES.index(ph) < PHASES.index(seen[-1]):
                    raise CsvError(t.path, t.line(i), 0,
                                   f"phase block '{ph}' out of canonical order "
                                   f"(expected {', '.join(PHASES)})")
                seen.append(ph)
                current = ph
            t.enum(i, "waypoint_type", WAYPOINT_TYPES)
            t.num(i, "pos_x", 0, 65535)
            t.num(i, "pos_y", 0, 65535)
            t.num(i, "pos_z", 0, 15)
        total_wp += len(t)
    checked["hunt_waypoints/*.csv"] = total_wp

    targets = Table(p("hunt_targets.csv"), ["script_id", "monster_name"])
    for i in range(len(targets)):
        sid = targets.num(i, "script_id", 1, 2 ** 31)
        if sid not in ids:
            raise CsvError(targets.path, targets.line(i), 0,
                           f"script_id {sid} has no row in hunt_scripts.csv")
        if not targets.raw(i, "monster_name").strip():
            raise CsvError(targets.path, targets.line(i), 0, "empty monster_name")
    checked["hunt_targets.csv"] = len(targets)

    routes = Table(p("city_routes.csv"), ["town_id", "source_name", "enabled"])
    route_keys, towns, unparsed_routes = set(), set(), []
    for i in range(len(routes)):
        town = routes.num(i, "town_id", 0, 2 ** 31)
        routes.num(i, "enabled", 0, 1)
        name = routes.raw(i, "source_name")
        if not name.strip():
            raise CsvError(routes.path, routes.line(i), 0, "empty source_name")
        if (town, name) in route_keys:
            raise CsvError(routes.path, routes.line(i), 0,
                           f"duplicate route (town_id={town}, source_name='{name}')")
        route_keys.add((town, name))
        towns.add(town)
        if town == 0:
            if "adventurer_stone" not in name:
                raise CsvError(routes.path, routes.line(i), 0,
                               f"unknown global (town 0) route source_name '{name}'")
        else:
            # PARITY, not strictness: loadCityRoutes parses "town|src~dst:" with
            # find('|') then find('~') and `continue`s if either is missing. 20 enabled
            # rows live in production do exactly that (depot|<town>|<n>: forms), which is
            # why 1831 routes yield 1810 pairs. Erroring here would abort the load on
            # good production data, and "fix the dead rows" is a behavior change that
            # does not belong inside a migration. Report, skip, match the engine.
            after = name.split("|", 1)[1] if "|" in name else None
            if after is None or "~" not in after:
                unparsed_routes.append((routes.line(i), name))
    checked["city_routes.csv"] = len(routes)

    rwp_dir = p("city_route_waypoints")
    total_rwp = 0
    for fn in sorted(os.listdir(rwp_dir)):
        if not fn.endswith(".csv"):
            continue
        if not fn.startswith("town_"):
            raise CsvError(os.path.join(rwp_dir, fn), 0, 0, "filename must be town_<id>.csv")
        try:
            town = int(fn[5:-4])
        except ValueError:
            raise CsvError(os.path.join(rwp_dir, fn), 0, 0, "filename must be town_<id>.csv")
        if town not in towns:
            raise CsvError(os.path.join(rwp_dir, fn), 0, 0,
                           f"orphan file — no city_routes.csv row for town {town}")
        t = Table(p("city_route_waypoints", fn),
                  ["source_name", "waypoint_type", "pos_x", "pos_y", "pos_z"], ["action_label"])
        groups, current = set(), None
        for i in range(len(t)):
            name = t.raw(i, "source_name")
            if (town, name) not in route_keys:
                raise CsvError(t.path, t.line(i), 0,
                               f"waypoint group '{name}' matches no city_routes.csv row for town {town}")
            if name != current:
                if name in groups:
                    raise CsvError(t.path, t.line(i), 0,
                                   f"waypoint group '{name}' is non-consecutive (block restarted)")
                groups.add(name)
                current = name
            t.enum(i, "waypoint_type", WAYPOINT_TYPES)
            t.num(i, "pos_x", 0, 65535)
            t.num(i, "pos_y", 0, 65535)
            t.num(i, "pos_z", 0, 15)
        total_rwp += len(t)
    checked["city_route_waypoints/*.csv"] = total_rwp

    pois = Table(p("city_pois.csv"),
                 ["town_id", "name", "pos_x", "pos_y", "pos_z", "poi_type", "enabled"], ["weight"])
    seen_poi = set()
    for i in range(len(pois)):
        town = pois.num(i, "town_id", 0, 2 ** 31)
        name = pois.raw(i, "name")
        if not name.strip():
            raise CsvError(pois.path, pois.line(i), 0, "empty POI name")
        if (town, name) in seen_poi:
            raise CsvError(pois.path, pois.line(i), 0, f"duplicate POI (town_id={town}, name='{name}')")
        seen_poi.add((town, name))
        pois.num(i, "pos_x", 0, 65535)
        pois.num(i, "pos_y", 0, 65535)
        pois.num(i, "pos_z", 0, 15)
        pois.enum(i, "poi_type", POI_TYPES)
        pois.num(i, "enabled", 0, 1)
    checked["city_pois.csv"] = len(pois)

    eq = Table(p("equipment.csv"),
               ["level", "vocation", "slot_head", "slot_armor", "slot_legs", "slot_feet",
                "slot_right", "slot_left", "slot_backpack"])
    seen_eq = set()
    for i in range(len(eq)):
        lv = eq.num(i, "level", 1, 5000)
        voc = eq.num(i, "vocation", 0, 9)
        if (lv, voc) in seen_eq:
            raise CsvError(eq.path, eq.line(i), 0, f"duplicate equipment row (level {lv}, vocation {voc})")
        seen_eq.add((lv, voc))
        for c in ("slot_head", "slot_armor", "slot_legs", "slot_feet",
                  "slot_right", "slot_left", "slot_backpack"):
            eq.num_or(i, c, 0, 65535, 0)
    checked["equipment.csv"] = len(eq)

    mapping = Table(p("town_mapping.csv"), ["source_name", "canary_town_id"])
    mapped = set()
    for i in range(len(mapping)):
        name = mapping.raw(i, "source_name")
        if not name.strip():
            raise CsvError(mapping.path, mapping.line(i), 0, "empty source_name")
        key = name.lower()
        if key in mapped:
            raise CsvError(mapping.path, mapping.line(i), 0, f"duplicate mapping for '{name}'")
        mapped.add(key)
        mapping.num(i, "canary_town_id", 0, 2 ** 31)
    checked["town_mapping.csv"] = len(mapping)

    travel = Table(p("travel_positions.csv"), ["source_name", "pos_x", "pos_y", "pos_z"])
    unmapped = []
    for i in range(len(travel)):
        name = travel.raw(i, "source_name")
        if not name.strip():
            raise CsvError(travel.path, travel.line(i), 0, "empty source_name")
        travel.num(i, "pos_x", 0, 65535)
        travel.num(i, "pos_y", 0, 65535)
        travel.num(i, "pos_z", 0, 15)
        if name.lower() not in mapped:
            unmapped.append((travel.line(i), name))
    checked["travel_positions.csv"] = len(travel)

    return checked, unmapped, unparsed_routes


def main():
    announce_authored_dir("validate")
    if not os.path.isdir(AUTHORED_DIR):
        print(f"FAIL {AUTHORED_DIR}: directory missing (run from the repo root)")
        return 1
    try:
        checked, unmapped, unparsed_routes = validate()
    except CsvError as e:
        print(f"FAIL {e}")
        return 1
    width = max(len(k) for k in checked)
    for k in sorted(checked):
        print(f"  ok  {k:<{width}}  {checked[k]:>6} rows")
    if unparsed_routes:
        print(f"  WARN city_routes.csv: {len(unparsed_routes)} enabled route(s) have a "
              "source_name the engine cannot parse into src~dst and will SKIP "
              "(pre-existing; 1831 routes -> 1810 pairs). First few:")
        for line, name in unparsed_routes[:5]:
            print(f"         :{line}: '{name}'")
    for line, name in unmapped:
        # Matches the engine's WARN: the old SQL INNER JOIN dropped these silently.
        print(f"  WARN travel_positions.csv:{line}: '{name}' has no town_mapping.csv entry — "
              "the engine will skip this row (same as the old INNER JOIN)")
    print("\nPASS — authored data is valid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
