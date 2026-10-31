#!/usr/bin/env python3
"""Build one frozen offline fairness probe with separate A3/A4 sources and ABIs."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
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

BASELINE = "a2dfb46322d217f32eb5cc2516de6107c10db4b8"
PROBE_SHA256 = "48ae8f4db3d29835403bc41daba2554d51a2c05d5a7c3901ee901193fd25aad6"
CASE = "S1 request then team fairness under one-slot release"
ORDER_EXPRESSIONS = (
    'sequence->read() == (std::vector<std::string>{"a1-first", "b1"})',
    'sequence->read() == (std::vector<std::string>{"a1-first", "b1", "a2"})',
)
WATCHDOG_SECONDS = 180


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def utc() -> str:
    return datetime.now(timezone.utc).isoformat()


def identity(path: Path) -> dict[str, object]:
    require(path.is_file() and not path.is_symlink(), f"non-regular input: {path}")
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"sha256": digest, "bytes": path.stat().st_size, "mode": path.stat().st_mode & 0o777}


def write_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def classify(role: str, code: int, stdout: str, stderr: str, probe: Path) -> str:
    """A red result requires the exact original order assertion, never any exit 1."""
    if code in (124, 137):
        return "WATCHDOG_OR_SIGNAL_FAILURE"
    if code < 0 or code >= 128:
        return "SIGNAL_FAILURE"
    if role == "candidate":
        expected = f"PASS {CASE}\nRESULT 1 passed, 0 failed\n"
        return "EXPECTED_GREEN" if code == 0 and stdout == expected and not stderr else "NOT_GREEN"
    require(role == "baseline", "unknown comparison role")
    errors = []
    for number, line in enumerate(probe.read_text(encoding="utf-8").splitlines(), 1):
        for expression in ORDER_EXPRESSIONS:
            if line.strip() == f"CHECK({expression});":
                errors.append(f"FAIL {CASE}: {probe}:{number}: {expression}\n")
    require(len(errors) == 2, "original business order assertions are missing")
    if code == 1 and stdout == "RESULT 0 passed, 1 failed\n" and stderr in errors:
        return "EXPECTED_BUSINESS_ORDER_RED"
    return "NOT_TARGET_RED"


def group_members(group: int) -> list[dict[str, object]]:
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
            if int(fields[2]) == group:
                found.append({"pid": int(entry.name), "ppid": int(fields[1]),
                              "pgid": int(fields[2]), "state": fields[0], "start_ticks": int(fields[19])})
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return found


class Recorder:
    def __init__(self, evidence: Path, env: dict[str, str]):
        self.evidence, self.env = evidence, env
        self.steps: list[dict[str, object]] = []

    def run(self, name: str, argv: list[str], cwd: Path, *, umask: int = 0o022) -> dict[str, object]:
        output, error = self.evidence / f"{name}.stdout.log", self.evidence / f"{name}.stderr.log"
        record: dict[str, object] = {"name": name, "argv": argv, "cwd": str(cwd),
                                     "environment": self.env.copy(), "umask": f"{umask:04o}",
                                     "started_at": utc(), "exit_code": None}
        self.steps.append(record)
        record_path = self.evidence / f"{name}.step.json"
        write_json(record_path, record)
        print(f"[{name}] started", flush=True)
        start, process = time.monotonic(), None
        try:
            with output.open("xb") as stdout, error.open("xb") as stderr:
                process = subprocess.Popen(argv, cwd=cwd, env=self.env, stdout=stdout, stderr=stderr,
                                           start_new_session=True, umask=umask)
                record["pid"] = process.pid
                write_json(record_path, record)
                record["exit_code"] = process.wait()
        finally:
            if process is not None:
                members = group_members(process.pid)
                record["process_group_after_wait"] = members
                record["cleanup_signals"] = []
                # Each step starts a new session. Only its still-owned group is touched.
                for signum in (signal.SIGTERM, signal.SIGKILL):
                    if not group_members(process.pid):
                        break
                    try:
                        os.killpg(process.pid, signum)
                        record["cleanup_signals"].append(int(signum))
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
                if process.poll() is not None:
                    record["exit_code"] = process.returncode
                record["remaining_processes"] = group_members(process.pid)
            record.update(ended_at=utc(), seconds=time.monotonic() - start)
            record["logs"] = {path.name: identity(path) for path in (output, error) if path.is_file()}
            write_json(record_path, record)
        require(not record.get("remaining_processes"), f"{name}: step children remain")
        return record

    def checked(self, name: str, argv: list[str], cwd: Path) -> str:
        record = self.run(name, argv, cwd)
        require(record["exit_code"] == 0, f"{name}: exit {record['exit_code']}; see original logs")
        return (self.evidence / f"{name}.stdout.log").read_text(encoding="utf-8")


def source_snapshot(source: Path, role: str, recorder: Recorder, phase: str, expected: str) -> dict[str, object]:
    head = recorder.checked(f"{role}-{phase}-head", ["git", "rev-parse", "HEAD"], source).strip()
    require(head == expected, f"{role}: unexpected source commit")
    status = recorder.checked(f"{role}-{phase}-status", ["git", "status", "--porcelain=v1", "--untracked-files=normal"], source)
    require(not status, f"{role}: source must be clean")
    names = recorder.checked(f"{role}-{phase}-tracked", ["git", "ls-files", "-z"], source).split("\0")
    snapshot = {name: identity(source / name) for name in names if name}
    require(bool(snapshot), f"{role}: empty source snapshot")
    write_json(recorder.evidence / f"{role}-source-{phase}.json", snapshot)
    return snapshot


def literal(path: Path) -> str:
    require("\n" not in str(path) and "\r" not in str(path), "newline in CMake path")
    equal = "="
    while "]" + equal + "]" in str(path):
        equal += "="
    return "[" + equal + "[" + str(path) + "]" + equal + "]"


def wrapper(source: Path, probe: Path) -> str:
    return f"""cmake_minimum_required(VERSION 3.20)
project(a4_fairness_same_source LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_TESTING OFF CACHE BOOL "Standalone fairness probe" FORCE)
set(PROBE_VERSION_SOURCE {literal(source)})
add_subdirectory("${{PROBE_VERSION_SOURCE}}" "${{CMAKE_CURRENT_BINARY_DIR}}/version" EXCLUDE_FROM_ALL)
add_executable(a4_fairness_probe {literal(probe)})
target_link_libraries(a4_fairness_probe PRIVATE wls_s1)
target_include_directories(a4_fairness_probe PRIVATE "${{PROBE_VERSION_SOURCE}}/tests")
target_compile_definitions(a4_fairness_probe PRIVATE WLS_SOURCE_DIR="${{PROBE_VERSION_SOURCE}}")
"""


def build_identity(build: Path) -> dict[str, object]:
    names = ("a4_fairness_probe", "version/libwls_s1.a", "version/libwls_kernel.a",
             "CMakeCache.txt", "compile_commands.json", "CMakeFiles/a4_fairness_probe.dir/link.txt")
    return {name: identity(build / name) for name in names}


def check_build(build: Path, source: Path, other: Path, probe: Path, compiler: str) -> None:
    entries = json.loads((build / "compile_commands.json").read_text())
    require(entries, "empty compile commands")
    for entry in entries:
        argv = entry.get("arguments") or shlex.split(entry["command"])
        require(Path(argv[0]).resolve() == Path(compiler).resolve(), "unexpected compiler or launcher")
        require(all(flag in argv for flag in ("-O3", "-DNDEBUG", "-Werror", "-std=c++20")),
                "missing original Release/strict flags")
        require(not any(str(other) in argument or "sanitize" in argument for argument in argv),
                "foreign source/ABI or unexpected sanitizer flags")
        compiled = Path(entry["file"]).resolve()
        require(compiled == probe or compiled.is_relative_to(source), "foreign translation unit")
    selected = [entry for entry in entries if Path(entry["file"]).resolve() == probe]
    require(len(selected) == 1 and str(source / "include") in selected[0]["command"],
            "probe must use this version's own headers")
    link = (build / "CMakeFiles/a4_fairness_probe.dir/link.txt").read_text()
    require("version/libwls_s1.a" in link and "version/libwls_kernel.a" in link and str(other) not in link,
            "probe must link this version's own libraries")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-source", required=True, type=Path)
    parser.add_argument("--candidate-source", required=True, type=Path)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--output", required=True, type=Path, help="new directory outside both checkouts")
    parser.add_argument("--compiler", default="g++-13")
    args = parser.parse_args(argv)
    require(platform.system() == "Linux" and os.getuid() != 0 and os.geteuid() != 0, "ordinary Linux user required")
    sources = {"baseline": args.baseline_source.resolve(strict=True), "candidate": args.candidate_source.resolve(strict=True)}
    require(sources["baseline"] != sources["candidate"], "use separate version checkouts")
    require(len(args.candidate_sha) == 40 and all(char in "0123456789abcdef" for char in args.candidate_sha),
            "candidate SHA must be the full lowercase commit ID")
    output = args.output.resolve()
    require(not any(output.is_relative_to(source) or source.is_relative_to(output) for source in sources.values()),
            "evidence/build directory must be separate from both source trees")
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    evidence = output / "evidence"
    evidence.mkdir(mode=0o700)
    compiler = shutil.which(args.compiler)
    require(compiler is not None, "GCC 13 must be provisioned; no automatic compiler substitution")
    env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8",
           "PYTHONDONTWRITEBYTECODE": "1", "PYTHONNOUSERSITE": "1"}
    recorder = Recorder(evidence, env)
    summary: dict[str, object] = {"classification": "A4_SAME_SOURCE_RED_GREEN", "started_at": utc(),
        "status": "NOT_COMPLETE", "baseline_sha": BASELINE, "candidate_sha": args.candidate_sha,
        "sources": {key: str(value) for key, value in sources.items()}, "uid": os.getuid(),
        "gid": os.getgid(), "groups": os.getgroups(), "platform": platform.platform(),
        "invocation": sys.argv if argv is None else argv, "runner": identity(Path(__file__).resolve()),
        "environment": env.copy(), "build_umask": "0022", "test_umask": "0002",
        "fixture_deadline_seconds": 5, "outer_watchdog_seconds": WATCHDOG_SECONDS,
        "existing_ctest_suites_changed": False, "product_ctest_gate_executed_by_this_runner": False,
        "counting": "One separate same-source diagnostic, with two version executions; not two extra CTest suites.",
        "steps": recorder.steps, "results": {}}
    write_json(evidence / "summary.json", summary)
    snapshots, binaries = {}, {}
    expected = {"baseline": BASELINE, "candidate": args.candidate_sha}
    probe = evidence / "same-source.cpp"
    try:
        original = sources["candidate"] / "tests/repros/a4_fairness_same_source.cpp"
        require(identity(original)["sha256"] == PROBE_SHA256, "frozen probe bytes changed")
        shutil.copyfile(original, probe)
        require(identity(probe)["sha256"] == PROBE_SHA256, "copied probe identity changed")
        summary["same_source"] = {"path": str(probe), **identity(probe)}
        for role, source in sources.items():
            snapshots[role] = source_snapshot(source, role, recorder, "before", expected[role])
        catalog = sources["candidate"] / "tests/gcc_asan_expected_tests.json"
        reviewed = json.loads(catalog.read_text(encoding="utf-8"))
        entries = reviewed.get("tests", [])
        require(reviewed.get("schema_version") == 1 and entries and
                all(isinstance(item.get("name"), str) and item["name"] and
                    type(item.get("timeout")) in (int, float) and item["timeout"] > 0 for item in entries),
                "invalid companion product test manifest")
        require(len({item["name"] for item in entries}) == len(entries), "duplicate product test names")
        summary["companion_product_manifest"] = {"path": str(catalog), **identity(catalog),
            "reviewed_tests": entries, "count": len(entries), "executed_by_this_diagnostic": False}
        shutil.copyfile(catalog, evidence / "companion-product-tests.json")
        require(recorder.checked("compiler-version", [compiler, "-dumpfullversion", "-dumpversion"], output).strip() == "13.3.0",
                "expected GCC 13.3.0")
        for name, command in (("compiler-banner", [compiler, "--version"]), ("cmake-version", ["cmake", "--version"]),
                              ("mount", ["findmnt", "--json", "--target", str(output)]),
                              ("disk", ["df", "-B1", str(output)])):
            recorder.checked(name, command, output)
        # Both versions finish compiling before either probe executes.
        for role, source in sources.items():
            version = output / role
            version.mkdir(mode=0o700)
            (version / "tmp").mkdir(mode=0o700)
            wrapper_dir = version / "wrapper"
            wrapper_dir.mkdir(mode=0o700)
            (wrapper_dir / "CMakeLists.txt").write_text(wrapper(source, probe))
            shutil.copyfile(wrapper_dir / "CMakeLists.txt", evidence / f"{role}-wrapper.CMakeLists.txt")
            build = version / "build"
            env["TMPDIR"] = str(version / "tmp")
            command = ["cmake", "-S", str(wrapper_dir), "-B", str(build), "-G", "Unix Makefiles",
                       "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_CXX_COMPILER={compiler}",
                       "-DCMAKE_CXX_FLAGS=", "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG",
                       "-DCMAKE_EXE_LINKER_FLAGS=", "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=",
                       "-DCMAKE_CXX_COMPILER_LAUNCHER=", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                       "-DBUILD_TESTING=OFF", "-DWLS_ENABLE_ASAN=OFF", "-DWLS_ENABLE_UBSAN=OFF"]
            recorder.checked(f"{role}-configure", command, output)
            recorder.checked(f"{role}-build", ["cmake", "--build", str(build), "--target", "a4_fairness_probe", "--parallel", "2"], output)
            other = sources["candidate" if role == "baseline" else "baseline"]
            check_build(build, source, other, probe, compiler)
            binaries[role] = build_identity(build)
            write_json(evidence / f"{role}-binary-before.json", binaries[role])
            for name in ("CMakeCache.txt", "compile_commands.json", "CMakeFiles/a4_fairness_probe.dir/link.txt"):
                shutil.copyfile(build / name, evidence / f"{role}-{Path(name).name}")
            for suffix, tool_args in (("file", ["file"]), ("notes", ["readelf", "-n"]), ("libraries", ["ldd"])):
                recorder.checked(f"{role}-binary-{suffix}", [*tool_args, str(build / "a4_fairness_probe")], output)
        for role in sources:
            build = output / role / "build"
            env["TMPDIR"] = str(output / role / "tmp")
            require(identity(probe)["sha256"] == PROBE_SHA256, "same-source input changed before execution")
            record = recorder.run(f"{role}-probe", ["/usr/bin/timeout", "--signal=TERM", "--kill-after=5",
                                  str(WATCHDOG_SECONDS), str(build / "a4_fairness_probe")], output, umask=0o002)
            stdout = (evidence / f"{role}-probe.stdout.log").read_text(encoding="utf-8")
            stderr = (evidence / f"{role}-probe.stderr.log").read_text(encoding="utf-8")
            summary["results"][role] = {"exit_code": record["exit_code"], "seconds": record["seconds"],
                                       "classification": classify(role, record["exit_code"], stdout, stderr, probe)}
        require(summary["results"]["baseline"]["classification"] == "EXPECTED_BUSINESS_ORDER_RED",
                "baseline did not demonstrate the original business order failure")
        require(summary["results"]["candidate"]["classification"] == "EXPECTED_GREEN", "candidate did not pass")
        summary["status"] = "RED_GREEN_VERIFIED"
    except Exception as error:
        summary["error"] = f"{type(error).__name__}: {error}"
    finally:
        try:
            for role, before in snapshots.items():
                after = source_snapshot(sources[role], role, recorder, "after", expected[role])
                require(before == after, f"{role}: source inputs changed")
            for role, before in binaries.items():
                after = build_identity(output / role / "build")
                write_json(evidence / f"{role}-binary-after.json", after)
                require(before == after, f"{role}: binary/build identity changed")
            require(identity(probe)["sha256"] == PROBE_SHA256, "probe input changed")
            require(identity(Path(__file__).resolve()) == summary["runner"], "runner input changed")
            summary["identities_unchanged"] = True
        except Exception as error:
            summary["identity_error"] = f"{type(error).__name__}: {error}"
            summary["status"] = "NOT_COMPLETE"
        summary["ended_at"] = utc()
        write_json(evidence / "summary.json", summary)
        write_json(evidence / "artifact-sha256.json", {path.name: identity(path) for path in sorted(evidence.iterdir())
                                                       if path.is_file() and path.name != "artifact-sha256.json"})
        print(f"{summary['status']}: {evidence / 'summary.json'}", flush=True)
    return 0 if summary["status"] == "RED_GREEN_VERIFIED" else 1


if __name__ == "__main__":
    def interrupted(signum: int, _frame: object) -> None:
        raise RuntimeError(f"runner interrupted by signal {signum}")
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    sys.exit(main())
