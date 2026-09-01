#!/usr/bin/env python3
"""Tolerance tests for the authored-CSV parser.

The whole point of the CSV move is that hand-editing must not break the import:
line endings, tabs, spaces, header order and editor artifacts have to survive a
round-trip through Excel, Notepad, git on Windows, and a careless paste.

Each ACCEPT case mangles a known-good file in a way that must parse to EXACTLY
the same fields as the clean original. Each REJECT case is a mutation that
changes MEANING and must be refused with a precise message — that is the
"flexible, but never a silent wrong load" invariant.

    python tools/bot_csv/test_tolerance.py
"""

import io
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csv_common import csv_field  # noqa: E402
from validate import WAYPOINT_TYPES, CsvError, Table, lex  # noqa: E402

CLEAN = (
    "phase,waypoint_type,pos_x,pos_y,pos_z,label,extra_data\n"
    "travel_to,node,33191,32852,7,\"Node (33191, 32852, 7)\",\n"
    "hunt_patrol,ladder,33208,32870,9,,\"tile_item:21104,21105\"\n"
    "hunt_patrol,stand,33210,32871,9,,\"fish:0,1\"\n"
)
COLS = (["phase", "waypoint_type", "pos_x", "pos_y", "pos_z"], ["label", "extra_data"])

PASS, FAIL = [], []


def parse(text, required=None, optional=None):
    fd, path = tempfile.mkstemp(suffix=".csv")
    os.close(fd)
    try:
        with io.open(path, "wb") as fh:
            fh.write(text if isinstance(text, bytes) else text.encode("utf-8"))
        t = Table(path, required or COLS[0], optional or COLS[1])
        # Run the same accessors a real loader runs: enum() and num() are where
        # case-folding, '+' handling and range checks live. Comparing their
        # OUTPUT is what proves tolerance normalizes rather than merely accepts.
        out = []
        for i in range(len(t)):
            out.append([
                t.enum(i, "phase", {"travel_to", "hunt_patrol", "travel_from"}),
                t.enum(i, "waypoint_type", WAYPOINT_TYPES),
                t.num(i, "pos_x", 0, 65535),
                t.num(i, "pos_y", 0, 65535),
                t.num(i, "pos_z", 0, 15),
                t.raw(i, "label"),
                t.raw(i, "extra_data"),
            ])
        return out
    finally:
        os.unlink(path)


BASELINE = parse(CLEAN)


def accept(name, text):
    try:
        got = parse(text)
    except CsvError as e:
        FAIL.append(f"{name}: REJECTED but should parse -> {e}")
        return
    if got != BASELINE:
        FAIL.append(f"{name}: parsed differently\n     got {got}\n     want {BASELINE}")
        return
    PASS.append(name)


def reject(name, text, expect_substr, required=None, optional=None):
    try:
        parse(text, required, optional)
    except CsvError as e:
        if expect_substr.lower() in str(e).lower():
            PASS.append(name)
        else:
            FAIL.append(f"{name}: rejected with the wrong reason -> {e}")
        return
    FAIL.append(f"{name}: ACCEPTED but must be refused")


# ---------------------------------------------------------------- ACCEPT
accept("CRLF line endings", CLEAN.replace("\n", "\r\n"))
accept("lone-CR line endings", CLEAN.replace("\n", "\r"))
accept("mixed CRLF/LF/CR in one file",
       CLEAN.split("\n")[0] + "\r\n" + CLEAN.split("\n")[1] + "\r"
       + CLEAN.split("\n")[2] + "\n" + CLEAN.split("\n")[3] + "\r\n")
accept("UTF-8 BOM", b"\xef\xbb\xbf" + CLEAN.encode("utf-8"))
accept("no final newline", CLEAN.rstrip("\n"))
accept("trailing blank lines", CLEAN + "\n\n\n")
accept("blank lines between rows", CLEAN.replace("\n", "\n\n", 2))
accept("comment lines", CLEAN.replace("\n", "\n# --- travel_to ---\n", 1))
accept("indented comment line", CLEAN.replace("\n", "\n   \t# note\n", 1))
accept("spaces around unquoted fields",
       CLEAN.replace("travel_to,node,33191", "  travel_to , node , 33191 "))
accept("tabs around unquoted fields",
       CLEAN.replace("travel_to,node,33191", "\ttravel_to\t,\tnode\t,\t33191\t"))
accept("header in a different order",
       "waypoint_type,phase,pos_y,pos_x,pos_z,extra_data,label\n"
       "node,travel_to,32852,33191,7,,\"Node (33191, 32852, 7)\"\n"
       "ladder,hunt_patrol,32870,33208,9,\"tile_item:21104,21105\",\n"
       "stand,hunt_patrol,32871,33210,9,\"fish:0,1\",\n")
accept("header case and spacing",
       CLEAN.replace("phase,waypoint_type,pos_x,pos_y,pos_z,label,extra_data",
                     " PHASE , Waypoint_Type ,POS_X,pos_Y,  pos_z,Label,EXTRA_DATA "))
accept("uppercase enum values", CLEAN.replace("travel_to,node", "TRAVEL_TO,Node"))
accept("leading + on integers", CLEAN.replace(",33191,", ",+33191,"))
accept("one trailing delimiter per row", CLEAN.replace("\n", ",\n"))
accept("whitespace after a closing quote",
       CLEAN.replace('"Node (33191, 32852, 7)",', '"Node (33191, 32852, 7)"  ,'))
accept("annotation column with _ prefix",
       "phase,waypoint_type,pos_x,pos_y,pos_z,label,extra_data,_note\n"
       "travel_to,node,33191,32852,7,\"Node (33191, 32852, 7)\",,mine\n"
       "hunt_patrol,ladder,33208,32870,9,,\"tile_item:21104,21105\",\n"
       "hunt_patrol,stand,33210,32871,9,,\"fish:0,1\",\n")

# ---------------------------------------------------------------- REJECT
reject("typo'd header column", CLEAN.replace("pos_z", "pos_zz"),
       "unrecognized header column 'pos_zz'")
reject("missing required column",
       CLEAN.replace("pos_x,", "").replace(",33191", ""), "missing required header column")
reject("duplicate header column", CLEAN.replace("label", "pos_x", 1), "duplicate header column")
# NB: the surplus field must be NON-EMPTY and on a row that does not already end in an
# empty field, because one trailing empty field is deliberately indistinguishable from a
# trailing delimiter (guide §2.2). That is the documented cost of trailing-comma tolerance.
reject("wrong field count", CLEAN.replace('9,,"fish:0,1"', '9,,"fish:0,1",surplus'),
       "expected 7 fields, got 8")
reject("non-numeric integer", CLEAN.replace(",33191,", ",33x91,"), "non-numeric value")
reject("out-of-range pos_z", CLEAN.replace(",7,\"Node", ",99,\"Node"), "out of range")
reject("unknown enum", CLEAN.replace("travel_to,node", "travel_to,teleprot"), "unknown value")
reject("unterminated quote", CLEAN.replace('"fish:0,1"', '"fish:0,1'), "unterminated quote")
reject("junk after a closing quote",
       CLEAN.replace('"Node (33191, 32852, 7)",', '"Node (33191, 32852, 7)" junk,'),
       "content after closing quote")
reject("bare quote inside an unquoted field",
       CLEAN.replace("travel_to,node", 'travel_to,no"de'), "quote inside unquoted field")
reject("semicolon-delimited file",
       CLEAN.replace(",", ";"), "separator must be a comma")
reject("empty file", "", "empty file")

# ------------------------------------------------- writer/reader round-trip
HOSTILE = [
    "Node (33152, 32751, 7)", "tile_item:21104,21105", "fish:0,1",
    "Edron Vampire Crypt  3,  4", 'has "quotes" inside', "#starts with hash",
    "  leading and trailing  ", "", "plain",
]
row = ",".join(csv_field(v) for v in HOSTILE)
hdr = ",".join(f"c{i}" for i in range(len(HOSTILE)))
try:
    fd, tp = tempfile.mkstemp(suffix=".csv")
    os.close(fd)
    with io.open(tp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(hdr + "\n" + row + "\n")
    back = list(lex(tp))[1][1]
    os.unlink(tp)
    if back == HOSTILE:
        PASS.append("writer->reader round-trip of hostile values")
    else:
        FAIL.append(f"round-trip mismatch:\n     got {back}\n     want {HOSTILE}")
except Exception as exc:  # noqa: BLE001
    FAIL.append(f"round-trip raised {exc}")

# ---------------------------------------------------------------- report
for f in FAIL:
    print(f"  FAIL {f}")
print(f"\n{len(PASS)} passed, {len(FAIL)} failed")
sys.exit(1 if FAIL else 0)
