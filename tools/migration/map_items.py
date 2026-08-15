#!/usr/bin/env python3
import csv
import json
import re
import urllib.request
from collections import defaultdict, Counter
from pathlib import Path
import xml.etree.ElementTree as ET

CRYSTAL_URL = "https://raw.githubusercontent.com/zimbadev/crystalserver/main/data/items/items.xml"
CANARY = Path("data/items/items.xml")
OUT = Path("data/items")
OUT.mkdir(parents=True, exist_ok=True)

def norm(s):
    return re.sub(r"\s+", " ", (s or "").strip().lower())

def parse(text):
    root = ET.fromstring(text)
    return [(int(e.get("id")), e.get("name", "")) for e in root.findall(".//item") if e.get("id")]

crystal_text = urllib.request.urlopen(CRYSTAL_URL, timeout=120).read().decode("latin1")
canary_text = CANARY.read_text(encoding="latin1")
crystal_items = parse(crystal_text)
canary_items = parse(canary_text)
can_by_id = {i: n for i, n in canary_items}
by_name = defaultdict(list)
for i, n in canary_items:
    by_name[norm(n)].append(i)

used = set(can_by_id)
mapping, reason = {}, {}
for cid, name in crystal_items:
    nm = norm(name)
    if cid in can_by_id and norm(can_by_id[cid]) == nm and cid not in mapping.values():
        mapping[cid] = cid
        reason[cid] = "same-id-same-name"
    elif cid not in used:
        mapping[cid] = cid
        reason[cid] = "free-crystal-id-preserved"
    elif len(by_name.get(nm, [])) == 1 and by_name[nm][0] not in used:
        mapping[cid] = by_name[nm][0]
        reason[cid] = "unique-name-match"
        used.add(mapping[cid])
    else:
        mapping[cid] = None
    if mapping[cid] is not None:
        used.add(mapping[cid])

next_id = max(used, default=0) + 1
for cid in mapping:
    if mapping[cid] is None:
        while next_id in used:
            next_id += 1
        mapping[cid] = next_id
        reason[cid] = "new-free-id"
        used.add(next_id)
        next_id += 1

# Match real XML item start tags. The old expression contained double-escaped
# backslashes, so it looked for a literal "\\b" instead of a word boundary.
pattern = re.compile(r'''(<item\b[^>]*?\bid\s*=\s*["'])(\d+)(["'])''', re.I)
matches = list(pattern.finditer(crystal_text))
if len(matches) != len(crystal_items):
    raise SystemExit(f"item tag mismatch: {len(matches)} != {len(crystal_items)}")

pieces, last = [], 0
for (cid, _), m in zip(crystal_items, matches):
    pieces.append(crystal_text[last:m.start(2)])
    pieces.append(str(mapping[cid]))
    last = m.end(2)
pieces.append(crystal_text[last:])
CANARY.write_text("".join(pieces), encoding="latin1")

rows = [{"crystal_id": cid, "canary_id": mapping[cid], "name": name, "reason": reason[cid]} for cid, name in crystal_items]
(OUT / "item_id_map.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
with (OUT / "item_id_map.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    w.writerow(["crystal_id", "canary_id", "name", "reason"])
    for r in rows:
        w.writerow([r["crystal_id"], r["canary_id"], r["name"], r["reason"]])

counts = Counter(reason.values())
(OUT / "CRYSTAL_CANARY_ID_MAP.md").write_text(
    "# Crystal -> Canary item ID map\n\n"
    f"Crystal items: {len(crystal_items)}\n"
    f"Original Canary items: {len(canary_items)}\n"
    f"Unique mapped IDs: {len(set(mapping.values()))}\n"
    f"Maximum mapped ID: {max(mapping.values())}\n\n"
    f"- same ID + same name: {counts['same-id-same-name']}\n"
    f"- free Crystal ID preserved: {counts['free-crystal-id-preserved']}\n"
    f"- unique name match: {counts['unique-name-match']}\n"
    f"- new IDs allocated: {counts['new-free-id']}\n",
    encoding="utf-8")
print(dict(counts))
