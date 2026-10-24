#!/usr/bin/env python3
"""Root-only local reconciliation; all UNKNOWN fixtures are disposable databases."""
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import unittest
if os.geteuid() == 0:
    raise SystemExit("ordinary user required")
spec = importlib.util.spec_from_file_location("offline_helpers", Path(__file__).with_name("s1_cli_smoke.py"))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)

class ReconcileCLI(helpers.OfflineCliSmoke):
    def unknown(self):
        request = self.submit()["request"]["request_id"]
        self.run_fake(request)
        # Mimic the persisted UNKNOWN reached after a host crash. This is a
        # CLI contract fixture, not evidence of a real strong sandbox run.
        with sqlite3.connect(self.database) as db:
            db.execute("UPDATE ops SET status='UNKNOWN',state_version=state_version+1")
        operation = self.rows("SELECT * FROM ops")[0]
        return request, operation

    def command(self, request, operation, *, evidence="host verified partial files and no remaining side effect", key="host-check", version=None, outcome="FAILED", error=None):
        path = self.directory / "evidence.txt"
        path.write_text(evidence, encoding="utf-8")
        return self.invoke("reconcile-op", "--db", self.database, "--request", request,
                           "--op", operation["op_id"], "--expected-version", version or operation["state_version"],
                           "--outcome", outcome, "--evidence-file", path, "--key", key, error=error)

    def state(self):
        return self.rows("SELECT request_id,state_version,dispatcher_epoch FROM requests"), self.rows("SELECT * FROM ops"), self.rows("SELECT * FROM events ORDER BY seq")

    def test_definite_failure_replays_and_worker_can_continue(self):
        request, operation = self.unknown()
        first = self.command(request, operation)
        self.assertEqual(first["status"], "FAILED")
        self.assertEqual(self.command(request, operation), first)
        records = self.rows("SELECT payload FROM events WHERE type='reconcile_op'")
        self.assertEqual(len(records), 1)
        self.assertIn("partial files", json.loads(records[0]["payload"])["host_evidence"])
        # A new actual CLI run acknowledges the same semantic order and writes
        # another SQLite note; the old operation itself is never re-executed.
        resumed = self.run_fake(request)
        self.assertEqual([c["role"] for c in resumed["calls"]], ["worker"])
        self.assertEqual(len(self.rows("SELECT * FROM ops")), 2)
        self.assertEqual(len(self.rows("SELECT * FROM instructions")), 1)

    def test_invalid_cas_outcome_and_evidence_do_not_take_over_writer(self):
        request, operation = self.unknown()
        before = self.state()
        self.command(request, operation, version=operation["state_version"]+1, error="conflict")
        self.assertEqual(self.state(), before)
        self.command(request, operation, outcome="UNKNOWN", error="invalid_input")
        self.assertEqual(self.state(), before)
        for evidence in ("", " \t\r\n", "x"*4097):
            # Oversized evidence is invalid control input, rejected before the Store.
            self.command(request, operation, evidence=evidence, error="invalid_input")
            self.assertEqual(self.state(), before)

    def test_wrong_request_cannot_reconcile_another_operation(self):
        request, operation = self.unknown()
        other = self.submit(key="unrelated")["request"]["request_id"]
        before = self.state()
        self.command(other, operation, error="unauthorized")
        self.assertEqual(self.state(), before)
        self.assertNotEqual(request, other)

    def test_changed_retry_evidence_conflicts_without_new_recovery(self):
        request, operation = self.unknown()
        self.command(request, operation)
        before = self.state()
        self.command(request, operation, evidence="different evidence", error="conflict")
        self.assertEqual(self.state(), before)

if __name__ == "__main__":
    suite = unittest.TestSuite(ReconcileCLI(name) for name in sorted(ReconcileCLI.__dict__) if name.startswith("test_"))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
