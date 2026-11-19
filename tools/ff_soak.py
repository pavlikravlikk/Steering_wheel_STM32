#!/usr/bin/env python3
"""Imitate a game session without a game: a random but repeatable mix of force effects for a few minutes.

    sudo python3 tools/ff_soak.py --minutes 3                # plays it (the wheel must be ARMED, hold the housing)
    python3 tools/ff_soak.py --minutes 3 --dry-run           # only prints the plan, touches nothing

The mix imitates driving: slowly changing sideways force (cornering), short bumps and impacts, rumble (about 12 Hz) and
fine texture (about 40 Hz), with short pauses. The percentages are of the FULL SCALE BEFORE the firmware's host_gain
(ffb.host_gain, 5 in the profile), so 20 percent already reaches the cap. It uses tools/ff_play.py for the device access;
do not run odrivetool or odrive_status.py while it plays (see docs/GAME_DAY.md).
Afterwards: the status recorder (hid_probe --status) must still say RUNNING / fault NONE, and the temperatures must be
warm, not hot.
"""
import argparse
import random
import sys

import ff_play

# (kind, min percent, max percent, min seconds, max seconds, extra), relative weight
MIX = [
    (("constant", 4.0, 20.0, 1.0, 3.0), 5),     # cornering force, either side
    (("pulse", 15.0, 40.0, 0.15, 0.15), 3),     # a bump or an impact
    (("sine", 5.0, 15.0, 1.0, 3.0, 80), 2),     # rumble (about 12 Hz)
    (("sine", 3.0, 10.0, 1.0, 3.0, 25), 2),     # fine texture (about 40 Hz)
]
MAX_PERCENT = 40.0
GAP_S = (0.3, 1.0)


def make_plan(minutes, seed=1):
    """A list of (kind, percent, dir_deg, seconds, period_ms, pause_after_s) covering about `minutes`."""
    rng = random.Random(seed)
    kinds = [m for m, _ in MIX]
    weights = [w for _, w in MIX]
    plan, total = [], 0.0
    limit = max(0.2, float(minutes)) * 60.0
    while total < limit:
        spec = rng.choices(kinds, weights)[0]
        kind, lo, hi, smin, smax = spec[:5]
        period = spec[5] if len(spec) > 5 else 100
        percent = min(rng.uniform(lo, hi), MAX_PERCENT)
        seconds = rng.uniform(smin, smax)
        direction = rng.choice([90.0, 270.0])
        pause = rng.uniform(*GAP_S)
        plan.append((kind, round(percent, 1), direction, round(seconds, 2), period, round(pause, 2)))
        total += seconds + pause
    return plan


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--minutes", type=float, default=3.0)
    ap.add_argument("--seed", type=int, default=1, help="same seed = same sequence")
    ap.add_argument("--dev", help="/dev/input/eventN (default: found by name)")
    ap.add_argument("--dry-run", action="store_true", help="print the plan and exit")
    a = ap.parse_args(argv)
    plan = make_plan(a.minutes, a.seed)
    if a.dry_run:
        for i, (kind, percent, deg, sec, period, pause) in enumerate(plan, 1):
            print("%3d  %-8s %5.1f %%  dir %3.0f  %.2f s  period %3d ms  pause %.2f s" % (i, kind, percent, deg, sec, period, pause))
        print("%d effects, about %.1f minutes" % (len(plan), sum(p[3] + p[5] for p in plan) / 60.0))
        return 0
    dev = a.dev
    if not dev:
        with open("/proc/bus/input/devices", encoding="utf-8", errors="replace") as f:
            dev = ff_play.find_event(f.read())
    if not dev:
        print("The steering wheel was not found in /proc/bus/input/devices (is USB USER plugged in?)")
        return 2
    print("device %s: %d effects, about %.1f minutes." % (dev, len(plan), sum(p[3] + p[5] for p in plan) / 60.0))
    print("The wheel must be ARMED (LD6 solid). Hold the housing with both hands. Press Enter to start, Ctrl+C to stop.")
    try:
        input()
    except (KeyboardInterrupt, EOFError):
        print("cancelled")
        return 1
    import time
    try:
        for i, (kind, percent, deg, sec, period, pause) in enumerate(plan, 1):
            print("%3d/%d  %-8s %5.1f %%  dir %3.0f  %.2f s" % (i, len(plan), kind, percent, deg, sec))
            ff_play.play(dev, ff_play.make_effect(kind, percent, deg, sec, period), sec, out=lambda s: None)
            time.sleep(pause)
    except KeyboardInterrupt:
        print("stopped by Ctrl+C (the effect in progress was removed)")
        return 1
    print("done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
