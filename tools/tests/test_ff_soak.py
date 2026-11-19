"""tools/ff_soak.py: the plan only (the device is never opened)."""
import io
import unittest
from contextlib import redirect_stdout

import ff_play
import ff_soak


class TestFfSoak(unittest.TestCase):
    def test_plan_is_repeatable_and_covers_the_time(self):
        a = ff_soak.make_plan(3, seed=7)
        self.assertEqual(a, ff_soak.make_plan(3, seed=7))
        self.assertNotEqual(a, ff_soak.make_plan(3, seed=8))
        total = sum(p[3] + p[5] for p in a)
        self.assertGreaterEqual(total, 180.0)
        self.assertLess(total, 180.0 + 6.0)                 # at most one effect over

    def test_every_item_is_within_the_safe_bounds(self):
        for kind, percent, deg, seconds, period, pause in ff_soak.make_plan(10, seed=3):
            self.assertIn(kind, ("constant", "pulse", "sine"))
            self.assertLessEqual(percent, ff_soak.MAX_PERCENT)
            self.assertGreaterEqual(percent, 0.0)
            self.assertIn(deg, (90.0, 270.0))
            self.assertLessEqual(seconds, 3.0)
            self.assertGreaterEqual(pause, ff_soak.GAP_S[0])
            if kind == "pulse":
                self.assertAlmostEqual(seconds, 0.15, places=2)
            e = ff_play.make_effect(kind, percent, deg, seconds, period)   # every item builds a valid effect
            self.assertLessEqual(e.replay.length, 3000)

    def test_mix_contains_all_kinds(self):
        kinds = {p[0] for p in ff_soak.make_plan(5, seed=1)}
        self.assertEqual(kinds, {"constant", "pulse", "sine"})

    def test_dry_run_touches_no_device(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = ff_soak.main(["--minutes", "0.5", "--dry-run"])
        self.assertEqual(rc, 0)
        self.assertIn("effects, about", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
