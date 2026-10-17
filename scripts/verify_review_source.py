#!/usr/bin/env python3
"""Verify the immutable user-supplied v0.4 review source; execute no templates."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys

from verify_handoff import VerificationError, actual_file_names, checked_file, require


MANIFEST_SHA256 = "8190bb1a4743aecb6a9489984c87a98353988b6f2a0fd8422477c2404c7bc603"
ZIP_SHA256 = "f093f21e1135dbc4d2fee71f8f98ad5af701e273f3247160ae32022cf10da2e5"


def verify(repo: Path) -> None:
    manifest_path = repo / "docs/handoff/v0.4-source-manifest.json"
    manifest_bytes = manifest_path.read_bytes()
    require(hashlib.sha256(manifest_bytes).hexdigest() == MANIFEST_SHA256,
            "v0.4 source manifest differs from the reviewed original")
    manifest = json.loads(manifest_bytes)
    require(manifest["zip_sha256"] == ZIP_SHA256, "v0.4 source ZIP identity differs")
    require(manifest["file_count"] == 49, "expected 49 original v0.4 files")
    root = (repo / "docs/handoff/v0.4").resolve(strict=True)
    names: set[str] = set()
    for entry in manifest["files"]:
        relative = entry["path"]
        require(relative not in names, f"duplicate v0.4 source path: {relative}")
        names.add(relative)
        path = checked_file(root, relative)
        content = path.read_bytes()
        require(len(content) == entry["size_bytes"], f"v0.4 source size differs: {relative}")
        require(hashlib.sha256(content).hexdigest() == entry["sha256"],
                f"v0.4 source SHA-256 differs: {relative}")
    require(len(names) == 49 and actual_file_names(root) == names,
            "v0.4 source files are missing or unexpected")


def main() -> int:
    try:
        verify(Path(__file__).resolve().parents[1])
    except (VerificationError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"FAIL v0.4 review source: {error}", file=sys.stderr)
        return 1
    print("PASS v0.4 review source: 49 original files match the pinned SHA-256 manifest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
