"""Bot Population Generator — emits a MySQL migration SQL that wipes the bot
account and re-inserts 997 fresh bot characters with deterministic but
distribution-aware level / vocation / town / name assignment.

Design:
- TOTAL_BOTS=997 is prime, picked so any plausible config value yields a
  non-trivial stride.
- Vocation + town are assigned via a SHUFFLED balanced sequence (Fisher-Yates
  with a fixed seed). Periodic assignment + integer-stride picking aliased
  catastrophically at target values where target * 4 ~ total (e.g. target=249
  collapsed to 100% sorcerers). Pre-shuffling guarantees ~uniform sampling
  for any target.
- Level distribution stays tiered by index range — wide tiers survive any
  stride, and we preserve the original 200-bot level shape scaled 5x.
- Names: character-level trigram Markov chain seeded from otservbr-npc.xml +
  the existing 200 bot first/last name pools. Generates ~1500 candidates,
  dedupes against itself and the corpus, builds 997 unique first+last combos.
- Bot equipment / imbuements / spells are runtime-applied by bot_engine.cpp and
  bot_system.lua on first activation — no per-bot DB seeding here.
- Visual identity (mount/outfit/colors): every bot owns the FULL catalog of
  mounts (235) and outfits (248 with all addons), seeded via player_storage.
  Initial looktype/addons/mount/colors are picked per-bot from a deterministic
  RNG keyed by bot id. The runtime engine reshuffles these on every server
  restart (see BOT_LIVENESS_PACK.md Phase C.1).

Run:
    python tools/bot_population_generator/generate.py \\
        --npc-xml data-otservbr-global/world/otservbr-npc.xml \\
        --outfits-xml data/XML/outfits.xml \\
        --mounts-xml data/XML/mounts.xml \\
        --out tools/bot_population_generator/bot_population_setup.sql
"""

from __future__ import annotations

import argparse
import math
import random
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# Bot population sizing — prime so stratified picker never aliases periodic
# voc/town assignment for any plausible target ≤ TOTAL_BOTS.
TOTAL_BOTS = 997
BOT_ACCOUNT_ID = 65000
BOT_ID_START = 65001

# IDs in the bot range that are already in use by non-bot characters
# (admin/test accounts). The generator skips these when allocating bot IDs so
# we never collide. Verify with:
#   SELECT id FROM players WHERE id BETWEEN 65001 AND 67000 AND account_id != 65000
RESERVED_IDS = {65201, 65202, 65203, 65204, 65205, 65206, 65207}

# 14 major towns: Sandhill, Carlin, Thais, Venore, Ab'Dendriel, Kazordoon,
# Ankrahmun, Liberty Bay, Port Hope, Yalahar, Farmine, Edron, Darashia, Svargrond
TOWN_IDS = [5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 20, 22]

# Levels assigned by index range (kept as widespread tiers so a stratified
# stride from any target still samples each tier proportionally).
#
# 2026-06-19 reshape: skew the population high (user request — "not enough
# bots level 500+"). Was ~60% at level 8-100 / ~7% at 500+. Now ~5% low and
# ~55% (547 bots) at level 500+, peaking in the 500-900 band. Counts per band
# were chosen by the user; index ranges are the contiguous cumulative spans.
# NOTE: the forge-tier bands in bot_engine.cpp::applyBotForgeTiers MUST stay in
# sync with these level bands (see the tier column below) — they are two
# independent definitions of the same level→tier mapping.
#   idx        count  level       forge tier
LEVEL_TIERS = [
    (0,   49,  8,    100 ),  # 50 bots   level 8-100    tier 0
    (50,  199, 100,  250 ),  # 150 bots  level 100-250  tier 1
    (200, 449, 250,  500 ),  # 250 bots  level 250-500  tier 2-3
    (450, 649, 500,  700 ),  # 200 bots  level 500-700  tier 3-4
    (650, 849, 700,  900 ),  # 200 bots  level 700-900  tier 4-5
    (850, 939, 900,  1100),  # 90 bots   level 900-1100 tier 5-6
    (940, 979, 1100, 1200),  # 40 bots   level 1100-1200 tier 5-6
    (980, 996, 1200, 1200),  # 17 bots   level 1200     tier 5-6
]


# ---------------------------------------------------------------------------
# Corpus extraction
# ---------------------------------------------------------------------------

# Existing 200-bot first/last names — match the prior SQL stored procedure so
# the Markov corpus carries the same "fantasy" register.
LEGACY_FIRST_NAMES = [
    "Aldric", "Lyra", "Theron", "Isolde", "Magnus",
    "Freya", "Ragnar", "Astrid", "Cedric", "Elara",
    "Finn", "Gwen", "Henrik", "Isla", "Jorik",
    "Keira", "Leander", "Mira", "Nikolai", "Ophelia",
]
LEGACY_LAST_NAMES = [
    "Steelarm", "Mistwalker", "Shadowbane", "Ironforge", "Stormwind",
    "Dawnbringer", "Nightwhisper", "Flamecrest", "Frostborn", "Stoneguard",
]

# Names matching these patterns are descriptive NPCs ("A Bearded Woman",
# "Captain Fearless"), not proper personal names — exclude from the corpus.
_DESCRIPTIVE_PREFIX = re.compile(r"^(A|The|An|Sir|Lady|Lord|Captain|Master|Mister|Mrs|Miss|Doctor|Father|Mother|Brother|Sister|Chief|King|Queen|Prince|Princess|Baron|Count|Duke|Madam|Mr|St|Saint|Old|Young|Dead|Crazy|Mad|Drunken|Insane|Stout)\b", re.I)
_HAS_NUMBER = re.compile(r"\d")
_HAS_PAREN = re.compile(r"[()]")
_VALID_NAME = re.compile(r"^[A-Z][a-z]+(?:[A-Z][a-z]+)?$")  # CamelCase or single capitalized word


def load_npc_names(npc_xml_path: Path) -> list[str]:
    """Extract proper-name candidates from otservbr-npc.xml."""
    tree = ET.parse(npc_xml_path)
    raw = set()
    for npc in tree.iter("npc"):
        n = npc.get("name")
        if n:
            raw.add(n.strip())

    keep = []
    for n in raw:
        if _DESCRIPTIVE_PREFIX.match(n):
            continue
        if _HAS_NUMBER.search(n) or _HAS_PAREN.search(n):
            continue
        # Split multi-word NPC names into individual tokens — each capitalized
        # alpha word becomes a corpus entry if it looks like a proper noun.
        for token in n.split():
            if _VALID_NAME.match(token) and 3 <= len(token) <= 12:
                keep.append(token)
    return sorted(set(keep))


# ---------------------------------------------------------------------------
# Trigram Markov name generator
# ---------------------------------------------------------------------------

_START = "^"
_END = "$"


def build_trigram_table(words: list[str]) -> dict[str, list[str]]:
    """Build a char-trigram next-char distribution from the corpus."""
    table: dict[str, list[str]] = {}
    for word in words:
        padded = _START + _START + word.lower() + _END
        for i in range(len(padded) - 2):
            key = padded[i:i + 2]
            nxt = padded[i + 2]
            table.setdefault(key, []).append(nxt)
    return table


def generate_name(table: dict[str, list[str]], rng: random.Random,
                  min_len: int = 4, max_len: int = 10) -> str | None:
    """Roll one name from the trigram table. Returns None on rejection."""
    out = []
    key = _START + _START
    for _ in range(max_len + 2):
        choices = table.get(key)
        if not choices:
            return None
        nxt = rng.choice(choices)
        if nxt == _END:
            break
        out.append(nxt)
        key = key[1] + nxt
    if len(out) < min_len:
        return None
    return out[0].upper() + "".join(out[1:])


def generate_unique_names(table: dict[str, list[str]], rng: random.Random,
                          n: int, corpus: set[str], min_len=4, max_len=10,
                          max_attempts: int = 50_000) -> list[str]:
    """Generate n distinct names not present in the source corpus."""
    seen: set[str] = set()
    out: list[str] = []
    attempts = 0
    while len(out) < n and attempts < max_attempts:
        attempts += 1
        name = generate_name(table, rng, min_len, max_len)
        if name is None:
            continue
        low = name.lower()
        if low in seen:
            continue
        # Reject generated names that exactly match a corpus name — keeps
        # the bot roster visually distinct from NPCs.
        if low in corpus:
            continue
        seen.add(low)
        out.append(name)
    if len(out) < n:
        raise RuntimeError(f"Markov generator exhausted: got {len(out)} of {n} requested")
    return out


# ---------------------------------------------------------------------------
# Bot row computation (levels / vocation / town / look / skills)
# ---------------------------------------------------------------------------

def level_for_index(i: int) -> int:
    for lo, hi, lvl_lo, lvl_hi in LEVEL_TIERS:
        if lo <= i <= hi:
            if lvl_lo == lvl_hi:
                return lvl_lo
            span = hi - lo
            return lvl_lo + math.floor((i - lo) * (lvl_hi - lvl_lo) / max(1, span))
    raise ValueError(f"No tier covers index {i}; check LEVEL_TIERS")


def build_voc_assignment(total: int, rng: random.Random) -> list[int]:
    """Pre-shuffled vocation list — exactly ceil(total/4) of each, then
    Fisher-Yates shuffled. Stratified picking at any stride samples roughly
    uniformly because the underlying sequence has no period."""
    per_voc = (total + 3) // 4  # ceil — at total=997 -> 250/249/249/249
    seq = []
    for voc in (1, 2, 3, 4):
        seq.extend([voc] * per_voc)
    seq = seq[:total]
    rng.shuffle(seq)
    return seq


def build_town_assignment(total: int, rng: random.Random) -> list[int]:
    """Pre-shuffled town list — ceil(total/14) of each, Fisher-Yates shuffled."""
    per_town = (total + len(TOWN_IDS) - 1) // len(TOWN_IDS)
    seq = []
    for town in TOWN_IDS:
        seq.extend([town] * per_town)
    seq = seq[:total]
    rng.shuffle(seq)
    return seq


def vocation_id(base_voc: int, level: int) -> int:
    """Apply promotion at level >= 20."""
    return base_voc + 4 if level >= 20 else base_voc


def health_mana_cap(base_voc: int, level: int) -> tuple[int, int, int]:
    if base_voc == 4:  # Knight
        return 185 + (level - 1) * 15, 90 + (level - 1) * 5, 470 + (level - 1) * 25
    if base_voc == 3:  # Paladin
        return 185 + (level - 1) * 10, 90 + (level - 1) * 15, 470 + (level - 1) * 20
    return 185 + (level - 1) * 5, 90 + (level - 1) * 30, 470 + (level - 1) * 10


def experience_for_level(level: int) -> int:
    return math.floor(50.0 * (level ** 3 - 6 * level ** 2 + 17 * level - 12) / 3.0)


def skills_for_vocation(base_voc: int, level: int, i: int) -> dict:
    # Flat 100 for every skill, every bot, every level. Matches the runtime
    # override in data/scripts/lib/bot_system.lua activateBot, so the DB seed
    # and the in-memory state stay consistent regardless of whether a bot is
    # in the currently-active rotation.
    return {"fist": 100, "sword": 100, "axe": 100, "club": 100, "dist": 100,
            "shield": 100, "fishing": 100, "maglevel": 100}


def look_colors_random(rng: random.Random) -> tuple[int, int, int, int]:
    """Per-bot random colors from the Tibia palette [0, 132]. Replaces the
    prior deterministic `(i*7+13)%133` formula which aliased every 8 bots to
    identical color tuples (every 16 indices repeat per sex)."""
    return (rng.randint(0, 132), rng.randint(0, 132),
            rng.randint(0, 132), rng.randint(0, 132))


def look_mount_colors_random(rng: random.Random) -> tuple[int, int, int, int]:
    """Same palette as outfit colors; only meaningful when the bot is mounted."""
    return (rng.randint(0, 132), rng.randint(0, 132),
            rng.randint(0, 132), rng.randint(0, 132))


def pick_looktype(outfit_catalog: dict[int, list[tuple[int, str]]],
                  sex: int, rng: random.Random) -> int:
    """Random looktype from the full catalog for the given sex (0=female, 1=male).
    Every bot owns every outfit via player_storage (see emit_storage_sql), so
    `canWear` accepts any choice here."""
    candidates = outfit_catalog.get(sex, [])
    if not candidates:
        return 138 if sex == 0 else 130  # fallback to starter outfit
    return rng.choice(candidates)[0]


def pick_addons(rng: random.Random) -> int:
    """Random addons in {0, 1, 2, 3}. Bot owns addons=3 for every outfit, so
    any value is valid."""
    return rng.randint(0, 3)


def pick_mount(mount_ids: list[int], rng: random.Random) -> int:
    if not mount_ids:
        return 0
    return rng.choice(mount_ids)


# ---------------------------------------------------------------------------
# Outfit + mount catalog loaders
# ---------------------------------------------------------------------------

def load_outfit_catalog(xml_path: Path) -> dict[int, list[tuple[int, str]]]:
    """Returns {sex_int: [(looktype, name), ...]} for every enabled outfit.
    sex 0 = female, 1 = male, matching the player.sex column convention."""
    tree = ET.parse(xml_path)
    out: dict[int, list[tuple[int, str]]] = {0: [], 1: []}
    for o in tree.iter("outfit"):
        if o.get("enabled", "yes") != "yes":
            continue
        sex = int(o.get("type"))
        lt = int(o.get("looktype"))
        name = o.get("name", "")
        out[sex].append((lt, name))
    return out


def load_mount_ids(xml_path: Path) -> list[int]:
    """Returns sorted list of all mount IDs from data/XML/mounts.xml."""
    tree = ET.parse(xml_path)
    ids = []
    for m in tree.iter("mount"):
        ids.append(int(m.get("id")))
    return sorted(ids)


# ---------------------------------------------------------------------------
# player_storage range encoding for mounts + outfits
# ---------------------------------------------------------------------------
# Mirrors src/utils/const.hpp:
#   PSTRG_OUTFITS_RANGE_START = 10001000, RANGE_SIZE = 500
#   PSTRG_MOUNTS_RANGE_START  = 10002001, RANGE_SIZE = 10
#   PSTRG_MOUNTS_CURRENTMOUNT = 10002011
# Mount ownership: 10 keys × 31 bits = 310 mount slots. Mount id N → bit
# (N-1) % 31 of key 10002001 + (N-1)/31. Verified against player.cpp:7601-7647.
# Outfit ownership: each owned outfit stored as one row with packed value
# (lookType << 16) | addons. Granting addons=3 grants both addon1 and addon2.

PSTRG_OUTFITS_RANGE_START = 10001000
PSTRG_OUTFITS_RANGE_SIZE = 500
PSTRG_MOUNTS_RANGE_START = 10002001
PSTRG_MOUNTS_RANGE_SIZE = 10
PSTRG_MOUNTS_CURRENTMOUNT = 10002011


def mount_ownership_bitmask_rows(mount_ids: list[int]) -> list[tuple[int, int]]:
    """Encode the full mount catalog into the bitmask key range.
    Returns list of (storage_key, int32_bitmask_value)."""
    bitmasks = [0] * PSTRG_MOUNTS_RANGE_SIZE
    for mid in mount_ids:
        if mid < 1:
            continue
        slot = (mid - 1) // 31
        bit = (mid - 1) % 31
        if slot < PSTRG_MOUNTS_RANGE_SIZE:
            bitmasks[slot] |= (1 << bit)
    return [(PSTRG_MOUNTS_RANGE_START + i, m) for i, m in enumerate(bitmasks) if m != 0]


def outfit_ownership_rows(outfit_catalog: dict[int, list[tuple[int, str]]],
                          sex: int) -> list[tuple[int, int]]:
    """One row per outfit (for the given sex) with addons=3 (both addons granted).
    Returns list of (slot_key, packed_value) where packed = (lookType<<16)|addons."""
    rows = []
    for slot, (lt, _name) in enumerate(outfit_catalog.get(sex, [])):
        if slot >= PSTRG_OUTFITS_RANGE_SIZE:
            break
        packed = (lt << 16) | 3
        rows.append((PSTRG_OUTFITS_RANGE_START + 1 + slot, packed))
    return rows


# ---------------------------------------------------------------------------
# SQL emission
# ---------------------------------------------------------------------------

INSERT_COLUMNS = (
    "`id`, `name`, `group_id`, `account_id`, `level`, `vocation`,"
    " `health`, `healthmax`, `experience`, `lookbody`, `lookfeet`, `lookhead`, `looklegs`,"
    " `looktype`, `lookaddons`,"
    " `lookmountbody`, `lookmountfeet`, `lookmounthead`, `lookmountlegs`,"
    " `maglevel`, `mana`, `manamax`, `manaspent`,"
    " `soul`, `town_id`, `posx`, `posy`, `posz`, `conditions`, `cap`, `sex`,"
    " `pronoun`, `lastlogin`, `lastip`, `save`, `skull`, `skulltime`,"
    " `lastlogout`, `blessings`, `blessings1`, `blessings2`, `blessings3`,"
    " `blessings4`, `blessings5`, `blessings6`, `blessings7`, `blessings8`,"
    " `onlinetime`, `deletion`, `balance`, `offlinetraining_time`, `offlinetraining_skill`,"
    " `stamina`,"
    " `skill_fist`, `skill_fist_tries`,"
    " `skill_club`, `skill_club_tries`,"
    " `skill_sword`, `skill_sword_tries`,"
    " `skill_axe`, `skill_axe_tries`,"
    " `skill_dist`, `skill_dist_tries`,"
    " `skill_shielding`, `skill_shielding_tries`,"
    " `skill_fishing`, `skill_fishing_tries`"
)


def emit_sql(bots: list[dict], mount_ids: list[int],
             outfit_catalog: dict[int, list[tuple[int, str]]],
             out_path: Path) -> None:
    lines = [
        "-- ============================================================================",
        "-- Bot Population Setup — 997 simulated players for Canary 13.x",
        "-- Generated by tools/bot_population_generator/generate.py",
        "--",
        "-- Idempotent: filters by account_id (not by ID range) so re-runs with a",
        "-- different TOTAL_BOTS leave no orphans.",
        "--",
        "-- Run: mysql -h $DB_HOST -u $DB_USER -p$DB_PASS canary < bot_population_setup.sql",
        "-- ============================================================================",
        "",
        "-- Bot account (id=65000, premium forever). Idempotent.",
        f"INSERT INTO `accounts` (`id`, `name`, `password`, `email`, `premdays`, `premdays_purchased`,",
        "        `lastday`, `type`, `coins`, `coins_transferable`, `creation`)",
        f"VALUES ({BOT_ACCOUNT_ID}, 'botaccount', SHA1('botaccount'), 'bot@localhost',",
        "        99999, 0, UNIX_TIMESTAMP(), 1, 0, 0, UNIX_TIMESTAMP())",
        "ON DUPLICATE KEY UPDATE `premdays` = 99999, `password` = SHA1('botaccount');",
        "",
        "-- Wipe all existing bot characters by account (cascades to player_items,",
        "-- player_storage, etc. via FK ON DELETE CASCADE).",
        f"DELETE FROM `players` WHERE `account_id` = {BOT_ACCOUNT_ID};",
        "",
        "-- Stale bot AI-state and command-queue rows reference names/guids that may",
        "-- not exist after repopulation. Truncate so the new bots start clean.",
        "TRUNCATE `bot_state_persistence`;",
        "TRUNCATE `bot_commands`;",
        "",
        "-- Bot character rows.",
        f"INSERT INTO `players` ({INSERT_COLUMNS}) VALUES",
    ]
    value_rows = []
    for b in bots:
        value_rows.append(
            "(" + ", ".join(str(x) if not isinstance(x, str) else f"'{x}'" for x in (
                b["id"], b["name"], 1, BOT_ACCOUNT_ID, b["level"], b["vocation"],
                b["hp"], b["hp"], b["exp"], b["lookbody"], b["lookfeet"], b["lookhead"], b["looklegs"],
                b["looktype"], b["addons"],
                b["lookmountbody"], b["lookmountfeet"], b["lookmounthead"], b["lookmountlegs"],
                b["maglevel"], b["mana"], b["mana"], 0,
                100, b["town_id"], 0, 0, 0, "", b["cap"], b["sex"],
                0, 0, 0, 1, 0, 0,
                0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
                0, 0, 100000, 43200, -1,
                2520,
                b["fist"], 0,
                b["club"], 0,
                b["sword"], 0,
                b["axe"], 0,
                b["dist"], 0,
                b["shield"], 0,
                b["fishing"], 0,
            )) + ")"
        )
    lines.append(",\n".join(value_rows) + ";")

    # ---- player_storage: visual identity ownership grants ----
    # Every bot owns the full mount catalog + every outfit (with addons=3) for
    # their sex. Initial selected mount stored under PSTRG_MOUNTS_CURRENTMOUNT.
    # Runtime engine (Phase C.1) shuffles the displayed looktype/addons/mount/
    # colors at every server restart by UPDATEing the players row.
    mount_rows = mount_ownership_bitmask_rows(mount_ids)  # 8 non-zero rows for 235 mounts
    outfits_by_sex = {0: outfit_ownership_rows(outfit_catalog, 0),
                      1: outfit_ownership_rows(outfit_catalog, 1)}
    lines.extend([
        "",
        f"-- player_storage: grant every bot the full mount catalog ({len(mount_ids)} mounts)",
        f"-- + every outfit for their sex (female={len(outfits_by_sex[0])}, male={len(outfits_by_sex[1])})",
        f"-- + their initially-selected mount. Total rows: {len(bots)} * (~{len(mount_rows)+1} mount + ~{len(outfits_by_sex[0])} outfit).",
        "INSERT INTO `player_storage` (`player_id`, `key`, `value`) VALUES",
    ])
    storage_rows = []
    for b in bots:
        pid = b["id"]
        # Mount ownership bitmask rows (same for every bot)
        for key, val in mount_rows:
            storage_rows.append(f"({pid}, {key}, {val})")
        # Currently-selected mount (per-bot random)
        storage_rows.append(f"({pid}, {PSTRG_MOUNTS_CURRENTMOUNT}, {b['chosen_mount']})")
        # Outfit ownership rows for this bot's sex
        for key, val in outfits_by_sex[b["sex"]]:
            storage_rows.append(f"({pid}, {key}, {val})")

    # Emit in chunks to keep individual statement size reasonable.
    CHUNK = 2000
    for i in range(0, len(storage_rows), CHUNK):
        chunk = storage_rows[i:i + CHUNK]
        prefix = "INSERT INTO `player_storage` (`player_id`, `key`, `value`) VALUES\n" if i > 0 else ""
        terminator = ";\n" if i + CHUNK < len(storage_rows) else ";"
        lines.append(prefix + ",\n".join(chunk) + terminator)
    print(f"    Storage rows emitted: {len(storage_rows):,}")

    lines.extend([
        "",
        "-- Verification.",
        f"SELECT COUNT(*) AS bot_count FROM `players` WHERE `account_id` = {BOT_ACCOUNT_ID};",
        "SELECT `vocation`, COUNT(*) FROM `players` WHERE `account_id` = "
        f"{BOT_ACCOUNT_ID} GROUP BY `vocation` ORDER BY `vocation`;",
        "SELECT `town_id`, COUNT(*) FROM `players` WHERE `account_id` = "
        f"{BOT_ACCOUNT_ID} GROUP BY `town_id` ORDER BY `town_id`;",
        f"SELECT COUNT(DISTINCT `looktype`) AS unique_looktypes FROM `players` WHERE `account_id` = {BOT_ACCOUNT_ID};",
        f"SELECT COUNT(*) AS storage_rows FROM `player_storage` WHERE `player_id` BETWEEN {BOT_ID_START} AND {BOT_ID_START + TOTAL_BOTS + 10};",
    ])
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------
# Level-only in-place UPDATE migration
# ---------------------------------------------------------------------------

def read_id_vocation_csv(csv_path: Path) -> list[tuple[int, int]]:
    """Read a `id,vocation` CSV (one row per bot, header optional), ordered as
    given (caller must produce it `ORDER BY id ASC`). Returns [(id, voc), ...]."""
    rows: list[tuple[int, int]] = []
    for raw in csv_path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue
        parts = [p.strip() for p in line.replace("\t", ",").split(",")]
        if len(parts) < 2:
            continue
        if not parts[0].lstrip("-").isdigit():  # skip header row
            continue
        rows.append((int(parts[0]), int(parts[1])))
    return rows


def emit_level_update_sql(id_voc_rows: list[tuple[int, int]], out_path: Path) -> None:
    """Emit an idempotent, transactional in-place UPDATE migration that rewrites
    ONLY level-derived columns for the existing bot rows — leaving names, ids,
    looks, town, vocation BASE, and player_storage untouched.

    Index `i` = rank of the id in ascending order (matches the generator's
    index→level tiering). base_voc is recovered seed-independently from the live
    vocation column as ((voc-1)%4)+1, then re-promoted for the NEW level.

    Idempotent: keyed by id, recomputed from a pure function of (index, base_voc);
    re-running yields the same result. Wrapped in a single transaction so the
    rewrite is atomic.
    """
    total = len(id_voc_rows)
    lines = [
        "-- ============================================================================",
        "-- Bot Level Redistribution — in-place UPDATE migration",
        "-- Generated by tools/bot_population_generator/generate.py --emit-update",
        "--",
        f"-- Rewrites level-derived columns for {total} bot rows (account_id="
        f"{BOT_ACCOUNT_ID}) to the LEVEL_TIERS distribution. Touches ONLY:",
        "--   level, vocation, health, healthmax, mana, manamax, experience, cap",
        "-- Does NOT touch names/ids/looks/town/player_storage (no FK cascade).",
        "--",
        "-- SAFETY: mysqldump the bot rows before applying — this is in-place with no",
        "-- automatic rollback once committed:",
        f"--   mysqldump ... canary players --where=\"account_id={BOT_ACCOUNT_ID}\" > bots_backup.sql",
        "-- ============================================================================",
        "",
        "START TRANSACTION;",
        "",
    ]

    for i, (pid, cur_voc) in enumerate(id_voc_rows):
        level = level_for_index(i)
        base_voc = ((cur_voc - 1) % 4) + 1  # recover base from promoted/base voc
        voc = vocation_id(base_voc, level)
        hp, mana, cap = health_mana_cap(base_voc, level)
        exp = experience_for_level(level)
        lines.append(
            f"UPDATE `players` SET `level`={level}, `vocation`={voc}, "
            f"`health`={hp}, `healthmax`={hp}, `mana`={mana}, `manamax`={mana}, "
            f"`experience`={exp}, `cap`={cap} "
            f"WHERE `id`={pid} AND `account_id`={BOT_ACCOUNT_ID};"
        )

    lines.extend([
        "",
        "COMMIT;",
        "",
        "-- Verification (run after commit):",
        "SELECT FLOOR(`level`/100)*100 AS lvl_band, COUNT(*) FROM `players` "
        f"WHERE `account_id`={BOT_ACCOUNT_ID} GROUP BY lvl_band ORDER BY lvl_band;",
        "SELECT SUM(`level`>=500) AS lvl_500_plus, COUNT(*) AS total FROM `players` "
        f"WHERE `account_id`={BOT_ACCOUNT_ID};",
        "SELECT `vocation`, COUNT(*) FROM `players` "
        f"WHERE `account_id`={BOT_ACCOUNT_ID} GROUP BY `vocation` ORDER BY `vocation`;",
    ])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    n500 = sum(1 for i in range(total) if level_for_index(i) >= 500)
    print(f"[*] Emitted level UPDATE migration for {total} bots -> {out_path}")
    print(f"    Level 500+: {n500} ({100*n500/max(1,total):.1f}%)")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--npc-xml", type=Path,
                   help="Path to otservbr-npc.xml for the name corpus (full regen only)")
    p.add_argument("--outfits-xml", type=Path,
                   default=Path("data/XML/outfits.xml"),
                   help="Path to data/XML/outfits.xml for visual catalog grants")
    p.add_argument("--mounts-xml", type=Path,
                   default=Path("data/XML/mounts.xml"),
                   help="Path to data/XML/mounts.xml for visual catalog grants")
    p.add_argument("--out", type=Path, required=True,
                   help="Where to write the migration SQL")
    p.add_argument("--seed", type=int, default=20260523,
                   help="RNG seed for deterministic name + look generation")
    p.add_argument("--emit-update", type=Path, metavar="ID_VOC_CSV",
                   help="Level-only in-place UPDATE mode: read an `id,vocation` CSV "
                        "(ORDER BY id ASC, one row per existing bot) and emit a "
                        "migration that rewrites only level-derived columns. Skips "
                        "the full INSERT regen entirely.")
    args = p.parse_args()

    # Level-only UPDATE migration: recompute level/derived columns for existing
    # bots from LEVEL_TIERS, keyed by id, base_voc from the live vocation column.
    if args.emit_update:
        id_voc_rows = read_id_vocation_csv(args.emit_update)
        if not id_voc_rows:
            print(f"[!] No id,vocation rows read from {args.emit_update}", file=sys.stderr)
            return 1
        if len(id_voc_rows) != TOTAL_BOTS:
            print(f"[!] WARNING: CSV has {len(id_voc_rows)} rows but TOTAL_BOTS="
                  f"{TOTAL_BOTS}; index→level tiering assumes the full sorted pool.",
                  file=sys.stderr)
        emit_level_update_sql(id_voc_rows, args.out)
        return 0

    if not args.npc_xml:
        print("[!] --npc-xml is required for full regeneration", file=sys.stderr)
        return 1

    # Three independent RNG streams. Voc + town seeds were empirically chosen
    # (search over 50 + 200 candidates) to minimize max-min variance at the
    # default target values (200 and 300). Don't change without re-checking
    # distribution across the full target range.
    rng = random.Random(args.seed)
    voc_rng = random.Random(22)
    town_rng = random.Random(56)

    print(f"[*] Loading NPC name corpus from {args.npc_xml}")
    npc_names = load_npc_names(args.npc_xml)
    print(f"    {len(npc_names)} proper-name candidates after filtering")

    print(f"[*] Loading outfit catalog from {args.outfits_xml}")
    outfit_catalog = load_outfit_catalog(args.outfits_xml)
    print(f"    {len(outfit_catalog[0])} female outfits, {len(outfit_catalog[1])} male outfits")

    print(f"[*] Loading mount catalog from {args.mounts_xml}")
    mount_ids = load_mount_ids(args.mounts_xml)
    print(f"    {len(mount_ids)} mounts (IDs {mount_ids[0]}..{mount_ids[-1]})")

    corpus = npc_names + LEGACY_FIRST_NAMES + LEGACY_LAST_NAMES
    corpus_lower = {n.lower() for n in corpus}
    print(f"    {len(corpus)} total corpus entries ({len(corpus_lower)} unique)")

    print("[*] Training trigram Markov model")
    table = build_trigram_table(corpus)

    # Need 997 unique first+last combos. With 50 firsts × 25 lasts = 1250 combos.
    # Use the legacy first names as a "real-Tibian-feel" anchor for the first 20.
    print("[*] Generating Markov names (50 firsts + 25 lasts)")
    firsts = list(LEGACY_FIRST_NAMES)
    firsts.extend(generate_unique_names(
        table, rng, n=30, corpus=corpus_lower | {f.lower() for f in firsts},
        min_len=4, max_len=8,
    ))
    lasts = list(LEGACY_LAST_NAMES)
    lasts.extend(generate_unique_names(
        table, rng, n=15, corpus=corpus_lower | {l.lower() for l in lasts},
        min_len=5, max_len=10,
    ))
    print(f"    {len(firsts)} firsts, {len(lasts)} lasts -> {len(firsts) * len(lasts)} combos available")

    # Pre-shuffle vocation + town assignments so stratified picking samples
    # uniformly at ANY target value (periodic assignment aliased badly).
    voc_assignment = build_voc_assignment(TOTAL_BOTS, voc_rng)
    town_assignment = build_town_assignment(TOTAL_BOTS, town_rng)

    # Allocate TOTAL_BOTS contiguous IDs, skipping any reserved IDs in the range.
    bot_ids = []
    candidate = BOT_ID_START
    while len(bot_ids) < TOTAL_BOTS:
        if candidate not in RESERVED_IDS:
            bot_ids.append(candidate)
        candidate += 1
    print(f"    Bot ID range: {bot_ids[0]}..{bot_ids[-1]} (skipped {len(RESERVED_IDS)} reserved IDs)")

    # Assign first+last by index. Cycle firsts through lasts; each combo unique.
    bots = []
    used_names: set[str] = set()
    for i in range(TOTAL_BOTS):
        first = firsts[i % len(firsts)]
        last = lasts[(i // len(firsts)) % len(lasts)]
        name = f"{first} {last}"
        if name in used_names:
            # Pathological case — cycle further. Shouldn't trigger at 50×25=1250 >> 997.
            for shift in range(1, len(lasts)):
                alt = f"{first} {lasts[(i // len(firsts) + shift) % len(lasts)]}"
                if alt not in used_names:
                    name = alt
                    break
        used_names.add(name)

        level = level_for_index(i)
        base_voc = voc_assignment[i]
        voc = vocation_id(base_voc, level)
        hp, mana, cap = health_mana_cap(base_voc, level)
        skills = skills_for_vocation(base_voc, level, i)
        sex = i % 2

        # Per-bot RNG seeded from bot id + master seed. Independent stream per bot
        # so colors / outfit / mount are uncorrelated across bots but reproducible
        # across regenerator runs.
        bot_rng = random.Random(bot_ids[i] ^ args.seed)
        lb, lf, lh, ll = look_colors_random(bot_rng)
        mlb, mlf, mlh, mll = look_mount_colors_random(bot_rng)
        chosen_looktype = pick_looktype(outfit_catalog, sex, bot_rng)
        chosen_addons = pick_addons(bot_rng)
        chosen_mount = pick_mount(mount_ids, bot_rng)

        bots.append({
            "id": bot_ids[i],
            "name": name,
            "level": level,
            "vocation": voc,
            "hp": hp, "mana": mana, "cap": cap,
            "exp": experience_for_level(level),
            "lookbody": lb, "lookfeet": lf, "lookhead": lh, "looklegs": ll,
            "lookmountbody": mlb, "lookmountfeet": mlf, "lookmounthead": mlh, "lookmountlegs": mll,
            "looktype": chosen_looktype,
            "addons": chosen_addons,
            "chosen_mount": chosen_mount,
            "maglevel": skills["maglevel"],
            "town_id": town_assignment[i],
            "sex": sex,
            "fist": skills["fist"], "sword": skills["sword"], "axe": skills["axe"],
            "club": skills["club"], "dist": skills["dist"], "shield": skills["shield"],
            "fishing": skills["fishing"],
        })

    # Sanity check distribution
    voc_counts = {1: 0, 2: 0, 3: 0, 4: 0}
    town_counts = {t: 0 for t in TOWN_IDS}
    for b in bots:
        voc_counts[((b["vocation"] - 1) % 4) + 1] += 1
        town_counts[b["town_id"]] += 1
    print(f"    Vocation distribution: {voc_counts}")
    print(f"    Town distribution:     {town_counts}")
    print(f"    Level range: {min(b['level'] for b in bots)} -> {max(b['level'] for b in bots)}")

    # Verify visual variety distribution
    unique_looktypes = len({b["looktype"] for b in bots})
    unique_color_tuples = len({(b["lookbody"], b["lookfeet"], b["lookhead"], b["looklegs"]) for b in bots})
    unique_mounts = len({b["chosen_mount"] for b in bots})
    print(f"    Visual variety: {unique_looktypes} unique looktypes, "
          f"{unique_color_tuples} unique color tuples, {unique_mounts} unique starting mounts")

    print(f"[*] Emitting SQL to {args.out}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    emit_sql(bots, mount_ids, outfit_catalog, args.out)
    print(f"    Done. {len(bots)} bots -> {args.out.stat().st_size:,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
