#!/usr/bin/env python3
"""S1.5 real C++ CLI, SQLite and filesystem integration with scripted models.

All workspaces, delivery roots and databases are disposable fixtures. The fixed
model decisions test identity, publication and recovery mechanics, not model
judgment quality, independent review, provider calls or real data deletion.
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
    raise SystemExit("usage: s15_cli_smoke.py WLS_BINARY SOURCE_ROOT")
BINARY = Path(sys.argv[1]).resolve()
SOURCE = Path(sys.argv[2]).resolve()
FIXTURES = SOURCE / "tests" / "fixtures"
sys.argv = [sys.argv[0]]


class AcceptanceCliSmoke(unittest.TestCase):
    def setUp(self):
        original_umask = os.umask(0o077)
        self.addCleanup(os.umask, original_umask)
        temporary = tempfile.TemporaryDirectory(prefix="wls-s15-cli-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.database = self.directory / "runtime.sqlite"
        self.workspace = self.directory / "alpha"
        self.delivery_root = self.directory / "deliveries"
        self.workspace.mkdir(mode=0o700)
        self.delivery_root.mkdir(mode=0o700)
        self.mapping = self.json_file("workspaces.json", {"alpha": str(self.workspace)})

    def invoke(self, *arguments, error=None):
        completed = subprocess.run(
            [str(BINARY), *(str(argument) for argument in arguments),
             "--source-root", str(SOURCE)], cwd=self.directory,
            capture_output=True, text=True, encoding="utf-8", timeout=35,
            check=False)
        if error is not None:
            self.assertNotEqual(completed.returncode, 0, completed.stdout)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(len(completed.stderr.strip().splitlines()), 1, completed.stderr)
            failure = json.loads(completed.stderr)
            if isinstance(error, tuple):
                self.assertIn(failure["error"], error, failure)
            else:
                self.assertEqual(failure["error"], error, failure)
            return failure
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")
        return json.loads(completed.stdout)

    def rows(self, query, arguments=()):
        with closing(sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True)) as database, database:
            database.row_factory = sqlite3.Row
            return [dict(row) for row in database.execute(query, arguments)]

    def snapshot(self):
        tables = self.rows("SELECT name FROM sqlite_schema WHERE type='table' "
                           "AND name NOT LIKE 'sqlite_%' ORDER BY name")
        return {table["name"]: sorted(self.rows('SELECT * FROM "' + table["name"] + '"'), key=repr)
                for table in tables}

    def json_file(self, name, value):
        path = self.directory / name
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return path

    def fixture(self, name="s15_fake_complete.json"):
        return json.loads((FIXTURES / name).read_text(encoding="utf-8"))

    def admit(self, key="acceptance-request", max_attempts=64):
        return self.invoke("submit", "--db", self.database, "--key", key,
                           "--text-file", FIXTURES / "s14_request.txt",
                           "--constraints-file", FIXTURES / "s14_constraints.json",
                           "--max-attempts", max_attempts)["request"]["request_id"]

    def view(self, request):
        return self.invoke("show", "--db", self.database, "--request", request)

    def pump(self, request, script=None, *, workspace=True, max_calls=12):
        arguments = ["pump-fake", "--db", self.database, "--request", request,
                     "--script", script or FIXTURES / "s15_fake_complete.json",
                     "--worker-calls", 2, "--max-calls", max_calls]
        if workspace:
            arguments.extend(["--workspaces-file", self.mapping])
        result = self.invoke(*arguments)
        self.assertEqual(result["real_model_calls"], 0)
        self.assertFalse(result["environment_checks_executed"])
        return result

    def accepted(self, *, script=None, max_attempts=64):
        request = self.admit(max_attempts=max_attempts)
        result = self.pump(request, script)
        view = self.view(request)
        self.assertTrue(view["acceptance_runtime_enabled"])
        self.assertEqual(view["tasks"][0]["status"], "ACCEPTED", result)
        self.assertNotEqual(view["request"]["status"], "DELIVERED")
        return request

    def jobs(self, request):
        return self.invoke("experience", "--db", self.database,
                           "--request", request)["experience_jobs"]

    def deliveries(self, request):
        return self.invoke("deliveries", "--db", self.database,
                           "--request", request)["deliveries"]

    def publish(self, request, delivery_id=None, root=None, error=None):
        arguments = ["publish-delivery", "--db", self.database, "--request", request,
                     "--delivery-dir", root or self.delivery_root]
        if delivery_id:
            arguments.extend(["--delivery", delivery_id])
        result = self.invoke(*arguments, error=error)
        if error is not None:
            return result
        self.assertFalse(result["receipt_created"])
        self.assertTrue(result["publication"]["verified"])
        self.assertTrue(result["publication"]["durable"])
        self.assertEqual(result["delivery"]["status"], "STORED")
        return result["delivery"]

    def ack(self, request, delivery, *, key="receipt-1", recipient="local-user",
            method="durable_local", manifest_hash=None, requirements=None,
            plan=None, root=None, error=None):
        return self.invoke("ack-delivery", "--db", self.database, "--request", request,
                           "--delivery", delivery["delivery_id"],
                           "--delivery-dir", root or self.delivery_root,
                           "--manifest-sha256", manifest_hash or delivery["manifest_sha256"],
                           "--requirements-revision", requirements or delivery["requirements_revision"],
                           "--plan-revision", plan or delivery["plan_revision"],
                           "--recipient", recipient, "--method", method, "--key", key,
                           error=error)

    def package(self, delivery):
        return self.delivery_root / delivery["delivery_id"]

    def assert_bundle_bytes(self, delivery):
        package = self.package(delivery)
        plan = delivery["frozen_plan"]
        raw_manifest = (package / "manifest.json").read_bytes()
        self.assertEqual(hashlib.sha256(raw_manifest).hexdigest(), delivery["manifest_sha256"])
        self.assertEqual(json.loads(raw_manifest), plan["manifest"])
        expected_names = {"manifest.json"}
        for entry in plan["files"]:
            expected_names.add(entry["name"])
            data = (package / entry["name"]).read_bytes()
            self.assertEqual(data, base64.b64decode(entry["content_base64"], validate=True))
            self.assertEqual(len(data), entry["size_bytes"])
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"])
        self.assertEqual({path.name for path in package.iterdir()}, expected_names)

    def test_complete_acceptance_package_receipt_and_retained_closeout(self):
        request = self.accepted()
        self.assertEqual(self.rows("PRAGMA user_version")[0]["user_version"], 408)
        self.assertEqual([job["status"] for job in self.jobs(request)], ["DONE"])
        delivery = self.publish(request)
        self.assert_bundle_bytes(delivery)
        self.assertEqual(self.view(request)["request"]["delivery_status"], "STORED")
        self.assertEqual(self.rows("SELECT * FROM receipts"), [])
        self.ack(request, delivery)
        view = self.view(request)
        self.assertEqual(view["request"]["status"], "DELIVERED")
        self.assertEqual(view["request"]["delivery_status"], "CONFIRMED")
        self.assertEqual(self.rows("SELECT final_status,cleanup_status FROM receipts"),
                         [{"final_status": "DELIVERED", "cleanup_status": "PENDING"}])
        self.assertEqual(self.rows("SELECT status FROM cleanup_jobs"), [{"status": "PENDING"}])
        self.assertTrue((self.workspace / "result.txt").exists())
        self.assertEqual(self.rows("SELECT count(*) AS n FROM requests")[0]["n"], 1)

    def test_package_uses_original_snapshot_after_workspace_changes(self):
        request = self.accepted()
        original = (self.workspace / "result.txt").read_bytes()
        (self.workspace / "result.txt").write_text("later live bytes are not the accepted result\n")
        delivery = self.publish(request)
        files = delivery["frozen_plan"]["files"]
        candidates = [entry for entry in files if entry["sha256"] == hashlib.sha256(original).hexdigest()]
        self.assertEqual(len(candidates), 1)
        self.assertEqual((self.package(delivery) / candidates[0]["name"]).read_bytes(), original)
        self.assert_bundle_bytes(delivery)

    def test_read_queries_and_verification_never_fence_or_ack(self):
        request = self.accepted()
        delivery = self.publish(request)
        before = self.snapshot()
        self.view(request)
        self.jobs(request)
        self.deliveries(request)
        self.invoke("closeout", "--db", self.database, "--request", request)
        self.invoke("pending", "--db", self.database)
        verified = self.invoke("verify-delivery", "--db", self.database, "--request", request,
                               "--delivery", delivery["delivery_id"], "--delivery-dir", self.delivery_root)
        self.assertTrue(verified["inspection"]["verified"])
        self.assertFalse(verified["inspection"]["durable"])
        self.assertFalse(verified["receipt_created"])
        self.assertEqual(before, self.snapshot())
        self.assertEqual(self.rows("SELECT * FROM receipts"), [])

    def test_wrong_receipt_tuple_fails_before_writer_takeover(self):
        request = self.accepted()
        delivery = self.publish(request)
        before = self.snapshot()
        self.ack(request, delivery, manifest_hash="0" * 64, error="conflict")
        self.ack(request, delivery, requirements=delivery["requirements_revision"] + 1, error="conflict")
        self.ack(request, delivery, plan=delivery["plan_revision"] + 1, error="conflict")
        self.ack(request, delivery, recipient=" ", error="invalid_input")
        self.assertEqual(before, self.snapshot())

    def test_foreign_request_and_replaced_delivery_root_reject(self):
        request = self.accepted()
        delivery = self.publish(request)
        other = self.admit(key="other-request")
        before = self.snapshot()
        self.ack(other, delivery, error="unauthorized")
        self.assertEqual(before, self.snapshot())
        moved = self.directory / "old-deliveries"
        self.delivery_root.rename(moved)
        self.delivery_root.mkdir(mode=0o700)
        self.ack(request, delivery, error="conflict")
        self.assertEqual(before, self.snapshot())
        self.assertTrue((moved / delivery["delivery_id"] / "manifest.json").exists())

    def test_team_check_alone_does_not_allow_publication(self):
        script = self.fixture()
        script["default"] = script["default"][:1]
        script["nodes"]["alpha"] = [step for step in script["nodes"]["alpha"]
                                      if step["tool_calls"][0]["name"] != "report_experience"]
        request = self.admit()
        self.pump(request, self.json_file("candidate-only.json", script))
        self.assertEqual(self.view(request)["tasks"][0]["status"], "SUBMITTED")
        self.assertEqual(self.jobs(request), [])
        self.publish(request, error="not_ready")
        self.assertEqual(list(self.delivery_root.iterdir()), [])
        self.assertEqual(self.deliveries(request), [])

    def test_amend_invalidates_stored_package_without_overwriting_history(self):
        request = self.accepted()
        delivery = self.publish(request)
        old_manifest = (self.package(delivery) / "manifest.json").read_bytes()
        self.invoke("amend", "--db", self.database, "--request", request,
                    "--text-file", FIXTURES / "s14_request.txt",
                    "--constraints-file", FIXTURES / "s14_constraints.json")
        before = self.snapshot()
        self.ack(request, delivery, error="stale")
        self.publish(request, delivery["delivery_id"], error="stale")
        self.assertEqual(before, self.snapshot())
        self.assertEqual(self.deliveries(request)[0]["status"], "SUPERSEDED")
        self.assertEqual((self.package(delivery) / "manifest.json").read_bytes(), old_manifest)
        self.assertEqual(self.rows("SELECT * FROM receipts"), [])

    def test_binary_and_empty_deliverables_survive_real_publication(self):
        binary = bytes([0, 255, 254, 1, 2, 3, 0, 10])
        (self.workspace / "raw.bin").write_bytes(binary)
        (self.workspace / "empty.bin").write_bytes(b"")
        script = self.fixture()
        for step in script["nodes"]["alpha"]:
            tool = step["tool_calls"][0]
            if tool["name"] == "submit":
                tool["arguments"]["deliverables"] = [
                    {"path": "/work/raw.bin", "description": "Original binary fixture bytes."},
                    {"path": "/work/empty.bin", "description": "Original empty fixture."}]
        request = self.accepted(script=self.json_file("binary.json", script))
        (self.workspace / "raw.bin").unlink()
        (self.workspace / "empty.bin").unlink()
        delivery = self.publish(request)
        self.assert_bundle_bytes(delivery)
        files = delivery["frozen_plan"]["files"]
        for original in (binary, b""):
            entries = [entry for entry in files if entry["sha256"] == hashlib.sha256(original).hexdigest()]
            self.assertEqual(len(entries), 1)
            self.assertEqual((self.package(delivery) / entries[0]["name"]).read_bytes(), original)

    def test_exact_receipt_replay_and_key_conflicts(self):
        request = self.accepted()
        delivery = self.publish(request)
        first = self.ack(request, delivery, method="explicit_ack")
        second = self.ack(request, delivery, method="explicit_ack")
        self.assertEqual(first, second)
        self.ack(request, delivery, recipient="another-recipient", method="explicit_ack", error="conflict")
        self.ack(request, delivery, method="durable_local", error="conflict")
        self.assertEqual(len(self.rows("SELECT * FROM receipts")), 1)
        self.assertEqual(len(self.rows("SELECT * FROM cleanup_jobs")), 1)
        self.assertEqual(self.view(request)["request"]["status"], "DELIVERED")

    def test_terminal_request_can_finish_only_explicit_pending_experience(self):
        script = self.fixture()
        script["nodes"]["alpha"] = [step for step in script["nodes"]["alpha"]
                                      if step["tool_calls"][0]["name"] != "report_experience"]
        request = self.accepted(script=self.json_file("pending-experience.json", script))
        original_job = self.jobs(request)[0]
        self.assertEqual(original_job["status"], "PENDING")
        delivery = self.publish(request)
        self.ack(request, delivery)
        original_manifest = (self.package(delivery) / "manifest.json").read_bytes()
        result = self.pump(request, FIXTURES / "s15_fake_report.json", workspace=False)
        self.assertEqual([call["role"] for call in result["calls"]], ["team_lead"], result)
        self.assertEqual(self.jobs(request)[0]["status"], "DONE", result)
        self.assertEqual(self.jobs(request)[0]["source_snapshot"], original_job["source_snapshot"])
        self.assertEqual((self.package(delivery) / "manifest.json").read_bytes(), original_manifest)
        self.assertEqual(self.view(request)["request"]["status"], "DELIVERED")
        self.assertEqual(self.rows("SELECT status FROM cleanup_jobs"), [{"status": "PENDING"}])

    def test_exhausted_experience_budget_stays_pending_after_receipt(self):
        request = self.accepted(max_attempts=6)
        self.assertEqual(self.jobs(request)[0]["status"], "PENDING")
        delivery = self.publish(request)
        self.ack(request, delivery)
        before = len(self.rows("SELECT * FROM call_attempts"))
        self.pump(request, FIXTURES / "s15_fake_report.json", workspace=False)
        self.assertEqual(len(self.rows("SELECT * FROM call_attempts")), before)
        self.assertEqual(self.jobs(request)[0]["status"], "PENDING")
        self.assertEqual(self.rows("SELECT status FROM cleanup_jobs"), [{"status": "PENDING"}])

    def test_publication_retry_reuses_the_exact_bundle(self):
        request = self.accepted()
        first = self.publish(request)
        paths = list(self.package(first).iterdir())
        originals = {path.name: (path.stat().st_ino, path.read_bytes()) for path in paths}
        second = self.publish(request, first["delivery_id"])
        third = self.publish(request)
        self.assertEqual(first["delivery_id"], second["delivery_id"])
        self.assertEqual(first["delivery_id"], third["delivery_id"])
        self.assertEqual(len(self.deliveries(request)), 1)
        self.assertEqual(originals, {path.name: (path.stat().st_ino, path.read_bytes())
                                     for path in self.package(second).iterdir()})
        self.assertEqual(self.rows("SELECT * FROM receipts"), [])

    def test_receipt_selects_one_package_and_preserves_other_published_bytes(self):
        request = self.accepted()
        first = self.publish(request)
        second_root = self.directory / "second-deliveries"
        second_root.mkdir(mode=0o700)
        second = self.publish(request, root=second_root)
        packages = [self.package(first), second_root / second["delivery_id"]]
        originals = [{path.name: path.read_bytes() for path in package.iterdir()}
                     for package in packages]
        receipt = self.ack(request, second, root=second_root)
        statuses = {item["delivery_id"]: item["status"] for item in self.deliveries(request)}
        self.assertEqual(statuses[first["delivery_id"]], "SUPERSEDED")
        self.assertEqual(statuses[second["delivery_id"]], "CONFIRMED")
        closeout = self.invoke("closeout", "--db", self.database, "--request", request)
        self.assertNotIn("delivery_unconfirmed", closeout["blockers"])
        self.assertEqual(self.ack(request, second, root=second_root), receipt)
        self.publish(request, first["delivery_id"], error="stale")
        for package, original in zip(packages, originals):
            self.assertEqual({path.name: path.read_bytes() for path in package.iterdir()}, original)
        self.assertEqual(len(self.rows("SELECT * FROM delivery_receipts")), 1)

    def test_tampered_or_extra_file_blocks_receipt_without_db_mutation(self):
        request = self.accepted()
        delivery = self.publish(request)
        package = self.package(delivery)
        entry = delivery["frozen_plan"]["files"][0]
        target = package / entry["name"]
        original = target.read_bytes()
        target.chmod(0o600)
        target.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        target.chmod(0o400)
        before = self.snapshot()
        self.ack(request, delivery, error="corrupt")
        self.assertEqual(before, self.snapshot())
        target.chmod(0o600)
        target.write_bytes(original)
        target.chmod(0o400)
        package.chmod(0o700)
        extra = package / "unexpected.txt"
        extra.write_text("unapproved extra content")
        extra.chmod(0o400)
        package.chmod(0o500)
        self.ack(request, delivery, error="corrupt")
        self.assertEqual(before, self.snapshot())

    def test_delivery_directory_must_not_overlap_control_or_workspace(self):
        request = self.accepted()
        before = self.snapshot()
        self.publish(request, root=self.workspace, error="unauthorized")
        self.publish(request, root=self.directory, error="unauthorized")
        nested = self.workspace / "packages"
        nested.mkdir(mode=0o700)
        self.publish(request, root=nested, error="unauthorized")
        self.assertEqual(before, self.snapshot())

    def test_cancellation_atomically_keeps_receipt_and_unfinished_cleanup(self):
        request = self.admit()
        self.invoke("cancel", "--db", self.database, "--request", request)
        self.assertEqual(self.rows("SELECT final_status,cleanup_status FROM receipts"),
                         [{"final_status": "CANCELLED", "cleanup_status": "PENDING"}])
        self.assertEqual(self.rows("SELECT status FROM cleanup_jobs"), [{"status": "PENDING"}])
        self.assertEqual(self.view(request)["request"]["status"], "CANCELLED")
        before = self.snapshot()
        self.invoke("closeout", "--db", self.database, "--request", request)
        self.invoke("pending", "--db", self.database)
        self.assertEqual(before, self.snapshot())

    def test_experience_entries_are_saved_as_proposals_without_memory_publication(self):
        script = self.fixture()
        report = next(step["tool_calls"][0] for step in script["nodes"]["alpha"]
                      if step["tool_calls"][0]["name"] == "report_experience")
        report["arguments"] = {"no_new_experience": False, "memory_assessments": [], "entries": [{
            "kind": "new", "type": "experience", "outcome": "success", "title": "Original snapshot delivery",
            "description": "Preserve and verify the accepted bytes through publication.",
            "body_markdown": "This fixed fixture exercises original snapshot publication.",
            "proposed_scope": "project", "evidence": "Saved alpha submission and Dispatcher acceptance."}]}
        request = self.accepted(script=self.json_file("experience-proposal.json", script))
        self.assertEqual(self.jobs(request)[0]["status"], "DONE")
        self.assertEqual(self.rows("SELECT count(*) AS n FROM memory_meta")[0]["n"], 0)
        delivery = self.publish(request)
        frozen = json.dumps(delivery["frozen_plan"], ensure_ascii=False)
        raw_sources = [base64.b64decode(entry["content_base64"]).decode("utf-8", errors="replace")
                       for entry in delivery["frozen_plan"]["files"]]
        self.assertTrue("Original snapshot delivery" in frozen or
                        any("Original snapshot delivery" in data for data in raw_sources))


if __name__ == "__main__":
    unittest.main(verbosity=2)
