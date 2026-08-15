#!/usr/bin/env python3
"""Rewrite Crystal item IDs to their mapped Canary IDs in data references.

Only known item-reference contexts are rewritten. Item declarations and
unrelated numeric IDs (spells, monsters, NPCs, actions, storages) are left
untouched. Replacements are protected with placeholders so a target ID that
also exists as a Crystal source ID cannot be remapped a second time.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOTS = [Path("data"), Path("data-otservbr-global")]
SKIP_NAMES = {"items.xml", "item_id_map.json", "item_id_map.csv"}
TEXT_EXTS = {".lua", ".xml", ".json", ".txt", ".cfg", ".otui", ".otml", ".md"}

PATTERNS = [
    # Lua/OTServ function calls whose first numeric argument is an item id.
    re.compile(r"(?P<prefix>\b(?:createItem|addItem|addContainerItem|doCreateItem|doPlayerAddItem|doPlayerAddItemEx|ItemType|getItemInfo|Game\.createItem|Game\.createItemEx|createContainer)\s*\(\s*)(?P<id>\d+)(?P<suffix>\s*[,\)])", re.I),
    # Method calls such as player:addItem(123) / container:addItem(123).
    re.compile(r"(?P<prefix>:\s*(?:addItem|addContainerItem|addItemEx)\s*\(\s*)(?P<id>\d+)(?P<suffix>\s*[,\)])", re.I),
    # Named Lua fields commonly used for loot/items.
    re.compile(r"(?P<prefix>\b(?:itemId|itemID|item_id|itemid|lootId|lootID|loot_id|itemType|itemtype)\s*=\s*)(?P<id>\d+)(?P<suffix>\b)", re.I),
    # XML loot/item references: <item id="123">.
    re.compile(r"(?P<prefix><item\b[^>]*?\bid\s*=\s*[\"'])(?P<id>\d+)(?P<suffix>[\"'])", re.I),
    # XML attributes explicitly naming an item id.
    re.compile(r"(?P<prefix>\b(?:itemid|item_id|lootid|loot_id)\s*=\s*[\"'])(?P<id>\d+)(?P<suffix>[\"'])", re.I),
]


def load_map() -> dict[int, int]:
    candidates = [Path("data/items/item_id_map.json"), Path("tools/migration/item_id_map.json")]
    for path in candidates:
        if not path.exists():
            continue
        raw = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(raw, list):
            return {int(row["crystal_id"]): int(row["canary_id"]) for row in raw}
        entries = raw.get("entries", raw) if isinstance(raw, dict) else None
        if isinstance(entries, dict):
            return {
                int(key): int(value.get("target_id", value) if isinstance(value, dict) else value)
                for key, value in entries.items()
            }
    raise SystemExit("No item ID map found. Run the Crystal -> Canary item mapper first.")


def rewrite(text: str, mapping: dict[int, int]) -> tuple[str, int, set[int]]:
    changed = 0
    seen: set[int] = set()
    placeholder_values: dict[str, str] = {}

    def repl(match: re.Match[str]) -> str:
        nonlocal changed
        old = int(match.group("id"))
        new = mapping.get(old)
        if new is None or new == old:
            return match.group(0)
        changed += 1
        seen.add(old)
        token = f"__CRYSTAL_CANARY_ITEM_{changed}_{new}__"
        placeholder_values[token] = str(new)
        return match.group("prefix") + token + match.group("suffix")

    for pattern in PATTERNS:
        text = pattern.sub(repl, text)

    for token, value in placeholder_values.items():
        text = text.replace(token, value)
    return text, changed, seen


def main() -> None:
    mapping = load_map()
    changed_files: list[str] = []
    total = 0
    ids: set[int] = set()

    for root in ROOTS:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.name in SKIP_NAMES or path.suffix.lower() not in TEXT_EXTS:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            new_text, count, seen = rewrite(text, mapping)
            if count:
                path.write_text(new_text, encoding="utf-8")
                changed_files.append(str(path))
                total += count
                ids.update(seen)

    report = Path("tools/migration/item_reference_remap_report.md")
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(
        "# Crystal -> Canary item reference remap\n\n"
        f"- files changed: {len(changed_files)}\n"
        f"- references changed: {total}\n"
        f"- source Crystal IDs referenced: {len(ids)}\n\n"
        "Only known item-reference contexts were rewritten. Item declarations, "
        "spell IDs, monster IDs, NPC IDs, action IDs and storage values are intentionally not touched.\n\n"
        "## Changed files\n" + "\n".join(f"- `{p}`" for p in changed_files) + "\n",
        encoding="utf-8",
    )
    print(f"changed_files={len(changed_files)} references={total} source_ids={len(ids)}")


if __name__ == "__main__":
    main()
