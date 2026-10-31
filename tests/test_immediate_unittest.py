#!/usr/bin/env python3
"""Test-tool selfchecks, separate from product and sanitizer test counts.

--baseline selects the original TextTestRunner for the same interruption probe.
Its missing traceback is the observable red control, not a product defect.
"""
import io
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

from immediate_unittest import ImmediateTextRunner

BASELINE = "--baseline" in sys.argv
if BASELINE:
    sys.argv.remove("--baseline")


class DiagnosticsContracts(unittest.TestCase):
    def test_failure_traceback_survives_termination_after_next_test_starts(self):
        child = r'''
import sys, unittest
from immediate_unittest import ImmediateTextRunner
class InterruptedSuite(unittest.TestCase):
    def test_01_failure(self):
        self.assertEqual("original", "different", "ORIGINAL_ASSERTION_WITNESS")
    def test_02_wait(self):
        print("NEXT_TEST_STARTED", flush=True)
        sys.stdin.readline()
runner = unittest.TextTestRunner if sys.argv[1] == "baseline" else ImmediateTextRunner
result = runner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(InterruptedSuite))
raise SystemExit(not result.wasSuccessful())
'''
        with tempfile.TemporaryDirectory(prefix="wls-failure-flush-") as temp:
            log = Path(temp) / "stderr.log"
            with log.open("wb") as stderr:
                process = subprocess.Popen(
                    [sys.executable, "-B", "-c", child, "baseline" if BASELINE else "candidate"],
                    cwd=Path(__file__).parent, stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, stderr=stderr, text=True,
                    env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
                ready = queue.Queue()
                reader = threading.Thread(target=lambda: ready.put(process.stdout.readline()), daemon=True)
                reader.start()
                try:
                    self.assertEqual(ready.get(timeout=10), "NEXT_TEST_STARTED\n")
                    # The next test is parked on stdin, not a timing-based sleep.
                    self.assertIsNone(process.poll())
                    before_termination = log.read_text()
                    process.terminate()
                    self.assertNotEqual(process.wait(timeout=10), 0)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=10)
                    process.stdin.close()
                    process.stdout.close()
                    reader.join(timeout=10)
                after_termination = log.read_text()
            self.assertEqual(after_termination, before_termination)
            self.assertIn("Traceback (most recent call last):", after_termination)
            self.assertIn("ORIGINAL_ASSERTION_WITNESS", after_termination)
            self.assertIn("AssertionError:", after_termination)
            self.assertNotIn("Ran 2 tests", after_termination)

    def test_original_accounting_order_tracebacks_and_failfast_are_preserved(self):
        visits = []

        class Cases(unittest.TestCase):
            def test_01_failure(self):
                visits.append("failure")
                self.fail("failure witness")

            def test_02_error(self):
                visits.append("error")
                raise RuntimeError("error witness")

            def test_03_success(self):
                visits.append("success")

        for runner in (unittest.TextTestRunner, ImmediateTextRunner):
            for failfast in (False, True):
                visits.clear()
                stream = io.StringIO()
                result = runner(stream=stream, verbosity=2, failfast=failfast).run(
                    unittest.defaultTestLoader.loadTestsFromTestCase(Cases))
                self.assertFalse(result.wasSuccessful())
                self.assertEqual(result.testsRun, 1 if failfast else 3)
                self.assertEqual(len(result.failures), 1)
                self.assertEqual(len(result.errors), 0 if failfast else 1)
                self.assertEqual(visits, ["failure"] if failfast else ["failure", "error", "success"])
                self.assertIn(result.failures[0][1], stream.getvalue())
                if not failfast:
                    self.assertIn(result.errors[0][1], stream.getvalue())

    def test_subtest_header_excludes_parameters_and_preserves_both_errors(self):
        stream = io.StringIO()

        class Cases(unittest.TestCase):
            def test_subtests(self):
                with self.subTest(private_parameter="PRIVATE_SCENARIO_DATA"):
                    self.fail("subtest failure")
                with self.subTest(private_parameter="PRIVATE_SCENARIO_DATA"):
                    raise RuntimeError("subtest error")
                with self.subTest(valid=True):
                    pass

        result = ImmediateTextRunner(stream=stream).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(Cases))
        self.assertEqual(result.testsRun, 1)
        self.assertEqual(len(result.failures), 1)
        self.assertEqual(len(result.errors), 1)
        headers = [line for line in stream.getvalue().splitlines() if line.startswith("[immediate-")]
        self.assertEqual(len(headers), 2)
        self.assertTrue(all("PRIVATE_SCENARIO_DATA" not in line for line in headers))
        self.assertIn(result.failures[0][1], stream.getvalue())
        self.assertIn(result.errors[0][1], stream.getvalue())

    def test_expected_failure_and_skip_are_not_reported_as_immediate_failures(self):
        class Cases(unittest.TestCase):
            @unittest.expectedFailure
            def test_expected(self):
                self.fail("expected")

            @unittest.skip("deliberate test-tool fixture")
            def test_skipped(self):
                self.fail("unreachable")

        stream = io.StringIO()
        result = ImmediateTextRunner(stream=stream).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(Cases))
        self.assertTrue(result.wasSuccessful())
        self.assertEqual(len(result.expectedFailures), 1)
        self.assertEqual(len(result.skipped), 1)
        self.assertNotIn("[immediate-", stream.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
