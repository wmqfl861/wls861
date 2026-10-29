#!/usr/bin/env python3
"""S1.3 subprocess regressions for durable independent task execution.

The C++ CLI performs the model, event, filesystem and SQLite work. Python only
supplies fixed FakeModel scripts, reads durable evidence, and manages disposable
directories. No provider or process-isolation acceptance is exercised here.
"""
from __future__ import annotations
from contextlib import closing

import base64
from collections import Counter
import copy
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
    raise SystemExit("usage: s13_cli_smoke.py WLS_BINARY SOURCE_ROOT")
BINARY = Path(sys.argv[1]).resolve()
SOURCE = Path(sys.argv[2]).resolve()
FIXTURES = SOURCE / "tests" / "fixtures"
sys.argv = [sys.argv[0]]


class EventCliSmoke(unittest.TestCase):
    def setUp(self):
        original_umask = os.umask(0o077)
        self.addCleanup(os.umask, original_umask)
        self.temporary = tempfile.TemporaryDirectory(prefix="wls-s13-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.database = self.directory / "runtime.sqlite"
        self.workspaces = {node: self.directory / node for node in ("alpha", "beta")}
        for path in self.workspaces.values():
            path.mkdir(mode=0o700)
        self.mapping = self.json_file("workspaces.json", {
            node: str(path) for node, path in self.workspaces.items()})

    def invoke(self, *arguments, error=None):
        completed = subprocess.run(
            [str(BINARY), *(str(argument) for argument in arguments),
             "--source-root", str(SOURCE)], cwd=self.directory,
            capture_output=True, text=True, encoding="utf-8", timeout=30,
            check=False)
        if error is not None:
            self.assertNotEqual(completed.returncode, 0, completed.stdout)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(len(completed.stderr.strip().splitlines()), 1,
                             completed.stderr)
            failure = json.loads(completed.stderr)
            self.assertEqual(failure["error"], error, failure)
            self.assertTrue(failure["message"])
            return failure
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")
        return json.loads(completed.stdout)

    def rows(self, query, arguments=()):
        with closing(sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True)) as db, db:
            db.row_factory = sqlite3.Row
            return [dict(row) for row in db.execute(query, arguments)]

    def snapshot(self):
        """Read all persistent rows, including epochs and claims, without a writer."""
        names = self.rows("SELECT name FROM sqlite_schema WHERE type='table' "
                          "AND name NOT LIKE 'sqlite_%' ORDER BY name")
        # FTS5 creates shadow tables without rowid; compare their complete rows
        # as well. repr also preserves raw BLOB bytes in a deterministic key.
        return {row["name"]: sorted(self.rows('SELECT * FROM "' + row["name"] + '"'), key=repr)
                for row in names}

    def json_file(self, name, value):
        path = self.directory / name
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return path

    def fixture(self, name):
        return json.loads((FIXTURES / name).read_text(encoding="utf-8"))

    def submit(self, key="independent-request"):
        return self.invoke("submit", "--db", self.database, "--key", key,
                           "--text-file", FIXTURES / "s13_request.txt",
                           "--constraints-file", FIXTURES / "s13_constraints.json")["request"]["request_id"]

    def show(self, request):
        return self.invoke("show", "--db", self.database, "--request", request)

    def tasks(self, request):
        return {task["node_id"]: task for task in self.show(request)["tasks"]}

    def pump(self, request, *, script=None, workers=2, max_calls=12,
             dispositions=None, mapping=None, error=None):
        arguments = ["pump-fake", "--db", self.database, "--request", request,
                     "--script", script or FIXTURES / "s13_fake_initial.json",
                     "--workspaces-file", mapping or self.mapping,
                     "--max-calls", max_calls, "--worker-calls", workers]
        if dispositions is not None:
            arguments.extend(["--dispositions-file", dispositions])
        return self.invoke(*arguments, error=error)

    def amend(self, request):
        return self.invoke("amend", "--db", self.database, "--request", request,
                           "--text-file", FIXTURES / "s13_amendment.txt",
                           "--constraints-file", FIXTURES / "s13_amended_constraints.json")

    def history(self, request, task, **options):
        arguments = ["history", "--db", self.database, "--request", request,
                     "--role", "worker", "--task", task]
        if "limit" in options:
            arguments.extend(["--limit", options["limit"]])
        return self.invoke(*arguments, error=options.get("error"))

    def diagnostic_script(self, *, nodes=("alpha", "beta"), offset=0,
                          max_bytes=6, amended=False):
        source = self.fixture("s13_fake_amended.json" if amended else "s13_fake_initial.json")
        groups = {}
        for node in nodes:
            lead = copy.deepcopy(source["nodes"][node][0])
            worker = copy.deepcopy(source["nodes"][node][1])
            worker["tool_calls"] = [{"call_id": "observe-" + node,
                                     "name": "fs_read", "arguments": {
                                         "path": "r1.txt", "offset": offset,
                                         "max_bytes": max_bytes}}]
            groups[node] = [lead, worker]
        return {"default": [], "nodes": groups}

    def attempts(self, task=None, *, role=None):
        query, arguments = "SELECT rowid AS ordinal,* FROM call_attempts WHERE 1=1", []
        if task is not None:
            query += " AND task_id=?"
            arguments.append(task)
        if role is not None:
            query += " AND role=?"
            arguments.append(role)
        return self.rows(query + " ORDER BY rowid", arguments)

    def feedback(self, task):
        return self.rows("SELECT f.* FROM model_tool_feedback f JOIN call_attempts a "
                         "USING(attempt_id) WHERE a.task_id=? AND a.role='worker' ORDER BY a.rowid",
                         (task,))

    def pending(self, task):
        return self.rows("SELECT * FROM event_inbox WHERE subject_task_id=? "
                         "AND status='PENDING' ORDER BY inbox_id", (task,))

    def context_history(self, attempt):
        """Parse the actual byte frames, including Chinese contract/message text."""
        envelope = json.loads(attempt["request_envelope"])
        remaining = envelope["context"]["dynamic_suffix"].encode("utf-8")
        frames = []
        while remaining:
            header, separator, body = remaining.partition(b"\n")
            self.assertEqual(separator, b"\n", "truncated context frame header")
            name, separator, digits = header.partition(b" ")
            self.assertEqual(separator, b" ")
            self.assertTrue(digits.isdigit())
            size = int(digits)
            self.assertGreater(len(body), size, "truncated context frame body")
            self.assertEqual(body[size:size + 1], b"\n")
            frames.append((name.decode("ascii"), body[:size].decode("utf-8")))
            remaining = body[size + 1:]
        self.assertTrue(frames)
        self.assertEqual(frames[0][0], "task_contract")
        self.assertIn("只读写明确指定的工作目录", frames[0][1])
        history = []
        for name, body in frames[1:]:
            self.assertEqual(name, "message")
            message = json.loads(body)
            self.assertEqual(message["role"], "tool")
            history.append(json.loads(message["content"]))
        return history

    def assert_original_history(self, history, attempts, feedback):
        expected = {attempt["attempt_id"]: attempt for attempt in attempts}
        receipts = {row["attempt_id"]: row for row in feedback}
        self.assertEqual([entry["attempt_id"] for entry in history], list(expected))
        for entry in history:
            original = expected[entry["attempt_id"]]
            self.assertNotIn("request_envelope", entry, "history recursively embeds its old compiled context")
            self.assertEqual(entry["frozen_binding"], json.loads(original["frozen_binding"]))
            self.assertEqual(entry["response"], json.loads(original["response"]))
            self.assertEqual(entry["role_call_index"],
                             json.loads(original["request_envelope"])["role_call_index"])
            self.assertFalse(entry["feedback_missing"])
            receipt = receipts[entry["attempt_id"]]
            for field in ("original_binding", "response", "result"):
                self.assertEqual(entry["feedback"][field], json.loads(receipt[field]))
            self.assertEqual(entry["feedback"]["record_sha256"], receipt["record_sha256"])

    def test_independent_files_and_original_diagnostic_feedback(self):
        request = self.submit()
        run = self.pump(request)
        self.assertEqual(Counter(call["role"] for call in run["calls"]),
                         {"dispatcher": 1, "team_lead": 2, "worker": 4})
        self.assertEqual(len(run["calls"]), 7)
        self.assertEqual(run["budget"]["attempts"], 7)
        self.assertFalse(run["business_delivered"])
        self.assertEqual(run["real_model_calls"], 0)
        tasks = self.tasks(request)
        self.assertEqual(set(tasks), {"alpha", "beta"})
        self.assertNotEqual(tasks["alpha"]["team_id"], tasks["beta"]["team_id"])
        self.assertEqual(len(self.rows("SELECT * FROM ops")), 2)
        for node, task in tasks.items():
            expected = (node + " original\n").encode()
            self.assertEqual((self.workspaces[node] / "r1.txt").read_bytes(), expected)
            attempts = self.attempts(task["task_id"], role="worker")
            receipts = self.feedback(task["task_id"])
            self.assertEqual(len(attempts), 2)
            self.assertEqual(len(receipts), 2)
            for attempt, receipt in zip(attempts, receipts):
                binding = json.loads(receipt["original_binding"])
                self.assertEqual(len(binding), 10)
                self.assertEqual(binding["task_id"], task["task_id"])
                self.assertEqual(binding, json.loads(attempt["frozen_binding"]))
                self.assertEqual(json.loads(receipt["response"]), json.loads(attempt["response"]))
                self.assertEqual(len(receipt["record_sha256"]), 64)
            observed = json.loads(receipts[1]["result"])
            self.assertEqual(base64.b64decode(observed["content_base64"], validate=True), expected[:6])
            self.assertEqual(observed["sha256"], hashlib.sha256(expected).hexdigest())
            self.assertEqual(json.loads(receipts[1]["response"])["tool_calls"][0]["arguments"],
                             {"path": "r1.txt", "max_bytes": 6})
            self.assert_original_history(self.context_history(attempts[1]), attempts[:1], receipts[:1])
            self.assert_original_history(self.history(request, task["task_id"]), attempts, receipts)
        self.assertEqual(self.rows("PRAGMA user_version")[0]["user_version"], 410)

    def test_reopen_restores_original_bytes_and_only_the_same_workers_history(self):
        request = self.submit()
        self.pump(request)
        tasks = self.tasks(request)
        original_attempts = {node: self.attempts(task["task_id"], role="worker")
                             for node, task in tasks.items()}
        original_feedback = {node: self.feedback(task["task_id"])
                             for node, task in tasks.items()}
        # The old diagnostic must remain the original observation even after
        # an explicitly controlled host change and another CLI process.
        changed = "替代现场，不能冒充先前的读取结果。\n".encode("utf-8")
        (self.workspaces["alpha"] / "r1.txt").write_bytes(changed)
        script = self.json_file("followup.json", self.diagnostic_script(max_bytes=3))
        # Explicit recovery keeps this test's original cross-epoch history case.
        # An idle CLI reopen is covered separately and preserves its epoch.
        self.invoke("init", "--db", self.database)
        run = self.pump(request, script=script, workers=1)
        self.assertEqual(sum(call["role"] == "worker" for call in run["calls"]), 2)
        self.assertFalse(any(call["role"] == "dispatcher" for call in run["calls"]))
        for node, task in tasks.items():
            attempts = self.attempts(task["task_id"], role="worker")
            self.assertEqual(len(attempts), 3)
            receipts = self.feedback(task["task_id"])
            self.assertEqual(receipts[:2], original_feedback[node], "old observations were rewritten")
            restored = self.context_history(attempts[-1])
            self.assert_original_history(restored, original_attempts[node], original_feedback[node])
            sibling = tasks["beta" if node == "alpha" else "alpha"]["task_id"]
            self.assertTrue(all(item["task_id"] != sibling for item in restored))
            self.assertNotEqual(json.loads(attempts[-1]["frozen_binding"])["execution_epoch"],
                                json.loads(attempts[0]["frozen_binding"])["execution_epoch"])
            self.assertEqual(json.loads(receipts[-1]["response"])["tool_calls"][0]["arguments"],
                             {"path": "r1.txt", "offset": 0, "max_bytes": 3})
            self.assert_original_history(self.history(request, task["task_id"]), attempts, receipts)
        alpha_receipts = self.feedback(tasks["alpha"]["task_id"])
        self.assertEqual(base64.b64decode(json.loads(alpha_receipts[1]["result"])["content_base64"]),
                         b"alpha ")
        self.assertEqual(base64.b64decode(json.loads(alpha_receipts[-1]["result"])["content_base64"]),
                         changed[:3])
        self.assertEqual(len(self.rows("SELECT * FROM ops")), 2,
                         "reconstructing history or reading again must not redispatch old writes")

    def test_partial_amendment_keeps_omitted_task_pending_then_explicit_keep_applies(self):
        request = self.submit()
        self.pump(request)
        original = self.tasks(request)
        beta_id = original["beta"]["task_id"]
        alpha_id = original["alpha"]["task_id"]
        beta_attempts = self.attempts(beta_id)
        beta_feedback = self.feedback(beta_id)
        self.amend(request)
        updated = self.pump(request, script=FIXTURES / "s13_fake_amended.json", workers=1,
                            dispositions=FIXTURES / "s13_update_alpha.json")
        self.assertEqual(sum(call["role"] == "dispatcher" for call in updated["calls"]), 1)
        self.assertEqual(self.attempts(beta_id), beta_attempts,
                         "omitted AMEND subject invoked a model")
        tasks = self.tasks(request)
        alpha_bytes = b"alpha revision 2\n"
        self.assertEqual((self.workspaces["alpha"] / "r2.txt").read_bytes(), alpha_bytes)
        self.assertFalse((self.workspaces["beta"] / "r2.txt").exists())
        self.assertEqual(tasks["beta"]["based_on_requirements_revision"], 1)
        self.assertEqual(tasks["beta"]["task_revision"], original["beta"]["task_revision"])
        self.assertEqual(tasks["beta"]["instruction_revision"], original["beta"]["instruction_revision"])
        self.assertEqual(tasks["beta"]["paused_for_change"], 1)
        self.assertEqual(tasks["alpha"]["based_on_requirements_revision"], 2)
        self.assertTrue(any(row["role"] == "dispatcher" and row["event_requirements_revision"] == 2
                            for row in self.pending(beta_id)), "beta AMEND item was silently consumed")
        self.assertEqual(self.rows("SELECT * FROM amend_dispositions WHERE task_id=?", (beta_id,)), [])
        current_plan = self.show(request)["request"]["plan_revision"]
        alpha_attempts = self.attempts(alpha_id, role="worker")
        alpha_operations = self.rows("SELECT * FROM ops WHERE task_id=? ORDER BY rowid", (alpha_id,))
        beta_only = self.fixture("s13_fake_amended.json")
        beta_only = {"default": [], "nodes": {"beta": beta_only["nodes"]["beta"]}}
        kept = self.pump(request, script=self.json_file("keep-beta-script.json", beta_only), workers=1,
                         dispositions=FIXTURES / "s13_keep_beta.json")
        self.assertFalse(any(call["role"] == "dispatcher" for call in kept["calls"]))
        self.assertEqual(self.attempts(alpha_id, role="worker"), alpha_attempts)
        self.assertEqual(self.rows("SELECT * FROM ops WHERE task_id=? ORDER BY rowid", (alpha_id,)),
                         alpha_operations)
        self.assertEqual((self.workspaces["alpha"] / "r2.txt").read_bytes(), alpha_bytes)
        self.assertEqual((self.workspaces["beta"] / "r2.txt").read_bytes(), b"beta revision 2\n")
        self.assertEqual(self.show(request)["request"]["plan_revision"], current_plan)
        beta = self.tasks(request)["beta"]
        self.assertGreater(beta["task_revision"], original["beta"]["task_revision"])
        self.assertGreater(beta["instruction_revision"], original["beta"]["instruction_revision"])
        self.assertEqual(beta["based_on_requirements_revision"], 2)
        self.assertEqual(beta["paused_for_change"], 0)
        for applied, target in (("applied_task_revision", "task_revision"),
                                ("applied_plan_revision", "plan_revision"),
                                ("applied_instruction_revision", "instruction_revision")):
            self.assertEqual(beta[applied], beta[target], "keep bypassed the new order/ack")
        decisions = self.rows("SELECT * FROM amend_dispositions WHERE task_id=?", (beta_id,))
        self.assertEqual(len(decisions), 1)
        self.assertEqual(decisions[0]["disposition"], "keep")
        self.assertEqual(decisions[0]["based_on_requirements_revision"], 2)
        self.assertEqual(self.feedback(beta_id)[:2], beta_feedback)
        newest_beta = self.attempts(beta_id, role="worker")[-1]
        self.assert_original_history(self.context_history(newest_beta),
                                     [row for row in beta_attempts if row["role"] == "worker"],
                                     beta_feedback)

    def test_wait_survives_reopen_releases_other_task_and_needs_exact_generation(self):
        request = self.submit()
        self.pump(request)
        tasks = self.tasks(request)
        alpha_id, beta_id = tasks["alpha"]["task_id"], tasks["beta"]["task_id"]
        self.invoke("wait", "--db", self.database, "--request", request,
                    "--task", alpha_id, "--reason", "等待人工确认；不占用 beta 的推进机会。")
        waiting = self.rows("SELECT * FROM worker_waits WHERE task_id=?", (alpha_id,))[0]
        self.assertEqual(waiting["status"], "WAITING")
        original_wait_binding = json.loads(waiting["original_binding"])
        before_alpha = self.attempts(alpha_id)
        before_beta = self.attempts(beta_id, role="worker")
        original_alpha = (self.workspaces["alpha"] / "r1.txt").read_bytes()
        script = self.json_file("wait-progress.json", self.diagnostic_script(offset=1, max_bytes=2))
        progress = self.pump(request, script=script, workers=1)
        self.assertEqual(self.attempts(alpha_id), before_alpha,
                         "waiting alpha invoked a TeamLead or Worker model")
        self.assertEqual(len(self.attempts(beta_id, role="worker")), len(before_beta) + 1)
        self.assertTrue(any(call["task_id"] == beta_id and call["role"] == "worker"
                            for call in progress["calls"]))
        self.assertEqual((self.workspaces["alpha"] / "r1.txt").read_bytes(), original_alpha)
        persisted = self.rows("SELECT * FROM worker_waits WHERE task_id=?", (alpha_id,))[0]
        self.assertEqual(persisted["status"], "WAITING")
        self.assertEqual(json.loads(persisted["original_binding"]), original_wait_binding)
        self.assertTrue(self.pending(alpha_id))
        generation = waiting["generation"]
        self.invoke("wake", "--db", self.database, "--request", request, "--task", alpha_id,
                    "--wait-generation", generation + 1, error="stale")
        self.assertEqual(self.rows("SELECT status FROM worker_waits WHERE task_id=?", (alpha_id,))[0]["status"],
                         "WAITING")
        self.invoke("wake", "--db", self.database, "--request", request, "--task", alpha_id,
                    "--wait-generation", generation)
        self.assertEqual(self.rows("SELECT status FROM worker_waits WHERE task_id=?", (alpha_id,))[0]["status"],
                         "AWAKE")
        only_alpha = self.json_file("wake-alpha.json", self.diagnostic_script(nodes=("alpha",)))
        self.pump(request, script=only_alpha, workers=1)
        self.assertEqual(len(self.attempts(alpha_id, role="worker")), 3)
        self.invoke("wait", "--db", self.database, "--request", request,
                    "--task", alpha_id, "--reason", "第二次独立等待。")
        renewed = self.rows("SELECT * FROM worker_waits WHERE task_id=?", (alpha_id,))[0]
        self.assertGreater(renewed["generation"], generation)
        self.invoke("wake", "--db", self.database, "--request", request, "--task", alpha_id,
                    "--wait-generation", generation, error="stale")
        self.assertEqual(self.rows("SELECT status FROM worker_waits WHERE task_id=?", (alpha_id,))[0]["status"],
                         "WAITING")

    def test_amendment_supersedes_old_wait_without_authorizing_the_new_task(self):
        request = self.submit()
        self.pump(request)
        alpha_id = self.tasks(request)["alpha"]["task_id"]
        self.invoke("wait", "--db", self.database, "--request", request,
                    "--task", alpha_id, "--reason", "此等待只适用于原始要求。")
        original = self.rows("SELECT * FROM worker_waits WHERE task_id=?", (alpha_id,))[0]
        attempts = self.attempts(alpha_id)
        self.amend(request)
        self.invoke("wake", "--db", self.database, "--request", request, "--task", alpha_id,
                    "--wait-generation", original["generation"], error="stale")
        superseded = self.rows("SELECT * FROM worker_waits WHERE task_id=?", (alpha_id,))[0]
        self.assertEqual(superseded["status"], "SUPERSEDED")
        self.assertEqual(superseded["original_binding"], original["original_binding"])
        self.assertEqual(self.attempts(alpha_id), attempts)
        self.assertEqual(self.tasks(request)["alpha"]["paused_for_change"], 1)
        self.assertEqual(self.rows("SELECT * FROM amend_dispositions WHERE task_id=?", (alpha_id,)), [])
        self.assertFalse((self.workspaces["alpha"] / "r2.txt").exists())
        self.assertTrue(any(row["role"] == "dispatcher" and row["event_requirements_revision"] == 2
                            for row in self.pending(alpha_id)))

    def test_workspace_map_rejects_conflicts_before_opening_a_writer(self):
        request = self.submit()
        nested = self.workspaces["alpha"] / "nested"
        nested.mkdir(mode=0o700)
        alias = self.directory / "beta-alias"
        alias.symlink_to(self.workspaces["beta"], target_is_directory=True)
        invalid = [
            ("duplicate-identity", {"alpha": str(self.workspaces["alpha"]),
                                    "beta": str(self.workspaces["alpha"])}, "conflict"),
            ("nested", {"alpha": str(self.workspaces["alpha"]), "beta": str(nested)}, "conflict"),
            ("symlink", {"alpha": str(self.workspaces["alpha"]), "beta": str(alias)}, "unauthorized"),
            ("relative", {"alpha": "alpha", "beta": str(self.workspaces["beta"])}, "invalid_input"),
        ]
        before = self.snapshot()
        for name, value, error in invalid:
            with self.subTest(mapping=name):
                self.pump(request, mapping=self.json_file(name + ".json", value), error=error)
                self.assertEqual(self.snapshot(), before, "invalid mapping opened a writer or changed durable state")
                self.assertFalse((self.workspaces["alpha"] / "r1.txt").exists())
        duplicate_key = self.directory / "duplicate-key.json"
        duplicate_key.write_text('{"alpha":' + json.dumps(str(self.workspaces["alpha"])) +
                                 ',"alpha":' + json.dumps(str(self.workspaces["beta"])) + '}',
                                 encoding="utf-8")
        self.pump(request, mapping=duplicate_key, error="invalid_input")
        inside = self.workspaces["alpha"] / "control.sqlite"
        self.invoke("pump-fake", "--db", inside, "--request", request,
                    "--script", FIXTURES / "s13_fake_initial.json",
                    "--workspaces-file", self.mapping, error="unauthorized")
        self.assertFalse(inside.exists(), "invalid location created a control database in the workspace")
        self.assertEqual(self.snapshot(), before)

    def test_read_only_inbox_history_and_capacity_error_preserve_all_rows(self):
        request = self.submit()
        self.pump(request)
        tasks = self.tasks(request)
        before = self.snapshot()
        dispatcher = self.invoke("inbox", "--db", self.database, "--request", request,
                                 "--role", "dispatcher", "--limit", 256)
        self.assertTrue(all(row["role"] == "dispatcher" and row["actor_task_id"] == ""
                            for row in dispatcher))
        for task in tasks.values():
            task_id = task["task_id"]
            for role in ("team_lead", "worker"):
                inbox = self.invoke("inbox", "--db", self.database, "--request", request,
                                    "--role", role, "--task", task_id, "--limit", 256)
                self.assertTrue(all(row["actor_task_id"] == task_id and row["role"] == role
                                    for row in inbox))
                history = self.invoke("history", "--db", self.database, "--request", request,
                                      "--role", role, "--task", task_id)
                self.assertTrue(history)
                self.assertTrue(all(row["task_id"] == task_id and row["role"] == role
                                    for row in history))
                self.assertTrue(all("request_envelope" not in row for row in history))
            self.history(request, task_id, limit=1, error="quota")
        self.invoke("show", "--db", self.database, "--request", request)
        self.invoke("events", "--db", self.database, "--request", request, "--limit", 256)
        self.assertEqual(self.snapshot(), before,
                         "read-only queries or history overflow changed epochs, events, attempts or claims")

    def test_invalid_options_and_foreign_scopes_do_not_reserve_attempts(self):
        request = self.submit()
        self.pump(request)
        alpha_id = self.tasks(request)["alpha"]["task_id"]
        foreign_request = self.submit(key="another-request")
        before = self.snapshot()
        for command in ("inbox", "history"):
            self.invoke(command, "--db", self.database, "--request", foreign_request,
                        "--role", "worker", "--task", alpha_id, error="unauthorized")
        invalid_disposition = self.json_file("extra-disposition-field.json", {
            "alpha": {"disposition": "keep", "rationale": "explicit decision", "extra": True}})
        blank_rationale = self.json_file("blank-rationale.json", {
            "alpha": {"disposition": "keep", "rationale": " \t\r\n"}})
        bad_script = self.json_file("malformed-script.json", {"default": "not-an-array", "nodes": {}})
        pump_base = ["pump-fake", "--db", self.database, "--request", request,
                     "--script", FIXTURES / "s13_fake_initial.json", "--workspaces-file", self.mapping]
        invalid = [
            pump_base + ["--max-calls", 0],
            pump_base + ["--max-calls", 65],
            pump_base + ["--max-calls", ""],
            pump_base + ["--worker-calls", 17],
            pump_base + ["--max-calls", 2, "--max-calls", 3],
            pump_base + ["--dispositions-file", invalid_disposition],
            pump_base + ["--dispositions-file", blank_rationale],
            pump_base + ["--unknown-option", "value"],
            ["pump-fake", "--db", self.database, "--request", request, "--script", bad_script],
            ["history", "--db", self.database, "--request", request, "--role", "worker"],
            ["inbox", "--db", self.database, "--request", request, "--role", "dispatcher", "--task", alpha_id],
            ["inbox", "--db", self.database, "--request", request, "--limit", 257],
            ["inbox", "--db", self.database, "--request", request, "--limit", ""],
            ["wait", "--db", self.database, "--request", request, "--task", alpha_id],
            ["wait", "--db", self.database, "--request", request, "--task", alpha_id,
             "--reason", "x" * 4097],
            ["wake", "--db", self.database, "--request", request, "--task", alpha_id],
            ["wake", "--db", self.database, "--request", request, "--task", alpha_id,
             "--wait-generation", str(1 << 63)],
        ]
        for index, arguments in enumerate(invalid):
            with self.subTest(invalid_option=index):
                self.invoke(*arguments, error="invalid_input")
                self.assertEqual(self.snapshot(), before,
                                 "bad host options created an attempt or opened a writer")

    def test_call_cap_and_missing_group_retain_specific_pending_work(self):
        request = self.submit()
        first = self.pump(request, max_calls=1)
        self.assertEqual([call["role"] for call in first["calls"]], ["dispatcher"])
        self.assertEqual(first["budget"]["attempts"], 1)
        tasks = self.tasks(request)
        self.assertEqual(set(tasks), {"alpha", "beta"})
        for node, task in tasks.items():
            self.assertTrue(self.pending(task["task_id"]))
            self.assertFalse((self.workspaces[node] / "r1.txt").exists())
        beta_id = tasks["beta"]["task_id"]
        beta_pending = {row["inbox_id"] for row in self.pending(beta_id)}
        script = self.fixture("s13_fake_initial.json")
        script["nodes"]["beta"] = []
        second = self.pump(request, script=self.json_file("no-beta-steps.json", script), workers=1)
        self.assertEqual(self.attempts(beta_id), [], "an empty node group reserved a fallback model attempt")
        self.assertEqual((self.workspaces["alpha"] / "r1.txt").read_bytes(), b"alpha original\n")
        self.assertFalse((self.workspaces["beta"] / "r1.txt").exists())
        self.assertTrue(beta_pending.issubset({row["inbox_id"] for row in self.pending(beta_id)}),
                        "beta's captured-but-uncompleted event was acknowledged")
        self.assertEqual(second["budget"]["attempts"], 3)
        before_attempts = self.attempts()
        pending_ids = {row["inbox_id"] for row in self.rows("SELECT * FROM event_inbox WHERE status='PENDING'")}
        empty = self.json_file("all-missing.json", {"default": [], "nodes": {"alpha": [], "beta": []}})
        final = self.pump(request, script=empty)
        self.assertEqual(final["calls"], [])
        self.assertEqual(self.attempts(), before_attempts)
        self.assertEqual(final["budget"]["attempts"], second["budget"]["attempts"])
        self.assertTrue(pending_ids.issubset({row["inbox_id"] for row in
                                            self.rows("SELECT * FROM event_inbox WHERE status='PENDING'")}),
                        "missing scripts lost prior pending event IDs")

    def test_legacy_single_task_runner_preserves_wait_until_exact_wake(self):
        request = self.invoke("submit", "--db", self.database, "--key", "legacy-wait-request",
                              "--text-file", FIXTURES / "s12_request.txt",
                              "--constraints-file", FIXTURES / "s12_constraints.json")["request"]["request_id"]
        workspace = self.workspaces["alpha"]
        run_arguments = ["run-fake", "--db", self.database, "--request", request,
                         "--workspace", workspace]
        initial = self.invoke(*run_arguments, "--worker-calls", 3,
                              "--script", FIXTURES / "s12_fake_workspace.json")
        task_id = initial["task_id"]
        self.assertEqual(len(self.tasks(request)), 1,
                         "this regression must exercise the legacy single-task runner, not its multi-task rejection")
        expected = b"verified from S1.2\n"
        self.assertEqual((workspace / "result.txt").read_bytes(), expected)
        self.invoke("wait", "--db", self.database, "--request", request, "--task", task_id,
                    "--reason", "等待宿主明确唤醒；重新打开旧入口不能自动换说明绕过等待。")
        waiting = self.rows("SELECT * FROM worker_waits WHERE task_id=?", (task_id,))[0]
        self.assertEqual(waiting["status"], "WAITING")
        attempts = self.attempts()
        budget = self.invoke("budget", "--db", self.database, "--request", request)
        operations = self.rows("SELECT * FROM ops ORDER BY rowid")
        results = self.rows("SELECT * FROM operation_results ORDER BY rowid")
        instructions = self.rows("SELECT * FROM instructions ORDER BY instruction_revision")
        receipts = self.feedback(task_id)

        # Reopening is a real writer takeover. It may fence execution epochs,
        # but must not let its automatic TeamLead step supersede the wait.
        self.invoke(*run_arguments, "--worker-calls", 3,
                    "--script", FIXTURES / "s12_fake_workspace.json", error="not_ready")
        self.assertEqual(self.attempts(), attempts, "the waiting legacy runner reserved another model attempt")
        self.assertEqual(self.invoke("budget", "--db", self.database, "--request", request), budget)
        self.assertEqual(self.rows("SELECT * FROM ops ORDER BY rowid"), operations)
        self.assertEqual(self.rows("SELECT * FROM operation_results ORDER BY rowid"), results)
        self.assertEqual(self.rows("SELECT * FROM instructions ORDER BY instruction_revision"), instructions,
                         "an automatic TeamLead instruction superseded the host's wait")
        self.assertEqual(self.feedback(task_id), receipts)
        self.assertEqual(self.rows("SELECT * FROM worker_waits WHERE task_id=?", (task_id,))[0], waiting)
        self.assertEqual((workspace / "result.txt").read_bytes(), expected)

        self.invoke("wake", "--db", self.database, "--request", request, "--task", task_id,
                    "--wait-generation", waiting["generation"])
        self.assertEqual(self.rows("SELECT status FROM worker_waits WHERE task_id=?", (task_id,))[0]["status"],
                         "AWAKE")
        source = self.fixture("s12_fake_workspace.json")
        lead = copy.deepcopy(next(step for step in source if step["role"] == "team_lead"))
        lead["tool_calls"][0]["arguments"]["approach"] = "唤醒后确认当前说明，读取已有成果，不重复旧文件写操作。"
        lead["tool_calls"][0]["arguments"]["steps"] = [{"id": "s1", "text": "读取 result.txt 的当前字节。"}]
        worker = copy.deepcopy(next(step for step in source
                                    if step["role"] == "worker" and step["tool_calls"][0]["name"] == "fs_read"))
        worker["role_call_index"] = 1
        worker["tool_calls"][0]["arguments"] = {"path": "result.txt", "max_bytes": 64}
        resumed = self.invoke(*run_arguments, "--script", self.json_file("legacy-after-wake.json", [lead, worker]))
        self.assertEqual([call["role"] for call in resumed["calls"]], ["worker"])
        self.assertEqual(resumed["budget"]["attempts"], budget["attempts"] + 1)
        self.assertEqual(base64.b64decode(resumed["calls"][-1]["result"]["content_base64"], validate=True), expected)
        self.assertEqual(self.rows("SELECT * FROM ops ORDER BY rowid"), operations,
                         "waking and observing the result must not repeat the old write or patch")
        self.assertEqual((workspace / "result.txt").read_bytes(), expected)
        current = self.tasks(request)["workspace-example"]
        self.assertEqual(current["applied_instruction_revision"], current["instruction_revision"])
        self.assertEqual(current["paused_for_change"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
