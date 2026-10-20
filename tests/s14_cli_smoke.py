#!/usr/bin/env python3
"""S1.4 subprocess regressions for durable questions and submission checks.

The C++ CLI owns every model attempt, state transition and artifact capture.
Python supplies fixed Fake responses, inspects persisted evidence and creates
disposable files. TeamLead checks here are scripted local checks, not product
acceptance, provider calls, strong-sandbox acceptance or business delivery.
"""
from __future__ import annotations

import base64
import copy
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest


if len(sys.argv) != 3:
    raise SystemExit("usage: s14_cli_smoke.py WLS_BINARY SOURCE_ROOT")
BINARY = Path(sys.argv[1]).resolve()
SOURCE = Path(sys.argv[2]).resolve()
FIXTURES = SOURCE / "tests" / "fixtures"
sys.argv = [sys.argv[0]]


class WorkflowCliSmoke(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="wls-s14-cli-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.database = self.directory / "runtime.sqlite"
        self.workspaces = {node: self.directory / node for node in ("alpha", "beta")}
        for directory in self.workspaces.values():
            directory.mkdir(mode=0o700)
        self.mapping = self.json_file("workspaces.json", {
            node: str(directory) for node, directory in self.workspaces.items()})

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
        with sqlite3.connect(self.database.as_uri() + "?mode=ro", uri=True) as db:
            db.row_factory = sqlite3.Row
            return [dict(row) for row in db.execute(query, arguments)]

    def snapshot(self):
        names = self.rows("SELECT name FROM sqlite_schema WHERE type='table' "
                          "AND name NOT LIKE 'sqlite_%' ORDER BY name")
        return {row["name"]: sorted(self.rows('SELECT * FROM "' + row["name"] + '"'), key=repr)
                for row in names}

    def json_file(self, name, value):
        path = self.directory / name
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return path

    def fixture(self, name):
        return json.loads((FIXTURES / name).read_text(encoding="utf-8"))

    def admit(self, key="workflow-request"):
        return self.invoke("submit", "--db", self.database, "--key", key,
                           "--text-file", FIXTURES / "s14_request.txt",
                           "--constraints-file", FIXTURES / "s14_constraints.json")["request"]["request_id"]

    def tasks(self, request):
        view = self.invoke("show", "--db", self.database, "--request", request)
        return {task["node_id"]: task for task in view["tasks"]}

    def questions(self, request, task=None):
        arguments = ["questions", "--db", self.database, "--request", request]
        if task is not None:
            arguments.extend(["--task", task])
        result = self.invoke(*arguments)
        self.assertEqual(result["request_id"], request)
        return result["questions"]

    def question(self, request, task):
        questions = self.questions(request, task)
        self.assertEqual(len(questions), 1, questions)
        return questions[0]

    def submissions(self, request, task):
        result = self.invoke("submissions", "--db", self.database,
                             "--request", request, "--task", task)
        self.assertEqual(result["request_id"], request)
        self.assertEqual(result["task_id"], task)
        return result["submissions"]

    def submission(self, request, task):
        submissions = self.submissions(request, task)
        self.assertEqual(len(submissions), 1, submissions)
        return submissions[0]

    def attempts(self, task=None, role=None):
        query, arguments = "SELECT rowid AS ordinal,* FROM call_attempts WHERE 1=1", []
        if task is not None:
            query += " AND task_id=?"
            arguments.append(task)
        if role is not None:
            query += " AND role=?"
            arguments.append(role)
        return self.rows(query + " ORDER BY rowid", arguments)

    def pending_ids(self, task, role):
        return {row["inbox_id"] for row in self.rows(
            "SELECT inbox_id FROM event_inbox WHERE actor_task_id=? AND role=? AND status='PENDING'",
            (task, role))}

    def pump(self, request, script=None, *, workers=1, max_calls=12):
        result = self.invoke("pump-fake", "--db", self.database, "--request", request,
                             "--script", script or FIXTURES / "s14_fake_ask.json",
                             "--workspaces-file", self.mapping,
                             "--worker-calls", workers, "--max-calls", max_calls)
        self.assertFalse(result["business_delivered"])
        self.assertFalse(result["environment_checks_executed"])
        self.assertEqual(result["real_model_calls"], 0)
        return result

    def step(self, role, name, arguments, index=1):
        return {"role": role, "role_call_index": index, "status": "completed",
                "tool_calls": [{"call_id": f"{role}-{name}-{index}", "name": name,
                                "arguments": copy.deepcopy(arguments)}],
                "usage": {"input_tokens": 100, "output_tokens": 20,
                          "cache_read_tokens": 0, "cache_write_tokens": 0,
                          "cost_micro_usd": 0}}

    def order(self, node="alpha"):
        return copy.deepcopy(self.fixture("s13_fake_initial.json")["nodes"][node][0])

    def check(self, passed=True):
        result = {"passed": passed,
                  "notes": "检查提交时的完整原始快照；业务验收和交付仍未执行。",
                  "criteria_results": [{"criterion_id": "ac1", "met": "yes" if passed else "partial",
                                        "note": "完整原始字节和版本已保存。"}]}
        if not passed:
            result["work_order"] = copy.deepcopy(next(
                step["tool_calls"][0]["arguments"]["work_order"]
                for step in self.fixture("s14_fake_rework.json")["nodes"]["alpha"]
                if step["role"] == "team_lead" and step["role_call_index"] == 2))
        return result

    def forward_to_user(self, request, task):
        script = self.json_file("forward-question.json", {
            "default": [self.step("dispatcher", "forward_question", {"note": "请用户确认，保留 Worker 原文。"})],
            "nodes": {"alpha": [self.step("team_lead", "forward_question",
                                             {"note": "本层无法依据当前契约消除歧义，原文上报。"})]}})
        # Both forwarding transitions fit one iterative pump. A second explicit
        # pump also permits an implementation that defers newly arrived inbox
        # work; neither invocation may borrow an unrelated node's script.
        self.pump(request, script)
        question = self.question(request, task)
        if question["holder"] == "dispatcher":
            self.pump(request, script)
            question = self.question(request, task)
        self.assertEqual(question["holder"], "user", question)
        self.assertEqual(question["status"], "OPEN")
        return question

    def answer(self, request, question, **options):
        arguments = ["answer", "--db", self.database, "--request", request,
                     "--question", question["question_id"],
                     "--expected-version", options.get("version", question["state_version"]),
                     "--text-file", options.get("text_file", FIXTURES / "s14_answer.txt")]
        if "selected_option" in options:
            arguments.extend(["--selected-option", options["selected_option"]])
        return self.invoke(*arguments, error=options.get("error"))

    def artifact_bytes(self, submission):
        snapshots = submission["artifact_snapshot"]
        self.assertEqual(len(snapshots), 1)
        artifact = snapshots[0]
        self.assertEqual(set(artifact), {"path", "sha256", "size_bytes", "content_base64"})
        content = base64.b64decode(artifact["content_base64"], validate=True)
        self.assertEqual(len(content), artifact["size_bytes"])
        self.assertEqual(hashlib.sha256(content).hexdigest(), artifact["sha256"])
        return content

    def context_messages(self, attempt):
        envelope = json.loads(attempt["request_envelope"])
        remaining = envelope["context"]["dynamic_suffix"].encode("utf-8")
        messages = []
        while remaining:
            header, separator, body = remaining.partition(b"\n")
            self.assertEqual(separator, b"\n")
            name, separator, digits = header.partition(b" ")
            self.assertEqual(separator, b" ")
            self.assertTrue(digits.isdigit())
            size = int(digits)
            self.assertEqual(body[size:size + 1], b"\n")
            value = body[:size].decode("utf-8")
            messages.append(json.loads(value) if name == b"message" else value)
            remaining = body[size + 1:]
        return messages

    def strings(self, value):
        if isinstance(value, str):
            yield value
            try:
                decoded = json.loads(value)
            except (ValueError, TypeError):
                return
            if isinstance(decoded, (dict, list)):
                yield from self.strings(decoded)
        elif isinstance(value, dict):
            for member in value.values():
                yield from self.strings(member)
        elif isinstance(value, list):
            for member in value:
                yield from self.strings(member)

    def test_blocking_question_survives_reopen_and_releases_sibling_progress(self):
        request = self.admit()
        self.pump(request)
        tasks = self.tasks(request)
        alpha, beta = tasks["alpha"]["task_id"], tasks["beta"]["task_id"]
        question = self.question(request, alpha)
        self.assertEqual(question["status"], "OPEN")
        self.assertEqual(question["holder"], "team_lead")
        original_text = self.fixture("s14_fake_ask.json")["nodes"]["alpha"][1]["tool_calls"][0]["arguments"]["text"]
        self.assertEqual(question["body"]["text"], original_text)
        before_alpha = self.attempts(alpha)
        self.assertEqual(question["original_binding"], json.loads(before_alpha[-1]["frozen_binding"]))
        waiting = self.rows("SELECT * FROM worker_waits WHERE task_id=?", (alpha,))[0]
        self.assertEqual(waiting["status"], "WAITING")
        self.assertEqual(question["wait_generation"], waiting["generation"])
        self.assertEqual((self.workspaces["beta"] / "r1.txt").read_bytes(), b"beta original\n")

        beta_script = self.json_file("beta-continues.json", {"default": [], "nodes": {
            "alpha": [], "beta": [self.order("beta"), self.step("worker", "fs_write", {
                "path": "followup.txt", "content": "beta continued while alpha waits\n"})]}})
        before_beta = self.attempts(beta, "worker")
        self.pump(request, beta_script)
        self.assertEqual(self.attempts(alpha), before_alpha,
                         "waiting alpha consumed a model call without a current answer")
        self.assertEqual(len(self.attempts(beta, "worker")), len(before_beta) + 1)
        self.assertEqual(self.question(request, alpha), question,
                         "writer takeover changed the original question or answer target")
        self.assertEqual((self.workspaces["beta"] / "followup.txt").read_bytes(),
                         b"beta continued while alpha waits\n")
        self.assertFalse((self.workspaces["alpha"] / "result.txt").exists())
        self.assertTrue(self.pending_ids(alpha, "team_lead"))

    def test_hierarchical_forwarding_preserves_original_question_and_root_answer(self):
        request = self.admit()
        routed = self.pump(request, FIXTURES / "s14_fake_question_route.json")
        self.assertEqual(len(routed["calls"]), 7)
        alpha = self.tasks(request)["alpha"]["task_id"]
        forwarded = self.question(request, alpha)
        original = copy.deepcopy(forwarded)
        self.assertEqual(forwarded["holder"], "user")
        self.assertEqual(forwarded["status"], "OPEN")
        asked = self.fixture("s14_fake_ask.json")["nodes"]["alpha"][1]["tool_calls"][0]["arguments"]
        self.assertEqual(forwarded["body"]["text"], asked["text"])
        self.assertEqual(forwarded["body"]["context"], asked["context"])
        self.assertEqual(forwarded["original_binding"],
                         json.loads(self.attempts(alpha, "worker")[0]["frozen_binding"]))
        self.assertEqual([item["role"] for item in forwarded["forwarded"]],
                         ["team_lead", "dispatcher"])
        self.assertTrue(all(item["note"] for item in forwarded["forwarded"]))
        self.answer(request, forwarded, selected_option="approved")
        answered = self.question(request, alpha)
        self.assertEqual(answered["status"], "ANSWERED")
        self.assertEqual(answered["holder"], "answered")
        expected_text = (FIXTURES / "s14_answer.txt").read_text(encoding="utf-8")
        self.assertEqual(answered["answer"]["text"], expected_text)
        self.assertEqual(answered["answer"]["selected_option"], "approved")
        self.assertEqual(answered["answer"]["answered_by"], "user")
        self.assertEqual(answered["answer"]["requirements_revision"], 1)
        self.assertEqual(answered["original_binding"], original["original_binding"])

        script = self.json_file("answered-worker.json", {"default": [], "nodes": {
            "alpha": [self.order(), self.step("worker", "fs_write", {
                "path": "result.txt", "content": "alpha approved\n"})]}})
        self.pump(request, script)
        self.assertEqual((self.workspaces["alpha"] / "result.txt").read_bytes(), b"alpha approved\n")
        resumed = self.attempts(alpha, "worker")[-1]
        values = list(self.strings(self.context_messages(resumed)))
        self.assertIn(expected_text, values, "original answer is missing from the resumed Worker's context")
        self.assertIn(original["body"]["text"], values)
        self.assertEqual(self.question(request, alpha)["original_binding"], original["original_binding"])

    def test_answer_preflight_rejects_wrong_owner_version_option_and_repeated_answer(self):
        request = self.admit()
        self.pump(request)
        alpha = self.tasks(request)["alpha"]["task_id"]
        initial = self.question(request, alpha)
        foreign = self.admit("foreign-request")
        before = self.snapshot()
        self.answer(foreign, initial, error="unauthorized")
        self.assertEqual(self.snapshot(), before)
        self.answer(request, initial, error="not_ready")
        self.assertEqual(self.snapshot(), before, "Root answered a question still owned by TeamLead")

        question = self.forward_to_user(request, alpha)
        before = self.snapshot()
        self.answer(request, question, version=question["state_version"] - 1, error="stale")
        self.assertEqual(self.snapshot(), before, "a stale UI response opened a writer")
        self.answer(request, question, selected_option="nonexistent-option", error="invalid_input")
        self.assertEqual(self.snapshot(), before, "an unlisted option opened a writer")
        # A host policy change must reject already captured old-policy calls,
        # while a fresh Root control answer can still settle the same question.
        # Otherwise the open question also blocks wake and new instructions.
        with sqlite3.connect(self.database) as db:
            db.execute("UPDATE runtime_policy SET denied_tools=?,policy_epoch=policy_epoch+1,"
                       "state_version=state_version+1 WHERE singleton=1", (json.dumps(["run"]),))
        self.answer(request, question, selected_option="approved")
        self.assertEqual(self.question(request, alpha)["original_binding"], question["original_binding"])
        self.assertGreater(self.rows("SELECT policy_epoch FROM runtime_policy")[0]["policy_epoch"],
                           question["original_binding"]["policy_epoch"])
        after = self.snapshot()
        self.answer(request, question, error="stale")
        self.assertEqual(self.snapshot(), after, "a repeated old answer took over or rewrote original evidence")

    def test_manual_wake_and_legacy_runner_cannot_bypass_an_open_question(self):
        request = self.admit()
        script = self.fixture("s14_fake_ask.json")
        script["default"][0]["tool_calls"][0]["arguments"]["nodes"] = script["default"][0]["tool_calls"][0]["arguments"]["nodes"][:1]
        script["nodes"].pop("beta")
        self.pump(request, self.json_file("single-ask.json", script))
        alpha = self.tasks(request)["alpha"]["task_id"]
        question = self.question(request, alpha)
        attempts = self.attempts()
        self.invoke("wake", "--db", self.database, "--request", request,
                    "--task", alpha, "--wait-generation", question["wait_generation"],
                    error="not_ready")
        legacy = self.json_file("legacy-cannot-reissue-order.json", [
            self.order(), self.step("worker", "fs_write", {
                "path": "bypassed.txt", "content": "must never be written"})])
        self.invoke("run-fake", "--db", self.database, "--request", request,
                    "--workspace", self.workspaces["alpha"], "--script", legacy,
                    error="not_ready")
        self.assertEqual(self.attempts(), attempts)
        self.assertEqual(self.question(request, alpha), question)
        self.assertFalse((self.workspaces["alpha"] / "bypassed.txt").exists())
        self.assertEqual(self.rows("SELECT status FROM worker_waits WHERE task_id=?", (alpha,))[0]["status"],
                         "WAITING")

    def test_submission_captures_complete_original_bytes_and_stops_further_worker_calls(self):
        request = self.admit()
        script = self.fixture("s14_fake_submit.json")
        script["nodes"]["alpha"].append(self.step("worker", "fs_write", {
            "path": "after-submit.txt", "content": "must not run while checking"}, 3))
        run = self.pump(request, self.json_file("submit-then-forbidden-write.json", script), workers=4)
        alpha = self.tasks(request)["alpha"]["task_id"]
        submission = self.submission(request, alpha)
        self.assertEqual(submission["status"], "SUBMITTED")
        self.assertEqual(self.artifact_bytes(submission), b"alpha candidate version 1\n")
        self.assertEqual(submission["artifact_snapshot"][0]["path"], "/work/result.txt")
        workers = self.attempts(alpha, "worker")
        self.assertEqual(len(workers), 2)
        self.assertEqual(submission["original_binding"], json.loads(workers[-1]["frozen_binding"]))
        for field, value in submission["original_binding"].items():
            self.assertEqual(submission["body"][field], value)
        self.assertEqual(sum(call["role"] == "worker" for call in run["calls"]), 2)
        self.assertFalse((self.workspaces["alpha"] / "after-submit.txt").exists())
        self.assertEqual(self.tasks(request)["alpha"]["status"], "SUBMITTED")
        self.assertTrue(self.pending_ids(alpha, "team_lead"))
        empty = self.json_file("submitted-no-check-script.json", {"default": [], "nodes": {"alpha": []}})
        self.assertEqual(self.pump(request, empty, workers=4)["calls"], [])
        self.assertEqual(self.submission(request, alpha), submission)
        attempts = self.attempts()
        legacy = self.json_file("legacy-cannot-overwrite-submission.json", [
            self.order(), self.step("worker", "fs_write", {
                "path": "after-submit.txt", "content": "legacy runner must also wait"})])
        self.invoke("run-fake", "--db", self.database, "--request", request,
                    "--workspace", self.workspaces["alpha"], "--script", legacy,
                    error="not_ready")
        self.assertEqual(self.attempts(), attempts)
        self.assertEqual(self.submission(request, alpha), submission)
        self.assertFalse((self.workspaces["alpha"] / "after-submit.txt").exists())

    def test_team_lead_check_uses_original_snapshot_after_host_file_changes(self):
        request = self.admit()
        self.pump(request, FIXTURES / "s14_fake_submit.json", workers=2)
        alpha = self.tasks(request)["alpha"]["task_id"]
        original = self.submission(request, alpha)
        changed = b"host replacement after submission; never old evidence\n"
        (self.workspaces["alpha"] / "result.txt").write_bytes(changed)
        script = self.json_file("check-submission.json", {"default": [], "nodes": {
            "alpha": [self.step("team_lead", "check_submission", self.check())]}})
        self.pump(request, script, max_calls=1)
        checked = self.submission(request, alpha)
        self.assertEqual(checked["status"], "CANDIDATE_READY")
        self.assertEqual(checked["artifact_snapshot"], original["artifact_snapshot"])
        self.assertEqual(checked["original_binding"], original["original_binding"])
        self.assertEqual(checked["body"], original["body"])
        self.assertEqual(len(checked["checks"]), 1)
        self.assertFalse(checked["business_accepted"])
        self.assertFalse(checked["business_delivered"])
        self.assertNotEqual(self.tasks(request)["alpha"]["status"], "ACCEPTED")
        self.assertEqual((self.workspaces["alpha"] / "result.txt").read_bytes(), changed)
        lead = self.attempts(alpha, "team_lead")[-1]
        values = list(self.strings(self.context_messages(lead)))
        self.assertIn(original["artifact_snapshot"][0]["content_base64"], values,
                      "TeamLead was not given the immutable submitted artifact")
        self.assertNotIn(base64.b64encode(changed).decode("ascii"), values)

    def test_rework_requires_new_instruction_ack_before_second_version_submission(self):
        request = self.admit()
        self.pump(request, FIXTURES / "s14_fake_submit.json", workers=2)
        before = self.tasks(request)["alpha"]
        alpha = before["task_id"]
        original = self.submission(request, alpha)
        operations = self.rows("SELECT * FROM ops ORDER BY rowid")
        rework = self.json_file("return-for-rework.json", {"default": [], "nodes": {
            "alpha": [self.step("team_lead", "check_submission", self.check(False))]}})
        self.pump(request, rework, max_calls=1)
        returned = self.submission(request, alpha)
        task = self.tasks(request)["alpha"]
        self.assertEqual(returned["status"], "RETURNED_FOR_REWORK")
        self.assertEqual(returned["artifact_snapshot"], original["artifact_snapshot"])
        self.assertEqual(returned["original_binding"], original["original_binding"])
        self.assertGreater(task["instruction_revision"], before["instruction_revision"])
        self.assertGreater(task["task_revision"], before["task_revision"])
        checks = self.rows("SELECT seq FROM events WHERE request_id=? AND type='check_submission' ORDER BY seq",
                           (request,))
        self.assertEqual(len(checks), 1)
        # The finite host may perform a free explicit Worker ack in this pump
        # even when no further model calls fit. Applied state must have its own
        # Worker event; the TeamLead check itself cannot forge confirmation.
        if task["applied_instruction_revision"] == task["instruction_revision"]:
            acknowledgments = self.rows("SELECT * FROM events WHERE request_id=? AND type='ack' AND seq>? ORDER BY seq",
                                        (request, checks[0]["seq"]))
            self.assertTrue(any(row["actor"] == "worker" and
                                json.loads(row["payload"])["binding"]["instruction_revision"] == task["instruction_revision"]
                                for row in acknowledgments), "new applied state has no separate Worker ack")
        self.assertEqual(self.rows("SELECT * FROM ops ORDER BY rowid"), operations)
        self.assertFalse((self.workspaces["alpha"] / "result-v2.txt").exists())

        source = self.fixture("s14_fake_rework.json")["nodes"]["alpha"]
        order = self.order()
        order["tool_calls"][0]["arguments"] = self.check(False)["work_order"]
        worker_steps = [copy.deepcopy(step) for step in source
                        if step["role"] == "worker" and step["role_call_index"] in (3, 4)]
        for index, step in enumerate(worker_steps, 1):
            step["role_call_index"] = index
        script = self.json_file("resubmit-current-version.json", {"default": [], "nodes": {
            "alpha": [order, *worker_steps]}})
        self.pump(request, script, workers=2)
        submissions = self.submissions(request, alpha)
        self.assertEqual(len(submissions), 2)
        old = next(item for item in submissions if item["submission_id"] == original["submission_id"])
        current = next(item for item in submissions if item["submission_id"] != original["submission_id"])
        self.assertEqual(old["status"], "RETURNED_FOR_REWORK")
        self.assertEqual(old["artifact_snapshot"], original["artifact_snapshot"])
        self.assertEqual(current["status"], "SUBMITTED")
        self.assertEqual(self.artifact_bytes(current), b"alpha candidate version 2\n")
        self.assertGreater(current["original_binding"]["instruction_revision"],
                           original["original_binding"]["instruction_revision"])
        task = self.tasks(request)["alpha"]
        self.assertEqual(task["applied_instruction_revision"], task["instruction_revision"])
        self.assertEqual(task["applied_task_revision"], task["task_revision"])
        self.assertEqual(task["applied_plan_revision"], task["plan_revision"])
        self.assertEqual((self.workspaces["alpha"] / "result.txt").read_bytes(),
                         b"alpha candidate version 1\n")
        self.assertEqual((self.workspaces["alpha"] / "result-v2.txt").read_bytes(),
                         b"alpha candidate version 2\n")

    def test_one_finite_pump_completes_local_rework_without_business_acceptance(self):
        request = self.admit()
        run = self.pump(request, FIXTURES / "s14_fake_rework.json", workers=4)
        alpha = self.tasks(request)["alpha"]["task_id"]
        submissions = self.submissions(request, alpha)
        self.assertEqual(len(submissions), 2)
        self.assertEqual({item["status"] for item in submissions},
                         {"RETURNED_FOR_REWORK", "CANDIDATE_READY"})
        self.assertEqual({self.artifact_bytes(item) for item in submissions},
                         {b"alpha candidate version 1\n", b"alpha candidate version 2\n"})
        self.assertEqual(len(run["calls"]), 8)
        self.assertEqual(run["budget"]["attempts"], 8)
        self.assertFalse(any("error" in call for call in run["calls"]), run)
        self.assertTrue(all(not item["business_accepted"] and not item["business_delivered"]
                            for item in submissions))
        self.assertNotEqual(run["request_status"], "DELIVERED")

    def test_amendment_supersedes_old_questions_and_submissions_without_adopting_them(self):
        request = self.admit()
        script = self.fixture("s14_fake_ask.json")
        submitted = copy.deepcopy(self.fixture("s14_fake_submit.json")["nodes"]["alpha"][2])
        submitted["tool_calls"][0]["arguments"]["deliverables"][0]["path"] = "/work/r1.txt"
        script["nodes"]["beta"].append(submitted)
        self.pump(request, self.json_file("question-and-submission.json", script), workers=2)
        tasks = self.tasks(request)
        alpha, beta = tasks["alpha"]["task_id"], tasks["beta"]["task_id"]
        question = self.question(request, alpha)
        submission = self.submission(request, beta)
        attempts = self.attempts()
        self.invoke("amend", "--db", self.database, "--request", request,
                    "--text-file", FIXTURES / "s13_amendment.txt",
                    "--constraints-file", FIXTURES / "s13_amended_constraints.json")
        superseded_question = self.question(request, alpha)
        superseded_submission = self.submission(request, beta)
        self.assertEqual(superseded_question["status"], "SUPERSEDED")
        self.assertEqual(superseded_submission["status"], "SUPERSEDED")
        self.assertEqual(superseded_question["body"], question["body"])
        self.assertEqual(superseded_question["original_binding"], question["original_binding"])
        self.assertEqual(superseded_submission["artifact_snapshot"], submission["artifact_snapshot"])
        self.assertEqual(superseded_submission["original_binding"], submission["original_binding"])
        before_answer = self.snapshot()
        self.answer(request, question, error="stale")
        self.assertEqual(self.snapshot(), before_answer)
        old_actions = self.json_file("cannot-answer-or-check-old-content.json", {"default": [], "nodes": {
            "alpha": [self.step("team_lead", "answer_question", {"text": "旧答案不能批准新需求。"})],
            "beta": [self.step("team_lead", "check_submission", self.check())]}})
        run = self.pump(request, old_actions, workers=2)
        self.assertEqual(run["calls"], [])
        self.assertEqual(self.attempts(), attempts)
        self.assertTrue(all(task["paused_for_change"] for task in self.tasks(request).values()))
        self.assertFalse(any(item["status"] == "CANDIDATE_READY" for item in self.submissions(request, beta)))

    def test_invalid_model_fields_and_missing_scripts_preserve_specific_pending_objects(self):
        request = self.admit()
        self.pump(request)
        alpha = self.tasks(request)["alpha"]["task_id"]
        question = self.question(request, alpha)
        pending = self.pending_ids(alpha, "team_lead")
        before = len(self.attempts())
        forged = self.json_file("forged-question-target.json", {"default": [], "nodes": {
            "alpha": [self.step("team_lead", "answer_question", {
                "text": "不得自行填入对象或 CAS。", "question_id": question["question_id"]})]}})
        run = self.pump(request, forged)
        self.assertEqual(len(run["calls"]), 1)
        self.assertEqual(run["calls"][0]["error"]["code"], "invalid_input")
        self.assertEqual(len(self.attempts()), before + 1,
                         "a consumed invalid Fake response must still be accounted for")
        self.assertEqual(self.question(request, alpha), question)
        self.assertTrue(pending.issubset(self.pending_ids(alpha, "team_lead")))
        no_steps = self.json_file("missing-question-steps.json", {"default": [], "nodes": {"alpha": []}})
        attempts = self.attempts()
        self.assertEqual(self.pump(request, no_steps)["calls"], [])
        self.assertEqual(self.attempts(), attempts)
        self.assertEqual(self.question(request, alpha), question)
        self.assertTrue(pending.issubset(self.pending_ids(alpha, "team_lead")))

    def test_workflow_queries_and_invalid_host_options_do_not_change_persistent_rows(self):
        request = self.admit()
        self.pump(request)
        tasks = self.tasks(request)
        alpha, beta = tasks["alpha"]["task_id"], tasks["beta"]["task_id"]
        question = self.question(request, alpha)
        foreign = self.admit("query-other-request")
        before = self.snapshot()
        self.assertEqual(self.questions(request), [question])
        self.assertEqual(self.questions(request, beta), [])
        self.assertEqual(self.submissions(request, alpha), [])
        self.invoke("questions", "--db", self.database, "--request", foreign,
                    "--task", alpha, error="unauthorized")
        self.invoke("submissions", "--db", self.database, "--request", foreign,
                    "--task", alpha, error="unauthorized")
        empty = self.directory / "empty-answer.txt"
        empty.write_text(" \t\r\n", encoding="utf-8")
        oversized = self.directory / "oversized-answer.txt"
        oversized.write_text("x" * 4001, encoding="utf-8")
        query_base = ["questions", "--db", self.database, "--request", request]
        submission_base = ["submissions", "--db", self.database, "--request", request, "--task", alpha]
        answer_base = ["answer", "--db", self.database, "--request", request,
                       "--question", question["question_id"], "--expected-version", question["state_version"]]
        invalid = [query_base + ["--limit", 0], query_base + ["--limit", 129],
                   query_base + ["--limit", ""], submission_base + ["--limit", 65],
                   submission_base + ["--unknown-option", "value"],
                   ["submissions", "--db", self.database, "--request", request],
                   answer_base, answer_base + ["--text-file", empty],
                   answer_base + ["--text-file", oversized],
                   answer_base + ["--selected-option", ""],
                   answer_base + ["--selected-option", "approved", "--command-id", "caller-selected"],
                   answer_base + ["--selected-option", "approved", "--expected-version", 2],
                   ["answer", "--db", self.database, "--request", request,
                    "--question", question["question_id"], "--text-file", FIXTURES / "s14_answer.txt"]]
        for index, arguments in enumerate(invalid):
            with self.subTest(invalid=index):
                self.invoke(*arguments, error="invalid_input")
                self.assertEqual(self.snapshot(), before,
                                 "invalid host input opened a writer or changed an epoch, claim or attempt")
        self.invoke("history", "--db", self.database, "--request", request,
                    "--role", "worker", "--task", alpha)
        self.invoke("show", "--db", self.database, "--request", request)
        self.assertEqual(self.snapshot(), before)

    def test_submission_capture_rejects_missing_and_escaping_files_without_candidates(self):
        outside = self.directory / "outside.txt"
        outside.write_bytes(b"self-created outside sentinel\n")
        source = self.fixture("s14_fake_submit.json")
        for index, path in enumerate(("/work/missing.txt", "/work/escape.txt", "/work/../outside.txt")):
            with self.subTest(path=path):
                # A distinct request also needs distinct pinned workspace
                # identities; otherwise a mapping conflict would hide the
                # specific missing-file or path-escape boundary being tested.
                self.workspaces = {node: self.directory / f"{node}-{index}"
                                   for node in ("alpha", "beta")}
                for directory in self.workspaces.values():
                    directory.mkdir(mode=0o700)
                self.mapping = self.json_file(f"workspaces-{index}.json", {
                    node: str(directory) for node, directory in self.workspaces.items()})
                (self.workspaces["alpha"] / "escape.txt").symlink_to(outside)
                request = self.admit(f"bad-artifact-{index}")
                script = copy.deepcopy(source)
                submission_step = script["nodes"]["alpha"][2]
                submission_step["role_call_index"] = 1
                submission_step["tool_calls"][0]["arguments"]["deliverables"][0]["path"] = path
                script["nodes"]["alpha"] = [script["nodes"]["alpha"][0], submission_step]
                run = self.pump(request, self.json_file(f"bad-artifact-{index}.json", script))
                alpha = self.tasks(request)["alpha"]["task_id"]
                self.assertTrue(any("error" in call for call in run["calls"]), run)
                self.assertEqual(self.submissions(request, alpha), [])
                self.assertNotEqual(self.tasks(request)["alpha"]["status"], "SUBMITTED")
                self.assertEqual(self.rows("SELECT * FROM ops WHERE task_id=?", (alpha,)), [])
                self.assertTrue(self.pending_ids(alpha, "worker"))
                self.assertEqual(outside.read_bytes(), b"self-created outside sentinel\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
