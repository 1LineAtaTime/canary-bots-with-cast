#!/bin/bash
# ============================================================
#  Dad's MMO Lab — Tibia (Canary Bots) Uninstaller
#  https://github.com/DadsMmoLab/dads-mmo-lab
#
#  Version: 1.0.0
#
#  Removes the local server, its private database, and the
#  launcher. Leaves system packages (mariadb, php) installed —
#  they're shared and safe to keep.
# ============================================================

INSTALLER_VERSION="1.0.0"
set -o pipefail

RST='\033[0m'; BOLD='\033[1m'
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; WHITE='\033[1;37m'; TB='\033[0;36m'

print_step() {
    echo ""
    echo -e "${TB}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
    echo -e "${WHITE}${BOLD} $1${RST}"
    echo -e "${TB}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RST}"
}
print_success() { echo -e "${GREEN}✅ $1${RST}"; }
print_info()    { echo -e "${BLUE}ℹ️  $1${RST}"; }

ask_yes_no() {
    while true; do
        printf "${WHITE}$1 (y/n): ${RST}"
        read -r answer
        case $answer in [Yy]*) return 0 ;; [Nn]*) return 1 ;; *) echo "Please answer y or n." ;; esac
    done
}

SERVER_DIR="$HOME/tibia-canary-server"
DB_DIR="$SERVER_DIR/mariadb-data"
LAUNCHER="$HOME/tibia-canary-launcher.sh"

clear
print_step "Tibia (Canary Bots) Uninstaller v${INSTALLER_VERSION}"
echo ""
echo -e "  This will permanently delete:"
echo -e "    - ${SERVER_DIR}  (server, datapack, private database, website)"
echo -e "    - ${LAUNCHER}"
echo ""
echo -e "${YELLOW}  Your characters/accounts live in the private database under that"
echo -e "  folder and will be GONE. System packages (mariadb, php) are kept.${RST}"
echo ""
ask_yes_no "Are you sure you want to uninstall?" || { print_info "Cancelled."; exit 0; }

print_step "Stopping the server"
# Stop the private MariaDB instance (only ours — matched by its pid file).
if [ -f "$DB_DIR/mysqld.pid" ]; then
    mysqladmin --socket="$DB_DIR/mysql.sock" shutdown 2>/dev/null || kill "$(cat "$DB_DIR/mysqld.pid")" 2>/dev/null || true
    sleep 2
fi
# Stop any running canary / launcher php -S that we started.
pkill -f "$SERVER_DIR/canary" 2>/dev/null || true
pkill -f "php -S 0.0.0.0" 2>/dev/null || true
print_success "Stopped"

print_step "Removing files"
rm -rf "$SERVER_DIR"
rm -f "$LAUNCHER"
print_success "Removed $SERVER_DIR and the launcher"

echo ""
print_info "Done. To also remove the shared packages (only if no other game needs them):"
echo -e "    sudo steamos-readonly disable && sudo pacman -R mariadb php php-gd composer && sudo steamos-readonly enable"
echo ""
