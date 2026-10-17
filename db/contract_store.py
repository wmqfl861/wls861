"""Executable v0.4.1 transaction reference, NOT the C++ Store/MemoryEngine.

Only local SQLite state is exercised. Actor and host_* arguments are trusted
host inputs; this module supplies neither authentication nor a tool/model/Git
executor. Do not expose it as an agent-facing service. See db/README.md.
"""
from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sqlite3
import uuid


class ContractError(ValueError):
    """A semantic, identity, state or policy precondition failed."""


class Conflict(ContractError):
    """A persisted intent, version or lease no longer matches."""


BINDING_FIELDS = (
    "request_id", "task_id", "team_id", "requirements_revision", "plan_revision",
    "task_revision", "instruction_revision", "dispatcher_epoch", "execution_epoch",
    "policy_epoch",
)
TERMINAL_REQUESTS = ("DELIVERED", "FAILED", "CANCELLED", "SUPERSEDED")
TERMINAL_TASKS = ("ACCEPTED", "FAILED", "CANCELLED", "SKIPPED", "SUPERSEDED")
UNSETTLED_OPS = ("PREPARED", "RUNNING", "CANCEL_REQUESTED", "UNKNOWN")


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


def digest(value):
    return hashlib.sha256(canonical(value).encode("utf-8")).hexdigest()


def now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def new_id(prefix):
    # Internal SQLite fixture IDs, not the ULID wire-format ID generator.
    return prefix + "_" + uuid.uuid4().hex


@dataclass(frozen=True)
class Actor:
    role: str
    principal_id: str
    request_id: str
    dispatcher_epoch: int
    task_id: str | None = None
    team_id: str | None = None
    execution_epoch: int | None = None

    @property
    def instance_id(self):
        return f"{self.principal_id}:{self.dispatcher_epoch}:{self.execution_epoch or 0}"


class ContractStore:
    def __init__(self, path, *, initialize=False):
        self.connection = sqlite3.connect(path, isolation_level=None, timeout=5)
        self.connection.row_factory = sqlite3.Row
        try:
            self.connection.execute("PRAGMA foreign_keys=ON")
            self.connection.execute("PRAGMA busy_timeout=5000")
            self.connection.execute("PRAGMA journal_mode=WAL")
            self.connection.execute("PRAGMA synchronous=FULL")
            if initialize:
                # Never interpret an existing v0.4 database as a fresh schema.
                if self.connection.execute("SELECT 1 FROM sqlite_master LIMIT 1").fetchone():
                    raise ContractError("initialization requires an empty database")
                self.connection.executescript(Path(__file__).with_name("schema.sql").read_text())
            row = self.connection.execute("SELECT schema_version FROM contract_metadata WHERE singleton=1").fetchone()
            if row is None or row[0] != "0.4.1":
                raise ContractError("unsupported schema version; explicit migration required")
        except Exception:
            self.connection.close()
            raise

    def close(self):
        self.connection.close()

    @contextmanager
    def transaction(self):
        self.connection.execute("BEGIN IMMEDIATE")
        try:
            yield
            self.connection.execute("COMMIT")
        except BaseException:
            self.connection.execute("ROLLBACK")
            raise

    def one(self, sql, args=()):
        row = self.connection.execute(sql, args).fetchone()
        if row is None:
            raise ContractError("missing authoritative record")
        return dict(row)

    def _cas(self, sql, args):
        if self.connection.execute(sql, args).rowcount != 1:
            raise Conflict("state_version compare-and-swap failed")

    def _replay(self, request_id, command_id, payload):
        row = self.connection.execute(
            "SELECT command_sha256,result FROM events WHERE request_id=? AND command_id=?",
            (request_id, command_id),
        ).fetchone()
        if row is None:
            return None
        if row["command_sha256"] != digest(payload):
            raise Conflict("command_id reused with different immutable intent")
        return json.loads(row["result"])

    def _event(self, request_id, command_id, payload, result, actor="runtime"):
        self.connection.execute(
            "INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) "
            "VALUES (?,?,?,?,?,?,?,?,?)",
            (new_id("evt"), request_id, payload["type"], actor, command_id, digest(payload),
             canonical(payload), canonical(result), now()),
        )

    def _actor(self, actor, roles):
        if actor.role not in roles:
            raise ContractError("role is not authorized")
        request = self.one("SELECT * FROM requests WHERE request_id=?", (actor.request_id,))
        if request["dispatcher_epoch"] != actor.dispatcher_epoch:
            raise Conflict("stale dispatcher epoch")
        if actor.role == "root":
            if actor.principal_id != request["root_id"] or actor.task_id or actor.team_id:
                raise ContractError("Root ownership mismatch")
        elif actor.role == "dispatcher":
            if actor.principal_id != request["dispatcher_id"] or actor.task_id or actor.team_id:
                raise ContractError("dispatcher ownership mismatch")
        elif actor.role in ("team_lead", "worker"):
            team = self.one("SELECT * FROM teams WHERE team_id=?", (actor.team_id,))
            if (team["request_id"] != actor.request_id or team["task_id"] != actor.task_id
                    or team["execution_epoch"] != actor.execution_epoch
                    or actor.principal_id != team["lead_id" if actor.role == "team_lead" else "worker_id"]
                    or team["status"] not in ("STARTING", "ACTIVE", "DRAINING")):
                raise Conflict("team identity or execution epoch mismatch")
        return request

    def actor(self, role, request_id, team_id=None):
        """HOST-ONLY fixture context issuance; this is not authentication."""
        request = self.one("SELECT * FROM requests WHERE request_id=?", (request_id,))
        if role == "root":
            return Actor(role, request["root_id"], request_id, request["dispatcher_epoch"])
        if role == "dispatcher":
            return Actor(role, request["dispatcher_id"], request_id, request["dispatcher_epoch"])
        if role not in ("worker", "team_lead"):
            raise ContractError("unsupported reference actor")
        team = self.one("SELECT * FROM teams WHERE request_id=? AND team_id=?", (request_id, team_id))
        return Actor(role, team["worker_id" if role == "worker" else "lead_id"], request_id,
                     request["dispatcher_epoch"], team["task_id"], team_id, team["execution_epoch"])

    def binding(self, task_id):
        return self.one(
            "SELECT r.request_id,t.task_id,g.team_id,r.requirements_revision,t.plan_revision,t.task_revision,"
            "t.instruction_revision,r.dispatcher_epoch,g.execution_epoch,p.policy_epoch "
            "FROM tasks t JOIN requests r ON r.request_id=t.request_id "
            "JOIN teams g ON g.task_id=t.task_id AND g.status IN ('STARTING','ACTIVE','DRAINING') "
            "JOIN runtime_policy p ON p.singleton=1 WHERE t.task_id=?", (task_id,))

    def _binding_matches(self, binding, *, policy=True):
        if set(binding) != set(BINDING_FIELDS):
            raise ContractError("complete frozen execution binding required")
        current = self.binding(binding["task_id"])
        for field in BINDING_FIELDS:
            if (policy or field != "policy_epoch") and binding.get(field) != current[field]:
                raise Conflict("stale frozen binding: " + field)
        return current

    def _ready(self, actor, binding, tool_id):
        request = self._actor(actor, ("worker",))
        if (actor.task_id, actor.team_id) != (binding["task_id"], binding["team_id"]):
            raise ContractError("operation belongs to another task or team")
        self._binding_matches(binding)
        task = self.one("SELECT * FROM tasks WHERE task_id=?", (actor.task_id,))
        team = self.one("SELECT * FROM teams WHERE team_id=?", (actor.team_id,))
        if (request["status"] not in ("RUNNING", "VERIFYING") or team["status"] != "ACTIVE"
                or task["status"] not in ("READY", "RUNNING") or team["paused_for_change"]
                or task["based_on_requirements_revision"] != request["requirements_revision"]
                or task["plan_revision"] != request["plan_revision"]
                or task["applied_task_revision"] != task["task_revision"]
                or task["applied_plan_revision"] != task["plan_revision"]
                or task["instruction_revision"] < 1
                or task["applied_instruction_revision"] != task["instruction_revision"]):
            raise ContractError("business execution is not ready")
        if self.connection.execute("SELECT 1 FROM ops WHERE team_id=? AND status='UNKNOWN'", (actor.team_id,)).fetchone():
            raise ContractError("unknown outcome requires host reconciliation")
        policy = self.one("SELECT * FROM runtime_policy WHERE singleton=1")
        if (tool_id not in json.loads(request["config_snapshot"]).get("tool_allowlist", [])
                or tool_id in json.loads(policy["denied_tools"])):
            raise ContractError("tool grant revoked or absent")

    def admit_request(self, request_id, *, tenant_id, project_id, root_id, key,
                      original_text, tool_allowlist, function_id="reference", command_id="admit"):
        """Real scoped idempotent admission; all arguments come from the host."""
        content = {"original_text": original_text, "function_id": function_id,
                   "tool_allowlist": sorted(tool_allowlist)}
        identity = (tenant_id, project_id, root_id, key)
        with self.transaction():
            for table in ("requests", "receipts"):
                old = self.connection.execute(
                    f"SELECT request_id,content_sha256 FROM {table} WHERE tenant_id=? AND project_id=? AND root_id=? AND idempotency_key=?", identity).fetchone()
                if old:
                    if old["content_sha256"] != digest(content):
                        raise Conflict("admission key reused with different original input")
                    return old["request_id"]
            stamp = now()
            self.connection.execute(
                "INSERT INTO requests(request_id,tenant_id,project_id,root_id,function_id,origin,idempotency_key,content_sha256,"
                "dispatcher_id,status,config_snapshot,budget,created_at,updated_at) VALUES (?,?,?,?,?,'cli',?,?,?,'RUNNING',?,'{}',?,?)",
                (request_id, tenant_id, project_id, root_id, function_id, key, digest(content), new_id("dispatcher"),
                 canonical({"tool_allowlist": sorted(tool_allowlist)}), stamp, stamp))
            self.connection.execute("INSERT INTO request_texts VALUES (?,1,'ORIGINAL',?,?)", (request_id, original_text, stamp))
            self._event(request_id, command_id, {"type": "admit", **content}, {"request_id": request_id}, "root")
            return request_id

    def publish_plan(self, actor, *, expected_version, body, command_id):
        payload = {"type": "plan", "body": body, "expected_version": expected_version}
        with self.transaction():
            request = self._actor(actor, ("dispatcher",))
            old = self._replay(actor.request_id, command_id, payload)
            if old is not None:
                return old["plan_revision"]
            if request["status"] in TERMINAL_REQUESTS:
                raise ContractError("terminal request")
            revision = request["plan_revision"] + 1
            self._cas("UPDATE requests SET plan_revision=?,state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?",
                      (revision, now(), actor.request_id, expected_version))
            self.connection.execute("INSERT INTO plans VALUES (?,?,?,?,?,?)", (
                actor.request_id, revision, request["requirements_revision"], canonical(body), None, now()))
            # A new request-level plan is a barrier until explicit task/instruction rebinding.
            self.connection.execute("UPDATE teams SET paused_for_change=1,state_version=state_version+1,updated_at=? WHERE request_id=? AND status='ACTIVE'",
                                    (now(), actor.request_id))
            self._fence_ops(actor.request_id)
            self._event(actor.request_id, command_id, payload, {"plan_revision": revision}, "dispatcher")
            return revision

    def create_task(self, actor, task_id, team_id, *, node_id, brief):
        """Host fixture allocation only; TeamLead must issue the first instruction."""
        with self.transaction():
            request = self._actor(actor, ("dispatcher",))
            if request["plan_revision"] < 1 or request["status"] in TERMINAL_REQUESTS:
                raise ContractError("current live plan required")
            stamp = now()
            self.connection.execute(
                "INSERT INTO tasks(task_id,request_id,node_id,plan_revision,based_on_requirements_revision,kind,status,brief,instruction_revision,created_at,updated_at) "
                "VALUES (?,?,?,?,?,'team_task','READY',?,0,?,?)",
                (task_id, actor.request_id, node_id, request["plan_revision"], request["requirements_revision"], canonical(brief), stamp, stamp))
            self.connection.execute(
                "INSERT INTO teams(team_id,request_id,task_id,lead_id,worker_id,template_id,status,paused_for_change,lead_config,worker_config,workspace_path,created_at,updated_at) "
                "VALUES (?,?,?,?,?,'reference','ACTIVE',1,'{}','{}','reference/no-files',?,?)",
                (team_id, actor.request_id, task_id, new_id("lead"), new_id("worker"), stamp, stamp))
            self._event(actor.request_id, "create:" + task_id, {"type": "create_task", "task_id": task_id}, {"team_id": team_id}, "dispatcher")
        return self.binding(task_id)

    def amend(self, actor, *, expected_version, text, command_id):
        payload = {"type": "amend", "text": text, "expected_version": expected_version}
        with self.transaction():
            request = self._actor(actor, ("root",))
            replay = self._replay(actor.request_id, command_id, payload)
            if replay is not None:
                return replay["requirements_revision"]
            if request["status"] in TERMINAL_REQUESTS:
                raise ContractError("terminal request")
            revision = request["requirements_revision"] + 1
            self._cas("UPDATE requests SET requirements_revision=?,state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?",
                      (revision, now(), actor.request_id, expected_version))
            self.connection.execute("INSERT INTO request_texts VALUES (?,?,'AMEND',?,?)", (actor.request_id, revision, text, now()))
            self.connection.execute("UPDATE teams SET paused_for_change=1,execution_epoch=execution_epoch+1,state_version=state_version+1,updated_at=? "
                                    "WHERE request_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", (now(), actor.request_id))
            self._fence_ops(actor.request_id)
            self._event(actor.request_id, command_id, payload, {"requirements_revision": revision}, "root")
            return revision

    def _fence_ops(self, request_id, team_id=None):
        where = "request_id=?" + (" AND team_id=?" if team_id else "")
        args = (request_id, team_id) if team_id else (request_id,)
        self.connection.execute(f"UPDATE ops SET status=CASE WHEN status='PREPARED' THEN 'CANCELLED' ELSE 'UNKNOWN' END,"
                                f"state_version=state_version+1 WHERE {where} AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED')", args)

    def set_amendment_disposition(self, actor, task_id, *, disposition, rationale,
                                  based_on_requirements_revision, target_plan_revision, expected_task_version, command_id):
        payload = {"type": "disposition", "task_id": task_id, "disposition": disposition,
                   "rationale": rationale, "based_on_requirements_revision": based_on_requirements_revision,
                   "target_plan_revision": target_plan_revision, "expected_task_version": expected_task_version}
        with self.transaction():
            request = self._actor(actor, ("dispatcher",))
            replay = self._replay(actor.request_id, command_id, payload)
            if replay is not None:
                return replay
            task = self.one("SELECT * FROM tasks WHERE request_id=? AND task_id=?", (actor.request_id, task_id))
            plan = self.one("SELECT * FROM plans WHERE request_id=? AND plan_revision=?", (actor.request_id, request["plan_revision"]))
            if (disposition not in ("update", "keep", "cancel") or not rationale.strip()
                    or request["status"] in TERMINAL_REQUESTS
                    or request["requirements_revision"] < 2 or task["status"] in ("CANCELLED", "SKIPPED", "SUPERSEDED")
                    or based_on_requirements_revision != request["requirements_revision"]
                    or target_plan_revision != request["plan_revision"]
                    or plan["based_on_requirements_revision"] != request["requirements_revision"]):
                raise ContractError("explicit disposition requires a current amendment plan")
            revision = task["task_revision"] + 1
            self._cas("UPDATE tasks SET task_revision=?,based_on_requirements_revision=?,plan_revision=?,status=?,"
                      "accepted_with_issues=0,review_round=0,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
                      (revision, request["requirements_revision"], request["plan_revision"],
                       "CANCELLED" if disposition == "cancel" else "READY", now(), task_id, expected_task_version))
            self.connection.execute("INSERT INTO amend_dispositions VALUES (?,?,?,?,?,?,?,?)", (
                actor.request_id, request["requirements_revision"], task_id, disposition, revision, request["plan_revision"], rationale, now()))
            self.connection.execute("UPDATE teams SET paused_for_change=1,status=CASE WHEN ?='cancel' THEN 'DRAINING' ELSE status END,"
                                    "state_version=state_version+1,updated_at=? WHERE task_id=? AND status IN ('STARTING','ACTIVE','DRAINING')",
                                    (disposition, now(), task_id))
            result = {"task_revision": revision, "disposition": disposition}
            self._event(actor.request_id, command_id, payload, result, "dispatcher")
            return result

    def issue_instruction(self, actor, *, expected_task_version, body, command_id):
        payload = {"type": "instruction", "task_id": actor.task_id, "body": body, "expected_task_version": expected_task_version}
        with self.transaction():
            request = self._actor(actor, ("team_lead",))
            replay = self._replay(actor.request_id, command_id, payload)
            if replay is not None:
                return replay
            task = self.one("SELECT * FROM tasks WHERE task_id=?", (actor.task_id,))
            if (task["status"] in TERMINAL_TASKS or request["status"] in TERMINAL_REQUESTS
                    or task["based_on_requirements_revision"] != request["requirements_revision"]
                    or task["plan_revision"] != request["plan_revision"]):
                raise ContractError("current disposition/plan required before new instruction")
            revision = task["instruction_revision"] + 1
            self._cas("UPDATE tasks SET instruction_revision=?,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
                      (revision, now(), actor.task_id, expected_task_version))
            self.connection.execute("UPDATE teams SET paused_for_change=1,state_version=state_version+1,updated_at=? WHERE team_id=?", (now(), actor.team_id))
            self._fence_ops(actor.request_id, actor.team_id)
            binding = self.binding(actor.task_id)
            self.connection.execute("INSERT INTO instructions VALUES (?,?,?,?,?,?,?,?,?,?,?)", (
                actor.request_id, actor.task_id, actor.team_id, revision, request["requirements_revision"],
                task["plan_revision"], task["task_revision"], request["dispatcher_epoch"], binding["execution_epoch"], canonical(body), now()))
            self._event(actor.request_id, command_id, payload, binding, "team_lead")
            return binding

    def acknowledge(self, actor, binding, *, expected_task_version, command_id):
        """Control guard intentionally permits paused teams and UNKNOWN ops."""
        payload = {"type": "ack", "binding": binding, "expected_task_version": expected_task_version}
        with self.transaction():
            request = self._actor(actor, ("worker",))
            if (actor.task_id, actor.team_id) != (binding["task_id"], binding["team_id"]):
                raise ContractError("ack belongs to another team")
            replay = self._replay(actor.request_id, command_id, payload)
            if replay is not None:
                return replay
            self._binding_matches(binding, policy=False)
            task = self.one("SELECT * FROM tasks WHERE task_id=?", (actor.task_id,))
            instruction = self.one("SELECT * FROM instructions WHERE task_id=? AND instruction_revision=?",
                                   (actor.task_id, task["instruction_revision"]))
            required = {"based_on_requirements_revision": request["requirements_revision"], "plan_revision": task["plan_revision"],
                        "task_revision": task["task_revision"], "dispatcher_epoch": request["dispatcher_epoch"],
                        "execution_epoch": actor.execution_epoch, "team_id": actor.team_id}
            if (task["status"] in TERMINAL_TASKS or request["status"] in TERMINAL_REQUESTS
                    or task["based_on_requirements_revision"] != request["requirements_revision"]
                    or task["plan_revision"] != request["plan_revision"]
                    or any(instruction[k] != v for k, v in required.items())
                    or (task["applied_task_revision"], task["applied_plan_revision"], task["applied_instruction_revision"]) ==
                       (task["task_revision"], task["plan_revision"], task["instruction_revision"])):
                raise ContractError("ack requires genuinely pending current instructions")
            self._cas("UPDATE tasks SET applied_task_revision=task_revision,applied_plan_revision=plan_revision,"
                      "applied_instruction_revision=instruction_revision,state_version=state_version+1,updated_at=? "
                      "WHERE task_id=? AND state_version=?", (now(), actor.task_id, expected_task_version))
            self.connection.execute("UPDATE teams SET paused_for_change=0,state_version=state_version+1,updated_at=? WHERE team_id=?", (now(), actor.team_id))
            self._event(actor.request_id, command_id, payload, binding, "worker")
            return binding

    def prepare_operation(self, actor, binding, *, key, tool_id, args_sha256, side_effect,
                          expected_task_version, command_id):
        intent = {"binding": binding, "key": key, "tool_id": tool_id, "args_sha256": args_sha256, "side_effect": side_effect}
        payload = {"type": "prepare", **intent, "expected_task_version": expected_task_version}
        with self.transaction():
            self._actor(actor, ("worker",))
            if (actor.request_id, actor.task_id, actor.team_id) != (binding["request_id"], binding["task_id"], binding["team_id"]):
                raise ContractError("operation owner mismatch")
            existing = self.connection.execute("SELECT * FROM ops WHERE request_id=? AND task_id=? AND business_idempotency_key=?",
                                               (actor.request_id, actor.task_id, key)).fetchone()
            if existing:
                fields = {**binding, "tool_id": tool_id, "args_sha256": args_sha256, "side_effect": side_effect}
                if any(existing[k] != v for k, v in fields.items()) or set(binding) != set(BINDING_FIELDS):
                    raise Conflict("business key reused with different content or frozen version")
                return dict(existing)  # Query only: UNKNOWN/terminal results are never executed here.
            self._ready(actor, binding, tool_id)
            self._cas("UPDATE tasks SET state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
                      (now(), actor.task_id, expected_task_version))
            op_id = new_id("op")
            columns = ["op_id", *BINDING_FIELDS, "business_idempotency_key", "tool_id", "args_sha256", "side_effect", "status", "created_at"]
            values = [op_id, *(binding[k] for k in BINDING_FIELDS), key, tool_id, args_sha256, side_effect, "PREPARED", now()]
            self.connection.execute(f"INSERT INTO ops({','.join(columns)}) VALUES ({','.join('?' for _ in columns)})", values)
            self._event(actor.request_id, command_id, payload, {"op_id": op_id}, "worker")
            return self.one("SELECT * FROM ops WHERE op_id=?", (op_id,))

    def start_operation(self, actor, op_id, *, expected_version):
        with self.transaction():
            op = self.one("SELECT * FROM ops WHERE op_id=?", (op_id,))
            self._ready(actor, {k: op[k] for k in BINDING_FIELDS}, op["tool_id"])
            if op["status"] != "PREPARED":
                raise ContractError("only PREPARED may dispatch")
            self._cas("UPDATE ops SET status='RUNNING',state_version=state_version+1,started_at=? WHERE op_id=? AND state_version=?",
                      (now(), op_id, expected_version))
            self._event(op["request_id"], "start:" + op_id, {"type": "start", "op_id": op_id}, {"status": "RUNNING"}, "worker")

    def cancel_operation(self, actor, op_id, *, expected_version, command_id):
        payload = {"type": "cancel_op", "op_id": op_id, "expected_version": expected_version}
        with self.transaction():
            self._actor(actor, ("worker",))
            op = self.one("SELECT * FROM ops WHERE op_id=?", (op_id,))
            if (op["request_id"], op["task_id"], op["team_id"]) != (actor.request_id, actor.task_id, actor.team_id):
                raise ContractError("cancel owner mismatch")
            replay = self._replay(actor.request_id, command_id, payload)
            if replay is not None:
                return replay["status"]
            if op["status"] not in ("PREPARED", "RUNNING"):
                raise ContractError("cancel cannot repeatedly settle an unknown/terminal op")
            status = "CANCELLED" if op["status"] == "PREPARED" else "UNKNOWN"
            self._cas("UPDATE ops SET status=?,state_version=state_version+1 WHERE op_id=? AND state_version=?", (status, op_id, expected_version))
            self._event(actor.request_id, command_id, payload, {"status": status}, "worker")
            return status

    def reconcile_operation(self, op_id, *, expected_version, outcome, host_evidence):
        """HOST-ONLY. An uncertain workspace effect needs reconciliation too."""
        if outcome not in ("SUCCEEDED", "FAILED", "CANCELLED") or not host_evidence.strip():
            raise ContractError("definite host outcome/evidence required")
        with self.transaction():
            op = self.one("SELECT * FROM ops WHERE op_id=?", (op_id,))
            if op["status"] != "UNKNOWN":
                raise ContractError("only UNKNOWN requires reconciliation")
            self._cas("UPDATE ops SET status=?,state_version=state_version+1,ended_at=? WHERE op_id=? AND state_version=?", (outcome, now(), op_id, expected_version))
            self._event(op["request_id"], "reconcile:" + op_id, {"type": "reconcile", "op_id": op_id, "host_evidence": host_evidence}, {"status": outcome})

    def recover_request(self, request_id, *, expected_version):
        """HOST-ONLY startup fencing, without rescheduling uncertain effects."""
        with self.transaction():
            request = self.one("SELECT * FROM requests WHERE request_id=?", (request_id,))
            if request["status"] in TERMINAL_REQUESTS:
                raise ContractError("terminal work is recovered through durable finalization jobs")
            self._cas("UPDATE requests SET dispatcher_epoch=dispatcher_epoch+1,state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?",
                      (now(), request_id, expected_version))
            self.connection.execute("UPDATE teams SET execution_epoch=execution_epoch+1,paused_for_change=1,state_version=state_version+1,updated_at=? "
                                    "WHERE request_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", (now(), request_id))
            self._fence_ops(request_id)
            self._event(request_id, new_id("recover"), {"type": "recover"}, {"dispatcher_epoch": request["dispatcher_epoch"] + 1})

    def register_memory(self, memory_id, revision, content_sha256, *, scope="project:project"):
        """HOST-ONLY index of already committed content; no Git writes."""
        with self.transaction():
            self.connection.execute("INSERT INTO memory_meta(memory_id,type,status,scope,title,description,path,revision,content_sha256,updated_at) "
                                    "VALUES (?,'experience','active',?,'reference','reference',?,?,?,?)",
                                    (memory_id, scope, "reference/" + memory_id, revision, content_sha256, now()))
            self.connection.execute("INSERT INTO memory_versions VALUES (?,?,?,?,?)", (memory_id, revision, content_sha256, "reference/" + memory_id, now()))

    def revise_memory(self, memory_id, *, expected_base_revision, expected_version, revision, content_sha256):
        with self.transaction():
            if revision == expected_base_revision:
                raise ContractError("a revision proposal must identify new content")
            self._cas("UPDATE memory_meta SET revision=?,content_sha256=?,scripts_approved=0,state_version=state_version+1,updated_at=? "
                      "WHERE memory_id=? AND revision=? AND state_version=?",
                      (revision, content_sha256, now(), memory_id, expected_base_revision, expected_version))
            row = self.one("SELECT * FROM memory_meta WHERE memory_id=?", (memory_id,))
            self.connection.execute("INSERT INTO memory_versions VALUES (?,?,?,?,?)", (memory_id, revision, content_sha256, row["path"], now()))
            self._event("memory:" + memory_id, new_id("revise"), {"type": "memory_revise", "expected_base_revision": expected_base_revision}, {"revision": revision})

    def load_memory(self, actor, memory_id, *, load_id, how="load"):
        with self.transaction():
            request = self._actor(actor, ("dispatcher", "team_lead", "worker"))
            memory = self.one("SELECT * FROM memory_meta WHERE memory_id=? AND status='active'", (memory_id,))
            allowed_scopes = {"global", "project:" + request["project_id"], "function:" + request["function_id"]}
            if memory["scope"] not in allowed_scopes:
                raise ContractError("memory scope does not apply to this request")
            self.connection.execute("INSERT INTO memory_loads VALUES (?,?,?,?,?,?,?,?,?,?,?)", (
                load_id, actor.request_id, actor.task_id, actor.team_id, actor.instance_id, actor.role,
                memory_id, memory["revision"], memory["content_sha256"], how, now()))
            return self.one("SELECT * FROM memory_loads WHERE load_id=?", (load_id,))

    def validate_usage(self, actor, usages):
        self._actor(actor, ("worker", "team_lead", "dispatcher"))
        seen = set()
        for use in usages:
            if use["load_id"] in seen:
                raise ContractError("duplicate memory load usage")
            seen.add(use["load_id"])
            row = self.one("SELECT * FROM memory_loads WHERE load_id=?", (use["load_id"],))
            if (row["request_id"], row["task_id"], row["team_id"], row["agent_instance_id"]) != (
                    actor.request_id, actor.task_id, actor.team_id, actor.instance_id):
                raise ContractError("memory load belongs to another task/agent instance")
            if (row["memory_id"], row["revision"]) != (use["memory_id"], use["revision"]) or row["how"] not in ("load", "rule"):
                raise ContractError("usage must reference the exact substantive loaded revision")
        return True

    def record_feedback(self, assessor, load_id, *, verdict, host_source_key, host_action_evidence):
        """Eligible samples require host-verified use, evidence and source grouping.

        The host must derive source/evidence from execution, not copy model claims.
        Within one source group the worst eligible verdict wins deterministically.
        """
        if verdict not in ("worked", "failed", "irrelevant") or not host_source_key.strip():
            raise ContractError("invalid feedback/source")
        with self.transaction():
            self._actor(assessor, ("team_lead", "dispatcher"))
            load = self.one("SELECT * FROM memory_loads WHERE load_id=?", (load_id,))
            if load["request_id"] != assessor.request_id or (assessor.role == "team_lead" and load["team_id"] != assessor.team_id):
                raise ContractError("feedback assessor does not own this load")
            loaded = int(load["how"] in ("load", "rule"))
            eligible = int(loaded and verdict != "irrelevant" and bool(host_action_evidence.strip()))
            old = self.connection.execute("SELECT * FROM memory_feedback WHERE memory_id=? AND revision=? AND source_key=?",
                                          (load["memory_id"], load["revision"], host_source_key)).fetchone()
            rank = {"irrelevant": 0, "worked": 1, "failed": 2}
            if old and (old["eligible"], rank[old["verdict"]]) >= (eligible, rank[verdict]):
                return dict(old)
            stamp = now()
            if old:
                self._cas("UPDATE memory_feedback SET load_id=?,request_id=?,task_id=?,agent_instance_id=?,loaded=?,eligible=?,verdict=?,"
                          "assessed_by=?,evidence=?,state_version=state_version+1,updated_at=? WHERE feedback_id=? AND state_version=?",
                          (load_id, load["request_id"], load["task_id"], load["agent_instance_id"], loaded, eligible, verdict,
                           assessor.role, host_action_evidence, stamp, old["feedback_id"], old["state_version"]))
                feedback_id = old["feedback_id"]
            else:
                feedback_id = new_id("feedback")
                self.connection.execute("INSERT INTO memory_feedback VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?)", (
                    feedback_id, load["memory_id"], load["revision"], load_id, load["request_id"], load["task_id"],
                    load["agent_instance_id"], loaded, eligible, verdict, assessor.role, host_action_evidence, host_source_key,
                    stamp, stamp))
            return self.one("SELECT * FROM memory_feedback WHERE feedback_id=?", (feedback_id,))

    def revoke_memory(self, memory_id, *, expected_version, host_revoked_tools=()):
        with self.transaction():
            self._cas("UPDATE memory_meta SET status='blocked',scripts_approved=0,state_version=state_version+1,updated_at=? WHERE memory_id=? AND state_version=?",
                      (now(), memory_id, expected_version))
            policy = self.one("SELECT * FROM runtime_policy WHERE singleton=1")
            denied = sorted(set(json.loads(policy["denied_tools"])) | set(host_revoked_tools))
            self._cas("UPDATE runtime_policy SET policy_epoch=policy_epoch+1,denied_tools=?,state_version=state_version+1 WHERE singleton=1 AND state_version=?",
                      (canonical(denied), policy["state_version"]))
            self._event("memory:" + memory_id, new_id("revoke"), {"type": "memory_revoke", "tools": denied}, {"policy_epoch": policy["policy_epoch"] + 1})

    def queue_experience(self, request_id, task_id, team_id, *, host_source_snapshot):
        with self.transaction():
            task = self.one("SELECT * FROM tasks WHERE request_id=? AND task_id=?", (request_id, task_id))
            self.one("SELECT * FROM teams WHERE request_id=? AND task_id=? AND team_id=?", (request_id, task_id, team_id))
            if task["status"] not in TERMINAL_TASKS:
                raise ContractError("experience is queued only for a settled task")
            issued = self.connection.execute("SELECT * FROM instructions WHERE team_id=? ORDER BY instruction_revision DESC LIMIT 1", (team_id,)).fetchone()
            # A retired team's report binds its own last instruction, not its
            # replacement team's newer high-water mark on the same task.
            source = dict(issued) if issued else {**task, "instruction_revision": 0}
            old = self.connection.execute("SELECT experience_id,source_snapshot FROM experience_jobs WHERE request_id=? AND task_id=? AND team_id=? AND task_revision=? AND instruction_revision=?",
                (request_id, task_id, team_id, source["task_revision"], source["instruction_revision"])).fetchone()
            if old:
                if old["source_snapshot"] != canonical(host_source_snapshot):
                    raise Conflict("experience source snapshot changed")
                return old[0]
            job_id = new_id("experience")
            self.connection.execute("INSERT INTO experience_jobs(experience_id,request_id,task_id,team_id,based_on_requirements_revision,plan_revision,task_revision,instruction_revision,status,source_snapshot,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,'PENDING',?,?,?)",
                (job_id, request_id, task_id, team_id, source["based_on_requirements_revision"], source["plan_revision"],
                 source["task_revision"], source["instruction_revision"], canonical(host_source_snapshot), now(), now()))
            return job_id

    def complete_experience(self, experience_id, *, expected_version, host_report_ref):
        if not host_report_ref.strip():
            raise ContractError("durable report or explicit no-experience receipt required")
        with self.transaction():
            self._cas("UPDATE experience_jobs SET status='DONE',report_ref=?,state_version=state_version+1,updated_at=? "
                      "WHERE experience_id=? AND state_version=? AND status IN ('PENDING','RUNNING','FAILED')",
                      (host_report_ref, now(), experience_id, expected_version))

    def enqueue_cleanup(self, request_id, *, host_manifest_snapshot, host_delivery_receipt_ref=None):
        """Persist intent only; never inspect/delete paths in this reference."""
        with self.transaction():
            request = self.one("SELECT * FROM requests WHERE request_id=?", (request_id,))
            if request["status"] not in TERMINAL_REQUESTS:
                raise ContractError("cleanup requires business-terminal request")
            if request["status"] == "DELIVERED" and (request["delivery_status"] != "CONFIRMED" or not host_delivery_receipt_ref):
                raise ContractError("delivered cleanup requires a confirmed host receipt")
            if self.connection.execute("SELECT 1 FROM ops WHERE request_id=? AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN')", (request_id,)).fetchone():
                raise ContractError("cleanup blocked by unsettled effects")
            if self.connection.execute("SELECT 1 FROM teams WHERE request_id=? AND status NOT IN ('STOPPED','CLEANED','CLEANUP_FAILED')", (request_id,)).fetchone():
                raise ContractError("cleanup requires stopped resources")
            if self.connection.execute(
                    "SELECT 1 FROM teams t JOIN tasks task ON task.task_id=t.task_id WHERE t.request_id=? AND NOT EXISTS "
                    "(SELECT 1 FROM experience_jobs e WHERE e.request_id=t.request_id AND e.task_id=t.task_id AND e.team_id=t.team_id AND e.status='DONE' "
                    "AND e.instruction_revision=COALESCE((SELECT MAX(i.instruction_revision) FROM instructions i WHERE i.team_id=t.team_id),0) "
                    "AND e.task_revision=COALESCE((SELECT i.task_revision FROM instructions i WHERE i.team_id=t.team_id ORDER BY i.instruction_revision DESC LIMIT 1),task.task_revision))",
                    (request_id,)).fetchone():
                raise ContractError("every team's durable experience job must be done")
            snapshot = {"manifest": host_manifest_snapshot, "delivery_receipt_ref": host_delivery_receipt_ref}
            old = self.connection.execute("SELECT * FROM cleanup_jobs WHERE request_id=?", (request_id,)).fetchone()
            if old:
                if old["manifest_snapshot"] != canonical(snapshot):
                    raise Conflict("cleanup intent changed")
                return old["cleanup_id"]
            self.connection.execute("INSERT INTO receipts(request_id,tenant_id,project_id,root_id,idempotency_key,content_sha256,function_id,final_status,"
                                    "manifest_sha256,usage_summary,cleanup_status,closed_at,expires_at) VALUES (?,?,?,?,?,?,?,?,?,?,'PENDING',?,?)",
                                    (request_id, request["tenant_id"], request["project_id"], request["root_id"], request["idempotency_key"],
                                     request["content_sha256"], request["function_id"], request["status"], digest(host_manifest_snapshot), request["usage"], now(), "9999-12-31T00:00:00Z"))
            cleanup_id = new_id("cleanup")
            self.connection.execute("INSERT INTO cleanup_jobs(cleanup_id,request_id,receipt_ref,manifest_snapshot,status,created_at,updated_at) VALUES (?,?,?,?,'PENDING',?,?)",
                                    (cleanup_id, request_id, request_id, canonical(snapshot), now(), now()))
            return cleanup_id

    def claim_cleanup(self, cleanup_id, *, expected_version, host_worker_id):
        """Host recovery explicitly reclaims RUNNING jobs with a new lease."""
        if not host_worker_id.strip():
            raise ContractError("cleanup worker identity required")
        with self.transaction():
            self._cas("UPDATE cleanup_jobs SET status='RUNNING',claimed_by=?,lease_epoch=lease_epoch+1,attempts=attempts+1,"
                      "state_version=state_version+1,updated_at=? WHERE cleanup_id=? AND state_version=? AND status IN ('PENDING','RUNNING','FAILED')",
                      (host_worker_id, now(), cleanup_id, expected_version))
            job = self.one("SELECT * FROM cleanup_jobs WHERE cleanup_id=?", (cleanup_id,))
            self.connection.execute("UPDATE receipts SET cleanup_status='RUNNING',state_version=state_version+1 WHERE request_id=?", (job["request_id"],))
            return job

    def finish_reference_cleanup(self, cleanup_id, *, expected_version, lease_epoch, host_worker_id):
        """TEST REFERENCE: deletes only SQLite request rows, never any files.

        A production adapter must first finish/verify idempotent filesystem
        cleanup while honoring this job's current lease and stored manifest.
        """
        with self.transaction():
            job = self.one("SELECT * FROM cleanup_jobs WHERE cleanup_id=?", (cleanup_id,))
            if job["status"] != "RUNNING" or job["lease_epoch"] != lease_epoch or job["claimed_by"] != host_worker_id:
                raise Conflict("stale cleanup worker lease")
            self._cas("UPDATE cleanup_jobs SET status='DONE',state_version=state_version+1,updated_at=? WHERE cleanup_id=? AND state_version=?", (now(), cleanup_id, expected_version))
            self.connection.execute("DELETE FROM requests WHERE request_id=?", (job["request_id"],))
            self.connection.execute("DELETE FROM events WHERE request_id=?", (job["request_id"],))
            self.connection.execute("UPDATE receipts SET cleanup_status='DONE',state_version=state_version+1 WHERE request_id=?", (job["request_id"],))
