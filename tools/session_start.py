#!/usr/bin/env python3
"""Start an ODrive session (after an ODrive power-up or an STM32 restart): errors, calibration, profile, verdict.

    ~/odrive-env/bin/python tools/session_start.py

Close odrivetool first (it holds the USB interface). The STM32 must be running (it feeds the ODrive watchdog).

What it does, in order (it asks before the motor moves):
 1. reads the ODrive (it must be IDLE and the supply inside the window) and the four error registers WITH NAMES;
 2. clears the errors ONLY if the sole one is WATCHDOG_TIMER_EXPIRED (0x800, the normal leftover of an STM32 restart).
    Any other error is printed and the script stops: read it, then run again with --force-clear;
 3. waits and checks that the errors stay 0 (they come back at once when the STM32 is not feeding the watchdog);
 4. if the encoder is not ready (or --force-calibrate): the encoder offset calibration. THE MOTOR MOVES about 180 deg
    and back and you must type I AM CLEAR first. It temporarily sets motor current_lim / calibration_current to the profile's
    encoder_cal_* values (3 A and 4 A fail on this rotor, 5 A with 6 A pass);
 5. applies the gentle profile (RAM only, never saved) and reads every value back;
 6. clears the 0x800 that enabling the watchdog can leave and checks that it stays 0;
 7. prints the same pre-arm verdict as tools/odrive_status.py --live.
It never saves the configuration, never reboots, never requests closed loop.
"""
import argparse
import sys
import time

import gen_config
import odrive_enums
import odrive_status

STATE_IDLE = 1
STATE_ENCODER_OFFSET_CALIBRATION = 7
WATCHDOG_EXPIRED = 0x800
CALIBRATION_TIMEOUT_S = 40.0


def _get(odrv, path):
    obj = odrv
    for part in path.split("."):
        obj = getattr(obj, part)
    return obj


def _has(odrv, path):
    try:
        _get(odrv, path)
        return True
    except AttributeError:
        return False


def _set(odrv, path, value):
    parts = path.split(".")
    obj = odrv
    for part in parts[:-1]:
        obj = getattr(obj, part)
    setattr(obj, parts[-1], value)


def _same(read_back, wanted):
    if isinstance(wanted, bool) or isinstance(read_back, bool):
        return bool(read_back) == bool(wanted)
    try:
        return abs(float(read_back) - float(wanted)) <= 1e-5 * max(1.0, abs(float(wanted)))
    except (TypeError, ValueError):
        return read_back == wanted


def run(odrv, profile, out=print, sleep=time.sleep, confirm=input,
        force_clear=False, force_calibrate=False, skip_calibration=False):
    """Returns 0 when the ODrive ends ready for the FFB pre-arm checks, otherwise a non-zero reason code."""
    o = lambda k: gen_config.val(profile, "odrive", k)  # noqa: E731
    f = lambda k: gen_config.val(profile, "ffb", k)     # noqa: E731
    axis_index = int(o("axis"))
    ax = "axis%d" % axis_index
    err_paths = [ax + ".error", ax + ".motor.error", ax + ".encoder.error", ax + ".controller.error"]

    def read_errors():
        return {p: int(_get(odrv, p)) for p in err_paths}

    def show(errors):
        for p, v in errors.items():
            names = ", ".join(odrive_enums.decode_register(p, v)) if v else "none"
            out("   %-24s = 0x%X (%s)" % (p, v, names))

    def only_watchdog(errors):
        return (all(v == 0 for p, v in errors.items() if p != ax + ".error")
                and errors[ax + ".error"] in (0, WATCHDOG_EXPIRED))

    def clear_and_check(label):
        _get(odrv, ax + ".clear_errors")()
        sleep(2.0)
        again = read_errors()
        if any(again.values()):
            out("%s: the errors CAME BACK:" % label)
            show(again)
            out("   Most likely the STM32 is not feeding the ODrive watchdog (not running, in FAULT, or its USB cable is")
            out("   unplugged). Power-cycle the STM32 (unplug its USB for 3 s) and run this script again.")
            return False
        return True

    odrive_status.PROFILE_CACHE[:] = [profile]

    # 1. look first
    state = int(_get(odrv, ax + ".current_state"))
    if state != STATE_IDLE:
        out("STOP: %s.current_state is %d, not IDLE (1). Disarm (B1 once) and run again." % (ax, state))
        return 2
    vbus = float(_get(odrv, "vbus_voltage"))
    if not (f("vbus_min_v") <= vbus <= f("vbus_max_v")):
        out("STOP: vbus_voltage %.2f V is outside %.1f..%.1f V." % (vbus, f("vbus_min_v"), f("vbus_max_v")))
        return 2
    out("1. ODrive is IDLE, supply %.2f V. Error registers:" % vbus)
    errors = read_errors()
    show(errors)
    if any(errors.values()) and not only_watchdog(errors) and not force_clear:
        out("STOP: errors other than the watchdog leftover. Read them above (copy them for the log), then run again with")
        out("   --force-clear if you want them cleared. They are NOT cleared automatically: they may be the cause of a fault.")
        return 3

    # 2. clear
    if any(errors.values()):
        out("2. clearing the errors ...")
        if not clear_and_check("after clearing"):
            return 4
    else:
        out("2. no errors to clear.")

    # 3. calibration
    ready = bool(_get(odrv, ax + ".encoder.is_ready"))
    if skip_calibration:
        out("3. calibration skipped (--skip-calibration).")
    elif ready and not force_calibrate:
        out("3. the encoder is already calibrated: skipped (use --force-calibrate to redo it).")
    else:
        out("3. ENCODER OFFSET CALIBRATION: THE MOTOR WILL ROTATE about 180 deg and back (about 10 s).")
        out("   Housing free, hands, cables and tools clear, hand next to the 24 V e-stop. Ctrl+C requests IDLE.")
        if confirm("   Type 'I AM CLEAR' to let the motor rotate: ").strip() != "I AM CLEAR":
            out("not confirmed: nothing was changed.")
            return 1
        _set(odrv, ax + ".motor.config.current_lim", float(o("encoder_cal_current_lim")))
        _set(odrv, ax + ".motor.config.calibration_current", float(o("encoder_cal_current")))
        try:
            _set(odrv, ax + ".requested_state", STATE_ENCODER_OFFSET_CALIBRATION)
            sleep(0.5)
            waited = 0.0
            while int(_get(odrv, ax + ".current_state")) != STATE_IDLE and waited < CALIBRATION_TIMEOUT_S:
                sleep(0.2)
                waited += 0.2
        except KeyboardInterrupt:
            _set(odrv, ax + ".requested_state", STATE_IDLE)
            out("interrupted: IDLE requested.")
            return 3
        if int(_get(odrv, ax + ".current_state")) != STATE_IDLE:
            _set(odrv, ax + ".requested_state", STATE_IDLE)
            out("the calibration did not finish in %.0f s: IDLE requested." % CALIBRATION_TIMEOUT_S)
            return 5
        errors = read_errors()
        if any(errors.values()) or not bool(_get(odrv, ax + ".encoder.is_ready")):
            out("calibration FAILED:")
            show(errors)
            out("   (CPR_POLEPAIRS_MISMATCH: too little calibration current, a wrong cpr or pole_pairs, or a slipping gear.)")
            return 5
        out("   calibrated: encoder.is_ready = True, errors 0.")

    # 4. profile
    out("4. applying the profile to the ODrive RAM (never saved) ...")
    bad = 0
    for path, wanted in gen_config.setup_settings(profile):
        path = path.replace("axis0", ax)
        if not _has(odrv, path):
            out("   SKIP %s (not present on this firmware build)" % path)
            continue
        _set(odrv, path, wanted)
        got = _get(odrv, path)
        if not _same(got, wanted):
            bad += 1
            out("   MISMATCH %s: wrote %r, read back %r" % (path, wanted, got))
    if bad:
        out("%d value(s) did not read back as written." % bad)
        return 6

    # 5. the watchdog leftover
    sleep(1.0)
    errors = read_errors()
    if any(errors.values()):
        if not only_watchdog(errors):
            out("5. unexpected errors after applying the profile:")
            show(errors)
            return 3
        out("5. the watchdog leftover (0x800) after enabling the watchdog: clearing it ...")
        if not clear_and_check("after the profile"):
            return 4
    else:
        out("5. no errors after the profile.")

    # 6. verdict
    values = odrive_status.read_live(odrv, odrive_status.live_paths(axis_index))
    checks = odrive_status.precheck(values, profile)
    out("6. FFB pre-arm checks (what the firmware will test):")
    for ok, text in checks:
        out("   %s %s" % ("ok   " if ok else "BLOCK", text))
    if all(ok for ok, _ in checks):
        out("READY: the ODrive is ready for the arm gesture.")
        return 0
    out("NOT READY: see the BLOCK lines.")
    return 7


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", default=gen_config.DEFAULT_PROFILE)
    ap.add_argument("--force-clear", action="store_true",
                    help="also clear errors other than the watchdog leftover (read them first)")
    ap.add_argument("--force-calibrate", action="store_true", help="calibrate even if encoder.is_ready is True")
    ap.add_argument("--skip-calibration", action="store_true", help="never calibrate (the motor does not move)")
    a = ap.parse_args(argv)
    try:
        profile = gen_config.load_profile(a.profile)
    except (gen_config.ProfileError, OSError) as e:
        print("cannot load the profile: %s" % e)
        return 2
    try:
        import odrive  # noqa: PLC0415 - only the real run needs it
    except ImportError:
        print("The 'odrive' Python package is missing: run this with ~/odrive-env/bin/python")
        return 2
    print("Searching for an ODrive over USB (close odrivetool first) ...")
    odrv = odrive.find_any(timeout=15)
    return run(odrv, profile, force_clear=a.force_clear, force_calibrate=a.force_calibrate,
               skip_calibration=a.skip_calibration)


if __name__ == "__main__":
    sys.exit(main())
