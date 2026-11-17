"""Parser/oracle fixtures only; these tests make no sandbox execution claim."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
FIXTURE = SOURCE / "tests/fixtures/g4"
sys.path.insert(0, str(SOURCE / "scripts"))
from check_g4d_csv import EXPECTED, check_output


class CsvFixture(unittest.TestCase):
    def test_parser_uses_original_bytes(self):
        original = (FIXTURE / "input/orders.csv").read_bytes()
        lines = original.decode().replace("华东,1200", "华东,1201").splitlines()
        changed = (lines[0] + "\n" + "\n".join(reversed(lines[1:])) + "\n").encode()
        for raw, expected in ((original, EXPECTED), (changed, EXPECTED.replace("4100", "4101"))):
            with self.subTest(raw=raw), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "orders.csv").write_bytes(raw)
                run = subprocess.run([sys.executable, str(FIXTURE / "csv-transform.py"),
                                      str(root / "orders.csv"), str(root / "summary.csv")],
                                     capture_output=True, check=True, timeout=5)
                self.assertEqual(run.stderr, b"")
                self.assertEqual((root / "summary.csv").read_bytes(), expected.encode())
                self.assertEqual((root / "orders.csv").read_bytes(), raw)
                self.assertEqual(json.loads(run.stdout)["input_sha256"], hashlib.sha256(raw).hexdigest())

    def test_oracle_rejects_invalid_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "summary.csv"
            output.write_text(EXPECTED, encoding="utf-8")
            self.assertTrue(check_output(root)["passed"])
            for invalid in (EXPECTED.replace("4100", "4101"), '"unterminated', EXPECTED[:-1], ""):
                output.write_text(invalid, encoding="utf-8")
                self.assertFalse(check_output(root)["passed"])
            output.write_text(EXPECTED, encoding="utf-8")
            extra = root / "extra.txt"
            extra.write_text("unexpected")
            self.assertFalse(check_output(root)["passed"])
            extra.unlink()
            output.unlink()
            output.symlink_to(FIXTURE / "csv-expected.csv")
            self.assertFalse(check_output(root)["passed"])


if __name__ == "__main__":
    unittest.main()
