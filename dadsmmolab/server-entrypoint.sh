#!/usr/bin/env bash
# Canary server entrypoint for the dadsmmolab stack. Prepares config.lua from
# the shipped .dist, waits for the DB, imports schema + bot data on first run,
# downloads the map, and starts canary. Idempotent: safe to restart.
set -euo pipefail

cd /srv/canary

DB_HOST="${MYSQL_HOST:-database}"; DB_PORT="${MYSQL_PORT:-3306}"
DB_USER="${MYSQL_USER:-canary}"; DB_PASS="${MYSQL_PASSWORD:-canary}"
DB_NAME="${MYSQL_DATABASE:-canary}"
SERVER_IP="${SERVER_IP:-127.0.0.1}"
MAP_URL="${MAP_URL:-}"
BOT_PLAYERS_ONLINE="${BOT_PLAYERS_ONLINE:-200}"

mysql_c() { mysql -u "$DB_USER" -p"$DB_PASS" -h "$DB_HOST" --port="$DB_PORT" "$@"; }

echo "===== Prepare config.lua ====="
[ -f config.lua ] || cp config.lua.dist config.lua
sed -i "/^mysqlHost = .*/c\\mysqlHost = \"$DB_HOST\"" config.lua
sed -i "/^mysqlUser = .*/c\\mysqlUser = \"$DB_USER\"" config.lua
sed -i "/^mysqlPass = .*/c\\mysqlPass = \"$DB_PASS\"" config.lua
sed -i "/^mysqlPort = .*/c\\mysqlPort = $DB_PORT" config.lua
sed -i "/^mysqlDatabase = .*/c\\mysqlDatabase = \"$DB_NAME\"" config.lua
sed -i "/^ip = .*/c\\ip = \"$SERVER_IP\"" config.lua
sed -i "/^botPlayersOnline = .*/c\\botPlayersOnline = $BOT_PLAYERS_ONLINE" config.lua

echo "===== Download map if missing ====="
if [ -n "$MAP_URL" ] && [ ! -f data-otservbr-global/world/otservbr.otbm ]; then
  wget --no-check-certificate "$MAP_URL" -O data-otservbr-global/world/otservbr.otbm
fi

echo "===== Wait for DB ====="
until mysql_c -e "SELECT 1" >/dev/null 2>&1; do echo "DB not ready..."; sleep 3; done

echo "===== First-run DB import (schema + bot data) ====="
if mysql_c -D "$DB_NAME" -e 'SHOW TABLES LIKE "server_config"' | grep -q server_config; then
  echo "Already initialized; skipping import."
else
  echo "Importing schema.sql"
  mysql_c -D "$DB_NAME" < schema.sql
  echo "Importing bot data (database/bots)"
  for f in database/bots/00_bot_schema.sql \
           database/bots/01_accounts.sql database/bots/02_players.sql \
           database/bots/03_player_storage.sql \
           database/bots/04_bot_hunt_scripts.sql database/bots/05_bot_hunt_waypoints.sql \
           database/bots/06_bot_hunt_targets.sql database/bots/07_bot_hunt_fields.sql \
           database/bots/08_bot_hunt_exclusion_zones.sql \
           database/bots/09_bot_city_routes.sql database/bots/10_bot_city_route_waypoints.sql \
           database/bots/11_bot_market_item_prices.sql database/bots/12_bot_equipment.sql \
           database/bots/13_bot_town_mapping.sql; do
    echo "  -> $f"; mysql_c -D "$DB_NAME" < "$f"
  done
  mysql_c -D "$DB_NAME" -e "ALTER TABLE accounts AUTO_INCREMENT=65001; ALTER TABLE players AUTO_INCREMENT=66100;"
  echo "Import complete. Admin login: @god / god12345 (change it)."
fi

echo "===== Start Canary ====="
ulimit -c unlimited || true
exec ./canary
