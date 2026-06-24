#!/usr/bin/env bash
# Generate MyAAC config.local.php from env, then run nginx + php-fpm.
set -euo pipefail

DB_HOST="${MYSQL_HOST:-database}"; DB_PORT="${MYSQL_PORT:-3306}"
DB_USER="${MYSQL_USER:-canary}"; DB_PASS="${MYSQL_PASSWORD:-canary}"
DB_NAME="${MYSQL_DATABASE:-canary}"
SERVER_NAME="${SERVER_NAME:-Canary Bots}"

CFG=/var/www/html/config.local.php
if [ ! -f "$CFG" ]; then
  cat > "$CFG" <<PHP
<?php
\$config['installed'] = true;
\$config['server_path'] = '/srv/canary/';
\$config['database_host'] = '${DB_HOST}';
\$config['database_port'] = ${DB_PORT};
\$config['database_user'] = '${DB_USER}';
\$config['database_password'] = '${DB_PASS}';
\$config['database_name'] = '${DB_NAME}';
\$config['database_socket'] = '';
\$config['database_auto_migrate'] = true;
\$config['database_overwrite'] = false;
\$config['server_name'] = '${SERVER_NAME}';
PHP
  chown www-data:www-data "$CFG"
fi

# Optional: fetch client assets (Tibia.dat/.spr or a packaged client) and serve
# them for players to download. CLIENT_ASSETS_URL is user-supplied (client
# assets are not bundled — see DOCKER-HOWTO.md for where to obtain them).
if [ -n "${CLIENT_ASSETS_URL:-}" ]; then
  mkdir -p /var/www/html/client
  echo "Fetching client assets from CLIENT_ASSETS_URL ..."
  if ! wget -q -O /var/www/html/client/assets.bin "${CLIENT_ASSETS_URL}"; then
    echo "WARN: client asset download failed; players can place Tibia.dat/.spr manually."
  fi
  chown -R www-data:www-data /var/www/html/client || true
fi

exec /usr/bin/supervisord -c /etc/supervisor/conf.d/web.conf
