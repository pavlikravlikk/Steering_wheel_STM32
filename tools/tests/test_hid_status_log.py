import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import hid_probe  # noqa: E402
import hid_status_log  # noqa: E402


def status(state=0, reason=0, fault=0, flags=0, ma=0, rate=200, loop=150, to=0, dv=241):
    return hid_probe.decode_ffb_status(struct.pack("<BBBBBBhHHHH", 0x40, 1, state, reason, fault, flags, ma, rate, loop, to, dv))


class FakeDevice:
    def __init__(self, reports):
        self.reports = list(reports)
        self.closed = False

    def open_path(self, path):
        pass

    def get_feature_report(self, rid, n):
        if getattr(self, "exact", None) and n != self.exact:
            raise OSError("wrong length")
        if not self.reports:
            raise OSError("gone")
        item = self.reports.pop(0)
        if isinstance(item, Exception):
            raise item
        return list(item)

    def close(self):
        self.closed = True


class FakeHid:
    def __init__(self, devices):
        self.devices = list(devices)

    def enumerate(self, vid, pid):
        return [{"path": b"p%d" % i} for i in range(len(self.devices))]

    def device(self):
        return self.devices.pop(0) if self.devices else FakeDevice([])



class TestMonitor(unittest.TestCase):
    def test_logs_only_changes_and_the_fault_with_history(self):
        m = hid_status_log.Monitor()
        lines = []
        for i in range(5):
            lines += m.feed(100.0 + i * 0.05, status(state=3, flags=0x11, ma=2000 + i, dv=241 + i))
        self.assertEqual(sum("FFB RUNNING" in l for l in lines), 1)          # one line for the whole stable stretch
        lines2 = m.feed(101.0, status(state=4, fault=4, flags=0x01, dv=290))
        text = "\n".join(lines2)
        self.assertIn("last samples before the FAULT", text)
        self.assertIn("*** FAULT = VBUS_RANGE ***", text)
        self.assertEqual(sum(l.startswith("    ") for l in lines2), 5)       # the 5 samples before it

    def test_summary_every_10_s(self):
        m = hid_status_log.Monitor()
        m.feed(0.0, status())
        out = m.feed(10.5, status(ma=-3000, dv=250))
        self.assertTrue(any("summary: peak" in l for l in out))

    def test_lost_and_back(self):
        m = hid_status_log.Monitor()
        m.feed(0.0, status())
        self.assertEqual(len(m.lost(1.0)), 1)
        self.assertEqual(m.lost(2.0), [])                                      # not repeated
        self.assertTrue(any("connected" in l for l in m.feed(3.0, status())))


class TestRun(unittest.TestCase):
    def test_reads_and_survives_a_unplug(self):
        good = struct.pack("<BBBBBBhHHHH", 0x40, 1, 3, 0, 0, 0x11, 500, 200, 150, 0, 241)
        fault = struct.pack("<BBBBBBhHHHH", 0x40, 1, 4, 0, 7, 0x01, 0, 200, 150, 0, 241)
        h = FakeHid([FakeDevice([good, good, fault, OSError("unplugged")])])
        out = []
        t = [0.0]

        def clock():
            t[0] += 0.05
            return t[0]

        rc = hid_status_log.run(h, None, 0.0, out=out.append, sleep=lambda s: None, clock=clock, max_loops=6)
        self.assertEqual(rc, 0)
        text = "\n".join(out)
        self.assertIn("FFB RUNNING", text)
        self.assertIn("*** FAULT = OVERSPEED ***", text)
        self.assertIn("wheel NOT readable", text)

    def test_finds_the_length_windows_wants(self):
        good = struct.pack("<BBBBBBhHHHH", 0x40, 1, 3, 0, 0, 0x11, 500, 200, 150, 0, 241)
        d = FakeDevice([good, good])
        d.exact = 33
        out = []
        self.assertEqual(hid_status_log.diagnose(FakeHid([d]), out=out.append), 0)
        self.assertTrue(any("(0x40, 33): OK" in l for l in out))
        self.assertTrue(any("wrong length" in l for l in out))

    def test_diag_reports_failure(self):
        d = FakeDevice([])
        out = []
        self.assertEqual(hid_status_log.diagnose(FakeHid([d]), out=out.append), 2)
        self.assertTrue(any("RESULT: the status report could not be read" in l for l in out))

    def test_hidraw_adapter_reports_no_device_cleanly(self):
        self.assertEqual(hid_status_log.HidrawModule.enumerate(0x0483, 0x5752) is not None, True)

    def test_once_without_device_returns_2(self):
        self.assertEqual(hid_status_log.run(FakeHid([]), None, 0.0, once=True, out=lambda s: None), 2)


if __name__ == "__main__":
    unittest.main()
