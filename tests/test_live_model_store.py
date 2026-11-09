#!/usr/bin/env python3
"""Original Store + loopback TLS integration; never a provider acceptance test.

All database rows, endpoints, certificates, bearer values and usage evidence
are disposable synthetic fixtures. SIGKILL tests retain the original database
and recover through the real writer takeover and Store accounting APIs.
"""
from __future__ import annotations

import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import selectors
import signal
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
import unittest

if len(sys.argv) != 3:
    raise SystemExit("usage: test_live_model_store.py DRIVER SOURCE")
DRIVER, SOURCE = (Path(value).resolve() for value in sys.argv[1:])
# Reuse the already exercised owned TLS peer/certificate fixture without
# changing or executing the historical transport test suite.
spec = importlib.util.spec_from_file_location("wls_transport_fixture", SOURCE / "tests/test_https_transport.py")
tls = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tls)
sys.argv = [sys.argv[0]]
BEARER = "WLS_SYNTHETIC_STORE_BEARER_8E42"

# Opt-in local raw-evidence capture for the two native wire/DB cases. With
# WLS_LIVE_MODEL_STORE_EVIDENCE_DIR unset or empty every test keeps its
# original path. The evidence root is created once per run and every case
# directory and original file is created exclusively (O_CREAT|O_EXCL|
# O_NOFOLLOW, 0600 files under 0700 directories) and never overwritten.
EVIDENCE_ENVIRONMENT = "WLS_LIVE_MODEL_STORE_EVIDENCE_DIR"
CAPTURE_TESTS = (
    "test_compat_two_native_rounds_persist_reopen_and_third_request_body",
    "test_compat_native_priced_pair_reopens_without_receipts_or_reconciliation",
)
DATABASE_SIDECAR_SUFFIXES = ("-wal", "-shm", "-journal")
EXCLUDED_CERTIFICATE_MATERIAL = ("*.key", "*.csr", "*.ext", "private.pem",
                                 "untrusted.pem", "wrong-host.pem")


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def read_regular_fd(descriptor):
    details = os.fstat(descriptor)
    if not stat.S_ISREG(details.st_mode):
        raise OSError("capture source is not a regular file")
    chunks = []
    while True:
        chunk = os.read(descriptor, 1 << 20)
        if not chunk:
            break
        chunks.append(chunk)
    return b"".join(chunks), details


def read_regular_file(path):
    # Refuse symlinks and non-regular paths so capture originals are never
    # read through a replaced path.
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        return read_regular_fd(descriptor)
    finally:
        os.close(descriptor)


class ReceivingConnection:
    """Recv-only adapter handed to Peer.request for observed requests.

    Every call forwards the exact arguments to the original connection recv,
    returns the same bytes and records them (their length, or null plus the
    exception type when recv raised). No extra receive call, no parsing and
    no other socket API is introduced.
    """

    def __init__(self, connection, record):
        self.connection = connection
        self.record = record

    def recv(self, *arguments, **keywords):
        record = self.record
        record["recv_call_count"] += 1
        try:
            chunk = self.connection.recv(*arguments, **keywords)
        except BaseException as error:
            record["recv_chunk_lengths"].append(None)
            record["recv_exceptions"].append(type(error).__name__)
            raise
        record["recv_chunk_lengths"].append(len(chunk))
        record["chunks"].append(chunk)
        return chunk


def prepare_evidence_root(raw):
    # Validate the operator-designated private target and create the single
    # capture root atomically. A pre-existing target (including a dangling
    # symlink), foreign ownership, a non-private parent or a symlinked path
    # component fails class setup; nothing is adopted, cleaned or retried.
    if not os.path.isabs(raw):
        raise AssertionError(f"{EVIDENCE_ENVIRONMENT} must be an absolute path")
    target = Path(raw)
    component = Path(target.anchor)
    for part in target.parent.parts[1:]:
        component = component / part
        details = os.lstat(component)
        if stat.S_ISLNK(details.st_mode) or not stat.S_ISDIR(details.st_mode):
            raise AssertionError(f"evidence path component is not a real directory: {component}")
    parent = os.stat(target.parent)
    if parent.st_uid != os.getuid():
        raise AssertionError("evidence parent directory must be owned by the current user")
    if stat.S_IMODE(parent.st_mode) & 0o077:
        raise AssertionError("evidence parent directory must be private")
    try:
        os.lstat(target)
    except FileNotFoundError:
        pass
    else:
        raise AssertionError(f"evidence target already exists: {target}")
    os.mkdir(target, 0o700)
    created = os.lstat(target)
    if not stat.S_ISDIR(created.st_mode) or stat.S_IMODE(created.st_mode) != 0o700 \
            or created.st_uid != os.getuid():
        raise AssertionError("evidence root was not created as a private 0700 directory")
    return target


class LiveStore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        requested = os.environ.get(EVIDENCE_ENVIRONMENT)
        cls.evidence_root = prepare_evidence_root(requested) if requested else None
        tls.HttpsTransport.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        tls.HttpsTransport.tearDownClass.__func__(cls)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="wls-live-store-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.directory.chmod(0o700)
        self.counter = 0
        self.live_capture = None
        if type(self).evidence_root is not None and self._testMethodName in CAPTURE_TESTS:
            self.live_capture = {
                "capture_id": os.urandom(16).hex(),
                "test_id": self._testMethodName,
                "started_utc": utc_now(),
                "requests": [],
                "response_sequence": [],
                "inputs": {},
                "reader_state": "not_opened",
                "reader_connection": None,
                "peer": None,
                "peer_quiescent": None,
                "assertions_completed": False,
                "action": None,
                "database": None,
                "driver": {"state": "not_run", "argv": None, "cwd": None, "timeout_seconds": None,
                           "returncode": None, "stdout": None, "stderr": None,
                           "timeout_stdout": None, "timeout_stderr": None, "exception": None},
                "capture_errors": [],
            }
            # Cleanup LIFO: the evidence save runs before the temporary
            # cleanup registered above, and any SQL reader callback added
            # inside the test body runs before this save.
            self.addCleanup(self.save_live_capture)

    def peer(self, handler=None):
        return tls.Peer(self.contexts["server"], handler or tls.fixed_response(tls.response_body()))

    def configuration(self, peer=None, **options):
        self.counter += 1
        database = options.pop("database", self.directory / f"database-{self.counter}.sqlite")
        config = {"database": str(database), **options}
        if peer:
            config.update(port=peer.port, ca_path=str(self.certificates / "ca.pem"))
        path = self.directory / f"configuration-{self.counter}.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        path.chmod(0o600)
        return path, Path(database)

    def invoke(self, peer=None, **options):
        path, database = self.configuration(peer, **options)
        started = time.monotonic()
        result = subprocess.run([str(DRIVER), str(path), str(SOURCE)], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=18, check=False)
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        self.assertEqual(result.stderr, "")
        self.assertLess(time.monotonic() - started, 16)
        parsed = json.loads(result.stdout)
        if not parsed.get("threw"):
            self.assertTrue(parsed["ledger"]["consistent"])
        return parsed, database

    def compat_profile(self):
        path = self.directory / "compat-profile.json"
        if not path.exists():
            path.write_text(json.dumps({
                "format": "wls-responses-compat-v1",
                "reference": "openai-responses-2026-11-08",
                "model_aliases": {"o3": ["o3-2025-04-16"]}}))
            path.chmod(0o600)
        return path

    def pricing_profile(self):
        # Both cache dimensions are "reported" so the compat fixtures that
        # report cache writes stay inside the frozen policy. Hand rates:
        # input 1.5, output 4.0, cache read 0.25, cache write 0.15 micro USD
        # per million tokens; all estimates below use one final ceil.
        path = self.directory / "pricing-profile.json"
        path.write_text(json.dumps({
            "format": "wls-token-pricing-v1", "currency": "USD",
            "unit": "micro_usd_per_million_tokens",
            "input_micro_usd_per_million_tokens": 1500000,
            "output_micro_usd_per_million_tokens": 4000000,
            "cache_read": {"applicability": "reported", "micro_usd_per_million_tokens": 250000},
            "cache_write": {"applicability": "reported", "micro_usd_per_million_tokens": 150000}}))
        path.chmod(0o600)
        return path

    def run_raw(self, peer=None, **options):
        # Driver actions with their own result document (no ledger key).
        path, database = self.configuration(peer, **options)
        capture = self.live_capture
        argv = [str(DRIVER), str(path), str(SOURCE)]
        if capture is None:
            result = subprocess.run(argv, cwd=self.directory, capture_output=True, text=True,
                                    encoding="utf-8", timeout=18, check=False)
        else:
            capture["action"] = options.get("action")
            capture["database"] = str(database)
            self.register_capture_input(capture, path.name)
            capture["driver"].update(argv=argv, cwd=str(self.directory), timeout_seconds=18)
            try:
                completed = subprocess.run(argv, cwd=self.directory, capture_output=True,
                                            timeout=18, check=False)
            except subprocess.TimeoutExpired as error:
                capture["driver"].update(state="timed_out", exception=type(error).__name__,
                                         timeout_stdout=error.stdout if isinstance(error.stdout, bytes) else None,
                                         timeout_stderr=error.stderr if isinstance(error.stderr, bytes) else None)
                raise
            # The raw originals reach case memory before any returncode,
            # stderr or JSON assertion observes them.
            capture["driver"].update(state="completed", returncode=completed.returncode,
                                     stdout=completed.stdout, stderr=completed.stderr)
            result = subprocess.CompletedProcess(argv, completed.returncode,
                                                 stdout=self.decode_driver_output(completed.stdout),
                                                 stderr=self.decode_driver_output(completed.stderr))
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        self.assertEqual(result.stderr, "")
        return json.loads(result.stdout), database

    def assert_original(self, result):
        attempt = result["attempt"]
        response = attempt["response"]
        request = attempt["request_envelope"]
        for field in ("call_id", "attempt_id", "role", "role_call_index", "binding", "config_snapshot_id"):
            self.assertEqual(response[field], request[field])
        self.assertEqual(result["safe"]["execution"]["execution_kind"], "local_tls")
        self.assertFalse(result["safe"]["accounting"]["business_authority_restored"])

    # ---- opt-in raw-evidence capture (native wire/DB cases only) ----

    @staticmethod
    def decode_driver_output(data):
        # Same strict UTF-8 plus universal-newline translation subprocess
        # applies for text=True runs, so assertions and json.loads observe
        # exactly their original inputs.
        return data.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")

    def register_capture_input(self, capture, name):
        try:
            data, _ = read_regular_file(self.directory / name)
        except OSError as error:
            capture["capture_errors"].append(f"input registration failed for {name}: {error!r}")
            return
        capture["inputs"][name] = {"bytes": data, "sha256": sha256_bytes(data)}

    def begin_live_capture(self, peer, *response_sequence):
        capture = self.live_capture
        if capture is None:
            return
        capture["peer"] = peer
        capture["response_sequence"] = [{"body": body, "sent": False} for body in response_sequence]
        for name in ("compat-profile.json", "pricing-profile.json"):
            if (self.directory / name).exists():
                self.register_capture_input(capture, name)

    def live_request(self, peer, connection):
        capture = self.live_capture
        if capture is None:
            peer.request(connection)
            return
        record = {"chunks": [], "recv_chunk_lengths": [], "recv_call_count": 0,
                  "recv_exceptions": [], "body": None, "line": None,
                  "request_error": None, "response_payload": None,
                  "response_body": None, "sendall_returned": None}
        capture["requests"].append(record)
        try:
            observation = peer.request(ReceivingConnection(connection, record))
        except BaseException as error:
            record["request_error"] = type(error).__name__
            raise
        record["body"] = observation["body"]
        record["line"] = observation["line"]

    def live_response(self, connection, payload, body):
        capture = self.live_capture
        record = capture["requests"][-1] if capture is not None and capture["requests"] else None
        if record is not None:
            record["response_payload"] = payload
            record["response_body"] = body
            record["sendall_returned"] = False
            for entry in capture["response_sequence"]:
                if entry["body"] is body:
                    entry["sent"] = True
                    break
        connection.sendall(payload)
        if record is not None:
            record["sendall_returned"] = True

    def register_live_capture_reader(self, connection):
        capture = self.live_capture
        if capture is None:
            return
        capture["reader_state"] = "open"
        capture["reader_connection"] = connection
        # Registered after the evidence cleanup, so LIFO closes this reader
        # before the evidence save and the original temporary cleanup.
        self.addCleanup(self.close_live_capture_reader)

    def close_live_capture_reader(self):
        capture = self.live_capture
        connection = capture["reader_connection"] if capture is not None else None
        if connection is None:
            return
        try:
            connection.close()
        except BaseException:
            capture["reader_state"] = "close_failed"
            raise
        capture["reader_state"] = "closed"

    def complete_live_capture(self):
        if self.live_capture is not None:
            self.live_capture["assertions_completed"] = True

    def open_capture_directories(self):
        case = type(self).evidence_root / self._testMethodName
        try:
            os.lstat(case)
        except FileNotFoundError:
            os.mkdir(case, 0o700)
        else:
            raise RuntimeError(f"capture case directory already exists: {case}")
        details = os.lstat(case)
        if stat.S_IMODE(details.st_mode) != 0o700:
            raise RuntimeError(f"capture case directory is not private: {case}")
        opened = {"case": os.open(case, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)}
        try:
            for name in ("requests", "responses", "inputs", "certificates", "db"):
                os.mkdir(name, 0o700, dir_fd=opened["case"])
                opened[name] = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                       dir_fd=opened["case"])
        except BaseException:
            for descriptor in opened.values():
                os.close(descriptor)
            raise
        return opened

    @staticmethod
    def write_capture_original(directory_fd, name, data):
        descriptor = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                             0o600, dir_fd=directory_fd)
        try:
            view = memoryview(data)
            while view:
                view = view[os.write(descriptor, view):]
            details = os.fstat(descriptor)
            if stat.S_IMODE(details.st_mode) != 0o600:
                raise RuntimeError(f"capture original {name} did not keep mode 0600")
        finally:
            os.close(descriptor)
        return {"bytes": len(data), "sha256": sha256_bytes(data), "mode": "0o600"}

    def copy_database_original(self, directory_fd, source, before_stat):
        # Physical byte copy of one pinned regular file: stat/read/verify
        # around the copy, never opened as a database and never rewritten.
        data, during_stat = read_regular_file(source)
        after_stat = os.lstat(source)
        identity = ("st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
        stable = all(getattr(before_stat, field) == getattr(during_stat, field) == getattr(after_stat, field)
                     for field in identity)
        written = self.write_capture_original(directory_fd, source.name, data)
        descriptor = os.open(source.name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory_fd)
        try:
            persisted, _ = read_regular_fd(descriptor)
        finally:
            os.close(descriptor)
        return {"basename": source.name, "absent": False, "regular": True,
                "bytes": written["bytes"], "sha256": written["sha256"], "mode": written["mode"],
                "source_stat": {field: getattr(before_stat, field) for field in identity},
                "source_unchanged_during_copy": stable,
                "copy_matches_source": persisted == data}

    def capture_source_identity(self):
        def facts(path):
            try:
                data, details = read_regular_file(path)
            except OSError as error:
                return {"path": str(path), "sha256": None, "bytes": None, "error": repr(error)}
            return {"path": str(path), "sha256": sha256_bytes(data), "bytes": details.st_size}
        return {
            "driver": facts(DRIVER),
            "source_root": str(SOURCE),
            "test_file": facts(Path(__file__).resolve()),
            "shared_tls_helper": facts(SOURCE / "tests/test_https_transport.py"),
            "fixtures": [facts(SOURCE / "tests/fixtures/responses/reasoning-tool-response.json"),
                         facts(SOURCE / "tests/fixtures/responses/reasoning-text-response.json")],
            "generated_round_b": {
                "derived_from": str(SOURCE / "tests/fixtures/responses/reasoning-tool-response.json"),
                "generated_once_in_test": True,
                "retained_as": "inputs/response-sequence-02.bin"},
        }

    def save_live_capture(self):
        capture = self.live_capture
        if capture is None:
            return
        capture["ended_utc"] = utc_now()
        errors = capture["capture_errors"]
        driver = capture["driver"]
        peer = capture["peer"]

        def keep(label, action):
            try:
                return action()
            except Exception as failure:  # keep saving the remaining originals
                errors.append(f"{label}: {failure!r}")
                return None

        if peer is not None:
            active = peer.thread.is_alive() or any(worker.is_alive() for worker in peer.workers)
            capture["peer_quiescent"] = not active
            if active:
                errors.append("peer listener/worker threads still active at capture time")
        if capture["reader_state"] == "open":
            errors.append("registered SQL reader was not closed before the capture save")
        parsed = None
        if driver["state"] == "completed" and isinstance(driver["stdout"], bytes):
            try:
                parsed = json.loads(driver["stdout"].decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                parsed = None

        originals = []
        manifest = {
            "capture_id": capture["capture_id"],
            "test_id": capture["test_id"],
            "action": capture["action"],
            "evidence_environment": EVIDENCE_ENVIRONMENT,
            "evidence_root": str(type(self).evidence_root),
            "started_utc": capture["started_utc"],
            "ended_utc": capture["ended_utc"],
            "driver": {key: driver[key] for key in ("state", "argv", "cwd", "timeout_seconds",
                                                    "returncode", "exception")},
            "driver_reaped": driver["state"] in ("completed", "timed_out", "not_run"),
            "peer": {"registered": peer is not None,
                     "quiescent": capture["peer_quiescent"],
                     "accepted": peer.accepted if peer is not None else None,
                     "requests_observed": len(capture["requests"])},
            "sql_reader": {"state": capture["reader_state"]},
            "assertions_completed": capture["assertions_completed"],
            "requests": [],
            "response_sequence": [],
            "database_files": [],
            "inputs": {},
            "originals": originals,
            "sources": {},
            "verification": {},
            "evidence_semantics": {
                "wire_kind": "original HTTP plaintext bytes returned by the existing recv calls "
                              "inside the loopback TLS connection; not TLS ciphertext, not a pcap, "
                              "and a sendall return is not client-consumption proof",
                "database_kind": "final physical end state of this capture; the reopen middle "
                                 "states remain only in the driver stdout originals",
                "manifest_kind": "derived index written last; it cannot replace the originals",
            },
            "excluded": {"certificate_material": list(EXCLUDED_CERTIFICATE_MATERIAL),
                         "reason": "private keys and unrelated certificate material are never "
                                   "copied; only the public ca.pem and server.pem bytes are retained"},
            "capture_errors": errors,
        }
        if isinstance(parsed, dict):
            def pick(node, key):
                if not isinstance(node, dict):
                    return None
                if key in node:
                    return node[key]
                envelope = node.get("request_envelope")
                return envelope.get(key) if isinstance(envelope, dict) else None

            def attempt_identity(node):
                if not isinstance(node, dict):
                    return None
                return {"attempt_id": pick(node, "attempt_id"), "call_id": pick(node, "call_id"),
                        "role": pick(node, "role"), "role_call_index": pick(node, "role_call_index")}

            before = parsed.get("before_reopen") if isinstance(parsed.get("before_reopen"), dict) else {}
            first = before.get("first") if isinstance(before.get("first"), dict) else {}
            second = before.get("second") if isinstance(before.get("second"), dict) else {}
            manifest["wire_identity"] = {
                "request_id": parsed.get("request_id"),
                "parsed_stdout": True,
                "first": attempt_identity(first.get("attempt")),
                "second": attempt_identity(second.get("attempt")),
                "third": attempt_identity(parsed.get("third_settlement")),
                "source": "same-capture driver stdout only; null fields were not present",
            }
        else:
            manifest["wire_identity"] = {"parsed_stdout": False,
                                         "source": "driver stdout unavailable or unparsable; identities unknown"}

        try:
            opened = self.open_capture_directories()
        except (OSError, RuntimeError) as error:
            errors.append(f"capture case directory failure: {error!r}")
            raise AssertionError(f"live evidence capture failed for {capture['test_id']}: "
                                 + "; ".join(errors)) from None

        def record_original(section, name, data):
            entry = self.write_capture_original(opened[section], name, data)
            originals.append({"path": name if section == "case" else f"{section}/{name}",
                              "bytes": entry["bytes"], "sha256": entry["sha256"],
                              "mode": entry["mode"]})

        try:
            # 1. Driver originals: the exact subprocess bytes, saved before use.
            if driver["state"] == "completed":
                keep("driver.stdout.bin", lambda: record_original("case", "driver.stdout.bin", driver["stdout"]))
                keep("driver.stderr.bin", lambda: record_original("case", "driver.stderr.bin", driver["stderr"]))
            elif driver["state"] == "timed_out":
                if driver["timeout_stdout"] is not None:
                    keep("driver.timeout-stdout.bin", lambda: record_original(
                        "case", "driver.timeout-stdout.bin", driver["timeout_stdout"]))
                if driver["timeout_stderr"] is not None:
                    keep("driver.timeout-stderr.bin", lambda: record_original(
                        "case", "driver.timeout-stderr.bin", driver["timeout_stderr"]))
            # 2. Wire originals in actual receive order.
            for index, record in enumerate(capture["requests"], start=1):
                prefix = f"{index:02d}"
                stream = b"".join(record["chunks"])
                keep(f"requests/{prefix}.read-stream.bin", lambda: record_original(
                    "requests", f"{prefix}.read-stream.bin", stream))
                if record["body"] is not None:
                    keep(f"requests/{prefix}.body.bin", lambda: record_original(
                        "requests", f"{prefix}.body.bin", record["body"]))
                if record["response_payload"] is not None:
                    keep(f"responses/{prefix}.payload.bin", lambda: record_original(
                        "responses", f"{prefix}.payload.bin", record["response_payload"]))
                    if record["response_body"] is not None:
                        keep(f"responses/{prefix}.body.bin", lambda: record_original(
                            "responses", f"{prefix}.body.bin", record["response_body"]))
                length_sum = sum(length for length in record["recv_chunk_lengths"] if length is not None)
                if length_sum != len(stream):
                    errors.append(f"request {index} recv lengths do not sum to the read stream")
                manifest["requests"].append({
                    "index": index,
                    "recv_call_count": record["recv_call_count"],
                    "recv_chunk_lengths": record["recv_chunk_lengths"],
                    "recv_exceptions": record["recv_exceptions"],
                    "read_stream_bytes": len(stream),
                    "body_bytes": len(record["body"]) if record["body"] is not None else None,
                    "request_error": record["request_error"],
                    "sendall_returned": record["sendall_returned"],
                })
            # 3. Preset response sequence; unused entries stay preset inputs.
            for index, entry in enumerate(capture["response_sequence"], start=1):
                keep(f"inputs/response-sequence-{index:02d}.bin", lambda: record_original(
                    "inputs", f"response-sequence-{index:02d}.bin", entry["body"]))
                manifest["response_sequence"].append({"index": index, "bytes": len(entry["body"]),
                                                      "sent": entry["sent"]})
            # 4. Driver input files, re-verified unchanged against the temp dir.
            for name, registered in capture["inputs"].items():
                keep(f"inputs/{name}", lambda: record_original("inputs", name, registered["bytes"]))
                current = keep(f"verify input {name}", lambda: read_regular_file(self.directory / name))
                unchanged = bool(current) and sha256_bytes(current[0]) == registered["sha256"]
                manifest["inputs"][name] = {"registered_sha256": registered["sha256"],
                                            "unchanged_after_run": unchanged}
                if not unchanged:
                    errors.append(f"captured input {name} changed during the run")
            # 5. Public certificates only; keys are excluded by policy.
            for name in ("ca.pem", "server.pem"):
                certificate = keep(f"read certificate {name}",
                                   lambda: read_regular_file(self.certificates / name))
                if certificate is not None:
                    keep(f"certificates/{name}", lambda: record_original(
                        "certificates", name, certificate[0]))
            # 6. Physical database files and real sidecars, byte for byte.
            if capture["database"] is not None:
                database_path = Path(capture["database"])
                for basename in [database_path.name] + [database_path.name + suffix
                                                        for suffix in DATABASE_SIDECAR_SUFFIXES]:
                    source = database_path.parent / basename
                    try:
                        before_stat = os.lstat(source)
                    except FileNotFoundError:
                        manifest["database_files"].append({"basename": basename, "absent": True})
                        continue
                    if not stat.S_ISREG(before_stat.st_mode):
                        errors.append(f"database file {basename} is not a regular file")
                        manifest["database_files"].append({"basename": basename, "absent": False,
                                                           "regular": False})
                        continue
                    entry = keep(f"copy {basename}", lambda: self.copy_database_original(
                        opened["db"], source, before_stat))
                    if entry is not None:
                        manifest["database_files"].append(entry)
                        originals.append({"path": f"db/{basename}", "bytes": entry["bytes"],
                                          "sha256": entry["sha256"], "mode": entry["mode"]})
                        if not entry["source_unchanged_during_copy"] or not entry["copy_matches_source"]:
                            errors.append(f"database copy verification failed for {basename}")
            # 7. Source identities: driver, test file, shared TLS helper, fixtures.
            manifest["sources"] = keep("source identity", self.capture_source_identity) or {}
            # 8. Same-capture consistency: the third wire equals the stdout field.
            third_field = parsed.get("third_request_body") if isinstance(parsed, dict) else None
            third_body = capture["requests"][2]["body"] if len(capture["requests"]) >= 3 else None
            if isinstance(third_field, str) and third_body is not None:
                equal = third_body == third_field.encode("utf-8")
                manifest["verification"]["third_wire_equals_stdout_field"] = equal
                if not equal:
                    errors.append("third captured request body does not equal the same-run stdout field")
            else:
                manifest["verification"]["third_wire_equals_stdout_field"] = None
            manifest["retention"] = {
                "originals_count": len(originals),
                "complete": (not errors and capture["assertions_completed"]
                             and driver["state"] == "completed" and len(capture["requests"]) == 3),
                "definition": "no capture errors, original assertions completed, driver completed "
                              "and all three requests retained",
            }
            document = json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8")
            keep("capture-manifest.json", lambda: self.write_capture_original(
                opened["case"], "capture-manifest.json", document))
        finally:
            for descriptor in opened.values():
                os.close(descriptor)
        if errors:
            raise AssertionError(f"live evidence capture failed for {capture['test_id']}: " + "; ".join(errors))

    def test_complete_original_store_path_and_exact_replay(self):
        body = tls.response_body("Loopback original bytes 😀")
        with self.peer(tls.fixed_response(body)) as peer:
            result, database = self.invoke(peer, reconcile=True)
            self.assertEqual(len(peer.requests), 1)
            self.assertEqual(peer.requests[0]["headers"][b"authorization"], f"Bearer {BEARER}".encode())
            self.assertEqual(peer.requests[0]["line"], b"POST /v1/responses HTTP/1.1")
        self.assert_original(result)
        self.assertEqual(result["attempt"]["accepted"], 1)
        self.assertTrue(result["first_sent"])
        self.assertFalse(result["replay_sent"])
        self.assertFalse(result["replay_new_send_authority"])
        self.assertEqual(bytes.fromhex(result["private"]["private_observation"]["raw_body_hex"]), body)
        self.assertIsNone(result["attempt"]["usage"]["cost_micro_usd"])
        self.assertEqual(result["budget"]["unknown_cost"], 100)
        after = result["after_reconcile"]
        self.assertEqual(after["attempt"], result["attempt"])
        self.assertEqual(after["budget"]["spent_cost"], 17)
        self.assertEqual(after["budget"]["reserved_cost"], 0)
        self.assertTrue(after["safe"]["accounting"]["accounting_complete"])
        with sqlite3.connect(database) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM live_send_claims").fetchone()[0], 1)
            self.assertEqual(connection.execute("SELECT count(*) FROM call_attempts").fetchone()[0], 1)

    def test_sse_tool_terminal_is_original_data_without_tool_dispatch(self):
        body = tls.tool_stream()
        with self.peer(tls.fixed_response(body, content_type="text/event-stream")) as peer:
            result, database = self.invoke(peer, stream=True)
            self.assertEqual(len(peer.requests), 1)
        self.assert_original(result)
        self.assertEqual(result["attempt"]["accepted"], 1)
        self.assertEqual(len(result["attempt"]["response"]["tool_calls"]), 1)
        with sqlite3.connect(database) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM ops").fetchone()[0], 0)

    def test_compat_two_round_reasoning_replay_over_tls(self):
        compat = self.compat_profile()
        first_body = (SOURCE / "tests/fixtures/responses/reasoning-tool-response.json").read_bytes()
        second_body = (SOURCE / "tests/fixtures/responses/reasoning-text-response.json").read_bytes()
        remaining = iter((first_body, second_body))

        def sequenced(peer, connection):
            peer.request(connection)
            body = next(remaining)
            connection.sendall(tls.headers(len(body)) + body)

        with self.peer(sequenced) as peer:
            result, database = self.run_raw(peer, action="compat-two-round",
                                            compat_profile=str(compat), reopen=True, reconcile=True)
            self.assertEqual(len(peer.requests), 2)
            captured = peer.requests[1]["body"]
        self.assertEqual(captured.decode(), result["second_request_body"])
        original = json.loads(first_body)
        reasoning, call = original["output"][0], original["output"][1]
        body = json.loads(result["second_request_body"])
        self.assertEqual(body["model"], "o3")
        self.assertEqual(body["include"], ["reasoning.encrypted_content"])
        with self.assertRaises(json.JSONDecodeError):
            json.loads(body["instructions"])
        items = body["input"]
        self.assertEqual(len(items), 4)  # task contract, reasoning, call, result
        self.assertEqual(items[1], reasoning)  # opaque value replayed unchanged
        self.assertEqual(items[2]["type"], "function_call")
        self.assertEqual(items[2]["call_id"], "call_compat_fixture")
        self.assertEqual(items[2]["arguments"], call["arguments"])
        self.assertEqual(items[3]["type"], "function_call_output")
        self.assertEqual(items[3]["call_id"], "call_compat_fixture")
        self.assertEqual(json.loads(items[3]["output"]), {"ok": True, "round": 1})
        self.assertFalse(any(item.get("role") == "assistant" for item in items))
        self.assertEqual(result["first_settlement"]["accepted"], 1)
        first = result["first_response"]
        self.assertEqual(first["status"], "completed")
        self.assertEqual(first["tool_calls"][0]["call_id"], "call_compat_fixture")
        self.assertEqual(first["responses_output"]["output"][0]["encrypted_content"],
                         reasoning["encrypted_content"])
        second = result["second_response"]
        self.assertEqual(second["status"], "completed")
        self.assertEqual(second["text"], "compat fixture text 原文😀")
        self.assertEqual(second["responses_output"]["output"][0]["summary"], [])
        with sqlite3.connect(database) as connection:
            self.assertEqual(connection.execute("PRAGMA user_version").fetchone()[0], 410)
            self.assertEqual(connection.execute("SELECT count(*) FROM live_send_claims").fetchone()[0], 2)
            self.assertEqual(connection.execute("SELECT count(*) FROM ops").fetchone()[0], 0)

    def test_compat_two_native_rounds_persist_reopen_and_third_request_body(self):
        compat = self.compat_profile()
        first_body = (SOURCE / "tests/fixtures/responses/reasoning-tool-response.json").read_bytes()
        # Round B serves the same supported shape with distinct provider
        # item/call IDs, a distinct opaque reasoning original and its own
        # arguments; it stays an official-schema-derived synthetic fixture.
        second_original = json.loads(first_body)
        second_original["id"] = "resp_compat_fixture_tool_b"
        second_original["output"][0]["id"] = "rs_compat_fixture_round_b"
        second_original["output"][0]["summary"][0]["text"] = "Round B synthetic summary for distinct opaque history."
        second_original["output"][0]["encrypted_content"] = (
            "gAAAAAB3bHlzLXN5bnRoZXRpYy1lbmNyeXB0ZWQtcmVhc29uaW5nLW8zLTAwMDM=")
        second_original["output"][1]["id"] = "fc_compat_fixture_b"
        second_original["output"][1]["call_id"] = "call_compat_fixture_b"
        second_original["output"][1]["arguments"] = '{"message":"compat fixture round B"}'
        second_body = tls.encoded(second_original)
        third_body = (SOURCE / "tests/fixtures/responses/reasoning-text-response.json").read_bytes()
        remaining = iter((first_body, second_body, third_body))

        def sequenced(peer, connection):
            self.live_request(peer, connection)
            body = next(remaining)
            self.live_response(connection, tls.headers(len(body)) + body, body)

        with self.peer(sequenced) as peer:
            self.begin_live_capture(peer, first_body, second_body, third_body)
            result, database = self.run_raw(peer, action="compat-native-two-reopen-third",
                                            compat_profile=str(compat), reconcile=True)
            self.assertEqual(len(peer.requests), 3)
            captured = peer.requests[2]["body"]
        self.assertEqual(captured.decode(), result["third_request_body"])
        # The persisted originals, settlements and immutable feedback
        # fingerprints are identical before and after the Store reopen.
        self.assertEqual(result["before_reopen"], result["after_reopen"])
        for settlement in ("first_settlement", "second_settlement", "third_settlement"):
            self.assertEqual(result[settlement]["accepted"], 1)
        first = result["after_reopen"]["first"]["attempt"]["response"]
        second = result["after_reopen"]["second"]["attempt"]["response"]
        self.assertEqual(first["tool_calls"][0]["call_id"], "call_compat_fixture")
        self.assertEqual(second["tool_calls"][0]["call_id"], "call_compat_fixture_b")
        original = json.loads(first_body)
        body = json.loads(result["third_request_body"])
        self.assertEqual(body["model"], "o3")
        self.assertEqual(body["include"], ["reasoning.encrypted_content"])
        with self.assertRaises(json.JSONDecodeError):
            json.loads(body["instructions"])
        items = body["input"]
        self.assertEqual(len(items), 7)  # task contract, then A and B reasoning/call/result
        self.assertEqual([item["type"] for item in items[1:]],
                         ["reasoning", "function_call", "function_call_output"] * 2)
        # Opaque reasoning originals and their calls replay unchanged, in
        # original order, exactly as persisted across the reopen.
        self.assertEqual(items[1], first["responses_output"]["output"][0])
        self.assertEqual(items[2], first["responses_output"]["output"][1])
        self.assertEqual(items[4], second["responses_output"]["output"][0])
        self.assertEqual(items[5], second["responses_output"]["output"][1])
        self.assertEqual(items[1], original["output"][0])
        self.assertEqual(items[2]["call_id"], "call_compat_fixture")
        self.assertEqual(json.loads(items[2]["arguments"]), json.loads(original["output"][1]["arguments"]))
        self.assertEqual(items[4], second_original["output"][0])
        self.assertEqual(items[5]["call_id"], "call_compat_fixture_b")
        self.assertEqual(json.loads(items[5]["arguments"]), json.loads(second_original["output"][1]["arguments"]))
        # Each function_call_output pairs with its own exact provider call ID
        # and the distinguishable A/B feedback result recorded for that round.
        self.assertEqual(items[3]["type"], "function_call_output")
        self.assertEqual(items[3]["call_id"], "call_compat_fixture")
        self.assertEqual(json.loads(items[3]["output"]), {"ok": True, "round": 1, "result": "tool-result-A"})
        self.assertEqual(items[6]["type"], "function_call_output")
        self.assertEqual(items[6]["call_id"], "call_compat_fixture_b")
        self.assertEqual(json.loads(items[6]["output"]), {"ok": True, "round": 2, "result": "tool-result-B"})
        self.assertFalse(any(item.get("role") == "assistant" for item in items))
        self.assertEqual(result["third_response"]["status"], "completed")
        with sqlite3.connect(database) as connection:
            self.register_live_capture_reader(connection)
            self.assertEqual(connection.execute("PRAGMA user_version").fetchone()[0], 410)
            self.assertEqual(connection.execute("SELECT count(*) FROM live_send_claims").fetchone()[0], 3)
            self.assertEqual(connection.execute("SELECT count(*) FROM ops").fetchone()[0], 0)
            self.assertEqual(connection.execute("SELECT count(*) FROM model_tool_feedback").fetchone()[0], 2)
            self.assertEqual(sorted(row[0] for row in connection.execute("SELECT attempt_id FROM model_tool_feedback")),
                             ["attempt-fixture", "attempt-fixture-2"])
        self.complete_live_capture()

    def test_compat_refusal_classifies_transport_completed_without_dispatch(self):
        compat = self.compat_profile()
        body = (SOURCE / "tests/fixtures/responses/refusal-response.json").read_bytes()
        with self.peer(tls.fixed_response(body)) as peer:
            result, database = self.run_raw(peer, action="refusal-transport",
                                            compat_profile=str(compat))
            self.assertEqual(len(peer.requests), 1)  # no automatic retry
        summary = result["transport_summary"]
        self.assertEqual(summary["status"], "completed")
        self.assertEqual(summary["reason"], "model_refusal")
        self.assertEqual(summary["response_usable"], False)
        self.assertEqual(summary["http_status"], 200)
        attempt = result["attempt"]
        self.assertEqual(attempt["status"], "ERROR")
        self.assertEqual(attempt["accepted"], 0)
        response = attempt["response"]
        self.assertEqual(response["status"], "error")
        self.assertEqual(response["error_code"], "model_refusal")
        self.assertEqual(response["text"], "")
        self.assertEqual(response["tool_calls"], [])
        self.assertEqual(response["usage"]["input_tokens"], 11)
        self.assertEqual(response["responses_output"]["output"][1]["content"][0]["type"], "refusal")
        with sqlite3.connect(database) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM ops").fetchone()[0], 0)
        # Transport-layer failures still override the model-layer classification.
        with self.peer(tls.fixed_response(body, status=500)) as peer:
            failure, _ = self.run_raw(peer, action="refusal-transport", compat_profile=str(compat))
        self.assertEqual(failure["transport_summary"]["status"], "http_error")
        self.assertEqual(failure["attempt"]["response"]["error_code"], "http_error")
        self.assertIsNone(failure["attempt"]["response"].get("responses_output"))

    def test_compat_native_priced_pair_reopens_without_receipts_or_reconciliation(self):
        compat, pricing = self.compat_profile(), self.pricing_profile()
        first_body = (SOURCE / "tests/fixtures/responses/reasoning-tool-response.json").read_bytes()
        # Round B is generated once, here, with its own distinct provider IDs,
        # opaque original and arguments; nothing rewrites the stored originals.
        second_original = json.loads(first_body)
        second_original["id"] = "resp_compat_fixture_tool_b"
        second_original["output"][0]["id"] = "rs_compat_fixture_round_b"
        second_original["output"][0]["summary"][0]["text"] = "Round B synthetic summary for distinct opaque history."
        second_original["output"][0]["encrypted_content"] = (
            "gAAAAAB3bHlzLXN5bnRoZXRpYy1lbmNyeXB0ZWQtcmVhc29uaW5nLW8zLTAwMDM=")
        second_original["output"][1]["id"] = "fc_compat_fixture_b"
        second_original["output"][1]["call_id"] = "call_compat_fixture_b"
        second_original["output"][1]["arguments"] = '{"message":"compat fixture round B"}'
        second_body = tls.encoded(second_original)
        third_body = (SOURCE / "tests/fixtures/responses/reasoning-text-response.json").read_bytes()
        remaining = iter((first_body, second_body, third_body))

        def sequenced(peer, connection):
            self.live_request(peer, connection)
            body = next(remaining)
            self.live_response(connection, tls.headers(len(body)) + body, body)

        with self.peer(sequenced) as peer:
            self.begin_live_capture(peer, first_body, second_body, third_body)
            result, database = self.run_raw(peer, action="compat-native-priced-two-reopen-third",
                                            compat_profile=str(compat), pricing_profile=str(pricing),
                                            reserved_cost=2000)
            self.assertEqual(len(peer.requests), 3)
            captured = peer.requests[2]["body"]
            for request in peer.requests:
                self.assertNotIn(b"x-wls-billing-challenge", request["headers"])  # no signed challenge
        self.assertEqual(captured.decode(), result["third_request_body"])
        self.assertEqual(result["before_reopen"], result["after_reopen"])
        # Hand-computed per-round estimates with one final ceil each:
        #   A/B report 11 in / 7 out / 2 cached-read / 1 cache-write:
        #     ceil(8*1.5 + 2*0.25 + 1*0.15 + 7*4.0) = ceil(40.65) = 41
        #   C reports 11 in / 5 out / no cache: ceil(11*1.5 + 5*4.0) = 37
        # The frozen minimum reservation is ceil(1000*1.5 + 100*4.0) = 1900,
        # so every round reserves 2000 and stays fully covered.
        expected_usage = {"first_settlement": (11, 7, 2, 1, 41),
                          "second_settlement": (11, 7, 2, 1, 41),
                          "third_settlement": (11, 5, 0, 0, 37)}
        for settlement, (in_tokens, out_tokens, cached, cache_write, estimate) in expected_usage.items():
            row = result[settlement]
            self.assertEqual(row["accepted"], 1)
            self.assertEqual((row["input_tokens"], row["output_tokens"], row["cache_read_tokens"],
                              row["cache_write_tokens"]), (in_tokens, out_tokens, cached, cache_write))
            self.assertIsNone(row["cost_micro_usd"])  # original cost stays null
        self.assertEqual(result["budget"]["spent_cost"], 41 + 41 + 37)
        self.assertEqual(result["budget"]["accounting_format"], "wls-estimated-budget-v1")
        self.assertEqual(result["budget"]["cost_breakdown"]["estimated_micro_usd"], 119)
        self.assertEqual(result["budget"]["cost_breakdown"]["reported_micro_usd"], 0)
        self.assertTrue(result["accounting"]["ready"], result["accounting"])
        self.assertTrue(result["ledger"]["consistent"])
        first = result["after_reopen"]["first"]["attempt"]["response"]
        second = result["after_reopen"]["second"]["attempt"]["response"]
        self.assertEqual(first["tool_calls"][0]["call_id"], "call_compat_fixture")
        self.assertEqual(second["tool_calls"][0]["call_id"], "call_compat_fixture_b")
        original = json.loads(first_body)
        body = json.loads(result["third_request_body"])
        self.assertEqual(body["model"], "o3")
        self.assertEqual(body["include"], ["reasoning.encrypted_content"])
        with self.assertRaises(json.JSONDecodeError):
            json.loads(body["instructions"])
        items = body["input"]
        self.assertEqual(len(items), 7)  # task contract, then A and B reasoning/call/result
        self.assertEqual([item["type"] for item in items[1:]],
                         ["reasoning", "function_call", "function_call_output"] * 2)
        self.assertEqual(items[1], first["responses_output"]["output"][0])
        self.assertEqual(items[2], first["responses_output"]["output"][1])
        self.assertEqual(items[4], second["responses_output"]["output"][0])
        self.assertEqual(items[5], second["responses_output"]["output"][1])
        self.assertEqual(items[1], original["output"][0])
        self.assertEqual(items[2]["call_id"], "call_compat_fixture")
        self.assertEqual(json.loads(items[3]["output"]), {"ok": True, "round": 1, "result": "tool-result-A"})
        self.assertEqual(items[4], second_original["output"][0])
        self.assertEqual(items[5]["call_id"], "call_compat_fixture_b")
        self.assertEqual(json.loads(items[6]["output"]), {"ok": True, "round": 2, "result": "tool-result-B"})
        self.assertEqual(result["third_response"]["status"], "completed")
        with sqlite3.connect(database) as connection:
            self.register_live_capture_reader(connection)
            self.assertEqual(connection.execute("PRAGMA user_version").fetchone()[0], 410)
            self.assertEqual(connection.execute("SELECT count(*) FROM live_send_claims").fetchone()[0], 3)
            self.assertEqual(connection.execute("SELECT count(*) FROM ops").fetchone()[0], 0)
            self.assertEqual(connection.execute("SELECT count(*) FROM model_tool_feedback").fetchone()[0], 2)
            self.assertEqual(connection.execute("SELECT count(*) FROM usage_reconciliations").fetchone()[0], 0)
            self.assertEqual(connection.execute("SELECT count(*) FROM live_usage_projection").fetchone()[0], 3)
        self.complete_live_capture()

    def test_dual_priced_refusal_never_becomes_an_estimate_or_second_send(self):
        compat, pricing = self.compat_profile(), self.pricing_profile()
        body = (SOURCE / "tests/fixtures/responses/refusal-response.json").read_bytes()
        with self.peer(tls.fixed_response(body)) as peer:
            result, database = self.run_raw(peer, action="refusal-transport",
                                            compat_profile=str(compat), pricing_profile=str(pricing),
                                            reserved_cost=2000)
            self.assertEqual(len(peer.requests), 1)  # first send only, no automatic retry
            for request in peer.requests:
                self.assertNotIn(b"x-wls-billing-challenge", request["headers"])
        summary = result["transport_summary"]
        self.assertEqual(summary["status"], "completed")
        self.assertEqual(summary["reason"], "model_refusal")
        self.assertEqual(summary["response_usable"], False)
        attempt = result["attempt"]
        self.assertEqual(attempt["status"], "ERROR")
        self.assertEqual(attempt["accepted"], 0)
        response = attempt["response"]
        self.assertEqual(response["error_code"], "model_refusal")
        self.assertEqual(response["text"], "")
        self.assertEqual(response["tool_calls"], [])
        self.assertEqual(response["usage"]["input_tokens"], 11)  # complete usage stays reported
        self.assertEqual(response["responses_output"]["output"][1]["content"][0]["type"], "refusal")
        # An Error attempt never books the deterministic estimate: its whole
        # reservation stays unknown and nothing is spent or reconciled.
        self.assertIsNone(attempt["usage"]["cost_micro_usd"])
        self.assertEqual(result["budget"]["spent_cost"], 0)
        self.assertEqual(result["budget"]["unknown_cost"], 2000)
        self.assertTrue(result["ledger"]["consistent"])
        with sqlite3.connect(database) as connection:
            self.assertEqual(connection.execute("SELECT count(*) FROM ops").fetchone()[0], 0)
            self.assertEqual(connection.execute("SELECT count(*) FROM live_send_claims").fetchone()[0], 1)
            self.assertEqual(connection.execute("SELECT count(*) FROM usage_reconciliations").fetchone()[0], 0)
            projection = connection.execute("SELECT cost_micro_usd FROM live_usage_projection").fetchall()
            self.assertTrue(projection)
            self.assertTrue(all(row[0] is None for row in projection))

    def test_private_echo_never_enters_safe_query_or_events(self):
        body = tls.response_body(BEARER + " synthetic secret echo")
        with self.peer(tls.fixed_response(body, extra=f"X-Echo: {BEARER}\r\n".encode())) as peer:
            result, database = self.invoke(peer)
        self.assertNotIn(BEARER, json.dumps(result["safe"]))
        private = result["private"]["private_observation"]
        self.assertIn(BEARER.encode(), bytes.fromhex(private["raw_headers_hex"]))
        self.assertIn(BEARER.encode(), bytes.fromhex(private["raw_body_hex"]))
        # Raw original model responses remain private Store events by design;
        # only the transport-observation event must use the safe summary.
        with sqlite3.connect(database) as connection:
            rows = connection.execute("SELECT payload,result FROM events WHERE type='record_live_transport'").fetchall()
            self.assertEqual(len(rows), 1)
            self.assertNotIn(BEARER, json.dumps(rows))

    def test_http_429_and_500_never_automatically_retry(self):
        for status in (429, 500):
            with self.subTest(status=status), self.peer(tls.fixed_response(b'{"error":"synthetic"}', status=status)) as peer:
                result, _ = self.invoke(peer)
                self.assertEqual(len(peer.requests), 1)
                self.assertEqual(result["attempt"]["accepted"], 0)
                self.assertEqual(result["attempt"]["response"]["tool_calls"], [])
                self.assertIsNone(result["attempt"]["usage"]["cost_micro_usd"])
                self.assertEqual(result["budget"]["attempts"], 1)
                self.assertEqual(result["budget"]["unknown_cost"], 100)
                self.assertFalse(result["replay_new_send_authority"])

    def test_partial_response_deadline_preserves_unknown_and_no_tools(self):
        def partial(peer, connection):
            peer.request(connection)
            connection.sendall(tls.headers(100000, content_type="text/event-stream") + tls.tool_stream()[:160])
            peer.stop.wait(5)
        with self.peer(partial) as peer:
            result, _ = self.invoke(peer, stream=True, timeout_ms=200)
            self.assertEqual(len(peer.requests), 1)
        self.assertEqual(result["attempt"]["accepted"], 0)
        self.assertEqual(result["attempt"]["response"]["tool_calls"], [])
        self.assertEqual(result["safe"]["transport"]["status"], "deadline_exceeded")
        self.assertGreater(len(bytes.fromhex(result["private"]["private_observation"]["raw_body_hex"])), 0)
        self.assertEqual(result["budget"]["reserved_cost"], 100)
        self.assertEqual(result["budget"]["unknown_cost"], 100)

    def test_large_escaped_valid_output_fits_original_settlement_event(self):
        base = len(tls.response_body(""))
        text = "\x01" * ((1024 * 1024 - 64 - base) // 6)
        body = tls.response_body(text)
        self.assertLess(len(body), 1024 * 1024)
        with self.peer(tls.fixed_response(body)) as peer:
            result, database = self.invoke(peer)
            self.assertEqual(len(peer.requests), 1)
        self.assertEqual(result["attempt"]["accepted"], 1)
        self.assertEqual(result["attempt"]["response"]["text"], text)
        self.assertEqual(bytes.fromhex(result["private"]["private_observation"]["raw_body_hex"]), body)
        with sqlite3.connect(database) as connection:
            payload, original_hash = connection.execute(
                "SELECT payload,command_sha256 FROM events WHERE type='settle_attempt'").fetchone()
            self.assertGreater(len(payload.encode()), 1024 * 1024)
            self.assertLessEqual(len(payload.encode()), 4 * 1024 * 1024)
            self.assertEqual(original_hash, result["attempt"]["settlement_sha256"])
            self.assertEqual(json.loads(payload)["response"]["text"], text)

    def test_effective_live_capacity_can_persist_actual_limit_observation(self):
        # Legal SSE comment lines exceed the frozen shared raw budget. The
        # transport records its bounded prefix; Store must persist that exact
        # observed prefix rather than failing on a predictable row-size limit.
        body = (b":" + b"p" * 100000 + b"\n\n") * 33
        with self.peer(tls.fixed_response(body, content_type="text/event-stream")) as peer:
            result, _ = self.invoke(peer, stream=True)
            self.assertEqual(len(peer.requests), 1)
        self.assertEqual(result["attempt"]["accepted"], 0)
        observation = result["private"]["private_observation"]
        raw_body = bytes.fromhex(observation["raw_body_hex"])
        raw_headers = bytes.fromhex(observation["raw_headers_hex"])
        self.assertEqual(len(raw_body) + len(raw_headers), 3 * 1024 * 1024)
        self.assertEqual(raw_body, body[:len(raw_body)])
        self.assertTrue(result["safe"]["transport"]["raw_body_truncated"])
        self.assertIsNotNone(result["attempt"]["settlement_sha256"])
        self.assertEqual(result["budget"]["unknown_cost"], 100)

    def test_disabled_or_failed_admission_has_zero_network_and_budget_effect(self):
        for option in ("disabled", "admission_fault"):
            with self.subTest(option=option), self.peer() as peer:
                result, database = self.invoke(peer, **{option: True})
                self.assertTrue(result["threw"])
                self.assertEqual(peer.accepted, 0)
                self.assertEqual(result["budget"]["attempts"], 0)
                with sqlite3.connect(database) as connection:
                    self.assertEqual(connection.execute("SELECT count(*) FROM live_send_claims").fetchone()[0], 0)
                    self.assertEqual(connection.execute("SELECT count(*) FROM call_attempts").fetchone()[0], 0)

    def test_late_complete_response_keeps_original_identity_after_all_fences(self):
        for mutation in ("amend", "cancel", "recover", "policy"):
            with self.subTest(mutation=mutation):
                received = self.directory / f"received-{mutation}"
                release = self.directory / f"release-{mutation}"
                def delayed(peer, connection):
                    peer.request(connection)
                    received.write_text("received")
                    deadline = time.monotonic() + 8
                    while not release.exists() and not peer.stop.is_set():
                        if time.monotonic() >= deadline:
                            raise AssertionError("mutation fixture did not release response")
                        peer.stop.wait(.002)
                    body = tls.response_body()
                    connection.sendall(tls.headers(len(body)) + body)
                with self.peer(delayed) as peer:
                    result, _ = self.invoke(peer, action="late", mutation=mutation, received_marker=str(received),
                                            release_marker=str(release), reconcile=True)
                    self.assertEqual(len(peer.requests), 1)
                self.assert_original(result)
                self.assertEqual(result["attempt"]["status"], "STALE")
                self.assertEqual(result["attempt"]["accepted"], 0)
                after = result["after_reconcile"]
                self.assertEqual(after["attempt"], result["attempt"])
                self.assertEqual(after["budget"]["spent_cost"], 17)
                self.assertFalse(after["safe"]["accounting"]["business_authority_restored"])

    def test_actual_sigkill_at_five_boundaries_never_resends(self):
        for stage in ("reservation", "claim", "inflight", "transport", "observation"):
            received = self.directory / f"kill-received-{stage}"
            def inflight(peer, connection):
                peer.request(connection)
                received.write_text("original request observed")
                peer.stop.wait(8)
            handler = inflight if stage == "inflight" else None
            with self.subTest(stage=stage), self.peer(handler) as peer:
                action = "pause-transport" if stage == "inflight" else f"pause-{stage}"
                config, database = self.configuration(peer, action=action, continue_marker=str(self.directory / "never-release"))
                process = subprocess.Popen([str(DRIVER), str(config), str(SOURCE)], cwd=self.directory,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
                try:
                    if stage == "inflight":
                        deadline = time.monotonic() + 12
                        while not received.exists() and process.poll() is None and time.monotonic() < deadline:
                            time.sleep(.002)
                        self.assertTrue(received.exists(), "peer did not observe original request before SIGKILL")
                        self.assertIsNone(process.poll())
                    else:
                        with selectors.DefaultSelector() as selector:
                            selector.register(process.stdout, selectors.EVENT_READ)
                            self.assertTrue(selector.select(12), "child did not reach durable boundary")
                        ready = json.loads(process.stdout.readline())
                        self.assertEqual(ready["ready"], stage)
                    process.send_signal(signal.SIGKILL)
                    stdout, stderr = process.communicate(timeout=5)
                    self.assertEqual(process.returncode, -signal.SIGKILL)
                    self.assertEqual(stdout, "")
                    self.assertEqual(stderr, "")
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate(timeout=5)
                expected_sends = 0 if stage in ("reservation", "claim") else 1
                self.assertEqual(len(peer.requests), expected_sends)
                result, _ = self.invoke(database=database, action="recover", reconcile=True)
                self.assertEqual(len(peer.requests), expected_sends)
                self.assertTrue(result["send_forbidden"])
                self.assertEqual(result["attempt"]["accepted"], 0)
                self.assertTrue(result["safe"]["accounting"]["accounting_complete"])
                if stage != "observation":
                    self.assertEqual(result["attempt"]["response"]["status"], "interrupted")
                    self.assertIsNone(result["attempt"]["usage"]["cost_micro_usd"])
                if stage == "reservation":
                    self.assertIsNone(result["safe"]["send_claim"])
                    self.assertEqual(result["budget"]["spent_cost"], 0)
                    self.assertEqual(result["attempt"]["response"]["error_code"], "not_dispatched_after_recovery")
                else:
                    self.assertIsNotNone(result["safe"]["send_claim"])
                    self.assertEqual(result["budget"]["spent_cost"], 17)
                self.assertEqual(result["budget"]["reserved_cost"], 0)
                self.assertEqual(result["budget"]["attempts"], 1)


if __name__ == "__main__":
    previous = os.umask(0)
    os.umask(previous)
    print(f"Live Store loopback UID={os.getuid()} effective_umask={previous:04o}", flush=True)
    unittest.main(verbosity=2)
