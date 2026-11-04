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
and target-exe binding against that process's own limited static identity.  It
does not call any WLS code, create a business Store or run product tests, and
it reaps only that one fixture process.  If the platform denies the needed
/proc access, the case is reported as TOOL_INTEGRATION_BLOCKED and skipped,
never silently passed.
"""

from __future__ import annotations

import errno
import hashlib
import os
import subprocess
import sys
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
    def __init__(self, values):
        self.values = list(values)

    def __call__(self):
        return self.values.pop(0) if self.values else self.values[-1:][0]


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
