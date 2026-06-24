#!/usr/bin/env bash
# ============================================================================
# Canary Bots with Cast — one-install (Docker). Interactive wizard that brings
# up MariaDB + the Canary bot server + MyAAC website with cast, on this machine.
#
#   chmod +x install-docker.sh && ./install-docker.sh
#
# This is the Docker alternative (VPS/Ubuntu). For the native Steam-Deck/Arch
# install, see guides/tibia/install-tibia.sh.
#
# NOTE: This Docker stack is authored but not yet verified on a Docker host.
# Expect to iterate on first run. See dadsmmolab/ for the compose + Dockerfiles.
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE="$ROOT/dadsmmolab/docker-compose.yml"
ENV_DIST="$ROOT/dadsmmolab/.env.dist"
ENV_FILE="$ROOT/.env"

say() { printf '\n\033[1;36m%s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31m%s\033[0m\n' "$*" >&2; exit 1; }

say "== Canary Bots with Cast — installer =="

# ---- Preflight ----
[ "$(uname -s)" = "Linux" ] || say "WARN: tested on Linux; other OSes may need tweaks."
command -v git >/dev/null   || die "git is required. Install it and re-run."
if ! command -v docker >/dev/null; then
  die "Docker is required. Install Docker Engine + the compose plugin, then re-run."
fi
docker compose version >/dev/null 2>&1 || die "The 'docker compose' plugin is required."

# ---- Config (.env) ----
if [ -f "$ENV_FILE" ]; then
  say "Found existing .env — reusing it. Delete it to reconfigure."
else
  cp "$ENV_DIST" "$ENV_FILE"
  say "Created .env from template. Let's set a few values (Enter = keep default)."

  read -r -p "Build mode [prebuilt/compile] (prebuilt): " bm; bm="${bm:-prebuilt}"
  sed -i "s|^SERVER_BUILD_TARGET=.*|SERVER_BUILD_TARGET=${bm}|" "$ENV_FILE"
  if [ "$bm" = "prebuilt" ]; then
    read -r -p "Release assets base URL (serves canary + libbot_engine.so): " ru
    [ -n "${ru:-}" ] && sed -i "s|^RELEASE_ASSETS_URL=.*|RELEASE_ASSETS_URL=${ru}|" "$ENV_FILE"
  fi

  read -r -p "Public/LAN server IP (127.0.0.1): " sip; sip="${sip:-127.0.0.1}"
  sed -i "s|^SERVER_IP=.*|SERVER_IP=${sip}|" "$ENV_FILE"

  read -r -p "Bots online 0-997 (200): " bots; bots="${bots:-200}"
  sed -i "s|^BOT_PLAYERS_ONLINE=.*|BOT_PLAYERS_ONLINE=${bots}|" "$ENV_FILE"

  read -r -p "MySQL root password: " rp; [ -n "${rp:-}" ] && sed -i "s|^MYSQL_ROOT_PASSWORD=.*|MYSQL_ROOT_PASSWORD=${rp}|" "$ENV_FILE"
  read -r -p "MySQL canary-user password: " cp; [ -n "${cp:-}" ] && sed -i "s|^MYSQL_PASSWORD=.*|MYSQL_PASSWORD=${cp}|" "$ENV_FILE"

  read -r -p "Client assets URL (Tibia.dat/.spr; blank to skip): " ca
  [ -n "${ca:-}" ] && sed -i "s|^CLIENT_ASSETS_URL=.*|CLIENT_ASSETS_URL=${ca}|" "$ENV_FILE"
fi

# ---- Build + up ----
say "Bringing the stack up (first run downloads/builds; compile mode can take 30-60 min)..."
docker compose --env-file "$ENV_FILE" -f "$COMPOSE" up -d --build

# ---- Readiness ----
say "Waiting for the server to come online (Ctrl-C to stop watching; it keeps running)..."
for i in $(seq 1 120); do
  if docker compose --env-file "$ENV_FILE" -f "$COMPOSE" logs server 2>/dev/null | grep -qiE 'bot system library loaded|server online|forgotten'; then
    say "Server is up."
    break
  fi
  sleep 5
done

# shellcheck disable=SC1090
set -a; . "$ENV_FILE"; set +a
say "== Done =="
cat <<EOF
- Game server:  ${SERVER_IP}:${GAME_PORT}
- Website:      http://${SERVER_IP}:${WEB_HTTP_PORT}
- Admin login:  account @god / password god12345   (CHANGE THIS)
- Spectate:     log in with account name '@cast' (no password) and pick a character
- Bots online:  ${BOT_PLAYERS_ONLINE}  (edit BOT_PLAYERS_ONLINE in .env, then: docker compose ... restart server)

Logs:   docker compose --env-file .env -f dadsmmolab/docker-compose.yml logs -f server
Stop:   docker compose --env-file .env -f dadsmmolab/docker-compose.yml down
Wipe:   add -v to 'down' to delete the database volume
EOF
