#!/usr/bin/env python3
"""Run a fresh, full GCC 13.3 Debug ASan/LSan gate; never reinterpret old runs."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET

ASAN = "halt_on_error=1:detect_leaks=1"
UBSAN = "halt_on_error=1:print_stacktrace=1"


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
    parser.add_argument("--expected-tests", type=int, default=89,
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
    evidence = build / "evidence"
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
        # Even a failed CTest must retain both identities. A verification error
        # cannot turn an earlier failure into success or discard its log.
        try:
            after = tracked_snapshot(source, env)
            write_json(evidence / "source-after.json", after)
            require(before is not None and after == before, "source changed during validation")
            current_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, env=env, text=True).strip()
            require(current_head == head, "HEAD changed during validation")
            require(not subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal"],
                                               cwd=source, env=env), "checkout changed during validation")
            if binaries is not None:
                after_binaries = build_snapshot(build)
                write_json(evidence / "binaries-after.json", after_binaries)
                require(after_binaries == binaries, "build identity changed during testing")
            summary["identities_unchanged"] = True
        except Exception as error:
            summary["status"] = "NOT_COMPLETE"
            summary["identity_error"] = str(error)
        if "error" in summary:
            summary["status"] = "NOT_COMPLETE"
        summary["ended_at"] = utc()
        write_json(evidence / "summary.json", summary)
        os.umask(old_mask)
        print(f"{summary['status']}: {evidence / 'summary.json'}", flush=True)
    return 0 if summary["status"] == "PASSED" else 1


if __name__ == "__main__":
    # SIGKILL cannot be caught; the prewritten NOT_COMPLETE receipt remains.
    def stop(_signal: int, _frame: object) -> None:
        raise KeyboardInterrupt("validation interrupted")
    signal.signal(signal.SIGTERM, stop)
    try:
        sys.exit(main())
    except (Exception, KeyboardInterrupt) as error:
        print(f"preflight refused: {error}", file=sys.stderr)
        sys.exit(1)
