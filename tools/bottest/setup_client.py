#!/usr/bin/env python3
"""Build the harness OTClient copy from the installed OTClient Redemption.

  python tools/bottest/setup_client.py [--src "C:\\Program Files (x86)\\TibiaOTClient-Redemption"]
                                       [--dest ~/Documents/otc-harness] [--force] [--mod-only]

The install is only READ. The copy gets: the exe + top-level runtime files, data/ (only
things/<--things>, default 1500), modules/, mods/ without game_bot, then the anchored edits in
otc_overlay/edits.json, then mods/harness_ctl from otc_mod/. No binary or CipSoft asset ever
lives in tools/bottest/: the exe and data/things are copied locally by this script.

--mod-only re-installs mods/harness_ctl into an existing copy (the edit loop for ctl.lua).
A marker file .otc-harness-setup.json records source, exe sha256 and the applied edits.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SRC = r"C:\Program Files (x86)\TibiaOTClient-Redemption"
TOP_FILES = ["otclient_gl_x64.exe", "init.lua", "config.ini", "otclientrc.lua", "cacert.pem", "meta.lua", "LICENSE"]
MARKER = ".otc-harness-setup.json"


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as fh:
        for b in iter(lambda: fh.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def apply_edits(dest: Path) -> list[dict]:
    spec = json.loads((HERE / "otc_overlay" / "edits.json").read_text(encoding="utf-8"))
    done = []
    for e in spec["edits"]:
        f = dest / e["file"]
        raw = f.read_bytes().decode("utf-8")
        crlf = "\r\n" in raw
        text = raw.replace("\r\n", "\n")
        if "regex" in e:
            n = len(re.findall(e["regex"], text))
            if n != 1:
                raise SystemExit(f"overlay edit for {e['file']} matched {n} times (want 1): {e['why']}")
            text = re.sub(e["regex"], lambda m: e["replace"], text)
        else:
            n = text.count(e["find"])
            if n != 1:
                raise SystemExit(f"overlay edit for {e['file']} matched {n} times (want 1): {e['why']}")
            text = text.replace(e["find"], e["replace"])
        f.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
        done.append({"file": e["file"], "why": e["why"]})
    return done


def install_mod(dest: Path):
    tgt = dest / "mods" / "harness_ctl"
    if tgt.exists():
        shutil.rmtree(tgt)
    shutil.copytree(HERE / "otc_mod" / "harness_ctl", tgt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--dest", default=os.path.expanduser("~/Documents/otc-harness"))
    ap.add_argument("--things", default="1500", help="comma list of data/things versions to copy")
    ap.add_argument("--force", action="store_true", help="replace an existing copy")
    ap.add_argument("--mod-only", action="store_true")
    a = ap.parse_args()
    src, dest = Path(a.src).resolve(), Path(a.dest).resolve()
    if dest == src or src in dest.parents:
        raise SystemExit("refusing: dest is inside the installed client")
    if a.mod_only:
        if not (dest / "otclient_gl_x64.exe").exists():
            raise SystemExit(f"{dest} is not a harness copy")
        install_mod(dest)
        print(f"OK mod installed into {dest / 'mods' / 'harness_ctl'}")
        return
    if not (src / "otclient_gl_x64.exe").exists():
        raise SystemExit(f"no otclient_gl_x64.exe in {src}")
    if dest.exists() and any(dest.iterdir()):
        if not a.force:
            raise SystemExit(f"{dest} exists; pass --force to rebuild it")
        if not (dest / MARKER).exists() and not (dest / "otclient_gl_x64.exe").exists():
            raise SystemExit(f"{dest} does not look like a harness copy; not deleting it")
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for f in TOP_FILES:
        if (src / f).exists():
            shutil.copy2(src / f, dest / f)
    shutil.copytree(src / "modules", dest / "modules")
    things = {t.strip() for t in a.things.split(",") if t.strip()}
    shutil.copytree(src / "data", dest / "data",
                    ignore=lambda d, names: [n for n in names if Path(d).name == "things" and n not in things])
    shutil.copytree(src / "mods", dest / "mods", ignore=lambda d, names: [n for n in names
                    if Path(d) == src / "mods" and n in ("game_bot", ".gitignore")])
    for t in things:
        if not (dest / "data" / "things" / t).is_dir():
            raise SystemExit(f"data/things/{t} missing in {src}")
    edits = apply_edits(dest)
    install_mod(dest)
    (dest / "records").mkdir(exist_ok=True)
    marker = {"created": time.strftime("%Y-%m-%d %H:%M:%S"), "src": str(src),
              "exe_sha256": sha256(dest / "otclient_gl_x64.exe"), "things": sorted(things), "edits": edits}
    (dest / MARKER).write_text(json.dumps(marker, indent=2), encoding="utf-8")
    print(f"OK harness client at {dest} (exe sha256 {marker['exe_sha256'][:16]}, {len(edits)} overlay edits)")


if __name__ == "__main__":
    main()
