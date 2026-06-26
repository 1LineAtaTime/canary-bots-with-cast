# Canary Bots with Cast

A ready-to-run fork of the [OpenTibiaBR / Canary](https://github.com/opentibiabr/canary)
MMORPG server that adds **autonomous bot players** and a **cast (spectator)
system** — so a fresh server feels populated, and anyone can watch a live
character (including bots) without an account.

- **Bot players** hunt, travel between cities, chat, trade on the market, own
  houses, form parties, and defend themselves. You choose how many are online
  (0 → ~997).
- **Cast** lets spectators log in with the account name `@cast` and watch any
  broadcasting character; bots broadcast automatically and wake on click.

Behavior, commands, and tuning: **[data/scripts/lib/BOT_SYSTEM_DOCS.md](data/scripts/lib/BOT_SYSTEM_DOCS.md)**.
Complete developer documentation (architecture, behavior internals, performance,
and tuning) for continuing development: **[data/scripts/lib/BOT_SYSTEM_DOCS_EXTENDED.md](data/scripts/lib/BOT_SYSTEM_DOCS_EXTENDED.md)**.

## 🎥 Demo

Clips of the bot system in action — 720p/60fps (the two ~5-minute "tour" clips are 480p/60fps). Click any clip to play.

Only the "GOD" and "Legolas" characters are being controlled manually to demonstrate the bot features. The rest of the players are ALL bots.

### Bots living in the world

**Bots walking around the city**

https://github.com/user-attachments/assets/dbed64ea-f161-43d2-9145-d106bedecf01

**Adventurer-stone bots**

https://github.com/user-attachments/assets/762f2661-866e-43f5-8c08-980f7c166a36

**Houses owned &amp; decorated by bots**

https://github.com/user-attachments/assets/282b8c4e-047e-4dd3-bb2d-3972e45b9d56

### Hunting &amp; questing

**Bot team hunt** _(~5 min, in 3 parts)_

Part 1

https://github.com/user-attachments/assets/71c8b5b1-83c7-4b7c-b3db-4c9e2c5f3385

Part 2

https://github.com/user-attachments/assets/aad08b1e-14e1-4835-b701-d13a2c23b992

Part 3

https://github.com/user-attachments/assets/318778e7-ac0a-4ef7-a292-df20398615e5

**Bots questing**

https://github.com/user-attachments/assets/677683d3-a918-4173-9fa2-84a8b06a31fa

**Full tour — depot, cities &amp; more** _(~5 min)_

https://github.com/user-attachments/assets/5c86888e-603b-4c44-9041-f940f94e14ad

### PvP / PK

**PK bot team jumping a player**

https://github.com/user-attachments/assets/6ed612d8-e7cc-43e0-8032-c52b8664483e

**PK bot chasing a player**

https://github.com/user-attachments/assets/21e2b835-3b74-4758-9b87-460d076bb60b

### Systems

**Cast character list**

https://github.com/user-attachments/assets/f8275ba5-20b4-4e05-82fe-6592361d3c8f

**Market bot offers**

https://github.com/user-attachments/assets/022c82b5-0315-4f14-a618-3c7d3e77eca1

> [!NOTE]
> **Base version.** This repository is forked from `opentibiabr/canary` at commit
> **`ded10949d`** (2026-02-19) and adds the bot + cast features on top. Use that
> commit as the reference point for the underlying Canary version/protocol.

## Tested on

| | |
|---|---|
| Host | Proxmox LXC container |
| OS | Ubuntu 22.04.5 LTS (kernel 6.8.x) |
| CPU | Intel Core i5-6260U (4 vCPU) |
| RAM | 8 GB |
| DB | MySQL / MariaDB |
| Web | nginx + php-fpm 8.1 + MyAAC |
| Client | OTClient Redemption (protocol 13+) |

It runs comfortably with a few hundred bots online on this modest spec.

---

## Quick start

> The build toolchain (vcpkg + CMake) is identical to upstream Canary. If you are
> new to building Canary, follow the official
> [build guides](https://github.com/opentibiabr/canary/tree/main/docs/building)
> for the vcpkg/`VCPKG_ROOT` setup first — then build **this** repo (it already
> contains the bot engine source).

### 1. Build

```bash
git clone <this-repo-url> canary-bots
cd canary-bots
cmake --preset linux-release -DTOGGLE_BIN_FOLDER=ON
cmake --build --preset linux-release -j4
# Low-RAM machines: build with fewer jobs, e.g.
#   cmake --build build/linux-release -j2
```

Outputs `canary` and `libbot_engine.so` under `build/linux-release/bin/`. Copy
both next to your server working directory.

> Prefer not to compile? Prebuilt Linux binaries (`canary` + `libbot_engine.so`)
> are attached to the GitHub **Releases** for this repo.

### 2. Database

```bash
mysql -u root -p -e "CREATE DATABASE canary DEFAULT CHARSET=utf8mb3;"
mysql -u root -p canary < schema.sql
# Seed the bot data (god account, bot account, bots, hunts, market, …):
cd database/bots && DB_USER=root DB_PASS=yourpass DB_NAME=canary ./import.sh
```

See [database/bots/README.md](database/bots/README.md) for the manual import
order and details. Default admin login afterwards is account **`@god`** /
password **`god12345`** — change it after first login.

### 3. Configure

```bash
cp config.lua.dist config.lua
```

Edit `config.lua`: set your `mysql*` connection, `ip = "127.0.0.1"` for a local
install, and the bot keys (see below). Then download the world map
[`otservbr.otbm`](https://github.com/opentibiabr/canary/releases) from the
upstream Canary release matching the base version and place it per `mapName`.

### 4. Run

Start `canary` (the `.so` is loaded automatically). On first boot the migrations
finish setting up the bot tables. Connect with an OTClient (protocol 13+).

### 5. (Optional) Website + cast + client

- **Website / cast login:** install [MyAAC](https://github.com/slawkens/myaac)
  and drop in our cast-aware `deployment/web/login.php` (it intercepts the
  `@cast` account). See [deployment/client/README.md](deployment/client/README.md)
  for the OTClient `init.lua` (`Services` / `Servers_init`) settings.
- **Client:** [OTClient Redemption](https://github.com/opentibiabr/otclient).

---

## Bots at a glance

- **How many:** `botPlayersOnline` in `config.lua` (default `500`, range `0`–~997).
- **What they do:** idle/wander, walk to POIs (depot/temple/shops/NPCs), hunt,
  travel by boat, chat (observer-gated banter + trade ads + keyword replies),
  trade on the market, own & furnish houses, form parties, defend themselves,
  and occasionally run gang raids. Bots no one can see **hibernate** to keep CPU
  flat and **wake** when a player or cast viewer comes near.
- **Geared & alive:** level/vocation-appropriate equipment with forge tiers and
  imbuements, mounts, human-like mid-walk pauses, and the occasional dropped item.

### Key commands

| Command | Who | Purpose |
|---|---|---|
| `/cavebot active` / `population` | god | list active bots / population |
| `/cavebot reload` | god | hot-reload the bot engine (no restart) |
| `/cavebot reload debug,N` | god | debug mode with N bots + telemetry |
| `/cavebot claim` | player | reserve the hunt spawn you're standing in (bots won't take it; ~1h) |
| `/cavebot release` | player | release a hunt spawn you claimed |
| `/party` | player | summon nearby bots into your party |
| `/cast on` · `/cast off` | player | start/stop broadcasting your character |
| `/house "<name>" owner\|sub-owner\|release` | player | claim / sub-own / release a bot house |

Spectate: log in with account name **`@cast`** (no password) and pick a character.

Full command list: [BOT_SYSTEM_DOCS.md §12](data/scripts/lib/BOT_SYSTEM_DOCS.md).

### Common config (`config.lua`)

```lua
botPlayersOnline       = 500    -- how many bots load at startup (0 disables)
botPlayersShowAsOnline = true   -- count bots in the online list
botDensityCapEnabled   = true   -- cap how many bots wake around a player
botTelemetryEnabled    = false  -- leave off in production
```

All `bot*` keys are documented inline in `config.lua.dist` and grouped in
[BOT_SYSTEM_DOCS.md §13](data/scripts/lib/BOT_SYSTEM_DOCS.md). They hot-reload via
`/cavebot reload`.

---

## License & credits

GPL-2.0 (inherited from Canary) — see [LICENSE](LICENSE). Bundled bot/market data
is attributed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): community
forum routes/ideas (incl. the Gesior Thais bot post), Gunzodus (house NPCs +
decoration items), and Tibia Wiki / Fandom market data (CC BY-SA 3.0).

Built on the upstream [Canary](https://github.com/opentibiabr/canary) server.
Thanks to the Canary contributors and community ([Discord](https://discord.gg/gvTj5sh9Mp)).
