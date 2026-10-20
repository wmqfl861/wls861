-- S1.5 current business acceptance, immutable delivery intents and retained
-- closeout facts. Applied inside the Store transaction to the known 405 DB.
CREATE TABLE acceptance_runtime_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton=1),
  store_version INTEGER NOT NULL CHECK (store_version=8),
  migration_sha256 TEXT NOT NULL CHECK (length(migration_sha256)=64)
) STRICT;

CREATE TABLE candidate_acceptances (
  acceptance_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  submission_id TEXT NOT NULL REFERENCES workflow_submissions(submission_id) ON DELETE CASCADE,
  submission_version INTEGER NOT NULL CHECK(submission_version>0),
  task_version INTEGER NOT NULL CHECK(task_version>0),
  producer_binding TEXT NOT NULL CHECK(json_valid(producer_binding)),
  actor TEXT NOT NULL CHECK(json_valid(actor)),
  captured_binding TEXT NOT NULL CHECK(json_valid(captured_binding)),
  decision TEXT NOT NULL CHECK(json_valid(decision)),
  record_sha256 TEXT NOT NULL CHECK(length(record_sha256)=64),
  status TEXT NOT NULL CHECK(status IN ('ACCEPTED','REJECTED','REVOKED','SUPERSEDED')),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK(state_version>0),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
) STRICT;
CREATE UNIQUE INDEX candidate_acceptances_current ON candidate_acceptances(task_id) WHERE status='ACCEPTED';
CREATE TRIGGER acceptance_cas BEFORE UPDATE ON candidate_acceptances WHEN NEW.state_version<>OLD.state_version+1
  BEGIN SELECT RAISE(ABORT,'acceptance state_version'); END;
CREATE TRIGGER acceptance_fact_immutable BEFORE UPDATE ON candidate_acceptances
  WHEN NEW.acceptance_id<>OLD.acceptance_id OR NEW.request_id<>OLD.request_id OR NEW.task_id<>OLD.task_id OR
    NEW.submission_id<>OLD.submission_id OR NEW.submission_version<>OLD.submission_version OR
    NEW.task_version<>OLD.task_version OR NEW.producer_binding<>OLD.producer_binding OR
    NEW.actor<>OLD.actor OR NEW.captured_binding<>OLD.captured_binding OR NEW.decision<>OLD.decision OR
    NEW.record_sha256<>OLD.record_sha256 OR NEW.created_at<>OLD.created_at
  BEGIN SELECT RAISE(ABORT,'immutable acceptance fact'); END;

CREATE TABLE experience_reports (
  experience_id TEXT PRIMARY KEY NOT NULL REFERENCES experience_jobs(experience_id),
  report_id TEXT NOT NULL UNIQUE,
  source_sha256 TEXT NOT NULL CHECK(length(source_sha256)=64),
  actor TEXT NOT NULL CHECK(json_valid(actor)),
  captured_binding TEXT NOT NULL CHECK(json_valid(captured_binding)),
  body TEXT NOT NULL CHECK(json_valid(body)),
  record_sha256 TEXT NOT NULL CHECK(length(record_sha256)=64),
  created_at TEXT NOT NULL
) STRICT;
CREATE TRIGGER experience_report_immutable BEFORE UPDATE ON experience_reports
  BEGIN SELECT RAISE(ABORT,'immutable experience report'); END;
CREATE TRIGGER experience_source_immutable BEFORE UPDATE ON experience_jobs
  WHEN NEW.experience_id<>OLD.experience_id OR NEW.request_id<>OLD.request_id OR NEW.task_id<>OLD.task_id OR
    NEW.team_id<>OLD.team_id OR NEW.based_on_requirements_revision<>OLD.based_on_requirements_revision OR
    NEW.plan_revision<>OLD.plan_revision OR NEW.task_revision<>OLD.task_revision OR
    NEW.instruction_revision<>OLD.instruction_revision OR NEW.source_snapshot<>OLD.source_snapshot OR NEW.created_at<>OLD.created_at
  BEGIN SELECT RAISE(ABORT,'immutable experience source'); END;
CREATE TABLE experience_attempts (
  attempt_id TEXT PRIMARY KEY NOT NULL REFERENCES call_attempts(attempt_id) ON DELETE CASCADE,
  experience_id TEXT NOT NULL REFERENCES experience_jobs(experience_id),
  job_version INTEGER NOT NULL CHECK(job_version>0),
  source_sha256 TEXT NOT NULL CHECK(length(source_sha256)=64)
) STRICT;
CREATE TRIGGER experience_attempt_immutable BEFORE UPDATE ON experience_attempts
  BEGIN SELECT RAISE(ABORT,'immutable experience attempt'); END;

CREATE TABLE delivery_runtime (
  delivery_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  requirements_revision INTEGER NOT NULL CHECK(requirements_revision>0),
  plan_revision INTEGER NOT NULL CHECK(plan_revision>0),
  captured_binding TEXT NOT NULL CHECK(json_valid(captured_binding)),
  frozen_plan TEXT NOT NULL CHECK(json_valid(frozen_plan)),
  manifest_sha256 TEXT NOT NULL CHECK(length(manifest_sha256)=64),
  acceptance_snapshot TEXT NOT NULL CHECK(json_valid(acceptance_snapshot)),
  status TEXT NOT NULL CHECK(status IN ('PREPARED','STORED','CONFIRMED','SUPERSEDED')),
  publication TEXT CHECK(publication IS NULL OR json_valid(publication)),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK(state_version>0),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
) STRICT;
CREATE INDEX delivery_request ON delivery_runtime(request_id,status);
CREATE TRIGGER delivery_cas BEFORE UPDATE ON delivery_runtime WHEN NEW.state_version<>OLD.state_version+1
  BEGIN SELECT RAISE(ABORT,'delivery state_version'); END;
CREATE TRIGGER delivery_plan_immutable BEFORE UPDATE ON delivery_runtime
  WHEN NEW.delivery_id<>OLD.delivery_id OR NEW.request_id<>OLD.request_id OR
    NEW.requirements_revision<>OLD.requirements_revision OR NEW.plan_revision<>OLD.plan_revision OR
    NEW.captured_binding<>OLD.captured_binding OR NEW.frozen_plan<>OLD.frozen_plan OR
    NEW.manifest_sha256<>OLD.manifest_sha256 OR NEW.acceptance_snapshot<>OLD.acceptance_snapshot OR NEW.created_at<>OLD.created_at
  BEGIN SELECT RAISE(ABORT,'immutable delivery plan'); END;

-- No FK to requests: receipts and finalization jobs survive future private-row
-- cleanup. This increment deliberately performs no purge or workspace delete.
CREATE TABLE delivery_receipts (
  receipt_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  delivery_id TEXT NOT NULL UNIQUE,
  root_id TEXT NOT NULL,
  idempotency_key TEXT NOT NULL,
  command_sha256 TEXT NOT NULL CHECK(length(command_sha256)=64),
  body TEXT NOT NULL CHECK(json_valid(body)),
  result TEXT NOT NULL CHECK(json_valid(result)),
  created_at TEXT NOT NULL,
  UNIQUE(request_id,idempotency_key)
) STRICT;
CREATE TRIGGER delivery_receipt_immutable BEFORE UPDATE ON delivery_receipts
  BEGIN SELECT RAISE(ABORT,'immutable delivery receipt'); END;
CREATE TABLE closeout_commands (
  request_id TEXT NOT NULL,
  command_id TEXT NOT NULL,
  root_id TEXT NOT NULL,
  command_sha256 TEXT NOT NULL CHECK(length(command_sha256)=64),
  result TEXT NOT NULL CHECK(json_valid(result)),
  created_at TEXT NOT NULL,
  PRIMARY KEY(request_id,command_id)
) STRICT;
CREATE TRIGGER closeout_command_immutable BEFORE UPDATE ON closeout_commands
  BEGIN SELECT RAISE(ABORT,'immutable closeout command'); END;

INSERT INTO schema_migrations VALUES (8,strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version=406;
