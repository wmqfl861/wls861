#!/usr/bin/env python3
"""Capture and preserve validation evidence locally; never upload private bytes."""
from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

SCHEMA = 1
REQUIRED = (
    "summary.json", "ctest-evidence.json", "ctest.xml", "ctest.log",
    "ctest.stderr.log", "ctest.step.json", "ctest-logs/LastTest.log",
    "source-before.json", "source-after.json", "binaries-before.json",
    "binaries-after.json", "expected-tests.json",
)
CAPTURE_FILES = {"command.json", "stdout.log", "stderr.log"}
PREREQUISITES = ["coordinator_acceptance", "restricted_destination",
                 "authorized_reviewer_acl", "enforced_short_retention",
                 "least_privilege_write_authentication", "verified_remote_readback"]
PUBLICATION = "PRIVATE; not an approved public artifact"
RETENTION = "NOT_CONFIGURED; no automatic expiry or secure-erasure claim"


class Refused(RuntimeError):
    pass


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise Refused(reason)


def utc() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def encode(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False,
                       indent=2) + "\n").encode("utf-8")


def decode(raw: bytes) -> object:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key")
            result[key] = value
        return result

    def finite(raw_number):
        number = float(raw_number)
        require(math.isfinite(number), "non-finite JSON number")
        return number

    def constant(_value):
        raise Refused("non-finite JSON number")

    return json.loads(raw, object_pairs_hook=unique, parse_float=finite,
                      parse_constant=constant)


def identity(info: os.stat_result) -> tuple[int, ...]:
    return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_gid,
            info.st_nlink, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def plain_path(path: Path) -> Path:
    absolute = Path(os.path.abspath(path))
    require(absolute == path.resolve(strict=True), "symlink or noncanonical path")
    return absolute


def private_root(path: Path) -> Path:
    path = plain_path(path)
    info = path.stat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.geteuid()
            and stat.S_IMODE(info.st_mode) == 0o700, "directory must be owned and mode 0700")
    return path


def exclusive(path: Path, data: bytes) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def file_record(path: Path) -> dict[str, object]:
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1
                and before.st_uid == os.geteuid(), "unsafe evidence file")
        checksum = hashlib.file_digest(stream, "sha256").hexdigest()
        require(identity(before) == identity(os.fstat(stream.fileno())) == identity(path.lstat()),
                "file changed during read")
        return {"sha256": checksum, "bytes": before.st_size, "mode": stat.S_IMODE(before.st_mode)}


def inventory(root: Path, destination: Path | None = None) -> dict[str, dict[str, object]]:
    """Pin each directory/file with openat; reject links, special files and races."""
    root = private_root(root)
    result = {}

    def walk(directory_fd: int, relative: Path) -> None:
        directory_before = os.fstat(directory_fd)
        require(directory_before.st_uid == os.geteuid()
                and stat.S_IMODE(directory_before.st_mode) == 0o700,
                "nested evidence directory must be owned and mode 0700")
        names = sorted(os.listdir(directory_fd))
        for name in names:
            require(name not in ("", ".", "..") and "/" not in name and "\\" not in name,
                    "unsafe evidence name")
            rel = relative / name
            info = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            if stat.S_ISDIR(info.st_mode):
                child = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                dir_fd=directory_fd)
                try:
                    require(identity(info) == identity(os.fstat(child)), "directory changed before read")
                    if destination is not None:
                        (destination / rel).mkdir(mode=0o700)
                    walk(child, rel)
                    require(identity(os.fstat(child)) == identity(
                        os.stat(name, dir_fd=directory_fd, follow_symlinks=False)), "directory replaced during read")
                finally:
                    os.close(child)
                continue
            require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1
                    and info.st_uid == os.geteuid(), "unsafe evidence file")
            descriptor = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                                 dir_fd=directory_fd)
            with os.fdopen(descriptor, "rb") as source:
                require(identity(info) == identity(os.fstat(source.fileno())), "file changed before read")
                checksum = hashlib.sha256()
                output = None
                try:
                    if destination is not None:
                        output = os.fdopen(os.open(destination / rel,
                            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), "wb")
                    while chunk := source.read(1024 * 1024):
                        checksum.update(chunk)
                        if output is not None:
                            output.write(chunk)
                    if output is not None:
                        output.flush()
                        os.fsync(output.fileno())
                finally:
                    if output is not None:
                        output.close()
                require(identity(info) == identity(os.fstat(source.fileno())) == identity(
                    os.stat(name, dir_fd=directory_fd, follow_symlinks=False)), "file changed during capture")
            result[rel.as_posix()] = {"sha256": checksum.hexdigest(), "bytes": info.st_size,
                                      "mode": stat.S_IMODE(info.st_mode)}
        require(names == sorted(os.listdir(directory_fd))
                and identity(directory_before) == identity(os.fstat(directory_fd)),
                "evidence directory changed during capture")

    descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        walk(descriptor, Path())
        require(identity(os.fstat(descriptor)) == identity(root.lstat()), "evidence root replaced")
    finally:
        os.close(descriptor)
    return result


def capture_command(argv: list[str], cwd: Path, output: Path, *, env=None) -> dict[str, object]:
    """Observe the actual child exit. This wrapper does not define a product gate."""
    require(bool(argv), "missing command")
    cwd = plain_path(cwd)
    parent = private_root(output.parent)
    output = parent / output.name
    output.mkdir(mode=0o700)
    receipt = {"schema_version": SCHEMA, "kind": "observed-child-exit-v1", "argv": argv,
               "cwd": str(cwd), "started_at": utc(), "ended_at": None, "exit_code": None,
               "pid": None, "signals": [], "forced_kill": False, "reaped": False,
               "status": "NOT_COMPLETE", "logs": {}}
    process = None
    started = time.monotonic()
    interrupted_at = None
    previous = {}

    def interrupt(number, _frame):
        receipt["signals"].append(number)

    try:
        for number in (signal.SIGINT, signal.SIGTERM):
            previous[number] = signal.signal(number, interrupt)
        with contextlib.ExitStack() as stack:
            handles = []
            for name in ("stdout.log", "stderr.log"):
                descriptor = os.open(output / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                handles.append(stack.enter_context(os.fdopen(descriptor, "wb")))
            process = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                       stdout=handles[0], stderr=handles[1], start_new_session=True)
            receipt["pid"] = process.pid
            while process.poll() is None:
                if receipt["signals"] and interrupted_at is None:
                    interrupted_at = time.monotonic()
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGTERM)
                if interrupted_at is not None and time.monotonic() - interrupted_at >= 30:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(process.pid, signal.SIGKILL)
                    receipt["forced_kill"] = True
                try:
                    process.wait(timeout=0.1)
                except subprocess.TimeoutExpired:
                    pass
            receipt["exit_code"] = process.wait()
            receipt["reaped"] = True
            receipt["status"] = "INTERRUPTED" if receipt["signals"] else "EXITED"
    except (Exception, KeyboardInterrupt) as error:
        receipt["error"] = type(error).__name__ + ": " + str(error)
        if process is not None:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            receipt["forced_kill"] = True
            receipt["exit_code"] = process.wait()
            receipt["reaped"] = True
        receipt["status"] = "CAPTURE_ERROR" if process is not None else "LAUNCH_FAILED"
    finally:
        try:
            if receipt["signals"] and receipt["status"] == "EXITED":
                receipt["status"] = "INTERRUPTED"
            receipt["ended_at"] = utc()
            receipt["seconds"] = time.monotonic() - started
            for name in ("stdout.log", "stderr.log"):
                if (output / name).exists():
                    receipt["logs"][name] = file_record(output / name)
            exclusive(output / "command.json", encode(receipt))
        finally:
            for number, handler in previous.items():
                signal.signal(number, handler)
    return receipt


def assessment(evidence: Path, capture: Path) -> dict[str, object]:
    """Check evidence links; retain all bytes even when a link is incomplete."""
    missing = [name for name in REQUIRED if not (evidence / name).is_file()]
    problems = []
    summary = {}
    ctest_exit = None
    ctest_interrupted = False
    junit_timeouts = None
    command = decode((capture / "command.json").read_bytes())
    fields = {"schema_version", "kind", "argv", "cwd", "started_at", "ended_at", "exit_code",
              "pid", "signals", "forced_kill", "reaped", "status", "logs", "seconds"}
    require(fields <= set(command) <= fields | {"error"}
            and command.get("schema_version") == SCHEMA and command.get("kind") == "observed-child-exit-v1",
            "unsupported command receipt")
    require(command["status"] in ("NOT_COMPLETE", "EXITED", "INTERRUPTED", "CAPTURE_ERROR", "LAUNCH_FAILED")
            and type(command["reaped"]) is bool and type(command["forced_kill"]) is bool
            and isinstance(command["signals"], list)
            and all(type(number) is int and number in (signal.SIGINT, signal.SIGTERM) for number in command["signals"])
            and isinstance(command["argv"], list) and bool(command["argv"])
            and all(isinstance(value, str) for value in command["argv"])
            and isinstance(command["cwd"], str) and Path(command["cwd"]).is_absolute()
            and set(command["logs"]) == {"stdout.log", "stderr.log"}, "invalid command receipt fields")
    require(command["status"] != "EXITED" or (command["reaped"] and not command["signals"]
            and not command["forced_kill"] and type(command["pid"]) is int and command["pid"] > 0),
            "inconsistent completed command receipt")
    for name in ("stdout.log", "stderr.log"):
        require(command["logs"].get(name) == file_record(capture / name), "command log mismatch")
    if command.get("reaped") is not True or type(command.get("exit_code")) is not int:
        problems.append("command_exit_unavailable")
    if command.get("status") != "EXITED":
        problems.append("command_not_uninterrupted")
    try:
        summary = decode((evidence / "summary.json").read_bytes())
        require(command["cwd"] == summary["source"], "command/source association mismatch")
        timestamps = [dt.datetime.fromisoformat(item) for item in
                      (command["started_at"], summary["started_at"], summary["ended_at"], command["ended_at"])]
        require(all(value.tzinfo is not None for value in timestamps) and timestamps == sorted(timestamps),
                "command/evidence time association mismatch")
        index_path = evidence / "ctest-evidence.json"
        require(summary["ctest_evidence"]["sha256"] == file_record(index_path)["sha256"],
                "CTest index identity mismatch")
        index = decode(index_path.read_bytes())
        for name, expected in index["artifacts"].items():
            require(Path(name).as_posix() == name and not Path(name).is_absolute()
                    and ".." not in Path(name).parts, "unsafe indexed evidence path")
            observed = file_record(evidence / name)
            require(all(observed[key] == expected[key] for key in ("sha256", "bytes")),
                    "indexed evidence identity mismatch")
        ctest_step = decode((evidence / "ctest.step.json").read_bytes())
        require(index["ctest_step"] == ctest_step, "CTest exit receipt mismatch")
        ctest_exit = ctest_step.get("exit_code")
        ctest_interrupted = bool(ctest_step.get("interrupted_or_launch_error"))
        require(ctest_exit is None or type(ctest_exit) is int, "invalid CTest exit code")
        if (evidence / "ctest.xml").is_file():
            try:
                junit = ET.parse(evidence / "ctest.xml").getroot()
                junit_timeouts = sum(failure.get("message") == "Timeout"
                                     for failure in junit.findall(".//testcase/failure"))
            except ET.ParseError:
                problems.append("junit_incomplete_or_malformed")
        if index["complete"] is not True:
            problems.append("runner_ctest_evidence_incomplete")
        if summary.get("identities_unchanged") is not True:
            problems.append("runner_identities_changed_or_unavailable")
        if command["exit_code"] == 0 and summary.get("status") != "PASSED":
            problems.append("zero_exit_without_passing_runner_summary")
        if command["exit_code"] != 0 and summary.get("status") == "PASSED":
            problems.append("nonzero_exit_with_passing_runner_summary")
    except (OSError, ValueError, KeyError, TypeError, Refused) as error:
        problems.append(type(error).__name__ + ": " + str(error))
    outcome = "UNKNOWN"
    if command["status"] == "INTERRUPTED" or ctest_interrupted:
        outcome = "INTERRUPTED"
    elif junit_timeouts:
        outcome = "TIMEOUT"
    elif command["reaped"] and type(command["exit_code"]) is int:
        outcome = "SUCCESS" if command["exit_code"] == 0 and summary.get("status") == "PASSED" else "FAILURE"
    return {"collection_status": "COMPLETE" if not missing and not problems else "INCOMPLETE",
            "missing": missing, "problems": problems, "runner_status": summary.get("status"),
            "command_status": command.get("status"), "command_exit_code": command.get("exit_code"),
            "execution_outcome": outcome, "ctest_exit_code": ctest_exit, "junit_timeout_cases": junit_timeouts,
            "hosted_persistence": "BLOCKED", "storage_scope": "LOCAL_PRIVATE_COPY_ONLY"}


def pack(evidence: Path, capture: Path, destination: Path) -> dict[str, object]:
    evidence, capture = private_root(evidence), private_root(capture)
    parent = private_root(destination.parent)
    destination = parent / destination.name
    require(not destination.is_relative_to(evidence) and not destination.is_relative_to(capture),
            "destination overlaps an evidence input")
    require(set(path.name for path in capture.iterdir()) == CAPTURE_FILES,
            "unexpected command capture member")
    # An exclusive directory is retained on any failure. Missing manifest means
    # incomplete packaging; it is never silently retried or overwritten.
    destination.mkdir(mode=0o700)
    records = {}
    for label, original in (("private-evidence", evidence), ("command-capture", capture)):
        target = destination / label
        target.mkdir(mode=0o700)
        copied = inventory(original, target)
        require(copied == inventory(original), "evidence changed before packaging completed")
        for name, item in copied.items():
            records[label + "/" + name] = dict(item, mode=0o600)
    result = assessment(destination / "private-evidence", destination / "command-capture")
    manifest = {"schema_version": SCHEMA, "kind": "local-private-evidence-bundle-v1",
                "created_at": utc(), "assessment": result, "files": records,
                "publication": PUBLICATION, "retention": RETENTION}
    exclusive(destination / "manifest.json", encode(manifest))
    verify(destination)
    return manifest


def verify(directory: Path) -> dict[str, object]:
    observed = inventory(directory)
    manifest = decode((directory / "manifest.json").read_bytes())
    require(set(manifest) == {"schema_version", "kind", "created_at", "assessment", "files", "publication", "retention"}
            and manifest["schema_version"] == SCHEMA and manifest["kind"] == "local-private-evidence-bundle-v1"
            and manifest["publication"] == PUBLICATION and manifest["retention"] == RETENTION,
            "unsupported private bundle")
    observed.pop("manifest.json")
    require(manifest["files"] == observed, "private bundle membership/hash/mode mismatch")
    require(manifest["assessment"] == assessment(directory / "private-evidence", directory / "command-capture"),
            "private bundle assessment mismatch")
    return manifest


def preflight(directory: Path) -> dict[str, object]:
    private_root(directory)
    return {"schema_version": SCHEMA, "local_directory": "OWNED_MODE_0700",
            "hosted_persistence": "BLOCKED", "missing_verified_prerequisites": PREREQUISITES,
            "raw_private_public_artifact_allowed": False,
            "encryption_or_upload_implemented": False,
            "note": "Local permissions do not establish remote ACL, retention or durable readback."}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    capture = sub.add_parser("capture")
    capture.add_argument("--directory", type=Path, required=True)
    capture.add_argument("--cwd", type=Path, required=True)
    capture.add_argument("command", nargs=argparse.REMAINDER)
    package = sub.add_parser("pack")
    package.add_argument("--evidence", type=Path, required=True)
    package.add_argument("--capture", type=Path, required=True)
    package.add_argument("--directory", type=Path, required=True)
    for name in ("verify", "preflight"):
        sub.add_parser(name).add_argument("--directory", type=Path, required=True)
    args = parser.parse_args(argv)
    require(sys.platform == "linux" and os.geteuid() != 0, "Linux ordinary user required")
    if args.action == "capture":
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        receipt = capture_command(command, args.cwd, args.directory)
        # A small, static terminal receipt. All argv, paths and raw output stay private.
        print(json.dumps({"status": receipt["status"], "command_exit_code": receipt["exit_code"]}))
        return 0 if receipt["status"] == "EXITED" and receipt["exit_code"] == 0 else 1
    if args.action == "preflight":
        print(json.dumps(preflight(args.directory), sort_keys=True))
        return 2
    manifest = pack(args.evidence, args.capture, args.directory) if args.action == "pack" else verify(args.directory)
    print(json.dumps({key: manifest["assessment"][key] for key in
                      ("collection_status", "command_exit_code", "runner_status", "hosted_persistence")}))
    return 0 if manifest["assessment"]["collection_status"] == "COMPLETE" else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Exception, KeyboardInterrupt):
        # Exception text can contain private paths/data. The exact invocation
        # and traceback may be captured by a private caller when diagnosing it.
        print("private evidence operation refused; originals remain private", file=sys.stderr)
        sys.exit(2)
