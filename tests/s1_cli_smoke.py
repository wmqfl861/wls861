#!/usr/bin/env python3
"""Actual subprocess/SQLite smoke tests for the bounded offline S1 CLI.

Every write is confined to a TemporaryDirectory. Fixtures are explicit fake
responses; no provider, environment command, shell business tool or delivery
is executed. Existing project assets are read only.
"""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest


if len(sys.argv) != 3:
    raise SystemExit("usage: s1_cli_smoke.py WLS_BINARY SOURCE_ROOT")
BINARY = Path(sys.argv[1]).resolve()
SOURCE = Path(sys.argv[2]).resolve()
sys.argv = [sys.argv[0]]
FIXTURES = SOURCE / "tests" / "fixtures"


class OfflineCliSmoke(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="wls-s1-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.database = self.directory / "runtime.sqlite"

    def invoke(self, *arguments, error=None):
        command = [str(BINARY), *(str(argument) for argument in arguments),
                   "--source-root", str(SOURCE)]
        completed = subprocess.run(command, cwd=self.directory, capture_output=True,
                                   text=True, encoding="utf-8", timeout=30, check=False)
        if error is None:
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stderr, "")
            self.assertTrue(completed.stdout.strip(), "CLI did not emit its JSON result")
            return json.loads(completed.stdout)
        self.assertNotEqual(completed.returncode, 0, "invalid CLI input unexpectedly succeeded")
        self.assertEqual(completed.stdout, "", "an error must not emit a success result")
        self.assertEqual(len(completed.stderr.strip().splitlines()), 1, completed.stderr)
        failure = json.loads(completed.stderr)
        self.assertEqual(failure["error"], error, failure)
        self.assertTrue(failure["message"])
        return failure

    def submit(self, *, key="request-one", **extra):
        arguments = ["submit", "--db", self.database, "--key", key,
                     "--text-file", FIXTURES / "s1_request.txt",
                     "--constraints-file", FIXTURES / "s1_constraints.json"]
        for option, value in extra.items():
            arguments.extend(["--" + option.replace("_", "-"), value])
        return self.invoke(*arguments)

    def show(self, request_id):
        return self.invoke("show", "--db", self.database, "--request", request_id)

    def budget(self, request_id):
        return self.invoke("budget", "--db", self.database, "--request", request_id)

    def run_fake(self, request_id, *, amended=False, disposition=None, error=None):
        arguments = ["run-fake", "--db", self.database, "--request", request_id,
                     "--script", FIXTURES / ("s1_fake_amended.json" if amended else "s1_fake_initial.json")]
        if disposition is not None:
            arguments.extend(["--disposition", disposition])
        return self.invoke(*arguments, error=error)

    def amend(self, request_id):
        return self.invoke("amend", "--db", self.database, "--request", request_id,
                           "--text-file", FIXTURES / "s1_amendment.txt",
                           "--constraints-file", FIXTURES / "s1_amended_constraints.json")

    def rows(self, query, arguments=()):
        with sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True) as connection:
            connection.row_factory = sqlite3.Row
            return [dict(row) for row in connection.execute(query, arguments)]

    def test_config_and_schema_commands_run_from_another_directory(self):
        checked = self.invoke("config", "check")
        self.assertTrue(checked["valid"])
        self.assertFalse(checked["real_calls_enabled"])
        self.assertFalse(checked["environment_checks_executed"])
        explained = self.invoke("config", "explain", "--role", "worker")
        self.assertEqual(explained["snapshot_sha256"], checked["snapshot_sha256"])
        self.assertEqual(explained["role"], "worker")
        self.assertEqual(explained["profile"]["adapter"], "fake")
        complete = self.invoke("schema", "check", "--schema", "plan", "--input",
                               SOURCE / "schemas" / "examples" / "plan.json")
        self.assertTrue(complete["valid"])
        self.assertEqual(complete["semantic_acceptance"], "not_evaluated")
        script = json.loads((FIXTURES / "s1_fake_initial.json").read_text())
        model_plan = script[0]["tool_calls"][0]["arguments"]
        projected_file = self.directory / "model-plan.json"
        projected_file.write_text(json.dumps(model_plan, ensure_ascii=False), encoding="utf-8")
        self.assertTrue(self.invoke("schema", "check", "--schema", "plan", "--input",
                                    projected_file, "--model-input")["valid"])
        model_plan["plan_revision"] = 999
        projected_file.write_text(json.dumps(model_plan, ensure_ascii=False), encoding="utf-8")
        self.invoke("schema", "check", "--schema", "plan", "--input", projected_file,
                    "--model-input", error="invalid_input")
        self.assertFalse(self.database.exists())

    def test_submit_exact_key_replays_and_changed_input_conflicts(self):
        first = self.submit()
        second = self.submit()
        request_id = first["request"]["request_id"]
        self.assertEqual(second["request"]["request_id"], request_id)
        self.assertEqual(first["request"]["config_snapshot_sha256"],
                         second["request"]["config_snapshot_sha256"])
        self.assertEqual(len(second["snapshot"]["texts"]), 1)
        self.assertEqual(second["snapshot"]["texts"][0]["text"],
                         (FIXTURES / "s1_request.txt").read_text())
        altered = self.directory / "different-request.txt"
        altered.write_text((FIXTURES / "s1_request.txt").read_text() + "新增另一个目标。", encoding="utf-8")
        self.invoke("submit", "--db", self.database, "--key", "request-one",
                    "--text-file", altered, "--constraints-file", FIXTURES / "s1_constraints.json",
                    error="conflict")
        self.assertEqual(len(self.rows("SELECT request_id FROM requests")), 1)
        self.assertEqual(self.budget(request_id)["attempts"], 0)

    def test_read_only_views_do_not_fence_or_append_events(self):
        request_id = self.submit()["request"]["request_id"]
        self.run_fake(request_id)
        before = self.show(request_id)
        events_before = self.invoke("events", "--db", self.database, "--request", request_id, "--limit", 1000)
        self.budget(request_id)
        self.invoke("config-snapshot", "--db", self.database, "--request", request_id, "--role", "worker")
        self.invoke("pending", "--db", self.database)
        after = self.show(request_id)
        events_after = self.invoke("events", "--db", self.database, "--request", request_id, "--limit", 1000)
        self.assertEqual(after, before)
        self.assertEqual(events_after, events_before)
        self.assertTrue(events_before)
        last_sequence = events_before[-1]["seq"]
        self.assertEqual(self.invoke("events", "--db", self.database, "--request", request_id,
                                     "--after", last_sequence), [])

    def test_three_role_fake_run_persists_attempts_context_and_note(self):
        admission = self.submit()
        request_id = admission["request"]["request_id"]
        result = self.run_fake(request_id)
        self.assertEqual([call["role"] for call in result["calls"]], ["dispatcher", "team_lead", "worker"])
        self.assertEqual(result["budget"]["attempts"], 3)
        self.assertEqual(result["budget"]["spent_input"], 300)
        self.assertEqual(result["budget"]["spent_output"], 60)
        self.assertEqual(result["budget"]["spent_cost"], 0)
        self.assertEqual(result["real_model_calls"], 0)
        self.assertFalse(result["business_delivered"])
        self.assertFalse(result["environment_checks_executed"])
        attempts = self.rows("SELECT * FROM call_attempts WHERE request_id=? ORDER BY created_at,attempt_id", (request_id,))
        self.assertEqual(len(attempts), 3)
        for attempt in attempts:
            self.assertEqual(attempt["status"], "COMPLETED")
            self.assertNotEqual(attempt["call_id"], attempt["attempt_id"])
            request = json.loads(attempt["request_envelope"])
            response = json.loads(attempt["response"])
            self.assertEqual(request["binding"], response["binding"])
            self.assertEqual(request["config_snapshot_id"], admission["request"]["config_snapshot_sha256"])
            self.assertIn("保留原始需求", request["context"]["dynamic_suffix"])
            self.assertEqual(len(request["context"]["prefix_sha256"]), 64)
        operations = self.rows("SELECT o.tool_id,o.status,o.side_effect,i.arguments,i.evidence_ref "
                               "FROM ops o JOIN operation_intents i USING(op_id) WHERE o.request_id=?", (request_id,))
        self.assertEqual(len(operations), 1)
        self.assertEqual((operations[0]["tool_id"], operations[0]["status"], operations[0]["side_effect"]),
                         ("note", "SUCCEEDED", "none"))
        self.assertIn("离线", json.loads(operations[0]["arguments"])["text"])
        self.assertTrue(operations[0]["evidence_ref"])
        self.assertFalse((self.directory / "deliveries").exists())

    def test_amend_requires_disposition_without_spending_then_updates(self):
        request_id = self.submit()["request"]["request_id"]
        self.run_fake(request_id)
        old_task = self.show(request_id)["tasks"][0]
        self.amend(request_id)
        before = self.budget(request_id)
        self.run_fake(request_id, amended=True, error="invalid_input")
        self.assertEqual(self.budget(request_id), before)
        paused = self.show(request_id)
        self.assertEqual(paused["request"]["requirements_revision"], 2)
        self.assertTrue(paused["tasks"][0]["paused_for_change"])
        updated = self.run_fake(request_id, amended=True, disposition="update")
        self.assertEqual(len(updated["calls"]), 3)
        self.assertEqual(updated["budget"]["attempts"], 6)
        current = self.show(request_id)
        self.assertEqual([item["text"] for item in current["snapshot"]["texts"]],
                         [(FIXTURES / "s1_request.txt").read_text(), (FIXTURES / "s1_amendment.txt").read_text()])
        task = current["tasks"][0]
        self.assertEqual(task["task_id"], old_task["task_id"])
        self.assertEqual(task["based_on_requirements_revision"], 2)
        self.assertGreater(task["task_revision"], old_task["task_revision"])
        self.assertEqual(task["applied_task_revision"], task["task_revision"])
        self.assertEqual(task["applied_plan_revision"], task["plan_revision"])
        self.assertEqual(task["applied_instruction_revision"], task["instruction_revision"])
        self.assertFalse(task["paused_for_change"])
        self.assertFalse(updated["business_delivered"])

    def test_reopen_uses_frozen_config_after_source_files_are_removed(self):
        copied_config = self.directory / "configuration"
        copied_prompts = self.directory / "role-prompts"
        shutil.copytree(SOURCE / "config" / "examples", copied_config)
        shutil.copytree(SOURCE / "prompts", copied_prompts)
        admission = self.submit(config=copied_config / "system.yaml",
                                functions_dir=copied_config / "functions", prompts_dir=copied_prompts)
        request_id = admission["request"]["request_id"]
        frozen_hash = admission["request"]["config_snapshot_sha256"]
        # Only disposable copies are removed. A new CLI process must reconstruct
        # the complete role input from SQLite, without consulting these paths.
        shutil.rmtree(copied_config)
        shutil.rmtree(copied_prompts)
        self.assertFalse(copied_config.exists())
        result = self.run_fake(request_id)
        self.assertEqual(result["config_snapshot_sha256"], frozen_hash)
        self.assertTrue(all(call["config_snapshot_sha256"] == frozen_hash for call in result["calls"]))
        explained = self.invoke("config-snapshot", "--db", self.database, "--request", request_id)
        self.assertEqual(explained["snapshot_sha256"], frozen_hash)
        self.assertEqual(self.show(request_id)["request"]["config_snapshot_sha256"], frozen_hash)

    def test_budget_exhaustion_preserves_cancel_control(self):
        request_id = self.submit(max_attempts=1)["request"]["request_id"]
        self.run_fake(request_id, error="quota")
        self.assertEqual(self.budget(request_id)["attempts"], 1)
        self.invoke("cancel", "--db", self.database, "--request", request_id)
        cancelled = self.show(request_id)
        self.assertEqual(cancelled["request"]["status"], "CANCELLED")
        self.assertEqual(cancelled["budget"]["attempts"], 1)
        self.assertEqual(cancelled["budget"]["max_attempts"], 1)
        self.assertNotEqual(cancelled["request"]["delivery_status"], "CONFIRMED")

    def test_duplicate_unknown_and_bad_options_are_structured_errors(self):
        self.invoke("unknown-command", "--db", self.database, error="invalid_input")
        self.invoke("init", "--db", self.database, "--db", self.database, error="invalid_input")
        self.invoke("init", "--db", self.database, "--unexpected", "value", error="invalid_input")
        self.invoke("submit", "--db", self.database, "--key", "bad",
                    "--text-file", FIXTURES / "s1_request.txt", "--max-attempts", "nan", error="invalid_input")
        self.assertFalse(self.database.exists(), "option validation must precede writer initialization")


if __name__ == "__main__":
    unittest.main(verbosity=2)
