#!/usr/bin/env python3
"""Actual CLI capacity pause/checkpoint; all databases and configs are disposable."""
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import unittest
if os.geteuid() == 0:
    raise SystemExit("ordinary user required")
spec = importlib.util.spec_from_file_location("capacity_helpers", Path(__file__).with_name("s1_cli_smoke.py"))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)

class CapacityCLI(helpers.OfflineCliSmoke):
    def setUp(self):
        super().setUp()
        self.config = self.directory / "system.yaml"
        data = (helpers.SOURCE / "config/examples/system.yaml").read_text()
        for old, new in (("max_teams_per_request: 4", "max_teams_per_request: 1"),
                         ("worker_max_tool_calls_per_order: 200", "worker_max_tool_calls_per_order: 4"),
                         ("manager_max_tool_calls_per_wake: 12", "manager_max_tool_calls_per_wake: 1"),
                         ("team_internal_rework_max: 3", "team_internal_rework_max: 0"),
                         ("business_events: 32768", "business_events: 128"),
                         ("control_events: 32768", "control_events: 64")):
            self.assertIn(old, data)
            data = data.replace(old, new)
        self.config.write_text(data)
        self.evidence = self.directory / "checkpoint.txt"
        self.evidence.write_text("host inspected settled operations and retained all original audit bytes")

    def small_request(self):
        request = self.submit(config=self.config, functions_dir=helpers.SOURCE / "config/examples/functions",
                              prompts_dir=helpers.SOURCE / "prompts")["request"]["request_id"]
        self.run_fake(request)
        return request

    def capacity(self, request):
        return self.invoke("capacity", "--db", self.database, "--request", request)

    def state(self):
        return self.rows("SELECT request_id,dispatcher_epoch,state_version FROM requests"), self.rows("SELECT * FROM events ORDER BY seq")

    def fill(self, request):
        task = self.rows("SELECT task_id FROM tasks WHERE request_id=?", (request,))[0]["task_id"]
        for _ in range(64):
            if self.capacity(request)["status"] == "WAITING_INPUT":
                return
            wait = self.invoke("wait", "--db", self.database, "--request", request, "--task", task,
                               "--reason", "explicit host waiting")
            self.invoke("wake", "--db", self.database, "--request", request, "--task", task,
                        "--wait-generation", wait["generation"])
        self.fail("expected bounded control window to pause")

    def checkpoint(self, request, *, window=0, key="checkpoint", error=None):
        return self.invoke("capacity-checkpoint", "--db", self.database, "--request", request,
                           "--expected-window", window, "--evidence-file", self.evidence, "--key", key, error=error)

    def test_readonly_and_frozen_source_removal(self):
        request = self.small_request()
        before = self.state(); original = self.capacity(request)
        self.config.unlink()
        self.assertEqual(self.capacity(request), original)
        self.assertEqual(self.state(), before)
        self.assertEqual(original["limits"]["control_events"], 64)

    def test_window_recovery_replay_preserves_audit_and_budget(self):
        request = self.small_request(); self.fill(request)
        before = self.state()[1]; budget = self.budget(request)
        self.assertTrue(self.rows("SELECT * FROM events WHERE type='capacity_warning'"))
        reply = self.checkpoint(request)
        self.assertEqual(reply["window"], 1)
        self.assertEqual(self.checkpoint(request), reply)
        self.assertEqual(len(self.rows("SELECT * FROM events WHERE type='capacity_checkpoint'")), 1)
        self.assertEqual(self.state()[1][:len(before)], before)
        self.assertEqual(self.budget(request), budget)
        resumed = self.run_fake(request)
        self.assertEqual([call["role"] for call in resumed["calls"]], ["worker"])
        self.assertEqual(resumed["budget"]["attempts"], budget["attempts"] + 1)
        before = self.state(); self.evidence.write_text("different intent")
        self.checkpoint(request, error="conflict")
        self.assertEqual(self.state(), before)

    def test_invalid_checkpoint_is_preflighted_without_takeover(self):
        request = self.small_request(); self.fill(request); before = self.state()
        self.checkpoint(request, window=7, error="conflict")
        self.assertEqual(self.state(), before)
        self.evidence.write_text("   ")
        self.checkpoint(request, error="invalid_input")
        self.assertEqual(self.state(), before)

    def test_unknown_effect_cannot_be_checkpointed_away(self):
        request = self.small_request(); self.fill(request)
        with sqlite3.connect(self.database) as db:
            db.execute("UPDATE ops SET status='UNKNOWN',state_version=state_version+1")
        before = self.state()
        self.checkpoint(request, error="unknown_outcome")
        self.assertEqual(self.state(), before)

    def test_unknown_model_accounting_cannot_be_checkpointed_away(self):
        request = self.small_request(); self.fill(request)
        with sqlite3.connect(self.database) as db:
            db.execute("UPDATE call_attempts SET unknown_cost=1,state_version=state_version+1")
        before = self.state()
        self.checkpoint(request, error="unknown_outcome")
        self.assertEqual(self.state(), before)

    def test_inconsistent_new_configuration_is_rejected_before_writer(self):
        request = self.small_request(); before = self.state()
        self.config.write_text(self.config.read_text().replace("operations: 4096", "operations: 1"))
        self.invoke("submit", "--db", self.database, "--key", "bad-admission", "--text-file",
                    helpers.FIXTURES / "s1_request.txt", "--config", self.config,
                    "--functions-dir", helpers.SOURCE / "config/examples/functions", "--prompts-dir", helpers.SOURCE / "prompts",
                    error="invalid_input")
        self.assertEqual(self.state(), before)
        self.assertEqual(self.capacity(request)["limits"]["operations"], 4096)

if __name__ == "__main__":
    suite = unittest.TestSuite(CapacityCLI(name) for name in sorted(CapacityCLI.__dict__) if name.startswith("test_"))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
