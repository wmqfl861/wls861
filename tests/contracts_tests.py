#!/usr/bin/env python3
"""Offline Schema/semantic and real SQLite reference regressions; no model/tools."""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec)
    sys.modules[name] = value
    spec.loader.exec_module(value)
    return value


store_ref = module("wls_contract_store", "db/contract_store.py")
contracts = module("wls_validate_contracts", "scripts/validate_contracts.py")
from jsonschema.exceptions import ValidationError, RefResolutionError


class SchemaContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.registry = contracts.Registry()

    def example(self, name):
        return json.loads((ROOT / "schemas" / "examples" / name).read_text())

    def test_all_local_schemas_examples_and_nested_briefs(self):
        with patch("socket.socket", side_effect=AssertionError("network is forbidden")):
            self.assertEqual(contracts.validate_examples(), (17, 13))
        plan = self.example("plan.json")
        del plan["nodes"][0]["brief"]["constraint_refs"]
        with self.assertRaises(ValidationError):
            self.registry.validate("plan", plan)

    def test_unknown_external_reference_fails_without_network(self):
        with tempfile.TemporaryDirectory() as temp:
            schema = {"$schema": "http://json-schema.org/draft-07/schema#", "$id": "https://local.invalid/test", "$ref": "https://outside.invalid/secret"}
            Path(temp, "test.schema.json").write_text(json.dumps(schema))
            with patch("socket.socket", side_effect=AssertionError("network is forbidden")):
                with self.assertRaisesRegex(contracts.SemanticError, "unknown/offline"):
                    contracts.Registry(temp)
                resolver = contracts.OfflineResolver.from_schema(schema, store={})
                with self.assertRaises(RefResolutionError):
                    resolver.resolve("https://outside.invalid/secret")

    def test_runtime_projection_rejects_forged_metadata_and_worker_self_approval(self):
        document = self.example("submission.json")
        document = {key: value for key, value in document.items()
                    if not self.registry.schemas["submission"]["properties"][key].get("x-runtime-field")}
        for usage in document.get("used_memories", []):
            usage.pop("revision")
        self.registry.validate("submission", document, model_input=True)
        for field, value in (("requirements_revision", 99), ("team_lead_check", {"passed": True})):
            with self.subTest(field=field):
                forged = copy.deepcopy(document)
                forged[field] = value
                with self.assertRaises(ValidationError):
                    self.registry.validate("submission", forged, model_input=True)
        document["used_memories"][0]["revision"] = "a" * 40
        with self.assertRaises(ValidationError):
            self.registry.validate("submission", document, model_input=True)

    def test_complete_submission_requires_load_and_revision(self):
        good = self.example("submission.json")
        self.registry.validate("submission", good)
        for key in ("load_id", "revision"):
            bad = copy.deepcopy(good)
            del bad["used_memories"][0][key]
            with self.assertRaises(ValidationError):
                self.registry.validate("submission", bad)
        bad = copy.deepcopy(good)
        del bad["instruction_revision"]
        with self.assertRaises(ValidationError):
            self.registry.validate("submission", bad)

    def test_projection_keeps_same_named_business_fields_in_separate_scopes(self):
        business = {
            "type": "object",
            "properties": {"host_epoch": {"type": "integer"}, "proof": {"type": "string"},
                           "inner_token": {"type": "string", "x-runtime-field": True}},
            "required": ["host_epoch", "inner_token"],
            "allOf": [{"if": {"required": ["host_epoch"]}, "then": {"required": ["proof"]}}],
        }
        literal = {"id": "business literal", "$ref": "https://unregistered.invalid/literal",
                   "properties": {"literal": {"x-runtime-field": True}}, "required": ["literal"]}
        schema = {
            "$schema": "http://json-schema.org/draft-07/schema#", "$id": "https://local.invalid/scopes",
            "type": "object",
            "properties": {
                "host_epoch": {"type": "integer", "x-runtime-field": True},
                "inner_token": {"type": "string"}, "business": business,
                "entries": {"type": "array", "items": business}, "literal": {"const": literal},
            },
            "required": ["host_epoch", "inner_token", "business", "entries", "literal"],
            "allOf": [{"required": ["host_epoch"]},
                      {"if": {"required": ["host_epoch"]}, "then": {"required": ["host_only_detail"]}}],
        }
        good = {"inner_token": "normal root business value", "business": {"host_epoch": 7, "proof": "yes"},
                "entries": [{"host_epoch": 8, "proof": "yes"}], "literal": literal}
        with tempfile.TemporaryDirectory() as temp:
            Path(temp, "scopes.schema.json").write_text(json.dumps(schema))
            registry = contracts.Registry(temp)
            registry.validate("scopes", good, model_input=True)
            for field in ("host_epoch", "proof"):
                for location in ("business", "entries"):
                    with self.subTest(field=field, location=location):
                        bad = copy.deepcopy(good)
                        target = bad[location] if location == "business" else bad[location][0]
                        del target[field]
                        with self.assertRaises(ValidationError):
                            registry.validate("scopes", bad, model_input=True)
            missing = copy.deepcopy(good)
            del missing["inner_token"]
            forged = copy.deepcopy(good)
            forged["host_epoch"] = 99
            nested_forged = copy.deepcopy(good)
            nested_forged["business"]["inner_token"] = "forged"
            for bad in (missing, forged, nested_forged):
                with self.assertRaises(ValidationError):
                    registry.validate("scopes", bad, model_input=True)
            self.assertEqual(contracts.project_tool_schema(schema)["properties"]["literal"]["const"], literal)

    def test_findings_51_and_100_round_trip_but_101_rejected(self):
        for count in (51, 100):
            verdict = self.example("review_verdict.json")
            findings = [{"id": f"f{i}", "severity": "major", "text": f"independent finding {i}"} for i in range(1, count + 1)]
            verdict["findings"] = findings
            self.registry.validate("review_verdict", verdict)
            parent = self.example("change_order.dispatcher.json")
            relay = self.example("change_order.team_lead.json")
            parent["findings"] = copy.deepcopy(findings)
            relay["findings"] = copy.deepcopy(findings)
            self.registry.validate("change_order", parent)
            self.registry.validate("change_order", relay)
            contracts.validate_change_order(relay, parent, self.example("request_texts.json"),
                assigned_constraint_refs=["c1", "c2", "c3", "c4", "c5"], frozen_target=relay)
        findings.append({"id": "f101", "severity": "major", "text": "one too many"})
        for name, document in (("review_verdict", verdict), ("change_order", relay)):
            document["findings"] = findings
            with self.assertRaises(ValidationError):
                self.registry.validate(name, document)

    def test_schema_pass_does_not_allow_omitted_original_constraint(self):
        brief = self.example("task_brief.json")
        brief["constraint_refs"].remove("c2")
        brief["constraints_verbatim"].remove("不要修改原始文件")
        self.registry.validate("task_brief", brief)  # Shape is valid: this is a semantic regression.
        with self.assertRaisesRegex(contracts.SemanticError, "assigned constraint"):
            contracts.validate_constraints(brief, self.example("request_texts.json"), assigned_constraint_refs=["c1", "c2", "c3"])

    def test_relay_cannot_weaken_findings_or_acceptance(self):
        parent = self.example("change_order.dispatcher.json")
        relay = self.example("change_order.team_lead.json")
        parent["findings"] = [{"id": "f1", "severity": "major", "text": "must preserve refunds"}]
        relay["findings"] = copy.deepcopy(parent["findings"])
        for weaken in ("finding", "criteria", "other_task"):
            bad = copy.deepcopy(relay)
            if weaken == "finding":
                bad["findings"][0]["severity"] = "nit"
            elif weaken == "criteria":
                bad["acceptance_changes"] = []
            else:
                bad["task_id"] = "tsk_01JBC7S1A2B3C4D5E6F7G8H9JK"
            self.registry.validate("change_order", bad)
            with self.assertRaises(contracts.SemanticError):
                contracts.validate_change_order(bad, parent, self.example("request_texts.json"),
                    assigned_constraint_refs=["c1", "c2", "c3", "c4", "c5"], frozen_target=relay)

    def review_context(self):
        verdict = self.example("review_verdict.json")
        ticket = self.example("review_request.json")
        source = {key: ticket[key] for key in contracts.REVIEW_SOURCE_FIELDS}
        reviewer = {key: ticket["reviewer_" + key] for key in contracts.EXECUTION_FIELDS}
        return verdict, ticket, source, reviewer

    def test_frozen_review_rejects_old_candidate_and_reviewer_restart(self):
        verdict, ticket, source, reviewer = self.review_context()
        contracts.validate_review_response(verdict, frozen_ticket=ticket, current_source=source, current_reviewer=reviewer)
        source["candidate_sha256"] = "c" * 64
        with self.assertRaises(contracts.SemanticError):
            contracts.validate_review_response(verdict, frozen_ticket=ticket, current_source=source, current_reviewer=reviewer)
        verdict, ticket, source, reviewer = self.review_context()
        reviewer["execution_epoch"] += 1
        with self.assertRaisesRegex(contracts.SemanticError, "reviewer"):
            contracts.validate_review_response(verdict, frozen_ticket=ticket, current_source=source, current_reviewer=reviewer)
        verdict["reviewer_execution_epoch"] = reviewer["execution_epoch"]  # Retagging a late response also fails.
        with self.assertRaisesRegex(contracts.SemanticError, "frozen"):
            contracts.validate_review_response(verdict, frozen_ticket=ticket, current_source=source, current_reviewer=reviewer)

    def test_deterministic_evidence_not_schema_claim_controls_acceptance(self):
        verdict, ticket, source, reviewer = self.review_context()
        verdict.update(verdict="pass", findings=[], evidence=[{"check": "totals", "result": "pass"}])
        self.registry.validate("review_verdict", verdict)
        kwargs = dict(frozen_ticket=ticket, current_source=source, current_reviewer=reviewer, required_checks=["totals"])
        with self.assertRaisesRegex(contracts.SemanticError, "missing"):
            contracts.require_product_acceptance(verdict, host_verifications=[], **kwargs)
        evidence = {key: ticket[key] for key in contracts.REVIEW_FIELDS}
        evidence.update(criterion_id="totals", result="fail", runner_id="host-fixture", evidence_ref="fixture:run/1")
        with self.assertRaisesRegex(contracts.SemanticError, "failed"):
            contracts.require_product_acceptance(verdict, host_verifications=[evidence], **kwargs)
        evidence["result"] = "pass"
        self.assertTrue(contracts.require_product_acceptance(verdict, host_verifications=[evidence], **kwargs))
        evidence["candidate_sha256"] = "d" * 64
        with self.assertRaises(contracts.SemanticError):
            contracts.require_product_acceptance(verdict, host_verifications=[evidence], **kwargs)

    def test_user_waiver_is_bound_and_cannot_waive_failed_deterministic_check(self):
        verdict, ticket, source, reviewer = self.review_context()
        kwargs = dict(frozen_ticket=ticket, current_source=source, current_reviewer=reviewer, required_checks=["totals"])
        evidence = {key: ticket[key] for key in contracts.REVIEW_FIELDS}
        evidence.update(criterion_id="totals", result="pass", runner_id="host-fixture", evidence_ref="fixture:run/1")
        waiver = {key: ticket[key] for key in contracts.REVIEW_FIELDS}
        waiver.update(actor_role="dispatcher", evidence_ref="fixture:user-command", finding_ids=["f1"])
        with self.assertRaises(contracts.SemanticError):
            contracts.require_product_acceptance(verdict, host_verifications=[evidence], host_user_waiver=waiver, **kwargs)
        waiver["actor_role"] = "user"
        self.assertTrue(contracts.require_product_acceptance(verdict, host_verifications=[evidence], host_user_waiver=waiver, **kwargs))
        evidence["result"] = "fail"
        with self.assertRaises(contracts.SemanticError):
            contracts.require_product_acceptance(verdict, host_verifications=[evidence], host_user_waiver=waiver, **kwargs)

    def test_submission_cannot_be_retagged_or_omit_current_criteria(self):
        submission = self.example("submission.json")
        binding = {key: submission[key] for key in contracts.EXECUTION_FIELDS}
        criteria = ["ac1", "ac2", "ac3", "ac4", "ac5", "ac6"]
        contracts.validate_submission(submission, frozen_call_binding=binding, current_binding=binding, authoritative_criteria=criteria)
        new_binding = dict(binding, instruction_revision=3)
        submission["instruction_revision"] = 3
        with self.assertRaisesRegex(contracts.SemanticError, "retagged"):
            contracts.validate_submission(submission, frozen_call_binding=binding, current_binding=new_binding, authoritative_criteria=criteria)
        submission["instruction_revision"] = 2
        submission["self_check"].pop()
        self.registry.validate("submission", submission)
        with self.assertRaises(contracts.SemanticError):
            contracts.validate_submission(submission, frozen_call_binding=binding, current_binding=binding, authoritative_criteria=criteria)

    def test_revision_proposal_requires_expected_base_and_safe_delivery_name(self):
        report = self.example("experience_report.json")
        entry = report["entries"][0]
        entry["kind"] = "revise"
        entry["target_memory_id"] = "exp-20261017-k3f9"
        entry.pop("expected_base_revision", None)
        with self.assertRaises(ValidationError):
            self.registry.validate("experience_report", report)
        entry["expected_base_revision"] = "a" * 40
        # The existing example's other fields remain legal for a revision proposal.
        self.registry.validate("experience_report", report)
        for name in (".", "..", "../x", "a/b", "a\\b"):
            manifest = self.example("delivery_manifest.json")
            manifest["deliverables"][0]["name"] = name
            with self.assertRaises(ValidationError):
                self.registry.validate("delivery_manifest", manifest)


class SQLiteContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="wls-contract-")
        self.path = Path(self.temp.name) / "reference.sqlite"
        self.store = store_ref.ContractStore(self.path, initialize=True)
        self.fixture("r1", "t1", "g1")

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def row(self, table, key, value):
        return self.store.one(f"SELECT * FROM {table} WHERE {key}=?", (value,))

    def version(self, table, key, value):
        return self.row(table, key, value)["state_version"]

    def fixture(self, request, task, team):
        self.store.admit_request(request, tenant_id="tenant", project_id="project", root_id="root", key=request,
            original_text="preserve input", tool_allowlist=["fixture.write", "fixture.read"])
        dispatcher = self.store.actor("dispatcher", request)
        self.store.publish_plan(dispatcher, expected_version=1, body={"fixture": True}, command_id="plan-1")
        self.add_team(request, task, team)

    def add_team(self, request, task, team):
        self.store.create_task(self.store.actor("dispatcher", request), task, team, node_id=task, brief={"fixture": "input unchanged"})
        lead = self.store.actor("team_lead", request, team)
        binding = self.store.issue_instruction(lead, expected_task_version=1, body={"steps": ["fixture only"]}, command_id="instruction:" + task)
        worker = self.store.actor("worker", request, team)
        self.store.acknowledge(worker, binding, expected_task_version=2, command_id="ack:" + task)

    def prepare(self, key="effect-1", task="t1", team="g1", **overrides):
        args = dict(key=key, tool_id="fixture.write", args_sha256="1" * 64, side_effect="workspace",
                    expected_task_version=self.version("tasks", "task_id", task), command_id="prepare:" + key)
        args.update(overrides)
        return self.store.prepare_operation(self.store.actor("worker", "r1", team), self.store.binding(task), **args)

    def test_sqlite_full_reopen_integrity_and_no_implicit_migration(self):
        self.assertEqual(self.store.connection.execute("PRAGMA synchronous").fetchone()[0], 2)
        self.assertEqual(self.store.connection.execute("PRAGMA journal_mode").fetchone()[0], "wal")
        self.assertEqual(self.store.connection.execute("PRAGMA foreign_key_check").fetchall(), [])
        self.assertEqual(self.store.connection.execute("PRAGMA integrity_check").fetchone()[0], "ok")
        self.store.close()
        self.store = store_ref.ContractStore(self.path)
        self.assertEqual(self.store.connection.execute("PRAGMA foreign_keys").fetchone()[0], 1)
        self.assertEqual(self.store.connection.execute("PRAGMA synchronous").fetchone()[0], 2)
        with self.assertRaises(store_ref.ContractError):
            store_ref.ContractStore(self.path, initialize=True)

    def test_scoped_admission_retries_bind_original_content_after_amend(self):
        root = self.store.actor("root", "r1")
        self.store.amend(root, expected_version=2, text="new request details", command_id="amend")
        args = dict(tenant_id="tenant", project_id="project", root_id="root", key="r1", original_text="preserve input", tool_allowlist=["fixture.write", "fixture.read"])
        self.assertEqual(self.store.admit_request("unused-id", **args), "r1")
        args["original_text"] = "different immutable admission"
        with self.assertRaises(store_ref.Conflict):
            self.store.admit_request("r2", **args)
        args["tenant_id"] = "other-tenant"
        self.assertEqual(self.store.admit_request("r2", **args), "r2")
        self.assertEqual(self.row("requests", "request_id", "r1")["requirements_revision"], 2)

    def test_dispatcher_and_foreign_root_cannot_amend_original_requirements(self):
        for actor in (self.store.actor("dispatcher", "r1"), store_ref.Actor("root", "foreign-root", "r1", 1)):
            with self.assertRaises(store_ref.ContractError):
                self.store.amend(actor, expected_version=2, text="injected amendment", command_id="forbidden-amend")
        self.assertEqual(self.row("requests", "request_id", "r1")["requirements_revision"], 1)

    def test_amend_omission_blocks_and_keep_needs_new_instruction_and_ack(self):
        self.add_team("r1", "t2", "g2")
        old_binding = self.store.binding("t1")
        self.store.amend(self.store.actor("root", "r1"), expected_version=2, text="amended", command_id="amend")
        dispatcher = self.store.actor("dispatcher", "r1")
        self.store.publish_plan(dispatcher, expected_version=3, body={"new": True}, command_id="plan-2")
        self.store.set_amendment_disposition(dispatcher, "t1", disposition="keep", rationale="still relevant to amended requirements", based_on_requirements_revision=2, target_plan_revision=2, expected_task_version=3, command_id="keep-1")
        for task, team in (("t1", "g1"), ("t2", "g2")):
            with self.assertRaises(store_ref.ContractError):
                self.prepare("blocked-" + task, task, team)
        worker = self.store.actor("worker", "r1", "g1")
        with self.assertRaises(store_ref.ContractError):
            self.store.acknowledge(worker, old_binding, expected_task_version=4, command_id="stale-ack")
        with self.assertRaises(store_ref.ContractError):
            self.store.acknowledge(worker, self.store.binding("t1"), expected_task_version=4, command_id="ack-without-new-instruction")
        binding = self.store.issue_instruction(self.store.actor("team_lead", "r1", "g1"), expected_task_version=4, body={"new": "keep confirmation"}, command_id="instruction-2")
        worker = self.store.actor("worker", "r1", "g1")
        self.store.acknowledge(worker, binding, expected_task_version=5, command_id="ack-2")
        self.assertEqual(self.prepare()["status"], "PREPARED")
        with self.assertRaises(store_ref.ContractError):
            self.prepare("omitted", "t2", "g2")
        self.assertEqual(self.row("teams", "team_id", "g2")["paused_for_change"], 1)

    def test_late_disposition_cannot_be_restamped_for_new_amendment(self):
        root = self.store.actor("root", "r1")
        self.store.amend(root, expected_version=2, text="revision two", command_id="amend-2")
        dispatcher = self.store.actor("dispatcher", "r1")
        self.store.publish_plan(dispatcher, expected_version=3, body={"for": 2}, command_id="plan-2")
        frozen_task_version = self.version("tasks", "task_id", "t1")
        self.store.amend(root, expected_version=4, text="revision three", command_id="amend-3")
        self.store.publish_plan(dispatcher, expected_version=5, body={"for": 3}, command_id="plan-3")
        with self.assertRaises(store_ref.ContractError):
            self.store.set_amendment_disposition(dispatcher, "t1", disposition="keep", rationale="decided for revision two",
                based_on_requirements_revision=2, target_plan_revision=2, expected_task_version=frozen_task_version, command_id="late-keep")
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM amend_dispositions").fetchone()[0], 0)
        self.assertEqual(self.version("tasks", "task_id", "t1"), frozen_task_version)
        self.store.set_amendment_disposition(dispatcher, "t1", disposition="keep", rationale="explicitly reviewed revision three",
            based_on_requirements_revision=3, target_plan_revision=3, expected_task_version=frozen_task_version, command_id="current-keep")
        self.assertEqual(self.store.one("SELECT * FROM amend_dispositions")["based_on_requirements_revision"], 3)

    def test_disposition_cannot_reopen_terminal_request(self):
        self.store.amend(self.store.actor("root", "r1"), expected_version=2, text="revision two", command_id="amend-2")
        dispatcher = self.store.actor("dispatcher", "r1")
        self.store.publish_plan(dispatcher, expected_version=3, body={"for": 2}, command_id="plan-2")
        for status in store_ref.TERMINAL_REQUESTS:
            with self.subTest(status=status):
                self.store.connection.execute("UPDATE requests SET status=?,state_version=state_version+1 WHERE request_id='r1'", (status,))
                with self.assertRaises(store_ref.ContractError):
                    self.store.set_amendment_disposition(dispatcher, "t1", disposition="keep", rationale="must not reopen closed request",
                        based_on_requirements_revision=2, target_plan_revision=2, expected_task_version=3, command_id="keep-" + status)
                self.assertEqual(self.row("requests", "request_id", "r1")["status"], status)
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM amend_dispositions").fetchone()[0], 0)
        self.assertEqual(self.version("tasks", "task_id", "t1"), 3)

    def test_instruction_exact_retry_and_same_key_different_body(self):
        lead = self.store.actor("team_lead", "r1", "g1")
        before_epoch = lead.execution_epoch
        args = dict(expected_task_version=3, body={"new": "instructions"}, command_id="instruction-2")
        first = self.store.issue_instruction(lead, **args)
        self.assertEqual(self.store.issue_instruction(lead, **args), first)
        self.assertEqual(first["execution_epoch"], before_epoch)
        args["body"] = {"new": "different instructions under same key"}
        with self.assertRaises(store_ref.Conflict):
            self.store.issue_instruction(lead, **args)
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM instructions").fetchone()[0], 2)
        self.assertEqual(self.row("tasks", "task_id", "t1")["instruction_revision"], 2)

    def test_first_instruction_and_worker_ack_are_both_required(self):
        self.store.create_task(self.store.actor("dispatcher", "r1"), "new-task", "new-team", node_id="new-task", brief={})
        with self.assertRaises(store_ref.ContractError):
            self.prepare("no-instruction", "new-task", "new-team")
        binding = self.store.issue_instruction(self.store.actor("team_lead", "r1", "new-team"),
            expected_task_version=1, body={"first": True}, command_id="first-instruction")
        with self.assertRaises(store_ref.ContractError):
            self.prepare("no-ack", "new-task", "new-team")
        self.store.acknowledge(self.store.actor("worker", "r1", "new-team"), binding, expected_task_version=2, command_id="first-ack")
        self.assertEqual(self.prepare("ready", "new-task", "new-team")["status"], "PREPARED")

    def test_plan_change_fences_prepared_and_running_operations(self):
        running = self.prepare("running")
        prepared = self.prepare("prepared")
        self.store.start_operation(self.store.actor("worker", "r1", "g1"), running["op_id"], expected_version=1)
        self.store.publish_plan(self.store.actor("dispatcher", "r1"), expected_version=2, body={"replan": True}, command_id="plan-2")
        self.assertEqual(self.row("ops", "op_id", running["op_id"])["status"], "UNKNOWN")
        self.assertEqual(self.row("ops", "op_id", prepared["op_id"])["status"], "CANCELLED")
        with self.assertRaises(store_ref.ContractError):
            self.store.start_operation(self.store.actor("worker", "r1", "g1"), prepared["op_id"], expected_version=2)

    def test_ack_control_can_progress_with_unknown_but_business_remains_blocked(self):
        op = self.prepare()
        worker = self.store.actor("worker", "r1", "g1")
        self.store.start_operation(worker, op["op_id"], expected_version=1)
        self.store.cancel_operation(worker, op["op_id"], expected_version=2, command_id="cancel")
        binding = self.store.issue_instruction(self.store.actor("team_lead", "r1", "g1"), expected_task_version=4, body={"new": "reconcile first"}, command_id="instruction-2")
        worker = self.store.actor("worker", "r1", "g1")
        self.store.acknowledge(worker, binding, expected_task_version=5, command_id="ack-2")
        self.assertEqual(self.row("ops", "op_id", op["op_id"])["status"], "UNKNOWN")
        with self.assertRaises(store_ref.ContractError):
            self.prepare("blocked")
        with self.assertRaises(store_ref.ContractError):
            self.store.acknowledge(worker, binding, expected_task_version=6, command_id="fresh-but-no-change")
        self.store.reconcile_operation(op["op_id"], expected_version=3, outcome="FAILED", host_evidence="fixture:verified-no-write")
        self.assertEqual(self.prepare("after-reconcile")["status"], "PREPARED")

    def test_operation_business_idempotency_persists_and_binds_arguments_versions(self):
        op = self.prepare()
        self.assertEqual(self.prepare()["op_id"], op["op_id"])
        self.store.close()
        self.store = store_ref.ContractStore(self.path)
        self.assertEqual(self.prepare()["op_id"], op["op_id"])
        with self.assertRaises(store_ref.Conflict):
            self.prepare(args_sha256="2" * 64)
        self.store.issue_instruction(self.store.actor("team_lead", "r1", "g1"), expected_task_version=4, body={"new": True}, command_id="instruction-2")
        with self.assertRaises(store_ref.Conflict):
            self.prepare()
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM ops").fetchone()[0], 1)

    def test_dispatch_rechecks_live_revocation_policy(self):
        op = self.prepare()
        self.store.register_memory("exp-20261017-k3f9", "a" * 40, "a" * 64)
        self.store.revoke_memory("exp-20261017-k3f9", expected_version=1, host_revoked_tools=["fixture.write"])
        with self.assertRaises(store_ref.ContractError):
            self.store.start_operation(self.store.actor("worker", "r1", "g1"), op["op_id"], expected_version=1)
        with self.assertRaises(store_ref.ContractError):
            self.prepare("new-but-denied")
        self.assertEqual(self.row("ops", "op_id", op["op_id"])["status"], "PREPARED")
        self.assertEqual(self.prepare("read-allowed", tool_id="fixture.read", side_effect="none")["policy_epoch"], 2)

    def test_request_restart_fences_old_actors_and_marks_running_effect_unknown(self):
        op = self.prepare()
        worker = self.store.actor("worker", "r1", "g1")
        self.store.start_operation(worker, op["op_id"], expected_version=1)
        self.store.recover_request("r1", expected_version=2)
        with self.assertRaises(store_ref.ContractError):
            self.store.cancel_operation(worker, op["op_id"], expected_version=3, command_id="late-cancel")
        self.assertEqual(self.row("ops", "op_id", op["op_id"])["status"], "UNKNOWN")
        self.assertEqual(self.row("requests", "request_id", "r1")["dispatcher_epoch"], 2)
        with self.assertRaises(store_ref.ContractError):
            self.prepare("do-not-retry")

    def test_cas_competition_and_failed_transaction_leave_no_partial_state(self):
        other = store_ref.ContractStore(self.path)
        try:
            stale_version = other.one("SELECT state_version FROM requests WHERE request_id='r1'")["state_version"]
            self.store.amend(self.store.actor("root", "r1"), expected_version=stale_version, text="winner", command_id="winner")
            with self.assertRaises(store_ref.Conflict):
                other.amend(other.actor("root", "r1"), expected_version=stale_version, text="loser", command_id="loser")
        finally:
            other.close()
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM request_texts").fetchone()[0], 2)
        self.assertIsNone(self.store.connection.execute("SELECT 1 FROM events WHERE command_id='loser'").fetchone())
        # Force a failure after task INSERT, proving the whole allocation rolls back.
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.create_task(self.store.actor("dispatcher", "r1"), "rolled-back", "g1", node_id="rolled-back", brief={})
        self.assertIsNone(self.store.connection.execute("SELECT 1 FROM tasks WHERE task_id='rolled-back'").fetchone())

    def test_foreign_keys_one_live_team_and_mutable_version_guardrails(self):
        self.fixture("r2", "t2", "g2")
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.connection.execute("INSERT INTO task_deps VALUES ('r1','t1','t2')")
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.connection.execute("UPDATE teams SET task_id='t1',request_id='r1',state_version=state_version+1 WHERE team_id='g2'")
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.connection.execute("UPDATE tasks SET status='FAILED' WHERE task_id='t1'")
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.connection.execute("UPDATE request_texts SET text='rewritten' WHERE request_id='r1'")
        for table in ("requests", "tasks", "teams", "ops", "questions", "change_orders", "llm_calls", "runtime_policy", "receipts", "experience_jobs", "cleanup_jobs", "memory_meta", "memory_feedback"):
            self.assertIn("state_version", {row[1] for row in self.store.connection.execute(f"PRAGMA table_info({table})")})
        self.assertEqual(self.store.connection.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_versions_a_and_b_load_in_same_task_without_inheriting_old_score(self):
        mid = "exp-20261017-k3f9"
        self.store.register_memory(mid, "a" * 40, "a" * 64)
        worker = self.store.actor("worker", "r1", "g1")
        lead = self.store.actor("team_lead", "r1", "g1")
        self.store.load_memory(worker, mid, load_id="load-a")
        # Five independent host provenance groups, all evaluating revision A.
        for i in range(5):
            self.store.record_feedback(lead, "load-a", verdict="worked", host_source_key=f"source:{i}", host_action_evidence=f"fixture:verified-use:{i}")
        self.store.revise_memory(mid, expected_base_revision="a" * 40, expected_version=1, revision="b" * 40, content_sha256="b" * 64)
        self.assertEqual(self.row("memory_current_stats", "memory_id", mid)["successes"], 0)
        self.store.load_memory(worker, mid, load_id="load-b")
        self.store.record_feedback(lead, "load-b", verdict="failed", host_source_key="source:0", host_action_evidence="fixture:verified-failure")
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM memory_loads").fetchone()[0], 2)
        current = self.row("memory_current_stats", "memory_id", mid)
        self.assertEqual((current["successes"], current["failures"]), (0, 1))
        old = self.store.one("SELECT * FROM memory_stats WHERE memory_id=? AND revision=?", (mid, "a" * 40))
        self.assertEqual((old["successes"], old["failures"]), (5, 0))
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM memory_feedback").fetchone()[0], 6)

    def test_revision_cas_conflict_is_atomic_and_original_load_is_immutable(self):
        mid = "exp-20261017-k3f9"
        self.store.register_memory(mid, "a" * 40, "a" * 64)
        self.store.load_memory(self.store.actor("worker", "r1", "g1"), mid, load_id="load-a")
        self.store.revise_memory(mid, expected_base_revision="a" * 40, expected_version=1, revision="b" * 40, content_sha256="b" * 64)
        with self.assertRaises(store_ref.Conflict):
            self.store.revise_memory(mid, expected_base_revision="a" * 40, expected_version=2, revision="c" * 40, content_sha256="c" * 64)
        self.assertEqual(self.row("memory_meta", "memory_id", mid)["revision"], "b" * 40)
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM memory_versions").fetchone()[0], 2)
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.connection.execute("UPDATE memory_loads SET revision=? WHERE load_id='load-a'", ("b" * 40,))

    def test_memory_scope_rejects_cross_project_and_unknown_scope(self):
        self.store.admit_request("other", tenant_id="tenant", project_id="other-project", root_id="root", key="other",
            original_text="other request", tool_allowlist=[], function_id="other-function")
        actor = self.store.actor("dispatcher", "other")
        for index, scope in enumerate(("project:project", "function:reference", "root:unimplemented")):
            mid = f"memory-denied-{index}"
            self.store.register_memory(mid, "a" * 40, "a" * 64, scope=scope)
            with self.assertRaisesRegex(store_ref.ContractError, "scope"):
                self.store.load_memory(actor, mid, load_id="denied-" + str(index))
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM memory_loads").fetchone()[0], 0)
        self.store.register_memory("global-memory", "a" * 40, "a" * 64, scope="global")
        self.store.register_memory("local-memory", "a" * 40, "a" * 64, scope="project:other-project")
        self.store.load_memory(actor, "global-memory", load_id="global-allowed")
        self.store.load_memory(actor, "local-memory", load_id="project-allowed")
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM memory_loads").fetchone()[0], 2)

    def test_usage_binds_actual_load_revision_task_agent_and_instance(self):
        mid = "exp-20261017-k3f9"
        self.store.register_memory(mid, "a" * 40, "a" * 64)
        worker = self.store.actor("worker", "r1", "g1")
        self.store.load_memory(worker, mid, load_id="load-a")
        use = {"memory_id": mid, "revision": "a" * 40, "load_id": "load-a"}
        self.assertTrue(self.store.validate_usage(worker, [use]))
        with self.assertRaises(store_ref.ContractError):
            self.store.validate_usage(worker, [dict(use, revision="b" * 40)])
        with self.assertRaises(store_ref.ContractError):
            self.store.validate_usage(self.store.actor("team_lead", "r1", "g1"), [use])
        self.store.recover_request("r1", expected_version=2)
        with self.assertRaises(store_ref.ContractError):
            self.store.validate_usage(self.store.actor("worker", "r1", "g1"), [use])

    def test_card_exposure_missing_evidence_and_duplicate_sources_cannot_inflate(self):
        mid = "exp-20261017-k3f9"
        self.store.register_memory(mid, "a" * 40, "a" * 64)
        worker = self.store.actor("worker", "r1", "g1")
        lead = self.store.actor("team_lead", "r1", "g1")
        self.store.load_memory(worker, mid, load_id="card", how="card")
        self.store.load_memory(worker, mid, load_id="load")
        self.store.record_feedback(lead, "card", verdict="worked", host_source_key="same-origin", host_action_evidence="model says worked")
        self.store.record_feedback(lead, "load", verdict="worked", host_source_key="no-evidence", host_action_evidence="")
        self.assertEqual(self.row("memory_current_stats", "memory_id", mid)["successes"], 0)
        for _ in range(5):
            self.store.record_feedback(lead, "load", verdict="worked", host_source_key="same-origin", host_action_evidence="fixture:actual-use")
        self.assertEqual(self.row("memory_current_stats", "memory_id", mid)["successes"], 1)
        self.store.record_feedback(lead, "load", verdict="failed", host_source_key="same-origin", host_action_evidence="fixture:actual-failure")
        self.store.record_feedback(lead, "load", verdict="worked", host_source_key="same-origin", host_action_evidence="fixture:late-positive")
        score = self.row("memory_current_stats", "memory_id", mid)
        self.assertEqual((score["successes"], score["failures"], score["assessments"]), (0, 1, 1))

    def settle_fixture(self):
        # Test host fixture only: no claim of production acceptance/resource cleanup.
        with self.store.transaction():
            self.store.connection.execute("UPDATE tasks SET status='ACCEPTED',state_version=state_version+1 WHERE request_id='r1'")
            self.store.connection.execute("UPDATE teams SET status='STOPPED',state_version=state_version+1 WHERE request_id='r1'")
            self.store.connection.execute("UPDATE requests SET status='DELIVERED',delivery_status='CONFIRMED',state_version=state_version+1 WHERE request_id='r1'")

    def test_multiteam_experience_cleanup_survives_reopen_and_request_deletion(self):
        self.add_team("r1", "t2", "g2")
        mid = "exp-20261017-k3f9"
        self.store.register_memory(mid, "a" * 40, "a" * 64)
        self.store.load_memory(self.store.actor("worker", "r1", "g1"), mid, load_id="retained-proof")
        self.store.record_feedback(self.store.actor("team_lead", "r1", "g1"), "retained-proof", verdict="worked", host_source_key="one-origin", host_action_evidence="fixture:actual-use")
        self.settle_fixture()
        jobs = [self.store.queue_experience("r1", task, team, host_source_snapshot={"load_ids": ["retained-proof"] if task == "t1" else []})
                for task, team in (("t1", "g1"), ("t2", "g2"))]
        self.store.complete_experience(jobs[0], expected_version=1, host_report_ref="fixture:experience/t1")
        cleanup_args = dict(host_manifest_snapshot={"fixture_artifacts": ["digest-only"]}, host_delivery_receipt_ref="fixture:root-receipt")
        with self.assertRaisesRegex(store_ref.ContractError, "every team's"):
            self.store.enqueue_cleanup("r1", **cleanup_args)
        self.store.complete_experience(jobs[1], expected_version=1, host_report_ref="fixture:no-new-experience/t2")
        cleanup = self.store.enqueue_cleanup("r1", **cleanup_args)
        first = self.store.claim_cleanup(cleanup, expected_version=1, host_worker_id="worker-a")
        # Simulate loss of volatile state; a RUNNING durable job is recoverable.
        self.store.close()
        self.store = store_ref.ContractStore(self.path)
        second = self.store.claim_cleanup(cleanup, expected_version=first["state_version"], host_worker_id="worker-b")
        with self.assertRaises(store_ref.Conflict):
            self.store.finish_reference_cleanup(cleanup, expected_version=second["state_version"], lease_epoch=first["lease_epoch"], host_worker_id="worker-a")
        self.store.finish_reference_cleanup(cleanup, expected_version=second["state_version"], lease_epoch=second["lease_epoch"], host_worker_id="worker-b")
        self.assertIsNone(self.store.connection.execute("SELECT 1 FROM requests WHERE request_id='r1'").fetchone())
        self.assertEqual(self.row("cleanup_jobs", "cleanup_id", cleanup)["status"], "DONE")
        self.assertEqual(self.row("receipts", "request_id", "r1")["cleanup_status"], "DONE")
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM experience_jobs").fetchone()[0], 2)
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM memory_loads").fetchone()[0], 0)
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM memory_feedback").fetchone()[0], 1)
        self.assertEqual(self.store.connection.execute("PRAGMA foreign_key_check").fetchall(), [])
        self.assertEqual(self.store.admit_request("new-id", tenant_id="tenant", project_id="project", root_id="root", key="r1",
            original_text="preserve input", tool_allowlist=["fixture.write", "fixture.read"]), "r1")

    def test_amended_accepted_task_requires_new_version_experience_report(self):
        with self.store.transaction():
            self.store.connection.execute("UPDATE tasks SET status='ACCEPTED',accepted_with_issues=1,state_version=state_version+1 WHERE task_id='t1'")
        old_job = self.store.queue_experience("r1", "t1", "g1", host_source_snapshot={"version": 1})
        self.store.complete_experience(old_job, expected_version=1, host_report_ref="fixture:old-report")
        self.store.amend(self.store.actor("root", "r1"), expected_version=2, text="accepted task must be reconsidered", command_id="amend-2")
        dispatcher = self.store.actor("dispatcher", "r1")
        self.store.publish_plan(dispatcher, expected_version=3, body={"for": 2}, command_id="plan-2")
        self.store.set_amendment_disposition(dispatcher, "t1", disposition="update", rationale="explicitly reopen accepted work",
            based_on_requirements_revision=2, target_plan_revision=2, expected_task_version=4, command_id="reopen")
        self.assertEqual(self.row("tasks", "task_id", "t1")["accepted_with_issues"], 0)
        binding = self.store.issue_instruction(self.store.actor("team_lead", "r1", "g1"), expected_task_version=5, body={"version": 2}, command_id="instruction-2")
        self.store.acknowledge(self.store.actor("worker", "r1", "g1"), binding, expected_task_version=6, command_id="ack-2")
        self.settle_fixture()
        with self.assertRaisesRegex(store_ref.ContractError, "every team's"):
            self.store.enqueue_cleanup("r1", host_manifest_snapshot={}, host_delivery_receipt_ref="fixture:current-receipt")
        new_job = self.store.queue_experience("r1", "t1", "g1", host_source_snapshot={"version": 2})
        self.assertNotEqual(new_job, old_job)
        self.store.complete_experience(new_job, expected_version=1, host_report_ref="fixture:current-report")
        self.store.enqueue_cleanup("r1", host_manifest_snapshot={}, host_delivery_receipt_ref="fixture:current-receipt")
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM experience_jobs").fetchone()[0], 2)

    def test_cleanup_rejects_unknown_and_missing_receipt(self):
        op = self.prepare()
        self.store.start_operation(self.store.actor("worker", "r1", "g1"), op["op_id"], expected_version=1)
        self.store.cancel_operation(self.store.actor("worker", "r1", "g1"), op["op_id"], expected_version=2, command_id="cancel")
        self.settle_fixture()
        with self.assertRaisesRegex(store_ref.ContractError, "confirmed host receipt"):
            self.store.enqueue_cleanup("r1", host_manifest_snapshot={})
        with self.assertRaisesRegex(store_ref.ContractError, "unsettled"):
            self.store.enqueue_cleanup("r1", host_manifest_snapshot={}, host_delivery_receipt_ref="fixture:receipt")
        self.assertEqual(self.store.connection.execute("SELECT count(*) FROM cleanup_jobs").fetchone()[0], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
