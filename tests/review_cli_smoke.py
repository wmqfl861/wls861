#!/usr/bin/env python3
"""Audit fixes at the real CLI boundary; only disposable data and FakeModel.

The test environment creates private fixtures explicitly. Individual tests can
still set insecure modes and verify that the production boundary rejects them.
"""
from __future__ import annotations
import importlib.util
import os
from pathlib import Path
import unittest

if os.geteuid() == 0:
    raise SystemExit("review permission regressions require a non-root test user")
spec = importlib.util.spec_from_file_location("acceptance_helpers", Path(__file__).with_name("s15_cli_smoke.py"))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)
SOURCE, FIXTURES = helpers.SOURCE, helpers.FIXTURES


class ReviewCLI(helpers.AcceptanceCliSmoke):
    def planned(self, key="planned"):
        request = self.admit(key)
        self.pump(request, workspace=False, max_calls=1)
        return request, self.view(request)["tasks"][0]["task_id"]

    def test_malformed_admission_does_not_recover_unrelated_request(self):
        self.admit("unrelated")
        before = self.snapshot()
        for value in ([{"a": 1, "b": 2, "c": 3}],
                      [{"constraint_id": 5, "text": "x", "source_revision": 1}],
                      [{"constraint_id": "c", "text": "x", "source_revision": True}]):
            failure = self.invoke("submit", "--db", self.database, "--key", "bad",
                                 "--text-file", FIXTURES / "s14_request.txt",
                                 "--constraints-file", self.json_file("bad.json", value), error="invalid_input")
            self.assertNotIn("boost", failure["message"].lower())
            self.assertEqual(self.snapshot(), before)

    def test_malformed_amendment_does_not_recover_any_request(self):
        request, _ = self.planned()
        self.admit("unrelated")
        before = self.snapshot()
        self.invoke("amend", "--db", self.database, "--request", request,
                    "--text-file", FIXTURES / "s14_request.txt", "--constraints-file",
                    self.json_file("bad.json", [{"a": 1, "b": 2, "c": 3}]), error="invalid_input")
        self.assertEqual(self.snapshot(), before)

    def test_decimal_budget_uses_identical_store_and_cli_conversion(self):
        config = self.directory / "money.yaml"
        text = (SOURCE / "config/examples/system.yaml").read_text(encoding="utf-8")
        self.assertIn("max_cost_usd: null", text)
        config.write_text(text.replace("max_cost_usd: null", "max_cost_usd: 2.01"), encoding="utf-8")
        request = self.invoke("submit", "--db", self.database, "--key", "money",
                              "--text-file", FIXTURES / "s14_request.txt", "--config", config,
                              "--functions-dir", SOURCE / "config/examples/functions")["request"]["request_id"]
        self.assertEqual(self.rows("SELECT cost_limit FROM request_budgets WHERE request_id=?", (request,))[0]["cost_limit"], 2010000)

    def test_invalid_and_foreign_task_controls_do_not_fence_other_work(self):
        request, task = self.planned()
        other = self.admit("other")
        before = self.snapshot()
        for command in ("wait", "wake"):
            extra = ["--reason", "hold"] if command == "wait" else ["--wait-generation", 1]
            self.invoke(command, "--db", self.database, "--request", request,
                        "--task", "missing-task", *extra, error=("invalid_input", "not_found"))
            self.invoke(command, "--db", self.database, "--request", other,
                        "--task", task, *extra, error="unauthorized")
            self.assertEqual(self.snapshot(), before)

    def test_duplicate_wait_and_stale_wake_are_read_only(self):
        request, task = self.planned()
        waiting = self.invoke("wait", "--db", self.database, "--request", request,
                              "--task", task, "--reason", "operator hold")
        before = self.snapshot()
        self.invoke("wait", "--db", self.database, "--request", request,
                    "--task", task, "--reason", "again", error="not_ready")
        self.invoke("wake", "--db", self.database, "--request", request,
                    "--task", task, "--wait-generation", waiting["generation"] + 1, error="stale")
        self.assertEqual(self.snapshot(), before)
        awake = self.invoke("wake", "--db", self.database, "--request", request,
                            "--task", task, "--wait-generation", waiting["generation"])
        self.assertEqual(awake["status"], "AWAKE")

    def test_config_shape_valid_does_not_claim_unenforced_limits(self):
        checked = self.invoke("config", "check")
        explained = self.invoke("config", "explain", "--role", "worker")
        self.assertTrue(checked["valid"])
        support = checked["runtime_support"]
        self.assertEqual(support, explained["runtime_support"])
        self.assertFalse(support["all_configured_controls_enforced"])
        self.assertFalse(support["real_calls_ready"])
        paths = {entry["path"] for entry in support["unenforced_settings"]}
        self.assertIn("system.budget_defaults.max_wall_minutes", paths)
        self.assertIn("system.context.compaction_at_fraction", paths)
        self.assertIn("system.limits.max_concurrent_ops", paths)

    def test_open_blocking_question_cannot_trigger_writer_takeover(self):
        request = self.admit()
        script = self.fixture("s14_fake_ask.json")
        script["default"][0]["tool_calls"][0]["arguments"]["nodes"] = script["default"][0]["tool_calls"][0]["arguments"]["nodes"][:1]
        script["nodes"].pop("beta", None)
        self.pump(request, self.json_file("ask.json", script), max_calls=3)
        task = self.view(request)["tasks"][0]["task_id"]
        self.assertEqual(self.rows("SELECT count(*) AS n FROM question_runtime WHERE status='OPEN' AND blocking=1")[0]["n"], 1)
        before = self.snapshot()
        self.invoke("wait", "--db", self.database, "--request", request,
                    "--task", task, "--reason", "replace question", error="not_ready")
        self.invoke("wake", "--db", self.database, "--request", request,
                    "--task", task, "--wait-generation", 1, error="not_ready")
        self.assertEqual(self.snapshot(), before)

    def test_twenty_failed_waits_leave_idle_request_unchanged(self):
        request = self.admit("idle")
        before = self.snapshot()
        for _ in range(20):
            self.invoke("wait", "--db", self.database, "--request", request,
                        "--task", "missing-task", "--reason", "bad target", error=("invalid_input", "not_found"))
        self.assertEqual(self.snapshot(), before)

    def test_insecure_workspace_still_refused(self):
        request, _ = self.planned()
        self.workspace.chmod(0o770)
        self.invoke("pump-fake", "--db", self.database, "--request", request,
                    "--script", FIXTURES / "s15_fake_complete.json",
                    "--workspaces-file", self.mapping, error="unauthorized")


if __name__ == "__main__":
    suite = unittest.TestSuite(ReviewCLI(name) for name in sorted(ReviewCLI.__dict__) if name.startswith("test_"))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
