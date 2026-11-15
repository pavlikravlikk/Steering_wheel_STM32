import io
import unittest
from contextlib import redirect_stdout

import gen_config
import measure_counts as mc


class TestMeasureCounts(unittest.TestCase):
    def setUp(self):
        self.p = gen_config.load_profile(gen_config.DEFAULT_PROFILE)

    def test_counts_per_turn(self):
        self.assertEqual(mc.counts_per_turn(100, 4900, 2), 2400.0)
        self.assertEqual(mc.counts_per_turn(4900, 100, 2), -2400.0)
        with self.assertRaises(ValueError):
            mc.counts_per_turn(0, 10, 0)

    def test_encoder_shaft_matches_profile(self):
        r = mc.analyse_encoder(117, 117 + 3 * 2400, 3, 600, 4)
        self.assertEqual(r["counts_per_encoder_turn_int"], 2400)
        self.assertEqual(r["warnings"], [])

    def test_encoder_shaft_mismatch_warns(self):
        r = mc.analyse_encoder(0, 3 * 1600, 3, 600, 4)
        self.assertTrue(any("profile says 2400" in w for w in r["warnings"]))
        r = mc.analyse_encoder(0, 7000, 3, 600, 4)   # 2333.3 counts: miscounted turns
        self.assertTrue(r["warnings"])

    def test_wheel_matching_profile(self):
        r = mc.analyse_wheel(-42, -42 + 2 * 2400, 2, self.p)
        self.assertEqual(r["cpr"], 2400)
        self.assertEqual(r["implied_teeth_ratio"], "1:1")
        self.assertEqual(r["odrive_turns_per_wheel_rev"], 1.0)
        self.assertEqual(r["warnings"], [])

    def test_wheel_3_to_1_belt(self):
        r = mc.analyse_wheel(0, -2 * 7200, 2, self.p, current_cpr=2400)
        self.assertEqual(r["cpr"], 7200)
        self.assertEqual(r["direction"], "negative")
        self.assertEqual(r["implied_teeth_ratio"], "3:1")
        self.assertAlmostEqual(r["odrive_turns_per_wheel_rev_with_current_cpr"], 3.0)
        self.assertTrue(any("differs from the profile" in w for w in r["warnings"]))
        self.assertTrue(any("does not match the profile 20:20" in w for w in r["warnings"]))

    def test_wheel_non_integer_result(self):
        # 20:7 belt -> 6857.14 counts per motor turn
        p = self.p
        p["belt"]["teeth_housing"]["value"] = 20
        p["belt"]["teeth_pulley"]["value"] = 7
        r = mc.analyse_wheel(0, int(2 * 2400 * 20 / 7), 2, p)
        self.assertTrue(any("rounded" in w or "integer" in w for w in r["warnings"]), r["warnings"])

    def test_cli(self):
        with redirect_stdout(io.StringIO()) as buf:
            rc = mc.main(["wheel", "--turns", "2", "--start", "0", "--end", "4800"])
        self.assertEqual(rc, 0)
        self.assertIn("Profile OK", buf.getvalue())
        with redirect_stdout(io.StringIO()):
            self.assertEqual(mc.main(["encoder", "--turns", "0", "--start", "0", "--end", "1"]), 2)


if __name__ == "__main__":
    unittest.main()
