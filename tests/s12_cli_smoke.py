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
import subprocess
import sys
import tempfile
import unittest


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
        self.assertEqual(len(run["calls"]), 3)
        self.assertEqual(run["calls"][-1]["result"]["operation"]["status"], "FAILED")
        self.assertEqual((self.workspace / "result.txt").read_text(), "host supplied")
        self.assertEqual(len(self.rows("SELECT * FROM ops")), 1)
        self.assertFalse(run["business_delivered"])

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
