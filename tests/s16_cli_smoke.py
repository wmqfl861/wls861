#!/usr/bin/env python3
"""Owned cleanup against the real CLI and disposable files only.

Reuses the S1.5 invocation helpers; runs ONLY this class's own test methods.
No production cleanup, providers, mounts or deployment services are touched.
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
    raise SystemExit("S1.6 permission regressions require a non-root test user; no fixtures were created")

arguments = list(sys.argv)
spec = importlib.util.spec_from_file_location("s15_helpers", Path(__file__).with_name("s15_cli_smoke.py"))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)
BINARY, SOURCE, FIXTURES = helpers.BINARY, helpers.SOURCE, helpers.FIXTURES


class OwnedCleanupCLI(helpers.AcceptanceCliSmoke):
    def setUp(self):
        super().setUp()
        self.owned_parent = self.directory / "owned"
        self.owned_parent.mkdir(mode=0o700)
        self.config = self.directory / "cleanup.yaml"
        content = (SOURCE / "config/examples/system.yaml").read_text(encoding="utf-8")
        assert "cleanup:\n  enabled: false" in content
        self.config.write_text(content.replace("cleanup:\n  enabled: false", "cleanup:\n  enabled: true"), encoding="utf-8")

    def start_owned(self, enabled=True):
        extra = ["--config", self.config, "--functions-dir", SOURCE / "config/examples/functions"] if enabled else []
        request = self.invoke("submit", "--db", self.database, "--key", "owned-request",
                              "--text-file", FIXTURES / "s14_request.txt", "--constraints-file", FIXTURES / "s14_constraints.json", *extra)["request"]["request_id"]
        self.pump(request, workspace=False, max_calls=1)
        task = self.view(request)["tasks"][0]["task_id"]
        resource = self.invoke("allocate-workspace", "--db", self.database, "--request", request,
                               "--task", task, "--parent", self.owned_parent, "--key", "owned-allocation")
        self.workspace = Path(resource["identity"]["workspace_identity"]["path"])
        self.mapping = self.json_file("owned-mapping.json", {"alpha": str(self.workspace)})
        script = self.fixture()
        script["default"] = [step for step in script["default"] if not (step["role"] == "dispatcher" and step["role_call_index"] == 1)]
        for step in script["default"]:
            if step["role"] == "dispatcher":
                step["role_call_index"] -= 1
        self.continuation = self.json_file("continue-owned.json", script)
        return request, task, resource

    def ready(self, enabled=True):
        request, task, resource = self.start_owned(enabled)
        self.pump(request, self.continuation)
        self.assertEqual(self.view(request)["tasks"][0]["status"], "ACCEPTED")
        delivery = self.publish(request)
        self.ack(request, delivery)
        return request, task, resource, delivery

    def cleanup(self, request, limit=1024, error=None):
        return self.invoke("cleanup-workspaces", "--db", self.database, "--request", request,
                           "--confirm-request", request, "--max-unlinks", limit, error=error)

    def test_exact_cleanup_keeps_original_delivery_and_private_evidence(self):
        request, _, resource, delivery = self.ready()
        result = self.cleanup(request)
        self.assertTrue(result["workspace_cleanup_recorded"])
        self.assertTrue(result["workspace_absence_verified_now"])
        self.assertFalse(result["cleanup_complete"])
        self.assertTrue(result["private_data_retained"])
        self.assertFalse(self.workspace.exists())
        self.assertTrue((self.workspace.parent / "owner.json").exists())
        self.assert_bundle_bytes(delivery)
        self.assertEqual(self.rows("SELECT status FROM cleanup_jobs"), [{"status": "PENDING"}])
        self.assertEqual(self.rows("SELECT status FROM requests"), [{"status": "DELIVERED"}])
        self.assertEqual(self.cleanup(request)["pass_removed_entries"], 0)
        self.assertEqual(self.rows("SELECT resource_id FROM owned_workspaces")[0]["resource_id"], resource["resource_id"])

    def test_default_disabled_rejects_without_writer_takeover(self):
        request, *_ = self.ready(enabled=False)
        before = self.snapshot()
        self.cleanup(request, error="not_ready")
        self.assertEqual(self.snapshot(), before)
        self.assertTrue((self.workspace / "result.txt").exists())

    def test_read_queries_and_invalid_confirmation_do_not_mutate(self):
        request, *_ = self.ready()
        before = self.snapshot()
        self.invoke("owned-workspaces", "--db", self.database, "--request", request)
        self.invoke("closeout", "--db", self.database, "--request", request)
        self.invoke("cleanup-workspaces", "--db", self.database, "--request", request,
                    "--confirm-request", "wrong", error="invalid_input")
        self.assertEqual(self.snapshot(), before)
        self.assertTrue(self.workspace.exists())

    def test_no_receipt_cannot_delete(self):
        request, *_ = self.start_owned()
        self.pump(request, self.continuation)
        self.cleanup(request, error="not_ready")
        self.assertTrue((self.workspace / "result.txt").exists())

    def test_missing_delivery_is_not_recreated(self):
        request, _, _, delivery = self.ready()
        path = self.package(delivery)
        retained = self.directory / "retained-package"
        permissions = stat.S_IMODE(path.stat().st_mode)
        # Fixture-only fault injection; cross-parent rename needs owner write.
        path.chmod(permissions | stat.S_IWUSR)
        path.rename(retained)
        retained.chmod(permissions)
        self.assertTrue((retained / "manifest.json").is_file())
        self.cleanup(request, error=("storage", "invalid_input", "conflict"))
        self.assertFalse(path.exists())
        self.assertTrue(self.workspace.exists())

    def test_corrupt_delivery_blocks_deletion(self):
        request, _, _, delivery = self.ready()
        manifest = self.package(delivery) / "manifest.json"
        permissions = stat.S_IMODE(manifest.stat().st_mode)
        manifest.chmod(permissions | stat.S_IWUSR)
        manifest.write_text("{}", encoding="utf-8")
        manifest.chmod(permissions)
        self.assertEqual(manifest.read_text(encoding="utf-8"), "{}")
        self.cleanup(request, error="conflict")
        self.assertTrue(self.workspace.exists())

    def test_delivery_stays_read_only_after_cleanup(self):
        request, _, _, delivery = self.ready()
        package = self.package(delivery)
        before = {path.name: (stat.S_IMODE(path.stat().st_mode), path.read_bytes())
                  for path in package.iterdir()}
        self.assertEqual(stat.S_IMODE(package.stat().st_mode), 0o500)
        self.assertTrue(all(mode == 0o400 for mode, _ in before.values()))
        self.assertTrue(self.cleanup(request)["workspace_absence_verified_now"])
        self.assertEqual(stat.S_IMODE(package.stat().st_mode), 0o500)
        after = {path.name: (stat.S_IMODE(path.stat().st_mode), path.read_bytes())
                 for path in package.iterdir()}
        self.assertEqual(after, before)
        self.assert_bundle_bytes(delivery)

    def test_bounded_pass_and_cross_process_resume(self):
        request, *_ = self.ready()
        (self.workspace / "z-extra").write_text("fixture only", encoding="utf-8")
        first = self.cleanup(request, limit=1)
        self.assertFalse(first["workspace_cleanup_recorded"])
        saved = self.rows("SELECT inventory FROM owned_workspaces")[0]["inventory"]
        self.assertTrue(self.cleanup(request)["workspace_cleanup_recorded"])
        self.assertEqual(self.rows("SELECT inventory FROM owned_workspaces")[0]["inventory"], saved)

    def test_resume_does_not_authorize_new_files(self):
        request, *_ = self.ready()
        (self.workspace / "z-old").write_text("original", encoding="utf-8")
        self.cleanup(request, limit=1)
        (self.workspace / "a-new").write_text("unapproved", encoding="utf-8")
        self.cleanup(request, error="conflict")
        self.assertTrue((self.workspace / "result.txt").exists())
        self.assertTrue((self.workspace / "a-new").exists())

    def test_failed_checkpoint_after_unlink_recovers(self):
        request, *_ = self.ready()
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute("CREATE TRIGGER fail_cleanup BEFORE UPDATE ON owned_workspaces WHEN NEW.proof IS NOT NULL "
                       "BEGIN SELECT RAISE(ABORT,'fixture record failure'); END")
        self.cleanup(request, error="conflict")
        self.assertFalse(self.workspace.exists())
        self.assertEqual(self.rows("SELECT status FROM owned_workspaces"), [{"status": "DELETING"}])
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute("DROP TRIGGER fail_cleanup")
        self.assertTrue(self.cleanup(request)["workspace_cleanup_recorded"])

    def test_marker_and_link_are_never_followed(self):
        request, *_ = self.ready()
        (self.workspace / "link").symlink_to(self.directory)
        self.cleanup(request, error="unauthorized")
        self.assertTrue((self.workspace / "result.txt").exists())
        (self.workspace / "link").unlink()
        (self.workspace.parent / "owner.json").write_text("changed", encoding="utf-8")
        self.cleanup(request, error="conflict")
        self.assertTrue(self.workspace.exists())

    def test_allocation_key_is_exact_and_does_not_adopt_legacy_work(self):
        request, task, resource = self.start_owned()
        again = self.invoke("allocate-workspace", "--db", self.database, "--request", request,
                             "--task", task, "--parent", self.owned_parent, "--key", "owned-allocation")
        self.assertEqual(again["resource_id"], resource["resource_id"])
        self.invoke("allocate-workspace", "--db", self.database, "--request", request,
                    "--task", task, "--parent", self.owned_parent, "--key", "different", error="conflict")
        self.assertEqual(len(self.rows("SELECT * FROM owned_workspaces")), 1)

    def test_foreign_task_rejected_before_writer_takeover(self):
        request, task, _ = self.start_owned()
        other = self.admit(key="another-request")
        before = self.snapshot()
        self.invoke("allocate-workspace", "--db", self.database, "--request", other,
                    "--task", task, "--parent", self.owned_parent, "--key", "foreign", error="unauthorized")
        self.assertEqual(self.snapshot(), before)
        self.assertNotEqual(request, other)

    def test_cancelled_work_is_retained_without_failure_package(self):
        request, *_ = self.start_owned()
        self.invoke("cancel", "--db", self.database, "--request", request)
        self.cleanup(request, error="not_ready")
        self.assertTrue(self.workspace.exists())


if __name__ == "__main__":
    suite = unittest.TestSuite(OwnedCleanupCLI(name) for name in sorted(OwnedCleanupCLI.__dict__) if name.startswith("test_"))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
