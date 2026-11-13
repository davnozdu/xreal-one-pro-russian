"""Run the installer test suite, then exit hard.

Two macOS-specific guards:

* os._exit at the end returns the real pass/fail status while skipping
  interpreter finalization. Tearing down Tk together with hidapi's
  IOHIDManager can otherwise hang (or hit a GIL race) long after every
  test has already passed.
* test_layout.py is excluded by default. It creates and destroys several
  Tk roots at tiny sizes and high DPI; on some macOS Tk builds that
  teardown hangs non-deterministically (it has also passed cleanly on the
  same machine), so it is unfit as an automated build gate. App
  construction is still covered end to end by app.py --self-test. Run the
  GUI test explicitly with:  python run_tests.py test_layout.py

Usage:
  python run_tests.py                 # all test_*.py except the GUI layout test
  python run_tests.py test_foo.py ... # only the named pattern(s)
"""
import os
import sys
import unittest

EXCLUDE_BY_DEFAULT = {'test_layout'}


def _flatten(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from _flatten(item)
        else:
            yield item


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    loader = unittest.TestLoader()
    patterns = sys.argv[1:]
    if patterns:
        suite = unittest.TestSuite()
        for pattern in patterns:
            suite.addTests(loader.discover(here, pattern=pattern))
    else:
        found = loader.discover(here, pattern='test_*.py')
        suite = unittest.TestSuite(
            test for test in _flatten(found)
            if test.__class__.__module__ not in EXCLUDE_BY_DEFAULT)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(0 if result.wasSuccessful() else 1)


if __name__ == '__main__':
    main()
