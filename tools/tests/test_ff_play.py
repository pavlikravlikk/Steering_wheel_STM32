"""Host tests for tools/ff_play.py (no device is opened)."""
import ctypes
import struct
import sys
import unittest

import ff_play

DEVICES = """I: Bus=0003 Vendor=046d Product=c52b Version=0111
N: Name="Logitech USB Receiver"
H: Handlers=sysrq kbd event3 leds

I: Bus=0003 Vendor=0483 Product=5752 Version=0111
N: Name="STMicroelectronics Steering Wheel Controller"
P: Phys=usb-0000:00:14.0-4/input0
H: Handlers=event14 js0
B: PROP=0
"""


class TestFfPlay(unittest.TestCase):
    @unittest.skipUnless(ctypes.sizeof(ctypes.c_void_p) == 8, "layout checked for 64-bit Linux")
    def test_struct_size_and_ioctl_numbers(self):
        self.assertEqual(ctypes.sizeof(ff_play.FFEffect), 48)
        self.assertEqual(ff_play.EVIOCSFF, 0x40304580)     # _IOW('E', 0x80, struct ff_effect)
        self.assertEqual(ff_play.EVIOCRMFF, 0x40044581)    # _IOW('E', 0x81, int)
        self.assertEqual(struct.calcsize(ff_play.EVENT_FMT), 24)

    def test_field_offsets_match_linux_input_h(self):
        E = ff_play.FFEffect
        self.assertEqual(E.id.offset, 2)
        self.assertEqual(E.direction.offset, 4)
        self.assertEqual(E.trigger.offset, 6)
        self.assertEqual(E.replay.offset, 10)
        self.assertEqual(E.u.offset, 16)
        P = ff_play.FFPeriodic
        self.assertEqual((P.waveform.offset, P.period.offset, P.magnitude.offset, P.offset.offset, P.phase.offset),
                         (0, 2, 4, 6, 8))

    def test_find_event_by_name(self):
        self.assertEqual(ff_play.find_event(DEVICES), "/dev/input/event14")
        self.assertIsNone(ff_play.find_event("N: Name=\"Other\"\nH: Handlers=event1\n"))

    def test_conversions(self):
        self.assertEqual(ff_play.percent_to_level(100), 32767)
        self.assertEqual(ff_play.percent_to_level(250), 32767)     # clamped
        self.assertEqual(ff_play.percent_to_level(-5), 0)
        self.assertEqual(ff_play.percent_to_level(50), 16384)
        self.assertEqual(ff_play.deg_to_direction(90), 0x4000)
        self.assertEqual(ff_play.deg_to_direction(270), 0xC000)
        self.assertEqual(ff_play.deg_to_direction(360), 0)

    def test_make_effect(self):
        e = ff_play.make_effect("constant", 60, 90, 3, 100)
        self.assertEqual((e.type, e.id, e.direction, e.replay.length), (ff_play.FF_CONSTANT, -1, 0x4000, 3000))
        self.assertEqual(e.u.constant.level, ff_play.percent_to_level(60))
        s = ff_play.make_effect("sine", 40, 90, 2, 80)
        self.assertEqual((s.type, s.u.periodic.waveform, s.u.periodic.period), (ff_play.FF_PERIODIC, ff_play.FF_SINE, 80))
        q = ff_play.make_effect("square", 40, 90, 2, 1)
        self.assertEqual(q.u.periodic.waveform, ff_play.FF_SQUARE)
        self.assertEqual(q.u.periodic.period, 5)                   # period floor
        long = ff_play.make_effect("constant", 10, 90, 999, 100)
        self.assertEqual(long.replay.length, int(ff_play.MAX_SECONDS * 1000))   # duration cap
        with self.assertRaises(ValueError):
            ff_play.make_effect("spring", 10, 90, 1, 100)

    def test_event_packing(self):
        raw = ff_play.pack_event(ff_play.FF_GAIN, 0xFFFF)
        sec, usec, typ, code, val = struct.unpack(ff_play.EVENT_FMT, raw)
        self.assertEqual((typ, code, val), (ff_play.EV_FF, ff_play.FF_GAIN, 0xFFFF))

    def test_selftest_and_missing_device(self):
        self.assertEqual(ff_play.main(["--selftest"]), 0)


if __name__ == "__main__":
    sys.exit(unittest.main())
