#!/bin/bash
# ============================================================
#  Dad's MMO Lab — Tibia (Canary Bots + Cast) Server Installer
#  Powered by Canary (OpenTibiaBR) + a bot-player & cast system
#
#  https://github.com/DadsMmoLab/dads-mmo-lab
#
#  Version: 1.0.0
#
#  Usage:
#    chmod +x install-tibia.sh
#    ./install-tibia.sh
#
#  What this does:
#    1. Installs deps (MariaDB, PHP) — runs natively, NO Docker
#    2. Downloads the prebuilt server (or compiles from source)
#    3. Initializes a private database (bots, hunts, market, houses)
#    4. Optionally sets up the MyAAC website (account creation + cast)
#    5. Sets up the Gaming Mode launcher
#
#  Powered by:
#    canary-bots-with-cast — a fork of opentibiabr/canary
#    github.com/1LineAtaTime/canary-bots-with-cast
#    (forked at upstream commit ded10949d; bot players + cast added)
#
#  This is a LOCAL/personal server: ~500 AI "bot players" populate
#  the world, you play with/against them, and you can spectate any
#  character (bots included) via the built-in cast viewer. No Docker,
#  no Proton — the server runs natively on Linux.
# ============================================================

INSTALLER_VERSION="1.0.0"

set -o pipefail

RST='\033[0m'; BOLD='\033[1m'; DIM='\033[2m'
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; WHITE='\033[1;37m'; CYAN='\033[0;36m'

TB='\033[0;36m'
TBB='\033[1;36m'

print_header() {
    clear
    echo ""
    echo -e "${TB}╔══════════════════════════════════════════════════╗${RST}"
    echo -e "${TB}║${WHITE}${BOLD}         🛡️  DAD'S MMO LAB                        ${RST}${TB}║${RST}"
    echo -e "${TB}║${WHITE}         Tibia (Canary Bots) Installer v${INSTALLER_VERSION}     ${RST}${TB}║${RST}"
    echo -e "${TB}║${BLUE}         Canary + Bot Players + Cast              ${RST}${TB}║${RST}"
    echo -e "${TB}╚══════════════════════════════════════════════════╝${RST}"
    echo ""
}

print_step() {
    echo ""
    echo -e "${TB}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
    echo -e "${WHITE}${BOLD} $1${RST}"
    echo -e "${TB}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
}

print_success() { echo -e "${GREEN}✅ $1${RST}"; }
print_warning() { echo -e "${YELLOW}⚠️  $1${RST}"; }
print_error()   { echo -e "${RED}❌ $1${RST}"; }
print_info()    { echo -e "${BLUE}ℹ️  $1${RST}"; }

ask_yes_no() {
    while true; do
        printf "${WHITE}$1 (y/n): ${RST}"
        read -r answer
        case $answer in
            [Yy]*) return 0 ;;
            [Nn]*) return 1 ;;
            *) echo "Please answer y or n." ;;
        esac
    done
}

# ─────────────────────────────────────────
# CONFIG
# ─────────────────────────────────────────
SERVER_DIR="$HOME/tibia-canary-server"
DB_DIR="$SERVER_DIR/mariadb-data"
DB_PORT="33066"
DB_NAME="canary"
DB_USER="canary"
DB_PASS="canary"
WEB_DIR="$SERVER_DIR/myaac"
WEB_PORT="80"
LOGFILE="/tmp/tibia-launch.log"

# Our public fork (forked from opentibiabr/canary at the commit below).
REPO="https://github.com/1LineAtaTime/canary-bots-with-cast"
RELEASE_TAG="v2026.06.23"
RELEASE="$REPO/releases/download/$RELEASE_TAG"
FORK_COMMIT="ded10949d5731d1a7f05de5a087aacefaa85c82c"
UPSTREAM="https://github.com/opentibiabr/canary"
# World map (downloaded once from the upstream Canary release; version pinned).
MAP_URL="https://github.com/opentibiabr/canary/releases/download/v3.1.0/otservbr.otbm"

# OTClient Redemption (the game client) + Tibia.dat/.spr asset sources (direct).
CLIENT_DIR="$SERVER_DIR/otclient"
OTCLIENT_URL="https://github.com/opentibiabr/otclient/releases/download/4.1/otclient-linux-release.zip"
DAT_SPR_URL_1="https://github.com/dudantas/tibia-client/releases/download/15.11.c9d1cf/client-11.zip"
DAT_SPR_URL_2="https://downloads.ots.me/data/tibia-clients/dat_and_spr/1501.zip"

INSTALL_WEB="yes"      # set by show_summary
BUILD_MODE="prebuilt"  # set by show_summary

# ─────────────────────────────────────────
# SYSTEM CHECK
# ─────────────────────────────────────────
check_system() {
    print_step "Checking System"
    [[ "$OSTYPE" != "linux-gnu"* ]] && { print_error "Linux required."; exit 1; }
    print_success "Linux detected"

    AVAILABLE_GB=$(df -BG "$HOME" 2>/dev/null | awk 'NR==2 {print $4}' | sed 's/G//' | tr -d ' ')
    if [ -n "$AVAILABLE_GB" ] && [ "$AVAILABLE_GB" -lt 15 ] 2>/dev/null; then
        print_error "Need at least 15GB free. Have ${AVAILABLE_GB}GB."
        exit 1
    fi
    print_success "Disk space OK (${AVAILABLE_GB:-unknown}GB available)"

    if ! ping -c 1 github.com &>/dev/null; then
        print_error "No internet connection."
        exit 1
    fi
    print_success "Internet OK"
}

# ─────────────────────────────────────────
# PACMAN HELPER (SteamOS read-only aware)
# ─────────────────────────────────────────
pac_install() {
    sudo steamos-readonly disable 2>/dev/null || true
    sudo pacman -Sy --noconfirm --needed "$@"
    sudo steamos-readonly enable 2>/dev/null || true
}

# ─────────────────────────────────────────
# SUMMARY & CHOICES
# ─────────────────────────────────────────
show_summary() {
    print_header
    print_step "STEP 1/5 — Summary"
    echo ""
    echo -e "  ${WHITE}This will install a local Tibia (Canary) server with bots into:${RST}"
    echo -e "    ${CYAN}$SERVER_DIR${RST}"
    echo ""
    echo -e "  ${WHITE}Base:${RST}   opentibiabr/canary @ ${DIM}${FORK_COMMIT:0:10}${RST} + bot players + cast"
    echo -e "  ${WHITE}DB:${RST}     private MariaDB on 127.0.0.1:${DB_PORT} (no system DB touched)"
    echo ""
    echo -e "  ${WHITE}How do you want to install the server?${RST}"
    echo -e "    ${WHITE}1)${RST} Prebuilt release binaries — fast (recommended)"
    echo -e "    ${WHITE}2)${RST} Compile: official Canary @ ${DIM}${FORK_COMMIT:0:10}${RST} + our patch (~30-60 min)"
    echo -e "    ${WHITE}3)${RST} Compile: directly from our public fork (~30-60 min)"
    printf "${WHITE}Choice [1/2/3] (1): ${RST}"; read -r sm
    case "$sm" in
        2) BUILD_MODE="patch" ;;
        3) BUILD_MODE="fork" ;;
        *) BUILD_MODE="prebuilt" ;;
    esac
    echo ""
    if ask_yes_no "Install the MyAAC website too (account creation + web)?"; then
        INSTALL_WEB="yes"
    else
        INSTALL_WEB="no"
    fi
    echo ""
    print_info "Build mode: $BUILD_MODE   |   Website: $INSTALL_WEB"
    if ! ask_yes_no "Proceed?"; then
        print_warning "Cancelled."; exit 0
    fi
}

# ─────────────────────────────────────────
# DEPENDENCIES
# ─────────────────────────────────────────
install_deps() {
    print_step "STEP 2/5 — Installing Dependencies"
    local pkgs=(mariadb wget unzip)
    [ "$INSTALL_WEB" = "yes" ] && pkgs+=(php php-gd composer)
    [ "$BUILD_MODE" != "prebuilt" ] && pkgs+=(git cmake ninja base-devel)
    print_info "Installing: ${pkgs[*]}"
    pac_install "${pkgs[@]}" || { print_error "Dependency install failed."; exit 1; }
    print_success "Dependencies installed"
}

# Build canary from a prepared source tree ($1) with vcpkg, then place the
# binaries + datapack/schema/dumps/config into the server dir.
build_from_src() {
    print_warning "Building from source — this can take 30-60 minutes."
    print_info "Requires the vcpkg toolchain (see the Canary build docs / VCPKG_ROOT)."
    ( cd "$1" && cmake --preset linux-release -DTOGGLE_BIN_FOLDER=ON && cmake --build build/linux-release -j"$(nproc)" ) \
        || { print_error "build failed — see the Canary build docs for prerequisites"; exit 1; }
    cp "$1/build/linux-release/bin/canary"           "$SERVER_DIR/canary"
    cp "$1/build/linux-release/bin/libbot_engine.so" "$SERVER_DIR/libbot_engine.so"
    cp -rn "$1/." "$SERVER_DIR/"   # datapack, schema.sql, database/bots, config.lua.dist
    chmod +x "$SERVER_DIR/canary"
}

# ─────────────────────────────────────────
# SERVER — three install methods (chosen in the summary):
#   prebuilt : download the compiled release binaries + runtime data (fast)
#   patch    : official Canary @ fork commit + our patch, then compile
#   fork     : clone our public fork, then compile
# ─────────────────────────────────────────
install_server() {
    print_step "STEP 3/5 — Installing the Server"
    mkdir -p "$SERVER_DIR"

    case "$BUILD_MODE" in
        prebuilt)
            print_info "Downloading prebuilt binaries + runtime data from the release..."
            wget -q --show-progress -O "$SERVER_DIR/canary"           "$RELEASE/canary"           || { print_error "download failed"; exit 1; }
            wget -q --show-progress -O "$SERVER_DIR/libbot_engine.so" "$RELEASE/libbot_engine.so" || { print_error "download failed"; exit 1; }
            wget -q --show-progress -O "$SERVER_DIR/runtime.tar.gz"   "$RELEASE/canary-bots-runtime-$RELEASE_TAG.tar.gz" || { print_error "download failed"; exit 1; }
            tar -xzf "$SERVER_DIR/runtime.tar.gz" -C "$SERVER_DIR" && rm -f "$SERVER_DIR/runtime.tar.gz"
            chmod +x "$SERVER_DIR/canary"
            print_success "Prebuilt server installed"
            ;;
        patch)
            # Legitimacy path: official Canary at the exact fork commit + our patch.
            print_info "Cloning official Canary @ ${FORK_COMMIT:0:10} ..."
            git clone "$UPSTREAM.git" "$SERVER_DIR/src" || { print_error "clone failed"; exit 1; }
            git -C "$SERVER_DIR/src" checkout "$FORK_COMMIT" || { print_error "checkout failed"; exit 1; }
            print_info "Applying the canary-bots patch..."
            wget -q -O "$SERVER_DIR/canary-bots.patch" "$RELEASE/canary-bots-$RELEASE_TAG.patch" || { print_error "patch download failed"; exit 1; }
            git -C "$SERVER_DIR/src" -c core.autocrlf=false apply "$SERVER_DIR/canary-bots.patch" || { print_error "patch failed to apply"; exit 1; }
            build_from_src "$SERVER_DIR/src"
            print_success "Built from official Canary + patch"
            ;;
        fork)
            # Compile straight from our public fork (pinned to the release tag).
            print_info "Cloning our public fork ($RELEASE_TAG)..."
            git clone --depth 1 --branch "$RELEASE_TAG" "$REPO.git" "$SERVER_DIR/src" || { print_error "clone failed"; exit 1; }
            build_from_src "$SERVER_DIR/src"
            print_success "Built from the public fork"
            ;;
    esac

    # Map (downloaded once; the binary needs it).
    if [ ! -f "$SERVER_DIR/data-otservbr-global/world/otservbr.otbm" ]; then
        print_info "Downloading world map..."
        wget -q --show-progress -O "$SERVER_DIR/data-otservbr-global/world/otservbr.otbm" "$MAP_URL" || print_warning "map download failed — place otservbr.otbm manually"
    fi

    # config.lua from the shipped template.
    [ -f "$SERVER_DIR/config.lua" ] || cp "$SERVER_DIR/config.lua.dist" "$SERVER_DIR/config.lua"
    sed -i "s|^mysqlHost = .*|mysqlHost = \"127.0.0.1\"|"   "$SERVER_DIR/config.lua"
    sed -i "s|^mysqlPort = .*|mysqlPort = $DB_PORT|"        "$SERVER_DIR/config.lua"
    sed -i "s|^mysqlUser = .*|mysqlUser = \"$DB_USER\"|"    "$SERVER_DIR/config.lua"
    sed -i "s|^mysqlPass = .*|mysqlPass = \"$DB_PASS\"|"    "$SERVER_DIR/config.lua"
    sed -i "s|^mysqlDatabase = .*|mysqlDatabase = \"$DB_NAME\"|" "$SERVER_DIR/config.lua"
    sed -i "s|^ip = .*|ip = \"127.0.0.1\"|"                "$SERVER_DIR/config.lua"
    print_success "config.lua written"
}

# ─────────────────────────────────────────
# DATABASE (private MariaDB datadir on 127.0.0.1:33066)
# ─────────────────────────────────────────
db_start() {
    [ -d "$DB_DIR/mysql" ] || mariadb-install-db --datadir="$DB_DIR" --auth-root-authentication-method=normal >/dev/null 2>&1
    mariadbd --datadir="$DB_DIR" --port="$DB_PORT" --socket="$DB_DIR/mysql.sock" \
             --skip-grant-tables --pid-file="$DB_DIR/mysqld.pid" >/dev/null 2>&1 &
    for _ in $(seq 1 30); do
        mysqladmin --socket="$DB_DIR/mysql.sock" ping >/dev/null 2>&1 && return 0
        sleep 1
    done
    return 1
}
db_stop() {
    mysqladmin --socket="$DB_DIR/mysql.sock" shutdown 2>/dev/null || \
        { [ -f "$DB_DIR/mysqld.pid" ] && kill "$(cat "$DB_DIR/mysqld.pid")" 2>/dev/null; }
}

init_database() {
    print_step "STEP 4/5 — Initializing the Database"
    mkdir -p "$DB_DIR"
    db_start || { print_error "MariaDB failed to start"; exit 1; }

    local m="mysql --socket=$DB_DIR/mysql.sock"
    if $m -e "USE \`$DB_NAME\`; SHOW TABLES LIKE 'server_config';" 2>/dev/null | grep -q server_config; then
        print_info "Database already initialized — skipping import."
    else
        print_info "Creating database + user..."
        $m -e "DROP DATABASE IF EXISTS \`$DB_NAME\`; CREATE DATABASE \`$DB_NAME\` DEFAULT CHARSET=utf8mb3;"
        $m -e "CREATE USER IF NOT EXISTS '$DB_USER'@'127.0.0.1' IDENTIFIED BY '$DB_PASS'; GRANT ALL ON \`$DB_NAME\`.* TO '$DB_USER'@'127.0.0.1'; FLUSH PRIVILEGES;"
        print_info "Importing schema + bot data (this takes a moment)..."
        $m "$DB_NAME" < "$SERVER_DIR/schema.sql"
        for f in "$SERVER_DIR"/database/bots/00_bot_schema.sql \
                 "$SERVER_DIR"/database/bots/0[1-9]_*.sql \
                 "$SERVER_DIR"/database/bots/1[0-3]_*.sql; do
            [ -f "$f" ] && $m "$DB_NAME" < "$f"
        done
        $m "$DB_NAME" -e "ALTER TABLE accounts AUTO_INCREMENT=65001; ALTER TABLE players AUTO_INCREMENT=66100;"
        print_success "Database ready (admin login: @god / god12345)"
    fi
    db_stop
}

# ─────────────────────────────────────────
# WEBSITE (MyAAC via PHP's built-in server)
# ─────────────────────────────────────────
setup_web() {
    [ "$INSTALL_WEB" = "yes" ] || { print_info "Skipping website."; return 0; }
    print_step "Setting Up the Website (MyAAC)"
    if [ ! -d "$WEB_DIR" ]; then
        git clone --depth 1 https://github.com/slawkens/myaac.git "$WEB_DIR" || { print_warning "MyAAC clone failed — skipping website"; INSTALL_WEB="no"; return 0; }
        ( cd "$WEB_DIR" && composer install --no-dev --no-interaction --optimize-autoloader ) || print_warning "composer install had issues"
    fi
    # Drop in our cast-aware login.php (intercepts @cast).
    [ -f "$SERVER_DIR/deployment/web/login.php" ] && cp "$SERVER_DIR/deployment/web/login.php" "$WEB_DIR/login.php"
    cat > "$WEB_DIR/config.local.php" <<PHP
<?php
\$config['installed'] = true;
\$config['server_path'] = '$SERVER_DIR/';
\$config['database_host'] = '127.0.0.1';
\$config['database_port'] = $DB_PORT;
\$config['database_user'] = '$DB_USER';
\$config['database_password'] = '$DB_PASS';
\$config['database_name'] = '$DB_NAME';
\$config['database_auto_migrate'] = true;
\$config['sql_port'] = $DB_PORT;
PHP
    print_success "Website ready (served at http://127.0.0.1:$WEB_PORT when running)"
}

# ─────────────────────────────────────────
# CLIENT (OTClient Redemption) + Tibia.dat/.spr assets + init.lua
# ─────────────────────────────────────────
setup_client() {
    print_step "Installing the Game Client (OTClient Redemption)"

    # 1) OTClient Redemption
    if [ ! -e "$CLIENT_DIR/otclient" ]; then
        print_info "Downloading OTClient Redemption..."
        wget -q --show-progress -O /tmp/otclient.zip "$OTCLIENT_URL" \
            && mkdir -p "$CLIENT_DIR" && unzip -oq /tmp/otclient.zip -d "$CLIENT_DIR" && rm -f /tmp/otclient.zip \
            || print_warning "OTClient download failed — install it manually from github.com/opentibiabr/otclient"
        chmod +x "$CLIENT_DIR/otclient" 2>/dev/null || true
    fi

    # 2) Tibia.dat / Tibia.spr (protocol assets) into data/things/1100/
    local things="$CLIENT_DIR/data/things/1100"
    if [ ! -f "$things/Tibia.dat" ]; then
        echo -e "  Pick a source for ${CYAN}Tibia.dat / Tibia.spr${RST}:"
        echo -e "    ${WHITE}1)${RST} dudantas tibia-client (client-11.zip)"
        echo -e "    ${WHITE}2)${RST} downloads.ots.me (1501.zip)"
        echo -e "    ${WHITE}3)${RST} Skip"
        printf "${WHITE}Choice [1/2/3]: ${RST}"; read -r a
        local url=""
        case "$a" in 1) url="$DAT_SPR_URL_1" ;; 2) url="$DAT_SPR_URL_2" ;; *) print_info "Skipped — place Tibia.dat/.spr in $things yourself."; return 0 ;; esac
        print_info "Downloading assets..."
        if wget -q --show-progress -O /tmp/datspr.zip "$url"; then
            mkdir -p "$things" /tmp/datspr && unzip -oq /tmp/datspr.zip -d /tmp/datspr
            # Copy the Tibia.dat/.spr out of the archive (it may be nested).
            find /tmp/datspr -iname 'Tibia.dat' -exec cp {} "$things/Tibia.dat" \; 2>/dev/null
            find /tmp/datspr -iname 'Tibia.spr' -exec cp {} "$things/Tibia.spr" \; 2>/dev/null
            rm -rf /tmp/datspr /tmp/datspr.zip
            [ -f "$things/Tibia.dat" ] && print_success "Assets placed in $things" || print_warning "Tibia.dat/.spr not found in the archive — place them manually in $things"
        else
            print_warning "Asset download failed — place Tibia.dat/.spr in $things manually."
        fi
    fi

    # 3) init.lua — point the client at the local server.
    if [ -d "$CLIENT_DIR" ]; then
        if [ "$INSTALL_WEB" = "yes" ]; then
            # httpLogin via the local MyAAC login.php (matches the standard setup).
            cat > "$CLIENT_DIR/init.lua" <<'INITLUA'
-- Dad's MMO Lab — Tibia (Canary Bots) client config (local server)
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
INITLUA
        else
            # No website: direct login to the server's login port.
            cat > "$CLIENT_DIR/init.lua" <<'INITLUA'
-- Dad's MMO Lab — Tibia (Canary Bots) client config (local server, direct login)
Servers_init = {
    ["Local"] = {
        ["host"] = "127.0.0.1",
        ["port"] = 7171,
        ["protocol"] = 1500,
        ["httpLogin"] = false
    }
}
INITLUA
        fi
        print_success "Client configured for 127.0.0.1"
    fi
}

# ─────────────────────────────────────────
# LAUNCHER
# ─────────────────────────────────────────
setup_launcher() {
    print_step "STEP 5/5 — Setting Up the Gaming Mode Launcher"
    cat > "$HOME/tibia-canary-launcher.sh" << LAUNCHER
#!/bin/bash
# Dad's MMO Lab — Tibia (Canary Bots) Launcher v${INSTALLER_VERSION}
export PATH="/usr/bin:/usr/local/bin:/bin:\$PATH"
unset LD_PRELOAD LD_LIBRARY_PATH
SERVER_DIR="${SERVER_DIR}"
DB_DIR="${DB_DIR}"
WEB_DIR="${WEB_DIR}"
CLIENT_DIR="${CLIENT_DIR}"
INSTALL_WEB="${INSTALL_WEB}"
LOGFILE="${LOGFILE}"
> "\$LOGFILE"

# Ordered teardown on exit: client, then web, then the game server (lets
# bots/players save), then the private database LAST (server writes through it).
cleanup() {
    [ -n "\$CLIENT_PID" ] && kill "\$CLIENT_PID" 2>/dev/null
    [ -n "\$WEB_PID" ] && kill "\$WEB_PID" 2>/dev/null
    [ -n "\$CANARY_PID" ] && kill "\$CANARY_PID" 2>/dev/null
    sleep 3
    mysqladmin --socket="\$DB_DIR/mysql.sock" shutdown 2>/dev/null || \
        { [ -f "\$DB_DIR/mysqld.pid" ] && kill "\$(cat "\$DB_DIR/mysqld.pid")" 2>/dev/null; }
}
trap cleanup EXIT INT TERM

echo "Starting private database..."
mariadbd --datadir="\$DB_DIR" --port=${DB_PORT} --socket="\$DB_DIR/mysql.sock" --pid-file="\$DB_DIR/mysqld.pid" >>"\$LOGFILE" 2>&1 &
for _ in \$(seq 1 30); do mysqladmin --socket="\$DB_DIR/mysql.sock" ping >/dev/null 2>&1 && break; sleep 1; done

if [ "\$INSTALL_WEB" = "yes" ] && [ -d "\$WEB_DIR" ]; then
    echo "Starting website on http://127.0.0.1:${WEB_PORT} ..."
    ( cd "\$WEB_DIR" && php -S 0.0.0.0:${WEB_PORT} router.php >>"\$LOGFILE" 2>&1 ) &
    WEB_PID=\$!
fi

echo "Starting Tibia server..."
cd "\$SERVER_DIR" || exit 1
./canary >>"\$LOGFILE" 2>&1 &
CANARY_PID=\$!

echo "Waiting for the server to come online..."
for _ in \$(seq 1 120); do
    grep -q "bot system library loaded" "\$LOGFILE" 2>/dev/null && break
    sleep 2
done
echo ""
echo "  Tibia server is up. Admin login: @god / god12345 (change it)."
echo "  Spectate bots: log in as @cast (no password)."

if [ -x "\$CLIENT_DIR/otclient" ]; then
    echo "  Launching the game client..."
    ( cd "\$CLIENT_DIR" && ./otclient >>"\$LOGFILE" 2>&1 ) &
    CLIENT_PID=\$!
    wait \$CLIENT_PID   # close the client -> the trap stops the server + db
else
    echo "  No client installed here. Connect with OTClient to 127.0.0.1."
    echo "  Close this window to stop the server."
    wait \$CANARY_PID
fi
LAUNCHER
    chmod +x "$HOME/tibia-canary-launcher.sh"
    print_success "Launcher created: ~/tibia-canary-launcher.sh"
}

# ─────────────────────────────────────────
# COMPLETION
# ─────────────────────────────────────────
show_completion() {
    echo ""
    echo -e "${TBB}╔══════════════════════════════════════════════════╗${RST}"
    echo -e "${TBB}║   🛡️  YOUR TIBIA SERVER IS READY!                ║${RST}"
    echo -e "${TBB}╚══════════════════════════════════════════════════╝${RST}"
    echo ""
    echo -e "  ${WHITE}${BOLD}Server:${RST}   ${TB}Canary + ~500 bot players + cast${RST}"
    echo -e "  ${WHITE}${BOLD}Folder:${RST}   ${TB}$SERVER_DIR${RST}"
    echo -e "  ${WHITE}${BOLD}Launcher:${RST} ${TB}~/tibia-canary-launcher.sh${RST}"
    echo -e "  ${WHITE}${BOLD}Admin:${RST}    ${TB}@god / god12345${RST}  (change it!)"
    echo ""
    echo -e "${TB}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
    echo -e "${WHITE}${BOLD} STEP A — Get a Client${RST}"
    echo -e "${TB}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
    echo -e "  OTClient Redemption was installed + configured for ${GREEN}127.0.0.1${RST}."
    echo -e "  Client folder: ${CYAN}$CLIENT_DIR${RST}"
    echo -e "  Log in with ${GREEN}@god / god12345${RST} (admin), or ${GREEN}@cast${RST} to spectate bots."
    [ "$INSTALL_WEB" = "yes" ] && echo -e "  Register normal accounts on the website at ${GREEN}http://127.0.0.1${RST}."
    echo ""
    echo -e "${TB}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
    echo -e "${WHITE}${BOLD} STEP B — Add to Steam (Gaming Mode)${RST}"
    echo -e "${TB}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
    echo -e "  1. Open Steam in Desktop Mode"
    echo -e "  2. Click ${CYAN}Games${RST} → ${CYAN}Add a Non-Steam Game${RST}"
    echo -e "  3. Browse to ${CYAN}/usr/bin/${RST} → select ${CYAN}konsole${RST}"
    echo -e "  4. Right-click → Properties → rename: ${GREEN}Tibia Server${RST}"
    echo -e "  5. Set Launch Options to:"
    echo ""
    echo -e "  ${GREEN}--hold -e bash ~/tibia-canary-launcher.sh${RST}"
    echo ""
    echo -e "  6. ${RED}Do NOT enable Proton${RST} — it runs natively!"
    echo ""
    [ "$INSTALL_WEB" = "yes" ] && echo -e "  Website (account creation): ${GREEN}http://127.0.0.1:$WEB_PORT${RST} (while running)"
    echo ""
    echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
    echo -e "${WHITE}  📦 github.com/1LineAtaTime/canary-bots-with-cast${RST}"
    echo -e "${WHITE}  📺 youtube.com/@DadsMmoLab${RST}"
    echo -e "${WHITE}  📦 github.com/DadsMmoLab/dads-mmo-lab${RST}"
    echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
    echo ""

    echo -e "${WHITE}Launch the Tibia server now to test it? (y/n): ${RST}"
    read -r launch_now
    if [[ "$launch_now" =~ ^[Yy]$ ]]; then
        print_info "Launching..."
        bash "$HOME/tibia-canary-launcher.sh"
    fi
}

print_header
check_system
show_summary
install_deps
install_server
init_database
setup_web
setup_client
setup_launcher
show_completion
