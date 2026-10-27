#!/usr/bin/env python3
"""Original Store + loopback TLS integration; never a provider acceptance test.

All database rows, endpoints, certificates, bearer values and usage evidence
are disposable synthetic fixtures. SIGKILL tests retain the original database
and recover through the real writer takeover and Store accounting APIs.
"""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import selectors
import signal
import sqlite3
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


class LiveStore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
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

    def assert_original(self, result):
        attempt = result["attempt"]
        response = attempt["response"]
        request = attempt["request_envelope"]
        for field in ("call_id", "attempt_id", "role", "role_call_index", "binding", "config_snapshot_id"):
            self.assertEqual(response[field], request[field])
        self.assertEqual(result["safe"]["execution"]["execution_kind"], "local_tls")
        self.assertFalse(result["safe"]["accounting"]["business_authority_restored"])

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
