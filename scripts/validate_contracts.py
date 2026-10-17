#!/usr/bin/env python3
"""Offline shape and selected host-authority semantics for v0.4.1 contracts.

This reference never executes command strings, tools, models or remote $refs.
A passing schema is not semantic requirement coverage or product acceptance.
"""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
from urllib.parse import urldefrag

from jsonschema import Draft7Validator, RefResolver
from jsonschema.exceptions import RefResolutionError

ROOT = Path(__file__).resolve().parents[1]
SCHEMA_DIR = ROOT / "schemas"
EXECUTION_FIELDS = (
    "request_id", "task_id", "team_id", "requirements_revision", "plan_revision",
    "task_revision", "instruction_revision", "dispatcher_epoch", "execution_epoch", "policy_epoch",
)
REVIEW_SOURCE_FIELDS = (
    "request_id", "task_id", "requirements_revision", "plan_revision", "task_revision",
    "instruction_revision", "dispatcher_epoch", "submission_id", "candidate_ref", "candidate_sha256",
)
REVIEW_FIELDS = (*REVIEW_SOURCE_FIELDS, "review_id", "round", *("reviewer_" + k for k in EXECUTION_FIELDS))


class SemanticError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise SemanticError(message)


class OfflineResolver(RefResolver):
    def resolve_remote(self, uri):
        # RefResolver's default fallback would use requests/urlopen. Never do so.
        raise RefResolutionError("unregistered reference refused offline: " + uri)


def project_tool_schema(schema):
    """Remove host fields recursively, including their required declarations.

    x-runtime-field is a protocol annotation, not a JSON Schema keyword. The
    projection must also apply to referenced schemas in the local registry.
    """
    if isinstance(schema, list):
        return [project_tool_schema(item) for item in schema]
    if not isinstance(schema, dict):
        return copy.deepcopy(schema)
    properties = schema.get("properties", {})
    removed = {key for key, value in properties.items() if value.get("x-runtime-field")}
    result = {}
    for key, value in schema.items():
        if key == "properties":
            result[key] = {name: project_tool_schema(prop) for name, prop in value.items() if name not in removed}
        elif key == "required":
            # allOf/then can require a removed runtime property declared higher up.
            result[key] = [name for name in value if name not in removed]
            if not result[key]:
                del result[key]
        else:
            result[key] = project_tool_schema(value)
    return result


def _strip_runtime_conditions(schema, runtime_names):
    """A projected input cannot be conditioned on a removed trusted property.

    Full-record validation still enforces the original role-specific condition
    after the host fills metadata from the frozen call context.
    """
    if isinstance(schema, list):
        return [_strip_runtime_conditions(item, runtime_names) for item in schema]
    if not isinstance(schema, dict):
        return schema
    result = {}
    for key, value in schema.items():
        if key == "required":
            value = [name for name in value if name not in runtime_names]
            if not value:
                continue
        result[key] = _strip_runtime_conditions(value, runtime_names)
    if "if" in result and set(schema["if"].get("properties", {})) & runtime_names:
        result.pop("if", None)
        result.pop("then", None)
        result.pop("else", None)
    return result


class Registry:
    def __init__(self, directory=SCHEMA_DIR):
        self.schemas = {}
        self.store = {}
        for path in sorted(Path(directory).glob("*.schema.json")):
            schema = json.loads(path.read_text(encoding="utf-8"))
            Draft7Validator.check_schema(schema)
            self.schemas[path.name.removesuffix(".schema.json")] = schema
            self.store[schema["$id"]] = schema
        # Check all references eagerly: even an unvisited branch must be local.
        def check_refs(value, base):
            if isinstance(value, dict):
                if "$ref" in value:
                    from urllib.parse import urljoin
                    uri, _ = urldefrag(urljoin(base, value["$ref"]))
                    require(uri in self.store, "unknown/offline $ref: " + uri)
                for child in value.values():
                    check_refs(child, base)
            elif isinstance(value, list):
                for child in value:
                    check_refs(child, base)
        for schema in self.schemas.values():
            check_refs(schema, schema["$id"])

    def validator(self, name, *, model_input=False):
        schema = self.schemas[name]
        store = self.store
        if model_input:
            def projected(item):
                runtime_names = set()
                def collect(value):
                    if isinstance(value, dict):
                        for key, prop in value.get("properties", {}).items():
                            if prop.get("x-runtime-field"):
                                runtime_names.add(key)
                        for child in value.values():
                            collect(child)
                    elif isinstance(value, list):
                        for child in value:
                            collect(child)
                collect(item)
                return _strip_runtime_conditions(project_tool_schema(item), runtime_names)
            store = {key: projected(item) for key, item in self.store.items()}
            schema = store[schema["$id"]]
        resolver = OfflineResolver.from_schema(schema, store=store)
        return Draft7Validator(schema, resolver=resolver)

    def validate(self, name, document, *, model_input=False):
        self.validator(name, model_input=model_input).validate(document)
        if name == "plan":
            # The protocol deliberately embeds unconstrained objects for dynamic
            # capability inputs. Team-task briefs have a known local contract.
            for node in document["nodes"]:
                if node["kind"] == "team_task":
                    self.validate("task_brief", node["brief"], model_input=model_input)
        return True


def validate_request_snapshot(snapshot):
    revisions = [item["requirements_revision"] for item in snapshot["texts"]]
    require(revisions == list(range(1, snapshot["requirements_revision"] + 1)), "request text revisions must be contiguous and ordered")
    require(snapshot["texts"][0]["kind"] == "ORIGINAL" and all(item["kind"] == "AMEND" for item in snapshot["texts"][1:]), "original/amend kind mismatch")
    texts = {item["requirements_revision"]: item["text"] for item in snapshot["texts"]}
    seen = set()
    for item in snapshot["constraint_catalog"]:
        require(item["constraint_id"] not in seen, "duplicate authoritative constraint id")
        seen.add(item["constraint_id"])
        require(item["source_revision"] in texts and item["text"].strip() in texts[item["source_revision"]], "catalog excerpt must occur in its original source revision")
    return True


def validate_constraints(message, snapshot, *, assigned_constraint_refs):
    """assigned_constraint_refs comes from host policy, never from the message."""
    validate_request_snapshot(snapshot)
    catalog = {item["constraint_id"]: item for item in snapshot["constraint_catalog"]}
    expected = set(assigned_constraint_refs)
    require(len(expected) == len(assigned_constraint_refs), "host assignment contains duplicate ids")
    require(expected <= set(catalog), "host assignment references an unknown constraint")
    require(set(message["constraint_refs"]) == expected and len(message["constraint_refs"]) == len(expected), "omitted, added or duplicate assigned constraint")
    excerpts = [item.strip() for item in message["constraints_verbatim"]]
    required_texts = {catalog[key]["text"].strip() for key in expected}
    require(required_texts <= set(excerpts), "required constraint text lost in handoff")
    texts = [item["text"] for item in snapshot["texts"]]
    require(all(excerpt and any(excerpt in text for text in texts) for excerpt in excerpts), "invented/non-verbatim constraint")
    return True


def validate_work_order(order, brief, snapshot, *, assigned_constraint_refs, frozen_binding):
    validate_constraints(brief, snapshot, assigned_constraint_refs=assigned_constraint_refs)
    validate_constraints(order, snapshot, assigned_constraint_refs=assigned_constraint_refs)
    require(all(order.get(k) == frozen_binding[k] for k in EXECUTION_FIELDS), "work order does not match its frozen host instruction binding")
    require(set(brief["constraints_verbatim"]) <= set(order["constraints_verbatim"]), "work order dropped a brief constraint")
    return True


def validate_change_order(change, parent, snapshot, *, assigned_constraint_refs, frozen_target):
    validate_constraints(change, snapshot, assigned_constraint_refs=assigned_constraint_refs)
    require(change["from_role"] == "team_lead", "relay guard expects owning TeamLead context")
    require(parent["from_role"] in ("dispatcher", "runtime"), "relay parent lacks change authority")
    require(change["parent_change_id"] == parent["change_id"], "wrong parent change")
    for field in ("request_id", "task_id", "based_on_requirements_revision", "target_plan_revision", "target_task_revision", "dispatcher_epoch"):
        require(change[field] == parent[field] == frozen_target[field], "change target binding mismatch: " + field)
    for field in ("target_instruction_revision", "execution_epoch"):
        require(change[field] == frozen_target[field], "stale TeamLead instruction binding: " + field)
    require(change["kind"] == parent["kind"], "relay changed change kind")
    require(change.get("acceptance_changes", []) == parent.get("acceptance_changes", []), "TeamLead cannot redefine or weaken parent acceptance criteria")
    parent_findings = {finding["id"]: finding for finding in parent.get("findings", [])}
    findings = {finding["id"]: finding for finding in change.get("findings", [])}
    require(len(findings) == len(change.get("findings", [])) and len(parent_findings) == len(parent.get("findings", [])), "duplicate finding id")
    require(all(findings.get(key) == value for key, value in parent_findings.items()), "relay lost or weakened an authoritative finding")
    require(set(parent["constraints_verbatim"]) <= set(change["constraints_verbatim"]), "relay lost an upstream constraint")
    return True


def validate_submission(submission, *, frozen_call_binding, current_binding, authoritative_criteria):
    for field in EXECUTION_FIELDS:
        require(submission[field] == frozen_call_binding[field] == current_binding[field], "stale/retagged submission binding: " + field)
    ids = [item["criterion_id"] for item in submission["self_check"]]
    require(len(ids) == len(set(ids)) and set(ids) == set(authoritative_criteria), "self-check must cover the current authoritative criteria exactly")
    # Load/version/instance provenance is checked by ContractStore.validate_usage.
    return True


def validate_review_response(verdict, *, frozen_ticket, current_source, current_reviewer):
    """Validate report provenance; changes_requested remains a valid report."""
    require(all(verdict[field] == frozen_ticket[field] for field in REVIEW_FIELDS), "review response does not match the frozen call ticket")
    require(all(frozen_ticket[field] == current_source[field] for field in REVIEW_SOURCE_FIELDS), "candidate or producing-task version has changed")
    require(all(frozen_ticket["reviewer_" + field] == current_reviewer[field] for field in EXECUTION_FIELDS), "reviewer identity/version/lease changed during the call")
    ids = [item["id"] for item in verdict["findings"]]
    require(len(ids) == len(set(ids)), "duplicate finding id")
    require(not (verdict["verdict"] == "pass" and any(item["severity"] in ("blocker", "major") for item in verdict["findings"])), "pass contradicts blocker/major findings")
    require(not (verdict["verdict"] == "pass" and any(item["result"] == "fail" for item in verdict.get("evidence", []))), "pass contradicts failed evidence")
    return True


def require_product_acceptance(verdict, *, frozen_ticket, current_source, current_reviewer,
                               required_checks, host_verifications, host_user_waiver=None):
    """This function consumes host-only persisted evidence, not manifest claims.

    A user waiver can waive named review findings, but never missing/failed
    deterministic checks. External user authentication remains an adapter duty.
    """
    validate_review_response(verdict, frozen_ticket=frozen_ticket, current_source=current_source, current_reviewer=current_reviewer)
    require(verdict["verdict"] != "inconclusive", "inconclusive review cannot establish acceptance")
    if verdict["verdict"] != "pass":
        require(bool(host_user_waiver), "review requests changes; current user waiver required")
        require(host_user_waiver.get("actor_role") == "user" and bool(host_user_waiver.get("evidence_ref")), "waiver must come from authenticated host user context")
        for field in REVIEW_FIELDS:
            require(host_user_waiver.get(field) == frozen_ticket[field], "waiver is not bound to the exact reviewed candidate/versions")
        require(set(host_user_waiver.get("finding_ids", [])) == {item["id"] for item in verdict["findings"]}, "user waiver must name every unresolved finding")
    checks = {}
    for check in host_verifications:
        require(check["criterion_id"] not in checks, "duplicate deterministic evidence")
        checks[check["criterion_id"]] = check
        require(all(check.get(field) == frozen_ticket[field] for field in REVIEW_FIELDS), "deterministic evidence belongs to a different ticket/candidate/version")
        require(bool(check.get("runner_id")) and bool(check.get("evidence_ref")), "deterministic evidence lacks host runner/receipt")
    require(set(checks) == set(required_checks) and len(set(required_checks)) == len(required_checks), "missing, duplicate or unexpected deterministic check")
    require(all(check["result"] == "pass" for check in checks.values()), "deterministic acceptance check failed or was not run")
    return True


def validate_examples():
    registry = Registry()
    examples = {}
    for path in sorted((SCHEMA_DIR / "examples").glob("*.json")):
        name = path.name.split(".")[0]
        schema_name = "request_snapshot" if name == "request_texts" else name
        document = json.loads(path.read_text(encoding="utf-8"))
        registry.validate(schema_name, document)
        examples[path.name] = document
    snapshot = examples["request_texts.json"]
    validate_request_snapshot(snapshot)
    brief = examples["task_brief.json"]
    order = examples["work_order.json"]
    assigned = ["c1", "c2", "c3"]  # Frozen host fixture assignment, not copied from the brief.
    validate_work_order(order, brief, snapshot, assigned_constraint_refs=assigned,
                        frozen_binding={key: order[key] for key in EXECUTION_FIELDS})
    change = examples["change_order.team_lead.json"]
    validate_change_order(change, examples["change_order.dispatcher.json"], snapshot,
                          assigned_constraint_refs=["c1", "c2", "c3", "c4", "c5"], frozen_target=change)
    verdict = examples["review_verdict.json"]
    ticket = examples["review_request.json"]
    validate_review_response(verdict, frozen_ticket=ticket,
                             current_source={key: ticket[key] for key in REVIEW_SOURCE_FIELDS},
                             current_reviewer={key: ticket["reviewer_" + key] for key in EXECUTION_FIELDS})
    return len(registry.schemas), len(examples)


if __name__ == "__main__":
    try:
        schemas, examples = validate_examples()
        print(f"contracts: {schemas} local schemas and {examples} complete examples passed; selected host semantic checks passed")
        print("No model, tool command, remote reference, or product acceptance was executed.")
    except Exception as exc:
        print(f"contracts: FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
