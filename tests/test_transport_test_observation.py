#!/usr/bin/env python3
"""Lightweight tooling selfchecks; not a product CTest/sanitizer registration."""
from __future__ import annotations

import importlib.util
import io
import json
import os
from pathlib import Path
import ssl
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import transport_test_observation as observation

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("transport_diagnostic", ROOT / "scripts/diagnose_transport_failures.py")
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)


class ObservationSelfchecks(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="transport-observer-selfcheck-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "observation.jsonl"

    def enabled(self):
        observer = observation.Observation(self.path)
        self.addCleanup(observer.close)
        return observer

    def rows(self):
        # Read before close: each phase must have been flushed already.
        return [json.loads(line) for line in self.path.read_text().splitlines()]

    def fixture(self, observer):
        spec = importlib.util.spec_from_file_location("observed_tls_fixture", ROOT / "tests/test_https_transport.py")
        module = importlib.util.module_from_spec(spec)
        with mock.patch.object(sys, "argv", ["fixture", "/unused-driver", str(ROOT)]):
            spec.loader.exec_module(module)
        module.OBSERVATION = observer
        return module

    def test_observation_io_failure_cannot_skip_real_peer_teardown(self):
        observer = self.enabled()
        fixture = self.fixture(observer)
        peer = fixture.Peer(None)
        try:
            with mock.patch.object(observer.stream, "write", side_effect=OSError("synthetic observation ENOSPC")), \
                 mock.patch.object(sys, "stderr", io.StringIO()):
                peer.close()
            self.assertFalse(peer.thread.is_alive())
            self.assertEqual(peer.listener.fileno(), -1)
            self.assertTrue(observer.failed)
        finally:
            # Keep the red selfcheck bounded too: independent fixture cleanup
            # after the injected failure has unwound, never a product retry.
            fixture.OBSERVATION = observation.Observation()
            peer.close()

    def test_prefix_observation_io_failure_keeps_original_network_exception(self):
        observer = self.enabled()
        fixture = self.fixture(observer)
        case = fixture.HttpsTransport("test_partial_request_upload_deadline_is_not_free_retry_authority")
        captured = {}
        class CapturedHandler(Exception):
            pass
        def capture(handler, **_):
            captured["handler"] = handler
            raise CapturedHandler()
        with mock.patch.object(case, "peer", side_effect=capture), self.assertRaises(CapturedHandler):
            case.test_partial_request_upload_deadline_is_not_free_retry_authority()
        original = ConnectionError("original synthetic partial-upload failure")
        connection = mock.Mock()
        connection.recv.side_effect = [b"abc", original]
        write = observer.stream.write
        def fail_final_stage(text):
            if '"stage": "prefix_receive_end"' in text:
                raise OSError("synthetic observation ENOSPC")
            return write(text)
        with mock.patch.object(observer.stream, "write", side_effect=fail_final_stage), \
             mock.patch.object(sys, "stderr", io.StringIO()), self.assertRaises(ConnectionError) as caught:
            captured["handler"](SimpleNamespace(prefixes=[], stop=mock.Mock()), connection)
        self.assertIs(caught.exception, original)
        self.assertTrue(observer.failed)

    def test_collector_requires_complete_observation_and_refuses_io_warning(self):
        observer = self.enabled()
        stderr = Path(self.directory.name) / "stderr.log"
        stderr.write_bytes(b"")
        observer.record("peer_closed", accepted=0, requests=0, prefixes=0, errors=0, live_threads=0)
        self.assertFalse(diagnostic.observation_complete(self.path, stderr))
        observer.close()
        self.assertTrue(diagnostic.observation_complete(self.path, stderr))
        stderr.write_bytes(b"Transport test observation I/O failed; diagnostic evidence is incomplete.\n")
        self.assertFalse(diagnostic.observation_complete(self.path, stderr))
        stderr.write_bytes(b"")
        self.path.write_text('{"sequence":0,"stage":"observation_limit"}\n'
                             '{"sequence":1,"stage":"observation_finished"}\n')
        self.assertFalse(diagnostic.observation_complete(self.path, stderr))

    def test_default_off_has_no_file_clock_or_json_parse(self):
        with mock.patch.object(observation.os, "open", side_effect=AssertionError("file effect")), \
             mock.patch.object(observation.time, "monotonic_ns", side_effect=AssertionError("clock effect")), \
             mock.patch.object(observation.json, "loads", side_effect=AssertionError("parse effect")):
            observer = observation.Observation()
            observer.record("unregistered stage", secret="not inspected")
            with observer.subprocess(18) as returned:
                returned(subprocess.CompletedProcess(["secret"], 0, "not JSON", "secret"))
            observer.close()
        self.assertFalse(self.path.exists())

    def test_result_allowlist_redacts_dynamic_strings_and_preserves_status(self):
        observer = self.enabled()
        secret = "Bearer credential-do-not-log/private/path?token=secret"
        original = {"safe": {"transport": {
            "status": "deadline_exceeded", "phase": "response_body", "reason": "deadline",
            "elapsed_us": 3000000, "body_bytes_observed": 4096, "raw_body_truncated": False,
            "request_body_sha256": secret, "body": secret}},
            "attempt": {"accepted": 0, "status": "INTERRUPTED", "settlement_sha256": secret,
                        "response": {"status": "interrupted", "text": secret, "tool_calls": [],
                                     "error_code": secret, "error_message": secret}},
            "error": secret, "private": {"raw_body_hex": secret.encode().hex()},
            "budget": {"unknown_cost": 100}, "ledger": {"consistent": True}}
        process = subprocess.CompletedProcess([secret], 0, json.dumps(original), secret)
        stdout = process.stdout
        with observer.subprocess(18) as returned:
            returned(process)
        self.assertEqual(process.stdout, stdout)
        text = self.path.read_text()
        self.assertNotIn(secret, text)
        self.assertNotIn(secret.encode().hex(), text)
        self.assertNotIn("request_body_sha256", text)
        result = self.rows()[-1]
        self.assertEqual(result["status"], "deadline_exceeded")
        self.assertEqual(result["reason"], "deadline")
        self.assertEqual(result["attempt_status"], "INTERRUPTED")
        self.assertEqual(result["attempt_accepted"], 0)
        self.assertEqual(result["response_error_code"], "redacted")
        self.assertEqual(result["response_text_characters"], len(secret))
        self.assertTrue(result["settlement_present"])
        self.assertEqual(result["unknown_cost"], 100)

    def test_timeout_and_other_exceptions_propagate_as_original_objects(self):
        observer = self.enabled()
        for error in (subprocess.TimeoutExpired(["private-command"], 18, b"secret", b"secret"),
                      RuntimeError("private exception message")):
            with self.assertRaises(type(error)) as caught:
                with observer.subprocess(18):
                    raise error
            self.assertIs(caught.exception, error)
        rows = self.rows()
        self.assertEqual([row["stage"] for row in rows], ["subprocess_start", "subprocess_timeout",
                                                          "subprocess_start", "subprocess_exception"])
        self.assertEqual(rows[1]["timeout_s"], 18)
        self.assertEqual(rows[1]["stdout_bytes"], 6)
        self.assertNotIn("secret", self.path.read_text())
        self.assertNotIn("private", self.path.read_text())

    def test_private_immediate_stage_counts_and_bounded_output(self):
        previous = os.umask(0o002)
        try:
            observer = self.enabled()
        finally:
            os.umask(previous)
        observer.record("tls_start")
        observer.exception("peer_expected_exception", ssl.SSLError("private certificate details"))
        observer.record("prefix_receive_end", prefix_bytes=137)
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        rows = self.rows()
        self.assertEqual(rows[-1]["prefix_bytes"], 137)
        self.assertEqual(rows[1]["exception_kind"], "tls")
        self.assertEqual([row["monotonic_us"] for row in rows], sorted(row["monotonic_us"] for row in rows))
        for _ in range(600):
            observer.record("peer_listening")
        rows = self.rows()
        self.assertEqual(len(rows), 513)
        self.assertEqual(rows[-1]["stage"], "observation_limit")
        self.assertNotIn("private", self.path.read_text())

    def test_failed_observation_write_does_not_replace_original_timeout(self):
        observer = self.enabled()
        original = subprocess.TimeoutExpired(["private-command"], 18)
        record = observer.record
        def failed_timeout_record(stage, **values):
            if stage == "subprocess_timeout":
                raise OSError("observation unavailable")
            record(stage, **values)
        with mock.patch.object(observer, "record", side_effect=failed_timeout_record):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                with observer.subprocess(18):
                    raise original
        self.assertIs(caught.exception, original)

    def test_unknown_types_and_labels_cannot_copy_payloads(self):
        observer = self.enabled()
        observer.record("driver_result", reason="secret", status={"secret": 1},
                        http_status="secret", threw="secret", attempt_accepted=11)
        self.assertEqual(self.rows()[-1]["reason"], "redacted")
        self.assertNotIn("secret", self.path.read_text())
        self.assertNotIn("http_status", self.rows()[-1])
        with self.assertRaises(ValueError):
            observer.record("secret")
        with self.assertRaises(ValueError):
            observer.record("driver_result", secret="secret")

    def test_ast_identity_preserves_assertions_timeouts_and_method_order(self):
        original = Path(self.directory.name) / "original.py"
        candidate = Path(self.directory.name) / "candidate.py"
        original.write_text("""import subprocess
def test_first():
    process = subprocess.run(['driver'], timeout=18, check=False)
    assert process.returncode == 0
def test_second():
    assert True
""")
        candidate.write_text("""import subprocess
from transport_test_observation import OBSERVATION
def test_first():
    OBSERVATION.record('configuration_ready')
    with OBSERVATION.subprocess(18) as observe_return:
        process = subprocess.run(['driver'], timeout=18, check=False)
        observe_return(process)
    assert process.returncode == 0
def test_second():
    assert True
""")
        source = candidate.read_text()
        self.assertEqual(diagnostic.semantic_tree(original), diagnostic.semantic_tree(candidate))
        for before, after in (("timeout=18", "timeout=19"), ("== 0", "!= 0"),
                              ("test_second", "test_different")):
            candidate.write_text(source.replace(before, after))
            self.assertNotEqual(diagnostic.semantic_tree(original), diagnostic.semantic_tree(candidate))

    def test_clean_environment_preserves_original_sanitizers_without_secrets(self):
        with mock.patch.dict(os.environ, {"PROVIDER_TOKEN": "secret", "HTTPS_PROXY": "secret",
                                          "LSAN_OPTIONS": "suppressions=private", "LD_PRELOAD": "secret"}):
            environment = diagnostic.clean_environment(Path("/temporary"), self.path)
        for key in ("PROVIDER_TOKEN", "HTTPS_PROXY", "LSAN_OPTIONS", "LD_PRELOAD"):
            self.assertNotIn(key, environment)
        self.assertEqual(environment["ASAN_OPTIONS"], "halt_on_error=1:detect_leaks=1")
        self.assertEqual(environment["UBSAN_OPTIONS"], "halt_on_error=1:print_stacktrace=1")
        self.assertEqual(environment[observation.ENVIRONMENT], str(self.path))
        self.assertEqual(len(diagnostic.CASES), 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
