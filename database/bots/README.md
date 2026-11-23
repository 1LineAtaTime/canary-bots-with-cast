# Bot data — `database/bots/`

This folder seeds a fresh Canary database with the bot system's runtime data: the
admin account, the bot account and its 997 bot players, the market price
reference, and bot house ownership + furniture.

> **The engine does not read hunt or route data from MySQL.** Since the BOT_CSV
> migration it loads that from `data/bot/authored/*.csv`, which ships in this
> repo — hunt scripts and their waypoints, city routes, POIs, equipment, town
> mapping and travel positions. There is nothing to import for any of it.
> To add or change a hunt script, edit the CSVs — see BOT_SYSTEM_DOCS.md §6c and
> `tools/cfg_importer/`, which imports an OTClient cavebot `.cfg` directly.

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

After import, the default admin login is account **`@god`** / password
**`god12345`** — change it after first login.

## Manual import (e.g. MySQL Workbench, no shell)

Run the files in this exact order (foreign-key / dependency order):

| # | File | Contents |
|---|------|----------|
| 00 | `00_bot_schema.sql` | all bot table DDL (`CREATE TABLE IF NOT EXISTS`), including the runtime/telemetry tables `bot_active_players`, `bot_commands`, `bot_chat_emissions`, `bot_hub_presence_60s` |
| 01 | `01_accounts.sql` | god account (`@god` / `god12345`) + bot account |
| 02 | `02_players.sql` | the GOD character + 997 bot players |
| 03 | `03_player_storage.sql` | bot/GOD storage (mount & outfit unlocks, etc.) |
| 11 | `11_bot_market_item_prices.sql` | market price reference (see header) |
| 14 | `14_houses.sql` | house ownership + metadata (bots own 794 of 993 houses) |
| 15 | `15_tile_store.sql` | house furniture/items (per-house serialized tile blobs) |
| 16 | `16_runtime_compatibility.sql` | runtime tables the engine/Lua write (`bot_state_persistence`, `bot_startup_config`, `bot_nav_events`, `bot_test_commands`) + widens `players.onlinetime` and the `player_charms` counters on databases created from an older `schema.sql`. Safe to re-run. |
| 23 | `23_bot_onlinetime_reset.sql` | zeroes the bot account's garbage `onlinetime`. Run with canary stopped. |
| 24 | `24_bot_state_persistence_saved_at.sql` | adds `bot_state_persistence.saved_at`; the engine restores only tasks saved within 24 h. Idempotent. **Must run before** an engine built with it starts. |
| 25 | `25_bot_hunt_danger.sql` | creates `bot_hunt_danger` (learned hunt-danger zones; also in `00_bot_schema.sql`). Idempotent; safe with canary up. Without it zones are memory-only. |
| 26 | `26_monk_conversion.sql` | turns 199 bots (20 % of every loaded/vocation/level-band stratum) into monks / exalted monks IN PLACE — vocation, HP/mana/cap from the 10/10/25 formula, the Monk outfit for half of them. Names, levels and storage unchanged. It is how `02_players.sql` gets its monks (02 is patched by this file, never re-dumped). On a live server run it with canary **stopped** via `tools/bot_population_generator/apply_monk_conversion.sh apply` (backup + exact rollback); `26_monk_conversion.before.sql` is the snapshot rollback, `26_monk_conversion.ids.txt` the converted ids. Idempotent. |

The numbering has gaps because the files that filled them seeded the old
hunt/route/equipment tables. Those are CSV now, and the tables are gone.

Then bump the auto-increment so new registrations don't collide with the seeded
bot id range:

```sql
ALTER TABLE `accounts` AUTO_INCREMENT = 65001;
ALTER TABLE `players`  AUTO_INCREMENT = 66100;
```

## Schema and `db_version`

- The bot schema is **not** a numbered migration. `data-otservbr-global/migrations/` holds
  upstream's files only, so `db_version` tracks upstream and upstream's own migrations always
  apply. `tools/fork_delta_check.sh` fails if a fork-owned file appears there (#150).
- `91_upstream_1525_schema.sql` is a **cutover-only** script, not part of the import above. It
  reconciles a database last used by the 15.00 bot server (which our old `62.lua` left at
  `db_version=62`) with upstream's migrations 56-59 and sets `db_version=59`. Run it once, with
  canary stopped and after a mysqldump, before the 15.25 port binary first starts. It is
  idempotent.

## Notes

- **No personal data.** The god account ships with the documented default
  login (account `@god` / password `god12345`), emails are placeholders, and
  last-login IPs are zeroed. Change the password after first login.
- **Data provenance** (`source` / `source_file`) is generic in the distributed
  authored CSVs; the functional `source_name` routing key on city routes is kept.
  Wiki-derived market prices are attributed under CC BY-SA 3.0 — see the header
  of `11_bot_market_item_prices.sql` and `THIRD_PARTY_NOTICES.md`.
- **House data** (`14_houses.sql` / `15_tile_store.sql`) pre-seeds bot house
  ownership and the furniture in those houses. House ids match the bundled
  `otservbr` map, so import these into the same map distribution. All owned
  houses belong to bots (account `65000`); there is no real-player ownership or
  bid data. Houses left unowned (owner `0`) simply won't render their stored
  items until a bot claims them.
- **Bot count at runtime** is controlled by `botPlayersOnline` in `config.lua`
  (default 500, capped at the 997 seeded). You do not need all of them online.
- To regenerate the bot population at a different size/name-set, see
  `tools/bot_population_generator/`.
