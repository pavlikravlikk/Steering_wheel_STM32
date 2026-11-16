"""The ODrive doctor and the dump parser against the REAL dump docs/odrive_dump.txt
(odrivetool/IPython session of the bench board), plus truncated/corrupted copies."""
import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout

import gen_config
import odrive_dump
import odrive_enums
import odrive_status
from tests.fake_odrive import FakeODrive

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DUMP = os.path.join(REPO, "docs", "odrive_dump.txt")


def real_text():
    with open(DUMP, encoding="utf-8") as f:
        return f.read()


def real_values():
    return odrive_dump.parse_dump(real_text())


def ready_values(profile):
    """The real dump after a human applied the gentle profile, cleared the errors,
    turned the startup flags off and calibrated the encoder (derived, not measured)."""
    v = real_values()
    for path, value in gen_config.setup_settings(profile) + gen_config.check_only_settings(profile):
        v[path] = value
    v.update({"axis0.error": 0, "axis0.motor.error": 0, "axis0.encoder.error": 0,
              "axis0.encoder.is_ready": True})
    return v


def measured(profile):
    for s in profile:
        for k in profile[s]:
            profile[s][k]["status"] = "measured"
    return profile


class TestEnums(unittest.TestCase):
    def test_decode_flags(self):
        self.assertEqual(odrive_enums.decode_flags(0, odrive_enums.AXIS_ERRORS), [])
        self.assertEqual(odrive_enums.decode_flags(0x800 | 0x100, odrive_enums.AXIS_ERRORS),
                         ["ENCODER_FAILED", "WATCHDOG_TIMER_EXPIRED"])
        self.assertEqual(odrive_enums.decode_register("axis0.encoder.error", 0x2), ["CPR_POLEPAIRS_MISMATCH"])
        self.assertIn("UNKNOWN_BITS_0x10000000", odrive_enums.decode_register("axis0.error", 0x10000000))
        self.assertIn("DC_BUS_UNDER_VOLTAGE (legacy 0.5.1 bit)", odrive_enums.decode_register("axis0.error", 0x2))
        self.assertTrue(odrive_enums.decode_register("axis0.motor.error", 0x4000)[0].startswith("DC_BUS_OVER_REGEN"))

    def test_real_dump_error_registers(self):
        v = real_values()
        self.assertEqual(odrive_enums.decode_register("axis0.error", v["axis0.error"]), ["ENCODER_FAILED"])
        self.assertEqual(odrive_enums.decode_register("axis0.motor.error", v["axis0.motor.error"]),
                         ["CONTROL_DEADLINE_MISSED"])
        self.assertEqual(odrive_enums.decode_register("axis0.encoder.error", v["axis0.encoder.error"]),
                         ["NO_RESPONSE"])

    def test_real_dump_modes_and_family(self):
        v = real_values()
        self.assertEqual(odrive_enums.CONTROL_MODES[v["axis0.controller.config.control_mode"]], "POSITION_CONTROL")
        self.assertEqual(odrive_enums.INPUT_MODES[v["axis0.controller.config.input_mode"]], "TRAP_TRAJ")
        self.assertEqual(odrive_enums.AXIS_STATES[v["axis0.current_state"]], "IDLE")
        self.assertEqual(odrive_enums.firmware_family(v), "early 0.5.x (0.5.1-like)")


class TestRealDumpParser(unittest.TestCase):
    def test_no_parse_problems_and_key_values(self):
        v, problems = odrive_dump.parse_dump_report(real_text())
        self.assertEqual(problems, [])
        self.assertGreater(len(v), 200)
        expect = {
            "vbus_voltage": 24.18603515625, "axis0.error": 256, "axis0.motor.error": 16,
            "axis0.encoder.error": 4, "axis0.current_state": 1,
            "axis0.config.enable_watchdog": False, "axis0.config.watchdog_timeout": 0.0,
            "axis0.config.startup_closed_loop_control": True,
            "axis0.config.startup_encoder_offset_calibration": True,
            "axis0.controller.config.control_mode": 3, "axis0.controller.config.input_mode": 5,
            "axis0.controller.config.vel_limit": 50.0, "axis0.controller.config.pos_gain": 30.0,
            "axis0.controller.config.enable_current_mode_vel_limit": True,
            "axis0.motor.config.current_lim": 12.0, "axis0.motor.config.calibration_current": 5.0,
            "axis0.motor.config.pole_pairs": 15, "axis0.motor.config.direction": -1,
            "axis0.motor.config.pre_calibrated": True, "axis0.motor.is_calibrated": True,
            "axis0.encoder.config.cpr": 2400, "axis0.encoder.config.mode": 0,
            "axis0.encoder.config.pre_calibrated": False, "axis0.encoder.config.offset": -9298,
            "axis0.encoder.is_ready": False, "axis0.encoder.hall_state": 7,
            "config.dc_max_negative_current": -3.0, "config.dc_max_positive_current": 20.0,
            "config.dc_bus_overvoltage_trip_level": 56.0, "config.dc_bus_undervoltage_trip_level": 8.0,
            "config.brake_resistance": 2.0, "config.max_regen_current": 0.0,
            "config.enable_uart": True, "config.uart_baudrate": 115200, "brake_resistor_armed": True,
            "fw_version_unreleased": 1, "hw_version_variant": 56, "axis1.error": 0,
            "config.gpio1_pwm_mapping.endpoint": None,
        }
        for k, want in expect.items():
            self.assertIn(k, v, k)
            self.assertEqual(v[k], want, k)
        self.assertAlmostEqual(v["axis0.motor.config.torque_constant"], 0.04, places=6)
        self.assertEqual(v["axis0.motor.config.torque_lim"], float("inf"))
        # methods and unexpanded sub-objects are not values
        for bad in ("axis0.clear_errors", "axis0.watchdog_feed", "axis0.config", "axis0.motor",
                    "save_configuration", "axis0.config.calibration_lockin"):
            self.assertNotIn(bad, v)
        # properties that do NOT exist in this build
        for absent in ("error", "config.enable_brake_resistor", "config.uart_a_baudrate",
                       "axis0.controller.config.enable_torque_mode_vel_limit"):
            self.assertNotIn(absent, v)

    def test_truncated_dump(self):
        text = real_text()
        cut = text[: text.index("In [10]")]            # stops before the motor config
        v, problems = odrive_dump.parse_dump_report(cut)
        self.assertNotIn("axis0.motor.config.current_lim", v)
        self.assertIn("axis0.controller.config.control_mode", v)
        # cut in the middle of a line: the broken value is reported, not used
        mid = text[: text.index("control_mode: 3 (int32)") + len("control_mode: 3 (int")]
        v, problems = odrive_dump.parse_dump_report(mid)
        self.assertNotIn("axis0.controller.config.control_mode", v)
        self.assertTrue(any("malformed" in p[2] for p in problems), problems)

    def test_corrupted_dump(self):
        text = real_text().replace("current_lim: 12.0 (float)", "current_lim: 12.0 (flo@t)")
        text = text.replace("pole_pairs: 15 (int32)", "pole_pairs 15 int32")
        text += "\nIn [13]: some_other_object\nOut[13]:\nfoo: 1 (int32)\n"
        v, problems = odrive_dump.parse_dump_report(text)
        self.assertNotIn("axis0.motor.config.current_lim", v)
        self.assertNotIn("axis0.motor.config.pole_pairs", v)
        self.assertNotIn("foo", v)
        reasons = [p[2] for p in problems]
        self.assertTrue(any("malformed" in r for r in reasons))
        self.assertTrue(any("no ':'" in r for r in reasons))
        self.assertTrue(any("unsupported expression" in r for r in reasons))

    def test_other_formats_still_work(self):
        flat = "odrv0.vbus_voltage = 24.0\naxis0.error = 0x0800\n"
        self.assertEqual(odrive_dump.parse_dump(flat), {"vbus_voltage": 24.0, "axis0.error": 0x800})
        tree = "error = 0x0000 (int)\nconfig:\n  enable_watchdog = True (bool)\n"
        self.assertEqual(odrive_dump.parse_dump(tree, prefix="axis0"),
                         {"axis0.error": 0, "axis0.config.enable_watchdog": True})

    def test_values(self):
        self.assertEqual(odrive_dump.parse_value("0x0800 (int)"), 0x800)
        self.assertIs(odrive_dump.parse_value("True (bool)"), True)
        self.assertEqual(odrive_dump.parse_value("-3 (int32)"), -3)
        self.assertEqual(odrive_dump.parse_value("55461792789557 (uint64)"), 55461792789557)
        self.assertIsNone(odrive_dump.parse_value("None (object_ref)"))
        self.assertEqual(odrive_dump.parse_value("abc"), "abc")


class TestNamesExistInRealDump(unittest.TestCase):
    """Every property name the tools read or write exists in this firmware build."""

    def test_setup_and_check_paths(self):
        v = real_values()
        p = gen_config.load_profile(gen_config.DEFAULT_PROFILE)
        for path, _ in gen_config.setup_settings(p) + gen_config.check_only_settings(p):
            self.assertIn(path, v, path)

    def test_doctor_read_paths(self):
        v = real_values()
        for path in odrive_status.READ_PATHS:
            self.assertIn(path, v, path)


class TestAnalyseRealDump(unittest.TestCase):
    def setUp(self):
        self.p = gen_config.load_profile(gen_config.DEFAULT_PROFILE)

    def test_real_state_differences_and_blocks(self):
        r = odrive_status.analyse(real_values(), self.p)
        lines = r["lines"]
        diffs = "\n".join(t for lvl, t in lines if lvl == "DIFF")
        for path in ("current_lim", "vel_limit", "control_mode", "input_mode", "enable_watchdog",
                     "watchdog_timeout", "dc_bus_overvoltage_trip_level", "dc_max_positive_current",
                     "calibration_current", "startup_closed_loop_control"):
            self.assertIn(path, diffs)
        self.assertNotIn("dc_max_negative_current", diffs)     # profile now = saved -3.0
        self.assertNotIn("brake_resistance", diffs)
        errors = "\n".join(t for lvl, t in lines if lvl == "ERROR")
        self.assertIn("ENCODER_FAILED", errors)
        self.assertIn("startup_closed_loop_control = True", errors)
        self.assertFalse(any(lvl == "WARN" and "NOT PRESENT" in t for lvl, t in lines))
        self.assertFalse(r["ffb_ready"])
        blocks = [t for ok, t in odrive_status.precheck(real_values(), self.p) if not ok]
        self.assertEqual(len(blocks), 9 + 1, blocks)            # + the profile-TBD gate

    def test_ready_state_passes_only_with_measured_profile(self):
        v = ready_values(self.p)
        failing = [t for ok, t in odrive_status.precheck(v, self.p) if not ok]
        self.assertEqual(len(failing), 1, failing)
        self.assertIn("TBD", failing[0])
        r = odrive_status.analyse(v, measured(self.p))
        self.assertTrue(r["ffb_ready"])
        self.assertEqual([t for lvl, t in r["lines"] if lvl == "DIFF"], [])

    def test_each_precheck_condition_can_block(self):
        base = ready_values(self.p)
        mutations = {
            "axis0.config.enable_watchdog": False, "axis0.config.watchdog_timeout": 2.0,
            "axis0.controller.config.control_mode": 3, "axis0.motor.config.current_lim": 12.0,
            "axis0.controller.config.vel_limit": 50.0, "axis0.motor.is_calibrated": False,
            "axis0.encoder.is_ready": False, "axis0.motor.error": 1, "vbus_voltage": 31.0,
            "axis0.motor.config.torque_constant": 0.0, "axis0.current_state": 8,
        }
        for k, bad in mutations.items():
            v = dict(base)
            v[k] = bad
            n_fail = sum(1 for ok, _ in odrive_status.precheck(v, self.p) if not ok)
            self.assertEqual(n_fail, 2, k)   # the mutated one + the profile-TBD gate

    def test_live_reader_is_read_only(self):
        odrv = FakeODrive(real_values())
        got = odrive_status.read_live(odrv, ["vbus_voltage", "axis0.error", "axis0.does_not_exist"])
        self.assertEqual(got, {"vbus_voltage": 24.18603515625, "axis0.error": 256})
        self.assertEqual(odrv.writes, [])
        self.assertEqual(odrv.calls, [])

    def test_cli(self):
        with redirect_stdout(io.StringIO()) as buf:
            rc = odrive_status.main(["--fixture", DUMP])
        self.assertEqual(rc, 1)
        out = buf.getvalue()
        self.assertIn("(211 values)", out)
        self.assertIn("FFB precheck would FAIL", out)
        self.assertNotIn("PARSE WARN", out)
        with redirect_stdout(io.StringIO()) as buf:
            self.assertEqual(odrive_status.main([]), 1)      # defaults to the committed dump
        self.assertIn("odrive_dump.txt", buf.getvalue())
        with redirect_stdout(io.StringIO()):
            self.assertEqual(odrive_status.main(["--manual"]), 0)
            self.assertEqual(odrive_status.main(["--fixture", "/nonexistent/dump.txt"]), 2)
        with tempfile.TemporaryDirectory() as d:
            bad = os.path.join(d, "bad.txt")
            with open(bad, "w") as f:
                f.write(real_text().replace("vel_limit: 50.0 (float)", "vel_limit: 50.0 (flo"))
            with redirect_stdout(io.StringIO()) as buf:
                odrive_status.main(["--fixture", bad])
            self.assertIn("PARSE WARN", buf.getvalue())
            empty = os.path.join(d, "empty.txt")
            open(empty, "w").close()
            with redirect_stdout(io.StringIO()):
                self.assertEqual(odrive_status.main(["--fixture", empty]), 2)


if __name__ == "__main__":
    unittest.main()
