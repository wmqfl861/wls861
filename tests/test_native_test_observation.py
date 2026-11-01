#!/usr/bin/env python3
"""Explicit runner selfcheck for the native test observation contract.

This file is an explicit runner selfcheck, not one of the 92 registered CTest
cases and not part of the CTest catalog. CI runs it as a selfcheck step in
the strict and ASan jobs; locally run it directly, for example

    CXX=g++ python3 tests/test_native_test_observation.py

It compiles one throwaway helper translation unit (tests/test_support.hpp
only: no product sources, Store, network or credentials) with the project's
strict warning flags, inside a private directory under the caller-selected
TMPDIR, which the selfcheck never overrides. Each child starts from a fixed
umask (022) so a missing test::run umask restore cannot hide behind a
restrictive caller umask. Because the throwaway directory is deleted
afterwards, the selfcheck prints compact JSON evidence records: compile argv
and exit, helper source/binary SHA256 identities, and one per-scenario child
receipt (pid, observed lines so far, actual final returncode and reap
status) emitted on success and failure paths alike. Synchronization is a
fixture-only raw handshake plus a signal, never a timed sleep and never the
START/PASS/FAIL output under test:

  * the blocking method announces itself with a raw POSIX marker that
    bypasses the cout buffering under test, then blocks in a raw read with
    no iostream tie that could flush cout on its behalf;
  * after that handshake the parent requires the full START/PASS/FAIL
    sequence to already precede the marker, so a test_support without
    immediate flushes fails on missing observations after the proven
    reached-method handshake, not on a guard timeout;
  * the killed run must die by SIGKILL specifically (-signal.SIGKILL), with
    the earlier completed and failed methods still observable in order;
  * a released failure run keeps the failure text (quotes, spaces and a
    backslash) verbatim in order, the totals "3 passed, 1 failed", exit
    code 1 and test::run's umask isolation/restore;
  * a released all-pass run keeps the totals "3 passed, 0 failed" and exit
    code 0 without weakening the failure cases.

Guard timeouts only convert broken infrastructure (a hung helper or a
failed compile) into a selfcheck failure; they never synchronize with the
child and never extend any product test deadline.
"""

import hashlib
import json
import os
import re
import select
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HELPER_NAME = "native_observation_helper"
COMPILE_GUARD_SECONDS = 120.0
LINE_GUARD_SECONDS = 30.0
MARKER = "REACHED-BLOCKER"
FAIL_PREFIX = "FAIL failed original method: "
ESCAPED_FRAGMENT = 'synthetic "quoted" expression\\path'
# Only the helper source path and its line number vary inside the FAIL line.
FAIL_LINE = re.compile("^" + re.escape(FAIL_PREFIX) + "[^:]+:[0-9]+: " +
                       re.escape(ESCAPED_FRAGMENT) + "$")
UMASK_RESTORED = re.compile("^UMASK-RESTORED [0-9]+$")
STRICT_FLAGS = ["-std=c++20", "-Wall", "-Wextra", "-Wpedantic",
                "-Wconversion", "-Wshadow", "-Werror"]

HELPER_SOURCE = r'''
#include "test_support.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>

namespace {
int umask_inside_methods = -1;

void completing_method() {}

void failing_method() {
    // Escaping probe: quotes, spaces and a backslash must survive verbatim.
    ::test::check(false, "synthetic \"quoted\" expression\\path", __FILE__, __LINE__);
}

void umask_probe_method() {
    const mode_t inside = ::umask(0);
    (void)::umask(inside);
    umask_inside_methods = static_cast<int>(inside);
}

void reached_marker() {
    // Fixture-only handshake written straight to the pipe: it must not go
    // through the cout buffering this selfcheck verifies.
    const char* const marker = "REACHED-BLOCKER\n";
    const std::size_t total = std::strlen(marker);
    std::size_t remaining = total;
    while (remaining > 0) {
        const ssize_t written = ::write(STDERR_FILENO, marker + (total - remaining), remaining);
        if (written <= 0) return; // release or kill still drives every scenario
        remaining -= static_cast<std::size_t>(written);
    }
}

void blocking_method() {
    reached_marker();
    // Raw wait for release: no iostream tie, so nothing flushes cout for us,
    // and the process stays inside this read until the parent acts.
    char release = 0;
    const ssize_t received = ::read(STDIN_FILENO, &release, 1);
    (void)received;
}
} // namespace

int main(int argc, char** argv) {
    const bool all_pass = argc > 1 && std::string(argv[1]) == "all-pass";
    const mode_t before = ::umask(0);
    (void)::umask(before);
    int code = 0;
    if (all_pass) {
        code = test::run({
            {"completed original method", completing_method},
            {"umask probe method", umask_probe_method},
            {"blocks until released", blocking_method},
        });
    } else {
        code = test::run({
            {"completed original method", completing_method},
            {"failed original method", failing_method},
            {"umask probe method", umask_probe_method},
            {"blocks until released", blocking_method},
        });
    }
    const mode_t after = ::umask(0);
    (void)::umask(after);
    if (umask_inside_methods != 0077 || after != before) {
        std::cerr << "UMASK-BROKEN inside=" << umask_inside_methods
                  << " before=" << before << " after=" << after << std::endl;
        return 100 + code;
    }
    std::cout << "UMASK-RESTORED " << before << std::endl;
    return code;
}
'''


class SelfcheckError(Exception):
    """The observation contract is broken or the helper misbehaved."""


class LineReader:
    """Reads whole lines from a child pipe, bounded by a failure-only guard.

    select() decides when bytes exist; guard expiry only reports broken
    infrastructure. The guard never synchronizes with the helper.
    """

    def __init__(self, pipe):
        self._pipe = pipe
        self._buffer = bytearray()

    def line_or_none(self):
        """Next flushed line, or None on a clean end of the stream."""
        deadline = time.monotonic() + LINE_GUARD_SECONDS
        while b"\n" not in self._buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self._pipe], [], [], remaining)[0]:
                raise SelfcheckError("no helper line arrived within the guard window")
            chunk = os.read(self._pipe.fileno(), 4096)
            if not chunk:
                if self._buffer:
                    raise SelfcheckError("helper exited with an unfinished line: %r"
                                         % bytes(self._buffer))
                return None
            self._buffer.extend(chunk)
        line, _, rest = self._buffer.partition(b"\n")
        self._buffer = bytearray(rest)
        return line.decode("utf-8", "replace")

    def line(self):
        line = self.line_or_none()
        if line is None:
            raise SelfcheckError("helper exited before producing the expected line")
        return line


def require(condition, message):
    if not condition:
        raise SelfcheckError(message)


def emit_record(record):
    """One compact JSON evidence line; argv lists only, never a shell string."""
    print(json.dumps(record, sort_keys=True), flush=True)


def sha256_of(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def spawn_helper(helper, arguments):
    # argv lists everywhere; the selfcheck never builds a shell string. The
    # child starts from a fixed umask so a missing test::run restore cannot
    # hide behind an already-restrictive caller umask.
    return subprocess.Popen([str(helper)] + arguments, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0, cwd=str(helper.parent), umask=0o022)


def collect_until_marker(child):
    """Handshake on the independent raw reached-marker only; the caller then
    verifies the START/PASS/FAIL observations that must already precede it."""
    reader = LineReader(child.stdout)
    lines = []
    while not lines or lines[-1] != MARKER:
        line = reader.line()
        if line.startswith("RESULT"):
            raise SelfcheckError("infrastructure guard: helper finished before the "
                                 "blocking method was reached: %r" % (lines + [line],))
        lines.append(line)
    return reader, lines


def verify_sequence(observed, expected, context):
    """expected entries are ("eq", exact-line) or ("re", pattern) checks; the
    FAIL line is never filtered away, only its file/line prefix is variable.
    Failure messages keep the observed lines so a red still proves the
    raw-marker handshake was reached."""
    require(len(observed) == len(expected),
            "%s: expected %d lines, observed %d: %r"
            % (context, len(expected), len(observed), observed))
    for index, (kind, want) in enumerate(expected):
        line = observed[index]
        if kind == "eq":
            require(line == want, "%s line %d: expected %r, observed %r"
                    % (context, index, want, observed))
        else:
            require(want.match(line), "%s line %d: %r does not match the expected shape, observed %r"
                    % (context, index, line, observed))


def expected_before_marker(with_failure):
    expected = [
        ("eq", "START completed original method"),
        ("eq", "PASS completed original method"),
    ]
    if with_failure:
        expected += [("eq", "START failed original method"), ("re", FAIL_LINE)]
    expected += [
        ("eq", "START umask probe method"),
        ("eq", "PASS umask probe method"),
        ("eq", "START blocks until released"),
        ("eq", MARKER),
    ]
    return expected


def release_blocker(child):
    written = os.write(child.stdin.fileno(), b"r")
    require(written == 1, "short release write %r; handshake incomplete" % (written,))


def read_tail(reader, observed, context):
    for _ in range(3):
        observed.append(reader.line())
    require(reader.line_or_none() is None,
            "%s: unexpected trailing output: %r" % (context, observed))


def close_child(child, name, observed):
    """Cleanup on every path; the wait guard only reports a stuck child and
    never synchronizes the scenarios. Each spawned helper leaves exactly one
    receipt with its pid, the observed lines so far and its actual final
    returncode/reap status, on success and failure paths alike."""
    killed_here = child.poll() is None
    if killed_here:
        child.kill()
    reaped = True
    try:
        child.wait(timeout=LINE_GUARD_SECONDS)
    except subprocess.TimeoutExpired:
        reaped = False
    for stream in (child.stdin, child.stdout):
        if stream is not None:
            try:
                stream.close()
            except OSError:
                pass
    emit_record({"record": "scenario-child", "name": name, "pid": child.pid,
                 "returncode": child.returncode, "reaped": reaped,
                 "cleanup": "killed" if killed_here else "already-exited",
                 "observed": observed})
    if not reaped and sys.exc_info()[0] is None:
        # Fail the selfcheck, but never mask an in-flight verification
        # failure: that exception stays the visible cause, and the receipt
        # above already records the not-reaped status.
        raise SelfcheckError("[%s] helper pid %d was not reaped within the guard window"
                             % (name, child.pid))


def observe_killed_run(helper, name):
    child = spawn_helper(helper, [])
    observed = []
    try:
        _, observed = collect_until_marker(child)
        verify_sequence(observed, expected_before_marker(True), "killed run after handshake")
        require(child.poll() is None,
                "the blocking method left its raw wait before the kill; handshake invalid: %r"
                % (observed,))
        child.kill()  # SIGKILL: the process cannot flush anything while dying.
        exit_status = child.wait(timeout=LINE_GUARD_SECONDS)
    finally:
        close_child(child, name, observed)
    require(exit_status == -signal.SIGKILL,
            "expected death by SIGKILL (%d), got exit status %r" % (-signal.SIGKILL, exit_status))


def observe_released_failure_run(helper, name):
    child = spawn_helper(helper, [])
    observed = []
    try:
        reader, observed = collect_until_marker(child)
        verify_sequence(observed, expected_before_marker(True),
                        "released failure run after handshake")
        release_blocker(child)
        read_tail(reader, observed, "released failure run")
        exit_status = child.wait(timeout=LINE_GUARD_SECONDS)
    finally:
        close_child(child, name, observed)
    require(exit_status == 1,
            "failure exit code not preserved: expected 1, got %r" % (exit_status,))
    require(observed[-3] == "PASS blocks until released",
            "released blocker did not complete: %r" % (observed[-3:],))
    require(observed[-2] == "RESULT 3 passed, 1 failed",
            "failure totals changed: %r" % (observed[-3:],))
    require(UMASK_RESTORED.match(observed[-1]),
            "umask isolation/restore marker missing or malformed: %r" % (observed[-1:],))


def observe_all_pass_run(helper, name):
    child = spawn_helper(helper, ["all-pass"])
    observed = []
    try:
        reader, observed = collect_until_marker(child)
        verify_sequence(observed, expected_before_marker(False), "all-pass run after handshake")
        release_blocker(child)
        read_tail(reader, observed, "all-pass run")
        exit_status = child.wait(timeout=LINE_GUARD_SECONDS)
    finally:
        close_child(child, name, observed)
    require(exit_status == 0,
            "all-pass exit code not preserved: expected 0, got %r" % (exit_status,))
    require(observed[-3] == "PASS blocks until released",
            "released blocker did not complete: %r" % (observed[-3:],))
    require(observed[-2] == "RESULT 3 passed, 0 failed",
            "all-pass totals changed: %r" % (observed[-3:],))
    require(UMASK_RESTORED.match(observed[-1]),
            "umask isolation/restore marker missing or malformed: %r" % (observed[-1:],))


def main():
    tests_dir = Path(__file__).resolve().parent
    require((tests_dir / "test_support.hpp").is_file(),
            "tests/test_support.hpp not found next to this selfcheck")
    compiler = os.environ.get("CXX", "g++")
    with tempfile.TemporaryDirectory(prefix="wls861-observation-selfcheck-") as work:
        work_dir = Path(work)
        source_path = work_dir / (HELPER_NAME + ".cpp")
        helper_path = work_dir / HELPER_NAME
        source_path.write_text(HELPER_SOURCE, encoding="utf-8")
        compile_argv = [compiler] + STRICT_FLAGS + ["-I", str(tests_dir), str(source_path),
                                                    "-o", str(helper_path)]
        compiled = subprocess.run(compile_argv, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=COMPILE_GUARD_SECONDS)
        # The throwaway directory is deleted, so retain identities as records.
        emit_record({"record": "compile", "argv": compile_argv, "exit": compiled.returncode})
        if compiled.returncode != 0:
            sys.stdout.write(compiled.stdout.decode("utf-8", "replace"))
            raise SelfcheckError("helper compilation failed under %s with the strict flags"
                                 % compiler)
        emit_record({"record": "helper-identity",
                     "source_sha256": sha256_of(source_path),
                     "binary_sha256": sha256_of(helper_path)})
        scenarios = (
            ("killed blocker run", observe_killed_run),
            ("released failure run", observe_released_failure_run),
            ("released all-pass run", observe_all_pass_run),
        )
        for name, scenario in scenarios:
            # The single per-scenario evidence record is emitted by
            # close_child, on success and failure paths alike.
            try:
                scenario(helper_path, name)
            except SelfcheckError as error:
                raise SelfcheckError("[%s] %s" % (name, error))
    print("native test observation selfcheck PASS: raw-marker handshakes verified the killed "
          "run's observations with death by SIGKILL, released failure totals '3 passed, 1 "
          "failed' with exit 1, all-pass totals '3 passed, 0 failed' with exit 0, escaping "
          "and umask behavior")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (SelfcheckError, subprocess.TimeoutExpired) as failure:
        print("native test observation selfcheck FAIL: %s" % failure, file=sys.stderr)
        sys.exit(1)
