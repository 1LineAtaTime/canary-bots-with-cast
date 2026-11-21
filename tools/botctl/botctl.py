#!/usr/bin/env python3
r"""botctl -- run one /cavebot command on the live server and print its reply (#34).

The server drains the `bot_commands` MySQL table (BotManager globalevent) and writes the
reply into `result` + `processed = 1`. botctl inserts a row, waits on it, and prints the reply,
so a script or an agent gets the command output synchronously, without an in-game GM.

usage:
  botctl.py '/cavebot "Kaelis Duke" status'      quoted bot name
  botctl.py '/cavebot Kaelis Duke status'        unquoted: longest bot-name prefix wins
  botctl.py --bot "Kaelis Duke" status           explicit bot
  botctl.py botcfg                               no bot name -> _global
  botctl.py --bench 100 --bot "Kaelis Duke" status   latency table (p50/p95/max)

exit codes / first stdout word:
  0 OK           the server answered and the reply is not a failure
  2 FAULT        the server answered with a failure ("not found", "Unknown command", "Usage", ...)
  3 NO-RESPONSE  no answer within --timeout (server down or not polling); the queued row is
                 deleted so a later boot does not run a stale command
  4 ERROR        botctl itself failed (DB unreachable, bad arguments)

DB: .claude/secrets.local (DB_HOST/DB_USER/DB_PASS) or BOTPERF_SECRETS; database from --db or
BOTPERF_DB (default canary_1525_port, the live 15.25 server). Uses mysql.connector when
installed (one connection, ~ms per poll); otherwise the mysql CLI through tools/botperf/driver.py.
"""
from __future__ import annotations

import argparse
import os
import random
import re
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.botperf.driver import BotCommandDriver, CommandError, DbConfig  # noqa: E402

EXIT_OK, EXIT_FAULT, EXIT_NORESP, EXIT_ERROR = 0, 2, 3, 4

# Replies the engine and the Lua queue use for a command that did not run.
FAULT_RE = re.compile(
    r"^(Bot '.*' not found|Bot '.*' is hibernated. Use|Bot '.*' is not (active|hunting|in a party)|Unknown command|Usage:|"
    r"Hunt script \d+ not found|.*\bnot online\b|.*\bERROR\b|.*\bfailed\b)", re.IGNORECASE)


class Channel:
    """One DB connection for insert + poll. mysql.connector if present, else the CLI driver."""

    def __init__(self, db: DbConfig):
        self.db = db
        self.conn = None
        try:
            import mysql.connector  # type: ignore
            self.conn = mysql.connector.connect(host=db.host, port=int(db.port), user=db.user,
                                                password=db.password, database=db.database,
                                                autocommit=True, connection_timeout=10)
        except ImportError:
            self.cli = BotCommandDriver(db)

    def insert(self, bot: str, command: str) -> int:
        if self.conn:
            cur = self.conn.cursor()
            cur.execute("INSERT INTO bot_commands (bot_name, command) VALUES (%s, %s)", (bot, command))
            return int(cur.lastrowid)
        rows = self.cli._sql("INSERT INTO bot_commands (bot_name, command) VALUES "
                             f"('{self.cli._esc(bot)}', '{self.cli._esc(command)}'); SELECT LAST_INSERT_ID();")
        return int(rows[0][0])

    def poll(self, cmd_id: int) -> str | None:
        if self.conn:
            cur = self.conn.cursor()
            cur.execute("SELECT processed, COALESCE(result, '') FROM bot_commands WHERE id = %s", (cmd_id,))
            row = cur.fetchone()
            return row[1] if row and int(row[0]) == 1 else None
        rows = self.cli._sql(f"SELECT processed FROM bot_commands WHERE id = {cmd_id};")
        return self.cli._read_result(cmd_id) if rows and rows[0][0] == "1" else None

    def drop_unprocessed(self, cmd_id: int) -> None:
        sql = f"DELETE FROM bot_commands WHERE id = {int(cmd_id)} AND processed = 0"
        if self.conn:
            self.conn.cursor().execute(sql)
        else:
            self.cli._sql(sql + ";")

    def bot_names(self) -> list[str]:
        sql = "SELECT name FROM players WHERE account_id = 65000"
        if self.conn:
            cur = self.conn.cursor()
            cur.execute(sql)
            return [r[0] for r in cur.fetchall()]
        return [r[0] for r in self.cli._sql(sql + ";")]


def split_target(ch: Channel, text: str, bot: str | None) -> tuple[str, str]:
    text = text.strip()
    # "/cavebot ..." (Git Bash may have rewritten the leading /cavebot into a Windows path)
    text = re.sub(r"^\S*/cavebot\s+", "", text, flags=re.IGNORECASE)
    if bot:
        return bot, text
    m = re.match(r'^"([^"]+)"\s+(.+)$', text)
    if m:
        return m.group(1), m.group(2)
    low = text.lower()
    best = ""
    for name in ch.bot_names():
        if len(name) > len(best) and low.startswith(name.lower() + " "):
            best = name
    if best:
        return best, text[len(best) + 1:]
    if low.startswith("_global "):
        return "_global", text[8:]
    return "_global", text


def run_one(ch: Channel, bot: str, command: str, timeout: float, interval: float) -> tuple[int, str, float]:
    t0 = time.monotonic()
    cmd_id = ch.insert(bot, command)
    while time.monotonic() - t0 < timeout:
        reply = ch.poll(cmd_id)
        if reply is not None:
            dt = time.monotonic() - t0
            first = reply.strip().splitlines()[0] if reply.strip() else ""
            return (EXIT_FAULT if FAULT_RE.match(first) else EXIT_OK), reply, dt
        time.sleep(interval)
    ch.drop_unprocessed(cmd_id)
    return EXIT_NORESP, "", time.monotonic() - t0


LABEL = {EXIT_OK: "OK", EXIT_FAULT: "FAULT", EXIT_NORESP: "NO-RESPONSE"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("command", nargs="+", help="command, optionally prefixed by /cavebot and a bot name")
    ap.add_argument("--bot", help="bot name (default: parsed from the command, else _global)")
    ap.add_argument("--db", default=os.environ.get("BOTPERF_DB", "canary_1525_port"))
    ap.add_argument("--timeout", type=float, default=10.0, help="seconds before NO-RESPONSE (default 10)")
    ap.add_argument("--interval", type=float, default=0.05, help="poll interval in seconds (default 0.05)")
    ap.add_argument("--jitter", type=float, default=1.0, help="bench: random 0..J s pause before each command")
    ap.add_argument("--bench", type=int, default=0, help="run the command N times and print a latency table")
    a = ap.parse_args()
    try:
        os.environ["BOTPERF_DB"] = a.db
        sec = os.environ.get("BOTPERF_SECRETS", str(ROOT / ".claude" / "secrets.local"))
        if not Path(sec).exists():  # a worktree has no secrets of its own
            sec = r"E:\TibiaCanary\canary-bot-players\.claude\secrets.local"
        ch = Channel(DbConfig.from_secrets(sec))
        bot, command = split_target(ch, " ".join(a.command), a.bot)
        if not a.bench:
            code, reply, dt = run_one(ch, bot, command, a.timeout, a.interval)
            print(f"{LABEL[code]} {dt * 1000:.0f}ms bot={bot!r} cmd={command!r}")
            if reply:
                print(reply)
            return code
        lat, codes = [], {EXIT_OK: 0, EXIT_FAULT: 0, EXIT_NORESP: 0}
        for _ in range(a.bench):
            # Random arrival phase: back-to-back commands phase-lock to the server's 1 s queue
            # poll (MANAGER_INTERVAL) and would measure the worst case every time.
            time.sleep(random.uniform(0, a.jitter))
            code, _, dt = run_one(ch, bot, command, a.timeout, a.interval)
            codes[code] += 1
            if code != EXIT_NORESP:
                lat.append(dt * 1000)
        lat.sort()
        q = (lambda p: lat[min(len(lat) - 1, int(p * len(lat)))]) if lat else (lambda p: float("nan"))
        print(f"BENCH n={a.bench} ok={codes[EXIT_OK]} fault={codes[EXIT_FAULT]} noresp={codes[EXIT_NORESP]} "
              f"min={lat[0] if lat else float('nan'):.0f}ms p50={q(0.5):.0f}ms p95={q(0.95):.0f}ms "
              f"max={lat[-1] if lat else float('nan'):.0f}ms mean={statistics.mean(lat) if lat else float('nan'):.0f}ms "
              f"under1s={sum(1 for x in lat if x <= 1000)}/{len(lat)}")
        return EXIT_OK if codes[EXIT_OK] == a.bench else (EXIT_NORESP if codes[EXIT_NORESP] else EXIT_FAULT)
    except (CommandError, OSError, KeyError, ValueError) as exc:
        print(f"ERROR {exc}")
        return EXIT_ERROR
    except Exception as exc:  # mysql.connector errors etc.
        print(f"ERROR {type(exc).__name__}: {exc}")
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
