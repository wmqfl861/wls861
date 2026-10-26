#!/usr/bin/env python3
"""Offline protocol CLI integration; all provider observations are fixtures."""
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

if len(sys.argv) != 3:
    raise SystemExit("usage: test_protocol_replay_cli.py WLS SOURCE")
BINARY, SOURCE = (Path(x).resolve() for x in sys.argv[1:])
FIXTURES = SOURCE / "tests/fixtures/responses"
PROFILE = SOURCE / "config/examples/providers/responses-offline.json"
REQUEST = FIXTURES / "request.json"
sys.argv = [sys.argv[0]]


class ProtocolCli(unittest.TestCase):
    def setUp(self):
        self.assertNotEqual(os.getuid(), 0)
        self.temporary = tempfile.TemporaryDirectory(prefix="wls-protocol-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.directory.chmod(0o700)
        self.request = json.loads(REQUEST.read_text())
        self.profile = json.loads(PROFILE.read_text())
        self.terminal = json.loads((FIXTURES / "http-response.json").read_text())

    def invoke(self, command, *args, expected=0, profile=PROFILE, request=REQUEST, env=None):
        process = subprocess.run(
            [str(BINARY), command, "--profile", str(profile), "--request", str(request),
             *(str(x) for x in args)],
            cwd=self.directory, capture_output=True, text=True, encoding="utf-8",
            timeout=30, check=False, env=env)
        self.assertEqual(process.returncode, expected, process.stderr or process.stdout)
        if expected in (0, 4):
            self.assertEqual(process.stderr, "")
            report = json.loads(process.stdout)
            self.assertEqual(report["network_requests"], 0)
            self.assertEqual(report["paid_attempts"], 0)
            self.assertEqual(report["tool_dispatches"], 0)
            self.assertFalse(report["credentials_resolved"])
            self.assertFalse(report["store_opened"])
            return report
        self.assertEqual(process.stdout, "")
        return json.loads(process.stderr)

    def file(self, name, content):
        path = self.directory / name
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content if isinstance(content, str) else
                            json.dumps(content, ensure_ascii=False))
        path.chmod(0o600)
        return path

    def state(self):
        return {p.name: (stat.S_IMODE(p.lstat().st_mode), hashlib.sha256(p.read_bytes()).hexdigest())
                for p in self.directory.iterdir() if p.is_file() and not p.is_symlink()}

    def host_tuple(self, response):
        for key in ("call_id", "attempt_id", "role", "role_call_index", "binding", "config_snapshot_id"):
            self.assertEqual(response[key], self.request[key])

    def test_encode_preserves_body_fingerprints_and_does_not_resolve_credentials(self):
        environment = dict(os.environ, WLS_OFFLINE_FIXTURE_API_KEY="UNREAD_OFFLINE_CANARY")
        before = self.state()
        report = self.invoke("encode-model", "--stream", "true", env=environment)
        encoded = report["encoded_request"]
        body = json.loads(encoded["body"])
        self.assertEqual(body["model"], self.profile["model"])
        self.assertTrue(body["stream"])
        self.assertFalse(body["store"])
        self.assertFalse(body["background"])
        self.assertEqual(body["truncation"], "disabled")
        self.assertEqual(encoded["method"], "POST")
        self.assertEqual(encoded["url"], self.profile["base_url"] + self.profile["responses_path"])
        self.assertEqual(encoded["body_sha256"], hashlib.sha256(encoded["body"].encode()).hexdigest())
        self.assertEqual(encoded["logical_request_sha256"], self.request["logical_request_sha256"])
        self.assertEqual(encoded["record_integrity"], "local_consistency_only")
        self.assertFalse(encoded["evidence"]["network_dispatched"])
        self.assertIsNone(encoded["evidence"]["wire_request_sha256"])
        self.assertNotIn("UNREAD_OFFLINE_CANARY", json.dumps(report))
        self.assertEqual(before, self.state())

    def test_complete_http_response_keeps_host_tuple_and_raw_bytes(self):
        report = self.invoke("replay-model", "--http-response", FIXTURES / "http-response.json")
        result = report["result"]
        response = result["response"]
        self.host_tuple(response)
        self.assertEqual(response["status"], "completed")
        self.assertEqual(response["text"], self.terminal["output"][0]["content"][0]["text"])
        self.assertIsNone(response["usage"]["cost_micro_usd"])
        self.assertEqual(bytes.fromhex(result["raw_body_hex"]), (FIXTURES / "http-response.json").read_bytes())
        self.assertFalse(result["evidence"]["business_accepted"])
        self.assertFalse(result["evidence"]["tool_dispatch_performed"])

    def test_function_response_is_decoded_without_dispatch(self):
        before = self.state()
        report = self.invoke("replay-model", "--http-response", FIXTURES / "tool-response.json")
        self.host_tuple(report["result"]["response"])
        calls = report["result"]["response"]["tool_calls"]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["call_id"], "call_provider_fixture")
        self.assertEqual(calls[0]["arguments"], {"message": "fixture"})
        self.assertEqual(before, self.state())

    def test_http_errors_are_reports_with_no_retry_or_usable_tools(self):
        path = self.file("error.json", {"error": {"code": "fixture_error", "message": "fixture"}})
        for status in (429, 500, 503):
            report = self.invoke("replay-model", "--http-response", path,
                                 "--http-status", status, expected=4)
            response = report["result"]["response"]
            self.host_tuple(response)
            self.assertEqual(response["status"], "error")
            self.assertEqual(response["tool_calls"], [])
            self.assertEqual(response["text"], "")
            self.assertIsNone(response["usage"]["cost_micro_usd"])

    def test_missing_usage_stays_null_and_contradictory_partial_usage_refuses(self):
        value = copy.deepcopy(self.terminal)
        value.pop("usage")
        path = self.file("usage.json", value)
        response = self.invoke("replay-model", "--http-response", path)["result"]["response"]
        self.assertTrue(all(v is None for v in response["usage"].values()))
        for usage in ({"output_tokens": 3, "total_tokens": 2}, {"input_tokens": True},
                      {"input_tokens": -1}, {"input_tokens": 1 << 64}):
            value["usage"] = usage
            path = self.file("usage.json", value)
            response = self.invoke("replay-model", "--http-response", path, expected=4)["result"]["response"]
            self.assertEqual(response["status"], "error")
            self.assertEqual(response["tool_calls"], [])

    def test_sse_one_byte_reads_match_http_result(self):
        http = self.invoke("replay-model", "--http-response", FIXTURES / "http-response.json")
        stream = self.invoke("replay-model", "--sse-stream", FIXTURES / "text.sse", "--chunk-bytes", 1)
        result = stream["result"]
        self.assertEqual(result["response"], http["result"]["response"])
        self.assertTrue(result["evidence"]["stream_complete"])
        self.assertTrue(result["evidence"]["response_usable"])
        self.assertEqual(base64.b64decode(result["raw_stream_base64"]), (FIXTURES / "text.sse").read_bytes())

    def test_explicit_cancel_and_exact_deadline_preserve_tuple(self):
        cancelled = self.invoke("replay-model", "--replay", FIXTURES / "cancel.json", expected=4)
        response = cancelled["result"]["response"]
        self.host_tuple(response)
        self.assertEqual(response["status"], "interrupted")
        self.assertEqual(response["error_code"], "cancelled")
        self.assertEqual(response["tool_calls"], [])
        replay = self.file("deadline.json", {"format": "wls-sse-replay-v1", "chunks": [
            {"data": (FIXTURES / "text.sse").read_text(),
             "elapsed_ms": self.request["timeout_ms"], "cancel_before": False}]})
        response = self.invoke("replay-model", "--replay", replay, expected=4)["result"]["response"]
        self.host_tuple(response)
        self.assertEqual(response["error_code"], "timeout")
        self.assertEqual(response["tool_calls"], [])

    def test_interrupted_and_trailing_streams_never_dispatch(self):
        stream = (FIXTURES / "text.sse").read_text()
        partial = self.file("partial.sse", stream.split("event: response.completed")[0])
        response = self.invoke("replay-model", "--sse-stream", partial, expected=4)["result"]["response"]
        self.assertEqual(response["status"], "interrupted")
        self.assertEqual(response["tool_calls"], [])
        trailing = self.file("trailing.sse", stream + 'data: {"type":"unexpected"}\n\n')
        report = self.invoke("replay-model", "--sse-stream", trailing, expected=4)
        response = report["result"]["response"]
        self.assertEqual(response["status"], "error")
        self.assertEqual(response["text"], "")
        self.assertEqual(response["tool_calls"], [])
        self.assertEqual(response["usage"]["input_tokens"], 9)
        self.assertTrue(report["result"]["evidence"]["terminal_usage_preserved_after_stream_error"])

    def test_invalid_options_do_not_read_inputs_or_open_store(self):
        absent = self.directory / "no-request.json"
        database = self.directory / "no-database.sqlite"
        for args in (("--db", database), ("--stream", "maybe")):
            error = self.invoke("encode-model", *args, request=absent, expected=2)
            self.assertEqual(error["error"], "invalid_input")
        for args in ((), ("--http-response", "a", "--sse-stream", "b"),
                     ("--http-response", "a", "--chunk-bytes", 1),
                     ("--sse-stream", "a", "--http-status", 200),
                     ("--sse-stream", "a", "--chunk-bytes", 0)):
            error = self.invoke("replay-model", *args, request=absent, expected=2)
            self.assertEqual(error["error"], "invalid_input")
        self.assertFalse(database.exists())
        self.assertFalse(Path(str(database) + ".writer-lock").exists())

    def test_profile_misconfiguration_and_modified_request_refuse(self):
        for update in ({"real_calls_enabled": True}, {"max_paid_retries": 1},
                       {"api_key": "never-allowed"}, {"base_url": "https://name:password@example.invalid"}):
            value = dict(self.profile, **update)
            path = self.file("profile.json", value)
            error = self.invoke("encode-model", profile=path, expected=2)
            self.assertIn(error["error"], ("invalid_input", "unsupported"))
        changed = copy.deepcopy(self.request)
        changed["max_output_tokens"] -= 1
        path = self.file("request.json", changed)
        self.assertEqual(self.invoke("encode-model", request=path, expected=2)["error"], "invalid_input")

    def test_bounded_reads_and_invalid_replay_shape(self):
        profile = copy.deepcopy(self.profile)
        profile["limits"]["max_response_bytes"] = 16
        path = self.file("profile.json", profile)
        self.assertEqual(self.invoke("replay-model", "--http-response", FIXTURES / "http-response.json",
                                    profile=path, expected=2)["error"], "invalid_input")
        for value in ({"format": "wrong", "chunks": []},
                      {"format": "wls-sse-replay-v1", "chunks": [{"data": "", "elapsed_ms": -1, "cancel_before": False}]},
                      {"format": "wls-sse-replay-v1", "chunks": [{"data": "", "elapsed_ms": 0, "cancel_before": False, "extra": 1}]}):
            path = self.file("replay.json", value)
            self.assertEqual(self.invoke("replay-model", "--replay", path, expected=2)["error"], "invalid_input")
        duplicate = self.file("duplicate.json", '{"format":"wls-sse-replay-v1","format":"wls-sse-replay-v1","chunks":[]}')
        self.assertEqual(self.invoke("replay-model", "--replay", duplicate, expected=2)["error"], "invalid_input")

    def test_input_symlinks_refuse_and_invalid_utf8_is_lossless_response_evidence(self):
        link = self.directory / "linked-profile"
        link.symlink_to(PROFILE)
        self.assertEqual(self.invoke("encode-model", profile=link, expected=2)["error"], "invalid_input")
        raw = b"\xff\x00invalid response"
        path = self.file("invalid-body", raw)
        report = self.invoke("replay-model", "--http-response", path, expected=4)
        self.assertEqual(bytes.fromhex(report["result"]["raw_body_hex"]), raw)
        self.assertEqual(report["result"]["response"]["tool_calls"], [])

    def test_invalid_path_bytes_and_oversized_paths_have_bounded_json_errors(self):
        invalid = os.fsdecode(os.fsencode(self.directory) + b"/missing-\xff")
        before = self.state()
        for path in (invalid, "x" * 4097):
            self.assertEqual(self.invoke("encode-model", profile=path, expected=2)["error"], "invalid_input")
            self.assertEqual(self.invoke("encode-model", request=path, expected=2)["error"], "invalid_input")
            for option in ("--http-response", "--sse-stream", "--replay"):
                self.assertEqual(self.invoke("replay-model", option, path, expected=2)["error"], "invalid_input")
        self.assertEqual(self.invoke("encode-model", "--source-root", self.directory,
                                     expected=2)["error"], "invalid_input")
        self.assertEqual(self.state(), before)


if __name__ == "__main__":
    previous = os.umask(0)
    os.umask(previous)
    print(f"protocol CLI fixture UID={os.getuid()} effective_umask={previous:04o}", flush=True)
    unittest.main(verbosity=2)
