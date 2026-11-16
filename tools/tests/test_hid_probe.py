import io
import os
import re
import struct
import tempfile
import unittest
from contextlib import redirect_stdout

import hid_probe

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FFB_SAFETY_H = os.path.join(REPO, "steeringwheel_firmware", "Core", "Inc", "ffb", "ffb_safety.h")


def c_enum(text, enum_name):
    """Parse 'typedef enum { A = 0, B, ... } enum_name;' -> {value: name-without-prefix}."""
    m = re.search(r"typedef\s+enum\s*\{([^{}]*)\}\s*%s\s*;" % enum_name, text)
    if not m:
        return None
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    out, cur = {}, -1
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" in item:
            name, val = [s.strip() for s in item.split("=")]
            cur = int(val, 0)
        else:
            name = item
            cur += 1
        out[cur] = name
    return out


class TestHidProbe(unittest.TestCase):
    def test_selftest_passes(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(hid_probe.selftest(), 0)

    def test_default_8_byte_report(self):
        r = hid_probe.decode_report(struct.pack("<HHHH", 12345, 0, 0, 0x0005))
        self.assertEqual((r["x"], r["y"], r["z"]), (12345, 0, 0))
        self.assertEqual(r["pressed"], [1, 3])
        self.assertIsNone(r["report_id"])

    def test_ffb_report_with_id(self):
        r = hid_probe.decode_report(b"\x01" + struct.pack("<HHHH", 1, 2, 3, 0x8000))
        self.assertEqual((r["x"], r["y"], r["z"]), (1, 2, 3))
        self.assertEqual(r["pressed"], [16])

    def test_truncated_and_unknown_are_rejected(self):
        for bad in (b"", b"\x01\x00", b"\x99" * 9, b"\x40" * 3):
            with self.assertRaises(ValueError):
                hid_probe.decode_report(bad)

    def test_file_mode_hex_and_raw(self):
        with tempfile.TemporaryDirectory() as d:
            hexf = os.path.join(d, "rec.txt")
            with open(hexf, "w") as f:
                f.write("# recorded\n0080000000000000\n01 ff ff 00 00 00 00 01 00\n")
            reps = [hid_probe.decode_report(b) for b in hid_probe.iter_file_reports(hexf)]
            self.assertEqual([r["x"] for r in reps], [32768, 65535])
            raw = os.path.join(d, "rec.bin")
            with open(raw, "wb") as f:
                f.write(struct.pack("<HHHH", 1, 0, 0, 0) + struct.pack("<HHHH", 2, 0, 0, 0))
            reps = [hid_probe.decode_report(b) for b in hid_probe.iter_file_reports(raw)]
            self.assertEqual([r["x"] for r in reps], [1, 2])
            with redirect_stdout(io.StringIO()):
                self.assertEqual(hid_probe.main(["--file", hexf]), 0)

    def test_ioctl_number(self):
        # HIDIOCGFEATURE(16) = 0xC0104807 on Linux
        self.assertEqual(hid_probe.hidiocgfeature(16), 0xC0104807)

    def test_status_layout_size(self):
        self.assertEqual(struct.calcsize("<BBBBBBhHHHH"), hid_probe.FFB_STATUS_LEN)

    def test_enums_match_firmware_header(self):
        if not os.path.exists(FFB_SAFETY_H):
            self.skipTest("ffb_safety.h not present")
        with open(FFB_SAFETY_H, encoding="utf-8") as f:
            text = f.read()
        for enum_name, prefix, table in (("ffb_state_t", "FFB_STATE_", hid_probe.FFB_STATES),
                                         ("ffb_reason_t", "FFB_REASON_", hid_probe.FFB_PREARM_REASONS),
                                         ("ffb_fault_t", "FFB_FAULT_", hid_probe.FFB_FAULTS)):
            c = c_enum(text, enum_name)
            self.assertIsNotNone(c, enum_name)
            c = {k: v[len(prefix):] for k, v in c.items() if not v.endswith("_COUNT")}
            self.assertEqual(c, table, enum_name)


if __name__ == "__main__":
    unittest.main()
