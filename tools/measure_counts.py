#!/usr/bin/env python3
"""Turn a hand measurement into encoder/belt numbers for config/hardware_profile.toml.

Read ``odrv0.axis0.encoder.shadow_count`` in odrivetool (READ only; it is the raw
x4 count and does NOT depend on the configured cpr), turn by hand, read again:

    # turned the ENCODER SHAFT (belt off) 3 full turns: 117 -> 7317
    python3 tools/measure_counts.py encoder --turns 3 --start 117 --end 7317

    # turned the STEERING WHEEL (belt on) 2 full turns clockwise: -42 -> 19158
    python3 tools/measure_counts.py wheel --turns 2 --start -42 --end 19158

Prints counts per turn, the implied belt ratio, cpr (counts per MOTOR turn, what
axis0.encoder.config.cpr must be), ODRIVE_TURNS_PER_WHEEL_REV and warnings for
non-integer results or a mismatch with the profile. Pure functions + tiny CLI;
never touches hardware.
"""
import argparse
import os
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_config  # noqa: E402

# A hand-turned measurement is never exact: accept this relative error.
DEFAULT_TOLERANCE = 0.02


def counts_per_turn(start, end, turns):
    """Signed counts per turn. Raises ValueError for zero/negative turns."""
    if not turns or turns <= 0:
        raise ValueError("turns must be > 0")
    return (end - start) / float(turns)


def nearest_integer(x):
    n = int(round(x))
    rel = abs(x - n) / abs(n) if n else float("inf")
    return n, rel


def analyse_encoder(start, end, turns, ppr_profile, quad_profile, tol=DEFAULT_TOLERANCE):
    """Encoder shaft turned by hand (belt off)."""
    cpt = counts_per_turn(start, end, turns)
    n, rel = nearest_integer(abs(cpt))
    expected = ppr_profile * quad_profile
    warnings = []
    if rel > tol:
        warnings.append("counts per encoder turn %.2f is far from an integer: count the turns again" % abs(cpt))
    if abs(n - expected) > tol * expected:
        warnings.append("measured %d counts per encoder turn, profile says %d (= %d PPR x %d): "
                        "update encoder.ppr / encoder.quadrature" % (n, expected, ppr_profile, quad_profile))
    return {
        "counts_per_encoder_turn": abs(cpt),
        "counts_per_encoder_turn_int": n,
        "direction": "positive" if cpt > 0 else "negative",
        "implied_ppr_x4": n / 4.0,
        "expected_from_profile": expected,
        "warnings": warnings,
        "suggest": {"encoder.ppr": n // quad_profile if n % quad_profile == 0 else n / quad_profile},
    }


def analyse_wheel(start, end, turns, profile, current_cpr=None, tol=DEFAULT_TOLERANCE):
    """Steering wheel turned by hand (belt on). Uses motor_turns_per_wheel_rev from the profile."""
    v = lambda s, k: gen_config.val(profile, s, k)  # noqa: E731
    cpt_wheel = counts_per_turn(start, end, turns)
    motor_per_wheel = float(v("motor", "motor_turns_per_wheel_rev"))
    counts_per_motor_turn = abs(cpt_wheel) / motor_per_wheel
    cpr, rel = nearest_integer(counts_per_motor_turn)
    counts_per_enc_turn = v("encoder", "ppr") * v("encoder", "quadrature")
    belt_ratio = counts_per_motor_turn / counts_per_enc_turn            # encoder turns per motor turn
    frac = Fraction(belt_ratio).limit_denominator(200)
    derived = gen_config.derive(profile)
    profile_cpr = derived["cpr"]
    warnings = []
    if not derived["cpr_is_integer"]:
        warnings.append("the profile tooth ratio %d:%d gives a NON-integer cpr (%.3f); the ODrive needs an "
                        "integer cpr - consider other pulleys" % (v("belt", "teeth_housing"),
                                                                 v("belt", "teeth_pulley"), derived["cpr_exact"]))
    if rel > tol:
        warnings.append("cpr %.2f is not close to an integer (%.1f %% off): repeat with more turns" %
                        (counts_per_motor_turn, 100 * rel))
    elif abs(counts_per_motor_turn - cpr) > 0.5:
        warnings.append("cpr %.2f rounded to %d - hand measurement error, or a non-integer belt ratio" %
                        (counts_per_motor_turn, cpr))
    if abs(cpr - profile_cpr) > tol * profile_cpr:
        warnings.append("measured cpr %d differs from the profile (%d = %d x %d/%d): fix belt.teeth_* "
                        % (cpr, profile_cpr, counts_per_enc_turn, v("belt", "teeth_housing"),
                           v("belt", "teeth_pulley")))
    exact_ratio = Fraction(v("belt", "teeth_housing"), v("belt", "teeth_pulley"))
    if abs(float(frac) - float(exact_ratio)) > tol * float(exact_ratio):
        warnings.append("implied tooth ratio ~%d:%d does not match the profile %d:%d"
                        % (frac.numerator, frac.denominator, v("belt", "teeth_housing"), v("belt", "teeth_pulley")))
    res = {
        "counts_per_wheel_turn": abs(cpt_wheel),
        "direction": "positive" if cpt_wheel > 0 else "negative",
        "counts_per_motor_turn": counts_per_motor_turn,
        "cpr": cpr,
        "belt_ratio_encoder_per_motor": belt_ratio,
        "implied_teeth_ratio": "%d:%d" % (frac.numerator, frac.denominator),
        "profile_cpr": profile_cpr,
        # ODrive turns = motor turns once axis0.encoder.config.cpr = cpr.
        "odrive_turns_per_wheel_rev": motor_per_wheel,
        "warnings": warnings,
    }
    if current_cpr:
        # Until the cpr is corrected, pos_estimate is scaled by the OLD cpr.
        res["odrive_turns_per_wheel_rev_with_current_cpr"] = abs(cpt_wheel) / float(current_cpr)
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=("encoder", "wheel"))
    ap.add_argument("--turns", type=float, required=True, help="full turns made (encoder shaft or wheel)")
    ap.add_argument("--start", type=int, required=True, help="shadow_count before")
    ap.add_argument("--end", type=int, required=True, help="shadow_count after")
    ap.add_argument("--current-cpr", type=int, help="cpr currently set in the ODrive (wheel mode)")
    ap.add_argument("--tolerance", type=float, default=DEFAULT_TOLERANCE)
    ap.add_argument("--profile", default=gen_config.DEFAULT_PROFILE)
    a = ap.parse_args(argv)
    profile = gen_config.load_profile(a.profile)
    try:
        if a.what == "encoder":
            r = analyse_encoder(a.start, a.end, a.turns, gen_config.val(profile, "encoder", "ppr"),
                                gen_config.val(profile, "encoder", "quadrature"), a.tolerance)
        else:
            r = analyse_wheel(a.start, a.end, a.turns, profile, a.current_cpr, a.tolerance)
    except ValueError as e:
        print("error: %s" % e)
        return 2
    for k, v in r.items():
        if k != "warnings":
            print("%-44s %s" % (k, ("%.4f" % v) if isinstance(v, float) else v))
    for w in r["warnings"]:
        print("WARNING: " + w)
    if a.what == "wheel" and not r["warnings"]:
        print("\nProfile OK. Set status = \"measured\" for belt.teeth_*, encoder.* and "
              "motor.motor_turns_per_wheel_rev, then run python3 tools/gen_config.py.")
    if a.what == "wheel":
        print("Direction: shadow_count went %s while you turned clockwise. If HID X must increase "
              "clockwise and this is negative, set wheel.invert = true." % r["direction"])
    return 1 if r["warnings"] else 0


if __name__ == "__main__":
    sys.exit(main())
