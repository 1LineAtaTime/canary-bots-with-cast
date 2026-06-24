# Dad's MMO Lab — Tibia (Canary Bots + Cast) Server: How-To Guide

A local Tibia server (Canary engine) that comes alive with ~500 AI **bot
players** — they hunt, travel, chat, trade, and own houses — plus a built-in
**cast** spectator mode so you can watch any character, bots included. Runs
natively on Linux. No Docker. No Proton.

> Powered by **canary-bots-with-cast**, a fork of
> [opentibiabr/canary](https://github.com/opentibiabr/canary) (forked at commit
> `ded10949d`) with the bot-player + cast systems added.

## What This Installs

- The **Canary** game server (prebuilt binary, or compiled from source).
- A **private MariaDB** database (its own datadir + port `33066`) seeded with
  ~997 bot characters, hunt routes, market data, equipment, and houses.
- An admin **god** account and the bot account.
- Optionally, the **MyAAC website** (account creation + web), served by PHP.
- A **Gaming Mode launcher** you add to Steam as a non-Steam game.

## Requirements

| | |
|---|---|
| Device | Steam Deck / Arch Linux (`pacman`) |
| Disk | ~15 GB free |
| RAM | 4 GB+ (8 GB+ if you choose compile-from-source) |
| Time | a few minutes (prebuilt) / 30–60 min (compile) |
| Client | OTClient Redemption (protocol 15) — you provide Tibia.dat/.spr |

## Step 1 — Run the Installer

```bash
chmod +x install-tibia.sh
./install-tibia.sh
```

The wizard asks two questions: prebuilt vs compile-from-source, and whether to
install the website. Then it does everything.

## What Happens During Install

### Step 1: Summary & Confirm
Shows where it installs and your choices.

### Step 2: Dependencies
Installs MariaDB (+ PHP if you chose the website) via `pacman`.

### Step 3: The Server (you pick one of three)
1. **Prebuilt (default, fast):** downloads the compiled `canary` binary,
   `libbot_engine.so`, and a runtime data tarball from the release, plus the map.
2. **Official Canary + patch (compile):** clones the official
   [opentibiabr/canary](https://github.com/opentibiabr/canary) at the exact fork
   commit (`ded10949d`), applies our `.patch`, and builds with vcpkg (~30–60 min).
   Maximum transparency — you see exactly what we changed on top of upstream.
3. **Our fork (compile):** clones this public fork directly and builds with vcpkg
   (~30–60 min).

All three produce the same server + bots; pick prebuilt for speed, option 2 for
auditability, option 3 for convenience. Compiling needs the vcpkg toolchain (see
the Canary build docs).

### Step 4: Database
Initializes a private MariaDB datadir and imports the schema + all bot data.
Admin login afterwards: **`@god` / `god12345`** (change it).

### Step 5: Launcher
Generates `~/tibia-canary-launcher.sh` (starts DB → website → server, stops them
cleanly when you close the window).

## Step 2 — Create Your Account

The seeded admin account is **`@god` / `god12345`** — log in with that to
administer, then change the password. To make normal player accounts, open the
website at `http://127.0.0.1` (if you installed it) and register.

## Step 3 — The Client (installed for you)

The installer downloads **OTClient Redemption**, drops `Tibia.dat`/`Tibia.spr`
into `data/things/1100/`, and writes its `init.lua` pointed at your local server.
Nothing to do here unless you skipped a step or want to do it by hand:

- Client: <https://github.com/opentibiabr/otclient/releases> (auto-downloaded)
- Assets (direct): <https://github.com/dudantas/tibia-client/releases/download/15.11.c9d1cf/client-11.zip>
  or <https://downloads.ots.me/data/tibia-clients/dat_and_spr/1501.zip> — put
  `Tibia.dat`/`Tibia.spr` in `data/things/1100/`.

The generated `init.lua` (local server) looks like this:

```lua
Services = {
    status        = "http://127.0.0.1/login.php",
    websites      = "http://127.0.0.1/?subtopic=accountmanagement",
    createAccount = "http://127.0.0.1/clientcreateaccount.php",
    getCoinsUrl   = "http://127.0.0.1/?subtopic=shop&step=terms",
}

Servers_init = {
    ["http://127.0.0.1/login.php"] = {
        ["port"] = 80,
        ["protocol"] = 1500,
        ["httpLogin"] = true
    }
}
```

To **spectate**, log in with account name **`@cast`** (no password) and pick a
character — bots included; they wake when you watch them.

## Step 4 — Add to Steam (Gaming Mode)

1. Desktop Mode → Steam → **Games → Add a Non-Steam Game**.
2. Browse to `/usr/bin/` → select **konsole** → Add.
3. Right-click it → **Properties** → rename to **Tibia Server**.
4. Launch Options:
   ```
   --hold -e bash ~/tibia-canary-launcher.sh
   ```
5. **Do NOT enable Proton** — it runs natively.

## Daily Use — Gaming Mode

Launch **Tibia Server** from your library, wait for "server is up", then start
OTClient and connect to `127.0.0.1`. Close the window to stop everything.

## Useful Commands

In-game (god account):

```
/cavebot active        # list active bots
/cavebot population    # how many are online
/cavebot reload        # hot-reload the bot engine (no restart)
```

Any player:

```
/party                 # summon nearby bots into a party
/cast on  |  /cast off # broadcast your own character
/house "<name>" owner  # claim a bot-owned house (also: sub-owner | release)
/cavebot claim         # reserve the hunt spawn you're standing in
```

## Files and Paths

| Path | What |
|---|---|
| `~/tibia-canary-server/` | server binary, datapack, private DB, website |
| `~/tibia-canary-server/config.lua` | server configuration (incl. `botPlayersOnline`) |
| `~/tibia-canary-server/mariadb-data/` | the private database |
| `~/tibia-canary-launcher.sh` | the Steam launcher |
| `/tmp/tibia-launch.log` | server log (check this if something's wrong) |

## Bot Settings

How many bots load is set by **`botPlayersOnline`** in `config.lua` (default
`500`, range `0`–~997). All other bot tuning lives in `config.lua` under the
`bot*` keys and hot-reloads with `/cavebot reload`. Full reference:
`data/scripts/lib/BOT_SYSTEM_DOCS.md` in the server folder.

## Troubleshooting

### Server won't start / closes immediately
Check `/tmp/tibia-launch.log`. Most common: the prebuilt binary needs a matching
glibc (built on Ubuntu 22.04 / current Arch is fine) — if not, re-run and choose
compile-from-source.

### "Database" errors
Make sure nothing else is using port `33066`. The launcher starts a private
MariaDB; it never touches a system database.

### Can't connect from the client
Confirm `init.lua` points at `127.0.0.1` port `7171`, and that the launcher
printed "server is up". Tibia.dat/.spr must be in `data/things/1100/`.

### Bots don't appear
Give it a minute after boot, and make sure `botPlayersOnline` > 0 in `config.lua`.

## Prefer Docker / a VPS?

This guide is the native, Steam-Deck-friendly path. For a containerized install
(Ubuntu/VPS), the same project ships a Docker stack — see the `dadsmmolab/`
folder in the [canary-bots-with-cast](https://github.com/1LineAtaTime/canary-bots-with-cast) repo.

## Links

- 📦 github.com/1LineAtaTime/canary-bots-with-cast
- 📺 youtube.com/@DadsMmoLab
- 📦 github.com/DadsMmoLab/dads-mmo-lab
