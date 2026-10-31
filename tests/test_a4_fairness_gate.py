#!/usr/bin/env python3
"""Lightweight output-classification checks; never execute a native probe."""
import importlib.util
from pathlib import Path
import unittest

SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fairness_gate", SOURCE / "scripts/run_a4_fairness_red_green.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
PROBE = SOURCE / "tests/repros/a4_fairness_same_source.cpp"


class FairnessEvidenceClassification(unittest.TestCase):
    def red_error(self, expression=gate.ORDER_EXPRESSIONS[0]):
        for number, line in enumerate(PROBE.read_text().splitlines(), 1):
            if line.strip() == f"CHECK({expression});":
                return f"FAIL {gate.CASE}: {PROBE}:{number}: {expression}\n"
        self.fail("frozen target assertion missing")

    def test_exact_business_order_failures_are_the_only_accepted_red(self):
        for expression in gate.ORDER_EXPRESSIONS:
            with self.subTest(expression=expression):
                self.assertEqual(gate.classify("baseline", 1, "RESULT 0 passed, 1 failed\n",
                                              self.red_error(expression), PROBE), "EXPECTED_BUSINESS_ORDER_RED")

    def test_deadline_abort_compile_and_unrelated_errors_cannot_be_red(self):
        failures = [
            (124, self.red_error()), (137, self.red_error()), (-6, self.red_error()),
            (134, self.red_error()), (2, self.red_error()), (0, self.red_error()),
            (1, "compilation terminated\n"),
            (1, f"FAIL {gate.CASE}: {PROBE}:129: std::chrono::steady_clock::now() < deadline\n"),
            (1, f"FAIL {gate.CASE}: {PROBE}:198: runtime.snapshot().outstanding == 0\n"),
            (1, self.red_error().replace(str(PROBE), "/different/source.cpp")),
            (1, self.red_error() + "additional exception\n"),
            (1, self.red_error().replace(": sequence->read()", ":99: sequence->read()")),
        ]
        for code, stderr in failures:
            with self.subTest(code=code, stderr=stderr):
                self.assertNotEqual(gate.classify("baseline", code, "RESULT 0 passed, 1 failed\n", stderr, PROBE),
                                    "EXPECTED_BUSINESS_ORDER_RED")

    def test_red_requires_original_final_accounting(self):
        for stdout in ("", "RESULT 1 passed, 0 failed\n", "RESULT 0 passed, 2 failed\n",
                       "RESULT 0 passed, 1 failed\nignored failure\n"):
            with self.subTest(stdout=stdout):
                self.assertEqual(gate.classify("baseline", 1, stdout, self.red_error(), PROBE), "NOT_TARGET_RED")

    def test_candidate_requires_zero_exit_and_exact_clean_success(self):
        stdout = f"PASS {gate.CASE}\nRESULT 1 passed, 0 failed\n"
        self.assertEqual(gate.classify("candidate", 0, stdout, "", PROBE), "EXPECTED_GREEN")
        for code, out, err in ((1, stdout, ""), (124, stdout, ""), (0, stdout, "warning\n"),
                               (0, "", ""), (0, "RESULT 1 passed, 0 failed\n", "")):
            with self.subTest(code=code, out=out, err=err):
                self.assertNotEqual(gate.classify("candidate", code, out, err, PROBE), "EXPECTED_GREEN")

    def test_probe_is_original_bytes_and_matches_current_business_case(self):
        self.assertEqual(gate.identity(PROBE)["sha256"], gate.PROBE_SHA256)
        def body(text):
            start = text.index("void queued_s1_requests_and_teams_rotate_without_starvation() {")
            return text[start:text.index("\n}\n", start) + 3]
        self.assertEqual(body(PROBE.read_text()), body((SOURCE / "tests/async_coordinator_tests.cpp").read_text()))
        self.assertIn("std::chrono::seconds(5)", PROBE.read_text())
        self.assertEqual(gate.WATCHDOG_SECONDS, 180)


if __name__ == "__main__":
    unittest.main(verbosity=2)
