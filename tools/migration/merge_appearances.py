#!/usr/bin/env python3
"""Prepare a safe Crystal 15.30 -> Canary appearance merge.

This intentionally does not rewrite binary appearances.dat. It builds a deterministic
inventory/map from server IDs and asset metadata available in the repository, while
preserving all existing Canary IDs and allocating new IDs only when a source identity
cannot be matched safely.
"""
import hashlib, json, re
from pathlib import Path

ROOT=Path('.')
OUT=ROOT/'data/items'
OUT.mkdir(parents=True, exist_ok=True)
items=OUT/'item_id_map.json'
rows=json.loads(items.read_text(encoding='utf-8')) if items.exists() else []

# Preserve server IDs; appearance IDs are a separate namespace and must not be guessed.
item_map=[{'crystal_id':r['crystal_id'],'canary_id':r['canary_id'],'name':r.get('name',''),'status':'server-id-mapped'} for r in rows]

# Inventory appearances files without modifying them.
assets=[]
for p in ROOT.rglob('appearances*.dat'):
    if p.is_file():
        b=p.read_bytes()
        assets.append({'path':str(p),'size':len(b),'sha256':hashlib.sha256(b).hexdigest()})

# Deterministic identity maps. Existing IDs remain authoritative; new appearance IDs are
# intentionally left unresolved until the binary protobuf is decoded and compared.
(OUT/'appearance_id_map.json').write_text(json.dumps({'version':1,'policy':'preserve-canary-ids','assets':assets,'items':item_map},indent=2,ensure_ascii=False),encoding='utf-8')
for name in ['monster_appearance_map.json','outfit_id_map.json','mount_id_map.json']:
    (OUT/name).write_text(json.dumps({'version':1,'policy':'preserve-canary-ids','status':'pending-binary-identity-match','entries':[]},indent=2),encoding='utf-8')

report=['# Crystal 15.30 -> Canary appearance migration','',
        '## Safety policy','',
        '- Existing Canary appearance IDs are preserved.',
        '- No binary `appearances.dat` is overwritten by this step.',
        '- Crystal IDs are not assumed to equal appearance IDs.',
        '- Conflicting identities must receive a new free ID after binary comparison.', '',
        f'- Item mappings inspected: {len(item_map)}',
        f'- appearances.dat files found: {len(assets)}', '']
for a in assets: report.append(f"- `{a['path']}` — {a['size']} bytes — `{a['sha256']}`")
report += ['', '## Pending', '', 'Binary protobuf identity comparison is required before assigning new appearance IDs or merging outfit/mount/creature appearances.']
(OUT/'CRYSTAL_CANARY_ASSET_MIGRATION.md').write_text('\n'.join(report)+'\n',encoding='utf-8')
print(f'items={len(item_map)} assets={len(assets)}')
