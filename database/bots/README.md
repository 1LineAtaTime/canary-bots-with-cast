# Bot data — `database/bots/`

This folder seeds a fresh Canary database with the bot system's data: the admin
account, the bot account and its ~997 bot players, and all hunt / city-route /
market / equipment reference data.

## Prerequisites

1. A MySQL/MariaDB database created and reachable.
2. The repo-root **`schema.sql`** already imported into it. (This distribution's
   `schema.sql` intentionally does **not** seed the default god account or sample
   characters — those come from the dumps here.)

## Quick import

```bash
DB_HOST=127.0.0.1 DB_USER=root DB_PASS=yourpass DB_NAME=canary ./import.sh
```

Defaults if you omit the variables: host `127.0.0.1`, port `3306`, user `root`,
empty password, database `canary`. Import into a **fresh** database (the data
files use plain `INSERT`s and will conflict on a re-import).

After import, the default admin login is **`god` / `god`** — change the password
after first login.

## Manual import (e.g. MySQL Workbench, no shell)

Run the files in this exact order (foreign-key / dependency order):

| # | File | Contents |
|---|------|----------|
| 00 | `00_bot_schema.sql` | bot table DDL (`CREATE TABLE IF NOT EXISTS`) |
| 01 | `01_accounts.sql` | god account (`@god` / `god12345`) + bot account |
| 02 | `02_players.sql` | the GOD character + ~997 bot players |
| 03 | `03_player_storage.sql` | bot/GOD storage (mount & outfit unlocks, etc.) |
| 04 | `04_bot_hunt_scripts.sql` | hunt script headers |
| 05 | `05_bot_hunt_waypoints.sql` | hunt waypoints |
| 06 | `06_bot_hunt_targets.sql` | hunt target monsters |
| 07 | `07_bot_hunt_fields.sql` | field (fire/energy/…) handling |
| 08 | `08_bot_hunt_exclusion_zones.sql` | hunt exclusion zones |
| 09 | `09_bot_city_routes.sql` | city navigation routes |
| 10 | `10_bot_city_route_waypoints.sql` | city route waypoints |
| 11 | `11_bot_market_item_prices.sql` | market price reference (see header) |
| 12 | `12_bot_equipment.sql` | per level/vocation equipment loadouts |
| 13 | `13_bot_town_mapping.sql` | source-town → Canary-town mapping |

Then bump the auto-increment so new registrations don't collide with the seeded
bot id range:

```sql
ALTER TABLE `accounts` AUTO_INCREMENT = 65001;
ALTER TABLE `players`  AUTO_INCREMENT = 66100;
```

## Notes

- **No personal data.** The god account ships with the documented default
  login (account `@god` / password `god12345`), emails are placeholders, and
  last-login IPs are zeroed. Change the password after first login.
- **Data provenance** (`source` / `source_file`) is intentionally omitted from
  the dumps; the functional `source_name` routing key on city routes is kept.
  Wiki-derived market prices are attributed under CC BY-SA 3.0 — see the header
  of `11_bot_market_item_prices.sql` and `THIRD_PARTY_NOTICES.md`.
- **Bot count at runtime** is controlled by `botPlayersOnline` in `config.lua`
  (default 200, capped at the ~997 seeded). You do not need all of them online.
- To regenerate the bot population at a different size/name-set, see
  `tools/bot_population_generator/`.
