-- S1.4 immutable questions, captured submission files and bounded TeamLead
-- checking. Executed transactionally on the known 404 schema; old assets stay
-- unchanged. The host records this migration's complete SHA-256 before commit.
CREATE TABLE workflow_runtime_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  store_version INTEGER NOT NULL CHECK (store_version = 7),
  migration_sha256 TEXT NOT NULL CHECK (length(migration_sha256) = 64)
) STRICT;

CREATE TABLE question_runtime (
  question_id TEXT PRIMARY KEY NOT NULL REFERENCES questions(question_id) ON DELETE CASCADE,
  original_binding TEXT NOT NULL CHECK (json_valid(original_binding) AND json_type(original_binding) = 'object'),
  original_sha256 TEXT NOT NULL CHECK (length(original_sha256) = 64),
  status TEXT NOT NULL CHECK (status IN ('OPEN','ANSWERED','SUPERSEDED')),
  blocking INTEGER NOT NULL CHECK (blocking IN (0,1)),
  wait_generation INTEGER CHECK (wait_generation > 0),
  CHECK ((blocking = 1) = (wait_generation IS NOT NULL))
) STRICT;
CREATE TRIGGER question_original_immutable BEFORE UPDATE ON questions
  WHEN NEW.question_id <> OLD.question_id OR NEW.request_id <> OLD.request_id OR
    NEW.task_id IS NOT OLD.task_id OR NEW.team_id IS NOT OLD.team_id OR
    NEW.asked_by <> OLD.asked_by OR NEW.body <> OLD.body OR NEW.created_at <> OLD.created_at
  BEGIN SELECT RAISE(ABORT, 'immutable original question'); END;
CREATE TRIGGER question_binding_immutable BEFORE UPDATE ON question_runtime
  WHEN NEW.question_id <> OLD.question_id OR NEW.original_binding <> OLD.original_binding OR
    NEW.original_sha256 <> OLD.original_sha256 OR NEW.blocking <> OLD.blocking OR
    NEW.wait_generation IS NOT OLD.wait_generation
  BEGIN SELECT RAISE(ABORT, 'immutable original question binding'); END;
CREATE TRIGGER question_answer_immutable BEFORE UPDATE ON questions
  WHEN OLD.answered_at IS NOT NULL AND (NEW.answer IS NOT OLD.answer OR
    NEW.answered_at IS NOT OLD.answered_at OR NEW.holder <> 'answered')
  BEGIN SELECT RAISE(ABORT, 'immutable original question answer'); END;

CREATE TABLE question_transitions (
  question_id TEXT NOT NULL REFERENCES questions(question_id) ON DELETE CASCADE,
  state_version INTEGER NOT NULL CHECK (state_version > 1),
  kind TEXT NOT NULL CHECK (kind IN ('FORWARDED','ANSWERED')),
  actor TEXT NOT NULL CHECK (json_valid(actor) AND json_type(actor) = 'object'),
  captured_binding TEXT NOT NULL CHECK (json_valid(captured_binding) AND json_type(captured_binding) = 'object'),
  body TEXT NOT NULL CHECK (json_valid(body) AND json_type(body) = 'object'),
  created_at TEXT NOT NULL,
  PRIMARY KEY (question_id, state_version)
) STRICT;
CREATE TRIGGER question_transition_immutable BEFORE UPDATE ON question_transitions
  BEGIN SELECT RAISE(ABORT, 'immutable question transition'); END;

CREATE TABLE workflow_submissions (
  submission_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  team_id TEXT NOT NULL,
  original_binding TEXT NOT NULL CHECK (json_valid(original_binding) AND json_type(original_binding) = 'object'),
  body TEXT NOT NULL CHECK (json_valid(body) AND json_type(body) = 'object'),
  artifact_snapshot TEXT NOT NULL CHECK (json_valid(artifact_snapshot) AND json_type(artifact_snapshot) = 'array'),
  record_sha256 TEXT NOT NULL CHECK (length(record_sha256) = 64),
  status TEXT NOT NULL CHECK (status IN ('SUBMITTED','CANDIDATE_READY','RETURNED_FOR_REWORK','BLOCKED','SUPERSEDED')),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK (state_version > 0),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  FOREIGN KEY (request_id, task_id, team_id) REFERENCES teams(request_id, task_id, team_id) ON DELETE CASCADE
) STRICT;
CREATE INDEX workflow_submissions_task ON workflow_submissions(task_id, status);
CREATE UNIQUE INDEX workflow_submissions_current ON workflow_submissions(task_id)
  WHERE status IN ('SUBMITTED','CANDIDATE_READY','BLOCKED');
CREATE TRIGGER workflow_submission_cas BEFORE UPDATE ON workflow_submissions
  WHEN NEW.state_version <> OLD.state_version + 1
  BEGIN SELECT RAISE(ABORT, 'workflow submission state_version'); END;
CREATE TRIGGER workflow_submission_immutable BEFORE UPDATE ON workflow_submissions
  WHEN NEW.submission_id <> OLD.submission_id OR NEW.request_id <> OLD.request_id OR
    NEW.task_id <> OLD.task_id OR NEW.team_id <> OLD.team_id OR
    NEW.original_binding <> OLD.original_binding OR NEW.body <> OLD.body OR
    NEW.artifact_snapshot <> OLD.artifact_snapshot OR NEW.record_sha256 <> OLD.record_sha256 OR
    NEW.created_at <> OLD.created_at
  BEGIN SELECT RAISE(ABORT, 'immutable original submission'); END;

CREATE TABLE team_lead_checks (
  submission_id TEXT NOT NULL REFERENCES workflow_submissions(submission_id) ON DELETE CASCADE,
  submission_version INTEGER NOT NULL CHECK (submission_version > 1),
  actor TEXT NOT NULL CHECK (json_valid(actor) AND json_type(actor) = 'object'),
  captured_binding TEXT NOT NULL CHECK (json_valid(captured_binding) AND json_type(captured_binding) = 'object'),
  check_body TEXT NOT NULL CHECK (json_valid(check_body) AND json_type(check_body) = 'object'),
  rework_order TEXT CHECK (rework_order IS NULL OR (json_valid(rework_order) AND json_type(rework_order) = 'object')),
  status TEXT NOT NULL CHECK (status IN ('CANDIDATE_READY','RETURNED_FOR_REWORK','BLOCKED')),
  created_at TEXT NOT NULL,
  PRIMARY KEY (submission_id, submission_version)
) STRICT;
CREATE TRIGGER team_lead_check_immutable BEFORE UPDATE ON team_lead_checks
  BEGIN SELECT RAISE(ABORT, 'immutable TeamLead check'); END;

CREATE TABLE team_workflow_state (
  team_id TEXT PRIMARY KEY NOT NULL REFERENCES teams(team_id) ON DELETE CASCADE,
  rework_count INTEGER NOT NULL DEFAULT 0 CHECK (rework_count >= 0)
) STRICT;

INSERT INTO schema_migrations VALUES (7, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version = 405;
