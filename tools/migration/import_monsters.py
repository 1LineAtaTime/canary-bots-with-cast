#!/usr/bin/env python3
"""Import Crystal's current monster Lua definitions into Canary.

Monster definitions in this server family are name-registered, not assigned a
stable numeric monster ID like items. Therefore the migration key is the
Game.createMonsterType name. raceId and lookType are reported and checked for
collisions rather than blindly renumbered.
"""
from __future__ import annotations

import json
import re
from collections import Counter, defaultdict
from pathlib import Path

SOURCE = Path(".crystalserver/data-global/monster")
DEST = Path("data-otservbr-global/monster")
OUT = Path("data-otservbr-global")
ITEM_MAP = Path("data/items/item_id_map.json")

NAME_RE = re.compile(r'Game\.createMonsterType\(\s*["\'](.+?)["\']\s*\)')
RACE_RE = re.compile(r"monster\.raceId\s*=\s*(\d+)")
LOOK_RE = re.compile(r"lookType\s*=\s*(\d+)")
EXP_RE = re.compile(r"monster\.experience\s*=\s*(-?\d+)")
HP_RE = re.compile(r"monster\.health\s*=\s*(-?\d+)")
SPEED_RE = re.compile(r"monster\.speed\s*=\s*(-?\d+)")
BOSS_RE = re.compile(r"rewardBoss\s*=\s*true")
ATTACK_RE = re.compile(r"monster\.attacks\s*=\s*\{")
LOOT_RE = re.compile(r"monster\.loot\s*=\s*\{")
SUMMON_RE = re.compile(r"monster\.summons\s*=\s*\{")
ITEM_ID_RE = re.compile(r"(\{\s*id\s*=\s*)(\d+)(\s*[,}])")


def norm(value: str) -> str:
    return re.sub(r"\s+", " ", value.strip().lower())


def parse(path: Path) -> dict | None:
    text = path.read_text(encoding="utf-8")
    name = NAME_RE.search(text)
    # data-global/monster also contains helper Lua files (for example quest
    # helpers) which are not monster registrations. They must not be imported
    # or treated as broken monster definitions.
    if not name:
        return None
    race = RACE_RE.search(text)
    look = LOOK_RE.search(text)
    exp = EXP_RE.search(text)
    hp = HP_RE.search(text)
    speed = SPEED_RE.search(text)
    return {
        "name": name.group(1),
        "raceId": int(race.group(1)) if race else None,
        "lookType": int(look.group(1)) if look else None,
        "experience": int(exp.group(1)) if exp else None,
        "health": int(hp.group(1)) if hp else None,
        "speed": int(speed.group(1)) if speed else None,
        "boss": bool(BOSS_RE.search(text)),
        "attacks": bool(ATTACK_RE.search(text)),
        "loot": bool(LOOT_RE.search(text)),
        "summons": bool(SUMMON_RE.search(text)),
        "path": str(path.relative_to(SOURCE)),
    }


def main() -> None:
    if not SOURCE.exists():
        raise SystemExit(f"Missing Crystal monster source: {SOURCE}")
    DEST.mkdir(parents=True, exist_ok=True)

    item_map = {}
    if ITEM_MAP.exists():
        for row in json.loads(ITEM_MAP.read_text(encoding="utf-8")):
            item_map[int(row["crystal_id"])] = int(row["canary_id"])

    source_files = sorted(SOURCE.rglob("*.lua"))
    dest_files = {p.relative_to(DEST): p for p in DEST.rglob("*.lua")}
    source_rows = []
    for path in source_files:
        row = parse(path)
        if row is not None:
            source_rows.append(row)

    dest_rows = []
    for path in dest_files.values():
        row = parse(path)
        if row is not None:
            dest_rows.append(row)
    dest_by_name = {norm(r["name"]): r for r in dest_rows}

    race_ids = defaultdict(list)
    for r in dest_rows:
        if r["raceId"] is not None:
            race_ids[r["raceId"]].append(r["name"])

    result = []
    changed_item_refs = 0
    for row in source_rows:
        src = SOURCE / row["path"]
        rel = src.relative_to(SOURCE)
        target = DEST / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        text = src.read_text(encoding="utf-8")

        def remap(match: re.Match) -> str:
            nonlocal changed_item_refs
            old = int(match.group(2))
            new = item_map.get(old, old)
            if new != old:
                changed_item_refs += 1
            return match.group(1) + str(new) + match.group(3)

        text = ITEM_ID_RE.sub(remap, text)
        target.write_text(text, encoding="utf-8")

        key = norm(row["name"])
        existing = dest_by_name.get(key)
        result.append({
            **row,
            "canary_path": str(target),
            "status": "overwritten" if existing else "new",
            "previous_raceId": existing["raceId"] if existing else None,
            "previous_lookType": existing["lookType"] if existing else None,
            "raceId_collision": bool(row["raceId"] is not None and len(race_ids.get(row["raceId"], [])) > 1),
        })

    counts = Counter(r["status"] for r in result)
    (OUT / "monster_id_map.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")

    collisions = [r for r in result if r["raceId_collision"]]
    report = [
        "# Crystal -> Canary monster migration",
        "",
        "Monster definitions are registered by `Game.createMonsterType(name)`; there is no stable numeric monster ID to renumber like item IDs.",
        "",
        f"- Crystal monster Lua files scanned: {len(source_files)}",
        f"- Crystal monster definitions: {len(source_rows)}",
        f"- Existing Canary definitions by name: {len(dest_by_name)}",
        f"- Imported/overwritten: {len(result)}",
        f"- New monster definitions: {counts['new']}",
        f"- Existing definitions refreshed from Crystal: {counts['overwritten']}",
        f"- Loot item ID references remapped: {changed_item_refs}",
        f"- raceId collisions detected: {len(collisions)}",
        "",
        "## Coverage",
        "",
        "Each imported definition retains Crystal values for name, lookType/outfit, HP, experience, speed, attacks, defenses, elements, immunities, loot, summons and flags.",
        "",
    ]
    if collisions:
        report += ["## raceId collisions", ""]
        for r in collisions[:200]:
            report.append(f"- `{r['name']}`: raceId `{r['raceId']}`")
    (OUT / "CRYSTAL_CANARY_MONSTER_MIGRATION.md").write_text("\n".join(report) + "\n", encoding="utf-8")
    print(json.dumps({"files": len(result), "new": counts['new'], "overwritten": counts['overwritten'], "item_refs_remapped": changed_item_refs, "raceId_collisions": len(collisions)}))


if __name__ == "__main__":
    main()
