#!/usr/bin/env python3
"""Load data/bot/authored/*.csv back into MySQL tables.

Two jobs:

1. ROLLBACK. If the CSV migration has to be reverted, this refills the original
   tables from the CSVs so the pre-CSV engine .so has current data to read.

2. THE SQL-SURVEY WORKFLOW. The recurring analyses on this project (z-match %,
   spawn proximity, level distribution, route de-dup) are SQL one-liners, and
   after the migration the data is in files. Point this at a SCRATCH database
   and the one-liners work again:

       python tools/bot_csv/import_to_mysql.py --db canary_scratch --create

By default it refuses to touch the `canary` database, because a rollback and a
survey look identical from here and only one of them is meant to overwrite
production. Pass --i-mean-it to override.

Reads through validate.py's parser, so a tree that fails validation cannot be
imported — same gate as the engine.
"""

import argparse
import io
import os
import sys

import mysql.connector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csv_common import AUTHORED_DIR, announce_authored_dir  # noqa: E402
from validate import CsvError, Table  # noqa: E402

# CREATE statements mirror the live schema as of the migration. Only used with
# --create (scratch databases); a rollback into the real DB uses the existing
# tables and must NOT redefine them.
SCHEMA = {
    "bot_hunt_scripts": """
        CREATE TABLE IF NOT EXISTS `bot_hunt_scripts` (
          `id` INT UNSIGNED NOT NULL PRIMARY KEY,
          `name` VARCHAR(128) NOT NULL,
          `source` VARCHAR(32) NOT NULL DEFAULT 'manual',
          `source_file` VARCHAR(255) NOT NULL DEFAULT '',
          `town_name` VARCHAR(64) NOT NULL DEFAULT '',
          `town_id` INT NOT NULL,
          `min_level` INT NOT NULL DEFAULT 1,
          `max_level` INT NOT NULL DEFAULT 9999,
          `vocation_mask` TINYINT UNSIGNED NOT NULL DEFAULT 15,
          `keep_distance_ek` TINYINT UNSIGNED NOT NULL DEFAULT 0,
          `keep_distance_ms` TINYINT UNSIGNED NOT NULL DEFAULT 0,
          `keep_distance_ed` TINYINT UNSIGNED NOT NULL DEFAULT 0,
          `keep_distance_rp` TINYINT UNSIGNED NOT NULL DEFAULT 0,
          `script_type` VARCHAR(32) DEFAULT NULL,
          `enabled` TINYINT(1) NOT NULL DEFAULT 1,
          `is_quest` TINYINT(1) NOT NULL DEFAULT 0,
          `script_category` VARCHAR(16) NOT NULL DEFAULT 'hunt',
          KEY `idx_town` (`town_id`)
        ) ENGINE=InnoDB""",
    "bot_hunt_waypoints": """
        CREATE TABLE IF NOT EXISTS `bot_hunt_waypoints` (
          `id` INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
          `script_id` INT UNSIGNED NOT NULL,
          `phase` VARCHAR(16) NOT NULL,
          `seq` INT UNSIGNED NOT NULL,
          `waypoint_type` VARCHAR(24) NOT NULL,
          `pos_x` INT NOT NULL, `pos_y` INT NOT NULL, `pos_z` TINYINT NOT NULL,
          `label` VARCHAR(128) DEFAULT NULL,
          `extra_data` VARCHAR(255) DEFAULT NULL,
          KEY `idx_script_phase_seq` (`script_id`,`phase`,`seq`)
        ) ENGINE=InnoDB""",
    "bot_hunt_targets": """
        CREATE TABLE IF NOT EXISTS `bot_hunt_targets` (
          `id` INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
          `script_id` INT UNSIGNED NOT NULL,
          `monster_name` VARCHAR(64) NOT NULL,
          KEY `idx_target_script` (`script_id`)
        ) ENGINE=InnoDB""",
    "bot_city_routes": """
        CREATE TABLE IF NOT EXISTS `bot_city_routes` (
          `id` INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
          `town_name` VARCHAR(64) NOT NULL DEFAULT '',
          `town_id` INT NOT NULL,
          `route_type` VARCHAR(32) NOT NULL DEFAULT '',
          `source_name` VARCHAR(128) DEFAULT NULL,
          `source` VARCHAR(32) NOT NULL DEFAULT 'manual',
          `enabled` TINYINT(1) NOT NULL DEFAULT 1,
          KEY `idx_town_id` (`town_id`)
        ) ENGINE=InnoDB""",
    "bot_city_route_waypoints": """
        CREATE TABLE IF NOT EXISTS `bot_city_route_waypoints` (
          `id` INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
          `route_id` INT UNSIGNED NOT NULL,
          `seq` INT UNSIGNED NOT NULL,
          `waypoint_type` VARCHAR(24) NOT NULL,
          `pos_x` INT NOT NULL, `pos_y` INT NOT NULL, `pos_z` TINYINT NOT NULL,
          `action_label` VARCHAR(128) DEFAULT NULL,
          KEY `idx_route_seq` (`route_id`,`seq`)
        ) ENGINE=InnoDB""",
    "bot_city_pois": """
        CREATE TABLE IF NOT EXISTS `bot_city_pois` (
          `id` INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
          `town_id` INT UNSIGNED NOT NULL,
          `name` VARCHAR(100) NOT NULL,
          `pos_x` SMALLINT UNSIGNED NOT NULL,
          `pos_y` SMALLINT UNSIGNED NOT NULL,
          `pos_z` TINYINT UNSIGNED NOT NULL,
          `poi_type` VARCHAR(24) NOT NULL DEFAULT 'depot',
          `weight` INT DEFAULT NULL,
          `enabled` TINYINT(1) NOT NULL DEFAULT 1,
          UNIQUE KEY `uq_town_name` (`town_id`,`name`)
        ) ENGINE=InnoDB""",
    "bot_equipment": """
        CREATE TABLE IF NOT EXISTS `bot_equipment` (
          `id` INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
          `level` SMALLINT UNSIGNED NOT NULL,
          `vocation` TINYINT UNSIGNED NOT NULL,
          `slot_head` INT UNSIGNED DEFAULT NULL,
          `slot_armor` INT UNSIGNED DEFAULT NULL,
          `slot_legs` INT UNSIGNED DEFAULT NULL,
          `slot_feet` INT UNSIGNED DEFAULT NULL,
          `slot_right` INT UNSIGNED DEFAULT NULL,
          `slot_left` INT UNSIGNED DEFAULT NULL,
          `slot_backpack` INT UNSIGNED DEFAULT 0,
          UNIQUE KEY `uq_level_voc` (`level`,`vocation`)
        ) ENGINE=InnoDB""",
    "travel_positions": """
        CREATE TABLE IF NOT EXISTS `travel_positions` (
          `source_name` VARCHAR(64) NOT NULL PRIMARY KEY,
          `pos_x` INT NOT NULL, `pos_y` INT NOT NULL, `pos_z` TINYINT UNSIGNED NOT NULL
        ) ENGINE=InnoDB""",
    "bot_town_mapping": """
        CREATE TABLE IF NOT EXISTS `bot_town_mapping` (
          `source_name` VARCHAR(64) NOT NULL PRIMARY KEY,
          `canary_town_id` INT NOT NULL,
          `canary_town_name` VARCHAR(64) NOT NULL DEFAULT ''
        ) ENGINE=InnoDB""",
}

ORDER = ["bot_hunt_scripts", "bot_hunt_waypoints", "bot_hunt_targets",
         "bot_city_routes", "bot_city_route_waypoints", "bot_city_pois",
         "bot_equipment", "travel_positions", "bot_town_mapping"]


def p(*parts):
    return os.path.join(AUTHORED_DIR, *parts)


def n(v):
    """CSV text -> value, empty becomes NULL (the export wrote NULL as empty)."""
    return None if v == "" else v


def load_all(cur):
    counts = {}

    t = Table(p("hunt_scripts.csv"),
              ["id", "name", "town_id", "min_level", "max_level", "vocation_mask",
               "keep_distance_ek", "keep_distance_ms", "keep_distance_ed",
               "keep_distance_rp", "enabled", "is_quest", "script_category",
               # CSV-only (BOT_LURE_KITE): declared so the header parses, deliberately
               # NOT inserted — bot_hunt_scripts has no min_monsters column and the
               # engine reads this value from the CSV, never from MySQL.
               "min_monsters"],
              ["source", "source_file", "town_name", "script_type"])
    rows = [(t.raw(i, "id"), t.raw(i, "name"), n(t.raw(i, "source")) or "manual",
             t.raw(i, "source_file"), t.raw(i, "town_name"), t.raw(i, "town_id"),
             t.raw(i, "min_level"), t.raw(i, "max_level"), t.raw(i, "vocation_mask"),
             t.raw(i, "keep_distance_ek"), t.raw(i, "keep_distance_ms"),
             t.raw(i, "keep_distance_ed"), t.raw(i, "keep_distance_rp"),
             n(t.raw(i, "script_type")), t.raw(i, "enabled"), t.raw(i, "is_quest"),
             t.raw(i, "script_category")) for i in range(len(t))]
    cur.executemany(
        "INSERT INTO bot_hunt_scripts (id,name,source,source_file,town_name,town_id,"
        "min_level,max_level,vocation_mask,keep_distance_ek,keep_distance_ms,"
        "keep_distance_ed,keep_distance_rp,script_type,enabled,is_quest,script_category) "
        "VALUES (" + ",".join(["%s"] * 17) + ")", rows)
    counts["bot_hunt_scripts"] = len(rows)
    script_ids = [int(t.raw(i, "id")) for i in range(len(t))]

    # seq is regenerated from LINE ORDER within each phase — that is the whole point
    # of dropping the column, and it is what makes this round-trip lossless.
    rows = []
    for sid in script_ids:
        w = Table(p("hunt_waypoints", f"{sid}.csv"),
                  ["phase", "waypoint_type", "pos_x", "pos_y", "pos_z"],
                  ["label", "extra_data"])
        seq = {}
        for i in range(len(w)):
            ph = w.raw(i, "phase")
            s = seq.get(ph, 0)
            seq[ph] = s + 1
            rows.append((sid, ph, s, w.raw(i, "waypoint_type"), w.raw(i, "pos_x"),
                         w.raw(i, "pos_y"), w.raw(i, "pos_z"),
                         n(w.raw(i, "label")), n(w.raw(i, "extra_data"))))
    cur.executemany(
        "INSERT INTO bot_hunt_waypoints (script_id,phase,seq,waypoint_type,pos_x,pos_y,"
        "pos_z,label,extra_data) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s)", rows)
    counts["bot_hunt_waypoints"] = len(rows)

    t = Table(p("hunt_targets.csv"), ["script_id", "monster_name"])
    rows = [(t.raw(i, "script_id"), t.raw(i, "monster_name")) for i in range(len(t))]
    cur.executemany("INSERT INTO bot_hunt_targets (script_id,monster_name) VALUES (%s,%s)", rows)
    counts["bot_hunt_targets"] = len(rows)

    # Route ids are synthesized in file order; nothing outside these two tables
    # references them (the engine keys routes by town_id + source_name).
    t = Table(p("city_routes.csv"), ["town_id", "source_name", "enabled"])
    route_id = {}
    rows = []
    for i in range(len(t)):
        rid = i + 1
        route_id[(t.raw(i, "town_id"), t.raw(i, "source_name"))] = rid
        rows.append((rid, t.raw(i, "town_id"), t.raw(i, "source_name"), t.raw(i, "enabled")))
    cur.executemany(
        "INSERT INTO bot_city_routes (id,town_id,source_name,enabled) VALUES (%s,%s,%s,%s)", rows)
    counts["bot_city_routes"] = len(rows)

    rows = []
    rwp_dir = p("city_route_waypoints")
    for fn in sorted(os.listdir(rwp_dir)):
        if not fn.endswith(".csv"):
            continue
        town = fn[5:-4]
        w = Table(os.path.join(rwp_dir, fn),
                  ["source_name", "waypoint_type", "pos_x", "pos_y", "pos_z"], ["action_label"])
        seq = {}
        for i in range(len(w)):
            sn = w.raw(i, "source_name")
            rid = route_id.get((town, sn))
            if rid is None:
                raise CsvError(w.path, w.line(i), 0,
                               f"waypoint group '{sn}' has no city_routes.csv row for town {town}")
            s = seq.get(sn, 0)
            seq[sn] = s + 1
            rows.append((rid, s, w.raw(i, "waypoint_type"), w.raw(i, "pos_x"),
                         w.raw(i, "pos_y"), w.raw(i, "pos_z"), n(w.raw(i, "action_label"))))
    cur.executemany(
        "INSERT INTO bot_city_route_waypoints (route_id,seq,waypoint_type,pos_x,pos_y,pos_z,"
        "action_label) VALUES (%s,%s,%s,%s,%s,%s,%s)", rows)
    counts["bot_city_route_waypoints"] = len(rows)

    t = Table(p("city_pois.csv"),
              ["town_id", "name", "pos_x", "pos_y", "pos_z", "poi_type", "enabled"], ["weight"])
    rows = [(t.raw(i, "town_id"), t.raw(i, "name"), t.raw(i, "pos_x"), t.raw(i, "pos_y"),
             t.raw(i, "pos_z"), t.raw(i, "poi_type"), n(t.raw(i, "weight")),
             t.raw(i, "enabled")) for i in range(len(t))]
    cur.executemany(
        "INSERT INTO bot_city_pois (town_id,name,pos_x,pos_y,pos_z,poi_type,weight,enabled) "
        "VALUES (%s,%s,%s,%s,%s,%s,%s,%s)", rows)
    counts["bot_city_pois"] = len(rows)

    t = Table(p("equipment.csv"),
              ["level", "vocation", "slot_head", "slot_armor", "slot_legs", "slot_feet",
               "slot_right", "slot_left", "slot_backpack"])
    rows = [(t.raw(i, "level"), t.raw(i, "vocation"), n(t.raw(i, "slot_head")),
             n(t.raw(i, "slot_armor")), n(t.raw(i, "slot_legs")), n(t.raw(i, "slot_feet")),
             n(t.raw(i, "slot_right")), n(t.raw(i, "slot_left")),
             n(t.raw(i, "slot_backpack"))) for i in range(len(t))]
    cur.executemany(
        "INSERT INTO bot_equipment (level,vocation,slot_head,slot_armor,slot_legs,slot_feet,"
        "slot_right,slot_left,slot_backpack) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s)", rows)
    counts["bot_equipment"] = len(rows)

    t = Table(p("travel_positions.csv"), ["source_name", "pos_x", "pos_y", "pos_z"])
    rows = [(t.raw(i, "source_name"), t.raw(i, "pos_x"), t.raw(i, "pos_y"),
             t.raw(i, "pos_z")) for i in range(len(t))]
    cur.executemany(
        "INSERT INTO travel_positions (source_name,pos_x,pos_y,pos_z) VALUES (%s,%s,%s,%s)", rows)
    counts["travel_positions"] = len(rows)

    t = Table(p("town_mapping.csv"), ["source_name", "canary_town_id"], ["canary_town_name"])
    rows = [(t.raw(i, "source_name"), t.raw(i, "canary_town_id"),
             t.raw(i, "canary_town_name")) for i in range(len(t))]
    cur.executemany(
        "INSERT INTO bot_town_mapping (source_name,canary_town_id,canary_town_name) "
        "VALUES (%s,%s,%s)", rows)
    counts["bot_town_mapping"] = len(rows)

    return counts


def main():
    announce_authored_dir("import_to_mysql")
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.environ.get("DB_HOST", "127.0.0.1"))
    ap.add_argument("--user", default=os.environ.get("DB_USER", "root"))
    ap.add_argument("--password", default=os.environ.get("DB_PASS", ""))
    ap.add_argument("--db", required=True, help="target database (NOT canary unless --i-mean-it)")
    ap.add_argument("--create", action="store_true", help="CREATE TABLE IF NOT EXISTS first")
    ap.add_argument("--i-mean-it", action="store_true",
                    help="allow writing to the production 'canary' database")
    args = ap.parse_args()

    if args.db == "canary" and not args.i_mean_it:
        print("REFUSING: --db canary overwrites production authored tables.\n"
              "This is the rollback path, not the survey path. For surveys use a scratch\n"
              "database (--db canary_scratch --create). To really roll back, add --i-mean-it\n"
              "and make sure canary is STOPPED first.")
        return 2

    cx = mysql.connector.connect(host=args.host, user=args.user,
                                 password=args.password, database=args.db)
    cur = cx.cursor()
    try:
        if args.create:
            for name in ORDER:
                cur.execute(SCHEMA[name])
        # One transaction: a partial import is worse than none.
        cur.execute("SET FOREIGN_KEY_CHECKS=0")
        for name in reversed(ORDER):
            cur.execute(f"DELETE FROM `{name}`")
        counts = load_all(cur)
        cur.execute("SET FOREIGN_KEY_CHECKS=1")
        cx.commit()
    except (CsvError, mysql.connector.Error) as e:
        cx.rollback()
        print(f"FAILED (rolled back, nothing written): {e}")
        return 1
    finally:
        cur.close()
        cx.close()

    width = max(len(k) for k in counts)
    for k in ORDER:
        print(f"  {k:<{width}}  {counts[k]:>6} rows")
    print(f"\nImported into `{args.db}`")
    return 0


if __name__ == "__main__":
    sys.exit(main())
