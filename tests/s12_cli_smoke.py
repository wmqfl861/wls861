#!/usr/bin/env python3
"""S1.2 CLI integration with actual files and SQLite in disposable directories.

Model responses are fixed fixtures. The process test deliberately supplies no
runtime manifest or cgroup and verifies refusal without executing its argv.
Positive strong isolation requires a separately provisioned environment.
"""
from __future__ import annotations
from contextlib import closing

import base64
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest
import uuid


if len(sys.argv) != 3:
    raise SystemExit("usage: s12_cli_smoke.py WLS_BINARY SOURCE_ROOT")
BINARY = Path(sys.argv[1]).resolve()
SOURCE = Path(sys.argv[2]).resolve()
FIXTURES = SOURCE / "tests" / "fixtures"
sys.argv = [sys.argv[0]]


class WorkerCliSmoke(unittest.TestCase):
    def setUp(self):
        original_umask = os.umask(0o077)
        self.addCleanup(os.umask, original_umask)
        self.temporary = tempfile.TemporaryDirectory(prefix="wls-s12-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.workspace = self.directory / "work"
        self.workspace.mkdir(mode=0o700)
        self.database = self.directory / "runtime.sqlite"

    def invoke(self, *arguments, error=None):
        completed = subprocess.run(
            [str(BINARY), *(str(arg) for arg in arguments), "--source-root", str(SOURCE)],
            cwd=self.directory, capture_output=True, text=True, encoding="utf-8",
            timeout=30, check=False)
        if error is not None:
            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(completed.stdout, "")
            failure = json.loads(completed.stderr)
            self.assertEqual(failure["error"], error, failure)
            return failure
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")
        return json.loads(completed.stdout)

    def rows(self, sql, values=()):
        with closing(sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True)) as db, db:
            db.row_factory = sqlite3.Row
            return [dict(row) for row in db.execute(sql, values)]

    def json_file(self, name, value):
        path = self.directory / name
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return path

    def submit(self):
        return self.invoke("submit", "--db", self.database, "--key", "workspace-request",
                           "--text-file", FIXTURES / "s12_request.txt",
                           "--constraints-file", FIXTURES / "s12_constraints.json")["request"]["request_id"]

    def run_files(self, request_id, script=None, calls=3, error=None, extra=()):
        return self.invoke("run-fake", "--db", self.database, "--request", request_id,
                           "--script", script or FIXTURES / "s12_fake_workspace.json",
                           "--workspace", self.workspace, "--worker-calls", calls, *extra, error=error)

    def query(self, request_id, task_id, tool, arguments, *, workspace=None, error=None):
        source = self.json_file("query.json", arguments)
        return self.invoke("tool-query", "--db", self.database, "--request", request_id,
                           "--task", task_id, "--workspace", workspace or self.workspace,
                           "--tool", tool, "--input", source, error=error)

    # G1-3 FINAL v14 3 (D2) local original capture (s12 overwrite contract).
    # The enable state is frozen by the execution environment's
    # WLS_G1_EVIDENCE_ROOT key and is constant for one test process: a
    # completely absent key disables only this optional capture I/O while
    # every business action, assertion and fixture cleanup still runs (such a
    # run never counts as retained evidence); a present value must name this
    # run's bound owner-only directory -- absolute, a real directory rather
    # than a symlink, owned by the current user, mode 0o700 -- and any other
    # present value is an explicit evidence failure, never a silent fallback
    # to a default location.
    def evidence_root(self):
        if "WLS_G1_EVIDENCE_ROOT" not in os.environ:
            return None
        root = os.environ["WLS_G1_EVIDENCE_ROOT"]
        if not root.startswith("/"):
            raise AssertionError(f"G1 evidence root must be absolute: {root!r}")
        try:
            info = os.lstat(root)
        except OSError as error:
            raise AssertionError(f"G1 evidence root not usable: {root!r}: {error}") from error
        if (os.path.islink(root) or not stat.S_ISDIR(info.st_mode)
                or info.st_uid != os.geteuid()
                or stat.S_IMODE(info.st_mode) != 0o700):
            raise AssertionError("G1 evidence root is not a bound absolute "
                                 f"owner-only 0700 directory: {root!r}")
        return Path(root)

    def capture_evidence(self, name, payload):
        root = self.evidence_root()
        if root is None:
            return None
        target = root / f"s12_cli_smoke-{self._testMethodName}-{uuid.uuid4().hex}-{name}"
        try:
            descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                                 | os.O_NOFOLLOW, 0o600)
        except OSError as error:
            raise AssertionError("G1 evidence target cannot be created "
                                 f"exclusively: {target}: {error}") from error
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            os.fchmod(stream.fileno(), 0o600)
        return target

    def capture_database(self, tag):
        # Called only after the CLI subprocesses exited and this suite's
        # read-only connections closed; copies the physical database plus any
        # WAL/SHM sidecars that still exist at this capture point. The
        # absent-key branch returns before any of these capture-only reads.
        if self.evidence_root() is None:
            return
        for suffix, label in (("", "-database"), ("-wal", "-wal"), ("-shm", "-shm")):
            source = Path(str(self.database) + suffix)
            if source.exists():
                self.capture_evidence(f"{tag}{label}", source.read_bytes())

    def capture_workspace_effects(self, tag):
        if self.evidence_root() is None:
            return
        effects = []
        for path in sorted(self.workspace.rglob("*")):
            relative = str(path.relative_to(self.workspace))
            if path.is_file():
                data = path.read_bytes()
                effects.append({"path": relative, "type": "file", "bytes": len(data),
                                "mode": oct(path.stat().st_mode & 0o777),
                                "sha256": hashlib.sha256(data).hexdigest()})
                self.capture_evidence(f"{tag}-file-" + relative.replace("/", "_"), data)
            else:
                effects.append({"path": relative, "type": "dir",
                                "mode": oct(path.stat().st_mode & 0o777)})
        self.capture_evidence(f"{tag}-effects.json",
                              json.dumps(effects, ensure_ascii=False, indent=1).encode())

    def test_doctor_is_read_only_and_new_database_uses_current_schema(self):
        before = set(self.directory.iterdir())
        doctor = self.invoke("doctor")
        self.assertFalse(doctor["strong_prerequisites"])
        self.assertFalse(doctor["process_isolation_verified"])
        self.assertFalse(doctor["environment_checks_executed"])
        self.assertEqual(doctor["evidence"]["programs_executed"], 0)
        self.assertTrue(doctor["issues"])
        self.assertEqual(set(self.directory.iterdir()), before)
        initialized = self.invoke("init", "--db", self.database)
        self.assertEqual(initialized["user_version"], 410)
        self.assertEqual(initialized["journal_mode"], "wal")
        self.assertEqual(initialized["synchronous"], 2)

    def test_multi_step_file_chain_persists_facts_and_complete_feedback(self):
        request_id = self.submit()
        run = self.run_files(request_id)
        expected = b"verified from S1.2\n"
        self.assertEqual((self.workspace / "result.txt").read_bytes(), expected)
        self.assertEqual([call["role"] for call in run["calls"]],
                         ["dispatcher", "team_lead", "worker", "worker", "worker"])
        self.assertEqual(run["budget"]["attempts"], 5)
        self.assertEqual(run["budget"]["spent_input"], 500)
        self.assertFalse(run["business_delivered"])
        self.assertTrue(run["worker_tools_enabled"])
        self.assertEqual(run["real_model_calls"], 0)
        self.assertEqual(len(self.rows("SELECT * FROM ops")), 2, "a read must not become a business operation")
        facts = self.rows("SELECT o.tool_id,o.status,r.* FROM ops o JOIN operation_results r USING(op_id)")
        self.assertEqual({row["tool_id"] for row in facts}, {"fs_write", "fs_patch"})
        for fact in facts:
            self.assertEqual((fact["status"], fact["observed_outcome"], fact["accepted"]),
                             ("SUCCEEDED", "SUCCEEDED", 1))
            self.assertEqual(len(json.loads(fact["original_binding"])), 10)
            self.assertEqual(len(fact["record_sha256"]), 64)
            self.assertTrue(json.loads(fact["evidence"]))
        last_call = run["calls"][-1]
        envelope = json.loads(self.rows("SELECT request_envelope FROM call_attempts WHERE attempt_id=?",
                                        (last_call["attempt_id"],))[0]["request_envelope"])
        self.assertIn("hello from S1.2", envelope["context"]["dynamic_suffix"])
        self.assertIn(hashlib.sha256(b"hello from S1.2\n").hexdigest(),
                      envelope["context"]["dynamic_suffix"])
        self.assertIn("只读写明确指定的工作目录", envelope["context"]["dynamic_suffix"])
        self.assertIn("fs_patch", envelope["context"]["tool_names"])
        workspace = self.rows("SELECT * FROM workspace_bindings")[0]
        self.assertEqual(workspace["canonical_path"], str(self.workspace))
        self.assertIn("max_total_bytes", json.loads(workspace["execution_policy"])["workspace_limits"])

    def test_read_only_tool_pages_preserve_epochs_events_and_budget(self):
        request_id = self.submit()
        run = self.run_files(request_id)
        task_id = run["task_id"]
        before = self.invoke("show", "--db", self.database, "--request", request_id)
        events = self.rows("SELECT * FROM events")
        first = self.query(request_id, task_id, "fs_read", {"path": "result.txt", "max_bytes": 8})
        second = self.query(request_id, task_id, "fs_read", {"path": "result.txt", "offset": first["next_offset"]})
        self.assertEqual(base64.b64decode(first["content_base64"]) + base64.b64decode(second["content_base64"]),
                         b"verified from S1.2\n")
        self.assertEqual(first["sha256"], second["sha256"])
        listing = self.query(request_id, task_id, "fs_list", {})
        self.assertEqual(len(listing["entries"]), 1)
        found = self.query(request_id, task_id, "fs_search", {"path": ".", "literal": "verified"})
        self.assertEqual(found["matches"][0]["byte_offset"], 0)
        op_id = self.rows("SELECT op_id FROM ops LIMIT 1")[0]["op_id"]
        status = self.query(request_id, task_id, "op_status", {"op_id": op_id})
        self.assertEqual(status["operation"]["status"], "SUCCEEDED")
        output = self.query(request_id, task_id, "op_output", {"op_id": op_id, "stream": "stdout"})
        self.assertTrue(output["eof"])
        self.assertEqual(output["data"], "")
        self.assertEqual(self.invoke("show", "--db", self.database, "--request", request_id), before)
        self.assertEqual(self.rows("SELECT * FROM events"), events)

    def test_read_paths_and_diagnostic_class_cannot_be_forged(self):
        request_id = self.submit()
        task_id = self.run_files(request_id)["task_id"]
        outside = self.directory / "outside.txt"
        outside.write_text("temporary test secret", encoding="utf-8")
        self.query(request_id, task_id, "fs_read", {"path": "../outside.txt"}, error="unauthorized")
        (self.workspace / "link").symlink_to(outside)
        self.query(request_id, task_id, "fs_read", {"path": "link"}, error="unauthorized")
        self.query(request_id, task_id, "fs_write", {"path": "forged.txt", "content": "bad"}, error="unauthorized")
        self.query(request_id, task_id, "fs_read", {"path": "result.txt", "class": "control"}, error="invalid_input")
        self.assertFalse((self.workspace / "forged.txt").exists())
        self.assertEqual(outside.read_text(), "temporary test secret")

    def test_process_without_manifest_refuses_and_persists_no_spawn(self):
        request_id = self.submit()
        script = json.loads((FIXTURES / "s12_fake_workspace.json").read_text())[:3]
        script[2]["tool_calls"] = [{"call_id": "run-refused", "name": "run",
                                   "arguments": {"argv": ["/bin/sh", "-c", "echo bad > refused.txt"]}}]
        run = self.run_files(request_id, self.json_file("refused.json", script), calls=1)
        result = run["calls"][-1]["result"]
        self.assertEqual(result["operation"]["status"], "FAILED")
        fact = result["execution_result"]
        self.assertEqual(fact["observed_outcome"], "FAILED")
        self.assertFalse(fact["result"]["spawned"])
        self.assertFalse(fact["result"]["strong_isolation"])
        self.assertEqual(fact["result"]["status"], "REFUSED")
        self.assertFalse((self.workspace / "refused.txt").exists())
        self.assertEqual(fact["stdout_bytes"], 0)
        self.assertNotIn("stdout_base64", fact["result"])

    def test_workspace_identity_and_policy_drift_are_rejected(self):
        request_id = self.submit()
        run = self.run_files(request_id)
        other = self.directory / "other"
        other.mkdir(mode=0o700)
        self.query(request_id, run["task_id"], "fs_list", {}, workspace=other, error="conflict")
        manifest = self.json_file("changed-policy.json", {"schema_version": 1, "max_output_bytes": 4096})
        self.run_files(request_id, calls=1, error="conflict", extra=["--sandbox-manifest", manifest])
        self.assertEqual((self.workspace / "result.txt").read_bytes(), b"verified from S1.2\n")
        self.assertEqual(len(self.rows("SELECT * FROM operation_results")), 2)

    def test_overwrite_conflict_stops_the_explicit_worker_batch(self):
        request_id = self.submit()
        (self.workspace / "result.txt").write_text("host supplied", encoding="utf-8")
        run = self.run_files(request_id)
        try:
            # The original contract of this scenario is 3 calls: Dispatcher,
            # TeamLead and the first Worker. The request-frozen same-call
            # schema retry cap admits exactly `schema_retry_max` additional
            # bounded correction rounds, so the legal upper bound is
            # 3 + frozen cap, derived here from this request's own frozen
            # configuration snapshot instead of a bare magic number.
            snapshot = json.loads(self.rows("SELECT config_snapshot FROM requests WHERE request_id=?",
                                            (request_id,))[0]["config_snapshot"])
            frozen_cap = snapshot["system"]["context"]["schema_retry_max"]
            self.assertEqual(frozen_cap, 2)
            upper_bound = 3 + frozen_cap
            self.assertEqual(len(run["calls"]), upper_bound)
            self.assertEqual([call["role"] for call in run["calls"]],
                             ["dispatcher", "team_lead", "worker", "worker", "worker"])
            # The chain stopped exactly at the derived bound: no sixth
            # attempt, send or business operation ever existed.
            self.assertEqual(
                self.rows("SELECT count(*) AS n FROM call_attempts WHERE request_id=?",
                          (request_id,))[0]["n"], upper_bound)
            self.assertEqual(run["budget"]["attempts"], upper_bound)
            self.assertEqual(run["budget"]["spent_input"], 100 * upper_bound)
            self.assertEqual(run["budget"]["spent_output"], 20 * upper_bound)
            # The corrected rounds are exactly the scripted next steps: the
            # diagnostic read observes the untouched host bytes and creates
            # no operation; the write and the hash-mismatched patch each keep
            # one immutable FAILED operation row.
            self.assertEqual(run["calls"][3]["result"]["content"], "host supplied")
            first_failure = run["calls"][2]["result"]
            last_failure = run["calls"][-1]["result"]
            for failure in (first_failure, last_failure):
                self.assertEqual(failure["operation"]["status"], "FAILED")
                self.assertEqual(failure["execution_result"]["observed_outcome"], "FAILED")
                self.assertEqual(failure["execution_result"]["evidence"]["failure_kind"],
                                 "content_precondition")
                self.assertEqual(failure["correction_feedback"]["format"],
                                 "wls-correction-feedback-v1")
                self.assertEqual(failure["correction_feedback"]["reason"], "content_precondition")
                self.assertTrue(failure["correction_feedback"]["correction_eligible"])
            self.assertEqual([{"tool_id": row["tool_id"], "status": row["status"]}
                              for row in self.rows("SELECT tool_id,status FROM ops ORDER BY rowid")],
                             [{"tool_id": "fs_write", "status": "FAILED"},
                              {"tool_id": "fs_patch", "status": "FAILED"}])
            # The host-supplied original bytes survive verbatim; the failed
            # chain never overwrote them nor created any new effect.
            self.assertEqual((self.workspace / "result.txt").read_text(), "host supplied")
            self.assertEqual([path.name for path in sorted(self.workspace.iterdir())],
                             ["result.txt"])
            self.assertFalse(run["business_delivered"])
        finally:
            # G1-3 FINAL v12 6.4: preserve the original command output,
            # physical database and workspace effect inventory even on the
            # RED/failure path, before TemporaryDirectory cleanup runs.
            self.capture_evidence("overwrite-run-result.json",
                                  json.dumps(run, ensure_ascii=False, indent=1).encode())
            self.capture_database("overwrite")
            self.capture_workspace_effects("overwrite")

    def test_invalid_new_options_do_not_open_a_writer(self):
        script = FIXTURES / "s12_fake_workspace.json"
        self.invoke("run-fake", "--db", self.database, "--request", "invalid",
                    "--script", script, "--worker-calls", 17, error="invalid_input")
        self.invoke("run-fake", "--db", self.database, "--request", "invalid",
                    "--script", script, "--worker-calls", 2, error="invalid_input")
        self.invoke("doctor", "--template", "missing", error="invalid_input")
        self.assertFalse(self.database.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
