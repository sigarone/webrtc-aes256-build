"""Unit tests of aruba/aruba_tool.py: the pure decision logic (gate, guard, window, metrics parsing).
The token / id / room parts are cross-checked against the Node modules in test/aruba.test.mjs."""

import os
import sys
import unittest
from datetime import datetime, timezone

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "aruba"))
import aruba_tool as t  # noqa: E402

METRICS = """# TYPE bcrypto_active_calls gauge
bcrypto_active_calls 0
bcrypto_total_calls 8
bcrypto_ws_messages_by_type_total{type="call_offer"} 26
bcrypto_ws_messages_by_type_total{type="call_answer"} 10
bcrypto_ws_messages_by_type_total{type="call_accepted"} 9
bcrypto_ws_messages_by_type_total{type="call_ice"} 126
bcrypto_ws_messages_by_type_total{type="call_video_state"} 686
bcrypto_ws_messages_by_type_total{type="group_call_join"} 4
"""


def utc(y, mo, d, h, mi):
    return datetime(y, mo, d, h, mi, tzinfo=timezone.utc)


def sample(**kw):
    base = {"load1": 0.1, "bc_cpu": 1.0, "health": (200, 1.0), "window_ok": True, "manual": False,
            "metrics": {"active_calls": 0.0, "total_calls": 8.0, "call_msgs": 45.0, "group_msgs": 4.0},
            "rooms": (0, 0, 0, 0)}
    base.update(kw)
    return base


class MetricsTest(unittest.TestCase):
    def test_parse(self):
        m = t.parse_metrics(METRICS)
        self.assertEqual(m, {"active_calls": 0.0, "total_calls": 8.0, "call_msgs": 45.0, "group_msgs": 4.0})

    def test_missing_values_stay_none(self):
        m = t.parse_metrics("something_else 1\n")
        self.assertIsNone(m["active_calls"])
        self.assertIsNone(m["total_calls"])


def _has_tz():
    try:
        from zoneinfo import ZoneInfo
        ZoneInfo("Europe/Rome")
        return True
    except Exception:
        return False


@unittest.skipUnless(_has_tz(), "no tz database on this machine (the node and the CI runners have one)")
class WindowTest(unittest.TestCase):
    # 2026-10-01 is CEST (UTC+2), 2026-11-15 is CET (UTC+1)
    def test_edges_in_summer_time(self):
        self.assertFalse(t.in_window(utc(2026, 10, 1, 21, 29)))   # 23:29 Rome
        self.assertTrue(t.in_window(utc(2026, 10, 1, 21, 30)))    # 23:30
        self.assertTrue(t.in_window(utc(2026, 10, 2, 3, 59)))     # 05:59
        self.assertFalse(t.in_window(utc(2026, 10, 2, 4, 0)))     # 06:00
        self.assertFalse(t.in_window(utc(2026, 10, 1, 19, 0)))    # 21:00

    def test_edges_in_winter_time(self):
        self.assertFalse(t.in_window(utc(2026, 11, 15, 22, 29)))  # 23:29 Rome
        self.assertTrue(t.in_window(utc(2026, 11, 15, 22, 30)))
        self.assertTrue(t.in_window(utc(2026, 11, 16, 4, 59)))
        self.assertFalse(t.in_window(utc(2026, 11, 16, 5, 0)))


class GateTest(unittest.TestCase):
    OK = dict(m={"active_calls": 0.0, "total_calls": 8.0, "call_msgs": 0.0, "group_msgs": 0.0}, quiet_lines=0,
              load1=0.2, health=(200, 2.0), rooms=(0, 0, 0, 0), window_ok=True)

    def gate(self, **kw):
        a = dict(self.OK)
        a.update(kw)
        return t.evaluate_gate(a["m"], a["quiet_lines"], a["load1"], a["health"], a["rooms"], a["window_ok"])

    def test_all_clear(self):
        self.assertEqual(self.gate(), [])

    def test_every_gate_closes_it(self):
        cases = {
            "window": dict(window_ok=False),
            "metrics unreadable": dict(m=None),
            "active call": dict(m={"active_calls": 1.0, "total_calls": 8.0, "call_msgs": 0.0, "group_msgs": 0.0}),
            "active calls unknown": dict(m={"active_calls": None, "total_calls": 8.0, "call_msgs": 0.0, "group_msgs": 0.0}),
            "total calls unknown": dict(m={"active_calls": 0.0, "total_calls": None, "call_msgs": 0.0, "group_msgs": 0.0}),
            "recent call": dict(quiet_lines=1),
            "journal unreadable": dict(quiet_lines=None),
            "load": dict(load1=1.6),
            "load unknown": dict(load1=None),
            "health": dict(health=(503, 2.0)),
            "health unknown": dict(health=None),
            "group call": dict(rooms=(1, 3, 0, 0)),
            "rooms unreadable": dict(rooms=None),
            "leftover rooms": dict(rooms=(0, 0, 2, 0)),
        }
        for name, kw in cases.items():
            self.assertTrue(self.gate(**kw), name)

    def test_idle_foreign_room_does_not_close_it(self):
        self.assertEqual(self.gate(rooms=(2, 0, 0, 0)), [])


class GuardTest(unittest.TestCase):
    def state(self, health_ms=2.0):
        return t.GuardState({"total_calls": 8.0, "call_msgs": 45.0, "group_msgs": 4.0}, health_ms)

    def test_quiet_run_goes_on(self):
        st = self.state()
        for _ in range(50):
            self.assertIsNone(st.check(sample()))

    def test_each_breach_aborts(self):
        self.assertIn("window", self.state().check(sample(window_ok=False)))
        self.assertIn("manual", self.state().check(sample(manual=True)))
        self.assertIn("load average", self.state().check(sample(load1=3.6)))
        self.assertIsNone(self.state().check(sample(load1=3.5)))
        self.assertIn("unreadable", self.state().check(sample(metrics=None)))
        m = sample()["metrics"]
        self.assertIn("real call is active", self.state().check(sample(metrics=dict(m, active_calls=1.0))))
        self.assertIn("started", self.state().check(sample(metrics=dict(m, total_calls=9.0))))
        self.assertIn("started", self.state().check(sample(metrics=dict(m, call_msgs=46.0))))
        self.assertIn("started", self.state().check(sample(metrics=dict(m, group_msgs=5.0))))
        self.assertIn("group call", self.state().check(sample(rooms=(1, 2, 0, 0))))

    def test_foreign_room_appearing_aborts(self):
        st = self.state()
        self.assertIsNone(st.check(sample(rooms=(1, 0, 4, 32))))  # baseline: one idle foreign room
        self.assertIsNone(st.check(sample(rooms=(1, 0, 4, 32))))
        self.assertIn("foreign room", st.check(sample(rooms=(2, 0, 4, 32))))

    def test_unreadable_rooms_do_not_abort_by_themselves(self):
        self.assertIsNone(self.state().check(sample(rooms=None)))

    def test_health_needs_three_in_a_row(self):
        st = self.state(health_ms=2.0)  # limit = max(150, 12) = 150 ms
        self.assertIsNone(st.check(sample(health=(200, 200.0))))
        self.assertIsNone(st.check(sample(health=(200, 200.0))))
        self.assertIsNone(st.check(sample(health=(200, 5.0))))     # a good sample resets the run
        self.assertIsNone(st.check(sample(health=(200, 200.0))))
        self.assertIsNone(st.check(sample(health=(200, 200.0))))
        self.assertIn("health", st.check(sample(health=(503, 3.0))))

    def test_health_limit_follows_a_slow_baseline(self):
        st = self.state(health_ms=40.0)  # limit = 240 ms
        for _ in range(5):
            self.assertIsNone(st.check(sample(health=(200, 200.0))))
        st = self.state(health_ms=40.0)
        self.assertIsNone(st.check(sample(health=(200, 250.0))))
        self.assertIsNone(st.check(sample(health=(200, 250.0))))
        self.assertIn("health", st.check(sample(health=(200, 250.0))))

    def test_cpu_needs_three_in_a_row(self):
        st = self.state()
        self.assertIsNone(st.check(sample(bc_cpu=41.0)))
        self.assertIsNone(st.check(sample(bc_cpu=41.0)))
        self.assertIsNone(st.check(sample(bc_cpu=10.0)))
        self.assertIsNone(st.check(sample(bc_cpu=41.0)))
        self.assertIsNone(st.check(sample(bc_cpu=41.0)))
        self.assertIn("CPU", st.check(sample(bc_cpu=41.0)))
        self.assertIsNone(self.state().check(sample(bc_cpu=None)))


class TokenTest(unittest.TestCase):
    def test_format_and_limits(self):
        tok = t.mint_token("sekret", 600, now=1000.0)
        self.assertRegex(tok, r"^1600,janus,janus\.plugin\.videoroom:[A-Za-z0-9+/]+={0,2}$")
        with self.assertRaises(t.ToolError):
            t.mint_token("sekret", t.MAX_TTL_SEC + 1)
        with self.assertRaises(t.ToolError):
            t.mint_token("sekret", 0)
        with self.assertRaises(t.ToolError):
            t.mint_token("", 60)


if __name__ == "__main__":
    unittest.main()
