"""tools/session_start.py against a fake ODrive (no hardware, no waiting)."""
import unittest

import gen_config
import session_start
from tests.fake_odrive import FakeODrive
from tests.test_odrive_status import measured, ready_values


def make_odrive(profile, overrides=None, calibrates=True, clear_works=True, watchdog_leftover=True):
    v = ready_values(profile)
    v.update(overrides or {})
    v["axis0.requested_state"] = 1
    state = {}

    def hook(path, value):
        if path == "axis0.requested_state" and value == 7:
            state["calibrated"] = True
            odrv.values["axis0.current_state"] = 1               # the calibration is over at once
            odrv.values["axis0.encoder.is_ready"] = calibrates
            if not calibrates:
                odrv.values["axis0.encoder.error"] = 2           # CPR_POLEPAIRS_MISMATCH
        if path == "axis0.config.enable_watchdog" and value is True and watchdog_leftover:
            odrv.values["axis0.error"] = 0x800                   # nothing fed it yet

    def clear():
        if clear_works:
            for p in ("axis0.error", "axis0.motor.error", "axis0.encoder.error", "axis0.controller.error"):
                odrv.values[p] = 0

    odrv = FakeODrive(v, on_write=hook)
    odrv.values["axis0.clear_errors"] = clear
    return odrv


def go(odrv, profile, answer="I AM CLEAR", **kw):
    out = []
    rc = session_start.run(odrv, profile, out=out.append, sleep=lambda s: None, confirm=lambda prompt: answer, **kw)
    return rc, "\n".join(out)


def written(odrv, path):
    return [v for p, v in odrv.writes if p == path]


class TestSessionStart(unittest.TestCase):
    def setUp(self):
        self.p = measured(gen_config.load_profile(gen_config.DEFAULT_PROFILE))
        self.cal_i = gen_config.val(self.p, "odrive", "encoder_cal_current")
        self.cal_lim = gen_config.val(self.p, "odrive", "encoder_cal_current_lim")

    def fresh_power_up(self):
        return make_odrive(self.p, {"axis0.encoder.is_ready": False, "axis0.motor.config.current_lim": 6.0,
                                    "axis0.motor.config.calibration_current": 5.0,
                                    "axis0.config.enable_watchdog": False, "axis0.config.watchdog_timeout": 0.0,
                                    "axis0.controller.config.vel_limit": 50.0,
                                    "axis0.controller.config.vel_gain": 0.02})

    def test_fresh_power_up_calibrates_then_applies_the_profile(self):
        odrv = self.fresh_power_up()
        rc, text = go(odrv, self.p)
        self.assertEqual(rc, 0, text)
        self.assertIn("READY", text)
        self.assertEqual(written(odrv, "axis0.requested_state"), [7])           # one calibration, nothing else
        lim = [v for p, v in odrv.writes if p == "axis0.motor.config.current_lim"]
        self.assertEqual(lim, [self.cal_lim, gen_config.val(self.p, "odrive", "current_lim")])   # raised, then restored
        cal = written(odrv, "axis0.motor.config.calibration_current")
        self.assertEqual(cal[0], self.cal_i)
        self.assertEqual(cal[-1], gen_config.val(self.p, "odrive", "calibration_current"))
        order = [p for p, _ in odrv.writes]
        self.assertLess(order.index("axis0.requested_state"), order.index("axis0.controller.config.vel_gain"))
        self.assertEqual(odrv.values["axis0.controller.config.vel_gain"], 0.4)
        self.assertEqual(odrv.values["axis0.error"], 0)                       # the 0x800 leftover was cleared
        self.assertEqual(odrv.calls, [])                                      # never saved, never rebooted

    def test_motor_never_moves_without_the_typed_confirmation(self):
        odrv = self.fresh_power_up()
        rc, text = go(odrv, self.p, answer="yes")
        self.assertEqual(rc, 1)
        self.assertEqual(odrv.writes, [])

    def test_already_calibrated_is_not_calibrated_again_and_a_watchdog_leftover_is_cleared(self):
        odrv = make_odrive(self.p, {"axis0.error": 0x800})
        rc, text = go(odrv, self.p)
        self.assertEqual(rc, 0, text)
        self.assertEqual(written(odrv, "axis0.requested_state"), [])
        self.assertIn("already calibrated", text)

    def test_other_errors_are_shown_and_not_cleared(self):
        odrv = make_odrive(self.p, {"axis0.controller.error": 1, "axis0.error": 0x200 | 0x800})
        rc, text = go(odrv, self.p)
        self.assertEqual(rc, 3)
        self.assertIn("OVERSPEED", text)
        self.assertIn("CONTROLLER_FAILED", text)
        self.assertEqual(odrv.writes, [])
        self.assertEqual(odrv.values["axis0.controller.error"], 1)            # untouched: it may be the cause of a fault
        rc2, text2 = go(odrv, self.p, force_clear=True)                       # after a human has read it
        self.assertEqual(rc2, 0, text2)

    def test_errors_that_come_back_mean_the_stm32_is_not_feeding(self):
        odrv = make_odrive(self.p, {"axis0.error": 0x800}, clear_works=False)
        rc, text = go(odrv, self.p)
        self.assertEqual(rc, 4)
        self.assertIn("STM32", text)
        self.assertEqual(written(odrv, "axis0.requested_state"), [])

    def test_calibration_failure_stops_before_the_profile(self):
        odrv = make_odrive(self.p, {"axis0.encoder.is_ready": False}, calibrates=False)
        rc, text = go(odrv, self.p)
        self.assertEqual(rc, 5)
        self.assertIn("CPR_POLEPAIRS_MISMATCH", text)
        self.assertEqual(written(odrv, "axis0.config.enable_watchdog"), [])   # the profile was not applied

    def test_refuses_when_not_idle_or_supply_out_of_range(self):
        odrv = make_odrive(self.p, {"axis0.current_state": 8})
        rc, text = go(odrv, self.p)
        self.assertEqual(rc, 2)
        self.assertEqual(odrv.writes, [])
        odrv = make_odrive(self.p, {"vbus_voltage": 31.0})
        rc, text = go(odrv, self.p)
        self.assertEqual(rc, 2)
        self.assertEqual(odrv.writes, [])

    def test_skip_calibration_leaves_the_motor_alone_and_reports_not_ready(self):
        odrv = make_odrive(self.p, {"axis0.encoder.is_ready": False})
        rc, text = go(odrv, self.p, skip_calibration=True)
        self.assertEqual(written(odrv, "axis0.requested_state"), [])
        self.assertEqual(rc, 7)
        self.assertIn("encoder.is_ready", text)

    def test_profile_entries_for_the_calibration(self):
        self.assertLessEqual(self.cal_i, self.cal_lim)
        bad = gen_config.load_profile(gen_config.DEFAULT_PROFILE)
        bad["odrive"]["encoder_cal_current"]["value"] = 7.0
        errors, _ = gen_config.validate(bad)
        self.assertTrue(any("encoder_cal_current" in e for e in errors), errors)


if __name__ == "__main__":
    unittest.main()
