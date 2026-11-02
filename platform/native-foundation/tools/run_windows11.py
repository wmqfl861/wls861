#!/usr/bin/env python3
"""Validate the existing four native component suites on an ordinary Windows 11 host.

This performs the pinned offline build and retains full original evidence. It
does not accept the full Windows runtime, ACL security or sandbox capability.
"""
import argparse
from pathlib import Path

from run_windows_ci import run_native


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    run_native(args.source_root, args.work_root, windows11=True)


if __name__ == '__main__':
    main()
