#!/usr/bin/env bash
# ============================================================================
# import.sh — load the bot schema + data into a Canary database.
# ----------------------------------------------------------------------------
# Prerequisite: the main `schema.sql` (repo root) must already be imported into
# the target database. This script then creates the bot tables and loads the
# bundled bot data (god account, bot account, GOD character, and all bot players
# + hunt/route/market/equipment data), in foreign-key order.
#
# Usage:
#   ./import.sh                 # uses the defaults below (local install)
#   DB_HOST=127.0.0.1 DB_USER=root DB_PASS=secret DB_NAME=canary ./import.sh
#
# Re-runnable: schema uses CREATE TABLE IF NOT EXISTS, but the data files use
# plain INSERTs — importing twice into the same DB will fail on duplicate keys.
# Import into a FRESH database.
# ============================================================================
set -euo pipefail

DB_HOST="${DB_HOST:-127.0.0.1}"
DB_PORT="${DB_PORT:-3306}"
DB_USER="${DB_USER:-root}"
DB_PASS="${DB_PASS:-}"
DB_NAME="${DB_NAME:-canary}"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mysql_args=(-h "$DB_HOST" -P "$DB_PORT" -u "$DB_USER")
[ -n "$DB_PASS" ] && mysql_args+=("-p$DB_PASS")
mysql_args+=("$DB_NAME")

run() {
  echo ">> importing $1"
  mysql "${mysql_args[@]}" < "$DIR/$1"
}

# Order matters: schema first, then parents before children (FKs).
FILES=(
  00_bot_schema.sql
  01_accounts.sql
  02_players.sql
  03_player_storage.sql
  04_bot_hunt_scripts.sql
  05_bot_hunt_waypoints.sql
  06_bot_hunt_targets.sql
  07_bot_hunt_fields.sql
  08_bot_hunt_exclusion_zones.sql
  09_bot_city_routes.sql
  10_bot_city_route_waypoints.sql
  11_bot_market_item_prices.sql
  12_bot_equipment.sql
  13_bot_town_mapping.sql
)

for f in "${FILES[@]}"; do run "$f"; done

# Keep new account/character registrations clear of the seeded bot id space.
echo ">> bumping AUTO_INCREMENT past the bot id range"
mysql "${mysql_args[@]}" <<'SQL'
ALTER TABLE `accounts` AUTO_INCREMENT = 65001;
ALTER TABLE `players`  AUTO_INCREMENT = 66100;
SQL

echo "Done. Bot data imported into '$DB_NAME'. Default admin login: @god / god12345 (change it)."
