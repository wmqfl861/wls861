-- S1.3 durable actor inboxes, bounded wake claims, explicit Worker waits and
-- immutable original model/tool feedback. Store executes this asset under one
-- transaction on a known 403 database and records its SHA-256 before commit.
CREATE TABLE event_runtime_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  store_version INTEGER NOT NULL CHECK (store_version = 6),
  migration_sha256 TEXT NOT NULL CHECK (length(migration_sha256) = 64)
) STRICT;

CREATE TABLE event_inbox (
  inbox_id INTEGER PRIMARY KEY AUTOINCREMENT,
  event_seq INTEGER NOT NULL REFERENCES events(seq) ON DELETE CASCADE,
  request_id TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  role TEXT NOT NULL CHECK (role IN ('dispatcher','team_lead','worker')),
  actor_task_id TEXT NOT NULL,
  subject_task_id TEXT NOT NULL,
  event_requirements_revision INTEGER NOT NULL CHECK (event_requirements_revision > 0),
  status TEXT NOT NULL DEFAULT 'PENDING' CHECK (status IN ('PENDING','CLAIMED','DONE')),
  claim_id TEXT,
  completed_at TEXT,
  UNIQUE (event_seq, role, subject_task_id),
  CHECK ((role = 'dispatcher' AND actor_task_id = '') OR
         (role <> 'dispatcher' AND actor_task_id <> '' AND actor_task_id = subject_task_id)),
  CHECK ((status = 'CLAIMED') = (claim_id IS NOT NULL)),
  CHECK ((status = 'DONE') = (completed_at IS NOT NULL))
) STRICT;
CREATE INDEX event_inbox_pending ON event_inbox(request_id, role, actor_task_id, status, inbox_id);

CREATE TABLE event_actor_claims (
  request_id TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  role TEXT NOT NULL CHECK (role IN ('dispatcher','team_lead','worker')),
  actor_task_id TEXT NOT NULL,
  principal_id TEXT NOT NULL,
  team_id TEXT NOT NULL,
  generation INTEGER NOT NULL CHECK (generation > 0),
  claim_id TEXT NOT NULL UNIQUE,
  status TEXT NOT NULL CHECK (status IN ('ACTIVE','CLOSED','RELEASED')),
  requirements_revision INTEGER NOT NULL CHECK (requirements_revision > 0),
  dispatcher_epoch INTEGER NOT NULL CHECK (dispatcher_epoch > 0),
  execution_epoch INTEGER NOT NULL CHECK (execution_epoch >= 0),
  captured_ids TEXT NOT NULL CHECK (json_valid(captured_ids) AND json_type(captured_ids) = 'array'),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  PRIMARY KEY (request_id, role, actor_task_id)
) STRICT;

CREATE TABLE worker_waits (
  task_id TEXT PRIMARY KEY NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,
  request_id TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  team_id TEXT NOT NULL,
  generation INTEGER NOT NULL CHECK (generation > 0),
  status TEXT NOT NULL CHECK (status IN ('WAITING','AWAKE','SUPERSEDED')),
  original_binding TEXT NOT NULL CHECK (json_valid(original_binding) AND json_type(original_binding) = 'object'),
  reason TEXT NOT NULL CHECK (length(reason) BETWEEN 1 AND 4096),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  FOREIGN KEY (request_id, task_id, team_id) REFERENCES teams(request_id, task_id, team_id) ON DELETE CASCADE
) STRICT;

CREATE TABLE model_tool_feedback (
  attempt_id TEXT PRIMARY KEY NOT NULL REFERENCES call_attempts(attempt_id) ON DELETE CASCADE,
  original_binding TEXT NOT NULL CHECK (json_valid(original_binding) AND json_type(original_binding) = 'object'),
  response TEXT NOT NULL CHECK (json_valid(response)),
  result TEXT NOT NULL CHECK (json_valid(result)),
  record_sha256 TEXT NOT NULL CHECK (length(record_sha256) = 64),
  recorded_at TEXT NOT NULL
) STRICT;
CREATE TRIGGER model_tool_feedback_immutable BEFORE UPDATE ON model_tool_feedback
  BEGIN SELECT RAISE(ABORT, 'immutable model tool feedback'); END;

INSERT INTO schema_migrations VALUES (6, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version = 404;
