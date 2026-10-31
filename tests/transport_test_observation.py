"""Opt-in, allowlisted observations for synthetic TLS tests, never a gate.

WLS_TEST_TRANSPORT_OBSERVATION names a new JSONL file. With no variable set,
there is no file, clock or additional JSON parsing on the normal path.
No request/response bytes, identifiers, hashes, paths or exception messages are
copied to the observation. Original unittest output is a separate private log.
Observing adds overhead; a diagnostic pass cannot close a historical failure.
"""
from __future__ import annotations

import atexit
import contextlib
import json
import os
import ssl
import subprocess
import sys
import threading
import time

ENVIRONMENT = "WLS_TEST_TRANSPORT_OBSERVATION"
STAGES = frozenset("""
    certificates_start certificates_ready fixture_start peer_listening
    peer_accepted tls_start tls_ready handler_start handler_complete
    peer_expected_exception peer_unexpected_exception peer_connection_closed
    request_receive_start request_first_chunk request_received
    prefix_receive_start prefix_first_chunk prefix_saved prefix_receive_end
    response_send_start response_send_complete peer_close_start peer_closed
    configuration_ready subprocess_start subprocess_return subprocess_timeout
    subprocess_exception driver_result driver_output_not_json observation_limit
    observation_finished
""".split())
COUNTS = frozenset("""
    accepted requests prefixes errors live_threads chunk_bytes prefix_bytes
    header_bytes body_bytes stdout_bytes stderr_bytes timeout_s returncode
    request_body_size request_body_bytes_supplied uploaded_body_bytes_reported
    header_bytes_observed header_bytes_retained body_bytes_observed
    body_bytes_retained elapsed_us connected_at_us tls_verified_at_us
    first_header_at_us first_body_at_us http_status response_text_characters
    response_tool_calls driver_error_characters unknown_cost reserved_cost
""".split())
FLAGS = frozenset("""
    threw network_started transfer_complete upload_may_have_occurred
    raw_headers_truncated raw_body_truncated response_usable settlement_present
    first_sent replay_sent replay_new_send_authority ledger_consistent
""".split())
REASONS = frozenset("""
    completed cancelled deadline header_limit body_limit total_limit chunk_limit
    invalid_http_header callback_failure repeat_request_refused socket_option_failed
    certificate_error connection_error invalid_http_framing transfer_error
    incomplete_http_framing http_error protocol_error
    not_dispatched_after_recovery unobserved_after_recovery
""".split())
ENUMS = {
    "status": frozenset("""completed http_error cancelled deadline deadline_exceeded
        certificate_error connection_error transfer_error limit limit_exceeded
        protocol_error""".split()),
    "phase": frozenset("""not_started connecting tls_handshake request_write
        response_headers response_body""".split()),
    "reason": REASONS,
    "response_error_code": REASONS,
    "response_status": frozenset(("completed", "interrupted", "error")),
    "attempt_status": frozenset(("PENDING", "UNKNOWN", "COMPLETED", "ERROR", "INTERRUPTED", "STALE")),
    "exception_kind": frozenset(("tls", "timeout", "connection", "os", "other")),
}


def byte_count(value):
    if isinstance(value, bytes):
        return len(value)
    return len(value.encode("utf-8")) if isinstance(value, str) else None


def exception_kind(error):
    # Never use a dynamic class name, repr(), str(), argv or error message.
    if isinstance(error, ssl.SSLError):
        return "tls"
    if isinstance(error, (TimeoutError, subprocess.TimeoutExpired)):
        return "timeout"
    if isinstance(error, ConnectionError):
        return "connection"
    return "os" if isinstance(error, OSError) else "other"


class Observation:
    def __init__(self, path=None):
        self.enabled = path is not None
        self.stream = None
        self.failed = False
        if self.enabled:
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            self.stream = os.fdopen(descriptor, "w", encoding="utf-8")
            self.lock = threading.Lock()
            self.started = time.monotonic_ns()
            self.rows = 0
            atexit.register(self.close)

    def close(self):
        if self.stream is not None and not self.stream.closed:
            self.record("observation_finished")
            with self.lock:
                try:
                    self.stream.close()
                except (OSError, ValueError):
                    self.io_failure()

    def io_failure(self):
        if not self.failed:
            self.failed = True
            try:
                sys.stderr.write("Transport test observation I/O failed; diagnostic evidence is incomplete.\n")
            except (OSError, ValueError):
                pass

    def record(self, stage, **values):
        if not self.enabled or self.failed:
            return
        if stage not in STAGES:
            raise ValueError("unregistered observation stage")
        clean = {}
        for key, value in values.items():
            if key in COUNTS:
                if value is None or type(value) is int and -(2**63) <= value < 2**63:
                    clean[key] = value
            elif key in FLAGS:
                if type(value) is bool:
                    clean[key] = value
            elif key == "attempt_accepted":
                if type(value) is int and value in (0, 1):
                    clean[key] = value
            elif key in ENUMS:
                clean[key] = value if type(value) is str and value in ENUMS[key] else "redacted"
            else:
                raise ValueError("unregistered observation field")
        with self.lock:
            if self.rows > 512:
                return
            if self.rows == 512:
                stage, clean = "observation_limit", {}
            row = {"sequence": self.rows, "monotonic_us": (time.monotonic_ns() - self.started) // 1000,
                   "stage": stage, **clean}
            try:
                self.stream.write(json.dumps(row, sort_keys=True) + "\n")
                self.stream.flush()
            except (OSError, ValueError):
                # Observation failure must not interrupt original socket/join
                # cleanup or replace a network/timeout exception in a finally.
                # Without the successful closing marker the collector refuses
                # to treat even a passing original method as valid diagnostics.
                self.io_failure()
                return
            self.rows += 1

    def exception(self, stage, error):
        if self.enabled:
            self.record(stage, exception_kind=exception_kind(error))

    def returned(self, process):
        if not self.enabled:
            return
        self.record("subprocess_return", returncode=process.returncode,
                    stdout_bytes=byte_count(process.stdout), stderr_bytes=byte_count(process.stderr))
        try:
            result = json.loads(process.stdout)
        except (TypeError, ValueError):
            self.record("driver_output_not_json")
            return
        if not isinstance(result, dict):
            self.record("driver_output_not_json")
            return
        values = {}
        # These paths are selected explicitly, never copied recursively.
        transport = result.get("safe", {}).get("transport") if isinstance(result.get("safe"), dict) else None
        transport = transport if isinstance(transport, dict) else result
        for key in COUNTS | FLAGS | {"status", "phase", "reason"}:
            if key in transport:
                values[key] = transport[key]
        for key in ("threw", "first_sent", "replay_sent", "replay_new_send_authority"):
            if key in result:
                values[key] = result[key]
        attempt = result.get("attempt")
        if isinstance(attempt, dict):
            values["attempt_status"] = attempt.get("status")
            values["attempt_accepted"] = attempt.get("accepted")
            values["settlement_present"] = attempt.get("settlement_sha256") is not None
        response = attempt.get("response") if isinstance(attempt, dict) else result.get("response")
        if isinstance(response, dict):
            values["response_status"] = response.get("status")
            values["response_error_code"] = response.get("error_code")
            if isinstance(response.get("text"), str):
                values["response_text_characters"] = len(response["text"])
            if isinstance(response.get("tool_calls"), list):
                values["response_tool_calls"] = len(response["tool_calls"])
        if isinstance(result.get("error"), str):
            values["driver_error_characters"] = len(result["error"])
        if isinstance(result.get("budget"), dict):
            for key in ("unknown_cost", "reserved_cost"):
                if key in result["budget"]:
                    values[key] = result["budget"][key]
        if isinstance(result.get("ledger"), dict):
            values["ledger_consistent"] = result["ledger"].get("consistent")
        self.record("driver_result", **values)

    @contextlib.contextmanager
    def subprocess(self, timeout_s):
        self.record("subprocess_start", timeout_s=timeout_s)
        try:
            yield self.returned
        except subprocess.TimeoutExpired as error:
            try:
                self.record("subprocess_timeout", timeout_s=timeout_s,
                            stdout_bytes=byte_count(error.stdout), stderr_bytes=byte_count(error.stderr))
            finally:
                # Even a failed observation write cannot replace the original
                # timeout/exception. The original unittest still reports it.
                raise error
        except BaseException as error:
            try:
                self.exception("subprocess_exception", error)
            finally:
                raise error


OBSERVATION = Observation(os.environ.get(ENVIRONMENT) or None)
