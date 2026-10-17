-- WLS861 v0.4.1 executable SQLite contract (fresh databases only).
-- This is the target Store layout, not the current C++ journal format.
-- See db/README.md for transaction ownership, CAS, recovery and trust boundaries.
-- Every connection MUST apply these settings, including after reopening.
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
PRAGMA busy_timeout = 5000;

BEGIN IMMEDIATE;
CREATE TABLE schema_migrations (
  version INTEGER PRIMARY KEY,
  applied_at TEXT NOT NULL
) STRICT;
CREATE TABLE contract_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  schema_version TEXT NOT NULL CHECK (schema_version = '0.4.1')
) STRICT;
INSERT INTO contract_metadata VALUES (1, '0.4.1');

CREATE TABLE runtime_policy (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  policy_epoch INTEGER NOT NULL CHECK (policy_epoch >= 1),
  denied_tools TEXT NOT NULL DEFAULT '[]' CHECK (json_valid(denied_tools)),
  state_version INTEGER NOT NULL CHECK (state_version >= 1)
) STRICT;
INSERT INTO runtime_policy(singleton, policy_epoch, state_version) VALUES (1, 1, 1);

CREATE TABLE requests (
  request_id TEXT PRIMARY KEY NOT NULL,
  tenant_id TEXT NOT NULL,
  project_id TEXT NOT NULL,
  root_id TEXT NOT NULL,
  function_id TEXT NOT NULL,
  origin TEXT NOT NULL CHECK (origin IN ('cli','api','mcp','capability_call')),
  caller_task_id TEXT,
  parent_request_id TEXT,
  replaces_request_id TEXT,
  idempotency_key TEXT NOT NULL CHECK (length(idempotency_key) BETWEEN 1 AND 256),
  content_sha256 TEXT NOT NULL CHECK (length(content_sha256) = 64),
  dispatcher_id TEXT NOT NULL UNIQUE,
  title TEXT,
  status TEXT NOT NULL CHECK (status IN ('RECEIVED','QUEUED','PLANNING','RUNNING','WAITING_INPUT',
    'VERIFYING','DELIVERING','DELIVERED','FAILED','CANCELLED','SUPERSEDED')),
  status_before_wait TEXT,
  requirements_revision INTEGER NOT NULL DEFAULT 1 CHECK (requirements_revision >= 1),
  plan_revision INTEGER NOT NULL DEFAULT 0 CHECK (plan_revision >= 0),
  dispatcher_epoch INTEGER NOT NULL DEFAULT 1 CHECK (dispatcher_epoch >= 1),
  config_snapshot TEXT NOT NULL CHECK (json_valid(config_snapshot)),
  budget TEXT NOT NULL CHECK (json_valid(budget)),
  usage TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(usage)),
  delivery_status TEXT NOT NULL DEFAULT 'NONE' CHECK (delivery_status IN ('NONE','STORED','CONFIRMED')),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  UNIQUE (tenant_id, project_id, root_id, idempotency_key)
) STRICT;
CREATE INDEX requests_status ON requests(status);

CREATE TABLE request_texts (
  request_id TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  requirements_revision INTEGER NOT NULL CHECK (requirements_revision >= 1),
  kind TEXT NOT NULL CHECK (kind IN ('ORIGINAL','AMEND')),
  text TEXT NOT NULL CHECK (length(text) BETWEEN 1 AND 65536),
  created_at TEXT NOT NULL,
  PRIMARY KEY (request_id, requirements_revision),
  CHECK ((requirements_revision = 1 AND kind = 'ORIGINAL') OR
         (requirements_revision > 1 AND kind = 'AMEND'))
) STRICT;

-- One immutable effective catalog per requirements revision. Extraction and
-- task assignment are trusted-host responsibilities, not JSON substring proof.
CREATE TABLE request_constraints (
  request_id TEXT NOT NULL,
  requirements_revision INTEGER NOT NULL,
  constraint_id TEXT NOT NULL,
  source_revision INTEGER NOT NULL CHECK (source_revision >= 1 AND source_revision <= requirements_revision),
  text TEXT NOT NULL CHECK (length(trim(text)) > 0),
  PRIMARY KEY (request_id, requirements_revision, constraint_id),
  FOREIGN KEY (request_id, requirements_revision) REFERENCES request_texts(request_id, requirements_revision) ON DELETE CASCADE,
  FOREIGN KEY (request_id, source_revision) REFERENCES request_texts(request_id, requirements_revision) ON DELETE CASCADE
) STRICT;

CREATE TABLE plans (
  request_id TEXT NOT NULL,
  plan_revision INTEGER NOT NULL CHECK (plan_revision >= 1),
  based_on_requirements_revision INTEGER NOT NULL CHECK (based_on_requirements_revision >= 1),
  body TEXT NOT NULL CHECK (json_valid(body)),
  rationale TEXT,
  created_at TEXT NOT NULL,
  PRIMARY KEY (request_id, plan_revision),
  UNIQUE (request_id, plan_revision, based_on_requirements_revision),
  FOREIGN KEY (request_id, based_on_requirements_revision) REFERENCES request_texts(request_id, requirements_revision) ON DELETE CASCADE
) STRICT;

CREATE TABLE tasks (
  task_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  node_id TEXT NOT NULL,
  plan_revision INTEGER NOT NULL CHECK (plan_revision >= 1),
  based_on_requirements_revision INTEGER NOT NULL CHECK (based_on_requirements_revision >= 1),
  task_revision INTEGER NOT NULL DEFAULT 1 CHECK (task_revision >= 1),
  applied_task_revision INTEGER NOT NULL DEFAULT 0 CHECK (applied_task_revision >= 0 AND applied_task_revision <= task_revision),
  applied_plan_revision INTEGER NOT NULL DEFAULT 0 CHECK (applied_plan_revision >= 0 AND applied_plan_revision <= plan_revision),
  instruction_revision INTEGER NOT NULL DEFAULT 0 CHECK (instruction_revision >= 0),
  applied_instruction_revision INTEGER NOT NULL DEFAULT 0 CHECK (applied_instruction_revision >= 0 AND applied_instruction_revision <= instruction_revision),
  kind TEXT NOT NULL CHECK (kind IN ('team_task','capability_call')),
  template_id TEXT,
  capability TEXT,
  status TEXT NOT NULL CHECK (status IN ('PENDING','READY','RUNNING','REVIEWING','SUBMITTED','ACCEPTED',
    'BLOCKED','FAILED','CANCELLED','SKIPPED','SUPERSEDED')),
  required INTEGER NOT NULL DEFAULT 1 CHECK (required IN (0,1)),
  review_config TEXT CHECK (review_config IS NULL OR json_valid(review_config)),
  review_round INTEGER NOT NULL DEFAULT 0 CHECK (review_round >= 0),
  accepted_with_issues INTEGER NOT NULL DEFAULT 0 CHECK (accepted_with_issues IN (0,1)),
  brief TEXT CHECK (brief IS NULL OR json_valid(brief)),
  capability_input TEXT CHECK (capability_input IS NULL OR json_valid(capability_input)),
  callee_request_id TEXT,
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  UNIQUE (request_id, node_id),
  UNIQUE (request_id, task_id),
  FOREIGN KEY (request_id, plan_revision, based_on_requirements_revision)
    REFERENCES plans(request_id, plan_revision, based_on_requirements_revision) ON DELETE CASCADE
) STRICT;
CREATE INDEX tasks_request ON tasks(request_id, status);

CREATE TABLE task_deps (
  request_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  depends_on TEXT NOT NULL,
  PRIMARY KEY (task_id, depends_on),
  CHECK (task_id <> depends_on),
  FOREIGN KEY (request_id, task_id) REFERENCES tasks(request_id, task_id) ON DELETE CASCADE,
  FOREIGN KEY (request_id, depends_on) REFERENCES tasks(request_id, task_id) ON DELETE CASCADE
) STRICT;

-- AMEND bumps the request and pauses all live teams. Every retained task needs
-- a new explicit disposition; absence is never interpreted as keep.
CREATE TABLE amend_dispositions (
  request_id TEXT NOT NULL,
  based_on_requirements_revision INTEGER NOT NULL CHECK (based_on_requirements_revision >= 2),
  task_id TEXT NOT NULL,
  disposition TEXT NOT NULL CHECK (disposition IN ('update','keep','cancel')),
  target_task_revision INTEGER NOT NULL CHECK (target_task_revision >= 1),
  target_plan_revision INTEGER NOT NULL CHECK (target_plan_revision >= 1),
  rationale TEXT NOT NULL CHECK (length(trim(rationale)) > 0),
  created_at TEXT NOT NULL,
  PRIMARY KEY (request_id, based_on_requirements_revision, task_id),
  FOREIGN KEY (request_id, based_on_requirements_revision) REFERENCES request_texts(request_id, requirements_revision) ON DELETE CASCADE,
  FOREIGN KEY (request_id, task_id) REFERENCES tasks(request_id, task_id) ON DELETE CASCADE,
  FOREIGN KEY (request_id, target_plan_revision, based_on_requirements_revision)
    REFERENCES plans(request_id, plan_revision, based_on_requirements_revision) ON DELETE CASCADE
) STRICT;

CREATE TABLE teams (
  team_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  lead_id TEXT NOT NULL,
  worker_id TEXT NOT NULL,
  template_id TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('STARTING','ACTIVE','DRAINING','STOPPED','CLEANED','CLEANUP_FAILED')),
  execution_epoch INTEGER NOT NULL DEFAULT 1 CHECK (execution_epoch >= 1),
  paused_for_change INTEGER NOT NULL DEFAULT 0 CHECK (paused_for_change IN (0,1)),
  work_order TEXT CHECK (work_order IS NULL OR json_valid(work_order)),
  lead_config TEXT NOT NULL CHECK (json_valid(lead_config)),
  worker_config TEXT NOT NULL CHECK (json_valid(worker_config)),
  workspace_path TEXT NOT NULL,
  cgroup_path TEXT,
  tool_calls_used INTEGER NOT NULL DEFAULT 0 CHECK (tool_calls_used >= 0),
  internal_rework_rounds INTEGER NOT NULL DEFAULT 0 CHECK (internal_rework_rounds BETWEEN 0 AND 3),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  UNIQUE (request_id, task_id, team_id),
  FOREIGN KEY (request_id, task_id) REFERENCES tasks(request_id, task_id) ON DELETE CASCADE
) STRICT;
CREATE UNIQUE INDEX teams_one_live ON teams(task_id) WHERE status IN ('STARTING','ACTIVE','DRAINING');
CREATE INDEX teams_request ON teams(request_id, status);

CREATE TABLE instructions (
  request_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  team_id TEXT NOT NULL,
  instruction_revision INTEGER NOT NULL CHECK (instruction_revision >= 1),
  based_on_requirements_revision INTEGER NOT NULL CHECK (based_on_requirements_revision >= 1),
  plan_revision INTEGER NOT NULL CHECK (plan_revision >= 1),
  task_revision INTEGER NOT NULL CHECK (task_revision >= 1),
  dispatcher_epoch INTEGER NOT NULL CHECK (dispatcher_epoch >= 1),
  execution_epoch INTEGER NOT NULL CHECK (execution_epoch >= 1),
  body TEXT NOT NULL CHECK (json_valid(body)),
  created_at TEXT NOT NULL,
  PRIMARY KEY (task_id, instruction_revision),
  FOREIGN KEY (request_id, task_id, team_id) REFERENCES teams(request_id, task_id, team_id) ON DELETE CASCADE,
  FOREIGN KEY (request_id, plan_revision, based_on_requirements_revision)
    REFERENCES plans(request_id, plan_revision, based_on_requirements_revision) ON DELETE CASCADE
) STRICT;

CREATE TABLE change_orders (
  change_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  from_role TEXT NOT NULL CHECK (from_role IN ('dispatcher','team_lead','runtime')),
  parent_change_id TEXT,
  based_on_requirements_revision INTEGER NOT NULL CHECK (based_on_requirements_revision >= 1),
  target_task_revision INTEGER NOT NULL CHECK (target_task_revision >= 1),
  target_plan_revision INTEGER NOT NULL CHECK (target_plan_revision >= 1),
  target_instruction_revision INTEGER CHECK (target_instruction_revision >= 1),
  dispatcher_epoch INTEGER NOT NULL CHECK (dispatcher_epoch >= 1),
  execution_epoch INTEGER CHECK (execution_epoch >= 1),
  body TEXT NOT NULL CHECK (json_valid(body)),
  acked_at TEXT,
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  created_at TEXT NOT NULL,
  UNIQUE (request_id, task_id, change_id),
  CHECK (from_role <> 'team_lead' OR (parent_change_id IS NOT NULL AND target_instruction_revision IS NOT NULL AND execution_epoch IS NOT NULL)),
  CHECK (parent_change_id IS NULL OR parent_change_id <> change_id),
  FOREIGN KEY (request_id, task_id) REFERENCES tasks(request_id, task_id) ON DELETE CASCADE,
  FOREIGN KEY (request_id, task_id, parent_change_id) REFERENCES change_orders(request_id, task_id, change_id),
  FOREIGN KEY (request_id, target_plan_revision, based_on_requirements_revision)
    REFERENCES plans(request_id, plan_revision, based_on_requirements_revision) ON DELETE CASCADE
) STRICT;
CREATE INDEX change_orders_task ON change_orders(task_id, created_at);

CREATE TABLE events (
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  event_id TEXT NOT NULL UNIQUE,
  request_id TEXT NOT NULL,
  task_id TEXT,
  team_id TEXT,
  type TEXT NOT NULL,
  actor TEXT NOT NULL CHECK (actor IN ('runtime','root','dispatcher','team_lead','worker','user')),
  command_id TEXT,
  command_sha256 TEXT CHECK (command_sha256 IS NULL OR length(command_sha256) = 64),
  causation_id TEXT,
  payload TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(payload)),
  result TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(result)),
  created_at TEXT NOT NULL,
  CHECK ((command_id IS NULL) = (command_sha256 IS NULL))
) STRICT;
CREATE UNIQUE INDEX events_command ON events(request_id, command_id) WHERE command_id IS NOT NULL;
CREATE INDEX events_request ON events(request_id, seq);

CREATE TABLE artifacts (
  artifact_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  kind TEXT NOT NULL CHECK (kind IN ('input','candidate','accepted_output','review_report','experience_report','trace_digest','delivery','other')),
  artifact_revision INTEGER NOT NULL CHECK (artifact_revision >= 1),
  path TEXT NOT NULL,
  sha256 TEXT NOT NULL CHECK (length(sha256) = 64),
  size_bytes INTEGER NOT NULL CHECK (size_bytes >= 0),
  created_at TEXT NOT NULL,
  UNIQUE (request_id, task_id, artifact_id),
  UNIQUE (request_id, task_id, artifact_revision),
  FOREIGN KEY (request_id, task_id) REFERENCES tasks(request_id, task_id) ON DELETE CASCADE
) STRICT;

-- candidate_ref/hash and every issuer version are frozen when the call starts.
CREATE TABLE review_tickets (
  review_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  submission_id TEXT NOT NULL,
  candidate_ref TEXT NOT NULL,
  candidate_sha256 TEXT NOT NULL CHECK (length(candidate_sha256) = 64),
  requirements_revision INTEGER NOT NULL CHECK (requirements_revision >= 1),
  plan_revision INTEGER NOT NULL CHECK (plan_revision >= 1),
  task_revision INTEGER NOT NULL CHECK (task_revision >= 1),
  instruction_revision INTEGER NOT NULL CHECK (instruction_revision >= 1),
  dispatcher_epoch INTEGER NOT NULL CHECK (dispatcher_epoch >= 1),
  reviewer_request_id TEXT NOT NULL,
  reviewer_task_id TEXT NOT NULL,
  reviewer_team_id TEXT NOT NULL,
  reviewer_requirements_revision INTEGER NOT NULL CHECK (reviewer_requirements_revision >= 1),
  reviewer_plan_revision INTEGER NOT NULL CHECK (reviewer_plan_revision >= 1),
  reviewer_task_revision INTEGER NOT NULL CHECK (reviewer_task_revision >= 1),
  reviewer_dispatcher_epoch INTEGER NOT NULL CHECK (reviewer_dispatcher_epoch >= 1),
  reviewer_execution_epoch INTEGER NOT NULL CHECK (reviewer_execution_epoch >= 1),
  reviewer_policy_epoch INTEGER NOT NULL CHECK (reviewer_policy_epoch >= 1),
  reviewer_instruction_revision INTEGER NOT NULL CHECK (reviewer_instruction_revision >= 1),
  round INTEGER NOT NULL CHECK (round >= 1),
  body TEXT NOT NULL CHECK (json_valid(body)),
  created_at TEXT NOT NULL,
  FOREIGN KEY (request_id, task_id, candidate_ref) REFERENCES artifacts(request_id, task_id, artifact_id) ON DELETE CASCADE
) STRICT;
CREATE TABLE review_results (
  review_id TEXT PRIMARY KEY NOT NULL REFERENCES review_tickets(review_id) ON DELETE CASCADE,
  verdict TEXT NOT NULL CHECK (verdict IN ('pass','changes_requested','inconclusive')),
  body TEXT NOT NULL CHECK (json_valid(body)),
  created_at TEXT NOT NULL
) STRICT;
CREATE TABLE verification_runs (
  verification_id TEXT PRIMARY KEY NOT NULL,
  review_id TEXT NOT NULL REFERENCES review_tickets(review_id) ON DELETE CASCADE,
  criterion_id TEXT NOT NULL,
  result TEXT NOT NULL CHECK (result IN ('pass','fail','not_run')),
  runner_id TEXT NOT NULL,
  evidence_ref TEXT NOT NULL CHECK (length(trim(evidence_ref)) > 0),
  created_at TEXT NOT NULL,
  UNIQUE (review_id, criterion_id)
) STRICT;

CREATE TABLE ops (
  op_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  team_id TEXT NOT NULL,
  tool_id TEXT NOT NULL,
  business_idempotency_key TEXT NOT NULL CHECK (length(business_idempotency_key) BETWEEN 1 AND 256),
  args_sha256 TEXT NOT NULL CHECK (length(args_sha256) = 64),
  requirements_revision INTEGER NOT NULL CHECK (requirements_revision >= 1),
  plan_revision INTEGER NOT NULL CHECK (plan_revision >= 1),
  task_revision INTEGER NOT NULL CHECK (task_revision >= 1),
  instruction_revision INTEGER NOT NULL CHECK (instruction_revision >= 1),
  dispatcher_epoch INTEGER NOT NULL CHECK (dispatcher_epoch >= 1),
  execution_epoch INTEGER NOT NULL CHECK (execution_epoch >= 1),
  policy_epoch INTEGER NOT NULL CHECK (policy_epoch >= 1),
  side_effect TEXT NOT NULL CHECK (side_effect IN ('none','workspace','external')),
  status TEXT NOT NULL CHECK (status IN ('PREPARED','RUNNING','SUCCEEDED','FAILED','TIMED_OUT','CANCEL_REQUESTED','CANCELLED','UNKNOWN')),
  exit_code INTEGER,
  error_signature TEXT,
  output_dir TEXT,
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  created_at TEXT NOT NULL,
  started_at TEXT,
  ended_at TEXT,
  UNIQUE (request_id, task_id, business_idempotency_key),
  FOREIGN KEY (request_id, task_id, team_id) REFERENCES teams(request_id, task_id, team_id) ON DELETE CASCADE
) STRICT;
CREATE INDEX ops_team ON ops(team_id, status);

CREATE TABLE questions (
  question_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  task_id TEXT,
  team_id TEXT,
  asked_by TEXT NOT NULL CHECK (asked_by IN ('worker','team_lead','dispatcher','runtime')),
  holder TEXT NOT NULL CHECK (holder IN ('team_lead','dispatcher','user','answered')),
  body TEXT NOT NULL CHECK (json_valid(body)),
  answer TEXT,
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  created_at TEXT NOT NULL,
  answered_at TEXT,
  CHECK ((holder = 'answered') = (answered_at IS NOT NULL)),
  FOREIGN KEY (request_id, task_id) REFERENCES tasks(request_id, task_id) ON DELETE CASCADE,
  FOREIGN KEY (request_id, task_id, team_id) REFERENCES teams(request_id, task_id, team_id) ON DELETE CASCADE
) STRICT;
CREATE INDEX questions_open ON questions(request_id, holder);
CREATE TABLE notes (
  note_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  team_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  kind TEXT NOT NULL CHECK (kind IN ('tool_usage','pitfall','method','fact')),
  text TEXT NOT NULL,
  evidence TEXT,
  created_at TEXT NOT NULL,
  FOREIGN KEY (request_id, task_id, team_id) REFERENCES teams(request_id, task_id, team_id) ON DELETE CASCADE
) STRICT;
CREATE TABLE llm_calls (
  call_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  task_id TEXT,
  team_id TEXT,
  role TEXT NOT NULL CHECK (role IN ('dispatcher','team_lead','worker','compactor','curator')),
  model_ref TEXT NOT NULL,
  frozen_binding TEXT NOT NULL CHECK (json_valid(frozen_binding)),
  prefix_sha256 TEXT,
  input_tokens INTEGER CHECK (input_tokens >= 0),
  cache_read_tokens INTEGER CHECK (cache_read_tokens >= 0),
  cache_write_tokens INTEGER CHECK (cache_write_tokens >= 0),
  output_tokens INTEGER CHECK (output_tokens >= 0),
  cost_micro_usd INTEGER CHECK (cost_micro_usd >= 0),
  latency_ms INTEGER CHECK (latency_ms >= 0),
  status TEXT NOT NULL CHECK (status IN ('pending','ok','retried','error')),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  created_at TEXT NOT NULL,
  FOREIGN KEY (request_id, task_id) REFERENCES tasks(request_id, task_id) ON DELETE CASCADE,
  FOREIGN KEY (request_id, task_id, team_id) REFERENCES teams(request_id, task_id, team_id) ON DELETE CASCADE
) STRICT;

-- Retained rows have NO request FK. Cleanup cannot erase its own recovery or
-- idempotency evidence. TTL is host policy, never the request's CASCADE.
CREATE TABLE receipts (
  request_id TEXT PRIMARY KEY NOT NULL,
  tenant_id TEXT NOT NULL,
  project_id TEXT NOT NULL,
  root_id TEXT NOT NULL,
  idempotency_key TEXT NOT NULL,
  content_sha256 TEXT NOT NULL CHECK (length(content_sha256) = 64),
  function_id TEXT NOT NULL,
  final_status TEXT NOT NULL CHECK (final_status IN ('DELIVERED','FAILED','CANCELLED','SUPERSEDED')),
  delivery_path TEXT,
  manifest_sha256 TEXT CHECK (manifest_sha256 IS NULL OR length(manifest_sha256) = 64),
  usage_summary TEXT NOT NULL CHECK (json_valid(usage_summary)),
  cleanup_status TEXT NOT NULL CHECK (cleanup_status IN ('PENDING','RUNNING','DONE','FAILED')),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  closed_at TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  UNIQUE (tenant_id, project_id, root_id, idempotency_key)
) STRICT;
CREATE INDEX receipts_expiry ON receipts(expires_at);
CREATE TABLE experience_jobs (
  experience_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  team_id TEXT NOT NULL,
  based_on_requirements_revision INTEGER NOT NULL CHECK (based_on_requirements_revision >= 1),
  plan_revision INTEGER NOT NULL CHECK (plan_revision >= 1),
  task_revision INTEGER NOT NULL CHECK (task_revision >= 1),
  instruction_revision INTEGER NOT NULL CHECK (instruction_revision >= 0),
  status TEXT NOT NULL CHECK (status IN ('PENDING','RUNNING','DONE','FAILED')),
  report_ref TEXT,
  source_snapshot TEXT NOT NULL CHECK (json_valid(source_snapshot)),
  claimed_by TEXT,
  lease_epoch INTEGER NOT NULL DEFAULT 0 CHECK (lease_epoch >= 0),
  attempts INTEGER NOT NULL DEFAULT 0 CHECK (attempts >= 0),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  UNIQUE (request_id, task_id, team_id, task_revision, instruction_revision),
  CHECK (status <> 'DONE' OR (report_ref IS NOT NULL AND length(report_ref) > 0))
) STRICT;
CREATE TABLE cleanup_jobs (
  cleanup_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL UNIQUE,
  receipt_ref TEXT NOT NULL REFERENCES receipts(request_id),
  manifest_snapshot TEXT NOT NULL CHECK (json_valid(manifest_snapshot)),
  status TEXT NOT NULL CHECK (status IN ('PENDING','RUNNING','DONE','FAILED')),
  claimed_by TEXT,
  lease_epoch INTEGER NOT NULL DEFAULT 0 CHECK (lease_epoch >= 0),
  attempts INTEGER NOT NULL DEFAULT 0 CHECK (attempts >= 0),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  last_error TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
) STRICT;
CREATE INDEX cleanup_jobs_recovery ON cleanup_jobs(status);
CREATE INDEX experience_jobs_recovery ON experience_jobs(status);

-- Git remains content authority. These version indexes are rebuildable;
-- loads, feedback and pending jobs are durable evidence, not a disposable index.
CREATE TABLE memory_meta (
  memory_id TEXT PRIMARY KEY NOT NULL,
  type TEXT NOT NULL CHECK (type IN ('rule','skill','experience','fact')),
  status TEXT NOT NULL CHECK (status IN ('draft','active','archived','blocked')),
  scope TEXT NOT NULL,
  proposed_scope TEXT,
  title TEXT NOT NULL,
  description TEXT NOT NULL,
  outcome TEXT CHECK (outcome IN ('success','failure','mixed')),
  intent TEXT,
  tags TEXT NOT NULL DEFAULT '[]' CHECK (json_valid(tags)),
  applies_to TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(applies_to)),
  enforcement TEXT CHECK (enforcement IN ('guidance','policy')),
  path TEXT NOT NULL,
  revision TEXT NOT NULL CHECK (length(revision) IN (40,64)),
  content_sha256 TEXT NOT NULL CHECK (length(content_sha256) = 64),
  scripts_approved INTEGER NOT NULL DEFAULT 0 CHECK (scripts_approved IN (0,1)),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  updated_at TEXT NOT NULL
) STRICT;
CREATE INDEX memory_meta_lookup ON memory_meta(status, type, scope);
CREATE INDEX memory_meta_intent ON memory_meta(intent);
CREATE TABLE memory_versions (
  memory_id TEXT NOT NULL REFERENCES memory_meta(memory_id) ON DELETE CASCADE,
  revision TEXT NOT NULL CHECK (length(revision) IN (40,64)),
  content_sha256 TEXT NOT NULL CHECK (length(content_sha256) = 64),
  path TEXT NOT NULL,
  created_at TEXT NOT NULL,
  PRIMARY KEY (memory_id, revision)
) STRICT;
CREATE TABLE memory_error_signatures (
  memory_id TEXT NOT NULL,
  revision TEXT NOT NULL,
  signature TEXT NOT NULL,
  PRIMARY KEY (memory_id, revision, signature),
  FOREIGN KEY (memory_id, revision) REFERENCES memory_versions(memory_id, revision) ON DELETE CASCADE
) STRICT;
CREATE INDEX memory_error_signatures_sig ON memory_error_signatures(signature);
CREATE VIRTUAL TABLE memory_fts USING fts5(
  memory_id UNINDEXED, revision UNINDEXED, title, description, tags, body,
  tokenize = 'trigram'
);

CREATE TABLE memory_loads (
  load_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  task_id TEXT,
  team_id TEXT,
  agent_instance_id TEXT NOT NULL,
  agent_role TEXT NOT NULL CHECK (agent_role IN ('dispatcher','team_lead','worker')),
  memory_id TEXT NOT NULL,
  revision TEXT NOT NULL CHECK (length(revision) IN (40,64)),
  content_sha256 TEXT NOT NULL CHECK (length(content_sha256) = 64),
  how TEXT NOT NULL CHECK (how IN ('rule','card','recall','load','error_trigger')),
  loaded_at TEXT NOT NULL,
  CHECK ((agent_role = 'dispatcher' AND task_id IS NULL AND team_id IS NULL) OR
         (agent_role <> 'dispatcher' AND task_id IS NOT NULL AND team_id IS NOT NULL)),
  FOREIGN KEY (request_id, task_id, team_id) REFERENCES teams(request_id, task_id, team_id) ON DELETE CASCADE
) STRICT;
CREATE INDEX memory_loads_version ON memory_loads(request_id, memory_id, revision);
CREATE INDEX memory_loads_instance ON memory_loads(task_id, agent_instance_id);

-- source_key is the host's independence/cohort key; separate requests can
-- share one key. Verbatim card/recall exposure is not a substantive use sample.
CREATE TABLE memory_feedback (
  feedback_id TEXT PRIMARY KEY NOT NULL,
  memory_id TEXT NOT NULL,
  revision TEXT NOT NULL CHECK (length(revision) IN (40,64)),
  load_id TEXT NOT NULL,
  request_id TEXT NOT NULL,
  task_id TEXT,
  agent_instance_id TEXT NOT NULL,
  loaded INTEGER NOT NULL CHECK (loaded IN (0,1)),
  eligible INTEGER NOT NULL CHECK (eligible IN (0,1)),
  verdict TEXT NOT NULL CHECK (verdict IN ('worked','failed','irrelevant')),
  assessed_by TEXT NOT NULL CHECK (assessed_by IN ('team_lead','dispatcher','user')),
  evidence TEXT NOT NULL,
  source_key TEXT NOT NULL CHECK (length(trim(source_key)) > 0),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  UNIQUE (memory_id, revision, source_key),
  CHECK (eligible = 0 OR (loaded = 1 AND verdict <> 'irrelevant' AND length(trim(evidence)) > 0))
) STRICT;
CREATE INDEX memory_feedback_memory ON memory_feedback(memory_id, revision);
CREATE VIEW memory_stats AS
SELECT memory_id, revision,
       SUM(CASE WHEN loaded = 1 AND eligible = 1 AND verdict = 'worked' THEN 1 ELSE 0 END) AS successes,
       SUM(CASE WHEN loaded = 1 AND eligible = 1 AND verdict = 'failed' THEN 1 ELSE 0 END) AS failures,
       SUM(CASE WHEN loaded = 1 AND eligible = 1 THEN 1 ELSE 0 END) AS assessments
FROM memory_feedback GROUP BY memory_id, revision;
CREATE VIEW memory_current_stats AS
SELECT m.memory_id, m.revision, COALESCE(s.successes, 0) AS successes,
       COALESCE(s.failures, 0) AS failures, COALESCE(s.assessments, 0) AS assessments
FROM memory_meta m LEFT JOIN memory_stats s ON s.memory_id = m.memory_id AND s.revision = m.revision;

-- Compare-and-swap guardrails: WHERE state_version = expected_version is still
-- mandatory in the host transaction. Triggers reject updates that forget the bump.
CREATE TRIGGER requests_cas BEFORE UPDATE ON requests WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'requests state_version'); END;
CREATE TRIGGER tasks_cas BEFORE UPDATE ON tasks WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'tasks state_version'); END;
CREATE TRIGGER teams_cas BEFORE UPDATE ON teams WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'teams state_version'); END;
CREATE TRIGGER ops_cas BEFORE UPDATE ON ops WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'ops state_version'); END;
CREATE TRIGGER questions_cas BEFORE UPDATE ON questions WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'questions state_version'); END;
CREATE TRIGGER changes_cas BEFORE UPDATE ON change_orders WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'change_orders state_version'); END;
CREATE TRIGGER calls_cas BEFORE UPDATE ON llm_calls WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'llm_calls state_version'); END;
CREATE TRIGGER policy_cas BEFORE UPDATE ON runtime_policy WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'runtime_policy state_version'); END;
CREATE TRIGGER receipt_cas BEFORE UPDATE ON receipts WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'receipts state_version'); END;
CREATE TRIGGER experience_cas BEFORE UPDATE ON experience_jobs WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'experience_jobs state_version'); END;
CREATE TRIGGER cleanup_cas BEFORE UPDATE ON cleanup_jobs WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'cleanup_jobs state_version'); END;
CREATE TRIGGER memory_cas BEFORE UPDATE ON memory_meta WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'memory_meta state_version'); END;
CREATE TRIGGER feedback_cas BEFORE UPDATE ON memory_feedback WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'memory_feedback state_version'); END;

CREATE TRIGGER texts_immutable BEFORE UPDATE ON request_texts BEGIN SELECT RAISE(ABORT, 'immutable request text'); END;
CREATE TRIGGER constraints_immutable BEFORE UPDATE ON request_constraints BEGIN SELECT RAISE(ABORT, 'immutable constraint catalog'); END;
CREATE TRIGGER plans_immutable BEFORE UPDATE ON plans BEGIN SELECT RAISE(ABORT, 'immutable plan'); END;
CREATE TRIGGER dispositions_immutable BEFORE UPDATE ON amend_dispositions BEGIN SELECT RAISE(ABORT, 'immutable disposition'); END;
CREATE TRIGGER instructions_immutable BEFORE UPDATE ON instructions BEGIN SELECT RAISE(ABORT, 'immutable instruction'); END;
CREATE TRIGGER events_immutable BEFORE UPDATE ON events BEGIN SELECT RAISE(ABORT, 'immutable event'); END;
CREATE TRIGGER artifacts_immutable BEFORE UPDATE ON artifacts BEGIN SELECT RAISE(ABORT, 'immutable artifact'); END;
CREATE TRIGGER tickets_immutable BEFORE UPDATE ON review_tickets BEGIN SELECT RAISE(ABORT, 'immutable review ticket'); END;
CREATE TRIGGER reviews_immutable BEFORE UPDATE ON review_results BEGIN SELECT RAISE(ABORT, 'immutable review result'); END;
CREATE TRIGGER verification_immutable BEFORE UPDATE ON verification_runs BEGIN SELECT RAISE(ABORT, 'immutable verification run'); END;
CREATE TRIGGER loads_immutable BEFORE UPDATE ON memory_loads BEGIN SELECT RAISE(ABORT, 'immutable memory load'); END;
CREATE TRIGGER versions_immutable BEFORE UPDATE ON memory_versions BEGIN SELECT RAISE(ABORT, 'immutable memory version'); END;

INSERT INTO schema_migrations VALUES (3, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version = 401;
COMMIT;
