#!/usr/bin/env python3
"""Audit Crystal 15.30 client assets against the migrated Canary data.

This deliberately does NOT rewrite appearances.dat blindly: modern 15.x
appearances are protobuf data and appearance IDs are client-side IDs. The
server item mapping must be kept separate from appearance IDs.
"""
from pathlib import Path
import hashlib, json, re
import xml.etree.ElementTree as ET

ROOT = Path('.')
ITEMS = ROOT / 'data/items/items.xml'
APPEAR = ROOT / 'data/items/appearances.dat'
OUT = ROOT / 'data/items/CRYSTAL_CANARY_ASSET_AUDIT.md'
MAP = ROOT / 'data/items/item_id_map.json'

if not APPEAR.exists():
    raise SystemExit('Missing data/items/appearances.dat')
if not ITEMS.exists():
    raise SystemExit('Missing data/items/items.xml')

raw = APPEAR.read_bytes()
sha = hashlib.sha256(raw).hexdigest()
size = len(raw)

# XML is text while appearances.dat is protobuf/binary. We validate the item
# inventory independently and report IDs which were allocated outside the
# Crystal client ID space; these require explicit appearance remapping.
root = ET.parse(ITEMS).getroot()
items = [e for e in root.findall('.//item') if e.get('id')]
ids = [int(e.get('id')) for e in items]

rows = []
if MAP.exists():
    rows = json.loads(MAP.read_text(encoding='utf-8'))
crystal_ids = {int(r['crystal_id']) for r in rows} if rows else set()
allocated = [r for r in rows if r.get('reason') == 'new-free-id']

# Heuristic only: do not claim that a protobuf byte exists for an item ID.
# A proper appearance mapping requires decoding the 15.x protobuf schema.
report = f'''# Crystal 15.30 asset audit

- `appearances.dat` size: {size:,} bytes
- `appearances.dat` SHA-256: `{sha}`
- migrated item definitions: {len(items):,}
- unique migrated item IDs: {len(set(ids)):,}
- Crystal item IDs in mapping: {len(crystal_ids):,}
- items allocated a new Canary/server ID: {len(allocated):,}

## Safety status

`appearances.dat` is present and is treated as a binary protobuf asset. It is **not** rewritten by this audit. Modern Canary uses client appearance IDs separately from server item IDs; therefore an item-ID collision cannot be resolved by blindly changing protobuf appearance IDs.

## Items requiring explicit appearance-ID verification

The following migrated items received a newly allocated server ID and therefore must be checked against their Crystal client appearance before final client packaging:

'''
for r in allocated[:200]:
    report += f"- Crystal `{r['crystal_id']}` → Canary `{r['canary_id']}` — {r.get('name','')}\n"
if len(allocated) > 200:
    report += f"\n…and {len(allocated)-200} more. Full source is `item_id_map.json`.\n"

report += '''\n## Outfit / mount / creature assets

The same `appearances.dat` contains the client appearance catalogue used by 15.x. Outfit, mount and creature appearance IDs must be decoded from the protobuf catalogue and matched to the imported server `lookType` values. No numeric ID is changed by this audit until that relationship is proven.\n'''
OUT.write_text(report, encoding='utf-8')
print(report)
