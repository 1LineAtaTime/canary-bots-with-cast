#!/usr/bin/env python3
"""Resolve duplicate Canary monster raceId values deterministically.

raceId is not an item-style global numeric ID. It is used by monster/bestiary
metadata, so we only rewrite actual raceId fields when the same value is used
by more than one monster definition. The first occurrence keeps its value;
subsequent collisions receive unused IDs above the current maximum. A report
records every change for later bestiary/client verification.
"""
from pathlib import Path
import re

MONSTER_DIR = Path("data-otservbr-global/monster")
REPORT = Path("data-otservbr-global/CRYSTAL_CANARY_RACEID_REMAP.md")
PATTERN = re.compile(r"(raceId\s*=\s*)(\d+)")

entries = []
for path in sorted(MONSTER_DIR.glob("*.lua")):
    text = path.read_text(encoding="utf-8")
    m_name = re.search(r'Game\.createMonsterType\(["\'](.+?)["\']\)', text)
    if not m_name:
        m_name = re.search(r'\.name\s*=\s*["\'](.+?)["\']', text)
    name = m_name.group(1) if m_name else path.stem
    m_race = PATTERN.search(text)
    if m_race:
        entries.append((path, name, int(m_race.group(2))))

used = {race for _, _, race in entries}
by_race = {}
for path, name, race in entries:
    by_race.setdefault(race, []).append((path, name))

collisions = {race: vals for race, vals in by_race.items() if len(vals) > 1}
next_id = max(used, default=0) + 1
changes = []

for race, vals in sorted(collisions.items()):
    # Keep the first deterministic definition; remap all later duplicates.
    for path, name in vals[1:]:
        while next_id in used:
            next_id += 1
        text = path.read_text(encoding="utf-8")
        text2, count = PATTERN.subn(lambda m: m.group(1) + str(next_id), text, count=1)
        if count != 1:
            raise SystemExit(f"failed to rewrite raceId in {path}")
        path.write_text(text2, encoding="utf-8")
        changes.append((name, path.as_posix(), race, next_id))
        used.add(next_id)
        next_id += 1

remaining = []
for path in sorted(MONSTER_DIR.glob("*.lua")):
    text = path.read_text(encoding="utf-8")
    m = PATTERN.search(text)
    if m:
        remaining.append((path, int(m.group(2))))
check = {}
for path, race in remaining:
    check.setdefault(race, []).append(path)
left = {race: paths for race, paths in check.items() if len(paths) > 1}
if left:
    raise SystemExit(f"raceId collisions remain: {len(left)}")

lines = [
    "# Crystal -> Canary raceId remap",
    "",
    f"Monster Lua files with raceId: {len(entries)}",
    f"Original colliding raceId values: {len(collisions)}",
    f"Definitions remapped: {len(changes)}",
    f"Remaining duplicate raceId values: {len(left)}",
    "",
]
if changes:
    lines += ["| Monster | File | Old raceId | New raceId |", "|---|---|---:|---:|"]
    lines += [f"| {n} | `{p}` | {old} | {new} |" for n, p, old, new in changes]
else:
    lines.append("No raceId changes were necessary.")
REPORT.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"collisions={len(collisions)} remapped={len(changes)} remaining=0")
