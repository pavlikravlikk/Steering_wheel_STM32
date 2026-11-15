import io
import os
import unittest
from contextlib import redirect_stdout

import gen_config
import odrive_enums
import odrive_status
from tests.fake_odrive import FakeODrive

FIX = os.path.join(os.path.dirname(__file__), "fixtures")


def fixture(name):
    with open(os.path.join(FIX, name), encoding="utf-8") as f:
        return f.read()


class TestEnums(unittest.TestCase):
    def test_decode_flags(self):
        self.assertEqual(odrive_enums.decode_flags(0, odrive_enums.AXIS_ERRORS), [])
        self.assertEqual(odrive_enums.decode_flags(0x800 | 0x100, odrive_enums.AXIS_ERRORS),
                         ["ENCODER_FAILED", "WATCHDOG_TIMER_EXPIRED"])
        self.assertEqual(odrive_enums.decode_register("axis0.encoder.error", 0x2), ["CPR_POLEPAIRS_MISMATCH"])
        self.assertIn("UNKNOWN_BITS_0x10000000", odrive_enums.decode_register("axis0.error", 0x10000000))
        self.assertIn("DC_BUS_UNDER_VOLTAGE (legacy 0.5.1 bit)", odrive_enums.decode_register("axis0.error", 0x2))
        self.assertEqual(odrive_enums.decode_register("odrv0.error", 0x4), ["DC_BUS_OVER_VOLTAGE"])


class TestDumpParser(unittest.TestCase):
    def test_tree_with_prefix(self):
        v = odrive_status.parse_dump(fixture("odrive_factory_axis0_tree.txt"), prefix="axis0")
        self.assertEqual(v["axis0.motor.config.current_lim"], 12.0)
        self.assertEqual(v["axis0.controller.config.control_mode"], 3)
        self.assertIs(v["axis0.config.startup_closed_loop_control"], False)
        self.assertEqual(v["axis0.error"], 0)
        self.assertEqual(v["axis0.encoder.shadow_count"], 117)

    def test_flat_with_odrv0_prefix_ignores_prefix_option(self):
        v = odrive_status.parse_dump(fixture("odrive_factory_root_flat.txt"), prefix="axis0")
        self.assertEqual(v["vbus_voltage"], 24.08)
        self.assertEqual(v["config.dc_bus_overvoltage_trip_level"], 56.0)

    def test_values(self):
        self.assertEqual(odrive_status.parse_value("0x0800 (int)"), 0x800)
        self.assertIs(odrive_status.parse_value("True (bool)"), True)
        self.assertEqual(odrive_status.parse_value("-3"), -3)
        self.assertEqual(odrive_status.parse_value("24.5 (float)"), 24.5)
        self.assertEqual(odrive_status.parse_value("abc"), "abc")


class TestAnalyse(unittest.TestCase):
    def setUp(self):
        self.p = gen_config.load_profile(gen_config.DEFAULT_PROFILE)

    def test_factory_state_lists_all_differences_and_blocks_ffb(self):
        v = odrive_status.parse_dump(fixture("odrive_factory_axis0_tree.txt"), prefix="axis0")
        v.update(odrive_status.parse_dump(fixture("odrive_factory_root_flat.txt")))
        r = odrive_status.analyse(v, self.p)
        diffs = [t for lvl, t in r["lines"] if lvl == "DIFF"]
        joined = "\n".join(diffs)
        for path in ("current_lim", "vel_limit", "control_mode", "enable_watchdog",
                     "dc_bus_overvoltage_trip_level", "calibration_current"):
            self.assertIn(path, joined)
        self.assertFalse(r["ffb_ready"])
        blocks = [t for lvl, t in r["lines"] if lvl == "BLOCK"]
        self.assertTrue(any("control_mode" in b for b in blocks))
        self.assertTrue(any("encoder.is_ready" in b for b in blocks))

    def test_ready_state_has_no_differences(self):
        v = odrive_status.parse_dump(fixture("odrive_ready_flat.txt"))
        r = odrive_status.analyse(v, self.p)
        self.assertEqual([t for lvl, t in r["lines"] if lvl == "DIFF"], [])
        pre = odrive_status.precheck(v, self.p)
        failing = [t for ok, t in pre if not ok]
        # only the profile-TBD gate remains until the profile is measured
        self.assertEqual(len(failing), 1, failing)
        self.assertIn("TBD", failing[0])

    def test_ready_state_with_measured_profile_passes(self):
        for s in self.p:
            for k in self.p[s]:
                self.p[s][k]["status"] = "measured"
        v = odrive_status.parse_dump(fixture("odrive_ready_flat.txt"))
        self.assertTrue(odrive_status.analyse(v, self.p)["ffb_ready"])

    def test_errors_are_decoded_with_hints(self):
        v = odrive_status.parse_dump(fixture("odrive_ready_flat.txt"))
        v["axis0.encoder.error"] = 0x2
        v["axis0.error"] = 0x800
        r = odrive_status.analyse(v, self.p)
        text = "\n".join(t for _, t in r["lines"])
        self.assertIn("CPR_POLEPAIRS_MISMATCH", text)
        self.assertIn("WATCHDOG_TIMER_EXPIRED", text)
        self.assertIn("measure_counts", text)
        self.assertFalse(r["ffb_ready"])

    def test_startup_flag_true_is_error(self):
        v = odrive_status.parse_dump(fixture("odrive_ready_flat.txt"))
        v["axis0.config.startup_closed_loop_control"] = True
        r = odrive_status.analyse(v, self.p)
        self.assertTrue(any(lvl == "ERROR" and "startup_closed_loop_control" in t for lvl, t in r["lines"]))

    def test_each_precheck_condition_can_block(self):
        base = odrive_status.parse_dump(fixture("odrive_ready_flat.txt"))
        mutations = {
            "axis0.config.enable_watchdog": False, "axis0.config.watchdog_timeout": 2.0,
            "axis0.controller.config.control_mode": 3, "axis0.motor.config.current_lim": 12.0,
            "axis0.controller.config.vel_limit": 50.0, "axis0.motor.is_calibrated": False,
            "axis0.encoder.is_ready": False, "axis0.motor.error": 1, "vbus_voltage": 31.0,
            "axis0.motor.config.torque_constant": 0.0,
        }
        for k, bad in mutations.items():
            v = dict(base)
            v[k] = bad
            n_fail = sum(1 for ok, _ in odrive_status.precheck(v, self.p) if not ok)
            self.assertEqual(n_fail, 2, k)   # the mutated one + the profile-TBD gate

    def test_live_reader_is_read_only(self):
        v = odrive_status.parse_dump(fixture("odrive_ready_flat.txt"))
        odrv = FakeODrive(v)
        got = odrive_status.read_live(odrv, ["vbus_voltage", "axis0.error", "axis0.does_not_exist"])
        self.assertEqual(got, {"vbus_voltage": 24.1, "axis0.error": 0})
        self.assertEqual(odrv.writes, [])
        self.assertEqual(odrv.calls, [])

    def test_cli_fixture_exit_codes(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = odrive_status.main(["--fixture", os.path.join(FIX, "odrive_factory_axis0_tree.txt"),
                                     "--prefix", "axis0"])
        self.assertEqual(rc, 1)
        self.assertIn("FFB precheck would FAIL", buf.getvalue())
        with redirect_stdout(io.StringIO()):
            self.assertEqual(odrive_status.main([]), 2)
            self.assertEqual(odrive_status.main(["--manual"]), 0)


if __name__ == "__main__":
    unittest.main()
