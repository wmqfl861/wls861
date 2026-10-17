#!/usr/bin/env python3
"""Verify the preserved v0.3 package without executing its templates.

The 33 extracted files correspond to the original ZIP's regular-file entries.
The original SHA256SUMS file covers 32 files; its own digest is pinned below,
checked against the uploaded ZIP during implementation on 2026-10-17.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys


SUMS_PATH = "06_清单与校验/SHA256SUMS.txt"
MANIFEST_PATH = "06_清单与校验/文件清单.json"
ACCEPTANCE_PATH = "02_交接与执行计划/拟定验收矩阵_未执行.csv"
SUMS_SHA256 = "4360fe06adf55bba5ad6e42ce75b5d6a34fcb3adaeb2024e07f286e39dd7642a"
ORIGINAL_FILE_COUNT = 33
SHA_ENTRY_COUNT = 32
PAYLOAD_FILE_COUNT = 31
ACCEPTANCE_CASE_COUNT = 51


class VerificationError(Exception):
    """A preserved-package invariant was violated."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def checked_file(root: Path, relative: str) -> Path:
    """Resolve only a regular, nonsymlink file strictly inside the package."""
    require(isinstance(relative, str) and bool(relative), "empty or invalid package path")
    logical = PurePosixPath(relative)
    components = relative.split("/")
    require(
        not logical.is_absolute()
        and "\\" not in relative
        and "\x00" not in relative
        and all(component not in {"", ".", ".."} for component in components),
        f"unsafe package path: {relative!r}",
    )
    path = root
    for component in components:
        path = path / component
        require(not path.is_symlink(), f"symlink inside preserved package: {relative}")
    resolved = path.resolve(strict=True)
    require(resolved.is_relative_to(root), f"path leaves preserved package: {relative}")
    require(resolved.is_file(), f"not a regular package file: {relative}")
    return resolved


def actual_file_names(root: Path) -> set[str]:
    names: set[str] = set()
    for current, directories, files in os.walk(root, followlinks=False):
        for name in directories:
            directory = Path(current) / name
            require(not directory.is_symlink(), f"symlink directory: {directory.relative_to(root)}")
        for name in files:
            relative = (Path(current) / name).relative_to(root).as_posix()
            checked_file(root, relative)
            names.add(relative)
    return names


def verify(root: Path) -> None:
    root = root.resolve(strict=True)
    require(root.is_dir(), "handoff root must be a directory")
    checksum_bytes = checked_file(root, SUMS_PATH).read_bytes()
    require(
        hashlib.sha256(checksum_bytes).hexdigest() == SUMS_SHA256,
        "original SHA256SUMS manifest has changed",
    )

    checksums: dict[str, str] = {}
    for number, line in enumerate(checksum_bytes.decode("utf-8").splitlines(), start=1):
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        require(match is not None, f"invalid checksum line {number}")
        assert match is not None
        digest, relative = match.groups()
        require(relative not in checksums, f"duplicate checksum path: {relative}")
        path = checked_file(root, relative)
        require(hashlib.sha256(path.read_bytes()).hexdigest() == digest,
                f"SHA-256 mismatch: {relative}")
        checksums[relative] = digest

    require(len(checksums) == SHA_ENTRY_COUNT, "expected 32 original SHA-256 entries")
    expected_names = set(checksums) | {SUMS_PATH}
    actual_names = actual_file_names(root)
    require(len(actual_names) == ORIGINAL_FILE_COUNT, "expected exactly 33 original package files")
    require(actual_names == expected_names, "package has missing or unexpected file paths")

    manifest = json.loads(checked_file(root, MANIFEST_PATH).read_text(encoding="utf-8"))
    require(manifest["stage"] == "design_only" and manifest["source_code_included"] is False,
            "handoff must retain its original design-only status")
    require(manifest["payload_file_count"] == PAYLOAD_FILE_COUNT,
            "original manifest must list 31 payload files")
    entries = manifest["files"]
    require(len(entries) == PAYLOAD_FILE_COUNT, "manifest payload entry count differs")
    excluded = set(manifest["manifest_excludes"])
    require(excluded == {MANIFEST_PATH, SUMS_PATH}, "unexpected manifest exclusions")
    payload_names: set[str] = set()
    for entry in entries:
        relative = entry["path"]
        require(relative not in payload_names, f"duplicate payload path: {relative}")
        payload_names.add(relative)
        path = checked_file(root, relative)
        require(path.stat().st_size == entry["size_bytes"], f"payload size differs: {relative}")
        require(checksums.get(relative) == entry["sha256"], f"manifest checksum differs: {relative}")
    require(payload_names | excluded == actual_names, "manifest does not cover the package")

    with checked_file(root, ACCEPTANCE_PATH).open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    require(len(rows) == ACCEPTANCE_CASE_COUNT, "expected 51 original acceptance cases")
    require(all(row["执行状态"] == "未执行" for row in rows),
            "original acceptance status must remain 未执行")
    require(all(row["证据"] == "" for row in rows), "original evidence cells must remain empty")
    require(len({row["用例编号"] for row in rows}) == ACCEPTANCE_CASE_COUNT,
            "acceptance case identifiers must be unique")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path,
        default=Path(__file__).resolve().parents[1] / "docs" / "handoff" / "v0.3",
        help="preserved extracted v0.3 directory (defaults to this repository)",
    )
    args = parser.parse_args()
    try:
        verify(args.root)
    except (VerificationError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"FAIL handoff: {error}", file=sys.stderr)
        return 1
    print("PASS handoff: 33 original files; 32 SHA-256 entries plus pinned manifest; "
          "31 payload entries; 51 acceptance cases remain 未执行")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
