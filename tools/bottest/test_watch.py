#!/usr/bin/env python3
"""Unit tests for watch.py detectors + the .steps parser. No server, no client, no canary.

  python -m unittest tools/bottest/test_watch.py -v

CRASH detection is verified WITHOUT touching canary: the detector is fed recorded
`systemctl show canary -p ExecMainPID,NRestarts,ActiveState,SubState` output (the first
sample below was captured read-only from production on 2026-11-18) and edited copies of it
that stand for a restart / a stopped service.
"""
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import watch  # noqa: E402

RECORDED = """NRestarts=0
ExecMainPID=203
ActiveState=active
SubState=running
"""
RESTARTED = RECORDED.replace("ExecMainPID=203", "ExecMainPID=4711").replace("NRestarts=0", "NRestarts=1")
FAILED = "NRestarts=1\nExecMainPID=0\nActiveState=failed\nSubState=failed\n"
ACTIVATING = "NRestarts=1\nExecMainPID=5000\nActiveState=activating\nSubState=auto-restart\n"
BACK = "NRestarts=2\nExecMainPID=5001\nActiveState=active\nSubState=running\n"


class CrashDetectorTest(unittest.TestCase):
    def feed(self, d, txt):
        return d.feed(watch.parse_show(txt))

    def test_steady_state_is_silent(self):
        d = watch.CrashDetector()
        for _ in range(5):
            self.assertIsNone(self.feed(d, RECORDED))

    def test_restart_detected_once_and_rearms(self):
        d = watch.CrashDetector()
        self.assertIsNone(self.feed(d, RECORDED))
        msg = self.feed(d, RESTARTED)
        self.assertIsNotNone(msg)
        self.assertTrue(msg.startswith("CRASH"))
        self.assertIn("203->4711", msg)
        self.assertIsNone(self.feed(d, RESTARTED))          # same incident: no repeat
        msg2 = self.feed(d, BACK)                           # second restart: re-armed
        self.assertTrue(msg2 and msg2.startswith("CRASH"))

    def test_down_then_up(self):
        d = watch.CrashDetector()
        self.feed(d, RECORDED)
        m1 = self.feed(d, FAILED)
        self.assertTrue(m1.startswith("CRASH") and "failed" in m1)
        self.assertIsNone(self.feed(d, ACTIVATING))         # still down: reported once
        m3 = self.feed(d, BACK)                             # new pid, active again
        self.assertTrue(m3.startswith("RECOVERED"))         # restart already reported by the down event
        self.assertIsNone(self.feed(d, BACK))

    def test_not_active_at_start(self):
        d = watch.CrashDetector()
        self.assertTrue(self.feed(d, FAILED).startswith("CRASH"))


class SlowDetectorTest(unittest.TestCase):
    def test_threshold_and_rearm(self):
        s = watch.SlowDetector(slow_ms=500, per_min=3)
        t = 1000.0
        for i in range(4):
            s.feed("x [CYCLE_SLOW] total=600ms exec=1ms", t + i)
        s.feed("x [CYCLE_SLOW] total=40ms exec=1ms", t + 5)  # below slow_ms: ignored
        m = s.check(t + 6)
        self.assertTrue(m and m.startswith("SLOW 4 lines"))
        self.assertIsNone(s.check(t + 7))
        self.assertIsNone(s.check(t + 200))                  # window empty -> re-armed
        for i in range(4):
            s.feed("[DISP_SLOW] kind=cycle name=BotEngine::tick duration=900ms", t + 300 + i)
        self.assertTrue(s.check(t + 305).startswith("SLOW"))


class RegexTest(unittest.TestCase):
    def test_abort(self):
        self.assertTrue(watch.ABORT_RE.search("[error] Lua Script Error: [Scripts Interface]"))
        self.assertTrue(watch.ABORT_RE.search("lua error: attempt to index nil"))
        self.assertFalse(watch.ABORT_RE.search("[BotEngine] terror bird spawn"))

    def test_runfail_and_done(self):
        self.assertTrue(watch.STUCK_RE.search("12:00:01 step 3 STUCK verb=goto reason=no-path pos=1,2,7"))
        self.assertTrue(watch.DONE_RE.search("x\nDONE\n"))
        self.assertFalse(watch.DONE_RE.search("state=DONE pos=1"))
        self.assertTrue(watch.DONE_RE.search("12:49:38 EXIT 0\n12:49:38 DONE\n"))

    def test_pid_alive(self):
        self.assertTrue(watch.pid_alive(os.getpid()))
        p = subprocess.Popen([sys.executable, "-c", "pass"])
        p.wait()
        time.sleep(0.2)
        self.assertFalse(watch.pid_alive(p.pid))


class FakeRemote:
    """Dummy target standing in for the ssh session: replays recorded systemctl/journal output."""

    def __init__(self, script):
        self.script = list(script)

    def poll(self):
        show, lines = self.script.pop(0)
        return watch.parse_show(show), lines


class WatcherEndToEndTest(unittest.TestCase):
    def test_watch_file(self):
        import argparse
        d = Path(tempfile.mkdtemp())
        a = argparse.Namespace(poll=0, logstall=180, heartbeat=30, slow_ms=500, slow_per_min=3,
                               max_age_min=1, runner_pid=0, ssh_target="x")
        w = watch.Watcher(d, a)
        line = ("1795025202.303181 Tibia canary[203]: [2026-18-11 13:06:42.303] [error] "
                "Lua Script Error: [Scripts Interface] attempt to index a nil value")
        fr = FakeRemote([(RECORDED, []), (RECORDED, [line]), (RESTARTED, []), (RESTARTED, [])])
        for _ in range(4):
            w.check_server(fr)
        # a client marked up whose pid is dead -> CLIENTDOWN; result row + DONE -> end markers
        (d / "clients.json").write_text('{"p1": {"pid": 999999, "state": "up", "role": "player"}}')
        w.check_clients()
        (d / "result.md").write_text("| Result | PASS | exit=0 |\n")
        (d / "run.log").write_text("12:00:00 step 1 x\n12:00:01 DONE\n")
        self.assertTrue(w.check_run())
        w.out.close()
        out = (d / ".watch").read_text()
        self.assertEqual(out.count("ABORT? "), 1)
        self.assertEqual(out.count("CRASH canary restarted: pid 203->4711"), 1)
        self.assertIn("CLIENTDOWN p1", out)
        self.assertIn("RESULT-READY", out)
        self.assertIn("RUN-ENDED", out)


class StepsParserTest(unittest.TestCase):
    def test_parse(self):
        import runner
        text = ('# SCENARIO: demo_one   SEED: 7   CAMERAS: none\n'
                '@p1 login\n'
                '@p1 goto 32369,32235,7 range=2  # trailing comment\n'
                '@cam:"Aldo Fenwick" login\n'
                '@srv cmd "Aldo Fenwick" status ; cmd "Aldo Fenwick" verbose on\n')
        hdr, steps = runner.parse_steps(text)
        self.assertEqual(hdr["SCENARIO"], "demo_one")
        self.assertEqual(hdr["SEED"], "7")
        self.assertEqual(hdr["CAMERAS"], "none")
        self.assertEqual([s.actor for s in steps], ["p1", "p1", "cam:Aldo Fenwick", "srv", "srv"])
        self.assertEqual(steps[1].verb, "goto")
        self.assertEqual(steps[1].kw["range"], "2")
        self.assertEqual(steps[1].pos, ["32369,32235,7"])
        self.assertEqual(steps[4].pos, ["Aldo Fenwick", "verbose", "on"])

    def test_vars(self):
        import runner
        self.assertEqual(runner.subst("$B1 and ${B2}.x", {"B1": "Kor", "B2": "Ann"}), "Kor and Ann.x")


class AuditTest(unittest.TestCase):
    """Phase 2 server audit: envelope parsing, PC-clock mapping, expect filters, census summary.
    The records below are hand-written in the exact shape bot_debug.cpp/bot_audit_sink.cpp emit."""
    SAMPLES = [{"at_pc_ms": 1_000_000.0, "offset_ms": -1000.0}]
    LINES = "\n".join([
        '{"v":1,"ts":1001000,"seq":1,"src":"srv","k":"audit","kinds":"probe,state,walkpause,census","watch":"Aldo Fenwick","watchResolved":1,"gen":3}',
        '{"v":1,"ts":1001100,"seq":2,"src":"srv","k":"walkpause","guid":65010,"cid":268500554,"bot":"Aldo Fenwick",'
        '"pos":[32369,32236,7],"st":"IDLE","hib":0,"tier":"observed","ms":4200,"walkBias":9,"nThisRoute":1,"steps":7}',
        '{"v":1,"ts":1001200,"seq":3,"src":"srv","k":"census","reg":997,"active":200,"awake":12,"hibn":188,"cast":1,'
        '"paused":0,"state":{"IDLE":50,"HUNTING":120},"phase":{"PATROLLING":[3,90]},"town":{"8":[5,20]},'
        '"vhunt":{"n":117,"patrolling":90,"kills":400,"wpPct":41},"lhunt":{"n":3,"patrolling":3,"kills":2,"wpPct":10},'
        '"anchors":1,"realPlayers":1,"wakeTried60s":4,"wakeGated60s":0,"wakeGranted60s":4}',
        '{"v":1,"ts":1001300,"seq":4,"src":"srv","k":"walk',  # torn line from a live tail: skipped
    ])

    def test_parse_and_clock(self):
        import merge
        recs = merge.parse_audit_text(self.LINES, self.SAMPLES)
        self.assertEqual([r["k"] for r in recs], ["audit", "walkpause", "census"])
        wp = recs[1]
        self.assertEqual(wp["src"], "srv:audit")
        self.assertEqual(wp["name"], "Aldo Fenwick")
        self.assertEqual(wp["pos"], {"x": 32369, "y": 32236, "z": 7})
        self.assertAlmostEqual(wp["ts"], 1001100 + 1000.0)  # pc = srv - offset, offset = -1000
        self.assertEqual(wp["srv_ts"], 1001100)

    def test_expect_filters(self):
        import merge
        import runner
        recs = merge.parse_audit_text(self.LINES, self.SAMPLES)
        r = runner.Runner.__new__(runner.Runner)  # filters only; no I/O
        fl = r.parse_filters({"src": "srv:audit", "k": "walkpause", "bot": "Aldo Fenwick", "tier": "observed",
                              "min": "1", "within": "300"})
        self.assertEqual(sum(1 for e in recs if r.ev_match(e, fl, {})), 1)
        fl_any = r.parse_filters({"k": "walkpause", "bot": "any"})
        self.assertEqual(sum(1 for e in recs if r.ev_match(e, fl_any, {})), 1)
        fl_jrnl = r.parse_filters({"src": "srv", "k": "walkpause"})  # journal-only src never matches audit
        self.assertEqual(sum(1 for e in recs if r.ev_match(e, fl_jrnl, {})), 0)

    def test_census_summary(self):
        import merge
        c = merge.census_summary(merge.parse_audit_text(self.LINES, self.SAMPLES))
        self.assertEqual(c["n"], 1)
        self.assertEqual(c["hib"]["p50"], 188.0)
        self.assertEqual(c["state_last"], {"IDLE": 50, "HUNTING": 120})
        self.assertEqual(c["vhunt_last"]["wpPct"], 41)


if __name__ == "__main__":
    unittest.main()
