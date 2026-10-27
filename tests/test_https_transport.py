#!/usr/bin/env python3
"""Ordinary-user HTTPS component tests against disposable loopback TLS peers.

The peers, certificates, credentials, requests and responses are synthetic.
No provider is contacted; local upload observations are not billing evidence.
"""
from __future__ import annotations

import contextlib
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest

if len(sys.argv) != 3:
    raise SystemExit("usage: test_https_transport.py DRIVER SOURCE")
DRIVER, SOURCE = (Path(value).resolve() for value in sys.argv[1:])
sys.argv = [sys.argv[0]]
FIXTURES = SOURCE / "tests/fixtures/responses"
BEARER = "WLS_SYNTHETIC_BEARER_ONLY_31A7"


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


def response_body(text="loopback 完成😀", *, tool=False):
    name = "tool-response.json" if tool else "http-response.json"
    value = json.loads((FIXTURES / name).read_text())
    if not tool:
        value["output"][0]["content"][0]["text"] = text
    return encoded(value)


def tool_stream():
    terminal = json.loads(response_body(tool=True))
    start = dict(terminal, status="in_progress", output=[], usage=None)
    item = terminal["output"][0]
    arguments = item["arguments"]
    events = [
        ("response.created", {"response": start}),
        ("response.output_item.added", {"output_index": 0,
            "item": dict(item, status="in_progress", arguments="")}),
        ("response.function_call_arguments.delta", {"output_index": 0,
            "item_id": item["id"], "delta": arguments[:8]}),
        ("response.function_call_arguments.delta", {"output_index": 0,
            "item_id": item["id"], "delta": arguments[8:]}),
        ("response.function_call_arguments.done", {"output_index": 0,
            "item_id": item["id"], "arguments": arguments}),
        ("response.output_item.done", {"output_index": 0, "item": item}),
        ("response.completed", {"response": terminal}),
    ]
    return b"".join(b"event: " + kind.encode() + b"\r\ndata: " +
                    encoded(dict(data, type=kind, sequence_number=index)) + b"\r\n\r\n"
                    for index, (kind, data) in enumerate(events))


class Peer:
    """Owned listener and connection threads; teardown closes every socket."""

    def __init__(self, context, handler=None, *, stall_tls=False, receive_buffer=None,
                 handshake_delay=0):
        self.context = context
        self.handler = handler
        self.stall_tls = stall_tls
        self.handshake_delay = handshake_delay
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.accepted = 0
        self.requests = []
        self.prefixes = []
        self.errors = []
        self.sockets = []
        self.workers = []
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if receive_buffer:
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.listener.bind(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(8)
        self.listener.settimeout(0.05)
        self.thread = threading.Thread(target=self._listen, name="wls-fixture-listener")
        self.thread.start()

    def _listen(self):
        while not self.stop.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self.lock:
                self.accepted += 1
                self.sockets.append(connection)
            worker = threading.Thread(target=self._serve, args=(connection,), name="wls-fixture-peer")
            self.workers.append(worker)
            worker.start()

    def _serve(self, connection):
        try:
            connection.settimeout(8)
            if self.stall_tls:
                self.stop.wait(8)
                return
            if self.handshake_delay:
                self.stop.wait(self.handshake_delay)
            stream = self.context.wrap_socket(connection, server_side=True)
            with self.lock:
                self.sockets.append(stream)
            with stream:
                self.handler(self, stream)
        except (ssl.SSLError, ConnectionError, socket.timeout, OSError):
            # Expected for certificate refusal, cancellation, deadlines and
            # deliberately truncated replies. Assertions still check results.
            pass
        except BaseException as error:
            self.errors.append(repr(error))
        finally:
            with contextlib.suppress(OSError):
                connection.close()

    def request(self, connection):
        received = bytearray()
        while b"\r\n\r\n" not in received:
            chunk = connection.recv(4096)
            if not chunk:
                raise ConnectionError("request headers interrupted")
            received.extend(chunk)
            if len(received) > 65536:
                raise AssertionError("fixture request header bound")
        head, _, body = bytes(received).partition(b"\r\n\r\n")
        headers = {}
        lines = head.split(b"\r\n")
        for line in lines[1:]:
            name, value = line.split(b":", 1)
            headers[name.lower()] = value.strip()
        length = int(headers[b"content-length"])
        if length > 4 * 1024 * 1024:
            raise AssertionError("fixture request body bound")
        while len(body) < length:
            chunk = connection.recv(min(65536, length - len(body)))
            if not chunk:
                raise ConnectionError("request body interrupted")
            body += chunk
        observation = {"line": lines[0], "headers": headers, "body": body}
        self.requests.append(observation)
        return observation

    def close(self):
        self.stop.set()
        self.listener.close()
        # First stop admission so no newly accepted descriptor can appear
        # after the active-socket teardown snapshot.
        self.thread.join(2)
        with self.lock:
            for connection in self.sockets:
                with contextlib.suppress(OSError):
                    connection.shutdown(socket.SHUT_RDWR)
                with contextlib.suppress(OSError):
                    connection.close()
        for worker in self.workers:
            worker.join(2)
        if self.thread.is_alive() or any(worker.is_alive() for worker in self.workers):
            raise AssertionError("fixture leaked a connection thread")
        if self.errors:
            raise AssertionError(self.errors)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def headers(length=None, *, status=200, content_type="application/json", extra=b"", chunked=False):
    result = f"HTTP/1.1 {status} Fixture\r\nContent-Type: {content_type}\r\nConnection: close\r\n".encode()
    if chunked:
        result += b"Transfer-Encoding: chunked\r\n"
    elif length is not None:
        result += f"Content-Length: {length}\r\n".encode()
    return result + extra + b"\r\n"


def fixed_response(body, **options):
    def handler(peer, connection):
        peer.request(connection)
        connection.sendall(headers(len(body), **options) + body)
    return handler


class HttpsTransport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.getuid() == 0:
            raise AssertionError("HTTPS integration must run as an ordinary user")
        cls.certificate_directory = tempfile.TemporaryDirectory(prefix="wls-tls-certificates-")
        cls.addClassCleanup(cls.certificate_directory.cleanup)
        cls.certificates = Path(cls.certificate_directory.name)
        cls.certificates.chmod(0o700)

        def openssl(*arguments):
            subprocess.run(["openssl", *arguments], cwd=cls.certificates, check=True,
                           capture_output=True, timeout=15)

        for stem in ("ca", "untrusted"):
            openssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                    "-nodes", "-keyout", f"{stem}.key", "-out", f"{stem}.pem", "-days", "1",
                    "-subj", f"/CN=WLS disposable {stem}",
                    "-addext", "basicConstraints=critical,CA:TRUE",
                    "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        for stem, names in (("server", "DNS:localhost,IP:127.0.0.1"),
                            ("wrong-host", "DNS:wrong-fixture.invalid")):
            openssl("req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                    "-nodes", "-keyout", f"{stem}.key", "-out", f"{stem}.csr",
                    "-subj", "/CN=WLS disposable server")
            (cls.certificates / f"{stem}.ext").write_text(
                f"subjectAltName={names}\nbasicConstraints=critical,CA:FALSE\n"
                "keyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n")
            openssl("x509", "-req", "-in", f"{stem}.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
                    "-CAcreateserial", "-out", f"{stem}.pem", "-days", "1", "-extfile", f"{stem}.ext")
        for path in cls.certificates.iterdir():
            path.chmod(0o600)
        cls.contexts = {}
        for stem in ("server", "wrong-host"):
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(cls.certificates / f"{stem}.pem", cls.certificates / f"{stem}.key")
            cls.contexts[stem] = context

    @classmethod
    def tearDownClass(cls):
        cls.certificate_directory.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="wls-https-fixture-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.directory.chmod(0o700)
        self.invocations = 0

    def peer(self, handler=None, **options):
        context = self.contexts[options.pop("certificate", "server")]
        return Peer(context, handler or fixed_response(response_body()), **options)

    def invoke(self, peer, *, environment=None, **configuration):
        self.invocations += 1
        config = {"port": peer.port, "ca_path": str(self.certificates / "ca.pem"), **configuration}
        path = self.directory / f"call-{self.invocations}.json"
        path.write_text(json.dumps(config))
        path.chmod(0o600)
        started = time.monotonic()
        process = subprocess.run([str(DRIVER), str(path)], cwd=self.directory,
                                 capture_output=True, text=True, encoding="utf-8", timeout=15,
                                 check=False, env=environment)
        self.assertEqual(process.returncode, 0, process.stderr or process.stdout)
        self.assertEqual(process.stderr, "")
        result = json.loads(process.stdout)
        self.assertLess(time.monotonic() - started, 12)
        if not result.get("threw"):
            self.assert_original_tuple(result)
            self.assert_observations(result)
        return result

    def assert_original_tuple(self, result):
        response = result["response"]
        for field in ("call_id", "attempt_id", "binding", "config_snapshot_id"):
            self.assertEqual(response[field], result[f"expected_{field}"])
        self.assertEqual(response["role"], "worker")
        self.assertEqual(response["role_call_index"], 1)

    def assert_observations(self, result):
        self.assertGreaterEqual(result["elapsed_us"], 0)
        self.assertEqual(len(result["record_sha256"]), 64)
        self.assertLessEqual(len(bytes.fromhex(result["raw_headers_hex"])), result["header_bytes_observed"])
        self.assertLessEqual(len(bytes.fromhex(result["raw_body_hex"])), result["body_bytes_observed"])
        self.assertLessEqual(result["request_body_bytes_supplied"], result["request_body_size"])
        self.assertLessEqual(result["uploaded_body_bytes_reported"], result["request_body_size"])
        observed_times = [result[key] for key in ("connected_at_us", "tls_verified_at_us",
                          "first_header_at_us", "first_body_at_us") if result[key] is not None]
        self.assertTrue(all(0 <= stamp <= result["elapsed_us"] for stamp in observed_times), result)
        self.assertEqual(observed_times, sorted(observed_times))
        serialized = json.dumps(result["summary"], ensure_ascii=False)
        self.assertNotIn(BEARER, serialized)
        self.assertNotIn(BEARER.encode().hex(), serialized)
        for key in ("encoded_record_sha256", "request_body_sha256", "profile_sha256",
                    "logical_request_sha256", "ca_bundle_sha256", "transport_options_sha256"):
            self.assertIn(result[key], serialized)

    def assert_failed(self, result, expected=None):
        self.assertFalse(result.get("threw"), result)
        if expected:
            self.assertEqual(result["status"], expected, result)
        else:
            self.assertNotEqual(result["status"], "completed")
        self.assertNotEqual(result["response"]["status"], "completed")
        self.assertEqual(result["response"]["text"], "")
        self.assertEqual(result["response"]["tool_calls"], [])
        self.assertIsNone(result["response"]["usage"]["cost_micro_usd"])

    def test_json_success_preserves_original_bytes_upload_and_explicit_request(self):
        body = response_body()
        with self.peer(fixed_response(body)) as peer:
            result = self.invoke(peer)
        self.assertEqual(result["status"], "completed")
        self.assertEqual(result["response"]["text"], "loopback 完成😀")
        self.assertEqual(bytes.fromhex(result["raw_body_hex"]), body)
        self.assertEqual(result["body_bytes_observed"], len(body))
        self.assertTrue(result["transfer_complete"])
        self.assertTrue(result["upload_may_have_occurred"])
        self.assertEqual(result["uploaded_body_bytes_reported"], result["request_body_size"])
        self.assertEqual(result["request_body_bytes_supplied"], result["request_body_size"])
        self.assertEqual(peer.accepted, 1)
        self.assertEqual(len(peer.requests), 1)
        sent = peer.requests[0]
        self.assertEqual(sent["line"], b"POST /v1/responses HTTP/1.1")
        self.assertEqual(sent["headers"][b"authorization"], f"Bearer {BEARER}".encode())
        self.assertEqual(hashlib.sha256(sent["body"]).hexdigest(), result["request_body_sha256"])
        request = json.loads(sent["body"])
        self.assertEqual(request["model"], "fixture-responses-model")
        self.assertFalse(request["background"])
        self.assertFalse(request["store"])
        self.assertEqual(request["truncation"], "disabled")

    def test_native_tool_is_returned_without_dispatch(self):
        with self.peer(fixed_response(response_body(tool=True))) as peer:
            result = self.invoke(peer)
        self.assertEqual(result["status"], "completed")
        calls = result["response"]["tool_calls"]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["call_id"], "call_provider_fixture")
        self.assertEqual(calls[0]["arguments"], {"message": "fixture"})
        self.assertEqual(set(p.name for p in self.directory.iterdir()), {"call-1.json"})

    def test_sse_chunked_utf8_boundaries_keep_exact_entity_bytes(self):
        body = (FIXTURES / "text.sse").read_bytes()
        split = body.index("离".encode()) + 1
        chunks = (body[:split], body[split:split + 1], body[split + 1:])

        def serve(peer, connection):
            peer.request(connection)
            connection.sendall(headers(content_type="text/event-stream", chunked=True))
            for chunk in chunks:
                connection.sendall(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            connection.sendall(b"0\r\n\r\n")

        with self.peer(serve) as peer:
            result = self.invoke(peer, stream=True)
        self.assertEqual(result["status"], "completed")
        self.assertEqual(result["response"]["text"], "离线协议回放完成。")
        self.assertEqual(bytes.fromhex(result["raw_body_hex"]), body)
        self.assertEqual(result["body_bytes_observed"], len(body))

    def test_sse_tool_completion(self):
        body = tool_stream()
        with self.peer(fixed_response(body, content_type="text/event-stream")) as peer:
            result = self.invoke(peer, stream=True)
        self.assertEqual(result["status"], "completed")
        self.assertEqual(len(result["response"]["tool_calls"]), 1)

    def test_wider_profile_stream_limit_does_not_reject_small_legal_response(self):
        body = tool_stream()
        with self.peer(fixed_response(body, content_type="text/event-stream")) as peer:
            result = self.invoke(peer, stream=True, max_stream_bytes=8 * 1024 * 1024)
        self.assertEqual(result["status"], "completed")
        self.assertEqual(bytes.fromhex(result["raw_body_hex"]), body)

    def test_hostname_uses_explicit_ca_and_hostname_verification(self):
        with self.peer() as peer:
            result = self.invoke(peer, host="localhost")
        self.assertEqual(result["status"], "completed")
        self.assertIsNotNone(result["tls_verified_at_us"])
        with self.peer(certificate="wrong-host") as peer:
            result = self.invoke(peer)
        self.assert_failed(result, "certificate_error")
        self.assertEqual(len(peer.requests), 0)
        self.assertEqual(result["uploaded_body_bytes_reported"], 0)

    def test_untrusted_certificate_has_zero_http_requests(self):
        with self.peer() as peer:
            result = self.invoke(peer, ca_path=str(self.certificates / "untrusted.pem"))
        self.assert_failed(result, "certificate_error")
        self.assertEqual(len(peer.requests), 0)
        self.assertIsNone(result["tls_verified_at_us"])
        self.assertEqual(result["request_body_bytes_supplied"], 0)

    def test_invalid_or_inconsistent_routes_refuse_before_connection(self):
        for route in ("invalid", "newline", "inconsistent"):
            with self.subTest(route=route), self.peer() as peer:
                result = self.invoke(peer, route_kind=route)
                self.assertTrue(result.get("threw"), result)
                self.assertEqual(peer.accepted, 0)

    def test_hostname_route_still_checks_original_hostname(self):
        with self.peer(certificate="wrong-host") as peer:
            result = self.invoke(peer, host="localhost", route_kind="loopback")
        self.assert_failed(result, "certificate_error")
        self.assertEqual(len(peer.requests), 0)

    def test_unmapped_hostname_requires_bounded_cares_resolver(self):
        # localhost is the only unbound name allowed by this test driver; no
        # external domain is queried. Threaded getaddrinfo builds must refuse
        # before connecting. A c-ares build may resolve this loopback hostname.
        with self.peer() as peer:
            result = self.invoke(peer, host="localhost", route_kind="none")
        if result["cares_resolver"]:
            self.assertEqual(result["status"], "completed")
            self.assertEqual(len(peer.requests), 1)
        else:
            self.assertTrue(result.get("threw"), result)
            self.assertEqual(result["error"], "unsupported")
            self.assertEqual(peer.accepted, 0)

    def test_http_errors_never_retry(self):
        for code in (429, 500, 503):
            with self.subTest(status=code):
                body = encoded({"error": {"message": "synthetic unavailable", "code": "fixture"}})
                with self.peer(fixed_response(body, status=code, extra=b"Retry-After: 0\r\n")) as peer:
                    result = self.invoke(peer)
                self.assert_failed(result, "http_error")
                self.assertEqual(result["http_status"], code)
                self.assertEqual(bytes.fromhex(result["raw_body_hex"]), body)
                self.assertEqual(peer.accepted, 1)
                self.assertEqual(len(peer.requests), 1)

    def test_redirect_never_contacts_target_or_forwards_credentials(self):
        with self.peer() as target:
            extra = f"Location: https://127.0.0.1:{target.port}/v1/responses\r\n".encode()
            with self.peer(fixed_response(b"{}", status=307, extra=extra)) as peer:
                result = self.invoke(peer)
            self.assert_failed(result, "http_error")
            self.assertEqual(target.accepted, 0)
        self.assertEqual(peer.accepted, 1)

    def test_proxy_environment_and_credential_reference_are_not_used(self):
        with self.peer() as proxy, self.peer() as peer:
            address = f"http://127.0.0.1:{proxy.port}"
            environment = dict(os.environ, https_proxy=address, HTTPS_PROXY=address,
                               all_proxy=address, ALL_PROXY=address, no_proxy="", NO_PROXY="",
                               WLS_OFFLINE_FIXTURE_API_KEY="UNREAD_ENVIRONMENT_CANARY")
            result = self.invoke(peer, environment=environment)
            self.assertEqual(result["status"], "completed")
            self.assertEqual(proxy.accepted, 0)
        self.assertNotIn("UNREAD_ENVIRONMENT_CANARY", json.dumps(result))
        self.assertEqual(peer.requests[0]["headers"][b"authorization"], f"Bearer {BEARER}".encode())

    def test_tls_keylog_environment_refuses_without_creating_keylog(self):
        keylog = self.directory / "must-not-exist.keylog"
        environment = dict(os.environ, SSLKEYLOGFILE=str(keylog))
        with self.peer() as peer:
            result = self.invoke(peer, environment=environment)
            self.assertTrue(result.get("threw"), result)
            self.assertEqual(peer.accepted, 0)
        self.assertFalse(keylog.exists())
        self.assertNotIn(str(keylog), json.dumps(result))
        self.assertNotIn(BEARER, json.dumps(result))

    def test_precancel_has_zero_connections_and_retains_original_identity(self):
        with self.peer() as peer:
            result = self.invoke(peer, pre_cancel=True)
            self.assertEqual(peer.accepted, 0)
        self.assert_failed(result, "cancelled")
        self.assertFalse(result["network_started"])
        self.assertFalse(result["upload_may_have_occurred"])
        self.assertEqual(result["phase"], "not_started")

    def test_mutated_encoding_and_binding_refuse_before_connection(self):
        for mutation in ("body", "url", "record", "profile", "request", "method"):
            with self.subTest(mutation=mutation), self.peer() as peer:
                result = self.invoke(peer, mutation=mutation)
                self.assertTrue(result.get("threw"), result)
                self.assertEqual(peer.accepted, 0)

    def test_credential_injection_empty_and_oversize_refuse_before_connection(self):
        for credential in ("newline", "nul", "empty", "nonascii", "oversized"):
            with self.subTest(credential=credential), self.peer() as peer:
                result = self.invoke(peer, credential_kind=credential)
                self.assertTrue(result.get("threw"), result)
                self.assertEqual(peer.accepted, 0)
                self.assertNotIn("Injected", json.dumps(result))

    def test_invalid_transport_limits_and_missing_trust_refuse_before_connection(self):
        for configuration in ({"max_header_bytes": 0}, {"max_header_line_bytes": 0},
                              {"max_total_bytes": 0}, {"poll_ms": 0}, {"empty_trust": True}):
            with self.subTest(configuration=configuration), self.peer() as peer:
                result = self.invoke(peer, **configuration)
                self.assertTrue(result.get("threw"), result)
                self.assertEqual(peer.accepted, 0)

    def test_tls_handshake_deadline_is_bounded(self):
        with self.peer(stall_tls=True) as peer:
            result = self.invoke(peer, timeout_ms=250)
        self.assert_failed(result, "deadline")
        self.assertTrue(result["network_started"])
        self.assertIsNone(result["tls_verified_at_us"])
        self.assertEqual(result["request_body_bytes_supplied"], 0)
        self.assertLess(result["elapsed_us"], 2_000_000)

    def test_header_wait_uses_same_deadline_after_complete_upload(self):
        def serve(peer, connection):
            peer.request(connection)
            peer.stop.wait(8)

        with self.peer(serve) as peer:
            result = self.invoke(peer, timeout_ms=300)
        self.assert_failed(result, "deadline")
        self.assertEqual(result["uploaded_body_bytes_reported"], result["request_body_size"])
        self.assertIsNotNone(result["tls_verified_at_us"])
        self.assertIsNone(result["first_header_at_us"])
        self.assertEqual(len(peer.requests), 1)

    def test_partial_header_deadline_retains_only_delivered_header_lines(self):
        complete = b"HTTP/1.1 200 Fixture\r\n"

        def serve(peer, connection):
            peer.request(connection)
            connection.sendall(complete + b"Content-Type: application/")
            peer.stop.wait(8)

        with self.peer(serve) as peer:
            result = self.invoke(peer, timeout_ms=300)
        self.assert_failed(result, "deadline")
        self.assertEqual(bytes.fromhex(result["raw_headers_hex"]), complete)
        self.assertEqual(result["header_bytes_observed"], len(complete))
        self.assertEqual(result["raw_body_hex"], "")

    def test_partial_sse_deadline_never_releases_tools(self):
        prefix = tool_stream().split(b"event: response.function_call_arguments.done")[0]

        def serve(peer, connection):
            peer.request(connection)
            connection.sendall(headers(len(prefix) + 100, content_type="text/event-stream") + prefix)
            peer.stop.wait(8)

        with self.peer(serve) as peer:
            result = self.invoke(peer, stream=True, timeout_ms=300)
        self.assert_failed(result, "deadline")
        self.assertEqual(bytes.fromhex(result["raw_body_hex"]), prefix)
        self.assertEqual(result["phase"], "response_body")
        self.assertFalse(result["transfer_complete"])

    def test_deadline_is_not_restarted_after_tls_or_headers(self):
        body = response_body()

        def serve(peer, connection):
            peer.request(connection)
            connection.sendall(headers(len(body)))
            if not peer.stop.wait(0.4):
                connection.sendall(body)

        # Each independent delay is below 500ms; their combined duration is
        # above it. Restarting the clock at TLS/header progress would succeed.
        with self.peer(serve, handshake_delay=0.25) as peer:
            result = self.invoke(peer, timeout_ms=500)
        self.assert_failed(result, "deadline")
        self.assertFalse(result["transfer_complete"])
        self.assertLess(result["elapsed_us"], 1_500_000)

    def test_tls_cancellation_does_not_wait_for_idle_progress_callback(self):
        with self.peer(stall_tls=True) as peer:
            result = self.invoke(peer, timeout_ms=4000, cancel_after_ms=180)
        self.assert_failed(result, "cancelled")
        self.assertLess(result["elapsed_us"], 1_000_000)
        self.assertEqual(result["uploaded_body_bytes_reported"], 0)

    def test_cancel_after_partial_body_keeps_failure_and_original_bytes(self):
        prefix = tool_stream().split(b"event: response.function_call_arguments.done")[0]
        marker = self.directory / "cancel-body"

        def serve(peer, connection):
            peer.request(connection)
            connection.sendall(headers(len(prefix) + 100, content_type="text/event-stream") + prefix)
            peer.stop.wait(0.15)
            marker.touch()
            peer.stop.wait(8)

        with self.peer(serve) as peer:
            result = self.invoke(peer, stream=True, timeout_ms=4000, cancel_marker=str(marker))
        self.assert_failed(result, "cancelled")
        self.assertEqual(bytes.fromhex(result["raw_body_hex"]), prefix)
        self.assertLess(result["elapsed_us"], 2_000_000)

    def test_valid_sse_terminal_before_http_end_still_obeys_cancel(self):
        body = tool_stream()
        marker = self.directory / "cancel-after-terminal"

        def serve(peer, connection):
            peer.request(connection)
            connection.sendall(headers(content_type="text/event-stream", chunked=True))
            connection.sendall(f"{len(body):x}\r\n".encode() + body + b"\r\n")
            # Withhold the final HTTP chunk; seeing response.completed cannot
            # turn a still-active transport into a successful tool response.
            peer.stop.wait(0.15)
            marker.touch()
            peer.stop.wait(8)

        with self.peer(serve) as peer:
            result = self.invoke(peer, stream=True, timeout_ms=4000, cancel_marker=str(marker))
        self.assert_failed(result, "cancelled")
        self.assertFalse(result["transfer_complete"])
        self.assertEqual(bytes.fromhex(result["raw_body_hex"]), body)

    def test_closed_connection_with_truncated_content_length_is_not_success(self):
        body = response_body(tool=True)
        prefix = body[:-15]

        def serve(peer, connection):
            peer.request(connection)
            connection.sendall(headers(len(body)) + prefix)

        with self.peer(serve) as peer:
            result = self.invoke(peer)
        self.assert_failed(result, "transfer_error")
        self.assertEqual(bytes.fromhex(result["raw_body_hex"]), prefix)
        self.assertTrue(result["upload_may_have_occurred"])
        self.assertFalse(result["transfer_complete"])
        self.assertEqual(peer.accepted, 1)

    def test_terminal_followed_by_illegal_sse_tail_invalidates_response(self):
        body = tool_stream() + b'data: {"type":"unexpected","sequence_number":7}\n\n'
        with self.peer(fixed_response(body, content_type="text/event-stream")) as peer:
            result = self.invoke(peer, stream=True)
        self.assert_failed(result, "protocol_error")
        self.assertTrue(result["transfer_complete"])
        self.assertEqual(bytes.fromhex(result["raw_body_hex"]), body)
        self.assertEqual(result["response"]["usage"]["input_tokens"], 9)

    def test_invalid_utf8_is_retained_losslessly_and_not_usable(self):
        body = b'{"invalid":"\xe4\xb8"}'
        with self.peer(fixed_response(body)) as peer:
            result = self.invoke(peer)
        self.assert_failed(result, "protocol_error")
        self.assertEqual(bytes.fromhex(result["raw_body_hex"]), body)

    def test_header_line_limit_is_observed_and_retained_with_bound(self):
        extra = b"X-Fixture: " + b"x" * 8192 + b"\r\n"
        with self.peer(fixed_response(response_body(), extra=extra)) as peer:
            result = self.invoke(peer, max_header_line_bytes=1024)
        self.assert_failed(result, "limit")
        self.assertTrue(result["raw_headers_truncated"])
        self.assertGreater(result["header_bytes_observed"], len(bytes.fromhex(result["raw_headers_hex"])))
        self.assertLessEqual(len(bytes.fromhex(result["raw_headers_hex"])), 65536)

    def test_total_header_limit_handles_many_small_lines(self):
        extra = b"".join(f"X-F-{index}: ".encode() + b"x" * 100 + b"\r\n" for index in range(60))
        with self.peer(fixed_response(response_body(), extra=extra)) as peer:
            result = self.invoke(peer, max_header_bytes=1024, max_header_line_bytes=512)
        self.assert_failed(result, "limit")
        self.assertTrue(result["raw_headers_truncated"])
        self.assertLessEqual(len(bytes.fromhex(result["raw_headers_hex"])), 1024)

    def test_json_body_limit_retains_bounded_prefix(self):
        body = response_body("x" * 5000)
        with self.peer(fixed_response(body)) as peer:
            result = self.invoke(peer, max_body_bytes=1024)
        self.assert_failed(result, "limit")
        self.assertTrue(result["raw_body_truncated"])
        retained = bytes.fromhex(result["raw_body_hex"])
        self.assertLessEqual(len(retained), 1024)
        self.assertTrue(body.startswith(retained))

    def test_sse_stream_limit_retains_bounded_prefix(self):
        body = tool_stream()
        with self.peer(fixed_response(body, content_type="text/event-stream")) as peer:
            result = self.invoke(peer, stream=True, max_stream_bytes=512)
        self.assert_failed(result, "limit")
        self.assertTrue(result["raw_body_truncated"])
        self.assertLessEqual(len(bytes.fromhex(result["raw_body_hex"])), 512)

    def test_total_raw_limit_is_shared_by_headers_and_body(self):
        body = response_body("x" * 1000)
        with self.peer(fixed_response(body)) as peer:
            result = self.invoke(peer, max_total_bytes=1024)
        self.assert_failed(result, "limit")
        retained = len(bytes.fromhex(result["raw_headers_hex"])) + len(bytes.fromhex(result["raw_body_hex"]))
        self.assertLessEqual(retained, 1024)
        self.assertTrue(result["raw_body_truncated"])

    def test_sse_event_and_output_limits_still_use_strict_decoder(self):
        body = (FIXTURES / "text.sse").read_bytes()
        for limits in ({"max_sse_line_bytes": 128}, {"max_sse_event_bytes": 128},
                       {"max_sse_events": 2}, {"max_text_bytes": 3}):
            with self.subTest(limits=limits), self.peer(fixed_response(body, content_type="text/event-stream")) as peer:
                result = self.invoke(peer, stream=True, **limits)
                self.assert_failed(result)
                self.assertIn(result["status"], ("limit", "protocol_error"))

    def test_partial_request_upload_deadline_is_not_free_retry_authority(self):
        def serve(peer, connection):
            prefix = bytearray()
            while len(prefix) < 4096:
                chunk = connection.recv(4096 - len(prefix))
                if not chunk:
                    raise ConnectionError("fixture upload ended before prefix")
                prefix.extend(chunk)
            peer.prefixes.append(bytes(prefix))
            peer.stop.wait(8)

        with self.peer(serve, receive_buffer=2048) as peer:
            result = self.invoke(peer, timeout_ms=3500, request_padding_bytes=900000)
        self.assert_failed(result, "deadline")
        self.assertEqual(len(peer.prefixes), 1)
        self.assertGreater(result["uploaded_body_bytes_reported"], 0)
        self.assertLess(result["uploaded_body_bytes_reported"], result["request_body_size"])
        self.assertGreaterEqual(result["request_body_bytes_supplied"], result["uploaded_body_bytes_reported"])
        self.assertLessEqual(result["request_body_bytes_supplied"], result["request_body_size"])
        self.assertTrue(result["upload_may_have_occurred"])
        self.assertEqual(peer.accepted, 1)
        self.assertEqual(peer.requests, [])
        _, separator, application_prefix = peer.prefixes[0].partition(b"\r\n\r\n")
        self.assertTrue(separator)
        self.assertGreater(len(application_prefix), 0)
        self.assertLess(len(application_prefix), result["request_body_size"])

    def test_wrong_content_type_or_compressed_body_never_becomes_tool_result(self):
        body = response_body(tool=True)
        for options in ({"content_type": "text/plain"},
                        {"extra": b"Content-Encoding: gzip\r\n"}):
            with self.subTest(options=options), self.peer(fixed_response(body, **options)) as peer:
                result = self.invoke(peer)
                self.assert_failed(result, "protocol_error")

    def test_conflicting_http_framing_fails_closed(self):
        body = response_body(tool=True)
        for extra in (b"Content-Length: 1\r\n", b"Transfer-Encoding: chunked\r\n"):
            with self.subTest(extra=extra), self.peer(fixed_response(body, extra=extra)) as peer:
                result = self.invoke(peer)
                self.assert_failed(result)

    def test_response_secret_echo_is_private_and_safe_summary_only_has_hashes(self):
        body = response_body(BEARER)
        with self.peer(fixed_response(body, extra=f"X-Private-Echo: {BEARER}\r\n".encode())) as peer:
            result = self.invoke(peer)
        self.assertEqual(result["status"], "completed")
        self.assertIn(BEARER.encode(), bytes.fromhex(result["raw_headers_hex"]))
        self.assertIn(BEARER.encode(), bytes.fromhex(result["raw_body_hex"]))
        self.assertEqual(result["response"]["text"], BEARER)
        self.assertNotIn(BEARER, json.dumps(result["summary"]))
        self.assertIn(hashlib.sha256(body).hexdigest(), json.dumps(result["summary"]))

    def test_safe_summary_refuses_changed_raw_decoded_or_options_evidence(self):
        with self.peer() as peer:
            result = self.invoke(peer, verify_summary_integrity=True)
        self.assertEqual(result["status"], "completed")
        self.assertEqual(result["integrity_checks"],
                         {"raw_body": True, "decoded_response": True, "transport_options": True})
        self.assertEqual(peer.accepted, 1)

    def test_interim_continue_does_not_hide_final_status_or_bytes(self):
        body = response_body()
        interim = b"HTTP/1.1 100 Continue\r\nX-Interim: fixture\r\n\r\n"

        def serve(peer, connection):
            peer.request(connection)
            connection.sendall(interim + headers(len(body)) + body)

        with self.peer(serve) as peer:
            result = self.invoke(peer)
        self.assertEqual(result["status"], "completed")
        self.assertEqual(result["http_status"], 200)
        self.assertTrue(bytes.fromhex(result["raw_headers_hex"]).startswith(interim))

    def test_connection_refusal_has_no_upload_and_no_retry(self):
        with self.peer() as peer:
            peer.close()
            result = self.invoke(peer, timeout_ms=500)
        self.assert_failed(result, "connection_error")
        self.assertEqual(result["uploaded_body_bytes_reported"], 0)
        self.assertEqual(result["request_body_bytes_supplied"], 0)
        self.assertFalse(result["upload_may_have_occurred"])


if __name__ == "__main__":
    previous = os.umask(0)
    os.umask(previous)
    print(f"HTTPS loopback fixture UID={os.getuid()} effective_umask={previous:04o}", flush=True)
    unittest.main(verbosity=2)
