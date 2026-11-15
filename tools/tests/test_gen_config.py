import copy
import io
import os
import types
import unittest
from contextlib import redirect_stdout, redirect_stderr

import gen_config
from tests.fake_odrive import FakeODrive

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")


def load():
    return gen_config.load_profile(gen_config.DEFAULT_PROFILE)


def set_val(p, section, key, value):
    p[section][key]["value"] = value


def module_from_source(name, src):
    mod = types.ModuleType(name)
    exec(compile(src, name, "exec"), mod.__dict__)
    return mod


def ready_values(p):
    """Values of a calibrated, IDLE ODrive with the profile applied."""
    vals = {path: value for path, value in gen_config.setup_settings(p)}
    vals.update({
        "vbus_voltage": 24.0, "error": 0, "axis0.error": 0, "axis0.motor.error": 0,
        "axis0.encoder.error": 0, "axis0.controller.error": 0, "axis0.current_state": 1,
        "axis0.requested_state": 0, "axis0.motor.is_calibrated": True, "axis0.encoder.is_ready": True,
        "axis0.motor.config.pre_calibrated": False,
    })
    return vals


class TestProfile(unittest.TestCase):
    def test_committed_profile_is_valid(self):
        errors, warnings = gen_config.validate(load())
        self.assertEqual(errors, [])

    def test_placeholders_reproduce_previous_firmware_values(self):
        d = gen_config.derive(load())
        self.assertEqual(d["cpr"], 2400)
        self.assertTrue(d["cpr_is_integer"])
        self.assertEqual(d["encoder_counts_per_wheel_rev"], 2400)
        self.assertEqual(d["odrive_turns_per_wheel_rev"], 1.0)
        h = gen_config.gen_header(load())
        for needle in ("#define ENCODER_COUNTS_PER_WHEEL_REV  2400", "#define WHEEL_LOCK_TO_LOCK_DEG        540",
                       "#define ODRIVE_TURNS_PER_WHEEL_REV    1.0f", "#define ODRIVE_UART_BAUD              115200U",
                       "#define MCP23017_I2C_ADDR_7BIT        0x20U", "#define ODRIVE_INVERT_DIRECTION       0"):
            self.assertIn(needle, h)

    def test_header_macros_are_guarded(self):
        lines = gen_config.gen_header(load()).splitlines()
        for i, line in enumerate(lines):
            if line.startswith("#define ") and not line.startswith(("#define WHEEL_CONFIG_GENERATED_H",
                                                                     "#define PROFILE_")):
                name = line.split()[1]
                self.assertEqual(lines[i - 1], "#ifndef " + name, name)

    def test_generated_files_are_up_to_date(self):
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(gen_config.main(["--check", "--quiet"]), 0)

    def test_non_integer_cpr_warns_loudly(self):
        p = load()
        set_val(p, "belt", "teeth_housing", 20)
        set_val(p, "belt", "teeth_pulley", 7)
        errors, warnings = gen_config.validate(p)
        self.assertEqual(errors, [])
        self.assertTrue(any("NOT an integer" in w for w in warnings), warnings)

    def test_integer_ratio_gives_scaled_cpr(self):
        p = load()
        set_val(p, "belt", "teeth_housing", 90)
        set_val(p, "belt", "teeth_pulley", 30)
        d = gen_config.derive(p)
        self.assertEqual(d["cpr"], 7200)
        self.assertEqual(d["encoder_counts_per_wheel_rev"], 7200)

    def test_calibration_current_above_current_lim_is_error(self):
        p = load()
        set_val(p, "odrive", "calibration_current", 5.0)
        errors, _ = gen_config.validate(p)
        self.assertTrue(any("calibration_current" in e for e in errors))

    def test_ffb_max_amps_above_hard_cap_is_error(self):
        p = load()
        set_val(p, "ffb", "max_amps", 1.5)
        errors, _ = gen_config.validate(p)
        self.assertTrue(any("ffb.max_amps" in e for e in errors))

    def test_range_and_type_errors(self):
        p = load()
        set_val(p, "odrive", "current_lim", 99.0)
        set_val(p, "belt", "teeth_pulley", 20.5)
        set_val(p, "wheel", "invert", 1)
        errors, _ = gen_config.validate(p)
        self.assertTrue(any("current_lim" in e for e in errors))
        self.assertTrue(any("teeth_pulley" in e for e in errors))
        self.assertTrue(any("wheel.invert" in e for e in errors))

    def test_watchdog_window_consistency(self):
        p = load()
        set_val(p, "odrive", "watchdog_timeout", 0.9)
        errors, _ = gen_config.validate(p)
        self.assertTrue(any("watchdog_timeout" in e for e in errors))

    def test_uart_load_limit(self):
        p = load()
        set_val(p, "ffb", "command_period_ms", 1)
        errors, _ = gen_config.validate(p)
        self.assertTrue(any("UART" in e for e in errors))
        set_val(p, "uart", "baud", 921600)
        set_val(p, "ffb", "stale_ms", 50)
        errors, _ = gen_config.validate(p)
        self.assertFalse(any("UART" in e for e in errors), errors)

    def test_require_measured_rejects_tbd(self):
        errors, _ = gen_config.validate(load(), require_measured=True)
        self.assertTrue(any("belt.teeth_housing is still TBD" in e for e in errors))
        p = load()
        for s in p:
            for k in p[s]:
                p[s][k]["status"] = "measured"
        errors, _ = gen_config.validate(p, require_measured=True)
        self.assertEqual(errors, [])

    def test_bad_status_is_rejected(self):
        with self.assertRaises(gen_config.ProfileError):
            gen_config.parse_profile({"belt": {"teeth_pulley": {"value": 1, "status": "maybe",
                                                                "how_to_measure": "x"}}})
        with self.assertRaises(gen_config.ProfileError):
            gen_config.parse_profile({"belt": {"teeth_pulley": 20}})

    def test_invert_flag_in_header(self):
        p = load()
        set_val(p, "wheel", "invert", True)
        h = gen_config.gen_header(p)
        self.assertIn("#define ODRIVE_INVERT_DIRECTION       1", h)
        self.assertIn("#define ENCODER_INVERT_DIRECTION      1", h)

    def test_tbd_counts_in_header(self):
        p = load()
        h = gen_config.gen_header(p)
        n = len(gen_config.tbd_entries(p))
        self.assertIn("#define PROFILE_TBD_COUNT             %dU" % n, h)


class TestSetupScript(unittest.TestCase):
    def setUp(self):
        self.p = load()
        self.mod = module_from_source("odrive_setup", gen_config.gen_setup_script(self.p))

    def test_dry_run_prints_commands_and_connects_to_nothing(self):
        def boom():
            raise AssertionError("dry run must not connect")
        self.mod.connect = boom
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertEqual(self.mod.main([]), 0)
        out = buf.getvalue()
        self.assertIn("DRY RUN", out)
        self.assertIn("odrv0.axis0.motor.config.current_lim = 4.0", out)

    def test_never_requests_state_or_motion(self):
        for path, _ in self.mod.SETTINGS:
            self.assertNotIn("requested_state", path)
            for token in ("input_torque", "input_pos", "input_vel"):
                self.assertNotIn(token, path)
            self.assertFalse(path.endswith("startup_closed_loop_control") and _ is True)

    def test_read_mode_writes_nothing(self):
        vals = ready_values(self.p)
        vals["axis0.motor.config.current_lim"] = 12.0
        odrv = FakeODrive(vals)
        out = []
        self.assertEqual(self.mod.apply(odrv, write=False, out=out.append), 0)
        self.assertEqual(odrv.writes, [])
        self.assertTrue(any("12.0 -> 4.0" in line for line in out))

    def test_apply_writes_only_differences_and_does_not_save(self):
        vals = ready_values(self.p)
        vals["axis0.motor.config.current_lim"] = 12.0
        vals["axis0.controller.config.vel_limit"] = 50.0
        odrv = FakeODrive(vals)
        self.assertEqual(self.mod.apply(odrv, write=True, out=lambda s: None), 0)
        self.assertEqual(sorted(odrv.writes), [("axis0.controller.config.vel_limit", 2.0),
                                               ("axis0.motor.config.current_lim", 4.0)])
        self.assertEqual(odrv.calls, [])

    def test_apply_save_only_with_flag(self):
        vals = ready_values(self.p)
        vals["axis0.motor.config.current_lim"] = 12.0
        odrv = FakeODrive(vals)
        self.mod.apply(odrv, write=True, save=True, out=lambda s: None)
        self.assertEqual(odrv.calls, ["save_configuration"])

    def test_apply_refused_when_not_idle_or_vbus_bad(self):
        for key, bad in (("axis0.current_state", 8), ("vbus_voltage", 40.0), ("vbus_voltage", 5.0)):
            vals = ready_values(self.p)
            vals["axis0.motor.config.current_lim"] = 12.0
            vals[key] = bad
            odrv = FakeODrive(vals)
            self.assertEqual(self.mod.apply(odrv, write=True, out=lambda s: None), 2, key)
            self.assertEqual(odrv.writes, [], key)

    def test_missing_paths_are_skipped(self):
        vals = ready_values(self.p)
        del vals["config.dc_max_negative_current"]
        vals["axis0.motor.config.current_lim"] = 12.0
        odrv = FakeODrive(vals)
        out = []
        self.assertEqual(self.mod.apply(odrv, write=True, out=out.append), 0)
        self.assertTrue(any("SKIP config.dc_max_negative_current" in line for line in out))

    def test_save_requires_apply(self):
        with redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.mod.main(["--save"])


class TestCalibrateScript(unittest.TestCase):
    def setUp(self):
        self.p = load()
        self.mod = module_from_source("odrive_calibrate", gen_config.gen_calibrate_script(self.p))

    def _odrv(self, **over):
        vals = ready_values(self.p)
        vals["axis0.motor.is_calibrated"] = False
        vals["axis0.encoder.is_ready"] = False
        vals.update(over)

        def on_write(path, value):
            # the fake axis finishes any requested calibration instantly
            if path == "axis0.requested_state":
                if value == 4:
                    odrv.values["axis0.motor.is_calibrated"] = True
                if value == 7:
                    odrv.values["axis0.encoder.is_ready"] = True
                odrv.values["axis0.current_state"] = 1
        odrv = FakeODrive(vals, on_write)
        return odrv

    def test_preflight_refuses_errors_and_loose_limits(self):
        self.assertEqual(self.mod.preflight(self._odrv()), [])
        probs = self.mod.preflight(self._odrv(**{"axis0.motor.error": 0x1, "axis0.motor.config.current_lim": 12.0}))
        self.assertTrue(any("PHASE_RESISTANCE_OUT_OF_RANGE" in p for p in probs))
        self.assertTrue(any("current_lim" in p for p in probs))
        probs = self.mod.preflight(self._odrv(**{"axis0.encoder.config.cpr": 8192}))
        self.assertTrue(any("cpr" in p for p in probs))

    def test_calibrate_sequence_motor_then_encoder_no_save(self):
        odrv = self._odrv()
        rc = self.mod.calibrate(odrv, save=False, out=lambda s: None, sleep=lambda s: None)
        self.assertEqual(rc, 0)
        self.assertEqual([w for w in odrv.writes if w[0] == "axis0.requested_state"],
                         [("axis0.requested_state", 4), ("axis0.requested_state", 7)])
        self.assertEqual(odrv.calls, [])

    def test_calibrate_skips_motor_if_calibrated_and_saves_with_flag(self):
        odrv = self._odrv(**{"axis0.motor.is_calibrated": True})
        rc = self.mod.calibrate(odrv, save=True, out=lambda s: None, sleep=lambda s: None)
        self.assertEqual(rc, 0)
        self.assertEqual([w for w in odrv.writes if w[0] == "axis0.requested_state"],
                         [("axis0.requested_state", 7)])
        self.assertEqual(odrv.calls, ["save_configuration"])

    def test_never_requests_closed_loop(self):
        src = gen_config.gen_calibrate_script(self.p)
        self.assertNotIn("requested_state = 8", src)
        src = gen_config.gen_setup_script(self.p)
        self.assertNotIn("requested_state", src)


if __name__ == "__main__":
    unittest.main()
