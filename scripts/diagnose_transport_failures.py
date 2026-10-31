#!/usr/bin/env python3
"""Prepare, then run three one-shot historical TLS diagnostics, never a gate.

Preparation checks the original gate's source/binary manifests and registration,
copies those source bytes into a private overlay, and changes only observations
in two Python fixtures. It does not rebuild or execute a product. Run the prepared
plan only in an exclusive diagnostic window, without compilation or other gates.
Each selected original method runs once, with the original timeout/ASan options.
Passing individual diagnostics never resolves the original full-suite failures.
"""
from __future__ import annotations

import argparse
import ast
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import signal
import stat
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
OBSERVED_FILES = ("tests/test_https_transport.py", "tests/test_live_model_store.py")
HELPER = "tests/transport_test_observation.py"
ENVIRONMENT = "WLS_TEST_TRANSPORT_OBSERVATION"
CASES = {
    "partial-upload": ("https_transport", OBSERVED_FILES[0], "HttpsTransport",
        "test_partial_request_upload_deadline_is_not_free_retry_authority"),
    "effective-capacity": ("live_model_store_tls", OBSERVED_FILES[1], "LiveStore",
        "test_effective_live_capacity_can_persist_actual_limit_observation"),
    "large-escaped": ("live_model_store_tls", OBSERVED_FILES[1], "LiveStore",
        "test_large_escaped_valid_output_fits_original_settlement_event"),
}


def require(value, message):
    if not value:
        raise RuntimeError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def identity(path):
    require(path.is_file() and not path.is_symlink(), "input is not a regular file: " + str(path))
    return {"sha256": digest(path), "mode": stat.S_IMODE(path.stat().st_mode)}


def write_json(path, value):
    # No replacement of an earlier attempt or receipt.
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat()


class WithoutObservation(ast.NodeTransformer):
    """Remove only the explicit observation hooks before comparing whole ASTs."""
    @staticmethod
    def hook(node):
        return isinstance(node, ast.Call) and (
            isinstance(node.func, ast.Attribute) and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "OBSERVATION"
            or isinstance(node.func, ast.Name) and node.func.id == "observe_return")

    def visit_ImportFrom(self, node):
        return None if node.module == "transport_test_observation" else node

    def visit_Expr(self, node):
        return None if self.hook(node.value) else self.generic_visit(node)

    def visit_With(self, node):
        node = self.generic_visit(node)
        if len(node.items) == 1 and self.hook(node.items[0].context_expr):
            return node.body
        return node

    def visit_If(self, node):
        node = self.generic_visit(node)
        return node if node.body or node.orelse else None

    def visit_Try(self, node):
        node = self.generic_visit(node)
        return node.body if not (node.handlers or node.finalbody or node.orelse) else node

    def visit_ExceptHandler(self, node):
        node = self.generic_visit(node)
        if not node.body:
            node.body = [ast.Pass()]
        if node.name == "error" and not any(
                isinstance(child, ast.Name) and child.id == "error"
                for statement in node.body for child in ast.walk(statement)):
            node.name = None
        return node


def semantic_tree(path):
    return ast.dump(WithoutObservation().visit(ast.parse(path.read_text(encoding="utf-8"))),
                    include_attributes=False)


def clean_environment(tmp, observation):
    # Same whitelist/options as the original GCC gate; never inherit secrets,
    # proxies, preload, suppressions, Python path or compiler settings.
    result = {key: os.environ[key] for key in
              ("PATH", "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "TZ") if key in os.environ}
    result.update(ASAN_OPTIONS="halt_on_error=1:detect_leaks=1",
                  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                  PYTHONNOUSERSITE="1", PYTHONDONTWRITEBYTECODE="1",
                  TMPDIR=str(tmp), **{ENVIRONMENT: str(observation)})
    return result


def prepare(args):
    source, build, output = args.source.resolve(), args.build.resolve(), args.output.resolve()
    require(not output.exists(), "diagnostic output already exists")
    evidence = build / "evidence"
    original_inputs = json.loads((evidence / "source-before.json").read_text())
    original_binaries = json.loads((evidence / "binaries-before.json").read_text())
    original_tests = json.loads((evidence / "test-plan.json").read_text())["tests"]
    require(original_inputs, "empty historical source manifest")
    for relative, expected in original_inputs.items():
        require(not Path(relative).is_absolute() and ".." not in Path(relative).parts,
                "invalid historical source path")
        require(identity(source / relative) == expected, "historical source identity changed: " + relative)
    for relative in OBSERVED_FILES:
        require(semantic_tree(source / relative) == semantic_tree(ROOT / relative),
                "overlay changes original behavior/ordering/assertions/deadlines: " + relative)
    tests = {test["name"]: test for test in original_tests}
    scenarios, binaries = {}, {}
    for name, (registration, relative, cls, method) in CASES.items():
        original = tests[registration]
        properties = {entry["name"]: entry["value"] for entry in original["properties"]}
        command = original["command"]
        require(len(command) == 4 and Path(command[1]) == source / relative and
                Path(command[3]) == source, "unexpected historical command")
        driver = Path(command[2])
        require(driver.parent == build and driver.name in original_binaries,
                "driver is not from the original build")
        binary = identity(driver)
        require(binary["sha256"] == original_binaries[driver.name], "historical driver changed")
        binaries[str(driver)] = binary
        python = Path(command[0])
        require(python.is_file(), "historical Python is missing")
        require(properties["TIMEOUT"] == 180, "historical TLS suite deadline changed")
        scenarios[name] = {"registration": registration, "script": relative, "class": cls,
                           "method": method, "driver": str(driver), "python": str(python),
                           "original_command": command, "timeout_s": properties["TIMEOUT"]}
    output.mkdir(mode=0o700, parents=True)
    overlay = output / "overlay"
    overlay.mkdir(mode=0o700)
    for relative in original_inputs:
        target = overlay / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / relative, target)
    for relative in (*OBSERVED_FILES, HELPER):
        shutil.copy2(ROOT / relative, overlay / relative)
    overlay_inputs = {relative: identity(overlay / relative) for relative in (*original_inputs, HELPER)}
    runner = output / "diagnose_transport_failures.py"
    shutil.copy2(Path(__file__).resolve(), runner)
    references = {str(evidence / name): identity(evidence / name)
                  for name in ("source-before.json", "binaries-before.json", "test-plan.json")}
    python_identities = {scenario["python"]: identity(Path(scenario["python"]).resolve())
                         for scenario in scenarios.values()}
    manifest = {"classification": "DIAGNOSTIC_NOT_GATE", "created_at": utc(),
                "source": str(source), "build": str(build), "overlay": str(overlay),
                "source_git_head": subprocess.check_output(
                    ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
                "original_inputs": original_inputs, "overlay_inputs": overlay_inputs,
                "binaries": binaries, "python_identities": python_identities,
                "references": references, "runner": {str(runner): identity(runner)},
                "scenarios": scenarios, "test_umask": "0002", "product_execution": "NOT_RUN",
                "historical_failures_closed": False,
                "hypotheses": {
                    "partial-upload": "Preparation, handshake, partial receive and prefix retention are distinguishable; prefix0 alone proves none of them.",
                    "effective-capacity": "A parent 18s timeout can be distinguished from a returned deadline/limit/Store rejection; no internal Store phase is inferred without observation.",
                    "large-escaped": "Original attempt status and transport/response codes can distinguish deadline interruption from a completed response refused at settlement."
                },
                "limits": ["Each original method once, selected rather than original suite order.",
                           "Observation timing and extra JSON parsing add measurable overhead.",
                           "No recompilation, producer changes, assertion/deadline changes or retries.",
                           "Parent-only observations cannot locate an unfinished internal Store operation.",
                           "A diagnostic pass cannot close earlier full-suite failures."]}
    write_json(output / "plan.json", manifest)
    print(json.dumps({"prepared": str(output / "plan.json"), "product_execution": "NOT_RUN"}))


def check_plan(plan):
    require(digest(Path(__file__).resolve()) == next(iter(plan["runner"].values()))["sha256"],
            "executing runner differs from prepared runner")
    for key in ("binaries", "references", "runner"):
        for path, expected in plan[key].items():
            require(identity(Path(path)) == expected, "prepared input changed: " + path)
    for path, expected in plan["python_identities"].items():
        require(identity(Path(path).resolve()) == expected, "historical Python changed")
    for label, base in (("original_inputs", "source"), ("overlay_inputs", "overlay")):
        for relative, expected in plan[label].items():
            require(identity(Path(plan[base]) / relative) == expected, "source input changed: " + relative)
    overlay = Path(plan["overlay"])
    actual = {path.relative_to(overlay).as_posix() for path in overlay.rglob("*")
              if path.is_file() or path.is_symlink()}
    require(actual == set(plan["overlay_inputs"]), "unexpected overlay file")


def worker(args):
    scenario = json.loads(args.output.joinpath("plan.json").read_text())["scenarios"][args.case]
    overlay = args.output / "overlay"
    script = overlay / scenario["script"]
    sys.path.insert(0, str(overlay / "tests"))
    sys.argv = [str(script), scenario["driver"], str(overlay)]
    module = runpy.run_path(str(script), run_name="selected_historical_diagnostic")
    suite = unittest.TestSuite([module[scenario["class"]](scenario["method"])])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    require(result.testsRun == 1 and not result.skipped, "diagnostic did not run its original method")
    return 0 if result.wasSuccessful() else 1


def observation_complete(path, stderr):
    try:
        warning = b"Transport test observation I/O failed; diagnostic evidence is incomplete."
        with stderr.open("rb") as stream:
            tail = b""
            while chunk := stream.read(65536):
                if warning in tail + chunk:
                    return False
                tail = chunk[-len(warning):]
        # The producer is bounded to 513 short records. Read with a separate
        # bound so a corrupt/unrelated file cannot become collector input.
        with path.open("rb") as stream:
            raw = stream.read(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            return False
        rows = [json.loads(line) for line in raw.splitlines()]
        return (bool(rows) and rows[-1].get("stage") == "observation_finished" and
                all(isinstance(row, dict) and row.get("sequence") == index and
                    row.get("stage") != "observation_limit" for index, row in enumerate(rows)))
    except (OSError, ValueError, AttributeError):
        return False


def run(args):
    output = args.output.resolve()
    plan = json.loads((output / "plan.json").read_text())
    check_plan(plan)
    require(not (output / "run-started.json").exists(), "this plan was already attempted; do not rerun for green")
    write_json(output / "run-started.json", {"started_at": utc(), "uid": os.getuid(),
                                           "plan_sha256": digest(output / "plan.json"),
                                           "classification": "DIAGNOSTIC_NOT_GATE"})
    receipts = []
    for name in CASES:
        scenario = plan["scenarios"][name]
        directory = output / name
        directory.mkdir(mode=0o700)
        temporary = directory / "tmp"
        temporary.mkdir(mode=0o700)
        command = [scenario["python"], str(output / "diagnose_transport_failures.py"),
                   "worker", "--output", str(output), "--case", name]
        started = time.monotonic()
        status, returncode, cleanup_signal = "NOT_RUN", None, None
        process = None
        with (directory / "stdout.log").open("xb") as stdout, (directory / "stderr.log").open("xb") as stderr:
            try:
                process = subprocess.Popen(command, cwd=plan["build"], stdout=stdout, stderr=stderr,
                    env=clean_environment(temporary, directory / "observation.jsonl"),
                    start_new_session=True, umask=0o002)
                try:
                    returncode = process.wait(timeout=scenario["timeout_s"])
                    status = "PASSED" if returncode == 0 else "FAILED"
                except subprocess.TimeoutExpired:
                    status = "TIMEOUT"
                finally:
                    # A dedicated process group contains both unittest and its
                    # child driver. Clean it even if its leader has already died.
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                        cleanup_signal = "SIGKILL"
                    except ProcessLookupError:
                        pass
                    returncode = process.wait()
            finally:
                receipt = {"classification": "DIAGNOSTIC_NOT_GATE", "method": scenario["method"],
                           "command": command, "cwd": plan["build"], "timeout_s": scenario["timeout_s"],
                           "elapsed_s": time.monotonic() - started, "status": status,
                           "returncode": returncode, "group_cleanup_signal": cleanup_signal,
                           "leader_reaped": process is not None and process.poll() is not None,
                           "observation_complete": observation_complete(directory / "observation.jsonl", directory / "stderr.log"),
                           "finished_at": utc(), "historical_failures_closed": False}
                write_json(directory / "execution.json", receipt)
                receipts.append(receipt)
        check_plan(plan)
    write_json(output / "completion.json", {"classification": "DIAGNOSTIC_NOT_GATE", "runs": receipts,
                                           "inputs_unchanged": True, "historical_failures_closed": False})
    return 0 if all(receipt["status"] == "PASSED" and receipt["observation_complete"]
                    for receipt in receipts) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    preparation = subparsers.add_parser("prepare")
    preparation.add_argument("--source", type=Path, required=True)
    preparation.add_argument("--build", type=Path, required=True)
    preparation.add_argument("--output", type=Path, required=True)
    execution = subparsers.add_parser("run")
    execution.add_argument("--output", type=Path, required=True)
    selected = subparsers.add_parser("worker", help=argparse.SUPPRESS)
    selected.add_argument("--output", type=Path, required=True)
    selected.add_argument("--case", choices=CASES, required=True)
    args = parser.parse_args()
    require(os.getuid() != 0, "ordinary user required")
    return {"prepare": prepare, "run": run, "worker": worker}[args.action](args)


if __name__ == "__main__":
    raise SystemExit(main())
