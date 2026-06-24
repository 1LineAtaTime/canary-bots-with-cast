# Canary Bots with Cast — HOWTO (Docker one-install)

> ⚠️ **Status:** the Docker one-install is authored but **not yet verified on a
> Docker host** — expect to iterate on first run. The server, database, SQL
> dumps, and bot system itself are validated; the containerization (compile/
> prebuilt images + MyAAC web) is the new, untested part.

## 1. What this installs

A complete, self-hosted server on one machine via Docker Compose:

- **MariaDB** with the schema + bot data auto-imported (god account, bot account,
  ~997 bot players, hunts, market, equipment).
- **Canary bot server** (the game server + bot AI engine).
- **MyAAC website** with the cast (spectator) login.

## 2. Requirements

| | |
|---|---|
| OS | Linux (tested on Ubuntu 22.04) |
| Software | Docker Engine + `docker compose` plugin, `git` |
| Disk | ~15 GB (more for the compile build path) |
| RAM | 4 GB+ (8 GB+ if compiling from source) |
| Time | minutes (prebuilt) / 30-60 min (compile, first run) |

## 3. Step 1 — Run the installer

```bash
git clone <this-repo-url> canary-bots && cd canary-bots
chmod +x install.sh && ./install.sh
```

The wizard asks for: build mode (**prebuilt** default, or **compile**), server IP
(default `127.0.0.1`), how many bots (`200`), DB passwords, and an optional client
assets URL (§7).

## 4. What happens during install

1. Checks Docker/git are present.
2. Writes `.env` from `dadsmmolab/.env.dist` with your answers.
3. `docker compose up -d --build` brings up `database`, `server`, `web`.
4. On first boot the server imports `schema.sql` + `database/bots/*` and bumps
   auto-increment past the bot id range, then loads the bots.
5. The installer waits for the "bot system library loaded" log line.

## 5. Step 2 — Your account

A god account is seeded: log in with account **`@god`** / password
**`god12345`**, then **change the password**. (Account name is `god`, email is
`@god`; either works to log in.) Create normal player accounts via the website.

## 6. Step 3 — Point your client

Use [OTClient Redemption](https://github.com/opentibiabr/otclient). Edit its
`init.lua` to point `Servers_init` / `Services` at your server's `login.php`
(`http://<SERVER_IP>/login.php`, port 80, protocol 1500, `httpLogin = true`).
Full snippet: [deployment/client/README.md](deployment/client/README.md).

To **spectate**, log in with the account name **`@cast`** (no password) and pick
any character — bots included (they wake on click).

## 7. Step 4 — Game assets (Tibia.dat / Tibia.spr)

These protocol-1100 graphics are **copyrighted CipSoft files** and are not
bundled. Provide a `CLIENT_ASSETS_URL` (in `.env`) pointing to a `Tibia.dat` /
`Tibia.spr` source you control/are entitled to, or place them manually in the
client's `data/things/1100/`. Where to obtain them:

- https://github.com/opentibiabr/otclient/wiki/Tutorial-Protocol-12.x-assets
- https://github.com/dudantas/tibia-client/releases
- https://downloads.ots.me/ → `data/tibia-clients/dat_and_spr/`

## 8. Daily use

```bash
# logs
docker compose --env-file .env -f dadsmmolab/docker-compose.yml logs -f server
# stop / start
docker compose --env-file .env -f dadsmmolab/docker-compose.yml down
docker compose --env-file .env -f dadsmmolab/docker-compose.yml up -d
```

## 9. Useful commands

| Command | What |
|---|---|
| `/cavebot active` / `population` | (god) list bots / population |
| `/cavebot reload` | (god) hot-reload the bot engine |
| `/party` | (player) summon bots into a party |
| `/cast on` · `/cast off` | (player) broadcast your character |

## 10. Files and paths

| Path | What |
|---|---|
| `.env` | your configuration (created by the installer) |
| `dadsmmolab/docker-compose.yml` | the stack |
| `dadsmmolab/Dockerfile.server` | compile/prebuilt server image |
| `dadsmmolab/Dockerfile.web` | MyAAC + nginx + php-fpm image |
| `database/bots/` | the bot data dumps + import tooling |
| `data/scripts/lib/BOT_SYSTEM_DOCS.md` | full bot system reference |

## 11. Bot settings

`BOT_PLAYERS_ONLINE` in `.env` sets how many bots load (0-997). All other bot
tuning lives in `config.lua` (`bot*` keys) and hot-reloads via `/cavebot reload`.
See [BOT_SYSTEM_DOCS.md](data/scripts/lib/BOT_SYSTEM_DOCS.md).

## 12. Troubleshooting

- **Server keeps restarting:** `... logs server`. Common causes: DB not ready
  (it retries), or (prebuilt) a wrong `RELEASE_ASSETS_URL`.
- **Compile path fails / OOM:** give Docker more RAM, or switch to `prebuilt`.
- **Website 502:** the `web` container's php-fpm/MyAAC is still starting, or
  MyAAC needs a moment to auto-migrate on first hit.
- **Bots don't appear in cast list:** confirm the server logged "bot system
  library loaded" and `botPlayersOnline` > 0.
