#!/usr/bin/env python3
"""Print the wheel's HID input reports (axes, buttons) - stdlib only.

    python3 tools/hid_probe.py --selftest            # decode canned reports, no hardware
    python3 tools/hid_probe.py --file rec.txt        # decode recorded reports (hex lines or raw)
    python3 tools/hid_probe.py                       # live: find /dev/hidrawN (VID 0483 PID 5752)
    python3 tools/hid_probe.py --dev /dev/hidraw3    # live, explicit node
    python3 tools/hid_probe.py --status              # FFB build: read the safety status
                                                     #   feature report (ID 0x40) once
    python3 tools/hid_probe.py --record rec.txt      # live, also append hex lines to a file

Report formats:
  * default build : 8 bytes, no Report ID: X, Y, Z (uint16 LE) + 16 button bits
  * FFB build     : Report ID 0x01 + the same 8 bytes (9 bytes); other IDs are
                    FFB reports (0x02 PID state) and are shown as such.
Access to /dev/hidraw* needs the udev rule tools/99-steeringwheel.rules.
"""
import argparse
import glob
import os
import struct
import sys
import time

VID, PID = 0x0483, 0x5752
JOY_FMT = "<HHHH"
JOY_LEN = struct.calcsize(JOY_FMT)          # 8
REPORT_ID_JOYSTICK = 0x01
REPORT_ID_PID_STATE = 0x02
REPORT_ID_FFB_STATUS = 0x40
FFB_STATUS_LEN = 16                         # incl. the Report ID byte
NUM_BUTTONS = 16

# Mirrors Core/Inc/ffb/ffb_safety.h (checked by tools/tests/test_hid_probe.py).
FFB_STATES = {0: "DISARMED", 1: "PRECHECK", 2: "ARMING", 3: "RUNNING", 4: "FAULT"}
FFB_PREARM_REASONS = {
    0: "OK", 1: "ARM_NOT_ALLOWED", 2: "PROFILE_TBD", 3: "SIMULATION_BUILD", 4: "POSITION_STALE",
    5: "READ_TIMEOUT", 6: "WATCHDOG_DISABLED", 7: "WATCHDOG_TIMEOUT_RANGE", 8: "CONTROL_MODE",
    9: "CURRENT_LIM_HIGH", 10: "VEL_LIMIT_HIGH", 11: "MOTOR_NOT_CALIBRATED", 12: "ENCODER_NOT_READY",
    13: "AXIS_ERROR", 14: "MOTOR_ERROR", 15: "ENCODER_ERROR", 16: "VBUS_RANGE",
    17: "TORQUE_CONSTANT_RANGE", 18: "NOT_CHECKED_YET", 19: "READ_ERROR", 20: "NOT_IDLE", 21: "WHEEL_OUT_OF_RANGE",
}
FFB_FAULTS = {
    0: "NONE", 1: "POSITION_STALE", 2: "ODRIVE_ERROR", 3: "UART_TIMEOUT", 4: "VBUS_RANGE",
    5: "WATCHDOG_FEED_FAILED", 6: "TORQUE_INVALID", 7: "OVERSPEED", 8: "ARMING_TIMEOUT",
    9: "LOOP_OVERRUN", 10: "READ_FAILED", 11: "INTERNAL",
}
FFB_FLAGS = {0: "arm_allowed_build", 1: "precheck_ok", 2: "profile_measured", 3: "host_effects_playing",
             4: "actuators_enabled", 5: "arm_gesture_active"}


def decode_joystick(payload):
    x, y, z, buttons = struct.unpack_from(JOY_FMT, bytes(payload))
    pressed = [i + 1 for i in range(NUM_BUTTONS) if buttons & (1 << i)]
    return {"type": "joystick", "x": x, "y": y, "z": z, "buttons": buttons, "pressed": pressed}


def decode_report(data):
    """Decode one input report (bytes/list). Raises ValueError on a bad length."""
    data = bytes(data)
    if len(data) == JOY_LEN:
        r = decode_joystick(data)
        r["report_id"] = None
        return r
    if not data:
        raise ValueError("empty report")
    rid = data[0]
    if rid == REPORT_ID_JOYSTICK and len(data) >= 1 + JOY_LEN:
        r = decode_joystick(data[1:1 + JOY_LEN])
        r["report_id"] = rid
        return r
    if rid == REPORT_ID_PID_STATE and len(data) >= 3:
        return {"type": "pid_state", "report_id": rid, "status": data[1],
                "effect_playing": bool(data[2] & 0x80), "block_index": data[2] & 0x7F}
    if rid == REPORT_ID_FFB_STATUS and len(data) >= FFB_STATUS_LEN:
        return decode_ffb_status(data)
    raise ValueError("unknown report: id 0x%02X, %d bytes" % (rid, len(data)))


def decode_ffb_status(data):
    data = bytes(data)
    if len(data) < FFB_STATUS_LEN or data[0] != REPORT_ID_FFB_STATUS:
        raise ValueError("not an FFB status report")
    (rid, version, state, reason, fault, flags, torque_ma, rate_hz, loop_us_max,
     timeouts, vbus_dv) = struct.unpack_from("<BBBBBBhHHHH", data)
    return {
        "type": "ffb_status", "report_id": rid, "version": version,
        "state": FFB_STATES.get(state, "?%d" % state),
        "prearm_reason": FFB_PREARM_REASONS.get(reason, "?%d" % reason),
        "fault": FFB_FAULTS.get(fault, "?%d" % fault),
        "flags": [name for bit, name in FFB_FLAGS.items() if flags & (1 << bit)],
        "torque_cmd_amps": torque_ma / 1000.0, "command_rate_hz": rate_hz,
        "loop_time_max_us": loop_us_max, "odrive_timeouts": timeouts, "vbus_v": vbus_dv / 10.0,
    }


def format_report(r):
    if r["type"] == "joystick":
        steer = (r["x"] - 32768) / 32768.0
        bar_w = 41
        pos = int(round((steer + 1) / 2 * (bar_w - 1)))
        bar = "".join("|" if i == bar_w // 2 and i != pos else ("#" if i == pos else "-") for i in range(bar_w))
        btn = "".join(("%X" % ((i + 1) % 16)) if r["buttons"] & (1 << i) else "." for i in range(NUM_BUTTONS))
        return "X %5d [%s] %+.3f  Y %5d  Z %5d  buttons %s" % (r["x"], bar, steer, r["y"], r["z"], btn)
    if r["type"] == "pid_state":
        return "PID state: status 0x%02X, effect %d playing=%s" % (r["status"], r["block_index"], r["effect_playing"])
    if r["type"] == "ffb_status":
        return ("FFB %-9s prearm=%-22s fault=%-20s torque=%+.3f A rate=%d Hz loop_max=%d us "
                "timeouts=%d vbus=%.1f V flags=%s" % (r["state"], r["prearm_reason"], r["fault"],
                                                     r["torque_cmd_amps"], r["command_rate_hz"],
                                                     r["loop_time_max_us"], r["odrive_timeouts"], r["vbus_v"],
                                                     ",".join(r["flags"]) or "-"))
    return repr(r)


# ----------------------------------------------------------------------------
SELFTEST_CASES = [
    # (bytes, expected subset)
    (bytes([0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]),
     {"x": 32768, "y": 0, "z": 0, "buttons": 0, "report_id": None}),
    (bytes([0xFF, 0xFF, 0x34, 0x12, 0x00, 0x00, 0x01, 0x02]),
     {"x": 65535, "y": 0x1234, "buttons": 0x0201, "pressed": [1, 10]}),
    (bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x03]),
     {"x": 0, "buttons": 0x03FF, "report_id": 1, "pressed": list(range(1, 11))}),
    (bytes([0x02, 0x03, 0x85]), {"type": "pid_state", "effect_playing": True, "block_index": 5}),
    (struct.pack("<BBBBBBhHHHH", 0x40, 1, 0, 9, 0, 0x01, 0, 200, 150, 3, 241),
     {"state": "DISARMED", "prearm_reason": "CURRENT_LIM_HIGH", "fault": "NONE", "vbus_v": 24.1,
      "flags": ["arm_allowed_build"], "command_rate_hz": 200}),
]


def selftest(out=print):
    fails = 0
    for data, want in SELFTEST_CASES:
        r = decode_report(data)
        for k, v in want.items():
            if r.get(k) != v:
                fails += 1
                out("FAIL %s: %s = %r, expected %r" % (data.hex(), k, r.get(k), v))
        out(format_report(r))
    for bad in (b"", b"\x01\x02", b"\x07" * 5):
        try:
            decode_report(bad)
            fails += 1
            out("FAIL: %r accepted" % bad)
        except ValueError:
            pass
    out("selftest: %s" % ("OK" if fails == 0 else "%d FAILURES" % fails))
    return 0 if fails == 0 else 1


def iter_file_reports(path, size=None):
    """Hex lines (one report per line, '#' comments) or a raw binary file of fixed-size reports."""
    with open(path, "rb") as f:
        blob = f.read()
    try:
        text = blob.decode("ascii")
        lines = [ln.split("#", 1)[0].strip() for ln in text.splitlines()]
        lines = [ln for ln in lines if ln]
        if lines and all(all(c in "0123456789abcdefABCDEF " for c in ln) for ln in lines):
            for ln in lines:
                yield bytes.fromhex(ln.replace(" ", ""))
            return
    except UnicodeDecodeError:
        pass
    size = size or JOY_LEN
    for i in range(0, len(blob) - size + 1, size):
        yield blob[i:i + size]


def find_hidraw():
    for uevent in sorted(glob.glob("/sys/class/hidraw/hidraw*/device/uevent")):
        try:
            with open(uevent) as f:
                text = f.read().upper()
        except OSError:
            continue
        if "HID_ID=" in text and ("%04X" % VID) in text and ("%04X" % PID) in text:
            return "/dev/" + uevent.split("/")[4]
    return None


def hidiocgfeature(length):
    # _IOC(_IOC_READ|_IOC_WRITE, 'H', 0x07, len) from <linux/hidraw.h>
    return (3 << 30) | (length << 16) | (ord("H") << 8) | 0x07


def read_feature(dev, report_id, length):
    import fcntl  # Linux only; imported lazily
    buf = bytearray(length)
    buf[0] = report_id
    fd = os.open(dev, os.O_RDWR)
    try:
        n = fcntl.ioctl(fd, hidiocgfeature(length), buf, True)
    finally:
        os.close(fd)
    return bytes(buf[:n if isinstance(n, int) and n > 0 else length])


def live(dev, record=None, out=print):
    fd = os.open(dev, os.O_RDONLY)
    rec = open(record, "a") if record else None
    out("reading %s (Ctrl+C to stop)" % dev)
    last = None
    try:
        while True:
            data = os.read(fd, 64)
            if rec:
                rec.write(data.hex() + "\n")
            try:
                line = format_report(decode_report(data))
            except ValueError as e:
                line = "?? %s (%s)" % (data.hex(), e)
            if line != last:
                out(line)
                last = line
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)
        if rec:
            rec.close()
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--file", help="decode recorded reports from a file")
    ap.add_argument("--size", type=int, help="raw files: bytes per report (default 8)")
    ap.add_argument("--dev", help="hidraw node (default: auto-detect VID 0483 PID 5752)")
    ap.add_argument("--status", action="store_true", help="FFB build: read the status feature report")
    ap.add_argument("--record", help="live mode: append raw reports as hex lines")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.file:
        bad = 0
        for data in iter_file_reports(a.file, a.size):
            try:
                print(format_report(decode_report(data)))
            except ValueError as e:
                bad += 1
                print("?? %s (%s)" % (data.hex(), e))
        return 1 if bad else 0
    dev = a.dev or find_hidraw()
    if not dev:
        print("No hidraw device with VID 0483 / PID 5752 found (plugged into USB USER? udev rule installed?)")
        return 2
    if a.status:
        print(format_report(decode_ffb_status(read_feature(dev, REPORT_ID_FFB_STATUS, FFB_STATUS_LEN))))
        return 0
    return live(dev, a.record)


if __name__ == "__main__":
    sys.exit(main())
