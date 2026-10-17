-- C++ S1 Store extension of the known v0.4.1 (user_version=401) schema.
-- Executed by Store under BEGIN IMMEDIATE after validating the source version.
-- Model/tool waits never occur inside this migration or a Store transaction.
CREATE TABLE store_runtime_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  store_version INTEGER NOT NULL CHECK (store_version = 4),
  migration_sha256 TEXT NOT NULL CHECK (length(migration_sha256) = 64)
) STRICT;

CREATE TABLE request_runtime (
  request_id TEXT PRIMARY KEY NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  config_snapshot_sha256 TEXT NOT NULL CHECK (length(config_snapshot_sha256) = 64),
  tool_allowlist TEXT NOT NULL CHECK (json_valid(tool_allowlist) AND json_type(tool_allowlist) = 'array')
) STRICT;

CREATE TABLE request_budgets (
  request_id TEXT PRIMARY KEY NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  max_attempts INTEGER NOT NULL CHECK (max_attempts > 0),
  input_limit INTEGER NOT NULL CHECK (input_limit >= 0),
  output_limit INTEGER NOT NULL CHECK (output_limit >= 0),
  cost_limit INTEGER NOT NULL CHECK (cost_limit >= 0),
  attempts INTEGER NOT NULL DEFAULT 0 CHECK (attempts >= 0),
  reserved_input INTEGER NOT NULL DEFAULT 0 CHECK (reserved_input >= 0),
  reserved_output INTEGER NOT NULL DEFAULT 0 CHECK (reserved_output >= 0),
  reserved_cost INTEGER NOT NULL DEFAULT 0 CHECK (reserved_cost >= 0),
  spent_input INTEGER NOT NULL DEFAULT 0 CHECK (spent_input >= 0),
  spent_output INTEGER NOT NULL DEFAULT 0 CHECK (spent_output >= 0),
  spent_cost INTEGER NOT NULL DEFAULT 0 CHECK (spent_cost >= 0),
  unknown_input INTEGER NOT NULL DEFAULT 0 CHECK (unknown_input >= 0 AND unknown_input <= reserved_input),
  unknown_output INTEGER NOT NULL DEFAULT 0 CHECK (unknown_output >= 0 AND unknown_output <= reserved_output),
  unknown_cost INTEGER NOT NULL DEFAULT 0 CHECK (unknown_cost >= 0 AND unknown_cost <= reserved_cost),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1)
) STRICT;

CREATE TABLE call_attempts (
  attempt_id TEXT PRIMARY KEY NOT NULL,
  call_id TEXT NOT NULL UNIQUE,
  request_id TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  task_id TEXT,
  team_id TEXT,
  role TEXT NOT NULL CHECK (role IN ('dispatcher','team_lead','worker')),
  principal_id TEXT NOT NULL,
  config_snapshot_sha256 TEXT NOT NULL CHECK (length(config_snapshot_sha256) = 64),
  model_ref TEXT NOT NULL,
  frozen_binding TEXT NOT NULL CHECK (json_valid(frozen_binding)),
  request_envelope TEXT NOT NULL CHECK (json_valid(request_envelope)),
  reservation_sha256 TEXT NOT NULL CHECK (length(reservation_sha256) = 64),
  reserved_input INTEGER NOT NULL CHECK (reserved_input >= 0),
  reserved_output INTEGER NOT NULL CHECK (reserved_output >= 0),
  reserved_cost INTEGER NOT NULL CHECK (reserved_cost >= 0),
  max_output_tokens INTEGER NOT NULL CHECK (max_output_tokens > 0),
  status TEXT NOT NULL CHECK (status IN ('PENDING','COMPLETED','ERROR','INTERRUPTED','UNKNOWN','STALE')),
  accepted INTEGER NOT NULL DEFAULT 0 CHECK (accepted IN (0,1)),
  input_tokens INTEGER CHECK (input_tokens >= 0),
  output_tokens INTEGER CHECK (output_tokens >= 0),
  cache_read_tokens INTEGER CHECK (cache_read_tokens >= 0),
  cache_write_tokens INTEGER CHECK (cache_write_tokens >= 0),
  cost_micro_usd INTEGER CHECK (cost_micro_usd >= 0),
  unknown_input INTEGER NOT NULL DEFAULT 0 CHECK (unknown_input IN (0,1)),
  unknown_output INTEGER NOT NULL DEFAULT 0 CHECK (unknown_output IN (0,1)),
  unknown_cost INTEGER NOT NULL DEFAULT 0 CHECK (unknown_cost IN (0,1)),
  response TEXT CHECK (response IS NULL OR json_valid(response)),
  usage TEXT CHECK (usage IS NULL OR json_valid(usage)),
  settlement_sha256 TEXT CHECK (settlement_sha256 IS NULL OR length(settlement_sha256) = 64),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1),
  created_at TEXT NOT NULL,
  settled_at TEXT,
  CHECK ((role = 'dispatcher' AND task_id IS NULL AND team_id IS NULL) OR
         (role <> 'dispatcher' AND task_id IS NOT NULL AND team_id IS NOT NULL)),
  FOREIGN KEY (request_id, task_id, team_id) REFERENCES teams(request_id, task_id, team_id) ON DELETE CASCADE
) STRICT;
CREATE INDEX call_attempts_pending ON call_attempts(request_id, status);

CREATE TABLE operation_intents (
  op_id TEXT PRIMARY KEY NOT NULL REFERENCES ops(op_id) ON DELETE CASCADE,
  arguments TEXT NOT NULL CHECK (json_valid(arguments)),
  evidence_ref TEXT,
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version >= 1)
) STRICT;

CREATE TRIGGER budgets_cas BEFORE UPDATE ON request_budgets WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'budget state_version'); END;
CREATE TRIGGER attempts_cas BEFORE UPDATE ON call_attempts WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'attempt state_version'); END;
CREATE TRIGGER intents_cas BEFORE UPDATE ON operation_intents WHEN NEW.state_version <> OLD.state_version + 1 BEGIN SELECT RAISE(ABORT, 'intent state_version'); END;
CREATE TRIGGER snapshots_immutable BEFORE UPDATE ON request_runtime BEGIN SELECT RAISE(ABORT, 'immutable request runtime snapshot'); END;

INSERT INTO schema_migrations VALUES (4, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version = 402;
