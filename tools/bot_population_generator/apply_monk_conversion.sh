#!/usr/bin/env bash
# ============================================================================
# apply_monk_conversion.sh — #197: turn ~20 % of the bot roster into monks, IN PLACE.
#
#   DB_HOST=.. DB_PORT=.. DB_USER=.. DB_PASS=.. DB_NAME=canary_1525_port \
#     ./apply_monk_conversion.sh apply            # the real thing (canary STOPPED)
#   ... ./apply_monk_conversion.sh dryrun [SCRATCH_DB]  # copy of the bot rows into SCRATCH_DB
#                                                      # (default <DB_NAME>_monkdry), convert,
#                                                      # verify, roll back, verify, drop it
#   ... ./apply_monk_conversion.sh rollback BEFORE_SQL # restore a backup's .before.sql (STOPPED)
#
# Order (90_monk_support.md §6.3): the gear rows, rotation and potions (feat/monk-support) must be
# LIVE first, or a converted bot logs in naked. players-row edits need canary STOPPED: every save
# rewrites the full row from memory, so a running server silently reverts this
# (memory: project_bot_rename_997).
#
# Writes into OUT_DIR (default ./monk_conversion_<ts>/): the mysqldump of every bot row, the
# .before.sql regenerated from the LIVE rows (exact rollback), and the verification output.
# ============================================================================
set -euo pipefail

MODE="${1:-}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SQL="$DIR/../../database/bots/26_monk_conversion.sql"
IDS="$DIR/../../database/bots/26_monk_conversion.ids.txt"
DB_HOST="${DB_HOST:-127.0.0.1}"; DB_PORT="${DB_PORT:-3306}"; DB_USER="${DB_USER:-root}"
DB_PASS="${DB_PASS:-}"; DB_NAME="${DB_NAME:-canary}"
ACC=65000
TS="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${OUT_DIR:-./monk_conversion_${TS}}"
mkdir -p "$OUT_DIR"

my() { mysql -h "$DB_HOST" -P "$DB_PORT" -u "$DB_USER" ${DB_PASS:+-p"$DB_PASS"} "$@" 2> >(grep -v "Using a password" >&2); }
dump() { mysqldump -h "$DB_HOST" -P "$DB_PORT" -u "$DB_USER" ${DB_PASS:+-p"$DB_PASS"} "$@" 2> >(grep -v "Using a password" >&2); }

ID_LIST="$(cut -f1 "$IDS" | grep -E '^[0-9]+$' | paste -sd, -)"
N_IDS="$(cut -f1 "$IDS" | grep -cE '^[0-9]+$')"
[ "$N_IDS" -gt 0 ] || { echo "no ids in $IDS"; exit 2; }

# .before.sql from the LIVE rows of db $1 (exact rollback, whatever changed since the snapshot)
make_before() {
	local db="$1" out="$2"
	{
		echo "-- #197 monk conversion rollback, generated $(date -Is) from $db (live rows)"
		echo "START TRANSACTION;"
		my -N -B "$db" -e "SELECT CONCAT('UPDATE \`players\` SET \`vocation\`=',vocation,', \`health\`=',health,', \`healthmax\`=',healthmax,', \`mana\`=',mana,', \`manamax\`=',manamax,', \`cap\`=',cap,', \`looktype\`=',looktype,' WHERE \`id\`=',id,' AND \`account_id\`=$ACC;') FROM players WHERE account_id=$ACC AND id IN ($ID_LIST) ORDER BY id"
		echo "COMMIT;"
	} > "$out"
	local n; n="$(grep -c '^UPDATE' "$out")"
	[ "$n" -eq "$N_IDS" ] || { echo "before.sql has $n rows, expected $N_IDS"; exit 3; }
}

# players_online is a MEMORY table that Game::updatePlayersOnline never prunes of bot rows (the
# cleanup DELETE filters account 65000 out on purpose), so a STOPPED server still lists every bot
# that was online at shutdown and the guard below would refuse forever. With canary verifiably
# stopped (systemctl is-active canary = inactive, no canary process), set CANARY_STOPPED_VERIFIED=1:
# the stale bot rows are cleared (the next boot re-inserts the online set) and the run proceeds.
guard_stopped() {
	local online
	online="$(my -N -B "$DB_NAME" -e "SELECT COUNT(*) FROM players_online po JOIN players p ON p.id=po.player_id WHERE p.account_id=$ACC")"
	if [ "$online" -ne 0 ] && [ "${CANARY_STOPPED_VERIFIED:-0}" = 1 ]; then
		my "$DB_NAME" -e "DELETE po FROM players_online po JOIN players p ON p.id=po.player_id WHERE p.account_id=$ACC"
		echo "cleared $online stale bot rows from players_online (CANARY_STOPPED_VERIFIED=1)"
		online=0
	fi
	[ "$online" -eq 0 ] || { echo "REFUSING: $online bots in players_online — stop canary first (bot rows outlive a stop: verify it is stopped, then CANARY_STOPPED_VERIFIED=1)"; exit 4; }
}

verify() {
	local db="$1"
	my -t "$db" -e "SELECT vocation, COUNT(*) n FROM players WHERE account_id=$ACC GROUP BY vocation ORDER BY vocation;
		SELECT COUNT(*) AS monks, SUM(healthmax <> 185 + (level-1)*10 OR manamax <> 90 + (level-1)*10
		  OR cap <> 470 + (level-1)*25 OR vocation <> IF(level >= 20, 10, 9)) AS bad_monk_rows
		FROM players WHERE account_id=$ACC AND vocation IN (9,10);"
}

case "$MODE" in
apply)
	guard_stopped
	pre="$(my -N -B "$DB_NAME" -e "SELECT COUNT(*) FROM players WHERE account_id=$ACC AND vocation IN (9,10)")"
	[ "$pre" -eq 0 ] || echo "NOTE: $pre monks already present (the conversion is idempotent; re-applying)"
	dump --single-transaction --complete-insert --skip-triggers --default-character-set=utf8mb3 \
		"$DB_NAME" players --where="account_id=$ACC" > "$OUT_DIR/backup_players_pre_monk.sql"
	make_before "$DB_NAME" "$OUT_DIR/26_monk_conversion.before.live.sql"
	echo "backup + rollback in $OUT_DIR"
	my "$DB_NAME" < "$SQL"
	verify "$DB_NAME" | tee "$OUT_DIR/verify_after.txt"
	;;
rollback)
	BEFORE="${2:?rollback needs the .before.sql path}"
	guard_stopped
	my "$DB_NAME" < "$BEFORE"
	verify "$DB_NAME"
	;;
dryrun)
	SCRATCH="${2:-${DB_NAME}_monkdry}"
	case "$SCRATCH" in *_monkdry) ;; *) echo "scratch db must end in _monkdry (safety)"; exit 5;; esac
	[ "$SCRATCH" != "$DB_NAME" ] || { echo "scratch == live db"; exit 5; }
	my -e "DROP DATABASE IF EXISTS \`$SCRATCH\`; CREATE DATABASE \`$SCRATCH\`;
		CREATE TABLE \`$SCRATCH\`.players LIKE \`$DB_NAME\`.players;
		INSERT INTO \`$SCRATCH\`.players SELECT * FROM \`$DB_NAME\`.players WHERE account_id=$ACC;"
	echo "== copied $(my -N -B "$SCRATCH" -e "SELECT COUNT(*) FROM players") bot rows into $SCRATCH"
	my -N -B "$SCRATCH" -e "SELECT id,vocation,health,healthmax,mana,manamax,cap,looktype,level,experience,name FROM players ORDER BY id" > "$OUT_DIR/scratch_before.tsv"
	make_before "$SCRATCH" "$OUT_DIR/26_monk_conversion.before.live.sql"
	echo "== BEFORE"; verify "$SCRATCH"
	my "$SCRATCH" < "$SQL"
	echo "== AFTER conversion"; verify "$SCRATCH" | tee "$OUT_DIR/verify_after.txt"
	# Untouched columns / rows must be untouched.
	my -N -B "$SCRATCH" -e "SELECT id,level,experience,name FROM players ORDER BY id" > "$OUT_DIR/scratch_after_keys.tsv"
	cut -f1,9,10,11 "$OUT_DIR/scratch_before.tsv" | diff - "$OUT_DIR/scratch_after_keys.tsv" >/dev/null \
		&& echo "== ids/levels/experience/names unchanged: OK" || { echo "== KEY COLUMNS CHANGED"; exit 6; }
	changed="$(my -N -B "$SCRATCH" -e "SELECT id,vocation,health,healthmax,mana,manamax,cap,looktype,level,experience,name FROM players ORDER BY id" \
		| diff "$OUT_DIR/scratch_before.tsv" - | grep -c '^>' || true)"
	echo "== rows changed: $changed (expected $N_IDS)"
	[ "$changed" -eq "$N_IDS" ] || { echo "row count mismatch"; exit 7; }
	my "$SCRATCH" < "$SQL"
	again="$(my -N -B "$SCRATCH" -e "SELECT id,vocation,health,healthmax,mana,manamax,cap,looktype,level,experience,name FROM players ORDER BY id" | md5sum)"
	echo "== idempotent re-apply: $(my -N -B "$SCRATCH" -e "SELECT COUNT(*) FROM players WHERE vocation IN (9,10)") monks"
	my "$SCRATCH" < "$OUT_DIR/26_monk_conversion.before.live.sql"
	rolled="$(my -N -B "$SCRATCH" -e "SELECT id,vocation,health,healthmax,mana,manamax,cap,looktype,level,experience,name FROM players ORDER BY id" | diff "$OUT_DIR/scratch_before.tsv" - | grep -c '^[<>]' || true)"
	echo "== ROLLBACK leaves $rolled differing lines vs the copy (expected 0)"
	[ "$rolled" -eq 0 ] || exit 8
	my -e "DROP DATABASE \`$SCRATCH\`;"
	echo "== dropped $SCRATCH. DRY RUN OK"
	;;
*)
	echo "usage: $0 apply | dryrun [SCRATCH_DB] | rollback BEFORE_SQL"; exit 1;;
esac
