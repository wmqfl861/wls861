-- S1.2 worker execution facts and immutable host workspace identities.
-- Store runs this asset in one BEGIN IMMEDIATE transaction on a known 402
-- database, then records this asset's SHA-256 before committing version 403.
-- Existing requests, snapshots, intents, attempts and events are retained.
CREATE TABLE worker_tools_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  store_version INTEGER NOT NULL CHECK (store_version = 5),
  migration_sha256 TEXT NOT NULL CHECK (length(migration_sha256) = 64)
) STRICT;

CREATE TABLE workspace_bindings (
  team_id TEXT PRIMARY KEY NOT NULL REFERENCES teams(team_id) ON DELETE CASCADE,
  canonical_path TEXT NOT NULL UNIQUE CHECK (length(canonical_path) BETWEEN 2 AND 4096),
  device INTEGER NOT NULL CHECK (device > 0),
  inode INTEGER NOT NULL CHECK (inode > 0),
  execution_policy TEXT NOT NULL CHECK (json_valid(execution_policy) AND json_type(execution_policy) = 'object'),
  execution_policy_sha256 TEXT NOT NULL CHECK (length(execution_policy_sha256) = 64),
  created_at TEXT NOT NULL,
  UNIQUE (device, inode)
) STRICT;

CREATE TABLE operation_results (
  op_id TEXT PRIMARY KEY NOT NULL REFERENCES ops(op_id) ON DELETE CASCADE,
  original_binding TEXT NOT NULL CHECK (json_valid(original_binding) AND json_type(original_binding) = 'object'),
  observed_outcome TEXT NOT NULL CHECK (observed_outcome IN ('SUCCEEDED','FAILED','TIMED_OUT','CANCELLED','UNKNOWN')),
  accepted INTEGER NOT NULL CHECK (accepted IN (0,1)),
  result TEXT NOT NULL CHECK (json_valid(result)),
  evidence TEXT NOT NULL CHECK (json_valid(evidence) AND json_type(evidence) = 'object'),
  stdout_bytes INTEGER NOT NULL CHECK (stdout_bytes BETWEEN 0 AND 1048576),
  stderr_bytes INTEGER NOT NULL CHECK (stderr_bytes BETWEEN 0 AND 1048576),
  stdout_truncated INTEGER NOT NULL CHECK (stdout_truncated IN (0,1)),
  stderr_truncated INTEGER NOT NULL CHECK (stderr_truncated IN (0,1)),
  stdout_sha256 TEXT NOT NULL CHECK (length(stdout_sha256) = 64),
  stderr_sha256 TEXT NOT NULL CHECK (length(stderr_sha256) = 64),
  record_sha256 TEXT NOT NULL CHECK (length(record_sha256) = 64),
  recorded_at TEXT NOT NULL
) STRICT;

CREATE TABLE operation_output_bytes (
  op_id TEXT NOT NULL REFERENCES operation_results(op_id) ON DELETE CASCADE,
  stream TEXT NOT NULL CHECK (stream IN ('stdout','stderr')),
  bytes BLOB NOT NULL CHECK (length(bytes) <= 1048576),
  PRIMARY KEY (op_id, stream)
) STRICT;

CREATE TRIGGER workspace_binding_immutable BEFORE UPDATE ON workspace_bindings BEGIN SELECT RAISE(ABORT, 'immutable workspace binding'); END;
CREATE TRIGGER operation_result_immutable BEFORE UPDATE ON operation_results BEGIN SELECT RAISE(ABORT, 'immutable operation result'); END;
CREATE TRIGGER operation_output_immutable BEFORE UPDATE ON operation_output_bytes BEGIN SELECT RAISE(ABORT, 'immutable operation output'); END;

INSERT INTO schema_migrations VALUES (5, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version = 403;
