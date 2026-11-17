#!/usr/bin/env python3
"""Play ONE force-feedback effect on the steering wheel (Linux evdev), with a strength you choose.

Why: fftest uses fixed, weak levels (25 %) and needs the right /dev/input/eventN. This tool finds the wheel by its name and
plays a constant force, a sine / square vibration (texture, rumble strips, gravel) or a short pulse (a crash).

The firmware still caps the TOTAL force at ffb.max_amps; this tool cannot exceed it. The wheel must be ARMED (B1 gesture) or
nothing will be felt (the firmware never applies torque while DISARMED).

    sudo python3 tools/ff_play.py constant --percent 60 --dir-deg 90 --seconds 3
    sudo python3 tools/ff_play.py sine --percent 50 --period-ms 80 --seconds 4     # texture (about 12 Hz)
    sudo python3 tools/ff_play.py pulse --percent 100 --dir-deg 90                  # a short bump / crash

--dir-deg is the USB PID direction: 90 = force along +X (HID X increases), 270 = along -X. Standard library only.
"""
import argparse
import ctypes
import fcntl
import os
import re
import struct
import sys
import time

DEVICE_NAME = "Steering Wheel Controller"

EV_FF = 0x15
FF_PERIODIC, FF_CONSTANT = 0x51, 0x52
FF_SQUARE, FF_SINE = 0x58, 0x5A
FF_GAIN = 0x60
MAX_SECONDS = 10.0


def _ioc(direction, kind, nr, size):
    return (direction << 30) | (size << 16) | (ord(kind) << 8) | nr


class FFEnvelope(ctypes.Structure):
    _fields_ = [("attack_length", ctypes.c_uint16), ("attack_level", ctypes.c_uint16),
                ("fade_length", ctypes.c_uint16), ("fade_level", ctypes.c_uint16)]


class FFConstant(ctypes.Structure):
    _fields_ = [("level", ctypes.c_int16), ("envelope", FFEnvelope)]


class FFPeriodic(ctypes.Structure):
    _fields_ = [("waveform", ctypes.c_uint16), ("period", ctypes.c_uint16), ("magnitude", ctypes.c_int16),
                ("offset", ctypes.c_int16), ("phase", ctypes.c_uint16), ("envelope", FFEnvelope),
                ("custom_len", ctypes.c_uint32), ("custom_data", ctypes.c_void_p)]


class FFUnion(ctypes.Union):
    _fields_ = [("constant", FFConstant), ("periodic", FFPeriodic), ("pad", ctypes.c_ubyte * 32)]


class FFTrigger(ctypes.Structure):
    _fields_ = [("button", ctypes.c_uint16), ("interval", ctypes.c_uint16)]


class FFReplay(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint16), ("delay", ctypes.c_uint16)]


class FFEffect(ctypes.Structure):
    """struct ff_effect from <linux/input.h> (48 bytes on 64-bit)."""
    _fields_ = [("type", ctypes.c_uint16), ("id", ctypes.c_int16), ("direction", ctypes.c_uint16),
                ("trigger", FFTrigger), ("replay", FFReplay), ("u", FFUnion)]


EVIOCSFF = _ioc(1, "E", 0x80, ctypes.sizeof(FFEffect))     # _IOW('E', 0x80, struct ff_effect)
EVIOCRMFF = _ioc(1, "E", 0x81, ctypes.sizeof(ctypes.c_int))  # _IOW('E', 0x81, int)
EVENT_FMT = "llHHi"                                         # struct input_event (64-bit)


def find_event(text):
    """Return '/dev/input/eventN' of the wheel from the text of /proc/bus/input/devices, or None."""
    for block in text.split("\n\n"):
        if ('Name="' in block) and (DEVICE_NAME in block):
            m = re.search(r"Handlers=.*?\bevent(\d+)", block)
            if m:
                return "/dev/input/event" + m.group(1)
    return None


def percent_to_level(percent):
    p = max(0.0, min(100.0, float(percent)))
    return int(round(p / 100.0 * 32767.0))


def deg_to_direction(deg):
    return int(round((float(deg) % 360.0) / 360.0 * 65536.0)) & 0xFFFF


def make_effect(kind, percent, dir_deg, seconds, period_ms):
    e = FFEffect()
    e.id = -1
    e.direction = deg_to_direction(dir_deg)
    e.replay.length = int(min(max(seconds, 0.05), MAX_SECONDS) * 1000)
    level = percent_to_level(percent)
    if kind in ("constant", "pulse"):
        e.type = FF_CONSTANT
        e.u.constant.level = level
    elif kind in ("sine", "square"):
        e.type = FF_PERIODIC
        e.u.periodic.waveform = FF_SINE if kind == "sine" else FF_SQUARE
        e.u.periodic.period = max(5, min(int(period_ms), 65535))
        e.u.periodic.magnitude = level
    else:
        raise ValueError("unknown effect kind: %r" % kind)
    return e


def pack_event(code, value, sec=0, usec=0):
    return struct.pack(EVENT_FMT, sec, usec, EV_FF, code, value)


def play(dev, effect, seconds, out=print):
    fd = os.open(dev, os.O_RDWR)
    eid = None
    try:
        os.write(fd, pack_event(FF_GAIN, 0xFFFF))               # device gain 100 %: the firmware cap is the limit
        buf = bytearray(ctypes.string_at(ctypes.byref(effect), ctypes.sizeof(effect)))
        fcntl.ioctl(fd, EVIOCSFF, buf, True)
        eid = struct.unpack_from("<h", buf, 2)[0]
        out("effect uploaded (id %d), playing for %.2f s ..." % (eid, seconds))
        os.write(fd, pack_event(eid, 1))
        time.sleep(seconds)
    finally:
        if eid is not None:
            try:
                os.write(fd, pack_event(eid, 0))
                fcntl.ioctl(fd, EVIOCRMFF, eid)
            except OSError:
                pass
        os.close(fd)
    out("done, effect removed.")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", nargs="?", choices=["constant", "sine", "square", "pulse"])
    ap.add_argument("--percent", type=float, default=30.0, help="strength 0..100 of the full scale (default 30)")
    ap.add_argument("--dir-deg", type=float, default=90.0, help="PID direction in degrees (90 = +X, 270 = -X)")
    ap.add_argument("--seconds", type=float, default=3.0, help="duration (max %.0f s)" % MAX_SECONDS)
    ap.add_argument("--period-ms", type=int, default=100, help="sine/square period in ms (100 = 10 Hz)")
    ap.add_argument("--dev", help="/dev/input/eventN (default: found by name)")
    ap.add_argument("--selftest", action="store_true", help="print the struct size and ioctl numbers, touch no device")
    a = ap.parse_args(argv)
    if a.selftest:
        print("sizeof(ff_effect) = %d, EVIOCSFF = 0x%08X, EVIOCRMFF = 0x%08X"
              % (ctypes.sizeof(FFEffect), EVIOCSFF, EVIOCRMFF))
        return 0
    if not a.kind:
        ap.error("choose an effect: constant, sine, square or pulse")
    dev = a.dev
    if not dev:
        with open("/proc/bus/input/devices", encoding="utf-8", errors="replace") as f:
            dev = find_event(f.read())
    if not dev:
        print("The steering wheel was not found in /proc/bus/input/devices (is USB USER plugged in?)")
        return 2
    seconds = 0.15 if a.kind == "pulse" else a.seconds
    print("device %s, effect %s, %.0f %% of the full scale, direction %.0f deg, %.2f s"
          % (dev, a.kind, a.percent, a.dir_deg, seconds))
    print("The wheel must be ARMED (LD6 solid). Hold the housing firmly. Press Enter to play, Ctrl+C to cancel.")
    try:
        input()
    except (KeyboardInterrupt, EOFError):
        print("cancelled")
        return 1
    play(dev, make_effect(a.kind, a.percent, a.dir_deg, seconds, a.period_ms), seconds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
