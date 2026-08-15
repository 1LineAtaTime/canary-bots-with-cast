#!/usr/bin/env python3
"""Rewrite Crystal item IDs to their mapped Canary IDs in data references.

This intentionally does NOT touch data/items/items.xml or item ID declarations.
Only references in known item-related contexts are changed, to avoid changing
spell IDs, monster IDs, NPC IDs, action IDs, storage values, etc.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOTS = [Path("data"), Path("data-otservbr-global")]
SKIP_NAMES = {"items.xml", "item_id_map.json", "item_id_map.csv"}
TEXT_EXTS = {".lua", ".xml", ".json", ".txt", ".cfg", ".otui", ".otml", ".md"}

# Contexts used by Canary/OTServ-style Lua and XML data for item references.
PATTERNS = [
    # Lua function calls: createItem(123), addItem(123), ItemType(123), etc.
    re.compile(r"(?P<prefix>\b(?:createItem|addItem|addContainerItem|doCreateItem|doPlayerAddItem|doPlayerAddItemEx|ItemType|getItemInfo|Game\.createItem)\s*\(\s*)(?P<id>\d+)(?P<suffix>\s*[,\)])", re.I),
    # Named Lua fields commonly used for loot/items.
    re.compile(r"(?P<prefix>\b(?:itemId|itemID|item_id|itemid|lootId|lootID|loot_id|clientId|serverId|itemType)\s*=\s*)(?P<id>\d+)(?P<suffix>\b)", re.I),
    # XML loot/item references: <item id="123"> / <item fromid="..." toid="...">.
    re.compile(r"(?P<prefix><item\b[^>]*?\bid\s*=\s*[\"'])(?P<id>\d+)(?P<suffix>[\"'])", re.I),
    # XML attributes explicitly named itemid.
    re.compile(r"(?P<prefix>\b(?:itemid|item_id|lootid)\s*=\s*[\"'])(?P<id>\d+)(?P<suffix>[\"'])", re.I),
]


def load_map() -> dict[int, int]:
    candidates = [Path("data/items/item_id_map.json"), Path("tools/migration/item_id_map.json")]
    for p in candidates:
        if p.exists():
            raw = json.loads(p.read_text(encoding="utf-8"))
            if isinstance(raw, list):
                return {int(r["crystal_id"]): int(r["canary_id"]) for r in raw}
            entries = raw.get("entries", raw)
            if isinstance(entries, dict):
                result = {}
                for k, v in entries.items():
                    target = v.get("target_id", v) if isinstance(v, dict) else v
                    result[int(k)] = int(target)
                return result
    raise SystemExit("No item ID map found. Run the Crystal -> Canary item mapper first.")


def rewrite(text: str, mapping: dict[int, int]) -> tuple[str, int, set[int]]:
    changed = 0
    seen: set[int] = set()

    def repl(m: re.Match[str]) -> str:
        nonlocal changed
        old = int(m.group("id"))
        new = mapping.get(old)
        if new is None or new == old:
            return m.group(0)
        changed += 1
        seen.add(old)
        return m.group("prefix") + str(new) + m.group("suffix")

    for pat in PATTERNS:
        text = pat.sub(repl, text)
    return text, changed, seen


def main() -> None:
    mapping = load_map()
    changed_files = []
    total = 0
    ids = set()
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
        "spell IDs, monster IDs, action IDs and storage values are intentionally not touched.\n\n"
        "## Changed files\n" + "\n".join(f"- `{p}`" for p in changed_files) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
