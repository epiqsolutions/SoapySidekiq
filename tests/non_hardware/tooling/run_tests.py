"""Report Python tooling regressions in the same format as the C++ harness."""

from pathlib import Path
import sys
import unittest


class ReportingResult(unittest.TextTestResult):
    def addSuccess(self, test):
        super().addSuccess(test)
        print(f"[PASS] {test.id()}", flush=True)

    def addFailure(self, test, error):
        super().addFailure(test, error)
        print(f"[FAIL] {test.id()}", flush=True)

    def addError(self, test, error):
        super().addError(test, error)
        print(f"[FAIL] {test.id()}", flush=True)

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        print(f"[SKIP] {test.id()}: {reason}", flush=True)


if __name__ == "__main__":
    tests = unittest.defaultTestLoader.discover(str(Path(__file__).parent))
    result = unittest.TextTestRunner(verbosity=0, resultclass=ReportingResult).run(tests)
    failed = len(result.failures) + len(result.errors)
    passed = result.testsRun - failed - len(result.skipped)
    print(f"{passed} passed, {failed} failed, {len(result.skipped)} skipped")
    sys.exit(0 if result.wasSuccessful() else 1)
