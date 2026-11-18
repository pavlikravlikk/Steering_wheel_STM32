#!/usr/bin/env python3
"""Generate firmware config and ODrive scripts from config/hardware_profile.toml.

    python3 tools/gen_config.py                  # validate + write all outputs
    python3 tools/gen_config.py --check          # exit 1 if outputs are out of date
    python3 tools/gen_config.py --require-measured   # refuse any status = "TBD"
    python3 tools/gen_config.py --derived        # print derived values only

Outputs (deterministic, no timestamps, safe to commit):
    steeringwheel_firmware/Core/Inc/wheel_config_generated.h
    tools/generated/odrive_setup.py       (dry run by default, never saves w/o --save)
    tools/generated/odrive_calibrate.py   (encoder offset calibration, asks first)

Standard library only (Python 3.11+ for tomllib). Never touches hardware.
"""
import argparse
import math
import os
import sys

try:
    import tomllib
except ImportError:  # pragma: no cover - Python < 3.11
    tomllib = None

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import odrive_enums  # noqa: E402

DEFAULT_PROFILE = os.path.join(REPO, "config", "hardware_profile.toml")
HEADER_PATH = os.path.join(REPO, "steeringwheel_firmware", "Core", "Inc", "wheel_config_generated.h")
SETUP_PATH = os.path.join(REPO, "tools", "generated", "odrive_setup.py")
CALIB_PATH = os.path.join(REPO, "tools", "generated", "odrive_calibrate.py")

STATUSES = ("TBD", "measured", "assumed")
# Sections whose TBD entries block arming the FFB (FFB_REQUIRE_MEASURED_PROFILE).
SAFETY_SECTIONS = ("belt", "encoder", "motor", "odrive", "wheel", "uart")

# (section, key, type, min, max) - hard ranges; outside = ERROR.
RANGES = [
    ("belt", "teeth_housing", int, 5, 1000),
    ("belt", "teeth_pulley", int, 5, 1000),
    ("encoder", "ppr", int, 1, 100000),
    ("encoder", "quadrature", int, 1, 4),
    ("motor", "pole_pairs", int, 1, 100),
    ("motor", "phase_resistance", float, 0.001, 100.0),
    ("motor", "motor_turns_per_wheel_rev", float, 0.01, 100.0),
    ("odrive", "axis", int, 0, 1),
    ("odrive", "current_lim", float, 0.5, 20.0),
    ("odrive", "calibration_current", float, 0.5, 20.0),
    ("odrive", "encoder_cal_current", float, 0.5, 20.0),
    ("odrive", "encoder_cal_current_lim", float, 0.5, 20.0),
    ("odrive", "vel_limit", float, 0.1, 20.0),
    ("odrive", "vel_gain", float, 0.001, 5.0),
    ("odrive", "dc_bus_overvoltage_trip_level", float, 12.0, 56.0),
    ("odrive", "dc_bus_undervoltage_trip_level", float, 8.0, 50.0),
    ("odrive", "dc_max_positive_current", float, 0.5, 30.0),
    ("odrive", "dc_max_negative_current", float, -30.0, 0.0),
    ("odrive", "brake_resistance", float, 0.0, 100.0),
    ("odrive", "max_regen_current", float, 0.0, 30.0),
    ("odrive", "watchdog_timeout", float, 0.01, 5.0),
    ("odrive", "torque_constant", float, 0.001, 10.0),
    ("odrive", "encoder_mode", int, 0, 0x100),
    ("wheel", "lock_to_lock_deg", float, 90.0, 3600.0),
    ("uart", "baud", int, 9600, 2000000),
    ("mcp23017", "address_7bit", int, 0x20, 0x27),
    ("ffb", "max_amps", float, 0.0, 6.0),
    ("ffb", "slew_amps_per_s", float, 0.01, 1000.0),
    ("ffb", "host_gain", float, 0.1, 10.0),
    ("ffb", "gain_periodic", float, 0.0, 10.0),
    ("ffb", "gain_constant", float, 0.0, 10.0),
    ("ffb", "gain_condition", float, 0.0, 10.0),
    ("ffb", "periodic_max_amps", float, 0.0, 6.0),
    ("ffb", "regen_power_limit_w", float, 0.0, 200.0),
    ("ffb", "spring_kp_amps_per_turn", float, 0.0, 20.0),
    ("ffb", "spring_amps_90", float, 0.0, 6.0),
    ("ffb", "spring_amps_180", float, 0.0, 6.0),
    ("ffb", "spring_amps_270", float, 0.0, 6.0),
    ("ffb", "spring_amps_360", float, 0.0, 6.0),
    ("ffb", "spring_amps_450", float, 0.0, 6.0),
    ("ffb", "spring_amps_540", float, 0.0, 6.0),
    ("ffb", "damper_amps_per_turn_s", float, 0.0, 20.0),
    ("ffb", "friction_amps", float, 0.0, 1.0),
    ("ffb", "inertia_amps_per_turn_s2", float, 0.0, 5.0),
    ("ffb", "endstop_width_deg", float, 1.0, 90.0),
    ("ffb", "endstop_amps", float, 0.0, 6.0),
    ("ffb", "endstop_damping_amps_per_turn_s", float, 0.0, 20.0),
    ("ffb", "overspeed_turns_s", float, 0.1, 20.0),
    ("ffb", "vbus_min_v", float, 8.0, 56.0),
    ("ffb", "vbus_max_v", float, 8.0, 56.0),
    ("ffb", "vbus_derate_start_v", float, 0.0, 56.0),
    ("ffb", "vbus_derate_end_v", float, 0.0, 56.0),
    ("ffb", "watchdog_timeout_min_s", float, 0.01, 5.0),
    ("ffb", "watchdog_timeout_max_s", float, 0.01, 5.0),
    ("ffb", "command_period_ms", int, 1, 100),
    ("ffb", "stale_ms", int, 5, 1000),
    ("ffb", "arm_hold_ms", int, 1000, 10000),
]

# UART bytes per FFB command cycle: torque write + watchdog feed + position request.
FFB_TX_BYTES_PER_CYCLE = len("w axis0.controller.input_torque -0.1234\n") + len("u 0\n") + len("f 0\n")


class ProfileError(Exception):
    pass


# ----------------------------------------------------------------------------
# Loading
# ----------------------------------------------------------------------------
def load_profile(path):
    if tomllib is None:
        raise ProfileError("Python 3.11+ is required (tomllib)")
    with open(path, "rb") as f:
        data = tomllib.load(f)
    return parse_profile(data)


def parse_profile(data):
    """Return {section: {key: {"value", "status", "how_to_measure"}}} and check the shape."""
    out = {}
    for section, entries in data.items():
        if section == "meta":
            continue
        if not isinstance(entries, dict):
            raise ProfileError("[%s] must be a table" % section)
        out[section] = {}
        for key, entry in entries.items():
            if not isinstance(entry, dict) or "value" not in entry:
                raise ProfileError("%s.%s: needs { value, status, how_to_measure }" % (section, key))
            status = entry.get("status")
            if status not in STATUSES:
                raise ProfileError("%s.%s: status must be one of %s, got %r" % (section, key, STATUSES, status))
            if not entry.get("how_to_measure"):
                raise ProfileError("%s.%s: how_to_measure is empty" % (section, key))
            out[section][key] = {"value": entry["value"], "status": status,
                                 "how_to_measure": entry["how_to_measure"]}
            if entry.get("note"):                 # optional free text (who/when confirmed)
                out[section][key]["note"] = str(entry["note"])
    return out


def val(p, section, key):
    try:
        return p[section][key]["value"]
    except KeyError:
        raise ProfileError("missing entry %s.%s" % (section, key))


def tbd_entries(p, sections=None):
    res = []
    for section in sorted(p):
        if sections is not None and section not in sections:
            continue
        for key in sorted(p[section]):
            if p[section][key]["status"] == "TBD":
                res.append("%s.%s" % (section, key))
    return res


# ----------------------------------------------------------------------------
# Derived values and validation
# ----------------------------------------------------------------------------
def derive(p):
    ppr = val(p, "encoder", "ppr")
    quad = val(p, "encoder", "quadrature")
    th = val(p, "belt", "teeth_housing")
    tp = val(p, "belt", "teeth_pulley")
    mt = float(val(p, "motor", "motor_turns_per_wheel_rev"))
    counts_per_encoder_rev = ppr * quad
    belt_ratio = th / tp                      # encoder turns per motor turn
    cpr_exact = counts_per_encoder_rev * belt_ratio
    cpr = int(round(cpr_exact))
    wheel_counts_exact = cpr_exact * mt
    baud = val(p, "uart", "baud")
    bytes_per_s = baud / 10.0                 # 8N1 = 10 bits per byte
    period_ms = val(p, "ffb", "command_period_ms")
    tx_load = FFB_TX_BYTES_PER_CYCLE / (bytes_per_s * period_ms / 1000.0)
    return {
        "counts_per_encoder_rev": counts_per_encoder_rev,
        "belt_ratio_encoder_per_motor": belt_ratio,
        "cpr_exact": cpr_exact,
        "cpr": cpr,
        "cpr_is_integer": abs(cpr_exact - cpr) < 1e-9,
        # ODrive "turns" are motor turns when cpr = counts per motor revolution.
        "odrive_turns_per_wheel_rev": mt,
        "encoder_counts_per_wheel_rev_exact": wheel_counts_exact,
        "encoder_counts_per_wheel_rev": int(round(wheel_counts_exact)),
        "counts_per_wheel_deg": wheel_counts_exact / 360.0,
        "uart_bytes_per_s": bytes_per_s,
        "ffb_tx_bytes_per_cycle": FFB_TX_BYTES_PER_CYCLE,
        "ffb_uart_tx_load": tx_load,
        "ffb_max_command_rate_hz": bytes_per_s / FFB_TX_BYTES_PER_CYCLE,
    }


def validate(p, require_measured=False):
    """Return (errors, warnings) as lists of strings."""
    errors, warnings = [], []
    for section, key, typ, lo, hi in RANGES:
        try:
            v = val(p, section, key)
        except ProfileError as e:
            errors.append(str(e))
            continue
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            errors.append("%s.%s: expected a number, got %r" % (section, key, v))
            continue
        if typ is int and not isinstance(v, int):
            errors.append("%s.%s: expected an integer, got %r" % (section, key, v))
            continue
        if not (lo <= v <= hi) or (isinstance(v, float) and not math.isfinite(v)):
            errors.append("%s.%s = %r outside the allowed range [%s, %s]" % (section, key, v, lo, hi))
    for section, key in (("wheel", "invert"), ("encoder", "has_index"), ("mcp23017", "use_ipol"),
                         ("odrive", "enable_watchdog"), ("odrive", "enable_current_mode_vel_limit"),
                         ("odrive", "enable_overspeed_error")):
        try:
            v = val(p, section, key)
            if not isinstance(v, bool):
                errors.append("%s.%s: expected true/false, got %r" % (section, key, v))
        except ProfileError as e:
            errors.append(str(e))
    if errors:
        return errors, warnings

    d = derive(p)
    if not d["cpr_is_integer"]:
        warnings.append(
            "!!! cpr = 4*PPR*teeth_housing/teeth_pulley = %.4f is NOT an integer. The ODrive needs an "
            "integer cpr (counts per MOTOR turn) for commutation; it will be rounded to %d and the "
            "commutation angle will drift by %.4f counts per turn. Choose pulleys with an integer "
            "ratio or accept the drift only after checking it. !!!" % (d["cpr_exact"], d["cpr"],
                                                                      d["cpr_exact"] - d["cpr"]))
    if abs(d["encoder_counts_per_wheel_rev_exact"] - d["encoder_counts_per_wheel_rev"]) > 1e-6:
        warnings.append("encoder counts per wheel turn = %.4f is not an integer (rounded to %d)"
                        % (d["encoder_counts_per_wheel_rev_exact"], d["encoder_counts_per_wheel_rev"]))
    if d["cpr"] < 4 * val(p, "motor", "pole_pairs"):
        errors.append("cpr %d is too small for %d pole pairs" % (d["cpr"], val(p, "motor", "pole_pairs")))

    o = lambda k: val(p, "odrive", k)  # noqa: E731
    f = lambda k: val(p, "ffb", k)     # noqa: E731
    if o("calibration_current") > o("current_lim"):
        errors.append("odrive.calibration_current must be <= odrive.current_lim")
    if o("encoder_cal_current") > o("encoder_cal_current_lim"):
        errors.append("odrive.encoder_cal_current must be <= odrive.encoder_cal_current_lim")
    if o("encoder_cal_current_lim") > 10.0:
        warnings.append("odrive.encoder_cal_current_lim > 10 A is a lot for a calibration")
    if o("dc_bus_undervoltage_trip_level") >= o("dc_bus_overvoltage_trip_level"):
        errors.append("odrive undervoltage trip must be below the overvoltage trip")
    if not (o("dc_bus_undervoltage_trip_level") < f("vbus_min_v") < f("vbus_max_v")
            < o("dc_bus_overvoltage_trip_level")):
        errors.append("need undervoltage_trip < ffb.vbus_min_v < ffb.vbus_max_v < overvoltage_trip")
    if f("max_amps") > o("current_lim"):
        errors.append("ffb.max_amps must not exceed odrive.current_lim")
    if f("endstop_amps") > f("max_amps"):
        warnings.append("ffb.endstop_amps > ffb.max_amps: the wall will be clipped at max_amps")
    if not (f("watchdog_timeout_min_s") <= o("watchdog_timeout") <= f("watchdog_timeout_max_s")):
        errors.append("odrive.watchdog_timeout must lie inside [ffb.watchdog_timeout_min_s, ffb.watchdog_timeout_max_s]"
                      " or the FFB precheck will always refuse")
    if o("watchdog_timeout") * 1000.0 < 4 * f("command_period_ms"):
        errors.append("odrive.watchdog_timeout must be at least 4 command periods")
    if f("vbus_derate_start_v") > 0 and not (f("vbus_min_v") < f("vbus_derate_start_v") < f("vbus_derate_end_v") < f("vbus_max_v")):
        errors.append("need ffb.vbus_min_v < vbus_derate_start_v < vbus_derate_end_v < vbus_max_v (or vbus_derate_start_v = 0 to switch it off)")
    curve = [f("spring_amps_%d" % d) for d in (90, 180, 270, 360, 450, 540)]
    if any(curve):
        if any(b < a for a, b in zip(curve, curve[1:])):
            errors.append("ffb.spring_amps_90 .. spring_amps_540 must not decrease with the angle")
        if max(curve) > f("max_amps"):
            warnings.append("ffb.spring_amps_* above ffb.max_amps are cut at max_amps (the curve goes flat there)")
    if f("periodic_max_amps") > f("max_amps"):
        warnings.append("ffb.periodic_max_amps > ffb.max_amps: the periodic limit has no effect")
    if f("stale_ms") < 3 * f("command_period_ms"):
        errors.append("ffb.stale_ms must be at least 3 command periods")
    if not o("enable_watchdog"):
        warnings.append("odrive.enable_watchdog is false: the FFB precheck will refuse to arm")
    if o("current_lim") > 10.0:
        warnings.append("odrive.current_lim > 10 A is not a gentle profile")
    if o("vel_limit") > 5.0:
        warnings.append("odrive.vel_limit > 5 turns/s is not a gentle profile")
    # Bench 2026-11-17: with enable_current_mode_vel_limit the ODrive clips the torque to (vel_limit - speed) * vel_gain,
    # i.e. vel_limit * vel_gain at rest. With vel_limit 2 and the old vel_gain 0.02 that was 0.04 Nm, so the motor
    # delivered about 0.04 Nm although 0.25 Nm was commanded (Iq_setpoint 0.0822 A = 0.041125 Nm / 0.5).
    if o("enable_current_mode_vel_limit") and (o("vel_limit") * o("vel_gain") < f("max_amps") * o("torque_constant") - 1e-9):
        errors.append("odrive.vel_limit * odrive.vel_gain = %.3f Nm is below ffb.max_amps * torque_constant = %.3f Nm: the ODrive "
                      "velocity limiter would clip the force even at rest (raise odrive.vel_gain)"
                      % (o("vel_limit") * o("vel_gain"), f("max_amps") * o("torque_constant")))
    if f("overspeed_turns_s") * val(p, "motor", "motor_turns_per_wheel_rev") > 1.5 * o("vel_limit") + 1e-9:
        warnings.append("ffb.overspeed_turns_s is far above odrive.vel_limit (FFB fault would come late)")
    if d["ffb_uart_tx_load"] > 0.9:
        errors.append("FFB command period %d ms needs %.0f%% of the UART at %d baud (max 90%%); "
                      "raise ffb.command_period_ms or the baud rate"
                      % (f("command_period_ms"), 100 * d["ffb_uart_tx_load"], val(p, "uart", "baud")))
    if val(p, "wheel", "centre_mode") != "power_up":
        errors.append("wheel.centre_mode: only 'power_up' is implemented")
    if require_measured:
        for name in tbd_entries(p):
            errors.append("%s is still TBD (--require-measured)" % name)
    return errors, warnings


# ----------------------------------------------------------------------------
# Generators
# ----------------------------------------------------------------------------
def c_float(x):
    s = repr(float(x))
    if "e" in s or "E" in s:
        s = "%.9g" % float(x)
        if "." not in s and "e" not in s:
            s += ".0"
    return s + "f"


def gen_header(p):
    d = derive(p)
    ax = val(p, "odrive", "axis")
    tbd = tbd_entries(p)
    safety_tbd = tbd_entries(p, SAFETY_SECTIONS)
    L = []
    w = L.append
    w("/* AUTO-GENERATED by tools/gen_config.py from config/hardware_profile.toml.")
    w(" * DO NOT EDIT: change the profile and rebuild (CMake re-runs the generator).")
    w(" * Every macro is #ifndef-guarded so -D on the command line still wins. */")
    w("#ifndef WHEEL_CONFIG_GENERATED_H")
    w("#define WHEEL_CONFIG_GENERATED_H")
    w("")
    w("#define PROFILE_VERSION               1")
    w("#define PROFILE_TBD_COUNT             %dU   /* entries still marked TBD          */" % len(tbd))
    w("#define PROFILE_SAFETY_TBD_COUNT      %dU   /* ...in belt/encoder/motor/odrive/wheel/uart */"
      % len(safety_tbd))
    # (The names are not listed here on purpose: ODrive property names must not
    # appear in firmware sources outside Core/*/ffb - tests/test_safety.c.)
    w("/* List of TBD entries: make status (tools/project_status.py). */")
    w("")

    def m(name, value, comment=""):
        w("#ifndef %s" % name)
        w("#define %-29s %s%s" % (name, value, ("   /* %s */" % comment) if comment else ""))
        w("#endif")

    w("/* --- encoder / belt / steering ------------------------------------- */")
    m("PROFILE_ENCODER_PPR", str(val(p, "encoder", "ppr")))
    m("PROFILE_BELT_TEETH_HOUSING", str(val(p, "belt", "teeth_housing")))
    m("PROFILE_BELT_TEETH_PULLEY", str(val(p, "belt", "teeth_pulley")))
    m("PROFILE_MOTOR_POLE_PAIRS", str(val(p, "motor", "pole_pairs")))
    m("ODRIVE_CPR", str(d["cpr"]), "counts per motor turn (axis0.encoder.config.cpr)")
    m("ENCODER_COUNTS_PER_WHEEL_REV", str(d["encoder_counts_per_wheel_rev"]),
      "STM32 TIM3 fallback source")
    m("WHEEL_LOCK_TO_LOCK_DEG", str(int(round(val(p, "wheel", "lock_to_lock_deg")))))
    inv = "1" if val(p, "wheel", "invert") else "0"
    m("ENCODER_INVERT_DIRECTION", inv)
    m("ODRIVE_INVERT_DIRECTION", inv)
    m("ODRIVE_TURNS_PER_WHEEL_REV", c_float(d["odrive_turns_per_wheel_rev"]),
      "ODrive turns (= motor turns) per wheel turn")
    w("")
    w("/* --- links ----------------------------------------------------------- */")
    m("ODRIVE_UART_BAUD", "%dU" % val(p, "uart", "baud"))
    m("ODRIVE_AXIS", str(ax))
    m("MCP23017_I2C_ADDR_7BIT", "0x%02XU" % val(p, "mcp23017", "address_7bit"))
    m("MCP23017_USE_IPOL", "1" if val(p, "mcp23017", "use_ipol") else "0")
    w("")
    w("/* --- FFB limits (used only with -DWHEEL_FFB=ON) ----------------------- */")
    f = lambda k: val(p, "ffb", k)     # noqa: E731
    o = lambda k: val(p, "odrive", k)  # noqa: E731
    m("FFB_PROFILE_MAX_AMPS", c_float(f("max_amps")))
    m("FFB_SLEW_AMPS_PER_S", c_float(f("slew_amps_per_s")))
    m("FFB_HOST_GAIN", c_float(f("host_gain")), "game force x gain, then clamped (max_amps is still the cap)")
    m("FFB_GAIN_PERIODIC", c_float(f("gain_periodic")), "game texture (sine/square/triangle/saw) x gain")
    m("FFB_GAIN_CONSTANT", c_float(f("gain_constant")), "game constant force / ramp x gain")
    m("FFB_GAIN_CONDITION", c_float(f("gain_condition")), "game spring/damper/friction/inertia x gain (keep low: closed loop)")
    m("FFB_PERIODIC_MAX_AMPS", c_float(f("periodic_max_amps")), "the periodic part alone is limited to this (A)")
    m("FFB_REGEN_POWER_LIMIT_W", c_float(f("regen_power_limit_w")), "W the motor may take out of the wheel (braking power limit, 0 = off)")
    m("FFB_TORQUE_CONSTANT_NOMINAL", c_float(o("torque_constant")), "Nm/A, only for the regeneration power estimate")
    for d in (90, 180, 270, 360, 450, 540):
        m("FFB_SPRING_AMPS_%d" % d, c_float(f("spring_amps_%d" % d)), "centring force (A) at %d deg from the centre" % d)
    m("FFB_SPRING_KP", c_float(f("spring_kp_amps_per_turn")), "A per wheel turn")
    m("FFB_DAMPER_KD", c_float(f("damper_amps_per_turn_s")), "A per wheel turn/s")
    m("FFB_FRICTION_AMPS", c_float(f("friction_amps")))
    m("FFB_INERTIA_KI", c_float(f("inertia_amps_per_turn_s2")), "placeholder")
    m("FFB_ENDSTOP_WIDTH_DEG", c_float(f("endstop_width_deg")))
    m("FFB_ENDSTOP_AMPS", c_float(f("endstop_amps")))
    m("FFB_ENDSTOP_DAMPING", c_float(f("endstop_damping_amps_per_turn_s")))
    m("FFB_OVERSPEED_TURNS_S", c_float(f("overspeed_turns_s")), "wheel turns/s")
    m("FFB_VBUS_MIN_V", c_float(f("vbus_min_v")))
    m("FFB_VBUS_MAX_V", c_float(f("vbus_max_v")))
    m("FFB_VBUS_DERATE_START_V", c_float(f("vbus_derate_start_v")), "force fades out above this supply voltage (0 = off)")
    m("FFB_VBUS_DERATE_END_V", c_float(f("vbus_derate_end_v")), "force is 0 at and above this voltage (must be below vbus_max_v)")
    m("FFB_WATCHDOG_TIMEOUT_MIN_S", c_float(f("watchdog_timeout_min_s")))
    m("FFB_WATCHDOG_TIMEOUT_MAX_S", c_float(f("watchdog_timeout_max_s")))
    m("FFB_COMMAND_PERIOD_MS", "%dU" % f("command_period_ms"))
    m("FFB_STALE_MS", "%dU" % f("stale_ms"))
    m("FFB_ARM_HOLD_MS", "%dU" % f("arm_hold_ms"))
    m("FFB_PRECHECK_CURRENT_LIM_MAX", c_float(o("current_lim")), "live value must be <= this")
    m("FFB_PRECHECK_VEL_LIMIT_MAX", c_float(o("vel_limit")), "live value must be <= this")
    w("")
    w("#endif /* WHEEL_CONFIG_GENERATED_H */")
    return "\n".join(L) + "\n"


def setup_settings(p):
    """Ordered list of (path relative to odrv0, value) applied by odrive_setup.py."""
    d = derive(p)
    ax = "axis%d" % val(p, "odrive", "axis")
    o = lambda k: val(p, "odrive", k)  # noqa: E731
    return [
        (ax + ".config.startup_encoder_offset_calibration", False),
        (ax + ".config.startup_closed_loop_control", False),
        (ax + ".motor.config.pole_pairs", int(val(p, "motor", "pole_pairs"))),
        (ax + ".motor.config.current_lim", float(o("current_lim"))),
        (ax + ".motor.config.calibration_current", float(o("calibration_current"))),
        (ax + ".motor.config.torque_constant", float(o("torque_constant"))),
        (ax + ".encoder.config.mode", int(o("encoder_mode"))),
        (ax + ".encoder.config.cpr", int(d["cpr"])),
        (ax + ".encoder.config.use_index", bool(val(p, "encoder", "has_index"))),
        (ax + ".controller.config.vel_limit", float(o("vel_limit"))),
        (ax + ".controller.config.vel_gain", float(o("vel_gain"))),
        (ax + ".controller.config.enable_current_mode_vel_limit", bool(o("enable_current_mode_vel_limit"))),
        (ax + ".controller.config.enable_overspeed_error", bool(o("enable_overspeed_error"))),
        (ax + ".controller.config.control_mode", odrive_enums.CONTROL_MODE_TORQUE_CONTROL),
        (ax + ".controller.config.input_mode", 1),  # INPUT_MODE_PASSTHROUGH
        (ax + ".config.watchdog_timeout", float(o("watchdog_timeout"))),
        (ax + ".config.enable_watchdog", bool(o("enable_watchdog"))),
        ("config.dc_bus_overvoltage_trip_level", float(o("dc_bus_overvoltage_trip_level"))),
        ("config.dc_bus_undervoltage_trip_level", float(o("dc_bus_undervoltage_trip_level"))),
        ("config.dc_max_positive_current", float(o("dc_max_positive_current"))),
        ("config.dc_max_negative_current", float(o("dc_max_negative_current"))),
    ]



def check_only_settings(p):
    """(path, expected value) compared by odrive_status.py but NEVER written by
    odrive_setup.py (brake/regen and UART settings are a human decision)."""
    o = lambda k: val(p, "odrive", k)  # noqa: E731
    return [
        ("config.brake_resistance", float(o("brake_resistance"))),
        ("config.max_regen_current", float(o("max_regen_current"))),
        ("config.enable_uart", True),
        ("config.uart_baudrate", int(val(p, "uart", "baud"))),
    ]

_SCRIPT_COMMON = r'''
ERROR_TABLES = {ERROR_TABLES}
AXIS = {AXIS}
VBUS_MIN = {VBUS_MIN}
VBUS_MAX = {VBUS_MAX}


def resolve(odrv, path):
    """Return (parent_object, attribute_name) for 'axis0.motor.config.x'."""
    parts = path.split(".")
    obj = odrv
    for part in parts[:-1]:
        obj = getattr(obj, part)
    return obj, parts[-1]


def read(odrv, path):
    parent, name = resolve(odrv, path)
    return getattr(parent, name)


def has(odrv, path):
    try:
        read(odrv, path)
        return True
    except AttributeError:
        return False


def decode(path, value):
    table = ERROR_TABLES.get(path.replace("axis1", "axis0"))
    if table is None:
        return hex(int(value))
    names = [n for bit, n in sorted(table.items()) if int(value) & bit]
    rest = int(value) & ~sum(table)
    if rest:
        names.append("UNKNOWN_0x%X" % rest)
    return ", ".join(names) if names else "none"


def errors(odrv):
    out = {{}}
    for path in ("axis%d.error" % AXIS, "axis%d.motor.error" % AXIS, "axis%d.encoder.error" % AXIS,
                 "axis%d.controller.error" % AXIS, "error"):
        if has(odrv, path):
            out[path] = int(read(odrv, path))
    return out


def connect():
    try:
        import odrive  # official package; optional
    except ImportError:
        sys.exit("The 'odrive' Python package is not installed. Use the dry run output and type the "
                 "commands into odrivetool by hand instead.")
    print("Searching for an ODrive over USB ...")
    return odrive.find_any(timeout=15)


def check_safe_to_touch(odrv):
    """Axis IDLE, vbus sane. Returns a list of problems (empty = ok)."""
    problems = []
    state = int(read(odrv, "axis%d.current_state" % AXIS))
    if state != 1:
        problems.append("axis%d.current_state = %d (must be 1 = IDLE)" % (AXIS, state))
    vbus = float(read(odrv, "vbus_voltage"))
    if not (VBUS_MIN <= vbus <= VBUS_MAX):
        problems.append("vbus_voltage = %.2f V outside [%.1f, %.1f] V" % (vbus, VBUS_MIN, VBUS_MAX))
    return problems
'''


def _dump_parser_source():
    """tools/odrive_dump.py without its module docstring, for embedding."""
    with open(os.path.join(HERE, "odrive_dump.py"), "r", encoding="utf-8") as f:
        src = f.read()
    if src.startswith('"""'):
        src = src[src.index('"""', 3) + 3:].lstrip("\n")
    return src.rstrip("\n")


def gen_setup_script(p):
    settings = setup_settings(p)
    ax = val(p, "odrive", "axis")
    tables = {k.replace("axis0", "axis%d" % ax): v for k, v in odrive_enums.ERROR_REGISTERS.items()}
    lines = ["    (%r, %r)," % (path, v) for path, v in settings]
    common = _SCRIPT_COMMON.format(ERROR_TABLES=repr(tables), AXIS=ax,
                                   VBUS_MIN=val(p, "ffb", "vbus_min_v"),
                                   VBUS_MAX=val(p, "ffb", "vbus_max_v"))
    return ('''#!/usr/bin/env python3
"""AUTO-GENERATED by tools/gen_config.py from config/hardware_profile.toml. DO NOT EDIT.

Apply the GENTLE ODrive profile.

    python3 tools/generated/odrive_setup.py            # DRY RUN: old -> new against docs/odrive_dump.txt
                                                       #   + the odrivetool commands; connects to nothing
    python3 tools/generated/odrive_setup.py --dump F   # DRY RUN against another saved dump
    python3 tools/generated/odrive_setup.py --read     # connect, show old -> new, write nothing
    python3 tools/generated/odrive_setup.py --apply    # connect, check IDLE + vbus, write, verify
    python3 tools/generated/odrive_setup.py --apply --save   # ... and save_configuration()

Never requests any motion or state change. Never saves unless --save is given.
Paths that do not exist on this firmware build are skipped with a warning.
"""
import argparse
import os
import sys

SETTINGS = [
%s
]
DEFAULT_DUMP = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs",
                                             "odrive_dump.txt"))
%s
# ---- dump parser (copied from tools/odrive_dump.py by the generator) ----
%s
# -------------------------------------------------------------------------

def plan_lines():
    return ["odrv0.%%s = %%r" %% (path, value) for path, value in SETTINGS]


def _same(old, new):
    if isinstance(new, bool) or isinstance(old, bool):
        return bool(old) == bool(new) and isinstance(old, (bool, int))
    if isinstance(new, float):
        try:
            return abs(float(old) - new) <= 1e-4 * max(1.0, abs(new))
        except (TypeError, ValueError):
            return False
    return old == new


def dry_run_diff(dump_values, out=print):
    """Old (saved, from the dump) -> new (profile). Returns the number of changes."""
    changes = 0
    out("# %%-58s %%-22s %%s" %% ("parameter", "dump (saved)", "profile (new)"))
    for path, new in SETTINGS:
        if path not in dump_values:
            out("  %%-58s %%-22s %%r   <-- NOT IN DUMP: would be skipped" %% (path, "-", new))
            continue
        old = dump_values[path]
        if _same(old, new):
            out("  %%-58s %%-22r %%r" %% (path, old, new))
        else:
            changes += 1
            out("  %%-58s %%-22r %%r   <-- change" %% (path, old, new))
    return changes


def apply(odrv, write=False, save=False, out=print):
    """Show old -> new for every setting; write only if write=True. Returns exit code."""
    problems = check_safe_to_touch(odrv)
    for name, value in errors(odrv).items():
        out("  %%-28s = 0x%%X (%%s)" %% (name, value, decode(name, value)))
    if problems and write:
        for p in problems:
            out("REFUSED: " + p)
        return 2
    for p in problems:
        out("WARNING: " + p)
    failures = 0
    for path, new in SETTINGS:
        if not has(odrv, path):
            out("  SKIP %%-55s (not present on this firmware)" %% path)
            continue
        old = read(odrv, path)
        mark = "" if _same(old, new) else "   <-- change"
        out("  %%-55s %%r -> %%r%%s" %% (path, old, new, mark))
        if write and not _same(old, new):
            parent, name = resolve(odrv, path)
            setattr(parent, name, new)
            back = getattr(parent, name)
            if isinstance(new, float):
                ok = abs(float(back) - new) <= 1e-4 * max(1.0, abs(new))
            else:
                ok = back == new
            if not ok:
                failures += 1
                out("  !!! read back %%r, expected %%r" %% (back, new))
    if write and save and failures == 0:
        out("Saving configuration (the ODrive reboots) ...")
        odrv.save_configuration()
    elif write and not save:
        out("Written to RAM only (lost at power-off). Re-run with --save to make it permanent.")
    return 1 if failures else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--read", action="store_true", help="connect and compare, write nothing")
    ap.add_argument("--apply", action="store_true", help="write the values (RAM only)")
    ap.add_argument("--save", action="store_true", help="with --apply: also save_configuration()")
    ap.add_argument("--dump", default=DEFAULT_DUMP, help="dry run: saved dump to diff against")
    a = ap.parse_args(argv)
    if a.save and not a.apply:
        ap.error("--save needs --apply")
    if not (a.read or a.apply):
        print("# DRY RUN - nothing is sent, nothing is connected.")
        changed = SETTINGS
        if a.dump and os.path.exists(a.dump):
            with open(a.dump, "r", encoding="utf-8", errors="replace") as fh:
                dump_values, problems = parse_dump_report(fh.read())
            print("# Old values: %%s (saved state when the dump was made; re-check live with --read)" %% a.dump)
            for no, line, why in problems:
                print("# PARSE WARN line %%d: %%s [%%s]" %% (no, line.strip()[:70], why))
            n = dry_run_diff(dump_values)
            print("# %%d change(s)." %% n)
            changed = [(p, v) for p, v in SETTINGS if p in dump_values and not _same(dump_values[p], v)]
        else:
            print("# (no dump found: listing every setting)")
        print("# Commands for odrivetool (only the changes):")
        for path, value in changed:
            print("odrv0.%%s = %%r" %% (path, value))
        print("# (then, only if you want it permanent: odrv0.save_configuration())")
        return 0
    odrv = connect()
    return apply(odrv, write=a.apply, save=a.save)


if __name__ == "__main__":
    sys.exit(main())
''' % ("\n".join(lines), common, _dump_parser_source()))


def gen_calibrate_script(p):
    ax = val(p, "odrive", "axis")
    tables = {k.replace("axis0", "axis%d" % ax): v for k, v in odrive_enums.ERROR_REGISTERS.items()}
    common = _SCRIPT_COMMON.format(ERROR_TABLES=repr(tables), AXIS=ax,
                                   VBUS_MIN=val(p, "ffb", "vbus_min_v"),
                                   VBUS_MAX=val(p, "ffb", "vbus_max_v"))
    return ('''#!/usr/bin/env python3
"""AUTO-GENERATED by tools/gen_config.py from config/hardware_profile.toml. DO NOT EDIT.

Encoder OFFSET calibration (run by a human, at the bench, wheel area clear).

    python3 tools/generated/odrive_calibrate.py

!!! THE MOTOR WILL ROTATE (about half a turn one way and back, several seconds). !!!
!!! Remove the steering wheel or keep hands, cables and tools clear of it.        !!!
!!! Keep a hand on the 24 V supply switch.                                         !!!

It runs MOTOR_CALIBRATION first if the motor is not calibrated yet (beeps, no
rotation), then ENCODER_OFFSET_CALIBRATION, then returns to IDLE. Ctrl+C sends
IDLE immediately. Nothing is saved unless --save is given. The LPD3806 has no
index pulse, so with 0.5.x the offset calibration is probably needed after
every power-up (TODO(TBD): confirm on the bench).
"""
import argparse
import sys
import time

CURRENT_LIM_MAX = %r
CALIBRATION_CURRENT_MAX = %r
CPR = %r
CONFIRM_TEXT = "I AM CLEAR"
%s

def wait_idle(odrv, timeout_s, out=print):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout_s:
        if int(read(odrv, "axis%%d.current_state" %% AXIS)) == 1:
            return True
        time.sleep(0.2)
    out("Timeout - requesting IDLE")
    setattr(read(odrv, "axis%%d" %% AXIS), "requested_state", 1)
    return False


def preflight(odrv):
    problems = check_safe_to_touch(odrv)
    for name, value in errors(odrv).items():
        if value:
            problems.append("%%s = 0x%%X (%%s): clear errors first (odrv0.clear_errors())"
                            %% (name, value, decode(name, value)))
    if float(read(odrv, "axis%%d.motor.config.current_lim" %% AXIS)) > CURRENT_LIM_MAX + 1e-6:
        problems.append("current_lim above the profile (run odrive_setup.py first)")
    if float(read(odrv, "axis%%d.motor.config.calibration_current" %% AXIS)) > CALIBRATION_CURRENT_MAX + 1e-6:
        problems.append("calibration_current above the profile (run odrive_setup.py first)")
    if int(read(odrv, "axis%%d.encoder.config.cpr" %% AXIS)) != CPR:
        problems.append("encoder cpr differs from the profile (%%d) - run odrive_setup.py first" %% CPR)
    return problems


def calibrate(odrv, save=False, out=print, sleep=time.sleep):
    axis = read(odrv, "axis%%d" %% AXIS)
    try:
        if not bool(read(odrv, "axis%%d.motor.is_calibrated" %% AXIS)):
            out("MOTOR_CALIBRATION (beep, no rotation) ...")
            axis.requested_state = 4
            sleep(0.5)
            wait_idle(odrv, 15, out)
        out("ENCODER_OFFSET_CALIBRATION - the motor turns now ...")
        axis.requested_state = 7
        sleep(0.5)
        wait_idle(odrv, 30, out)
    except KeyboardInterrupt:
        axis.requested_state = 1
        out("Interrupted - IDLE requested")
        return 3
    errs = errors(odrv)
    for name, value in errs.items():
        out("  %%-28s = 0x%%X (%%s)" %% (name, value, decode(name, value)))
    ready = bool(read(odrv, "axis%%d.encoder.is_ready" %% AXIS))
    out("encoder.is_ready = %%s" %% ready)
    if any(errs.values()) or not ready:
        out("Calibration FAILED - see docs/HARDWARE_BRINGUP.md (CPR_POLEPAIRS_MISMATCH = wrong cpr/belt ratio).")
        return 1
    if save:
        out("Setting motor.config.pre_calibrated = True and saving ...")
        axis.motor.config.pre_calibrated = True
        odrv.save_configuration()
    out("Done. The axis is IDLE.")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--save", action="store_true",
                    help="after success: motor.config.pre_calibrated = True + save_configuration()")
    a = ap.parse_args(argv)
    print(__doc__)
    odrv = connect()
    problems = preflight(odrv)
    if problems:
        for p in problems:
            print("REFUSED: " + p)
        return 2
    answer = input("Type '%%s' to let the motor rotate: " %% CONFIRM_TEXT)
    if answer.strip() != CONFIRM_TEXT:
        print("Not confirmed - nothing done.")
        return 2
    return calibrate(odrv, save=a.save)


if __name__ == "__main__":
    sys.exit(main())
''' % (float(val(p, "odrive", "current_lim")), float(val(p, "odrive", "calibration_current")),
       int(derive(p)["cpr"]), common))


# ----------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------
def outputs(p, header_only=False):
    res = [(HEADER_PATH, gen_header(p))]
    if not header_only:
        res.append((SETUP_PATH, gen_setup_script(p)))
        res.append((CALIB_PATH, gen_calibrate_script(p)))
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", default=DEFAULT_PROFILE)
    ap.add_argument("--check", action="store_true", help="do not write; exit 1 if outputs are stale")
    ap.add_argument("--require-measured", action="store_true", help="any TBD entry is an error")
    ap.add_argument("--header-only", action="store_true", help="only write the C header (used by CMake)")
    ap.add_argument("--derived", action="store_true", help="print derived values and exit")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    try:
        p = load_profile(a.profile)
        errors, warnings = validate(p, a.require_measured)
    except (ProfileError, OSError, ValueError) as e:
        print("gen_config: ERROR: %s" % e, file=sys.stderr)
        return 2
    for wmsg in warnings:
        print("gen_config: WARNING: %s" % wmsg, file=sys.stderr)
    if errors:
        for e in errors:
            print("gen_config: ERROR: %s" % e, file=sys.stderr)
        return 2
    if a.derived:
        for k, v in derive(p).items():
            print("%-36s %s" % (k, v))
        return 0

    stale = []
    for path, text in outputs(p, a.header_only):
        try:
            with open(path, "r", encoding="utf-8") as f:
                current = f.read()
        except OSError:
            current = None
        if current == text:
            continue
        stale.append(os.path.relpath(path, REPO))
        if not a.check:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            if path.endswith(".py"):
                os.chmod(path, 0o755)
    if a.check:
        if stale:
            print("gen_config: out of date: %s (run python3 tools/gen_config.py)" % ", ".join(stale),
                  file=sys.stderr)
            return 1
        if not a.quiet:
            print("gen_config: generated files are up to date")
        return 0
    if not a.quiet:
        n_tbd = len(tbd_entries(p))
        print("gen_config: %s; %d profile entries still TBD"
              % ("wrote " + ", ".join(stale) if stale else "nothing changed", n_tbd))
    return 0


if __name__ == "__main__":
    sys.exit(main())
