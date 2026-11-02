#!/usr/bin/env python3
"""Run a fresh, full GCC 13.3 Debug ASan/LSan gate; never reinterpret old runs."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET

DEFAULT_EXPECTED_TESTS = 92

ASAN = "halt_on_error=1:detect_leaks=1"
UBSAN = "halt_on_error=1:print_stacktrace=1"
STEP_NAMES = {"compiler-version", "compiler-banner", "cmake-version", "python-dependencies",
              "mount", "configure", "build", "test-plan", "ctest"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path: Path, value: object) -> None:
    # Each run owns a new directory. Atomic replacement never touches old runs.
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def utc() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def clean_environment(original: dict[str, str]) -> dict[str, str]:
    # Do not inherit provider secrets, flags, launchers, preload or suppressions.
    env = {k: original[k] for k in ("PATH", "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "TZ") if k in original}
    env.update(ASAN_OPTIONS=ASAN, UBSAN_OPTIONS=UBSAN,
               PYTHONNOUSERSITE="1", PYTHONDONTWRITEBYTECODE="1")
    return env


def configure_command(source: Path, build: Path, compiler: str, python: str) -> list[str]:
    return ["cmake", "-S", str(source), "-B", str(build), "-G", "Unix Makefiles",
            "-DCMAKE_BUILD_TYPE=Debug", f"-DCMAKE_CXX_COMPILER={compiler}",
            "-DCMAKE_CXX_FLAGS=", "-DCMAKE_CXX_FLAGS_DEBUG=-g",
            "-DCMAKE_EXE_LINKER_FLAGS=", "-DCMAKE_EXE_LINKER_FLAGS_DEBUG=",
            "-DCMAKE_CXX_COMPILER_LAUNCHER=", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DBUILD_TESTING=ON", "-DWLS_ENABLE_ASAN=ON", "-DWLS_ENABLE_UBSAN=OFF",
            f"-DPython3_EXECUTABLE={python}"]


def check_compile_commands(path: Path, compiler: str) -> None:
    commands = json.loads(path.read_text(encoding="utf-8"))
    require(bool(commands), "empty compile command set")
    for entry in commands:
        args = entry.get("arguments") or shlex.split(entry["command"])
        require(Path(args[0]).resolve() == Path(compiler).resolve(), "unexpected compiler/launcher")
        require(all(x in args for x in ("-g", "-Werror", "-fsanitize=address", "-fno-omit-frame-pointer")),
                "missing Debug/strict/ASan flags")
        require(not any(x.startswith(("-O", "-DNDEBUG", "-fno-sanitize")) for x in args),
                "optimization, NDEBUG or sanitizer disabling flag in Debug gate")


def registered_tests(path: Path, expected: int, manifest: Path | None = None) -> list[str]:
    tests = json.loads(path.read_text(encoding="utf-8"))["tests"]
    names = [t["name"] for t in tests]
    require(expected > 0 and len(names) == expected and len(set(names)) == expected,
            "unexpected/empty/duplicate CTest registration set")
    for test in tests:
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        require(not props.get("DISABLED", False), "disabled test in full gate")
    if manifest is not None:
        approved = json.loads(manifest.read_text(encoding="utf-8"))
        require(approved.get("schema_version") == 1 and isinstance(approved.get("tests"), list),
                "invalid reviewed test manifest")
        entries = approved["tests"]
        require(len(entries) == expected and all(
            isinstance(t.get("name"), str) and t["name"] and
            type(t.get("timeout")) in (int, float) and t["timeout"] > 0 for t in entries),
            "reviewed test manifest has an invalid count, name or timeout")
        approved_names = [t["name"] for t in entries]
        require(len(set(approved_names)) == expected and sorted(names) == sorted(approved_names),
                "CTest names differ from the reviewed test manifest")
        deadlines = {t["name"]: t["timeout"] for t in entries}
        for test in tests:
            props = {p["name"]: p["value"] for p in test.get("properties", [])}
            require(props.get("TIMEOUT") == deadlines[test["name"]],
                    "CTest deadline differs from the reviewed test manifest")
    return sorted(names)


def check_junit(path: Path, expected: list[str]) -> None:
    cases = ET.parse(path).getroot().findall(".//testcase")
    require(sorted(c.get("name", "") for c in cases) == expected, "JUnit test set does not match registration")
    for case in cases:
        require(case.get("status", "run") == "run" and
                all(case.find(tag) is None for tag in ("failure", "error", "skipped")),
                "failed, skipped or unrun test in JUnit")


def tracked_snapshot(source: Path, env: dict[str, str]) -> dict[str, dict[str, object]]:
    paths = subprocess.check_output(["git", "ls-files", "-z"], cwd=source, env=env).split(b"\0")
    result = {}
    for raw in filter(None, paths):
        name = os.fsdecode(raw)
        path = source / name
        require(not path.is_symlink() and path.is_file(), f"non-regular tracked input: {name}")
        result[name] = {"sha256": digest(path), "mode": path.stat().st_mode & 0o777}
    require(bool(result), "empty source snapshot")
    return result


def build_snapshot(build: Path) -> dict[str, str]:
    paths = [p for p in build.iterdir() if p.is_file() and
             (os.access(p, os.X_OK) or p.suffix == ".a" or
              p.name in ("CMakeCache.txt", "compile_commands.json", "CTestTestfile.cmake"))]
    require((build / "wls").is_file() and (build / "wls861").is_file(), "runtime binaries are missing")
    return {p.name: digest(p) for p in sorted(paths)}


def preserve_ctest_evidence(build: Path, evidence: Path,
                            steps: list[dict[str, object]]) -> dict[str, object]:
    """Copy original CTest bytes after its process has exited, including failures.

    JUnit may truncate successful output. LastTest.log is a separate original,
    not reconstructed from the console or XML. Interrupted .tmp files are kept
    under their own names and never passed off as complete LastTest.log files.
    """
    records = [step for step in steps if step["name"] == "ctest"]
    step = records[-1] if records else None
    finished = bool(step and step.get("ended_at") and step.get("exit_code") is not None
                    and not step.get("interrupted_or_launch_error"))
    index: dict[str, object] = {
        "schema_version": 1, "captured_at": utc(), "ctest_attempted": step is not None,
        "ctest_step": step, "execution_completed": finished, "complete": False,
        "artifacts": {}, "warnings": [],
        "publication_review": "private originals; never upload this directory",
    }
    artifacts, warnings = index["artifacts"], index["warnings"]
    native = evidence / "ctest-logs"
    native.mkdir(mode=0o700)
    for name in ("LastTest.log", "LastTestsFailed.log", "LastTest.log.tmp", "LastTestsFailed.log.tmp"):
        original = build / "Testing" / "Temporary" / name
        if not original.exists():
            continue
        try:
            require(original.is_file() and not original.is_symlink(), "non-regular CTest log")
            target = native / name
            with original.open("rb") as source, target.open("xb") as destination:
                shutil.copyfileobj(source, destination)
            checksum = digest(target)
            require(checksum == digest(original), "CTest log changed during preservation")
            artifacts["ctest-logs/" + name] = {
                "source": str(original), "bytes": target.stat().st_size, "sha256": checksum,
                "partial": name.endswith(".tmp"),
            }
        except Exception as error:
            warnings.append(f"could not preserve {name}: {type(error).__name__}: {error}")
    # These originals already live in the unique evidence directory. Index
    # their bytes without rewriting JUnit, command receipts or identity files.
    for name in ("ctest.xml", "ctest.log", "ctest.stderr.log", "ctest.step.json",
                 "source-before.json", "source-after.json", "binaries-before.json",
                 "binaries-after.json", "expected-tests.json"):
        path = evidence / name
        if path.is_file() and not path.is_symlink():
            artifacts[name] = {"source": str(path), "bytes": path.stat().st_size, "sha256": digest(path)}
    if step is not None:
        for name in ("ctest-logs/LastTest.log", "ctest.xml", "ctest.log", "ctest.stderr.log", "ctest.step.json"):
            if name not in artifacts:
                warnings.append(f"missing original {name}")
        if not finished:
            warnings.append("CTest has no complete, uninterrupted exit record")
        index["complete"] = finished and not warnings
    write_json(evidence / "ctest-evidence.json", index)
    for warning in warnings:
        print(f"CTest evidence incomplete: {warning}", file=sys.stderr, flush=True)
    return index


def public_method_names(source: Path) -> tuple[set[str], set[str], set[str]]:
    native, methods, classes = set(), set(), set()
    for path in (source / "tests").rglob("*"):
        if path.suffix not in (".cpp", ".hpp", ".py") or not path.is_file():
            continue
        content = path.read_text(encoding="utf-8")
        if path.suffix == ".py":
            methods.update(re.findall(r"^\s*def (test_[A-Za-z0-9_]+)\(", content, re.M))
            classes.update(re.findall(r"^\s*class ([A-Za-z0-9_]+)\b", content, re.M))
        elif "test::run({" in content:
            for match in re.finditer(r'\{\s*("(?:[^"\\\n]|\\.)*")\s*,', content.split("test::run({", 1)[1]):
                try:
                    name = json.loads(match[1])
                except ValueError:
                    continue
                if "\n" not in name and "\r" not in name:
                    native.add(name)
    return native, methods, classes


def public_log_line(line: str, native: set[str], methods: set[str], classes: set[str],
                    tests: set[str]) -> bool:
    """Allow only static method names and numeric test observations, not data."""
    if line in ("", "OK", "Test Passed.", "Test Failed.", "<end of output>", "Output:",
                "The following tests FAILED:", "Errors while running CTest"):
        return True
    if re.fullmatch(r"[-=]{3,}", line):
        return True
    if any(line == prefix + name for prefix in ("START ", "PASS ") for name in native):
        return True
    if re.fullmatch(r"RESULT \d+ passed, \d+ failed|Ran \d+ tests? in [0-9.]+s|"
                    r"FAILED \((?:failures|errors|skipped)=\d+(?:, (?:failures|errors|skipped)=\d+)*\)|"
                    r"Test time =\s*[0-9.]+ sec|Test timeout computed to be: [0-9.]+|"
                    r"\d+% tests passed, \d+ tests failed out of \d+|"
                    r"Total Test time \(real\) =\s*[0-9.]+ sec", line):
        return True
    match = re.fullmatch(r"(test_[A-Za-z0-9_]+) \((?:__main__|[A-Za-z0-9_]+)\.([A-Za-z0-9_]+)\.\1\)(?: \.\.\. (?:ok|FAIL|ERROR))?", line)
    if match and match[1] in methods and match[2] in classes:
        return True
    match = re.fullmatch(r"\[immediate-unittest (?:FAIL|ERROR) (?:__main__|[A-Za-z0-9_]+)\.([A-Za-z0-9_]+)\.(test_[A-Za-z0-9_]+)\]", line)
    if match and match[1] in classes and match[2] in methods:
        return True
    for name in tests:
        escaped = re.escape(name)
        if re.fullmatch(r"\s*(?:\d+/\d+ Testing: |Start\s+\d+: )" + escaped, line):
            return True
        if re.fullmatch(r"\s*\d+/\d+ Test\s+#\d+: " + escaped +
                        r"\s*\.+\s+(?:Passed|\*\*\*Failed|\*\*\*Timeout|\*\*\*Exception: [A-Za-z ]+)\s+[0-9.]+ sec", line):
            return True
        if re.fullmatch(r"\s*\d+ - " + escaped + r" \((?:Failed|Timeout|Not Run)\)", line):
            return True
        if re.fullmatch('"' + escaped + r'" time elapsed: \d+:\d+:\d+', line):
            return True
    return False


def public_log_projection(raw: bytes, names: tuple[set[str], set[str], set[str]],
                          tests: set[str]) -> tuple[bytes, list[dict[str, object]]]:
    spans: list[tuple[bool, int, int]] = []
    offset = 0
    def span(keep: bool, begin: int, end: int) -> None:
        if begin == end:
            return
        if spans and spans[-1][0] == keep and spans[-1][2] == begin:
            spans[-1] = (keep, spans[-1][1], end)
        else:
            spans.append((keep, begin, end))
    for line in raw.splitlines(keepends=True):
        content = line.rstrip(b"\r\n")
        try:
            text = content.decode("utf-8")
        except UnicodeError:
            text = "\0"
        if public_log_line(text, *names, tests):
            span(True, offset, offset + len(line))
        else:
            # Preserve the known FAIL method, while its exception text stays
            # private. Exceptions/tracebacks may embed Doctor/config/headers.
            prefix = next((f"FAIL {name}: ".encode() for name in names[0]
                           if content.startswith(f"FAIL {name}: ".encode())), b"")
            span(True, offset, offset + len(prefix))
            span(False, offset + len(prefix), offset + len(line))
        offset += len(line)
    projected = bytearray()
    ranges = []
    for keep, begin, end in spans:
        start = len(projected)
        projected.extend(raw[begin:end] if keep else f"[private output omitted: {end - begin} bytes]\n".encode())
        ranges.append({"kind": "verbatim" if keep else "redacted", "source_bytes": [begin, end],
                       "public_bytes": [start, len(projected)]})
    return bytes(projected), ranges


def check_public_log(raw: bytes, names: tuple[set[str], set[str], set[str]], tests: set[str]) -> None:
    for line in raw.decode("utf-8").splitlines():
        if public_log_line(line, *names, tests) or re.fullmatch(r"\[private output omitted: \d+ bytes\]", line):
            continue
        if any(re.fullmatch(re.escape(f"FAIL {name}: ") + r"\[private output omitted: \d+ bytes\]", line)
               for name in names[0]):
            continue
        raise RuntimeError("unapproved dynamic line in public log projection")


def publish_public_evidence(source: Path, build: Path, private: Path,
                            summary: dict[str, object]) -> Path:
    """Publish only a checked allowlist projection; raw evidence stays private."""
    # This immutable input is a proposed publication status, not the final
    # process receipt. summary.json stays NOT_COMPLETE until this returns.
    summary_input = private / "summary-publication-input.json"
    with summary_input.open("x", encoding="utf-8") as destination:
        destination.write(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    staging = build / "public-evidence-pending"
    staging.mkdir(mode=0o700)
    names = public_method_names(source)
    catalog = json.loads((source / "tests/gcc_asan_expected_tests.json").read_text())
    tests = {entry["name"] for entry in catalog["tests"]}
    manifest: dict[str, object] = {"schema_version": 1, "policy": "static-test-observations-v1",
                                 "private_directory": "private-evidence", "artifacts": {}}
    def emit(name: str, data: bytes, original: Path, kind: str, ranges=None) -> None:
        (staging / name).write_bytes(data)
        manifest["artifacts"][name] = {
            "source": str(original.relative_to(private)), "source_bytes": original.stat().st_size,
            "source_sha256": digest(original), "public_bytes": len(data),
            "public_sha256": hashlib.sha256(data).hexdigest(), "kind": kind,
            "ranges": ranges if ranges is not None else "structured projection; no byte-copy claim",
        }
    for original in sorted(private.rglob("*")):
        if not original.is_file() or original.is_symlink():
            continue
        relative = original.relative_to(private).as_posix()
        if original.suffix == ".log" or original.name.endswith(".log.tmp"):
            projected, ranges = public_log_projection(original.read_bytes(), names, tests)
            emit(relative.replace("/", "__") + ".projection.txt", projected, original, "log-projection", ranges)
        elif relative in ("source-before.json", "source-after.json", "binaries-before.json", "binaries-after.json"):
            values = json.loads(original.read_text())
            for path, value in values.items():
                require(not Path(path).is_absolute() and ".." not in Path(path).parts,
                        "invalid public identity path")
                checksum = value["sha256"] if isinstance(value, dict) else value
                require(re.fullmatch(r"[a-f0-9]{64}", checksum) is not None, "invalid public identity digest")
                require(not isinstance(value, dict) or set(value) == {"sha256", "mode"}, "unexpected identity fields")
            data = original.read_bytes()
            emit(relative, data, original, "verbatim-identity", [{"kind": "verbatim", "source_bytes": [0, len(data)], "public_bytes": [0, len(data)]}])
    safe_summary = {key: summary[key] for key in ("head", "status", "started_at", "ended_at", "uid", "gid",
                    "build_umask", "test_umask", "sanitizer_environment", "historical_clang_asan_failures_closed")}
    safe_summary["identities_unchanged"] = summary.get("identities_unchanged", False)
    safe_summary["ctest_evidence_complete"] = summary.get("ctest_evidence", {}).get("complete", False)
    safe_summary["registered_tests"] = summary.get("registered_tests")
    safe_summary["steps"] = [{key: record[key] for key in ("name", "started_at", "ended_at", "seconds", "exit_code") if key in record}
                             for record in summary["steps"]]
    for public, original, value in (("summary.projection.json", summary_input, safe_summary),
                                    ("expected-tests.json", source / "tests/gcc_asan_expected_tests.json", catalog)):
        if public == "expected-tests.json":
            # Source catalog may be needed even when setup failed before CTest.
            private_catalog = private / "public-catalog-source.json"
            with private_catalog.open("xb") as destination:
                destination.write(original.read_bytes())
            original = private_catalog
        emit(public, (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode(), original, "structured-projection")
    junit = private / "ctest.xml"
    if junit.is_file():
        try:
            root = ET.Element("testsuite")
            for case in ET.parse(junit).getroot().findall(".//testcase"):
                require(case.get("name") in tests, "unknown public JUnit test name")
                status, elapsed = case.get("status", "run"), case.get("time", "0")
                require(status in ("run", "notrun") and re.fullmatch(r"[0-9.eE+-]+", elapsed) is not None,
                        "unknown public JUnit status/time")
                projected = ET.SubElement(root, "testcase", name=case.get("name"), status=status, time=elapsed)
                for tag in ("failure", "error", "skipped"):
                    if case.find(tag) is not None:
                        ET.SubElement(projected, tag).text = "Original diagnostic is retained privately."
            emit("ctest.projection.xml", ET.tostring(root, encoding="utf-8", xml_declaration=True), junit, "junit-status-projection")
        except ET.ParseError:
            # Keep malformed/interrupted XML privately; never invent test rows.
            emit("ctest-incomplete.json", b'{"status":"MALFORMED_OR_INCOMPLETE_JUNIT"}\n', junit, "structured-projection")
    write_json(staging / "public-index.json", manifest)
    check_public_package(staging, names, tests)
    destination = build / "public-evidence"
    require(not destination.exists(), "public evidence already exists")
    staging.rename(destination)
    return destination


def read_public_json(path: Path) -> object:
    """Reject ignored duplicate values and non-finite numbers at every depth."""
    def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate public JSON key")
            result[key] = value
        return result

    def finite_float(raw: str) -> float:
        value = float(raw)
        require(math.isfinite(value), "non-finite public JSON number")
        return value

    def reject_constant(_raw: str) -> None:
        raise RuntimeError("non-finite public JSON number")

    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object,
                      parse_float=finite_float, parse_constant=reject_constant)


def check_public_package(directory: Path, names: tuple[set[str], set[str], set[str]], tests: set[str]) -> None:
    manifest = read_public_json(directory / "public-index.json")
    require(set(manifest) == {"schema_version", "policy", "private_directory", "artifacts"}
            and manifest["schema_version"] == 1 and manifest["policy"] == "static-test-observations-v1"
            and manifest["private_directory"] == "private-evidence", "invalid public index")
    expected = set(manifest["artifacts"]) | {"public-index.json"}
    require({path.name for path in directory.iterdir()} == expected, "unexpected public artifact")
    for name, entry in manifest["artifacts"].items():
        require(set(entry) == {"source", "source_bytes", "source_sha256", "public_bytes", "public_sha256", "kind", "ranges"},
                "unexpected public provenance fields")
        source_name = entry["source"]
        originals = {stem + suffix for stem in STEP_NAMES for suffix in (".log", ".stderr.log")}
        originals.update({"ctest-logs/" + n for n in ("LastTest.log", "LastTestsFailed.log", "LastTest.log.tmp", "LastTestsFailed.log.tmp")})
        originals.update({"source-before.json", "source-after.json", "binaries-before.json", "binaries-after.json",
                          "summary-publication-input.json", "public-catalog-source.json", "ctest.xml"})
        require(source_name in originals, "unapproved private source name")
        require(all(re.fullmatch(r"[a-f0-9]{64}", entry[key]) for key in ("source_sha256", "public_sha256")),
                "invalid public provenance hash")
        require(all(type(entry[key]) is int and entry[key] >= 0 for key in ("source_bytes", "public_bytes")),
                "invalid public provenance size")
        if isinstance(entry["ranges"], list):
            source_offset = public_offset = 0
            for span in entry["ranges"]:
                require(set(span) == {"kind", "source_bytes", "public_bytes"}
                        and span["kind"] in ("verbatim", "redacted"), "invalid public byte range")
                for key, offset in (("source_bytes", source_offset), ("public_bytes", public_offset)):
                    bounds = span[key]
                    require(isinstance(bounds, list) and len(bounds) == 2 and
                            all(type(value) is int for value in bounds) and bounds[0] == offset and bounds[1] >= offset,
                            "noncontiguous public byte ranges")
                source_offset, public_offset = span["source_bytes"][1], span["public_bytes"][1]
            require(source_offset == entry["source_bytes"] and public_offset == entry["public_bytes"],
                    "incomplete public byte ranges")
        else:
            require(entry["ranges"] == "structured projection; no byte-copy claim", "invalid projection scope")
        path = directory / name
        require(Path(name).name == name and path.is_file() and not path.is_symlink(), "unsafe public artifact")
        require(path.stat().st_size == entry["public_bytes"] and digest(path) == entry["public_sha256"],
                "public artifact identity mismatch")
        if entry["kind"] == "log-projection":
            require(name == source_name.replace("/", "__") + ".projection.txt", "unapproved public log name")
            check_public_log(path.read_bytes(), names, tests)
        elif entry["kind"] == "verbatim-identity":
            require(name == source_name and name in {"source-before.json", "source-after.json", "binaries-before.json", "binaries-after.json"},
                    "unapproved identity artifact")
            for relative, value in read_public_json(path).items():
                require(not Path(relative).is_absolute() and ".." not in Path(relative).parts and
                        not any(character in relative for character in ('\n', '\r', ':', '"', '{', '}')),
                        "invalid identity path")
                if isinstance(value, dict):
                    require(set(value) == {"sha256", "mode"} and type(value["mode"]) is int and 0 <= value["mode"] <= 0o777,
                            "invalid source identity fields")
                checksum = value["sha256"] if isinstance(value, dict) else value
                require(isinstance(checksum, str) and re.fullmatch(r"[a-f0-9]{64}", checksum), "invalid identity hash")
        elif entry["kind"] == "junit-status-projection":
            require(name == "ctest.projection.xml" and source_name == "ctest.xml", "unapproved JUnit projection")
            raw = path.read_bytes()
            root = ET.fromstring(raw)
            # We publish only our generator's exact XML serialization. DOM
            # checks alone miss comments, PIs and DTDs discarded by ET.parse.
            require(ET.tostring(root, encoding="utf-8", xml_declaration=True) == raw,
                    "noncanonical public JUnit XML")
            require(root.tag == "testsuite" and not root.attrib, "invalid public JUnit root")
            require(not (root.text or "").strip() and not (root.tail or "").strip(), "unexpected public JUnit root text")
            for case in root:
                require(case.tag == "testcase" and set(case.attrib) == {"name", "status", "time"} and
                        case.get("name") in tests and case.get("status") in ("run", "notrun") and
                        re.fullmatch(r"[0-9.eE+-]+", case.get("time", "")), "invalid public JUnit case")
                require(not (case.text or "").strip() and not (case.tail or "").strip(), "unexpected public JUnit output")
                for child in case:
                    require(child.tag in ("failure", "error", "skipped") and not child.attrib and not list(child)
                            and child.text == "Original diagnostic is retained privately."
                            and not (child.tail or "").strip(), "unexpected public JUnit diagnostic")
        elif entry["kind"] == "structured-projection":
            value = read_public_json(path)
            if name == "summary.projection.json" and source_name == "summary-publication-input.json":
                require(set(value) == {"head", "status", "started_at", "ended_at", "uid", "gid", "build_umask", "test_umask",
                    "sanitizer_environment", "historical_clang_asan_failures_closed", "identities_unchanged",
                    "ctest_evidence_complete", "registered_tests", "steps"}, "unexpected public summary fields")
                require(re.fullmatch(r"[a-f0-9]{40}", value["head"]) and value["status"] in ("PASSED", "NOT_COMPLETE"),
                        "invalid public summary identity/status")
                require(value["sanitizer_environment"] == {"ASAN_OPTIONS": ASAN, "UBSAN_OPTIONS": UBSAN, "LSAN_OPTIONS": None}
                        and value["build_umask"] == "0022" and value["test_umask"] == "0002", "invalid public gate options")
                require(value["historical_clang_asan_failures_closed"] is False and
                        all(type(value[key]) is bool for key in ("identities_unchanged", "ctest_evidence_complete")) and
                        all(type(value[key]) is int for key in ("uid", "gid")) and
                        (value["registered_tests"] is None or type(value["registered_tests"]) is int), "invalid public summary scalars")
                for key in ("started_at", "ended_at"):
                    dt.datetime.fromisoformat(value[key])
                for record in value["steps"]:
                    require(set(record) <= {"name", "started_at", "ended_at", "seconds", "exit_code"}
                            and record["name"] in STEP_NAMES, "invalid public command metadata")
                    require(record.get("exit_code") is None or type(record["exit_code"]) is int, "invalid public command exit")
                    require("seconds" not in record or type(record["seconds"]) in (int, float), "invalid public command duration")
                    for key in ("started_at", "ended_at"):
                        if key in record:
                            dt.datetime.fromisoformat(record[key])
            elif name == "expected-tests.json" and source_name == "public-catalog-source.json":
                require(set(value) == {"schema_version", "tests"} and value["schema_version"] == 1 and
                        {case["name"] for case in value["tests"]} == tests, "invalid public catalog")
                for case in value["tests"]:
                    require(set(case) == {"name", "timeout"} and type(case["timeout"]) in (int, float), "invalid public deadline")
            else:
                require(name == "ctest-incomplete.json" and source_name == "ctest.xml" and
                        value == {"status": "MALFORMED_OR_INCOMPLETE_JUNIT"}, "unapproved public JSON artifact")
        else:
            raise RuntimeError("unapproved public artifact kind")


class Recorder:
    def __init__(self, directory: Path, source: Path, env: dict[str, str]):
        self.directory, self.source, self.env = directory, source, env
        self.steps: list[dict[str, object]] = []

    def run(self, name: str, argv: list[str], *, json_output: bool = False) -> Path:
        output = self.directory / (name + (".json" if json_output else ".log"))
        error = self.directory / (name + ".stderr.log")
        record: dict[str, object] = {"name": name, "argv": argv, "cwd": str(self.source),
                                     "started_at": utc(), "exit_code": None}
        self.steps.append(record)
        start = time.monotonic()
        process = None
        print(f"[{name}] started", flush=True)
        try:
            with output.open("xb") as out, error.open("xb") as err:
                process = subprocess.Popen(argv, cwd=self.source, env=self.env,
                                           stdout=out, stderr=err, start_new_session=True)
                record["exit_code"] = process.wait()
        except BaseException:
            if process is not None and process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                record["exit_code"] = process.returncode
            record["interrupted_or_launch_error"] = True
            raise
        finally:
            record.update(ended_at=utc(), seconds=time.monotonic() - start)
            record["logs"] = {p.name: digest(p) for p in (output, error) if p.is_file()}
            write_json(self.directory / (name + ".step.json"), record)
        require(record["exit_code"] == 0, f"{name} failed with exit {record['exit_code']}; see {output}")
        print(f"[{name}] exit 0", flush=True)
        return output


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--python", required=True, help="existing venv Python with requirements-test.txt installed")
    parser.add_argument("--compiler", default="g++-13")
    parser.add_argument("--expected-tests", type=int, default=DEFAULT_EXPECTED_TESTS,
                        help="reviewed count, also checked against tests/gcc_asan_expected_tests.json")
    parser.add_argument("--data-root", type=Path, help="on the user's server, require checkout under this mounted data disk")
    args = parser.parse_args(argv)
    require(platform.system() == "Linux" and os.geteuid() != 0, "run on Linux as an ordinary user, not root")
    source = args.source.resolve(strict=True)
    if args.data_root is not None:
        data = args.data_root.resolve(strict=True)
        require(data != Path("/") and os.path.ismount(data) and source.is_relative_to(data),
                "checkout must be below the explicit mounted data disk")
    compiler = shutil.which(args.compiler)
    require(compiler is not None, "GCC 13.3 is not installed; no automatic toolchain substitution")
    # Do not resolve the venv symlink: that would silently select system Python.
    python = os.path.abspath(args.python)
    require(Path(python).is_file() and os.access(python, os.X_OK), "test Python is not executable")
    env = clean_environment(dict(os.environ))
    require(not subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal"],
                                       cwd=source, env=env), "use a clean checkout; existing work is never reset")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, env=env, text=True).strip()
    old_mask = os.umask(0o022)
    build = source / ("build-gcc-asan-" + dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8])
    build.mkdir(mode=0o700)
    evidence = build / "private-evidence"
    evidence.mkdir(mode=0o700)
    temporary = build / "tmp"
    temporary.mkdir(mode=0o700)
    env["TMPDIR"] = str(temporary)
    recorder = Recorder(evidence, source, env)
    summary: dict[str, object] = {"head": head, "status": "NOT_COMPLETE", "started_at": utc(),
        "source": str(source), "build": str(build), "uid": os.geteuid(), "gid": os.getegid(),
        "groups": os.getgroups(), "platform": platform.platform(), "build_umask": "0022", "test_umask": "0002",
        "sanitizer_environment": {"ASAN_OPTIONS": ASAN, "UBSAN_OPTIONS": UBSAN, "LSAN_OPTIONS": None},
        "historical_clang_asan_failures_closed": False, "steps": recorder.steps}
    write_json(evidence / "summary.json", summary)
    before = binaries = None
    try:
        before = tracked_snapshot(source, env)
        write_json(evidence / "source-before.json", before)
        version = recorder.run("compiler-version", [compiler, "-dumpfullversion", "-dumpversion"]).read_text().strip()
        require(version == "13.3.0", f"expected GCC 13.3.0, got {version}")
        recorder.run("compiler-banner", [compiler, "--version"])
        recorder.run("cmake-version", ["cmake", "--version"])
        recorder.run("python-dependencies", [python, "-I", "-c", "import sys,jsonschema; print(sys.version); print(jsonschema.__file__)"])
        recorder.run("mount", ["findmnt", "--json", "--target", str(build)])
        recorder.run("configure", configure_command(source, build, compiler, python))
        recorder.run("build", ["cmake", "--build", str(build), "--parallel", "2"])
        check_compile_commands(build / "compile_commands.json", compiler)
        listing = recorder.run("test-plan", ["ctest", "--test-dir", str(build), "--show-only=json-v1"], json_output=True)
        manifest = source / "tests" / "gcc_asan_expected_tests.json"
        names = registered_tests(listing, args.expected_tests, manifest)
        shutil.copy2(manifest, evidence / "expected-tests.json")
        summary["expected_tests_sha256"] = digest(manifest)
        summary["registered_tests"] = len(names)
        for name in ("CMakeCache.txt", "compile_commands.json", "CTestTestfile.cmake"):
            shutil.copy2(build / name, evidence / name)
        binaries = build_snapshot(build)
        write_json(evidence / "binaries-before.json", binaries)
        os.umask(0o002)
        recorder.run("ctest", ["ctest", "--test-dir", str(build), "--output-on-failure", "--parallel", "1",
                               "--no-tests=error", "--output-junit", str(evidence / "ctest.xml")])
        check_junit(evidence / "ctest.xml", names)
        summary["status"] = "PASSED"
    except (Exception, KeyboardInterrupt) as error:
        summary["error"] = str(error) or type(error).__name__
    finally:
        # Collect each identity independently: source/HEAD/checkout errors must
        # not suppress the final build snapshot or erase the original failure.
        identity_errors = []
        try:
            after = tracked_snapshot(source, env)
            write_json(evidence / "source-after.json", after)
            require(before is not None and after == before, "source changed during validation")
        except (Exception, KeyboardInterrupt) as error:
            identity_errors.append("source: " + (str(error) or type(error).__name__))
        try:
            current_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, env=env, text=True).strip()
            require(current_head == head, "HEAD changed during validation")
        except (Exception, KeyboardInterrupt) as error:
            identity_errors.append("HEAD: " + (str(error) or type(error).__name__))
        try:
            require(not subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal"],
                                               cwd=source, env=env), "checkout changed during validation")
        except (Exception, KeyboardInterrupt) as error:
            identity_errors.append("checkout: " + (str(error) or type(error).__name__))
        try:
            after_binaries = build_snapshot(build)
            write_json(evidence / "binaries-after.json", after_binaries)
            require(binaries is not None and after_binaries == binaries, "build identity changed or unavailable during testing")
        except (Exception, KeyboardInterrupt) as error:
            identity_errors.append("build: " + (str(error) or type(error).__name__))
        summary["identities_unchanged"] = not identity_errors
        if identity_errors:
            summary["status"] = "NOT_COMPLETE"
            summary["identity_error"] = "; ".join(identity_errors)
        if "error" in summary:
            summary["status"] = "NOT_COMPLETE"
        try:
            retained = preserve_ctest_evidence(build, evidence, recorder.steps)
            summary["ctest_evidence"] = {
                "index": "ctest-evidence.json", "sha256": digest(evidence / "ctest-evidence.json"),
                "complete": retained["complete"], "warnings": retained["warnings"],
            }
            if not retained["complete"]:
                summary["status"] = "NOT_COMPLETE"
        except (Exception, KeyboardInterrupt) as error:
            summary["status"] = "NOT_COMPLETE"
            summary["ctest_evidence_error"] = str(error) or type(error).__name__
        summary["ended_at"] = utc()
        checkpoint = dict(summary, status="NOT_COMPLETE")
        write_json(evidence / "summary.json", checkpoint)
        try:
            publish_public_evidence(source, build, evidence, summary)
            write_json(evidence / "summary.json", summary)
        except (Exception, KeyboardInterrupt) as error:
            summary["status"] = "NOT_COMPLETE"
            summary["public_evidence_error"] = str(error) or type(error).__name__
            # A catchable interruption can arrive after the publisher's rename.
            # Preserve those bytes outside the exact CI upload directory.
            public = build / "public-evidence"
            try:
                if public.exists():
                    public.rename(build / ("public-evidence-incomplete-" + uuid.uuid4().hex))
            except (Exception, KeyboardInterrupt) as quarantine_error:
                # Failure to quarantine must not suppress the final failure
                # receipt. A surviving candidate cannot establish CI success.
                summary["public_quarantine_error"] = str(quarantine_error) or type(quarantine_error).__name__
                summary["failed_ci_required"] = True
            write_json(evidence / "summary.json", summary)
        os.umask(old_mask)
        print(f"{summary['status']}: {evidence / 'summary.json'}", flush=True)
    return 0 if summary["status"] == "PASSED" else 1


if __name__ == "__main__":
    # SIGKILL cannot be finalized. External exit/CI records remain necessary;
    # a publication input or an artifact alone never proves process exit 0.
    def stop(_signal: int, _frame: object) -> None:
        raise KeyboardInterrupt("validation interrupted")
    signal.signal(signal.SIGTERM, stop)
    try:
        sys.exit(main())
    except (Exception, KeyboardInterrupt) as error:
        print(f"preflight refused: {error}", file=sys.stderr)
        sys.exit(1)
