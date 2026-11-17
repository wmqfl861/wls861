"""Only self-authored fixtures execute here; downloaded artifacts are never run."""
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "scripts"))
from check_g4d_function import EXPECTED, FIXTURE, check_output


class FunctionFixture(unittest.TestCase):
    def test_independent_vectors_and_generated_tests(self):
        clamp = runpy.run_path(str(FIXTURE / "clamp.py"))["clamp"]
        for arguments, expected in (((-4, -1, 8), -1), ((3, -1, 8), 3), ((9, -1, 8), 8), ((0, 6, 6), 6)):
            self.assertEqual(clamp(*arguments), expected)
        with self.assertRaises(ValueError):
            clamp(1, 2, -2)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in EXPECTED:
                shutil.copyfile(FIXTURE / name, root / name)
            run = subprocess.run([sys.executable, "-B", "test_clamp.py"], cwd=root, capture_output=True, timeout=5)
            self.assertTrue(check_output(root, run.returncode, run.stdout, run.stderr)["passed"])
            self.assertFalse(check_output(root, 1, run.stdout, run.stderr)["passed"])
            self.assertFalse(check_output(root, 0, run.stdout, run.stderr.replace(b"5 tests", b"4 tests"))["passed"])
            (root / "extra.txt").touch()
            self.assertFalse(check_output(root, 0, run.stdout, run.stderr)["passed"])
            (root / "extra.txt").unlink()
            (root / "clamp.py").unlink()
            (root / "clamp.py").symlink_to(FIXTURE / "clamp.py")
            self.assertFalse(check_output(root, 0, run.stdout, run.stderr)["passed"])
            (root / "clamp.py").unlink()
            (root / "clamp.py").write_text("def clamp(value, lower, upper):\n    return value\n")
            wrong = subprocess.run([sys.executable, "-B", "test_clamp.py"], cwd=root, capture_output=True, timeout=5)
            self.assertNotEqual(wrong.returncode, 0)
            self.assertFalse(check_output(root, wrong.returncode, wrong.stdout, wrong.stderr)["passed"])


if __name__ == "__main__":
    unittest.main()
