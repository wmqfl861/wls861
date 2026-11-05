#!/usr/bin/env python3
"""Bounded /proc text capture with real EOF proof, exact target process/ELF
identity binding, and direct-DSO mapping verification.

Purpose and evidence limits
---------------------------
This module is diagnostic tooling only.  It is NOT registered with CTest, Make
or any CI gate, its results are NOT product sanitizer results, and it never
reclassifies a native or CTest exit or any product gate.

* ``read_status == "COMPLETE"`` only proves that this one /proc text stream
  ended with a real zero-byte read inside the byte / read-attempt /
  monotonic-time budgets, where every read's budget is judged from one
  monotonic sample taken after that read returns: a zero-byte EOF landing
  past the time budget stays INCOMPLETE/TIME_BUDGET_EXHAUSTED with
  ``eof_observed`` recording the EOF fact, never COMPLETE.  It is not an
  atomic global memory-mapping snapshot and says nothing about unsampled
  intervals or transient dlopen/unload.
* ``"FAIL"`` (``SIZE_LIMIT_EXCEEDED``) is proven the moment the
  (limit + 1)-th byte is observed; no oversized raw snapshot is retained for
  it and no later read may relabel it COMPLETE.  For one and the same
  returned read, SIZE_LIMIT_EXCEEDED outranks TIME_BUDGET_EXHAUSTED, which
  outranks an EOF COMPLETE; the synchronous loop cannot hard-interrupt a
  read callback, so no real-time interruption is promised.
* ``"INCOMPLETE"`` keeps a concrete reason; every failure or incomplete branch
  blocks ``evidence_complete``.
* Target identity and DSO verification compare only one explicitly expected,
  caller-supplied process identity, ELF file object and direct-DSO file-object
  list.  Descendant or parent mappings never fill a target's direct set.
  Mappings marked ``(deleted)`` are never accepted as evidence for a current
  file object.
* ``verify_file_object`` binds stat facts, the full EOF-terminated content,
  the actual byte count and the hash to one fixed read-only fd opened after
  an lstat type precheck, re-checks that fd after EOF and the pathname after
  the read, and only then compares the frozen expectation.  Its success
  domain is regular non-symlink files; the pre-read lstat, post-EOF fstat
  and final-path lstat are three separate samples, not an atomic filesystem
  snapshot (the pathname can be replaced after the final sample).  This
  fixed-fd policy does not apply to the ``/proc/PID/exe`` magic link read
  by ``RealProcIO.exe_identity``.

All frozen identities must be passed explicitly by the caller.  This module
never guesses inputs from cwd, ambient configuration or implicit search paths
and contains no machine-specific or product-specific constants.

Direct self-check (deterministic tool mechanism checks only; not a product
regression; synthetic fixtures only):

    python3 -B tests/test_proc_dso_observation.py -v
"""

from __future__ import annotations

import errno
import hashlib
import os
import re
import stat
import time

__all__ = [
    "DEFAULT_MAX_BYTES", "DEFAULT_MAX_READ_ATTEMPTS", "DEFAULT_CAPTURE_SECONDS",
    "DEFAULT_READ_CHUNK", "DEFAULT_MAX_EXE_BYTES", "DEFAULT_STAT_MAX_BYTES",
    "READ_COMPLETE", "READ_INCOMPLETE", "READ_FAIL",
    "SNAPSHOT_MAX_COUNT", "SNAPSHOT_MAX_TOTAL_BYTES",
    "ReadResult", "read_proc_stream", "ReadError",
    "ProcIO", "RealProcIO", "parse_maps", "MapEntry",
    "verify_direct_dso_entries", "verify_file_object",
    "pid_identity_matches", "exe_identity_matches",
    "SnapshotStore", "discover_target_child", "observe_target",
    "proc_path",
]

# Per-capture budgets (see plan contract: 2 MiB, 1024 attempts, 0.25 s).
DEFAULT_MAX_BYTES = 2 * 1024 * 1024
DEFAULT_MAX_READ_ATTEMPTS = 1024
DEFAULT_CAPTURE_SECONDS = 0.25
DEFAULT_READ_CHUNK = 65536
DEFAULT_MAX_EXE_BYTES = 1024 * 1024 * 1024
DEFAULT_STAT_MAX_BYTES = 8192

READ_COMPLETE = "COMPLETE"
READ_INCOMPLETE = "INCOMPLETE"
READ_FAIL = "FAIL"

REASON_EOF = "EOF"
REASON_SIZE_LIMIT_EXCEEDED = "SIZE_LIMIT_EXCEEDED"
REASON_READ_ATTEMPTS_EXHAUSTED = "READ_ATTEMPTS_EXHAUSTED"
REASON_EINTR_BUDGET_EXHAUSTED = "EINTR_BUDGET_EXHAUSTED"
REASON_TIME_BUDGET_EXHAUSTED = "TIME_BUDGET_EXHAUSTED"
REASON_EAGAIN = "EAGAIN"
REASON_PERMISSION_DENIED = "PERMISSION_DENIED"
REASON_PROCESS_DISAPPEARED = "PROCESS_DISAPPEARED"
REASON_OS_ERROR = "OS_ERROR"

# Snapshot evidence budget: at most 4 deduplicated raw maps snapshots of the
# target process, each at most 2 MiB (8 MiB combined).
SNAPSHOT_MAX_COUNT = 4
SNAPSHOT_MAX_TOTAL_BYTES = 8 * 1024 * 1024

# Only these /proc names may be opened by this module.
PROC_TEXT_NAMES = frozenset({"maps", "stat", "status", "children"})
PROC_EXE_NAME = "exe"


class ReadError(Exception):
    """A /proc access failed in a way the caller must see."""


def _require(condition, message):
    if not condition:
        raise ValueError(message)


class ReadResult:
    """Outcome of one bounded /proc text stream capture."""

    def __init__(self, read_status, reason, eof_observed, data, total_bytes,
                 read_attempts, read_lengths, elapsed_seconds):
        self.read_status = read_status
        self.reason = reason
        self.eof_observed = eof_observed
        self.data = data
        self.total_bytes = total_bytes
        self.read_attempts = read_attempts
        self.read_lengths = read_lengths
        self.elapsed_seconds = elapsed_seconds

    def as_dict(self):
        return {
            "read_status": self.read_status,
            "reason": self.reason,
            "eof_observed": self.eof_observed,
            "data_bytes": None if self.data is None else len(self.data),
            "data_sha256": None if self.data is None else hashlib.sha256(self.data).hexdigest(),
            "total_bytes": self.total_bytes,
            "read_attempts": self.read_attempts,
            "read_lengths": list(self.read_lengths),
            "elapsed_seconds": self.elapsed_seconds,
        }


def read_proc_stream(read, *, max_bytes=DEFAULT_MAX_BYTES,
                     max_read_attempts=DEFAULT_MAX_READ_ATTEMPTS,
                     capture_seconds=DEFAULT_CAPTURE_SECONDS,
                     clock=time.monotonic, chunk=DEFAULT_READ_CHUNK):
    """Loop-read one already-open file object until EOF is proven or a budget
    boundary is hit.

    ``read(n)`` must behave like ``os.read`` on the caller's descriptor.  EOF
    is only a real zero-byte return.  Reading the (max_bytes + 1)-th byte
    proves SIZE_LIMIT_EXCEEDED (FAIL, no data retained); exhausting the time
    or read-attempt budget, EINTR storms, EAGAIN, permission errors and
    process disappearance are INCOMPLETE with a concrete reason.

    Every read's budget is judged from one monotonic sample taken after the
    read returns (the same sample also becomes that step's reported
    elapsed): a zero-byte EOF landing past ``capture_seconds`` stays
    INCOMPLETE/TIME_BUDGET_EXHAUSTED with ``eof_observed`` recording the EOF
    fact, and a non-empty block landing past the budget keeps its in-budget
    data and real counts.  For one and the same returned read,
    SIZE_LIMIT_EXCEEDED outranks TIME_BUDGET_EXHAUSTED, which outranks
    COMPLETE; an elapsed exactly equal to the budget keeps completing (the
    strict ``>`` boundary is unchanged).  This synchronous loop cannot
    hard-interrupt a running read callback, so no real-time interruption is
    promised.
    """
    _require(callable(read), "read must be callable")
    _require(callable(clock), "clock must be callable")
    _require(max_bytes >= 1, "max_bytes must be positive")
    _require(max_read_attempts >= 1, "max_read_attempts must be positive")
    _require(capture_seconds > 0, "capture_seconds must be positive")
    _require(chunk >= 1, "chunk must be positive")

    started = clock()
    attempts = 0
    blocks = []
    lengths = []
    total = 0
    last_was_eintr = False

    def finish(status, reason, eof, data, elapsed_seconds):
        return ReadResult(status, reason, eof, data, total, attempts, lengths,
                          elapsed_seconds)

    while True:
        elapsed = clock() - started
        if elapsed > capture_seconds:
            return finish(READ_INCOMPLETE, REASON_TIME_BUDGET_EXHAUSTED, False,
                          b"".join(blocks), elapsed)
        if attempts >= max_read_attempts:
            reason = (REASON_EINTR_BUDGET_EXHAUSTED if last_was_eintr and not lengths
                      else REASON_READ_ATTEMPTS_EXHAUSTED)
            return finish(READ_INCOMPLETE, reason, False, b"".join(blocks), elapsed)
        want = max_bytes - total + 1
        if want > chunk:
            want = chunk
        attempts += 1
        try:
            block = read(want)
        except InterruptedError:
            last_was_eintr = True
            continue
        except BlockingIOError:
            return finish(READ_INCOMPLETE, REASON_EAGAIN, False,
                          b"".join(blocks), clock() - started)
        except PermissionError:
            return finish(READ_INCOMPLETE, REASON_PERMISSION_DENIED, False,
                          b"".join(blocks), clock() - started)
        except (ProcessLookupError, FileNotFoundError):
            return finish(READ_INCOMPLETE, REASON_PROCESS_DISAPPEARED, False,
                          b"".join(blocks), clock() - started)
        except OSError as error:
            if error.errno == errno.EAGAIN:
                return finish(READ_INCOMPLETE, REASON_EAGAIN, False,
                              b"".join(blocks), clock() - started)
            if error.errno == errno.EACCES or error.errno == errno.EPERM:
                return finish(READ_INCOMPLETE, REASON_PERMISSION_DENIED, False,
                              b"".join(blocks), clock() - started)
            if error.errno == errno.ESRCH or error.errno == errno.ENOENT:
                return finish(READ_INCOMPLETE, REASON_PROCESS_DISAPPEARED, False,
                              b"".join(blocks), clock() - started)
            raise ReadError(f"proc read os error {error.errno}") from error
        last_was_eintr = False
        # One monotonic sample after every successful read drives both that
        # read's boundary decision and its reported elapsed.
        elapsed = clock() - started
        if not block:
            if elapsed > capture_seconds:
                # The zero-byte read genuinely proved EOF, but only after the
                # budget: keep the EOF fact, never COMPLETE.
                return finish(READ_INCOMPLETE, REASON_TIME_BUDGET_EXHAUSTED,
                              True, b"".join(blocks), elapsed)
            return finish(READ_COMPLETE, REASON_EOF, True, b"".join(blocks),
                          elapsed)
        lengths.append(len(block))
        total += len(block)
        if total > max_bytes:
            # Proven over budget: keep no oversized raw snapshot; this
            # outranks a time overrun from the same read.
            return finish(READ_FAIL, REASON_SIZE_LIMIT_EXCEEDED, False, None,
                          elapsed)
        blocks.append(block)
        if elapsed > capture_seconds:
            return finish(READ_INCOMPLETE, REASON_TIME_BUDGET_EXHAUSTED, False,
                          b"".join(blocks), elapsed)


def proc_path(pid, name):
    _require(isinstance(pid, int) and pid > 0, "pid must be a positive int")
    _require(name in PROC_TEXT_NAMES or name == PROC_EXE_NAME,
             f"proc name not allowed: {name!r}")
    return f"/proc/{pid}/{name}"


class ProcIO:
    """Interface for all /proc access; injectable for deterministic tests."""

    def list_pids(self):
        raise NotImplementedError

    def pid_identity(self, pid):
        raise NotImplementedError

    def exe_identity(self, pid):
        raise NotImplementedError

    def read_text_stream(self, pid, name, **budgets):
        raise NotImplementedError


class RealProcIO(ProcIO):
    """Read-only /proc access with the original open flags.

    Text names are opened O_RDONLY | O_NONBLOCK | O_NOFOLLOW; the magic
    ``exe`` link is only opened O_RDONLY (following that one verified link is
    how the actual ELF file object is read and hashed).
    """

    def list_pids(self):
        return sorted(int(entry) for entry in os.listdir("/proc")
                      if entry.isdigit())

    def pid_identity(self, pid):
        result = self.read_text_stream(pid, "stat", max_bytes=DEFAULT_STAT_MAX_BYTES)
        if result.read_status != READ_COMPLETE:
            return None
        try:
            text = result.data.decode("utf-8", "surrogateescape")
            tail = text[text.rfind(")") + 2:].split()
            identity = {
                "pid": int(pid),
                "state": tail[0],
                "ppid": int(tail[1]),
                "pgid": int(tail[2]),
                "sid": int(tail[3]),
                "start_ticks": int(tail[19]),
            }
            identity["uid"] = os.stat(f"/proc/{int(pid)}").st_uid
            return identity
        except (ValueError, IndexError, OSError):
            return None

    def exe_identity(self, pid):
        fd = os.open(proc_path(pid, PROC_EXE_NAME), os.O_RDONLY | os.O_CLOEXEC)
        try:
            info = os.fstat(fd)
            digest = hashlib.sha256()
            total = 0
            interrupted = 0
            while True:
                try:
                    block = os.read(fd, 1024 * 1024)
                except InterruptedError:
                    interrupted += 1
                    if interrupted > DEFAULT_MAX_READ_ATTEMPTS:
                        return None
                    continue
                if not block:
                    break
                total += len(block)
                if total > DEFAULT_MAX_EXE_BYTES:
                    return None
                digest.update(block)
            return {
                "device": [os.major(info.st_dev), os.minor(info.st_dev)],
                "inode": info.st_ino,
                "bytes": info.st_size,
                "read_bytes": total,
                "sha256": digest.hexdigest(),
            }
        finally:
            os.close(fd)

    def read_text_stream(self, pid, name, **budgets):
        if name == PROC_EXE_NAME:
            raise ValueError("exe is not a text stream; use exe_identity")
        path = proc_path(pid, name)
        flags = os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
        fd = os.open(path, flags)
        try:
            return read_proc_stream(lambda count: os.read(fd, count), **budgets)
        finally:
            os.close(fd)


# Fields are structurally fixed; the kernel may pad between the inode and the
# pathname with extra alignment spaces, which is still unambiguous.
_MAP_LINE = re.compile(
    rb"^([0-9a-fA-F]+)-([0-9a-fA-F]+)[ ]+([rwxps-]{4})[ ]+([0-9a-fA-F]+)[ ]+"
    rb"([0-9a-f]+):([0-9a-f]+)[ ]+(\d+)(?:[ ]+(.*))?$")


class MapEntry:
    def __init__(self, line_index, start, end, perms, offset, device, inode,
                 path, deleted):
        self.line_index = line_index
        self.start = start
        self.end = end
        self.perms = perms
        self.offset = offset
        self.device = device  # [major, minor] as ints
        self.inode = inode
        self.path = path      # str or None (anonymous / special mapping)
        self.deleted = deleted

    def as_dict(self):
        return {"line_index": self.line_index, "start": self.start, "end": self.end,
                "perms": self.perms, "offset": self.offset,
                "device": list(self.device), "inode": self.inode,
                "path": self.path, "deleted": self.deleted}


def parse_maps(data):
    """Strictly parse complete maps bytes.  Unparseable lines are reported,
    never repaired or silently matched."""
    _require(isinstance(data, (bytes, bytearray)), "maps data must be bytes")
    data = bytes(data)
    entries = []
    errors = []
    if b"\x00" in data:
        return [], [{"line_index": None, "reason": "NUL_BYTE_IN_STREAM"}]
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    for index, line in enumerate(lines):
        match = _MAP_LINE.match(line)
        if match is None:
            errors.append({"line_index": index, "reason": "UNPARSEABLE_LINE"})
            continue
        start, end, perms, offset, major, minor, inode, path = match.groups()
        path_text = None
        deleted = False
        if path:  # empty/absent pathname means an anonymous mapping
            if not path.startswith(b"/"):
                # [stack]/[heap]/[vvar] style special mappings keep their
                # bracket name; anything else ambiguous is an error.
                if not (path.startswith(b"[") and path.endswith(b"]")):
                    errors.append({"line_index": index, "reason": "AMBIGUOUS_PATH"})
                    continue
            path_text = path.decode("utf-8", "surrogateescape")
            deleted = path_text.endswith(" (deleted)")
        entries.append(MapEntry(index, int(start, 16), int(end, 16),
                                 perms.decode("ascii"), int(offset, 16),
                                 [int(major, 16), int(minor, 16)], int(inode),
                                 path_text, deleted))
    return entries, errors


def verify_direct_dso_entries(entries, expected_dsos):
    """Check that every expected direct DSO has a non-deleted mapping of the
    exact expected file object (device + inode) in these target-only entries.

    Descendant/parent entries are not part of ``entries`` by contract; nothing
    outside this list can complete an expected DSO.
    """
    results = []
    for expected in expected_dsos:
        name = expected["name"]
        device = [int(expected["device"][0]), int(expected["device"][1])]
        inode = int(expected["inode"])
        matched = []
        deleted_only = []
        for entry in entries:
            if entry.inode != inode or entry.device != device:
                continue
            if entry.path is None:
                continue
            if entry.deleted:
                deleted_only.append(entry.path)
            else:
                matched.append(entry.path)
        results.append({"name": name, "observed": bool(matched),
                        "matched_paths": matched,
                        "deleted_only_paths": deleted_only})
    return {"dsos": results,
            "complete": bool(results) and all(item["observed"] for item in results),
            "missing": [item["name"] for item in results if not item["observed"]]}


_VFO_FROZEN_FIELDS = ("device", "inode", "type", "size", "mtime_ns", "ctime_ns")


def _stat_facts(info):
    """File-object facts for identity comparison.  The frozen comparison
    covers device, inode, type, size, mtime_ns and ctime_ns; extra sampled
    facts (mode/uid/gid/nlink) ride along for diagnosis only."""
    return {
        "device": [os.major(info.st_dev), os.minor(info.st_dev)],
        "inode": info.st_ino,
        "type": stat.S_IFMT(info.st_mode),
        "size": info.st_size,
        "mtime_ns": info.st_mtime_ns,
        "ctime_ns": info.st_ctime_ns,
        "mode": info.st_mode,
        "uid": info.st_uid,
        "gid": info.st_gid,
        "nlink": info.st_nlink,
    }


def _same_frozen_object(left, right):
    return all(left[field] == right[field] for field in _VFO_FROZEN_FIELDS)


def verify_file_object(path, expected, *, max_bytes=DEFAULT_MAX_EXE_BYTES):
    """Verify one expected file object through a single fixed read-only fd.

    ``matches=True`` means: this one fixed-opened regular (non-symlink) file
    object's complete EOF-terminated content, actual byte count, device and
    inode equal the frozen expectation, and the pre-read pathname lstat, the
    post-EOF fd fstat and the final pathname lstat all showed the same
    frozen object.  The flow is: pathname lstat type precheck -> one
    read-only ``os.open`` with O_CLOEXEC/O_NONBLOCK/O_NOFOLLOW -> fstat type
    and identity alignment with the precheck -> chunked hashing on that one
    fd, each request at most ``min(1 MiB, remaining + 1)`` bytes and bounded
    by what is really read (rejected the moment the (max_bytes + 1)-th byte
    is actually observed, without ever buffering the whole file) -> a real
    zero-byte EOF -> fd fstat recheck -> final pathname lstat recheck ->
    comparison with ``expected`` over device/inode/bytes/sha256.

    The success domain is regular non-symlink files: directories, FIFOs,
    symlinks and post-open type changes are rejected without completing.
    Over-budget inputs keep the historical ``EXCEEDS_HASH_BUDGET`` reason,
    with ``stage`` separating the preflight stat-size rejection
    (``preflight_size``) from a really observed (limit + 1)-th byte
    (``read_limit_plus_one``); a file of exactly ``max_bytes`` bytes still
    needs a real zero-byte EOF to complete.  Incomplete observations emit no
    ``actual`` hash field that could be mistaken for a complete one.

    The precheck, the post-EOF fd stat and the final pathname lstat are
    three separate samples, not an atomic filesystem snapshot: the pathname
    can still be replaced after the final sample.  This fixed-fd policy is
    for ordinary caller-supplied paths only; the ``/proc/PID/exe`` magic
    link read by ``RealProcIO.exe_identity`` keeps its own contract.

    I/O errors from lstat/open/read/fstat propagate unchanged and never
    become ``matches=True``; InterruptedError from reads is retried at most
    ``DEFAULT_MAX_READ_ATTEMPTS`` times and then reported as a structured
    rejection.  The single fd is closed on every return and raise.
    """
    _require(isinstance(max_bytes, int) and not isinstance(max_bytes, bool),
             "max_bytes must be an integer")
    _require(max_bytes >= 1, "max_bytes must be positive")

    def rejection(reason, stage, read_bytes, eof_observed):
        return {"path": path, "matches": False, "actual": None, "reason": reason,
                "stage": stage, "observation_complete": False,
                "read_bytes": read_bytes, "eof_observed": eof_observed}

    pre_facts = _stat_facts(os.lstat(path))
    if pre_facts["type"] != stat.S_IFREG:
        return rejection("NOT_A_REGULAR_FILE", "preflight_lstat_type", 0, False)
    if pre_facts["size"] > max_bytes:
        return rejection("EXCEEDS_HASH_BUDGET", "preflight_size", 0, False)

    flags = (os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
             | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0))
    fd = os.open(path, flags)
    try:
        opened_facts = _stat_facts(os.fstat(fd))
        if opened_facts["type"] != stat.S_IFREG:
            return rejection("NOT_A_REGULAR_FILE", "open_fstat_type", 0, False)
        if not _same_frozen_object(pre_facts, opened_facts):
            return rejection("OBJECT_REPLACED_BEFORE_OPEN", "open_identity",
                             0, False)

        digest = hashlib.sha256()
        total = 0
        eof_observed = False
        interrupted = 0
        while True:
            want = min(1024 * 1024, max_bytes - total + 1)
            try:
                block = os.read(fd, want)
            except InterruptedError:
                interrupted += 1
                if interrupted > DEFAULT_MAX_READ_ATTEMPTS:
                    return rejection("EINTR_BUDGET_EXHAUSTED", "read_eintr",
                                     total, eof_observed)
                continue
            if not block:
                eof_observed = True
                break
            total += len(block)
            if total > max_bytes:
                return rejection("EXCEEDS_HASH_BUDGET", "read_limit_plus_one",
                                 total, False)
            digest.update(block)

        after_facts = _stat_facts(os.fstat(fd))
        if after_facts["type"] != stat.S_IFREG:
            return rejection("NOT_A_REGULAR_FILE", "post_read_fstat_type",
                             total, eof_observed)
        if not _same_frozen_object(opened_facts, after_facts):
            return rejection("OBJECT_CHANGED_DURING_READ", "post_read_fstat",
                             total, eof_observed)
        if total != after_facts["size"]:
            return rejection("SIZE_MISMATCH", "post_read_size", total,
                             eof_observed)

        final_facts = _stat_facts(os.lstat(path))
        if final_facts["type"] != stat.S_IFREG:
            return rejection("NOT_A_REGULAR_FILE", "final_lstat_type",
                             total, eof_observed)
        if not _same_frozen_object(after_facts, final_facts):
            return rejection("PATH_REPLACED_AFTER_READ", "final_lstat",
                             total, eof_observed)

        actual = {"device": opened_facts["device"],
                  "inode": opened_facts["inode"], "bytes": total,
                  "read_bytes": total, "sha256": digest.hexdigest()}
        keys = ("device", "inode", "bytes", "sha256")
        matches = all(actual[key] == expected.get(key) for key in keys)
        return {"path": path, "actual": actual, "matches": matches,
                "reason": None, "stage": None, "observation_complete": True,
                "read_bytes": total, "eof_observed": eof_observed}
    finally:
        os.close(fd)


def pid_identity_matches(expected, actual):
    if not isinstance(expected, dict) or not isinstance(actual, dict):
        return False
    keys = ("pid", "start_ticks", "uid", "ppid", "pgid", "sid")
    return all(expected.get(key) == actual.get(key) for key in keys)


def exe_identity_matches(expected, actual):
    if not isinstance(expected, dict) or not isinstance(actual, dict):
        return False
    keys = ("device", "inode", "bytes", "sha256")
    return all(expected.get(key) == actual.get(key) for key in keys)


class SnapshotStore:
    """Bounded, deduplicating raw-maps snapshot budget for one target."""

    def __init__(self, max_count=SNAPSHOT_MAX_COUNT,
                 max_total_bytes=SNAPSHOT_MAX_TOTAL_BYTES):
        self.max_count = max_count
        self.max_total_bytes = max_total_bytes
        self.records = []
        self.total_bytes = 0
        self.budget_exhausted = False

    def add(self, data, target_identity, meta):
        _require(isinstance(data, (bytes, bytearray)), "snapshot data must be bytes")
        data = bytes(data)
        sha = hashlib.sha256(data).hexdigest()
        for record in self.records:
            if record["sha256"] == sha:
                # Identical content: reference the original record, no new bytes.
                return {"status": "DEDUPLICATED", "snapshot_id": record["id"],
                        "sha256": sha, "bytes": len(data)}
        if self.budget_exhausted or len(self.records) >= self.max_count \
                or self.total_bytes + len(data) > self.max_total_bytes:
            self.budget_exhausted = True
            return {"status": "REJECTED_EVIDENCE_BUDGET", "sha256": sha,
                    "bytes": len(data)}
        record = {"id": len(self.records), "sha256": sha, "bytes": len(data),
                  "target_identity": dict(target_identity), "meta": dict(meta),
                  "data": data}
        self.records.append(record)
        self.total_bytes += len(data)
        return {"status": "STORED", "snapshot_id": record["id"], "sha256": sha,
                "bytes": len(data)}


def discover_target_child(root_pid, *, expected_root_identity, io):
    """Find direct children of one verified root PID.  PID reuse (start_ticks
    mismatch), a vanished root, zero or multiple indistinguishable candidates
    all report found/unambiguous truthfully; no guessing."""
    root = io.pid_identity(root_pid)
    if root is None or not pid_identity_matches(expected_root_identity, root):
        return {"root_verified": False, "root_identity": root, "candidates": [],
                "found": False, "unambiguous": False,
                "reason": "ROOT_IDENTITY_MISMATCH_OR_GONE"}
    candidates = []
    for pid in io.list_pids():
        identity = io.pid_identity(pid)
        if identity is None:
            continue
        if identity["ppid"] == root_pid and identity["uid"] == os.getuid():
            candidates.append(identity)
    candidates.sort(key=lambda item: item["pid"])
    if len(candidates) == 1:
        reason = None
    elif not candidates:
        reason = "NO_DIRECT_CHILD_FOUND"
    else:
        reason = "AMBIGUOUS_DIRECT_CHILDREN"
    return {"root_verified": True, "root_identity": root, "candidates": candidates,
            "found": bool(candidates), "unambiguous": len(candidates) == 1,
            "reason": reason}


def observe_target(pid, *, expected_pid_identity, expected_elf, expected_dsos,
                   io, snapshot_store=None, **capture_budgets):
    """Capture one target: identity before, bounded maps read to real EOF,
    identity after, then direct-DSO verification from this one target's
    entries only.  Any failure branch blocks evidence_complete."""
    result = {
        "pid": pid,
        "identity_before": None,
        "identity_after": None,
        "exe_identity_before": None,
        "exe_identity_after": None,
        "identity_failure": None,
        "maps_read": None,
        "maps_eof_complete": False,
        "target_identity_verified": False,
        "parse_errors": [],
        "dso_verification": None,
        "target_direct_dso_complete": False,
        "missing_dsos": [item["name"] for item in expected_dsos],
        "snapshot": None,
        "evidence_complete": False,
    }
    try:
        before = io.pid_identity(pid)
    except (OSError, ReadError):
        before = None
    result["identity_before"] = before
    if before is None or not pid_identity_matches(expected_pid_identity, before):
        result["identity_failure"] = "PID_IDENTITY_BEFORE_MISMATCH"
        return result
    try:
        exe_before = io.exe_identity(pid)
    except (OSError, ReadError):
        exe_before = None
    result["exe_identity_before"] = exe_before
    if exe_before is None or not exe_identity_matches(expected_elf, exe_before):
        result["identity_failure"] = "EXE_IDENTITY_BEFORE_MISMATCH"
        return result

    maps = io.read_text_stream(pid, "maps", **capture_budgets)
    result["maps_read"] = maps.as_dict()
    result["maps_eof_complete"] = maps.read_status == READ_COMPLETE

    try:
        after = io.pid_identity(pid)
    except (OSError, ReadError):
        after = None
    result["identity_after"] = after
    try:
        exe_after = io.exe_identity(pid)
    except (OSError, ReadError):
        exe_after = None
    result["exe_identity_after"] = exe_after
    identity_ok = (after is not None
                   and pid_identity_matches(expected_pid_identity, after)
                   and exe_after is not None
                   and exe_identity_matches(expected_elf, exe_after))
    result["target_identity_verified"] = identity_ok

    if maps.read_status == READ_COMPLETE and identity_ok:
        entries, errors = parse_maps(maps.data)
        result["parse_errors"] = errors
        verification = verify_direct_dso_entries(entries, expected_dsos)
        result["dso_verification"] = verification
        result["target_direct_dso_complete"] = bool(verification["complete"]) and not errors
        result["missing_dsos"] = verification["missing"]
        if snapshot_store is not None:
            result["snapshot"] = snapshot_store.add(
                maps.data,
                {"pid": before["pid"], "start_ticks": before["start_ticks"],
                 "exe_sha256": exe_before["sha256"]},
                maps.as_dict())

    result["evidence_complete"] = bool(
        result["maps_eof_complete"] and result["target_identity_verified"]
        and result["target_direct_dso_complete"])
    return result
