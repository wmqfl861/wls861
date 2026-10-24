#!/usr/bin/env python3
"""Cross-process epoch-only resume with deliberately absent TeamLead scripts."""
import importlib.util
import json
import os
from pathlib import Path
import unittest
if os.geteuid() == 0:
    raise SystemExit("ordinary user required")
spec = importlib.util.spec_from_file_location("offline_helpers", Path(__file__).with_name("s1_cli_smoke.py"))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)

class EpochResume(helpers.OfflineCliSmoke):
    def worker_script(self):
        source = json.loads((helpers.FIXTURES / "s1_fake_initial.json").read_text())
        script = self.directory / "worker-only.json"
        script.write_text(json.dumps([step for step in source if step["role"] == "worker"]))
        return script

    def test_legacy_reopen_keeps_order_and_does_not_call_teamlead(self):
        request = self.submit()["request"]["request_id"]
        self.run_fake(request)
        original = self.rows("SELECT body,instruction_revision FROM instructions")
        for _ in range(2):
            result = self.invoke("run-fake", "--db", self.database, "--request", request,
                                 "--script", self.worker_script())
            self.assertEqual([c["role"] for c in result["calls"]], ["worker"])
        self.assertEqual(self.rows("SELECT body,instruction_revision FROM instructions"), original)
        self.assertEqual(len(self.rows("SELECT attempt_id FROM call_attempts WHERE role='team_lead'")), 1)
        self.assertEqual(self.show(request)["tasks"][0]["applied_instruction_revision"], 1)
        self.assertEqual(self.rows("SELECT tool_calls_used FROM teams")[0]["tool_calls_used"], 3)

    def test_pending_amend_cannot_reuse_old_semantics(self):
        request = self.submit()["request"]["request_id"]
        self.run_fake(request)
        self.amend(request)
        before = len(self.rows("SELECT attempt_id FROM call_attempts"))
        self.invoke("run-fake", "--db", self.database, "--request", request,
                    "--script", self.worker_script(), error="invalid_input")
        self.assertEqual(len(self.rows("SELECT attempt_id FROM call_attempts")), before)
        self.assertEqual(self.show(request)["tasks"][0]["instruction_revision"], 1)

if __name__ == "__main__":
    suite = unittest.TestSuite(EpochResume(name) for name in sorted(EpochResume.__dict__) if name.startswith("test_"))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
