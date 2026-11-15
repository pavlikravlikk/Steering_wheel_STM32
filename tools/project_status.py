#!/usr/bin/env python3
"""Project status: what is still TBD, what blocks which bring-up step.

    python3 tools/project_status.py                 # print
    python3 tools/project_status.py --no-tests      # skip running the test suites
    python3 tools/project_status.py --write-status  # also refresh the generated
                                                    #   section of docs/STATUS.md
    make status                                     # same as --write-status

Read-only towards git (only `git log`) and never touches hardware.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import gen_config  # noqa: E402

STATUS_MD = os.path.join(REPO, "docs", "STATUS.md")
BEGIN = "<!-- BEGIN GENERATED: tools/project_status.py -->"
END = "<!-- END GENERATED: tools/project_status.py -->"
CODE_DIRS = [os.path.join(REPO, "steeringwheel_firmware", "Core"), HERE]

# Bring-up steps (docs/HARDWARE_BRINGUP.md) and the profile entries / hardware they need.
STEPS = [
    ("H0 host checks (tools/check_all.sh)", [], []),
    ("H1 HID path, simulation build", [], ["STM32F3Discovery on USB USER"]),
    ("H2 ODrive doctor (read-only dump)", [], ["ODrive on 24 V + USB"]),
    ("H3 UART wiring + position stream", ["pins.uart_tx", "pins.uart_rx", "uart.baud"], ["UART wires"]),
    ("H4 encoder counts per turn (belt off)", ["encoder.ppr", "encoder.quadrature"], []),
    ("H5 belt ratio / cpr (belt on)", ["belt.teeth_housing", "belt.teeth_pulley",
                                       "motor.motor_turns_per_wheel_rev"], ["timing belt installed"]),
    ("H6 gentle ODrive profile (odrive_setup.py)", ["odrive.current_lim", "odrive.calibration_current",
                                                    "odrive.vel_limit", "odrive.dc_bus_overvoltage_trip_level",
                                                    "odrive.dc_bus_undervoltage_trip_level"], []),
    ("H7 motor + encoder offset calibration", ["belt.teeth_housing", "belt.teeth_pulley", "motor.pole_pairs",
                                               "odrive.calibration_current"], ["timing belt installed"]),
    ("H8 steering direction / lock-to-lock", ["wheel.invert", "wheel.lock_to_lock_deg"], []),
    ("H9 MCP23017 buttons", ["mcp23017.address_7bit", "pins.i2c"], ["MCP23017 board"]),
    ("F1+ force feedback bring-up (docs/FFB_SAFETY.md)", "SAFETY", ["hardware e-stop", "wheel dismounted for F1"]),
]


def find_todo_tbd():
    hits = []
    for d in CODE_DIRS:
        for root, dirs, files in os.walk(d):
            dirs[:] = [x for x in dirs if x not in ("__pycache__", "build", "generated", "tests")]
            for fn in sorted(files):
                if not fn.endswith((".c", ".h", ".py")):
                    continue
                path = os.path.join(root, fn)
                if os.path.abspath(path) == os.path.abspath(__file__):
                    continue
                try:
                    with open(path, encoding="utf-8", errors="replace") as f:
                        for no, line in enumerate(f, 1):
                            if "TODO(TBD)" in line:
                                hits.append((os.path.relpath(path, REPO), no, line.strip()[:110]))
                except OSError:
                    pass
    return hits


def run(cmd, cwd=REPO, timeout=600):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        return 127, str(e)


def test_summary():
    res = []
    rc, out = run(["make", "-C", "tests"])
    m = re.findall(r"^\d+ tests, \d+ failed.*$", out, re.M)
    res.append(("host C tests", rc == 0, m[-1] if m else "no summary (rc %d)" % rc))
    rc, out = run([sys.executable, "-m", "unittest", "discover", "-s", "tools/tests", "-t", "tools"])
    m = re.findall(r"^Ran \d+ tests? in .*$", out, re.M)
    tail = out.strip().splitlines()[-1] if out.strip() else ""
    res.append(("host Python tests", rc == 0, (m[-1] + ", " + tail) if m else "rc %d" % rc))
    return res


def build_report(profile, with_tests=True):
    L = []
    w = L.append
    tbd = gen_config.tbd_entries(profile)
    safety = set(gen_config.tbd_entries(profile, gen_config.SAFETY_SECTIONS))
    w("### Profile entries still TBD (%d)" % len(tbd))
    w("")
    w("| entry | value | how to measure |")
    w("|---|---|---|")
    for name in tbd:
        s, k = name.split(".", 1)
        e = profile[s][k]
        w("| `%s` | `%s` | %s |" % (name, e["value"], e["how_to_measure"].replace("|", "/")))
    w("")
    w("### Bring-up steps and what blocks them")
    w("")
    w("| step | blocked by (TBD) | hardware needed |")
    w("|---|---|---|")
    for title, needs, hw in STEPS:
        needs_list = sorted(safety) if needs == "SAFETY" else needs
        blocking = [n for n in needs_list if n in tbd]
        if needs == "SAFETY" and blocking:
            btxt = "%d safety entries TBD" % len(blocking)
        else:
            btxt = ", ".join("`%s`" % b for b in blocking) or "-"
        w("| %s | %s | %s |" % (title, btxt, ", ".join(hw) or "-"))
    w("")
    hits = find_todo_tbd()
    w("### `TODO(TBD)` markers in code (%d)" % len(hits))
    w("")
    for path, no, line in hits:
        w("- `%s:%d` %s" % (path, no, line.replace("`", "'")))
    w("")
    if with_tests:
        w("### Tests")
        w("")
        for name, ok, summary in test_summary():
            w("- %s: **%s** - %s" % (name, "PASS" if ok else "FAIL", summary))
        w("")
    rc, out = run(["git", "log", "--oneline", "-n", "12"])
    w("### Last commits")
    w("")
    w("```")
    w(out.strip() if rc == 0 else "(git log unavailable)")
    w("```")
    return "\n".join(L) + "\n"


def write_status(text):
    try:
        with open(STATUS_MD, encoding="utf-8") as f:
            doc = f.read()
    except OSError:
        doc = "# Status\n"
    block = "%s\n_Generated by `make status`; do not edit by hand._\n\n%s%s" % (BEGIN, text, END)
    if BEGIN in doc and END in doc:
        pre = doc.split(BEGIN)[0]
        post = doc.split(END, 1)[1]
        doc = pre + block + post
    else:
        doc = doc.rstrip() + "\n\n## Generated status\n\n" + block + "\n"
    with open(STATUS_MD, "w", encoding="utf-8") as f:
        f.write(doc)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-tests", action="store_true")
    ap.add_argument("--write-status", action="store_true")
    ap.add_argument("--profile", default=gen_config.DEFAULT_PROFILE)
    a = ap.parse_args(argv)
    profile = gen_config.load_profile(a.profile)
    text = build_report(profile, with_tests=not a.no_tests)
    print(text)
    if a.write_status:
        write_status(text)
        print("docs/STATUS.md updated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
