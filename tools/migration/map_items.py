#!/usr/bin/env python3
import json, re, urllib.request
from pathlib import Path
import xml.etree.ElementTree as ET
from collections import defaultdict

CRYSTAL_URL = "https://raw.githubusercontent.com/zimbadev/crystalserver/main/data/items/items.xml"
CANARY = Path("data/items/items.xml")
OUT = Path("tools/migration")
OUT.mkdir(parents=True, exist_ok=True)

def norm(s):
    return re.sub(r"\s+", " ", (s or "").strip().lower())

def expand_items(root):
    out = {}
    for e in root.findall("item"):
        attrs = dict(e.attrib)
        if "id" in attrs:
            ids = [int(attrs["id"])]
        elif "fromid" in attrs and "toid" in attrs:
            ids = range(int(attrs["fromid"]), int(attrs["toid"]) + 1)
        else:
            continue
        for i in ids:
            out[i] = e
    return out

def clone_with_id(e, item_id):
    n = ET.Element("item")
    for k, v in e.attrib.items():
        if k not in ("id", "fromid", "toid"):
            n.set(k, v)
    n.set("id", str(item_id))
    for c in list(e):
        n.append(c)
    return n

def item_name(e):
    return norm(e.attrib.get("name"))

crystal_bytes = urllib.request.urlopen(CRYSTAL_URL, timeout=60).read()
crystal_root = ET.fromstring(crystal_bytes)
canary_root = ET.parse(CANARY).getroot()
crystal = expand_items(crystal_root)
canary = expand_items(canary_root)

used = set(canary)
name_to_ids = defaultdict(list)
for i, e in canary.items():
    n = item_name(e)
    if n:
        name_to_ids[n].append(i)

next_id = max(used | set(crystal)) + 1
mapping, reason = {}, {}
for sid in sorted(crystal):
    ce = crystal[sid]
    cname = item_name(ce)
    if sid in canary and item_name(canary[sid]) == cname:
        mapping[sid], reason[sid] = sid, "same-id-same-name"
        continue
    candidates = name_to_ids.get(cname, [])
    if len(candidates) == 1:
        mapping[sid], reason[sid] = candidates[0], "name-match"
        continue
    while next_id in used:
        next_id += 1
    mapping[sid], reason[sid] = next_id, "new-id"
    used.add(next_id)
    next_id += 1

merged = dict(canary)
for sid, tid in mapping.items():
    if tid not in merged:
        merged[tid] = clone_with_id(crystal[sid], tid)

root = ET.Element("items")
for iid in sorted(merged):
    root.append(merged[iid])
ET.indent(root, space="\t")
ET.ElementTree(root).write(OUT / "items_crystal_to_canary.xml", encoding="ISO-8859-1", xml_declaration=True)

(OUT / "item_id_map.json").write_text(json.dumps({"source_url": CRYSTAL_URL, "entries": {str(k): {"target_id": v, "reason": reason[k], "name": crystal[k].attrib.get("name", "")} for k, v in sorted(mapping.items())}}, indent=2, ensure_ascii=False), encoding="utf-8")

stats = defaultdict(int)
for r in reason.values(): stats[r] += 1
(OUT / "item_id_map_report.md").write_text(f"# Crystal → Canary item ID mapping\n\n- Crystal IDs: {len(crystal)}\n- Canary IDs: {len(canary)}\n- same ID + same name: {stats['same-id-same-name']}\n- matched by unique name: {stats['name-match']}\n- allocated new IDs: {stats['new-id']}\n- max merged ID: {max(merged)}\n\nExisting Canary IDs are never overwritten.\n", encoding="utf-8")
print(dict(stats))
