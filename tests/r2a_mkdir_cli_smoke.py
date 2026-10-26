#!/usr/bin/env python3
"""R2a.1 directory creation through the real CLI, SQLite and private files.

Fake responses exercise declared tools and persisted accounting. No process
runner, external model or runtime-owned allocation is invoked by this suite.
"""
from __future__ import annotations

from contextlib import closing
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest


if len(sys.argv) != 3:
    raise SystemExit("usage: r2a_mkdir_cli_smoke.py WLS_BINARY SOURCE_ROOT")
BINARY = Path(sys.argv[1]).resolve()
SOURCE = Path(sys.argv[2]).resolve()
FIXTURES = SOURCE / "tests" / "fixtures"
sys.argv = [sys.argv[0]]


class MkdirCliSmoke(unittest.TestCase):
    def setUp(self):
        previous_umask = os.umask(0o077)
        self.addCleanup(os.umask, previous_umask)
        temporary = tempfile.TemporaryDirectory(prefix="wls-r2a-mkdir-cli-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
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
        with closing(sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True)) as db:
            db.row_factory = sqlite3.Row
            return [dict(row) for row in db.execute(sql, values)]

    def snapshot(self):
        with closing(sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True)) as db:
            tables = [row[0] for row in db.execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name")]
            return {table: db.execute(f'SELECT * FROM "{table}" ORDER BY rowid').fetchall()
                    for table in tables}

    def json_file(self, name, value):
        path = self.directory / name
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return path

    def submit(self, key="mkdir-request", functions=None):
        args = ["submit", "--db", self.database, "--key", key,
                "--text-file", FIXTURES / "s12_request.txt",
                "--constraints-file", FIXTURES / "s12_constraints.json"]
        if functions is not None:
            args += ["--functions-dir", functions]
        return self.invoke(*args)["request"]["request_id"]

    def script(self, calls):
        source = json.loads((FIXTURES / "s12_fake_workspace.json").read_text(encoding="utf-8"))
        script = copy.deepcopy(source[:2])
        script[0]["tool_calls"][0]["arguments"]["summary"] = "在指定目录内创建输出子目录并保留工具执行事实。"
        order = script[1]["tool_calls"][0]["arguments"]
        order["approach"] = "确认当前说明后，只使用已声明的有界目录和文件工具。"
        order["steps"] = [{"id": "s1", "text": "逐项执行当前脚本，失败时保留原始证据。"}]
        for index, (name, arguments) in enumerate(calls, 1):
            step = copy.deepcopy(source[2])
            step["role_call_index"] = index
            step["tool_calls"] = [{"call_id": f"mkdir-step-{index}", "name": name, "arguments": arguments}]
            script.append(step)
        return self.json_file("mkdir-script.json", script)

    def run_calls(self, request, calls, error=None):
        script = self.script(calls)
        return self.invoke("run-fake", "--db", self.database, "--request", request,
                           "--script", script, "--workspace", self.workspace,
                           "--worker-calls", len(calls), error=error)

    def frozen_rows(self, request):
        return self.rows(
            "SELECT r.config_snapshot,rr.config_snapshot_sha256,rr.tool_allowlist "
            "FROM requests r JOIN request_runtime rr USING(request_id) WHERE request_id=?",
            (request,))[0]

    def test_new_frozen_grant_creates_private_directory_then_writes_and_reads(self):
        request = self.submit()
        run = self.run_calls(request, [
            ("fs_mkdir", {"path": "output"}),
            ("fs_write", {"path": "output/result.txt", "content": "directory result\n"}),
            ("fs_read", {"path": "output/result.txt"}),
        ])
        self.assertEqual([call["role"] for call in run["calls"]],
                         ["dispatcher", "team_lead", "worker", "worker", "worker"])
        self.assertEqual(stat.S_IMODE((self.workspace / "output").stat().st_mode), 0o700)
        self.assertEqual((self.workspace / "output/result.txt").read_bytes(), b"directory result\n")
        self.assertEqual(run["calls"][-1]["result"]["content"], "directory result\n")
        self.assertEqual(run["budget"]["attempts"], 5)
        self.assertEqual(run["budget"]["spent_input"], 500)
        self.assertEqual(run["real_model_calls"], 0)
        self.assertFalse(run["business_delivered"])
        self.assertIn("fs_mkdir", json.loads(self.frozen_rows(request)["tool_allowlist"]))
        facts = self.rows("SELECT o.tool_id,o.side_effect,o.status,r.* FROM ops o "
                          "JOIN operation_results r USING(op_id) WHERE o.request_id=? ORDER BY o.rowid", (request,))
        self.assertEqual([fact["tool_id"] for fact in facts], ["fs_mkdir", "fs_write"])
        for fact in facts:
            self.assertEqual((fact["side_effect"], fact["status"], fact["observed_outcome"], fact["accepted"]),
                             ("workspace", "SUCCEEDED", "SUCCEEDED", 1))
            self.assertEqual(len(json.loads(fact["original_binding"])), 10)
            self.assertEqual(len(fact["record_sha256"]), 64)
        mkdir_fact = facts[0]
        evidence = json.loads(mkdir_fact["evidence"])
        self.assertTrue(all(evidence[field] for field in
                            ("mkdir_attempted", "created", "child_synced", "parent_synced", "durable")))
        self.assertEqual(evidence["directory_inode"], (self.workspace / "output").stat().st_ino)
        self.assertEqual(json.loads(mkdir_fact["result"]),
                         {"path": "output", "created": True, "type": "directory", "mode": 0o700})
        envelope = json.loads(self.rows("SELECT request_envelope FROM call_attempts WHERE attempt_id=?",
                                        (run["calls"][-1]["attempt_id"],))[0]["request_envelope"])
        self.assertIn("fs_mkdir", envelope["context"]["tool_names"])
        self.assertIn("mkdir_attempted", envelope["context"]["dynamic_suffix"])
        binding = self.rows("SELECT execution_policy FROM workspace_bindings")[0]
        self.assertEqual(json.loads(binding["execution_policy"])["workspace_limits"]["tool_schema_version"],
                         "wls-worker-tools-v1")
        self.assertEqual(self.rows("SELECT * FROM owned_workspaces"), [],
                         "business mkdir must not create runtime ownership or cleanup authority")

    def test_existing_directory_is_failed_and_stops_following_worker_effects(self):
        (self.workspace / "output").mkdir(mode=0o700)
        retained = self.workspace / "output/original.txt"
        retained.write_bytes(b"host fixture must remain")
        inode = (self.workspace / "output").stat().st_ino
        request = self.submit()
        run = self.run_calls(request, [
            ("fs_mkdir", {"path": "output"}),
            ("fs_write", {"path": "output/should-not-run.txt", "content": "blocked"}),
        ])
        self.assertEqual(len(run["calls"]), 3)
        result = run["calls"][-1]["result"]
        self.assertEqual(result["operation"]["status"], "FAILED")
        self.assertEqual(result["execution_result"]["observed_outcome"], "FAILED")
        self.assertFalse(result["execution_result"]["evidence"]["mkdir_attempted"])
        self.assertEqual((self.workspace / "output").stat().st_ino, inode)
        self.assertEqual(retained.read_bytes(), b"host fixture must remain")
        self.assertFalse((self.workspace / "output/should-not-run.txt").exists())
        self.assertEqual(len(self.rows("SELECT * FROM ops")), 1)
        self.assertEqual(run["budget"]["attempts"], 3)

    def test_missing_parent_does_not_create_intermediate_directories(self):
        request = self.submit()
        run = self.run_calls(request, [("fs_mkdir", {"path": "absent/child"})])
        result = run["calls"][-1]["result"]
        self.assertEqual(result["operation"]["status"], "FAILED")
        self.assertFalse(result["execution_result"]["evidence"]["mkdir_attempted"])
        self.assertFalse((self.workspace / "absent").exists())
        self.assertEqual(self.rows("SELECT status FROM ops"), [{"status": "FAILED"}])

    def test_diagnostic_entry_refuses_mkdir_without_any_database_mutation(self):
        request = self.submit()
        run = self.run_calls(request, [("fs_mkdir", {"path": "allowed"})])
        args = self.json_file("diagnostic.json", {"path": "diagnostic-forgery"})
        before = self.snapshot()
        self.invoke("tool-query", "--db", self.database, "--request", request,
                    "--task", run["task_id"], "--workspace", self.workspace,
                    "--tool", "fs_mkdir", "--input", args, error="unauthorized")
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.workspace / "diagnostic-forgery").exists())
        self.assertTrue((self.workspace / "allowed").is_dir())

    def test_model_cannot_request_recursive_creation_or_supply_authority(self):
        request = self.submit()
        self.run_calls(request, [("fs_mkdir", {"path": "forged", "parents": True, "mode": 0o777})],
                       error="invalid_input")
        self.assertFalse((self.workspace / "forged").exists())
        self.assertEqual(self.rows("SELECT * FROM ops"), [])
        budget = self.rows("SELECT attempts,spent_input,spent_output FROM request_budgets")[0]
        self.assertEqual(budget, {"attempts": 3, "spent_input": 300, "spent_output": 60})
        worker_attempt = self.rows("SELECT request_envelope,response FROM call_attempts WHERE role='worker'")[0]
        self.assertIn("fs_mkdir", json.loads(worker_attempt["request_envelope"])["context"]["tool_names"])
        response = json.loads(worker_attempt["response"])
        self.assertTrue(response["tool_calls"][0]["arguments"]["parents"])

    def test_upgrading_template_does_not_grant_mkdir_to_existing_frozen_request(self):
        functions = self.directory / "functions"
        shutil.copytree(SOURCE / "config/examples/functions", functions)
        template = functions / "general/function.yaml"
        current_template = template.read_text(encoding="utf-8")
        template.write_text(re.sub(r"\bfs_mkdir,\s*", "", current_template), encoding="utf-8")
        request = self.submit(key="old-grant", functions=functions)
        frozen_before = self.frozen_rows(request)
        self.assertNotIn("fs_mkdir", json.loads(frozen_before["tool_allowlist"]))
        self.run_calls(request, [("note", {"text": "Bind the old frozen policy before the template upgrade."})])
        policy_before = self.rows("SELECT * FROM workspace_bindings")
        self.assertEqual(json.loads(policy_before[0]["execution_policy"])["workspace_limits"]["tool_schema_version"],
                         "wls-worker-tools-v1")
        old_facts = self.rows("SELECT * FROM operation_results ORDER BY op_id")
        before_budget = self.rows("SELECT attempts,spent_input FROM request_budgets WHERE request_id=?", (request,))[0]
        template.write_text(current_template, encoding="utf-8")
        self.run_calls(request, [("fs_mkdir", {"path": "not-retroactively-granted"})], error="invalid_input")
        self.assertEqual(self.frozen_rows(request), frozen_before)
        self.assertEqual(self.rows("SELECT * FROM workspace_bindings"), policy_before)
        self.assertEqual(self.rows("SELECT * FROM operation_results ORDER BY op_id"), old_facts)
        self.assertFalse((self.workspace / "not-retroactively-granted").exists())
        latest = self.rows("SELECT request_envelope FROM call_attempts WHERE request_id=? AND role='worker' "
                           "ORDER BY rowid DESC LIMIT 1", (request,))[0]
        self.assertNotIn("fs_mkdir", json.loads(latest["request_envelope"])["context"]["tool_names"])
        after_budget = self.rows("SELECT attempts,spent_input FROM request_budgets WHERE request_id=?", (request,))[0]
        self.assertEqual(after_budget["attempts"], before_budget["attempts"] + 1)
        self.assertEqual(after_budget["spent_input"], before_budget["spent_input"] + 100)
        new_request = self.submit(key="new-grant", functions=functions)
        self.assertIn("fs_mkdir", json.loads(self.frozen_rows(new_request)["tool_allowlist"]))
        self.assertEqual(hashlib.sha256(frozen_before["config_snapshot"].encode()).hexdigest(),
                         frozen_before["config_snapshot_sha256"])
        self.assertEqual(self.frozen_rows(request), frozen_before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
