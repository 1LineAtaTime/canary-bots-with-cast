#!/usr/bin/env python3
"""Conservative Crystal items.xml -> Canary items.xml helper.

The script does not invent client appearance IDs. It preserves Canary entries,
adds Crystal-only entries, and emits conflicts for IDs that exist in both files.
Resolve conflicts explicitly before using the result in production.
"""
from __future__ import annotations

import argparse
import copy
import xml.etree.ElementTree as ET
from pathlib import Path


def parse(path: Path) -> ET.Element:
    return ET.parse(path).getroot()


def by_id(root: ET.Element):
    out = {}
    for item in root.findall(".//item"):
        iid = item.get("id")
        if iid:
            out[int(iid)] = item
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--crystal", required=True, type=Path)
    ap.add_argument("--canary", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--conflicts", required=True, type=Path)
    args = ap.parse_args()

    crystal_root = parse(args.crystal)
    canary_root = parse(args.canary)
    crystal = by_id(crystal_root)
    canary = by_id(canary_root)

    conflicts = []
    added = 0

    # Add Crystal-only IDs to the first compatible <items> container.
    target = canary_root if canary_root.tag == "items" else canary_root.find(".//items")
    if target is None:
        raise SystemExit("Could not find <items> container in Canary XML")

    for iid in sorted(crystal):
        if iid in canary:
            if ET.tostring(crystal[iid], encoding="unicode") != ET.tostring(canary[iid], encoding="unicode"):
                conflicts.append(iid)
            continue
        target.append(copy.deepcopy(crystal[iid]))
        added += 1

    ET.indent(canary_root, space="  ")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.conflicts.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(canary_root).write(args.output, encoding="utf-8", xml_declaration=True)
    args.conflicts.write_text("\n".join(map(str, conflicts)) + ("\n" if conflicts else ""), encoding="utf-8")

    print(f"Crystal IDs: {len(crystal)}")
    print(f"Canary IDs: {len(canary)}")
    print(f"Added Crystal-only IDs: {added}")
    print(f"Conflicting existing IDs: {len(conflicts)}")
    print(f"Output: {args.output}")
    print(f"Conflicts: {args.conflicts}")


if __name__ == "__main__":
    main()
