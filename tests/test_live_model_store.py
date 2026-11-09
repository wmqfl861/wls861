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

    def compat_profile(self):
        path = self.directory / "compat-profile.json"
        if not path.exists():
            path.write_text(json.dumps({
                "format": "wls-responses-compat-v1",
                "reference": "openai-responses-2026-11-08",
                "model_aliases": {"o3": ["o3-2025-04-16"]}}))
            path.chmod(0o600)
        return path

    def run_raw(self, peer=None, **options):
        # Driver actions with their own result document (no ledger key).
        path, database = self.configuration(peer, **options)
        result = subprocess.run([str(DRIVER), str(path), str(SOURCE)], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=18, check=False)
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
            peer.request(connection)
            body = next(remaining)
            connection.sendall(tls.headers(len(body)) + body)

        with self.peer(sequenced) as peer:
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
            self.assertEqual(connection.execute("PRAGMA user_version").fetchone()[0], 410)
            self.assertEqual(connection.execute("SELECT count(*) FROM live_send_claims").fetchone()[0], 3)
            self.assertEqual(connection.execute("SELECT count(*) FROM ops").fetchone()[0], 0)
            self.assertEqual(connection.execute("SELECT count(*) FROM model_tool_feedback").fetchone()[0], 2)
            self.assertEqual(sorted(row[0] for row in connection.execute("SELECT attempt_id FROM model_tool_feedback")),
                             ["attempt-fixture", "attempt-fixture-2"])

    def test_readback_opaque_continuation_over_tls_keeps_native_order(self):
        # Frozen utf8-token-v1 readback + worker window + compatibility profile
        # in one admission; the second request is produced by the production
        # capture path, and the captured loopback body must equal the encode
        # of that capture with the opaque original and IDs unchanged.
        compat = self.compat_profile()
        first_body = (SOURCE / 'tests/fixtures/responses/reasoning-tool-response.json').read_bytes()
        second_body = (SOURCE / 'tests/fixtures/responses/reasoning-text-response.json').read_bytes()
        remaining = iter((first_body, second_body))

        def sequenced(peer, connection):
            peer.request(connection)
            body = next(remaining)
            connection.sendall(tls.headers(len(body)) + body)

        with self.peer(sequenced) as peer:
            result, database = self.run_raw(peer, action='readback-opaque',
                                            compat_profile=str(compat), readback=True)
            self.assertEqual(len(peer.requests), 2)
            captured = peer.requests[1]['body']
        self.assertEqual(captured.decode(), result['second_request_body'])
        original = json.loads(first_body)
        reasoning = original['output'][0]
        first = result['first_response']
        body = json.loads(result['second_request_body'])
        self.assertEqual(body['model'], 'o3')
        self.assertEqual(body['include'], ['reasoning.encrypted_content'])
        items = body['input']
        self.assertEqual(len(items), 4)  # task contract, reasoning, call, result
        self.assertEqual(items[1], first['responses_output']['output'][0])
        self.assertEqual(items[1], reasoning)  # opaque original replayed unchanged
        self.assertEqual(items[2]['type'], 'function_call')
        self.assertEqual(items[2]['call_id'], 'call_compat_fixture')
        self.assertEqual(items[3]['type'], 'function_call_output')
        self.assertEqual(items[3]['call_id'], 'call_compat_fixture')
        self.assertEqual(json.loads(items[3]['output']), {'ok': True, 'round': 1})
        self.assertFalse(any(item.get('role') == 'assistant' for item in items))
        # The production capture measured both logical envelopes; carrying the
        # opaque continuation must cost bytes over the first envelope.
        self.assertGreater(result['second_envelope_bytes'], result['first_envelope_bytes'])
        self.assertEqual(result['first_settlement']['accepted'], 1)
        self.assertEqual(result['second_settlement']['accepted'], 1)
        self.assertEqual(result['budget']['attempts'], 2)
        with sqlite3.connect(database) as connection:
            self.assertEqual(connection.execute('SELECT count(*) FROM live_send_claims').fetchone()[0], 2)
            self.assertEqual(connection.execute('SELECT count(*) FROM model_tool_feedback').fetchone()[0], 1)

    def test_readback_opaque_low_budget_quota_never_attempts_or_sends(self):
        compat = self.compat_profile()
        with self.peer(tls.fixed_response(tls.response_body())) as peer:
            result, database = self.run_raw(peer, action='readback-opaque',
                                            compat_profile=str(compat), readback=True,
                                            quota=True, input_budget=1)
            self.assertEqual(peer.accepted, 0)
        self.assertTrue(result['quota'])
        self.assertEqual(result['budget']['attempts'], 0)
        self.assertEqual(result['send_claims'], '0')
        with sqlite3.connect(database) as connection:
            self.assertEqual(connection.execute('SELECT count(*) FROM call_attempts').fetchone()[0], 0)
            self.assertEqual(connection.execute('SELECT count(*) FROM live_send_claims').fetchone()[0], 0)

    def test_readback_config_late_response_fences_preserve_original_identity(self):
        # The live AMEND/cancel/policy/recover fences keep their exact original
        # semantics under a frozen readback configuration as well.
        for mutation in ('amend', 'cancel', 'policy', 'recover'):
            with self.subTest(mutation=mutation):
                received = self.directory / f'readback-received-{mutation}'
                release = self.directory / f'readback-release-{mutation}'
                def delayed(peer, connection, received=received, release=release):
                    peer.request(connection)
                    received.write_text('received')
                    deadline = time.monotonic() + 8
                    while not release.exists() and not peer.stop.is_set():
                        if time.monotonic() >= deadline:
                            raise AssertionError('mutation fixture did not release response')
                        peer.stop.wait(.002)
                    body = tls.response_body()
                    connection.sendall(tls.headers(len(body)) + body)
                with self.peer(delayed) as peer:
                    result, _ = self.invoke(peer, action='late', mutation=mutation,
                                            received_marker=str(received), release_marker=str(release),
                                            readback=True, reconcile=True)
                    self.assertEqual(len(peer.requests), 1)
                self.assert_original(result)
                self.assertEqual(result['attempt']['status'], 'STALE')
                self.assertEqual(result['attempt']['accepted'], 0)
                after = result['after_reconcile']
                self.assertEqual(after['attempt'], result['attempt'])
                self.assertEqual(after['budget']['spent_cost'], 17)
                self.assertFalse(after['safe']['accounting']['business_authority_restored'])

    def test_readback_config_exact_replay_never_resends(self):
        with self.peer(tls.fixed_response(tls.response_body())) as peer:
            result, _ = self.invoke(peer, readback=True, reconcile=True)
            self.assertEqual(len(peer.requests), 1)
        self.assertTrue(result['first_sent'])
        self.assertFalse(result['replay_sent'])
        self.assertFalse(result['replay_new_send_authority'])
        self.assertEqual(result['budget']['attempts'], 1)

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
