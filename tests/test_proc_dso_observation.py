#!/usr/bin/env python3
"""Deterministic self-checks for scripts/proc_dso_observation.py.

Run directly (NOT registered with CTest or Make, NOT a product sanitizer
result; run from the repository root or any directory):

    python3 -B tests/test_proc_dso_observation.py -v

All fixtures in groups 1-3 are synthetic bytes/paths/identities built by this
file; they are not and must not contain private experiment data, frozen
product inputs, real ELF/library hashes or machine paths.  Where the
historical single-read mechanism appears as a counterexample, this file
re-implements that mechanism synthetically (one bounded read call, no EOF
observation) instead of importing or executing any historical runner; the
binding of that counterexample to the original archived source is recorded in
the private implementation receipts, not here.

Group 4 runs one small ordinary-user fixture process (a stock interpreter
waiting on a controlled pipe).  It exercises the real /proc paths, real EOF
and target-exe binding against that process's own limited static identity.
It does not call any WLS code, create a business Store or run product tests,
and it reaps only that one fixture process.  If the platform denies the
needed /proc access, the case is reported as TOOL_INTEGRATION_BLOCKED and
skipped, never silently passed.

The F-02 additions in StreamReadTests check the post-read budget contract
with an event-driven injected clock (the clock only advances at read
boundaries, so the same fixtures stay deterministic on any implementation).
The F-03 additions in FileObjectTests check the fixed-fd file-object
contract with deterministic wrapped-OS-call seams confined to one fixture
path/fd, same-bytes different-inode swap files, and EBADF fd-closure proofs
(no /proc/self/fd enumeration).  All their fixtures are created and removed
inside this file's own temporary directories.
"""

from __future__ import annotations

import errno
import hashlib
import os
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "scripts"))

import proc_dso_observation as pdo  # noqa: E402


class ScriptedRead:
    """Deterministic read/clock fixtures; no sleeps are used anywhere."""

    def __init__(self, chunks, error=None, always_error=None):
        self.chunks = list(chunks)
        self.error = error
        self.always_error = always_error
        self.calls = 0

    def __call__(self, count):
        self.calls += 1
        if self.always_error is not None:
            raise self.always_error
        if not self.chunks:
            return b""
        if self.error is not None and self.calls > len(self.chunks):
            raise self.error
        return self.chunks.pop(0)


class ListClock:
    """Deterministic clock over a finite value list.  Once the list is
    exhausted the last value is explicitly remembered and repeated, so
    implementations that sample the clock a different number of times still
    see a defined monotonic instant instead of an indexing failure."""

    def __init__(self, values):
        self.values = list(values)
        self.last = self.values[0] if self.values else None

    def __call__(self):
        if self.values:
            self.last = self.values.pop(0)
        return self.last


def advancing_clock_read(events, start=0.0):
    """Deterministic (read, clock) pair for post-read budget checks: one
    shared monotonic clock that only advances when the read callback runs,
    to the scheduled instant of that read.  No sleeps are used; the queue is
    finite, so an implementation needing one read more than scheduled fails
    loudly instead of silently reusing a stale instant."""
    state = {"now": start}
    queue = list(events)

    def read(_count):
        when, block = queue.pop(0)
        state["now"] = when
        return block

    def clock():
        return state["now"]

    return read, clock


def chunked_read(data, size):
    state = {"pos": 0}

    def read(_count):
        start = state["pos"]
        state["pos"] = start + size
        return data[start:state["pos"]]
    return read


def map_line(start, end, perms, offset, device, inode, path):
    return (f"{start:x}-{end:x} {perms} {offset:08x} "
            f"{device[0]:02x}:{device[1]:02x} {inode} {path}")


class SyntheticMaps:
    """Nine synthetic expected DSOs with distinct file-object identities."""

    DEVICE = (5, 1)
    NAMES = [f"libsynthetic{i}.so.{i}" for i in range(9)]

    def __init__(self, missing=None, deleted=None, wrong_inode=None):
        self.missing = set(missing or ())
        self.deleted = set(deleted or ())
        self.wrong_inode = set(wrong_inode or ())
        lines = [map_line(0x1000, 0x2000, "rw-p", 0, (0, 0), 0, "[stack]"),
                 map_line(0x2000, 0x3000, "rw-p", 0, (0, 0), 0, "[heap]")]
        start = 0x7f0000000000
        for index, name in enumerate(self.NAMES):
            if name in self.missing:
                continue
            inode = 20000 + index + (1000 if name in self.wrong_inode else 0)
            suffix = " (deleted)" if name in self.deleted else ""
            lines.append(map_line(start, start + 0x1000, "r-xp", 0, self.DEVICE,
                                  inode, f"/usr/synthetic/lib/{name}{suffix}"))
            start += 0x2000
        lines.append(map_line(0xffffffffff600000, 0xffffffffff601000,
                              "--xp", 0, (0, 0), 0, "[vsyscall]"))
        self.bytes = ("\n".join(lines) + "\n").encode()
        self.expected = [{"name": name,
                          "device": [self.DEVICE[0], self.DEVICE[1]],
                          "inode": 20000 + index}
                         for index, name in enumerate(self.NAMES)]

    @property
    def complete_expected(self):
        return self.expected


class FakeProcIO(pdo.ProcIO):
    """Whole-/proc injection for observe_target and discovery tests."""

    def __init__(self, processes):
        self.processes = {int(pid): spec for pid, spec in processes.items()}

    def list_pids(self):
        return sorted(self.processes)

    def pid_identity(self, pid):
        spec = self.processes.get(int(pid))
        return dict(spec["identity"]) if spec else None

    def exe_identity(self, pid):
        spec = self.processes.get(int(pid))
        return dict(spec["exe"]) if spec else None

    def read_text_stream(self, pid, name, **budgets):
        spec = self.processes.get(int(pid))
        if spec is None:
            raise ProcessLookupError(f"no such pid {pid}")
        if name == "maps":
            return pdo.read_proc_stream(chunked_read(spec["maps"], 13), **budgets)
        raise ValueError(f"unsupported fake stream {name}")


class LateEofProcIO(FakeProcIO):
    """FakeProcIO whose maps stream ends with a genuine zero-byte EOF that
    only arrives after the capture budget: the fixture-owned clock jumps
    exactly when the final EOF read runs.  Identity/exe premises stay valid,
    so only the late EOF can block evidence completion."""

    def __init__(self, processes, jump_to=5.0):
        super().__init__(processes)
        self.jump_to = jump_to

    def read_text_stream(self, pid, name, **budgets):
        spec = self.processes.get(int(pid))
        if spec is None:
            raise ProcessLookupError(f"no such pid {pid}")
        if name != "maps":
            raise ValueError(f"unsupported fake stream {name}")
        if "clock" in budgets:
            raise ValueError("LateEofProcIO owns the clock for this fixture")
        base = chunked_read(spec["maps"], 13)
        state = {"now": 0.0}

        def read(count):
            block = base(count)
            if not block:
                state["now"] = self.jump_to
            return block

        return pdo.read_proc_stream(read, clock=lambda: state["now"], **budgets)


def synthetic_identity(pid=4242, ppid=100, start_ticks=999999):
    return {"pid": pid, "state": "S", "ppid": ppid, "pgid": ppid,
            "sid": ppid, "start_ticks": start_ticks, "uid": os.getuid()}


def synthetic_exe(sha="a" * 64, inode=7777, device=(9, 9), size=4096):
    return {"device": [device[0], device[1]], "inode": inode,
            "bytes": size, "read_bytes": size, "sha256": sha}


class StreamReadTests(unittest.TestCase):
    def test_single_read_counterexample_and_new_loop_eof_proof(self):
        """Group 1: the old one-shot mechanism returns a prefix with no EOF
        proof; the new loop recovers the exact original bytes and a real
        zero-byte EOF.  The old mechanism is re-implemented synthetically here
        (one os.read-style call); its archived-source binding is private."""
        maps = SyntheticMaps()
        legacy_reader = chunked_read(maps.bytes, 13)
        legacy_prefix = legacy_reader(len(maps.bytes) + 1)
        self.assertLess(len(legacy_prefix), len(maps.bytes))
        self.assertNotEqual(legacy_prefix, maps.bytes)
        # Required DSO rows all sit after the first short block, and at least
        # one mapping row is split across a read boundary.
        first_lib = maps.bytes.index(b"/usr/synthetic/lib/")
        self.assertGreater(first_lib, len(legacy_prefix))
        boundaries = set(range(13, len(maps.bytes), 13))
        line_bounds = []
        offset = 0
        for line in maps.bytes.split(b"\n"):
            if line.startswith(b"7f") and b"/usr/synthetic" in line:
                line_bounds.append((offset, offset + len(line)))
            offset += len(line) + 1
        self.assertTrue(any(any(start < b < end for b in boundaries)
                            for start, end in line_bounds))
        # New mechanism: full original bytes plus a genuinely observed EOF.
        result = pdo.read_proc_stream(chunked_read(maps.bytes, 13))
        self.assertEqual(result.read_status, pdo.READ_COMPLETE)
        self.assertTrue(result.eof_observed)
        self.assertEqual(result.data, maps.bytes)
        self.assertEqual(result.reason, pdo.REASON_EOF)

    def test_complete_at_exact_byte_limit_with_eof(self):
        payload = b"x" * 16
        reads = ScriptedRead([b"xxxxxxxxxxxxxxxx", b""])
        result = pdo.read_proc_stream(reads, max_bytes=16, max_read_attempts=8,
                                      capture_seconds=1.0)
        self.assertEqual(result.read_status, pdo.READ_COMPLETE)
        self.assertTrue(result.eof_observed)
        self.assertEqual(result.data, payload)

    def test_fail_one_byte_over_limit(self):
        reads = ScriptedRead([b"x" * 16, b"y"])
        result = pdo.read_proc_stream(reads, max_bytes=16, max_read_attempts=8,
                                      capture_seconds=1.0)
        self.assertEqual(result.read_status, pdo.READ_FAIL)
        self.assertEqual(result.reason, pdo.REASON_SIZE_LIMIT_EXCEEDED)
        self.assertFalse(result.eof_observed)
        self.assertIsNone(result.data)
        self.assertEqual(result.total_bytes, 17)

    def test_repeated_eintr_exhausts_read_budget(self):
        reads = ScriptedRead([], always_error=InterruptedError())
        result = pdo.read_proc_stream(reads, max_read_attempts=5,
                                      capture_seconds=1.0)
        self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
        self.assertEqual(result.reason, pdo.REASON_EINTR_BUDGET_EXHAUSTED)
        self.assertEqual(result.read_attempts, 5)
        self.assertFalse(result.eof_observed)

    def test_eagain_is_incomplete(self):
        reads = ScriptedRead([], always_error=BlockingIOError())
        result = pdo.read_proc_stream(reads, capture_seconds=1.0)
        self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
        self.assertEqual(result.reason, pdo.REASON_EAGAIN)

    def test_permission_denied_is_incomplete(self):
        reads = ScriptedRead([], always_error=PermissionError())
        result = pdo.read_proc_stream(reads, capture_seconds=1.0)
        self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
        self.assertEqual(result.reason, pdo.REASON_PERMISSION_DENIED)

    def test_process_disappearance_is_incomplete(self):
        reads = ScriptedRead([], always_error=ProcessLookupError())
        result = pdo.read_proc_stream(reads, capture_seconds=1.0)
        self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
        self.assertEqual(result.reason, pdo.REASON_PROCESS_DISAPPEARED)

    def test_monotonic_time_budget_exhausted(self):
        reads = ScriptedRead([b"partial-data"])
        clock = ListClock([0.0, 0.0, 0.3])
        result = pdo.read_proc_stream(reads, capture_seconds=0.25, clock=clock)
        self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
        self.assertEqual(result.reason, pdo.REASON_TIME_BUDGET_EXHAUSTED)
        self.assertFalse(result.eof_observed)
        self.assertEqual(result.data, b"partial-data")

    def test_read_attempt_budget_exhausted_with_data(self):
        reads = ScriptedRead([b"ab"] * 4)
        result = pdo.read_proc_stream(reads, max_read_attempts=4,
                                      capture_seconds=1.0)
        self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
        self.assertEqual(result.reason, pdo.REASON_READ_ATTEMPTS_EXHAUSTED)
        self.assertEqual(result.total_bytes, 8)

    def test_generic_oserror_eio_is_readerror(self):
        """A generic OSError without a specific subclass mapping (EIO) must
        surface as ReadError chained to the original error; it is never
        swallowed, returned as a ReadResult, or downgraded to success.  When
        the module was missing the errno import this path escaped as
        NameError instead of the designed ReadError."""
        error = OSError(errno.EIO, "Input/output error")
        reads = ScriptedRead([], always_error=error)
        with self.assertRaises(pdo.ReadError) as caught:
            pdo.read_proc_stream(reads, capture_seconds=1.0)
        self.assertIs(caught.exception.__cause__, error)
        self.assertEqual(caught.exception.__cause__.errno, errno.EIO)
        self.assertEqual(reads.calls, 1)

    def test_elapsed_seconds_from_nonzero_clock(self):
        """Elapsed time from a nonzero monotonic clock start must not subtract
        the start twice; each branch keeps its original status/reason/EOF
        semantics with a nonnegative, exactly representable elapsed."""
        with self.subTest("eof_complete"):
            reads = ScriptedRead([b"abc", b""])
            clock = ListClock([100.0, 100.0, 100.0625, 100.125])
            result = pdo.read_proc_stream(reads, capture_seconds=1.0,
                                          clock=clock)
            self.assertEqual(result.read_status, pdo.READ_COMPLETE)
            self.assertEqual(result.reason, pdo.REASON_EOF)
            self.assertTrue(result.eof_observed)
            self.assertEqual(result.data, b"abc")
            self.assertEqual(result.total_bytes, 3)
            self.assertEqual(result.elapsed_seconds, 0.125)
            self.assertGreaterEqual(result.elapsed_seconds, 0.0)
        with self.subTest("time_budget_incomplete"):
            reads = ScriptedRead([b"partial-data"])
            clock = ListClock([100.0, 100.0, 100.5])
            result = pdo.read_proc_stream(reads, capture_seconds=0.25,
                                          clock=clock)
            self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
            self.assertEqual(result.reason, pdo.REASON_TIME_BUDGET_EXHAUSTED)
            self.assertFalse(result.eof_observed)
            self.assertEqual(result.data, b"partial-data")
            self.assertEqual(result.elapsed_seconds, 0.5)
            self.assertGreaterEqual(result.elapsed_seconds, 0.0)
        with self.subTest("read_attempts_incomplete"):
            reads = ScriptedRead([b"ab"] * 4)
            clock = ListClock([100.0, 100.0, 100.0, 100.0, 100.0, 100.125])
            result = pdo.read_proc_stream(reads, max_read_attempts=4,
                                          capture_seconds=1.0, clock=clock)
            self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
            self.assertEqual(result.reason, pdo.REASON_READ_ATTEMPTS_EXHAUSTED)
            self.assertFalse(result.eof_observed)
            self.assertEqual(result.total_bytes, 8)
            self.assertEqual(result.elapsed_seconds, 0.125)
            self.assertGreaterEqual(result.elapsed_seconds, 0.0)
        with self.subTest("size_limit_fail"):
            reads = ScriptedRead([b"x" * 16, b"y"])
            clock = ListClock([100.0, 100.0, 100.0, 100.5])
            result = pdo.read_proc_stream(reads, max_bytes=16,
                                          max_read_attempts=8,
                                          capture_seconds=1.0, clock=clock)
            self.assertEqual(result.read_status, pdo.READ_FAIL)
            self.assertEqual(result.reason, pdo.REASON_SIZE_LIMIT_EXCEEDED)
            self.assertFalse(result.eof_observed)
            self.assertIsNone(result.data)
            self.assertEqual(result.total_bytes, 17)
            self.assertEqual(result.elapsed_seconds, 0.5)
            self.assertGreaterEqual(result.elapsed_seconds, 0.0)

    def test_final_eof_over_time_budget(self):
        """F-02 core counterexample: the genuine zero-byte EOF lands after
        the monotonic capture budget.  The original loop checked time only
        before each read and classified this COMPLETE; the contract now
        samples the clock once after every successful read, so the same EOF
        must be INCOMPLETE/TIME_BUDGET_EXHAUSTED with the EOF fact kept.
        Real helper with an injected event clock; no /proc end-to-end run is
        implied."""
        read, clock = advancing_clock_read([(0.5, b"")])
        result = pdo.read_proc_stream(read, capture_seconds=0.25, clock=clock)
        self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
        self.assertEqual(result.reason, pdo.REASON_TIME_BUDGET_EXHAUSTED)
        self.assertTrue(result.eof_observed)
        self.assertEqual(result.elapsed_seconds, 0.5)
        self.assertEqual(result.read_attempts, 1)
        self.assertEqual(result.data, b"")

    def test_post_read_time_boundaries(self):
        """F-02 boundary matrix: an EOF inside or exactly at the budget still
        completes (the strict ``>`` boundary and every budget are unchanged);
        a non-empty block landing past the budget is INCOMPLETE with its
        in-budget data and real counts kept; an EOF on the last permitted
        read still completes; exhausting attempts without any EOF stays
        INCOMPLETE."""
        with self.subTest("eof_just_inside_budget_completes"):
            read, clock = advancing_clock_read([(0.249, b"")])
            result = pdo.read_proc_stream(read, capture_seconds=0.25, clock=clock)
            self.assertEqual(result.read_status, pdo.READ_COMPLETE)
            self.assertEqual(result.reason, pdo.REASON_EOF)
            self.assertTrue(result.eof_observed)
            self.assertEqual(result.elapsed_seconds, 0.249)
        with self.subTest("eof_exactly_at_budget_completes"):
            read, clock = advancing_clock_read([(0.25, b"")])
            result = pdo.read_proc_stream(read, capture_seconds=0.25, clock=clock)
            self.assertEqual(result.read_status, pdo.READ_COMPLETE)
            self.assertTrue(result.eof_observed)
            self.assertEqual(result.elapsed_seconds, 0.25)
        with self.subTest("late_non_empty_block_is_incomplete"):
            read, clock = advancing_clock_read([(0.5, b"data")])
            result = pdo.read_proc_stream(read, capture_seconds=0.25, clock=clock)
            self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
            self.assertEqual(result.reason, pdo.REASON_TIME_BUDGET_EXHAUSTED)
            self.assertFalse(result.eof_observed)
            self.assertEqual(result.data, b"data")
            self.assertEqual(result.total_bytes, 4)
            self.assertEqual(result.read_lengths, [4])
            self.assertEqual(result.read_attempts, 1)
            self.assertEqual(result.elapsed_seconds, 0.5)
        with self.subTest("eof_on_last_permitted_read_completes"):
            read, clock = advancing_clock_read([(0.0, b"a"), (0.1, b"")])
            result = pdo.read_proc_stream(read, capture_seconds=1.0,
                                           max_read_attempts=2, clock=clock)
            self.assertEqual(result.read_status, pdo.READ_COMPLETE)
            self.assertTrue(result.eof_observed)
            self.assertEqual(result.read_attempts, 2)
            self.assertEqual(result.data, b"a")
        with self.subTest("attempts_exhausted_without_eof_stays_incomplete"):
            def always_data(_count):
                return b"ab"
            result = pdo.read_proc_stream(always_data, capture_seconds=5.0,
                                           max_read_attempts=4)
            self.assertEqual(result.read_status, pdo.READ_INCOMPLETE)
            self.assertEqual(result.reason, pdo.REASON_READ_ATTEMPTS_EXHAUSTED)
            self.assertFalse(result.eof_observed)
            self.assertEqual(result.total_bytes, 8)
            self.assertEqual(result.read_attempts, 4)

    def test_size_limit_outranks_late_time_in_same_read(self):
        """F-02 priority: when one and the same read both delivers the
        (limit + 1)-th byte and lands past the time budget,
        SIZE_LIMIT_EXCEEDED wins; no oversized snapshot is retained and EOF
        cannot be claimed for that read."""
        read, clock = advancing_clock_read([(0.5, b"x" * 17)])
        result = pdo.read_proc_stream(read, capture_seconds=0.25,
                                      max_bytes=16, clock=clock)
        self.assertEqual(result.read_status, pdo.READ_FAIL)
        self.assertEqual(result.reason, pdo.REASON_SIZE_LIMIT_EXCEEDED)
        self.assertFalse(result.eof_observed)
        self.assertIsNone(result.data)
        self.assertEqual(result.total_bytes, 17)
        self.assertEqual(result.read_attempts, 1)
        self.assertEqual(result.elapsed_seconds, 0.5)

    def test_late_eof_blocks_evidence_complete(self):
        """F-02 propagation through the synthetic upper layer: FakeProcIO
        forwards to the real read_proc_stream, so a genuinely late EOF keeps
        the target/DSO premises valid while maps_eof_complete and
        evidence_complete must both stay false.  Explicitly a synthetic
        upper-layer composition, not a real /proc observation."""
        maps = SyntheticMaps()
        io = LateEofProcIO({4242: {"identity": synthetic_identity(),
                                   "exe": synthetic_exe(),
                                   "maps": maps.bytes}}, jump_to=5.0)
        expected_identity = {"pid": 4242, "start_ticks": 999999,
                             "uid": os.getuid(), "ppid": 100, "pgid": 100,
                             "sid": 100}
        result = pdo.observe_target(4242, expected_pid_identity=expected_identity,
                                    expected_elf=synthetic_exe(),
                                    expected_dsos=maps.expected, io=io)
        self.assertEqual(result["maps_read"]["read_status"], pdo.READ_INCOMPLETE)
        self.assertEqual(result["maps_read"]["reason"],
                         pdo.REASON_TIME_BUDGET_EXHAUSTED)
        self.assertTrue(result["maps_read"]["eof_observed"])
        self.assertFalse(result["maps_eof_complete"])
        self.assertTrue(result["target_identity_verified"])
        self.assertFalse(result["evidence_complete"])


class MapsParseTests(unittest.TestCase):
    def test_parse_complete_maps_and_reject_bad_lines(self):
        maps = SyntheticMaps()
        entries, errors = pdo.parse_maps(maps.bytes)
        self.assertEqual(errors, [])
        self.assertEqual(len(entries), 12)
        dso_entries = [e for e in entries if e.path and e.path.startswith("/usr/synthetic")]
        self.assertEqual(len(dso_entries), 9)
        corrupted = maps.bytes + b"not a maps line\n"
        entries2, errors2 = pdo.parse_maps(corrupted)
        self.assertEqual([e["reason"] for e in errors2], ["UNPARSEABLE_LINE"])
        self.assertEqual(len(entries2), 12)
        self.assertEqual(pdo.parse_maps(b"bad\x00data")[1][0]["reason"],
                         "NUL_BYTE_IN_STREAM")

    def test_parse_accepts_kernel_alignment_padding(self):
        padded = (b"00400000-00420000 r--p 00000000 fd:01 6316"
                  b"                               /usr/synthetic/lib/libpad.so\n")
        entries, errors = pdo.parse_maps(padded)
        self.assertEqual(errors, [])
        self.assertEqual(len(entries), 1)
        entry = entries[0]
        self.assertEqual(entry.inode, 6316)
        self.assertEqual(entry.device, [0xfd, 0x1])
        self.assertEqual(entry.path, "/usr/synthetic/lib/libpad.so")
        self.assertFalse(entry.deleted)


class TargetBindingTests(unittest.TestCase):
    def _io(self, maps_obj, identity=None, exe=None, pid=4242):
        spec = {"identity": identity or synthetic_identity(pid=pid),
                "exe": exe or synthetic_exe(), "maps": maps_obj.bytes}
        return FakeProcIO({pid: spec, 100: {"identity": synthetic_identity(pid=100, ppid=1),
                                            "exe": synthetic_exe(),
                                            "maps": b""}})

    def _expected_identity(self, pid=4242, ppid=100, start_ticks=999999):
        return {"pid": pid, "start_ticks": start_ticks, "uid": os.getuid(),
                "ppid": ppid, "pgid": ppid, "sid": ppid}

    def test_complete_target_passes(self):
        maps = SyntheticMaps()
        result = pdo.observe_target(4242, expected_pid_identity=self._expected_identity(),
                                    expected_elf=synthetic_exe(),
                                    expected_dsos=maps.expected, io=self._io(maps))
        self.assertTrue(result["maps_eof_complete"])
        self.assertTrue(result["target_identity_verified"])
        self.assertTrue(result["target_direct_dso_complete"])
        self.assertTrue(result["evidence_complete"])
        self.assertEqual(result["missing_dsos"], [])

    def test_missing_dso_cannot_be_filled_by_parent_union(self):
        """Group 3: the target lacks one of nine; a parent holding all nine
        must not complete the target's direct set."""
        target_maps = SyntheticMaps(missing=[SyntheticMaps.NAMES[3]])
        io = self._io(target_maps)
        result = pdo.observe_target(4242, expected_pid_identity=self._expected_identity(),
                                    expected_elf=synthetic_exe(),
                                    expected_dsos=SyntheticMaps().expected, io=io)
        self.assertFalse(result["target_direct_dso_complete"])
        self.assertFalse(result["evidence_complete"])
        self.assertEqual(result["missing_dsos"], [SyntheticMaps.NAMES[3]])
        # The same expected set verified against a parent's complete mappings
        # in a separate call does not change the target verdict above.
        parent_result = pdo.verify_direct_dso_entries(
            pdo.parse_maps(SyntheticMaps().bytes)[0], SyntheticMaps().expected)
        self.assertTrue(parent_result["complete"])
        self.assertFalse(result["evidence_complete"])

    def test_start_ticks_mismatch_fails_identity(self):
        maps = SyntheticMaps()
        wrong = self._expected_identity(start_ticks=111111)
        result = pdo.observe_target(4242, expected_pid_identity=wrong,
                                    expected_elf=synthetic_exe(),
                                    expected_dsos=maps.expected, io=self._io(maps))
        self.assertEqual(result["identity_failure"], "PID_IDENTITY_BEFORE_MISMATCH")
        self.assertFalse(result["target_identity_verified"])
        self.assertFalse(result["evidence_complete"])

    def test_same_pid_swapped_exe_fails(self):
        maps = SyntheticMaps()
        expected_exe = synthetic_exe(sha="b" * 64)
        result = pdo.observe_target(4242, expected_pid_identity=self._expected_identity(),
                                    expected_elf=expected_exe,
                                    expected_dsos=maps.expected,
                                    io=self._io(maps, exe=synthetic_exe(sha="c" * 64)))
        self.assertEqual(result["identity_failure"], "EXE_IDENTITY_BEFORE_MISMATCH")
        self.assertFalse(result["evidence_complete"])

    def test_device_or_inode_mismatch_keeps_dso_missing(self):
        maps = SyntheticMaps(wrong_inode=[SyntheticMaps.NAMES[7]])
        result = pdo.observe_target(4242, expected_pid_identity=self._expected_identity(),
                                    expected_elf=synthetic_exe(),
                                    expected_dsos=maps.expected, io=self._io(maps))
        self.assertFalse(result["target_direct_dso_complete"])
        self.assertEqual(result["missing_dsos"], [SyntheticMaps.NAMES[7]])

    def test_deleted_mapping_is_not_evidence(self):
        maps = SyntheticMaps(deleted=[SyntheticMaps.NAMES[0]])
        result = pdo.observe_target(4242, expected_pid_identity=self._expected_identity(),
                                    expected_elf=synthetic_exe(),
                                    expected_dsos=maps.expected, io=self._io(maps))
        self.assertFalse(result["target_direct_dso_complete"])
        self.assertEqual(result["missing_dsos"], [SyntheticMaps.NAMES[0]])
        deleted_entry = [e for e in pdo.parse_maps(maps.bytes)[0]
                         if e.path and e.path.endswith(" (deleted)")]
        self.assertEqual(len(deleted_entry), 1)
        self.assertTrue(deleted_entry[0].deleted)

    def test_discovery_rejects_root_reuse_and_ambiguity(self):
        io = self._io(SyntheticMaps())
        reused = dict(self._expected_identity(pid=100, ppid=1), start_ticks=1)
        found = pdo.discover_target_child(100, expected_root_identity=reused, io=io)
        self.assertFalse(found["root_verified"])
        self.assertFalse(found["found"])
        io.processes[5001] = {"identity": synthetic_identity(pid=5001, ppid=100),
                              "exe": synthetic_exe(), "maps": SyntheticMaps().bytes}
        io.processes[5002] = {"identity": synthetic_identity(pid=5002, ppid=100),
                              "exe": synthetic_exe(), "maps": SyntheticMaps().bytes}
        two = pdo.discover_target_child(100, expected_root_identity=self._expected_identity(pid=100, ppid=1),
                                        io=io)
        self.assertTrue(two["root_verified"])
        self.assertTrue(two["found"])
        self.assertFalse(two["unambiguous"])
        self.assertEqual(two["reason"], "AMBIGUOUS_DIRECT_CHILDREN")


class SnapshotStoreTests(unittest.TestCase):
    def test_dedup_and_budget_boundaries(self):
        store = pdo.SnapshotStore(max_count=4, max_total_bytes=64)
        first = store.add(b"a" * 16, {"pid": 1}, {"read_attempts": 2})
        self.assertEqual(first["status"], "STORED")
        duplicate = store.add(b"a" * 16, {"pid": 1}, {"read_attempts": 2})
        self.assertEqual(duplicate["status"], "DEDUPLICATED")
        self.assertEqual(duplicate["snapshot_id"], first["snapshot_id"])
        self.assertEqual(store.total_bytes, 16)
        store.add(b"b" * 16, {"pid": 1}, {})
        store.add(b"c" * 16, {"pid": 1}, {})
        store.add(b"d" * 16, {"pid": 1}, {})
        self.assertEqual(len(store.records), 4)
        rejected = store.add(b"e" * 16, {"pid": 1}, {})
        self.assertEqual(rejected["status"], "REJECTED_EVIDENCE_BUDGET")
        self.assertTrue(store.budget_exhausted)
        oversized = pdo.SnapshotStore(max_count=4, max_total_bytes=20)
        self.assertEqual(oversized.add(b"x" * 32, {"pid": 1}, {})["status"],
                         "REJECTED_EVIDENCE_BUDGET")

    def test_observe_target_stores_snapshot(self):
        maps = SyntheticMaps()
        store = pdo.SnapshotStore()
        io = FakeProcIO({4242: {"identity": synthetic_identity(),
                                "exe": synthetic_exe(), "maps": maps.bytes}})
        result = pdo.observe_target(4242,
                                    expected_pid_identity={"pid": 4242,
                                                           "start_ticks": 999999,
                                                           "uid": os.getuid(),
                                                           "ppid": 100, "pgid": 100,
                                                           "sid": 100},
                                    expected_elf=synthetic_exe(),
                                    expected_dsos=maps.expected, io=io,
                                    snapshot_store=store)
        self.assertEqual(result["snapshot"]["status"], "STORED")
        self.assertEqual(store.total_bytes, len(maps.bytes))
        self.assertEqual(store.records[0]["target_identity"]["exe_sha256"],
                         synthetic_exe()["sha256"])


class _OwnedFdSeams:
    """Deterministic OS-call seams for exactly one verify_file_object fixture.

    For the duration of one helper call this wraps the module-level ``open``
    (the original baseline helper's read path), ``os.open``/``os.close`` (the
    fixed-fd helper path) and ``os.read``/``os.fstat``.  The pathname swap
    fires at most once and only for this fixture's own target path; the
    read/fstat seams fire only for fds obtained from that path.  Every other
    path, fd and call is passed through to the saved original function, and
    everything is restored by ``restore()``.  Fd closure is proven with the
    real ``os.fstat`` raising EBADF after the helper returned and before any
    other open could reuse a number; a close spy alone is never trusted and
    /proc/self/fd is never enumerated.
    """

    def __init__(self, target):
        self.target = os.path.abspath(target)
        self.real_open = open
        self.real_os_open = os.open
        self.real_os_close = os.close
        self.real_os_read = os.read
        self.real_os_fstat = os.fstat
        self.swap_action = None      # callable(target_path), fires at most once
        self.swap_triggered = False
        self.read_hook = None        # callable(fd, block) after a real read
        self.read_override = None    # callable(fd, count) replacing the read
        self.fstat_hook = None       # callable(fd) before a real fstat
        self.owned_fds = []
        self.helper_closed_fds = set()
        self.harness_closed_fds = []
        self.installed = False

    def install(self):
        if "open" in vars(pdo):
            raise AssertionError("proc_dso_observation unexpectedly defines open")
        self.installed = True
        pdo.open = self._wrapped_module_open
        os.open = self._wrapped_os_open
        os.close = self._wrapped_os_close
        os.read = self._wrapped_os_read
        os.fstat = self._wrapped_os_fstat

    def restore(self):
        if not self.installed:
            return
        del pdo.open
        os.open = self.real_os_open
        os.close = self.real_os_close
        os.read = self.real_os_read
        os.fstat = self.real_os_fstat
        self.installed = False

    def _maybe_swap(self, path):
        if self.swap_action is None or self.swap_triggered:
            return
        try:
            candidate = os.fspath(path)
        except TypeError:
            return
        if os.path.abspath(candidate) != self.target:
            return
        self.swap_triggered = True
        self.swap_action(self.target)

    def _wrapped_module_open(self, file, mode="r", *args, **kwargs):
        self._maybe_swap(file)
        stream = self.real_open(file, mode, *args, **kwargs)
        try:
            self.owned_fds.append(stream.fileno())
        except (OSError, ValueError):
            pass
        return stream

    def _wrapped_os_open(self, path, flags, *args, **kwargs):
        self._maybe_swap(path)
        fd = self.real_os_open(path, flags, *args, **kwargs)
        try:
            if os.path.abspath(os.fspath(path)) == self.target:
                self.owned_fds.append(fd)
        except (TypeError, ValueError):
            pass
        return fd

    def _wrapped_os_close(self, fd, *args, **kwargs):
        result = self.real_os_close(fd, *args, **kwargs)
        self.helper_closed_fds.add(fd)
        return result

    def _wrapped_os_read(self, fd, count):
        if fd in self.owned_fds and self.read_override is not None:
            return self.read_override(fd, count)
        block = self.real_os_read(fd, count)
        if fd in self.owned_fds and self.read_hook is not None:
            self.read_hook(fd, block)
        return block

    def _wrapped_os_fstat(self, fd):
        if fd in self.owned_fds and self.fstat_hook is not None:
            self.fstat_hook(fd)
        return self.real_os_fstat(fd)

    def unclosed_fds(self):
        """Fds this fixture observed that are still open once the helper has
        returned (the real ``os.fstat`` did not raise EBADF)."""
        leftovers = []
        for fd in self.owned_fds:
            try:
                self.real_os_fstat(fd)
            except OSError as error:
                if error.errno != errno.EBADF:
                    raise
            else:
                leftovers.append(fd)
        return leftovers

    def harness_close(self, fds):
        """Test-side fallback so that even a failing assertion never leaks
        the helper's fd; recorded separately from helper-managed closure."""
        for fd in fds:
            try:
                self.real_os_close(fd)
                self.harness_closed_fds.append(fd)
            except OSError:
                pass


class FileObjectTests(unittest.TestCase):
    """F-03: verify_file_object must bind stat facts, the complete EOF
    content, the actual byte count and the hash to one fixed opened file
    object, and must reject replacement, in-place change, non-regular types
    and over-budget reads with stable reason codes.  Every fixture is a
    regular file this class creates inside its own task-private temporary
    directory; replacements are same-bytes different-inode files from the
    same test.  No product DSO is loaded, no archived probe is executed and
    no other task's files are touched."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="proc-f03-")
        self.addCleanup(self._tmp.cleanup)
        self.dir = self._tmp.name

    def _write(self, name, payload):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as handle:
            handle.write(payload)
        return path

    def _expected_for(self, path, payload):
        info = os.lstat(path)
        return {"device": [os.major(info.st_dev), os.minor(info.st_dev)],
                "inode": info.st_ino, "bytes": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest()}

    def _run_with_seams(self, seams, call):
        """Install the seams, run ``call``, restore, then verify every fd
        observed for this fixture is closed (real EBADF) before anything
        else could open; leftovers are harness-closed so no assertion
        failure or helper exception leaks an fd."""
        try:
            seams.install()
            try:
                outcome = call()
            finally:
                seams.restore()
        finally:
            leftovers = seams.unclosed_fds()
            seams.harness_close(leftovers)
        return outcome, leftovers

    def test_swap_before_open_rejected(self):
        """F-03 core counterexample: the pathname is atomically replaced by
        a same-bytes different-inode file between the identity precheck and
        the first real open.  The original helper hashed the replacement
        while labelling the result with the old stat and reported
        matches=true; the fixed helper compares the fixed fd's identity with
        the precheck and rejects the mixed object."""
        payload = b"swap-before-open fixture payload"
        target = self._write("target.bin", payload)
        replacement = self._write("replacement.bin", payload)
        expected = self._expected_for(target, payload)
        self.assertNotEqual(os.lstat(target).st_ino,
                            os.lstat(replacement).st_ino)
        seams = _OwnedFdSeams(target)
        seams.swap_action = lambda path: os.replace(replacement, path)
        result, leftovers = self._run_with_seams(
            seams, lambda: pdo.verify_file_object(target, expected))
        self.assertEqual(leftovers, [])
        self.assertTrue(seams.swap_triggered)
        self.assertFalse(result["matches"])
        self.assertEqual(result["reason"], "OBJECT_REPLACED_BEFORE_OPEN")
        self.assertEqual(result["stage"], "open_identity")
        self.assertFalse(result["observation_complete"])
        self.assertIsNone(result["actual"])
        self.assertFalse(result["eof_observed"])
        self.assertEqual(result["read_bytes"], 0)

    def test_path_swapped_during_read_rejected(self):
        """F-03: the pathname is replaced after one data block was read from
        the still-open fd; the fixed fd is drained to its real EOF, but the
        final pathname identity check must reject the object mix."""
        payload = bytes(range(256)) * 8
        target = self._write("target.bin", payload)
        other = self._write("other.bin", payload)
        expected = self._expected_for(target, payload)
        seams = _OwnedFdSeams(target)
        state = {"swapped": False}

        def on_read(fd, block):
            if block and not state["swapped"]:
                state["swapped"] = True
                os.replace(other, target)

        seams.read_hook = on_read
        result, leftovers = self._run_with_seams(
            seams, lambda: pdo.verify_file_object(target, expected))
        self.assertEqual(leftovers, [])
        self.assertTrue(state["swapped"])
        self.assertFalse(result["matches"])
        self.assertEqual(result["reason"], "PATH_REPLACED_AFTER_READ")
        self.assertEqual(result["stage"], "final_lstat")
        self.assertTrue(result["eof_observed"])
        self.assertEqual(result["read_bytes"], len(payload))
        self.assertFalse(result["observation_complete"])
        self.assertIsNone(result["actual"])

    def test_path_swapped_at_eof_boundary_rejected(self):
        """F-03: replacement exactly at the EOF read seam: the fixed fd
        proved EOF for its own object, but the pathname now names a
        different one."""
        payload = b"eof-boundary fixture payload"
        target = self._write("target.bin", payload)
        other = self._write("other.bin", payload)
        expected = self._expected_for(target, payload)
        seams = _OwnedFdSeams(target)
        state = {"swapped": False}

        def on_read(fd, block):
            if not block and not state["swapped"]:
                state["swapped"] = True
                os.replace(other, target)

        seams.read_hook = on_read
        result, leftovers = self._run_with_seams(
            seams, lambda: pdo.verify_file_object(target, expected))
        self.assertEqual(leftovers, [])
        self.assertTrue(state["swapped"])
        self.assertFalse(result["matches"])
        self.assertEqual(result["reason"], "PATH_REPLACED_AFTER_READ")
        self.assertEqual(result["stage"], "final_lstat")
        self.assertTrue(result["eof_observed"])
        self.assertEqual(result["read_bytes"], len(payload))

    def test_in_place_change_during_read_rejected(self):
        """F-03: same inode and size, but the timestamps change while the fd
        is being read; the post-EOF fd recheck must reject before any
        comparison is made."""
        payload = b"in-place change fixture block\n" * 64
        target = self._write("target.bin", payload)
        expected = self._expected_for(target, payload)
        seams = _OwnedFdSeams(target)
        state = {"touched": False}

        def on_read(fd, block):
            if block and not state["touched"]:
                state["touched"] = True
                stamp = time.time_ns() + 1_000_000_000
                os.utime(target, ns=(stamp, stamp))

        seams.read_hook = on_read
        result, leftovers = self._run_with_seams(
            seams, lambda: pdo.verify_file_object(target, expected))
        self.assertEqual(leftovers, [])
        self.assertTrue(state["touched"])
        self.assertFalse(result["matches"])
        self.assertEqual(result["reason"], "OBJECT_CHANGED_DURING_READ")
        self.assertEqual(result["stage"], "post_read_fstat")
        self.assertTrue(result["eof_observed"])
        self.assertEqual(result["read_bytes"], len(payload))
        self.assertIsNone(result["actual"])

    def test_growth_beyond_limit_during_read_rejected(self):
        """F-03: the file grows past max_bytes while being read; the loop
        must stop at exactly the (limit + 1)-th observed byte and reject with
        the read-stage reason, never a preflight stat guess and never a
        hash."""
        payload = b"g" * (1024 * 1024)
        target = self._write("target.bin", payload)
        expected = self._expected_for(target, payload)
        seams = _OwnedFdSeams(target)
        state = {"grown": False}

        def on_read(fd, block):
            if block and not state["grown"]:
                state["grown"] = True
                with open(target, "ab") as handle:
                    handle.write(b"+")

        seams.read_hook = on_read
        result, leftovers = self._run_with_seams(
            seams, lambda: pdo.verify_file_object(target, expected,
                                                  max_bytes=1024 * 1024))
        self.assertEqual(leftovers, [])
        self.assertTrue(state["grown"])
        self.assertFalse(result["matches"])
        self.assertEqual(result["reason"], "EXCEEDS_HASH_BUDGET")
        self.assertEqual(result["stage"], "read_limit_plus_one")
        self.assertEqual(result["read_bytes"], 1024 * 1024 + 1)
        self.assertFalse(result["eof_observed"])
        self.assertIsNone(result["actual"])

    def test_preflight_size_over_budget_rejected_without_open(self):
        """F-03: a static file already over max_bytes is rejected by the
        preflight stat size (preflight_size), with no descriptor taken at
        all."""
        payload = b"z" * 64
        target = self._write("big.bin", payload)
        expected = self._expected_for(target, payload)
        seams = _OwnedFdSeams(target)
        result, leftovers = self._run_with_seams(
            seams, lambda: pdo.verify_file_object(target, expected, max_bytes=63))
        self.assertEqual(leftovers, [])
        self.assertEqual(seams.owned_fds, [])
        self.assertFalse(result["matches"])
        self.assertEqual(result["reason"], "EXCEEDS_HASH_BUDGET")
        self.assertEqual(result["stage"], "preflight_size")
        self.assertEqual(result["read_bytes"], 0)
        self.assertFalse(result["eof_observed"])

    def test_stable_regular_file_matches(self):
        """F-03 positive case: one stable regular file observed through one
        fixed fd matches its frozen expectation, with the fd closed by the
        helper itself."""
        payload = b"stable fixture content line\n" * 24
        target = self._write("stable.bin", payload)
        expected = self._expected_for(target, payload)
        seams = _OwnedFdSeams(target)
        result, leftovers = self._run_with_seams(
            seams, lambda: pdo.verify_file_object(target, expected))
        self.assertEqual(leftovers, [])
        self.assertEqual(len(seams.owned_fds), 1)
        self.assertEqual(seams.helper_closed_fds, set(seams.owned_fds))
        self.assertEqual(seams.harness_closed_fds, [])
        self.assertTrue(result["matches"])
        self.assertTrue(result["observation_complete"])
        self.assertTrue(result["eof_observed"])
        self.assertIsNone(result["reason"])
        self.assertIsNone(result["stage"])
        self.assertEqual(result["read_bytes"], len(payload))
        self.assertEqual(result["actual"]["bytes"], len(payload))
        self.assertEqual(result["actual"]["read_bytes"], len(payload))
        self.assertEqual(result["actual"]["inode"], expected["inode"])
        self.assertEqual(result["actual"]["device"], expected["device"])
        self.assertEqual(result["actual"]["sha256"], expected["sha256"])

    def test_exact_max_bytes_boundary_with_real_eof(self):
        """F-03: a file of exactly max_bytes bytes must still obtain a real
        zero-byte EOF before it may match."""
        payload = b"e" * 32
        target = self._write("exact.bin", payload)
        expected = self._expected_for(target, payload)
        seams = _OwnedFdSeams(target)
        result, leftovers = self._run_with_seams(
            seams, lambda: pdo.verify_file_object(target, expected, max_bytes=32))
        self.assertEqual(leftovers, [])
        self.assertTrue(result["matches"])
        self.assertTrue(result["observation_complete"])
        self.assertTrue(result["eof_observed"])
        self.assertEqual(result["read_bytes"], 32)
        self.assertEqual(result["actual"]["bytes"], 32)

    def test_wrong_expected_identity_or_hash_not_matched(self):
        """F-03: a complete observation against a wrong frozen expectation
        stays matches=false with the true actual facts, not an error."""
        payload = b"mismatch fixture"
        target = self._write("mismatch.bin", payload)
        info = os.lstat(target)
        wrong_inode = {"device": [os.major(info.st_dev), os.minor(info.st_dev)],
                       "inode": info.st_ino + 1, "bytes": len(payload),
                       "sha256": hashlib.sha256(payload).hexdigest()}
        result = pdo.verify_file_object(target, wrong_inode)
        self.assertFalse(result["matches"])
        self.assertTrue(result["observation_complete"])
        self.assertIsNone(result["reason"])
        self.assertEqual(result["actual"]["inode"], info.st_ino)
        wrong_hash = {"device": [os.major(info.st_dev), os.minor(info.st_dev)],
                      "inode": info.st_ino, "bytes": len(payload),
                      "sha256": "f" * 64}
        result = pdo.verify_file_object(target, wrong_hash)
        self.assertFalse(result["matches"])
        self.assertTrue(result["observation_complete"])
        self.assertEqual(result["actual"]["sha256"],
                         hashlib.sha256(payload).hexdigest())

    def test_rejects_fifo_directory_and_symlink_without_opening(self):
        """F-03: the lstat type precheck rejects a FIFO (without the
        blocking open the original helper would have done), a directory and
        a symlink (never followed), before any descriptor is taken."""
        fifo = os.path.join(self.dir, "fifo")
        os.mkfifo(fifo)
        real = self._write("real.bin", b"behind the link")
        link = os.path.join(self.dir, "link")
        os.symlink(real, link)
        directory = os.path.join(self.dir, "subdir")
        os.mkdir(directory)
        seams = _OwnedFdSeams(fifo)

        def call():
            return [pdo.verify_file_object(path, {}) for path in
                    (fifo, directory, link)]

        results, leftovers = self._run_with_seams(seams, call)
        self.assertEqual(leftovers, [])
        self.assertEqual(seams.owned_fds, [])
        for result in results:
            self.assertFalse(result["matches"])
            self.assertEqual(result["reason"], "NOT_A_REGULAR_FILE")
            self.assertEqual(result["stage"], "preflight_lstat_type")
            self.assertEqual(result["read_bytes"], 0)
            self.assertFalse(result["eof_observed"])

    def test_symlink_swapped_in_before_open_is_not_followed(self):
        """F-03: a symlink moved onto the pathname between the precheck and
        the open must make the open itself fail (O_NOFOLLOW) instead of
        silently reading through the link."""
        payload = b"never read through a swapped link"
        target = self._write("target.bin", payload)
        elsewhere = self._write("elsewhere.bin", payload)
        expected = self._expected_for(target, payload)
        link = os.path.join(self.dir, "swap-link")
        os.symlink(elsewhere, link)
        seams = _OwnedFdSeams(target)
        seams.swap_action = lambda path: os.replace(link, path)

        def call():
            with self.assertRaises(OSError):
                pdo.verify_file_object(target, expected)

        _, leftovers = self._run_with_seams(seams, call)
        self.assertEqual(leftovers, [])
        self.assertTrue(seams.swap_triggered)
        self.assertEqual(seams.owned_fds, [])

    def test_eintr_storm_is_bounded_structured_rejection(self):
        """F-03: an endless EINTR storm is retried at most
        DEFAULT_MAX_READ_ATTEMPTS times and then rejected without success;
        the fd is closed."""
        payload = b"payload"
        target = self._write("eintr.bin", payload)
        expected = self._expected_for(target, payload)
        seams = _OwnedFdSeams(target)

        def override(fd, count):
            raise InterruptedError()

        seams.read_override = override
        result, leftovers = self._run_with_seams(
            seams, lambda: pdo.verify_file_object(target, expected))
        self.assertEqual(leftovers, [])
        self.assertEqual(seams.helper_closed_fds, set(seams.owned_fds))
        self.assertFalse(result["matches"])
        self.assertEqual(result["reason"], "EINTR_BUDGET_EXHAUSTED")
        self.assertFalse(result["observation_complete"])
        self.assertFalse(result["eof_observed"])
        self.assertIsNone(result["actual"])

    def test_read_oserror_propagates_and_fd_is_closed(self):
        payload = b"payload"
        target = self._write("eio.bin", payload)
        expected = self._expected_for(target, payload)
        seams = _OwnedFdSeams(target)

        def override(fd, count):
            raise OSError(errno.EIO, "Input/output error")

        seams.read_override = override

        def call():
            with self.assertRaises(OSError):
                pdo.verify_file_object(target, expected)

        _, leftovers = self._run_with_seams(seams, call)
        self.assertEqual(leftovers, [])
        self.assertEqual(seams.helper_closed_fds, set(seams.owned_fds))

    def test_fstat_oserror_propagates_and_fd_is_closed(self):
        payload = b"payload"
        target = self._write("fstat.bin", payload)
        expected = self._expected_for(target, payload)
        seams = _OwnedFdSeams(target)

        def hook(fd):
            raise OSError(errno.EIO, "Input/output error")

        seams.fstat_hook = hook

        def call():
            with self.assertRaises(OSError):
                pdo.verify_file_object(target, expected)

        _, leftovers = self._run_with_seams(seams, call)
        self.assertEqual(leftovers, [])
        self.assertEqual(seams.helper_closed_fds, set(seams.owned_fds))

    def test_max_bytes_must_be_positive_integer(self):
        target = self._write("budget.bin", b"x")
        for bad in (0, -1, 1.5, True):
            with self.assertRaises(ValueError):
                pdo.verify_file_object(target, {}, max_bytes=bad)


class RealProcFixtureTests(unittest.TestCase):
    def test_real_interpreter_fixture_eof_and_exe_binding(self):
        """Group 4: a stock interpreter process waits on a controlled pipe.
        The collector reads its real /proc maps to a proven EOF, binds
        /proc/PID/exe to the interpreter image, and checks that one limited
        static mapping (its own executable image) is present with the exact
        file-object identity.  This is a tool-integration check of the
        mechanism only; it is not a claim about any product ELF or any
        nine-library set.  Budget: this whole case must finish within five
        seconds, and only this fixture process is reaped."""
        if not os.path.isdir("/proc") or not os.access("/proc/self/maps", os.R_OK):
            self.skipTest("TOOL_INTEGRATION_BLOCKED: /proc unavailable on this platform")
        if os.name != "posix":
            self.skipTest("TOOL_INTEGRATION_BLOCKED: posix-only fixture")
        read_fd, write_fd = os.pipe()
        child = subprocess.Popen(
            [sys.executable, "-c", "import sys; sys.stdin.buffer.read(1)"],
            stdin=read_fd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        started = time.monotonic()
        try:
            io = pdo.RealProcIO()
            identity = None
            while identity is None and time.monotonic() - started < 2.0:
                identity = io.pid_identity(child.pid)
                if identity is None:
                    time.sleep(0.01)  # waiting for our own child to appear
            if identity is None:
                self.skipTest("TOOL_INTEGRATION_BLOCKED: fixture identity unavailable")
            info = os.stat(sys.executable)
            with open(sys.executable, "rb") as stream:
                image_sha = hashlib.sha256(stream.read()).hexdigest()
            expected_elf = {"device": [os.major(info.st_dev), os.minor(info.st_dev)],
                            "inode": info.st_ino, "bytes": info.st_size,
                            "sha256": image_sha}
            expected_identity = {"pid": child.pid,
                                 "start_ticks": identity["start_ticks"],
                                 "uid": os.getuid(), "ppid": os.getpid(),
                                 "pgid": os.getpgid(child.pid),
                                 "sid": os.getsid(child.pid)}
            expected_dsos = [{"name": "<interpreter-image>",
                              "device": expected_elf["device"],
                              "inode": expected_elf["inode"]}]
            try:
                result = pdo.observe_target(child.pid,
                                            expected_pid_identity=expected_identity,
                                            expected_elf=expected_elf,
                                            expected_dsos=expected_dsos, io=io)
            except PermissionError as error:
                self.skipTest(f"TOOL_INTEGRATION_BLOCKED: {error}")
            self.assertTrue(result["maps_eof_complete"],
                            f"maps read did not reach EOF: {result['maps_read']}")
            self.assertTrue(result["target_identity_verified"])
            self.assertEqual(result["exe_identity_before"]["sha256"], image_sha)
            self.assertTrue(result["target_direct_dso_complete"],
                            f"missing: {result['missing_dsos']}")
            self.assertTrue(result["evidence_complete"])
            self.assertLess(time.monotonic() - started, 5.0)
        finally:
            try:
                os.write(write_fd, b"\n")
            except OSError:
                pass
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.terminate()
                child.wait(timeout=5)
            os.close(read_fd)
            os.close(write_fd)


if __name__ == "__main__":
    unittest.main(verbosity=2)
