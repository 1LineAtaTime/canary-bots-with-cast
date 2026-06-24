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
