#!/usr/bin/env python3
"""Owned allocation recovery through real CLI processes and disposable SQLite/files.

The fixed model script creates only fixture tasks and accepted fixture bytes.
Injected SQLite failures exercise persisted allocation boundaries, not providers.
"""
from __future__ import annotations

from contextlib import closing
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import stat
import sys
import unittest

if os.geteuid() == 0:
    raise SystemExit("allocation recovery regressions require an ordinary user")

spec = importlib.util.spec_from_file_location("allocation_cleanup_helpers", Path(__file__).with_name("s16_cli_smoke.py"))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)
BINARY, SOURCE, FIXTURES = helpers.BINARY, helpers.SOURCE, helpers.FIXTURES


class AllocationRecoveryCLI(helpers.OwnedCleanupCLI):
    def snapshot_request(self, request):
        """Every table with direct or child ownership, including original bytes.

        Global metadata tables are included unchanged; no rowid assumptions are
        made for WITHOUT ROWID tables, and duplicate complete tuples are kept.
        """
        result = {}
        with closing(sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True)) as db:
            names = [row[0] for row in db.execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name")]
            columns = {name: {row[1] for row in db.execute(f'PRAGMA table_info("{name}")')} for name in names}
            parents = {"attempt_id": "call_attempts", "op_id": "ops", "team_id": "teams", "task_id": "tasks",
                       "experience_id": "experience_jobs", "delivery_id": "delivery_runtime",
                       "question_id": "questions", "resource_id": "owned_workspaces", "event_seq": "events"}
            for name in names:
                if "request_id" in columns[name]:
                    values = db.execute(f'SELECT * FROM "{name}" WHERE request_id=?', (request,)).fetchall()
                else:
                    owner = next(((key, parent) for key, parent in parents.items()
                                  if key in columns[name] and parent in names and "request_id" in columns[parent]), None)
                    if owner:
                        key, parent = owner
                        parent_key = "seq" if key == "event_seq" else key
                        values = db.execute(f'SELECT c.* FROM "{name}" c JOIN "{parent}" p '
                                            f'ON c."{key}"=p."{parent_key}" WHERE p.request_id=?', (request,)).fetchall()
                    else:
                        values = db.execute(f'SELECT * FROM "{name}"').fetchall()
                result[name] = sorted(values, key=repr)
        return result

    def inject(self, statement):
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute(statement)

    def resource(self, resource_id):
        value = self.rows("SELECT * FROM owned_workspaces WHERE resource_id=?", (resource_id,))[0]
        for field in ("intent", "identity", "inventory", "proof", "allocation_receipt", "abandonment"):
            if value[field] is not None:
                value[field] = json.loads(value[field])
        return value

    def paths(self, resource):
        intent = resource["intent"]
        parent = Path(intent["parent_identity"]["path"])
        return parent / intent["stage_leaf"], parent / intent["leaf"]

    def prepare_failed(self, phase="receipt", key="interrupted", request=None, task=None):
        if request is None:
            request = self.invoke("submit", "--db", self.database, "--key", key,
                                  "--text-file", FIXTURES / "s14_request.txt",
                                  "--constraints-file", FIXTURES / "s14_constraints.json",
                                  "--config", self.config, "--functions-dir", SOURCE / "config/examples/functions")["request"]["request_id"]
            self.pump(request, workspace=False, max_calls=1)
            task = self.view(request)["tasks"][0]["task_id"]
        condition = "NEW.allocation_receipt IS NOT NULL" if phase == "receipt" else "NEW.status='OWNED'"
        self.inject(f"CREATE TRIGGER allocation_fixture_failure BEFORE UPDATE ON owned_workspaces WHEN {condition} "
                    "BEGIN SELECT RAISE(ABORT,'injected allocation record failure'); END")
        self.invoke("allocate-workspace", "--db", self.database, "--request", request, "--task", task,
                    "--parent", self.owned_parent, "--key", key, error="conflict")
        self.inject("DROP TRIGGER allocation_fixture_failure")
        row = self.rows("SELECT resource_id FROM owned_workspaces WHERE request_id=? AND command_key=?", (request, key))[0]
        resource = self.resource(row["resource_id"])
        self.assertEqual(resource["status"], "PREPARED")
        return request, task, resource

    def abandon(self, request, resource, *, key="explicit-abandonment", reason="retain interrupted paths for inspection", error=None):
        return self.invoke("abandon-workspace", "--db", self.database, "--request", request,
                           "--resource", resource["resource_id"], "--expected-state-version", resource["state_version"],
                           "--key", key, "--reason", reason, error=error)

    def continue_owned(self, request, resource):
        self.workspace = Path(resource["identity"]["workspace_identity"]["path"])
        self.mapping = self.json_file("allocation-workspaces.json", {"alpha": str(self.workspace)})
        script = self.fixture()
        script["default"] = [step for step in script["default"]
                             if not (step["role"] == "dispatcher" and step["role_call_index"] == 1)]
        for step in script["default"]:
            if step["role"] == "dispatcher":
                step["role_call_index"] -= 1
        result = self.pump(request, self.json_file("allocation-continue.json", script))
        self.assertEqual(result["real_model_calls"], 0)
        self.assertEqual(self.view(request)["tasks"][0]["status"], "ACCEPTED")

    def test_current_allocation_receipt_private_modes_and_readonly_queries(self):
        request, task, resource = self.start_owned()
        self.assertEqual(self.rows("PRAGMA user_version"), [{"user_version": 408}])
        stage, final = self.paths(resource)
        self.assertFalse(stage.exists())
        self.assertEqual(stat.S_IMODE(final.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((final / "work").stat().st_mode), 0o700)
        receipt = resource["allocation_receipt"]
        self.assertTrue(receipt["durable"])
        self.assertEqual(receipt["stage_identity"]["inode"], final.stat().st_ino)
        self.assertEqual(receipt["workspace_identity"]["inode"], (final / "work").stat().st_ino)
        before = self.snapshot()
        self.invoke("owned-workspaces", "--db", self.database, "--request", request)
        self.invoke("closeout", "--db", self.database, "--request", request)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.invoke("allocate-workspace", "--db", self.database, "--request", request, "--task", task,
                                     "--parent", self.owned_parent, "--key", "owned-allocation")["allocation_receipt"], receipt)

    def test_failed_registration_recovers_same_final_directory_across_cli_processes(self):
        request, task, resource = self.prepare_failed("registration")
        stage, final = self.paths(resource)
        self.assertFalse(stage.exists())
        self.assertTrue(final.is_dir())
        before = (final.stat().st_ino, (final / "work").stat().st_ino, (final / "owner.json").read_bytes())
        repaired = self.invoke("allocate-workspace", "--db", self.database, "--request", request, "--task", task,
                               "--parent", self.owned_parent, "--key", "interrupted")
        self.assertEqual(repaired["status"], "OWNED")
        self.assertEqual(repaired["allocation_receipt"], resource["allocation_receipt"])
        self.assertEqual(before, (final.stat().st_ino, (final / "work").stat().st_ino, (final / "owner.json").read_bytes()))

    def test_failed_receipt_requires_explicit_abandonment_and_new_key(self):
        request, task, resource = self.prepare_failed()
        stage, final = self.paths(resource)
        self.assertIsNone(resource["allocation_receipt"])
        self.assertTrue(stage.is_dir())
        self.assertFalse(final.exists())
        original = (stage / "owner.json").read_bytes()
        self.invoke("allocate-workspace", "--db", self.database, "--request", request, "--task", task,
                    "--parent", self.owned_parent, "--key", "interrupted", error="conflict")
        abandoned = self.abandon(request, resource)
        self.assertEqual(abandoned["status"], "ABANDONED")
        self.assertIsNone(abandoned["proof"])
        self.assertEqual(self.abandon(request, resource), abandoned)
        self.abandon(request, resource, reason="changed reason", error="conflict")
        same = self.invoke("allocate-workspace", "--db", self.database, "--request", request, "--task", task,
                           "--parent", self.owned_parent, "--key", "interrupted")
        self.assertEqual(same, abandoned)
        fresh = self.invoke("allocate-workspace", "--db", self.database, "--request", request, "--task", task,
                            "--parent", self.owned_parent, "--key", "replacement")
        self.assertNotEqual(fresh["resource_id"], resource["resource_id"])
        self.assertEqual(fresh["status"], "OWNED")
        self.assertEqual((stage / "owner.json").read_bytes(), original)
        self.assertFalse(final.exists())
        self.assertEqual(len(self.rows("SELECT * FROM owned_workspaces")), 2)

    def test_invalid_abandonment_preflight_never_takes_over(self):
        request, _, resource = self.prepare_failed()
        other = self.admit(key="foreign-request")
        before = self.snapshot()
        self.abandon(request, resource, reason="   ", error="invalid_input")
        self.abandon(other, resource, error="unauthorized")
        wrong = dict(resource, state_version=resource["state_version"] + 1)
        self.abandon(request, wrong, error="conflict")
        missing = dict(resource, resource_id="res_missing")
        self.abandon(request, missing, error="invalid_input")
        self.assertEqual(self.snapshot(), before)

    def test_abandonment_after_cancel_and_missing_parent_preserves_original_bytes(self):
        request, _, resource = self.prepare_failed("registration")
        _, final = self.paths(resource)
        original = (final / "owner.json").read_bytes()
        self.invoke("cancel", "--db", self.database, "--request", request)
        retained = self.directory / "retained-parent"
        self.owned_parent.rename(retained)
        self.assertFalse(self.owned_parent.exists())
        result = self.abandon(request, resource)
        self.assertEqual(result["status"], "ABANDONED")
        self.assertEqual(result["allocation_receipt"], resource["allocation_receipt"])
        self.assertIsNone(result["proof"])
        self.assertEqual((retained / final.name / "owner.json").read_bytes(), original)
        self.assertEqual(self.view(request)["request"]["status"], "CANCELLED")
        closeout = self.invoke("closeout", "--db", self.database, "--request", request)
        self.assertFalse(closeout["cleanup_complete"])
        self.assertIn("abandoned", json.dumps(closeout).lower())

    def test_targeted_abandonment_preserves_all_sibling_rows_and_cleanup_lease(self):
        sibling, *_ = self.ready()
        request, task, resource = self.prepare_failed(key="target")
        before = self.snapshot_request(sibling)
        self.abandon(request, resource)
        self.assertEqual(self.snapshot_request(sibling), before)
        self.invoke("allocate-workspace", "--db", self.database, "--request", request, "--task", task,
                    "--parent", self.owned_parent, "--key", "replacement")
        self.assertEqual(self.snapshot_request(sibling), before)
        self.assertEqual(self.view(sibling)["request"]["status"], "DELIVERED")
        self.assertTrue(self.rows("SELECT * FROM cleanup_jobs WHERE request_id=?", (sibling,)))

    def test_abandoned_residual_survives_active_owned_cleanup_and_frozen_manifest(self):
        request, task, resource = self.prepare_failed()
        stage, _ = self.paths(resource)
        original = (stage / "owner.json").read_bytes()
        abandoned = self.abandon(request, resource)
        owned = self.invoke("allocate-workspace", "--db", self.database, "--request", request, "--task", task,
                            "--parent", self.owned_parent, "--key", "replacement")
        self.continue_owned(request, owned)
        delivery = self.publish(request)
        self.ack(request, delivery)
        first = self.cleanup(request, limit=1)
        self.assertFalse(first["workspace_cleanup_recorded"])
        raw_manifest = self.rows("SELECT manifest FROM workspace_cleanup_runs")[0]["manifest"]
        manifest = json.loads(raw_manifest)
        self.assertEqual([target["resource_id"] for target in manifest["targets"]], [owned["resource_id"]])
        done = self.cleanup(request)
        self.assertTrue(done["workspace_cleanup_recorded"])
        self.assertTrue(done["workspace_absence_verified_now"])
        self.assertFalse(done["cleanup_complete"])
        self.assertEqual(self.rows("SELECT manifest FROM workspace_cleanup_runs")[0]["manifest"], raw_manifest)
        self.assertEqual(self.resource(resource["resource_id"]), abandoned)
        self.assertEqual((stage / "owner.json").read_bytes(), original)
        self.assertTrue((stage / "work").is_dir())
        self.assertFalse(self.workspace.exists())
        self.assertIn("abandoned", json.dumps(done).lower())
        self.assert_bundle_bytes(delivery)

    def test_owned_resource_abandonment_rejected_without_write_takeover(self):
        request, _, owned = self.start_owned()
        before = self.snapshot()
        self.abandon(request, owned, error="not_ready")
        self.assertEqual(self.snapshot(), before)
        self.assertTrue(Path(owned["identity"]["workspace_identity"]["path"]).is_dir())

    def test_abandoned_key_replays_when_original_parent_is_missing_or_replaced(self):
        request, task, resource = self.prepare_failed()
        abandoned = self.abandon(request, resource)
        retained = self.directory / "retained-original-parent"
        self.owned_parent.rename(retained)
        original_rows = self.rows("SELECT * FROM owned_workspaces")
        allocation_events = self.rows("SELECT * FROM events WHERE type IN "
                                      "('prepare_owned_workspace','allocation_receipt','allocation_registered','allocation_abandoned') ORDER BY seq")
        for replaced in (False, True):
            if replaced:
                self.owned_parent.write_bytes(b"unrelated replacement is a regular file")
            result = self.invoke("allocate-workspace", "--db", self.database, "--request", request, "--task", task,
                                 "--parent", self.owned_parent, "--key", "interrupted")
            self.assertEqual(result, abandoned)
            self.assertEqual(self.rows("SELECT * FROM owned_workspaces"), original_rows)
            self.assertEqual(self.rows("SELECT * FROM events WHERE type IN "
                                       "('prepare_owned_workspace','allocation_receipt','allocation_registered','allocation_abandoned') ORDER BY seq"),
                             allocation_events)
            if replaced:
                self.assertEqual(self.owned_parent.read_bytes(), b"unrelated replacement is a regular file")
            else:
                self.assertFalse(self.owned_parent.exists())
        self.invoke("allocate-workspace", "--db", self.database, "--request", request, "--task", task,
                    "--parent", retained, "--key", "interrupted", error="conflict")
        self.assertEqual(self.rows("SELECT * FROM owned_workspaces"), original_rows)
        self.assertEqual((retained / resource["intent"]["stage_leaf"] / "owner.json").read_bytes(),
                         json.dumps(resource["intent"]["marker"], sort_keys=True, separators=(",", ":")).encode())


if __name__ == "__main__":
    suite = unittest.TestSuite(AllocationRecoveryCLI(name) for name in sorted(AllocationRecoveryCLI.__dict__)
                               if name.startswith("test_"))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
