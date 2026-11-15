"""stm_32_wheel.py needs pygame (not required for the tests): compile it and test
its pure report parser in isolation."""
import ast
import os
import struct
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "stm_32_wheel.py")


def load_parser():
    with open(SRC, encoding="utf-8") as f:
        text = f.read()
    tree = ast.parse(text)
    keep = [n for n in tree.body
            if (isinstance(n, ast.FunctionDef) and n.name == "parse_report")
            or (isinstance(n, ast.Assign) and any(getattr(t, "id", "") in
                ("REPORT_FMT", "REPORT_LEN", "JOYSTICK_REPORT_ID", "NUM_BUTTONS") for t in n.targets))]
    mod = ast.Module(body=keep, type_ignores=[])
    ns = {"struct": struct}
    exec(compile(mod, SRC, "exec"), ns)
    return ns


class TestVisualizer(unittest.TestCase):
    def test_compiles(self):
        with open(SRC, encoding="utf-8") as f:
            compile(f.read(), SRC, "exec")

    def test_parse_old_and_ffb_reports(self):
        ns = load_parser()
        parse = ns["parse_report"]
        self.assertEqual(ns["NUM_BUTTONS"], 16)
        self.assertEqual(parse(struct.pack("<HHHH", 1, 2, 3, 0x8001)), (1, 2, 3, 0x8001))
        self.assertEqual(parse(b"\x01" + struct.pack("<HHHH", 4, 5, 6, 7)), (4, 5, 6, 7))
        self.assertIsNone(parse(b"\x02\x00\x00"))
        self.assertIsNone(parse(b""))


if __name__ == "__main__":
    unittest.main()
