#!/usr/bin/env python3
"""ODrive "doctor" - READ-ONLY status check against config/hardware_profile.toml.

    python3 tools/odrive_status.py --fixture dump.txt   # analyse a saved dump (no hardware)
    python3 tools/odrive_status.py --live               # read a connected ODrive over USB
                                                        #   (needs the official 'odrive' package)
    python3 tools/odrive_status.py --live --dump-out d.txt   # also save what was read
    python3 tools/odrive_status.py --manual             # print odrivetool commands to make a dump

It prints the error registers decoded to names, vbus_voltage, encoder counts,
the startup flags, compares the live configuration with the profile (every
difference is listed), and evaluates the same pre-arm conditions the FFB
firmware checks. It never writes anything: only attribute reads.

Dump formats accepted by --fixture (see tools/odrive_dump.py):
  * the odrivetool/IPython session format of docs/odrive_dump.txt
    ("In [n]: odrv0.axis0" blocks, "name: value (type)" lines);
  * flat:  "axis0.motor.config.current_lim = 12.0"  (optionally prefixed "odrv0.");
  * older trees with "name = value (type)"; --prefix axis0 for a tree printed
    from odrv0.axis0 without an "In [n]:" line.
Exit code: 0 = all good, 1 = problems/differences found, 2 = could not read.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import odrive_enums  # noqa: E402
import gen_config    # noqa: E402
from odrive_dump import parse_dump, parse_dump_report, parse_value  # noqa: E402,F401

DEFAULT_DUMP = os.path.join(os.path.dirname(HERE), "docs", "odrive_dump.txt")

# Paths read in live mode (relative to odrv0); axis0 is replaced by the profile axis.
# All of them exist in docs/odrive_dump.txt (checked by tools/tests/test_odrive_status.py).
READ_PATHS = [
    "vbus_voltage", "fw_version_major", "fw_version_minor", "fw_version_revision",
    "fw_version_unreleased", "hw_version_major", "hw_version_minor", "hw_version_variant",
    "brake_resistor_armed", "brake_resistor_saturated", "ibus",
    "axis0.error", "axis0.motor.error", "axis0.encoder.error", "axis0.controller.error",
    "axis0.current_state", "axis0.motor.is_calibrated", "axis0.encoder.is_ready",
    "axis0.encoder.pos_estimate", "axis0.encoder.shadow_count", "axis0.encoder.vel_estimate",
    "axis0.encoder.hall_state", "axis0.encoder.config.offset",
    "axis0.motor.config.phase_resistance", "axis0.motor.config.phase_inductance",
    "axis0.motor.config.pre_calibrated", "axis0.motor.config.direction",
    "axis0.encoder.config.pre_calibrated", "axis0.motor.effective_current_lim",
    "axis0.config.startup_motor_calibration", "axis0.config.startup_encoder_index_search",
    "axis0.config.startup_encoder_offset_calibration", "axis0.config.startup_closed_loop_control",
    "axis0.config.startup_homing", "axis0.config.startup_sensorless_control",
    "axis0.controller.config.pos_gain", "axis0.controller.config.vel_gain",
    "axis0.controller.config.vel_integrator_gain", "axis0.motor.config.torque_lim",
    "axis0.controller.config.enable_vel_limit", "axis0.controller.config.vel_limit_tolerance",
    "config.enable_uart", "config.uart_baudrate", "config.enable_ascii_protocol_on_usb",
]
# Present in other 0.5.x builds only; read if they exist (live mode skips missing ones).
OPTIONAL_PATHS = ["error", "config.enable_brake_resistor", "config.enable_uart_a", "config.uart_a_baudrate"]

MANUAL = """\
Without the 'odrive' Python package, make a dump by hand (READ only):
  1. odrivetool                       (on the PC connected to the ODrive's USB port)
  2. Type these, one after the other, and copy the whole session text into a file:
       odrv0
       odrv0.config
       odrv0.axis0
       odrv0.axis0.controller.config
       odrv0.axis0.motor.config
       odrv0.axis0.encoder.config
  3. python3 tools/odrive_status.py --fixture <file>
     (the "In [n]: odrv0.xxx" lines tell the parser where each block belongs;
      docs/odrive_dump.txt is an example made exactly this way.)
Do not assign anything while making the dump.
"""


# ----------------------------------------------------------------------------
# Live reading (read-only)
# ----------------------------------------------------------------------------
def read_live(odrv, paths):
    out = {}
    for path in paths:
        obj = odrv
        try:
            for part in path.split("."):
                obj = getattr(obj, part)
        except AttributeError:
            continue
        if callable(obj):
            continue
        out[path] = obj
    return out


def live_paths(axis):
    settings = [p for p, _ in gen_config.setup_settings(PROFILE_CACHE[0])] if PROFILE_CACHE else []
    paths = []
    for p in READ_PATHS + OPTIONAL_PATHS + settings:
        p = p.replace("axis0", "axis%d" % axis)
        if p not in paths:
            paths.append(p)
    return paths


PROFILE_CACHE = []


# ----------------------------------------------------------------------------
# Analysis
# ----------------------------------------------------------------------------
def _close(a, b):
    if isinstance(b, bool) or isinstance(a, bool):
        return bool(a) == bool(b)
    if isinstance(b, float) or isinstance(a, float):
        try:
            return abs(float(a) - float(b)) <= 1e-4 * max(1.0, abs(float(b)))
        except (TypeError, ValueError):
            return False
    return a == b


def analyse(values, profile):
    """Return a report dict: sections of (level, text) lines and a problem count."""
    ax = gen_config.val(profile, "odrive", "axis")
    A = "axis%d" % ax
    lines = []
    problems = 0

    def add(level, text):
        nonlocal problems
        lines.append((level, text))
        if level in ("ERROR", "DIFF", "WARN"):
            problems += 1

    def get(path, default=None):
        return values.get(path.replace("axis0", A), default)

    # -- firmware
    if "fw_version_major" in values:
        add("INFO", "firmware %s.%s.%s%s, hardware %s.%s variant %s" % (
            values.get("fw_version_major"), values.get("fw_version_minor"), values.get("fw_version_revision"),
            " (unreleased dev build)" if values.get("fw_version_unreleased") else "",
            values.get("hw_version_major"), values.get("hw_version_minor"), values.get("hw_version_variant")))
    add("INFO", "API generation (from the property names): %s" % odrive_enums.firmware_family(values))
    cm, im = get("axis0.controller.config.control_mode"), get("axis0.controller.config.input_mode")
    if cm is not None or im is not None:
        add("INFO", "control_mode = %r (%s), input_mode = %r (%s)" % (
            cm, odrive_enums.CONTROL_MODES.get(cm, "?"), im, odrive_enums.INPUT_MODES.get(im, "?")))
    if "config.enable_uart" in values:
        add("INFO", "UART enabled = %r, baud = %r (profile uart.baud = %r)" % (
            values.get("config.enable_uart"), values.get("config.uart_baudrate"),
            gen_config.val(profile, "uart", "baud")))
        if values.get("config.uart_baudrate") not in (None, gen_config.val(profile, "uart", "baud")):
            add("ERROR", "UART baud differs from the profile: the STM32 will not understand the ODrive")
    if "brake_resistor_armed" in values or "config.brake_resistance" in values:
        add("INFO", "brake resistor armed = %r, brake_resistance = %r ohm, max_regen_current = %r A" % (
            values.get("brake_resistor_armed"), values.get("config.brake_resistance"),
            values.get("config.max_regen_current")))
    # -- errors
    lines.append(("HEAD", "Error registers"))
    for reg in ("error", "axis0.error", "axis0.motor.error", "axis0.encoder.error", "axis0.controller.error"):
        v = get(reg)
        if v is None:
            add("INFO", "%-24s (not in dump)" % reg.replace("axis0", A))
            continue
        try:
            iv = int(v)
        except (TypeError, ValueError):
            add("WARN", "%-24s unreadable value %r" % (reg, v))
            continue
        names = odrive_enums.decode_register(reg, iv)
        if iv == 0:
            add("OK", "%-24s 0x0" % reg.replace("axis0", A))
        else:
            add("ERROR", "%-24s 0x%X  %s" % (reg.replace("axis0", A), iv, ", ".join(names)))
            if "CPR_POLEPAIRS_MISMATCH" in names:
                add("HINT", "  cpr does not match the belt ratio/pole pairs: re-count the teeth and run "
                            "tools/measure_counts.py")
            if "WATCHDOG_TIMER_EXPIRED" in names:
                add("HINT", "  the axis watchdog expired (nothing fed it). If this appears while IDLE, your "
                            "0.5.x build checks the watchdog in IDLE: see docs/HARDWARE_BRINGUP.md step F3")

    # -- state / power / encoder
    lines.append(("HEAD", "State"))
    st = get("axis0.current_state")
    if st is not None:
        name = odrive_enums.AXIS_STATES.get(int(st), "?")
        add("OK" if int(st) == odrive_enums.AXIS_STATE_IDLE else "WARN", "current_state = %s (%s)" % (st, name))
    vbus = get("vbus_voltage")
    vmin, vmax = gen_config.val(profile, "ffb", "vbus_min_v"), gen_config.val(profile, "ffb", "vbus_max_v")
    if vbus is not None:
        ok = vmin <= float(vbus) <= vmax
        add("OK" if ok else "WARN", "vbus_voltage = %.2f V (window %.1f..%.1f V)" % (float(vbus), vmin, vmax))
    for path in ("axis0.encoder.shadow_count", "axis0.encoder.pos_estimate", "axis0.encoder.vel_estimate",
                 "axis0.motor.is_calibrated", "axis0.encoder.is_ready", "axis0.motor.config.phase_resistance",
                 "axis0.motor.config.pre_calibrated", "axis0.encoder.config.pre_calibrated",
                 "axis0.motor.config.direction", "axis0.encoder.hall_state"):
        v = get(path)
        if v is not None:
            add("INFO", "%s = %r" % (path.replace("axis0", A), v))

    # -- startup flags
    lines.append(("HEAD", "Startup flags (must all be False)"))
    for flag in ("startup_motor_calibration", "startup_encoder_index_search",
                 "startup_encoder_offset_calibration", "startup_closed_loop_control", "startup_homing",
                 "startup_sensorless_control"):
        v = get("axis0.config." + flag)
        if v is None:
            continue
        add("OK" if v is False else "ERROR", "%s.config.%s = %r" % (A, flag, v))
        if v is True and flag == "startup_closed_loop_control":
            add("HINT", "  the ODrive engages CLOSED LOOP by itself at every power-up (with the saved "
                        "control mode and gains). Set it False in odrivetool and save before anything else.")

    # -- profile comparison
    lines.append(("HEAD", "Configuration vs config/hardware_profile.toml"))
    ndiff = 0
    entries = [(p, v, "") for p, v in gen_config.setup_settings(profile)]
    entries += [(p, v, " [check only, never written]") for p, v in gen_config.check_only_settings(profile)]
    for path, want, note in entries:
        have = values.get(path)
        if have is None:
            add("WARN", "%-55s NOT PRESENT in this firmware/dump (profile: %r)%s" % (path, want, note))
            continue
        if _close(have, want):
            add("OK", "%-55s %r%s" % (path, have, note))
        else:
            ndiff += 1
            add("DIFF", "%-55s live %r, profile %r%s" % (path, have, want, note))
    if ndiff:
        add("HINT", "%d difference(s): python3 tools/generated/odrive_setup.py --read shows the same; "
                    "--apply writes the profile (a human decides)" % ndiff)

    # -- FFB pre-arm conditions (same as Core/Src/ffb/ffb_precheck.c)
    lines.append(("HEAD", "FFB pre-arm conditions (firmware precheck)"))
    pre = precheck(values, profile)
    for ok, text in pre:
        lines.append(("OK" if ok else "BLOCK", text))
    ready = all(ok for ok, _ in pre)
    lines.append(("HEAD", "Verdict"))
    lines.append(("OK" if ready else "BLOCK",
                  "FFB precheck would PASS" if ready else "FFB precheck would FAIL (see BLOCK lines)"))
    return {"lines": lines, "problems": problems, "ffb_ready": ready}


def precheck(values, profile):
    """Mirror of the firmware pre-arm checks. Returns [(ok, text)]."""
    ax = gen_config.val(profile, "odrive", "axis")
    A = "axis%d" % ax
    o = lambda k: gen_config.val(profile, "odrive", k)  # noqa: E731
    f = lambda k: gen_config.val(profile, "ffb", k)     # noqa: E731
    g = lambda p: values.get(p.replace("axis0", A))     # noqa: E731
    res = []

    def chk(cond, text):
        res.append((bool(cond), text))

    def num(p):
        v = g(p)
        try:
            return float(v)
        except (TypeError, ValueError):
            return None

    wd = g("axis0.config.enable_watchdog")
    chk(wd is True or wd == 1, "enable_watchdog is True (live %r)" % (wd,))
    t = num("axis0.config.watchdog_timeout")
    chk(t is not None and f("watchdog_timeout_min_s") <= t <= f("watchdog_timeout_max_s"),
        "watchdog_timeout in [%.2f, %.2f] s (live %r)" % (f("watchdog_timeout_min_s"), f("watchdog_timeout_max_s"), t))
    cm = g("axis0.controller.config.control_mode")
    chk(cm == odrive_enums.CONTROL_MODE_TORQUE_CONTROL, "control_mode = 1 TORQUE_CONTROL (live %r)" % (cm,))
    cl = num("axis0.motor.config.current_lim")
    chk(cl is not None and cl <= o("current_lim") + 1e-6, "current_lim <= %.2f A (live %r)" % (o("current_lim"), cl))
    vl = num("axis0.controller.config.vel_limit")
    chk(vl is not None and vl <= o("vel_limit") + 1e-6, "vel_limit <= %.2f turns/s (live %r)" % (o("vel_limit"), vl))
    kt = num("axis0.motor.config.torque_constant")
    chk(kt is not None and 0.001 <= kt <= 10.0, "torque_constant sane (live %r)" % (kt,))
    chk(g("axis0.motor.is_calibrated") in (True, 1), "motor.is_calibrated (live %r)" % (g("axis0.motor.is_calibrated"),))
    chk(g("axis0.encoder.is_ready") in (True, 1), "encoder.is_ready (live %r)" % (g("axis0.encoder.is_ready"),))
    for reg in ("axis0.error", "axis0.motor.error", "axis0.encoder.error"):
        v = g(reg)
        chk(v == 0, "%s == 0 (live %r)" % (reg.replace("axis0", A), v))
    st = g("axis0.current_state")
    chk(st == odrive_enums.AXIS_STATE_IDLE, "current_state == 1 IDLE before arming (live %r)" % (st,))
    vb = num("vbus_voltage")
    chk(vb is not None and f("vbus_min_v") <= vb <= f("vbus_max_v"),
        "vbus_voltage in [%.1f, %.1f] V (live %r)" % (f("vbus_min_v"), f("vbus_max_v"), vb))
    tbd = gen_config.tbd_entries(profile, gen_config.SAFETY_SECTIONS)
    chk(not tbd, "profile safety entries measured (%d still TBD; firmware FFB_REQUIRE_MEASURED_PROFILE)" % len(tbd))
    return res


def render(report, out=print):
    marks = {"OK": "  ok ", "INFO": "     ", "WARN": " WARN", "ERROR": "ERROR", "DIFF": " DIFF",
             "HINT": "     ", "BLOCK": "BLOCK"}
    for level, text in report["lines"]:
        if level == "HEAD":
            out("")
            out("== %s ==" % text)
        else:
            out("%s %s" % (marks.get(level, level), text))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--fixture", action="append", help="saved dump file (repeatable)")
    src.add_argument("--live", action="store_true", help="read a connected ODrive (read-only)")
    src.add_argument("--manual", action="store_true", help="print how to make a dump by hand")
    ap.add_argument("--prefix", default="", help="path prefix for tree dumps, e.g. axis0")
    ap.add_argument("--profile", default=gen_config.DEFAULT_PROFILE)
    ap.add_argument("--dump-out", help="live mode: save the values read as a flat dump")
    a = ap.parse_args(argv)

    if a.manual:
        print(MANUAL)
        return 0
    if not (a.fixture or a.live):
        if os.path.exists(DEFAULT_DUMP):
            a.fixture = [DEFAULT_DUMP]          # the committed real dump
        else:
            print(MANUAL)
            return 2
    try:
        profile = gen_config.load_profile(a.profile)
    except (gen_config.ProfileError, OSError) as e:
        print("cannot load profile: %s" % e)
        return 2
    PROFILE_CACHE[:] = [profile]

    if a.fixture:
        values = {}
        problems = []
        for path in a.fixture:
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    v, p = parse_dump_report(fh.read(), a.prefix)
            except OSError as e:
                print("cannot read %s: %s" % (path, e))
                return 2
            values.update(v)
            problems += [(path,) + x for x in p]
        print("Source: %s (%d values)" % (", ".join(a.fixture), len(values)))
        for path, no, line, why in problems:
            print("PARSE WARN %s:%d: %s  [%s]" % (path, no, line.strip()[:80], why))
        if not values:
            print("No values could be read from the dump.")
            return 2
    else:
        try:
            import odrive  # optional, official package
        except ImportError:
            print("The 'odrive' package is not installed.\n")
            print(MANUAL)
            return 2
        print("Searching for an ODrive over USB (read-only) ...")
        odrv = odrive.find_any(timeout=15)
        values = read_live(odrv, live_paths(gen_config.val(profile, "odrive", "axis")))
        if a.dump_out:
            with open(a.dump_out, "w", encoding="utf-8") as fh:
                for k in sorted(values):
                    fh.write("%s = %r\n" % (k, values[k]))
            print("dump written to %s" % a.dump_out)
    report = analyse(values, profile)
    render(report)
    return 0 if report["problems"] == 0 and report["ffb_ready"] else 1


if __name__ == "__main__":
    sys.exit(main())
