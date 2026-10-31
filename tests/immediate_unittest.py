"""Test-only unittest diagnostics that survive a later suite timeout.

Normal unittest accounting, failfast, test order and final reporting are retained.
Only already registered failures/errors are emitted early; no locals, environment
or fixture state is added. This is not a product error-reporting interface.
"""
import re
import unittest


class ImmediateTextResult(unittest.TextTestResult):
    def _emit(self, kind, test, formatted):
        # A subtest's parameters may contain fixture data. Identify its parent
        # method only; the traceback itself is unittest's original stored text.
        parent = getattr(test, "test_case", test)
        scenario = re.sub(r"[^A-Za-z0-9_.-]", "_", parent.id())[:240]
        self.stream.write(f"\n[immediate-unittest {kind} {scenario}]\n")
        self.stream.write(formatted)
        self.stream.flush()

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._emit("FAIL", test, self.failures[-1][1])

    def addError(self, test, err):
        super().addError(test, err)
        self._emit("ERROR", test, self.errors[-1][1])

    def addSubTest(self, test, subtest, err):
        failures, errors = len(self.failures), len(self.errors)
        super().addSubTest(test, subtest, err)
        if len(self.failures) > failures:
            self._emit("FAIL", subtest, self.failures[-1][1])
        elif len(self.errors) > errors:
            self._emit("ERROR", subtest, self.errors[-1][1])


class ImmediateTextRunner(unittest.TextTestRunner):
    resultclass = ImmediateTextResult
