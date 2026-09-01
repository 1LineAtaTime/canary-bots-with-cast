"""Shared CSV writer for the authored bot-data tree.

The quoting rule here MUST match botCsvField() in
src/creatures/players/bot/bot_csv.cpp exactly, or a round-trip through the
in-game editor (Milestone 2) would rewrite files the exporter produced and
generate spurious diffs.

Quote iff the field contains , " CR or LF, has leading/trailing whitespace
(which the reader would trim off an unquoted field), or starts with '#' (which
the reader would treat as a comment line). Embedded " is doubled.
"""

import io
import os

# $BOT_AUTHORED_DIR redirects every tool in this directory at a different copy of the
# authored tree. It exists so convert_cfg.py can validate a STAGED copy — build the new
# rows in a temp dir, run validate.py against that, and only touch data/bot/authored/
# once it passes. A malformed tree cannot be walked back on a running server: /cavebot
# reload destroys the engine before parsing the CSVs, so there is no previous data to
# fall back to and no bot activates until someone fixes the file.
#
# validate.py does `from csv_common import AUTHORED_DIR`, so the value is bound once at
# first import — one process therefore sees exactly one tree, and the staged validation
# has to run as a SUBPROCESS with this variable set. See run_validator() in convert_cfg.py.
AUTHORED_DIR = os.environ.get("BOT_AUTHORED_DIR") or os.path.join("data", "bot", "authored")


def announce_authored_dir(tool_name):
    """Print the effective tree when it is NOT the default.

    A stale `export BOT_AUTHORED_DIR=...` left in a shell would otherwise silently
    redirect every one of these tools with no visible sign. Staying quiet in the normal
    case keeps the exporter's output diffable.
    """
    if os.environ.get("BOT_AUTHORED_DIR"):
        print(f"[{tool_name}] BOT_AUTHORED_DIR is set — using {AUTHORED_DIR}")


def csv_field(value):
    """Render one field with minimal RFC-4180 quoting. None -> empty."""
    if value is None:
        return ""
    s = str(value)
    quote = any(c in s for c in (",", '"', "\r", "\n"))
    if not quote and s:
        quote = s[0] in (" ", "\t", "#") or s[-1] in (" ", "\t")
    if not quote:
        return s
    return '"' + s.replace('"', '""') + '"'


def csv_row(values):
    return ",".join(csv_field(v) for v in values)


def write_csv(path, header, rows, banners=None):
    """Write a CSV atomically-ish (tmp + replace), always LF, no BOM.

    `banners` is an optional list of (row_index, text) pairs; each emits a blank
    line and a '# text' comment before that row. Both are ignored by the reader
    (guide 2.3) and exist purely so a human can navigate the file.
    """
    banner_at = {}
    for idx, text in (banners or []):
        banner_at.setdefault(idx, []).append(text)

    out = [csv_row(header)]
    for i, row in enumerate(rows):
        for text in banner_at.get(i, []):
            out.append("")
            out.append("# " + text)
        out.append(csv_row(row))
    body = "\n".join(out) + "\n"

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with io.open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(body)
    os.replace(tmp, path)
    return len(rows)
