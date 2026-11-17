"""The decoder inside tools/wheel_status.html must agree with tools/hid_probe.py (needs node; skipped without it)."""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import unittest

import hid_probe

HTML = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "wheel_status.html")
NODE = shutil.which("node") or shutil.which("nodejs")


def js_decode(raw):
    with open(HTML, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"// BEGIN DECODE.*?\n(.*?)// END DECODE", text, re.S)
    assert m, "decode block markers not found"
    script = m.group(1) + "\nconst raw = %s;\nconsole.log(JSON.stringify({d: decodeStatus(Uint8Array.from(raw)), "
    script += "tables: {STATES, REASONS, FAULTS, FLAGS}}));\n"
    out = subprocess.run([NODE, "-e", script % json.dumps(list(raw))], capture_output=True, text=True, timeout=30)
    assert out.returncode == 0, out.stderr
    return json.loads(out.stdout)


@unittest.skipUnless(NODE, "node is not installed")
class TestWheelStatusHtml(unittest.TestCase):
    SAMPLES = [
        struct.pack("<BBBBBBhHHHH", 0x40, 1, 0, 9, 0, 0x01, 0, 200, 150, 3, 241),
        struct.pack("<BBBBBBhHHHH", 0x40, 1, 3, 0, 0, 0x1D, -800, 200, 412, 311, 242),     # RUNNING, effects playing
        struct.pack("<BBBBBBhHHHH", 0x40, 1, 4, 20, 1, 0x15, 0, 200, 65535, 8912, 0),      # FAULT, not idle
    ]

    def test_decoder_matches_hid_probe(self):
        for raw in self.SAMPLES:
            py = hid_probe.decode_ffb_status(raw)
            for variant in (raw, raw[1:]):                 # with and without the report ID byte
                js = js_decode(variant)["d"]
                self.assertEqual(js["state"], py["state"])
                self.assertEqual(js["reason"], py["prearm_reason"])
                self.assertEqual(js["fault"], py["fault"])
                self.assertEqual(js["flags"], py["flags"])
                self.assertAlmostEqual(js["torqueA"], py["torque_cmd_amps"], places=6)
                self.assertEqual(js["rateHz"], py["command_rate_hz"])
                self.assertEqual(js["loopUsMax"], py["loop_time_max_us"])
                self.assertEqual(js["timeouts"], py["odrive_timeouts"])
                self.assertAlmostEqual(js["vbusV"], py["vbus_v"], places=6)

    def test_tables_match_hid_probe(self):
        t = js_decode(self.SAMPLES[0])["tables"]
        self.assertEqual({int(k): v for k, v in t["STATES"].items()}, hid_probe.FFB_STATES)
        self.assertEqual({int(k): v for k, v in t["REASONS"].items()}, hid_probe.FFB_PREARM_REASONS)
        self.assertEqual({int(k): v for k, v in t["FAULTS"].items()}, hid_probe.FFB_FAULTS)
        self.assertEqual({int(k): v for k, v in t["FLAGS"].items()}, hid_probe.FFB_FLAGS)

    def test_short_report_is_rejected(self):
        with self.assertRaises(AssertionError):
            js_decode(bytes([0x40, 1, 2]))


if __name__ == "__main__":
    sys.exit(unittest.main())
