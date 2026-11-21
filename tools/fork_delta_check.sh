#!/usr/bin/env bash
# fork_delta_check.sh: durability guard for the bot fork. Run after every `git merge upstream/...`.
# A non-zero exit means a rule was violated.
#
# Rule 1 (blocking): no fork-owned file under data-otservbr-global/migrations/.
#   The bot schema is out of the numbered chain (database/bots/*.sql, #150), so `db_version`
#   tracks upstream only. A fork-owned numbered migration (our old 62.lua) sets db_version past
#   upstream's head, and the stock runner then never applies upstream's next migrations
#   (it runs a file only when fileVersion > db_version). Put bot DDL in database/bots/ instead.
#
# Rule 2 (blocking): every upstream file the fork modifies has a row in FORK_DELTA.md.
#   "Modified" = present at the merge-base with the upstream ref and different at HEAD. The row is
#   the file's path in backticks. A file with no row means the merge worklist was incomplete: the
#   next sync would resolve it blind.
#
# usage: tools/fork_delta_check.sh [upstream-ref]     (default: upstream/main)
set -euo pipefail

UPSTREAM_REF="${1:-upstream/main}"
fail=0

if ! git rev-parse --verify --quiet "${UPSTREAM_REF}^{commit}" >/dev/null; then
  echo "fork_delta_check: unknown ref '${UPSTREAM_REF}' (git fetch upstream first)" >&2
  exit 2
fi

echo "== fork_delta_check against ${UPSTREAM_REF} =="

echo "-- Rule 1: no fork-owned migrations --"
while IFS= read -r f; do
  [ -z "$f" ] && continue
  if ! git cat-file -e "${UPSTREAM_REF}:${f}" 2>/dev/null; then
    echo "  VIOLATION: $f does not exist in ${UPSTREAM_REF}: a fork-owned numbered migration."
    echo "             Move its DDL to database/bots/*.sql (idempotent CREATE ... IF NOT EXISTS)."
    fail=1
  fi
done < <(git ls-files 'data-otservbr-global/migrations/*.lua')
[ "$fail" -eq 0 ] && echo "  OK: every migration file is upstream-owned."

echo "-- Rule 2: every fork-modified upstream file has a FORK_DELTA.md row --"
MB="$(git merge-base HEAD "${UPSTREAM_REF}" 2>/dev/null || true)"
if [ -z "$MB" ]; then
  echo "  VIOLATION: no merge-base between HEAD and ${UPSTREAM_REF}."
  fail=1
elif [ ! -f FORK_DELTA.md ]; then
  echo "  VIOLATION: FORK_DELTA.md is missing."
  fail=1
else
  missing=0
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    if ! grep -qF "\`$f\`" FORK_DELTA.md; then
      echo "  UNDOCUMENTED: $f (add a row to FORK_DELTA.md)"
      missing=1
    fi
  done < <(git diff --diff-filter=M --name-only "$MB" HEAD)
  if [ "$missing" -eq 0 ]; then
    echo "  OK: every modified upstream file (base ${MB:0:9}) has a row."
  else
    fail=1
  fi
fi

echo "== done (exit $fail) =="
exit "$fail"
