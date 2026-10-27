-- R3.3b: immutable live identity beside the original call_attempts reservation.
-- No historical fake row, request snapshot or settlement is rewritten.
CREATE TABLE live_model_metadata (
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  store_version INTEGER NOT NULL CHECK(store_version=11),
  migration_sha256 TEXT NOT NULL CHECK(length(migration_sha256)=64)
) STRICT;
CREATE TABLE live_attempt_execution (
  attempt_id TEXT PRIMARY KEY NOT NULL REFERENCES call_attempts(attempt_id),
  execution_kind TEXT NOT NULL CHECK(execution_kind IN ('local_tls','provider')),
  settings TEXT NOT NULL CHECK(json_valid(settings)),
  settings_sha256 TEXT NOT NULL CHECK(length(settings_sha256)=64),
  profile_sha256 TEXT NOT NULL CHECK(length(profile_sha256)=64),
  transport_options_sha256 TEXT NOT NULL CHECK(length(transport_options_sha256)=64),
  encoded_record_sha256 TEXT NOT NULL CHECK(length(encoded_record_sha256)=64),
  request_body_sha256 TEXT NOT NULL CHECK(length(request_body_sha256)=64),
  logical_request_sha256 TEXT NOT NULL CHECK(length(logical_request_sha256)=64),
  writer_instance TEXT NOT NULL,
  created_at TEXT NOT NULL
) STRICT;
CREATE TABLE live_send_claims (
  attempt_id TEXT PRIMARY KEY NOT NULL REFERENCES live_attempt_execution(attempt_id),
  claim_id TEXT UNIQUE NOT NULL,
  command_id TEXT NOT NULL,
  command_sha256 TEXT NOT NULL CHECK(length(command_sha256)=64),
  original_attempt_version INTEGER NOT NULL CHECK(original_attempt_version>0),
  frozen_binding TEXT NOT NULL CHECK(json_valid(frozen_binding)),
  actor TEXT NOT NULL CHECK(json_valid(actor)),
  writer_instance TEXT NOT NULL,
  created_at TEXT NOT NULL
) STRICT;
CREATE TABLE live_unobserved_settlements (
  attempt_id TEXT PRIMARY KEY NOT NULL REFERENCES live_attempt_execution(attempt_id),
  claim_id TEXT UNIQUE REFERENCES live_send_claims(claim_id),
  idempotency_key TEXT NOT NULL,
  command_sha256 TEXT NOT NULL CHECK(length(command_sha256)=64),
  original_attempt_version INTEGER NOT NULL CHECK(original_attempt_version>0),
  actor TEXT NOT NULL CHECK(json_valid(actor)),
  reason TEXT NOT NULL CHECK(reason IN ('unobserved_after_recovery','not_dispatched_after_recovery')),
  created_at TEXT NOT NULL,
  CHECK((reason='not_dispatched_after_recovery')=(claim_id IS NULL))
) STRICT;
CREATE TABLE live_transport_observations (
  attempt_id TEXT PRIMARY KEY NOT NULL REFERENCES live_send_claims(attempt_id),
  claim_id TEXT UNIQUE NOT NULL REFERENCES live_send_claims(claim_id),
  record_sha256 TEXT NOT NULL CHECK(length(record_sha256)=64),
  safe_summary TEXT NOT NULL CHECK(json_valid(safe_summary)),
  response TEXT NOT NULL CHECK(json_valid(response)),
  raw_headers_hex TEXT NOT NULL CHECK(length(raw_headers_hex)%2=0),
  raw_body_hex TEXT NOT NULL CHECK(length(raw_body_hex)%2=0),
  protocol_evidence TEXT NOT NULL CHECK(json_valid(protocol_evidence)),
  created_at TEXT NOT NULL
) STRICT;
CREATE TRIGGER live_execution_no_update BEFORE UPDATE ON live_attempt_execution
  BEGIN SELECT RAISE(ABORT,'immutable live attempt identity'); END;
CREATE TRIGGER live_execution_no_delete BEFORE DELETE ON live_attempt_execution
  BEGIN SELECT RAISE(ABORT,'immutable live attempt identity'); END;
CREATE TRIGGER live_claim_no_update BEFORE UPDATE ON live_send_claims
  BEGIN SELECT RAISE(ABORT,'immutable live send claim'); END;
CREATE TRIGGER live_claim_no_delete BEFORE DELETE ON live_send_claims
  BEGIN SELECT RAISE(ABORT,'immutable live send claim'); END;
CREATE TRIGGER live_unobserved_no_update BEFORE UPDATE ON live_unobserved_settlements
  BEGIN SELECT RAISE(ABORT,'immutable host missing-response observation'); END;
CREATE TRIGGER live_unobserved_no_delete BEFORE DELETE ON live_unobserved_settlements
  BEGIN SELECT RAISE(ABORT,'immutable host missing-response observation'); END;
CREATE TRIGGER live_observation_no_update BEFORE UPDATE ON live_transport_observations
  BEGIN SELECT RAISE(ABORT,'immutable live transport observation'); END;
CREATE TRIGGER live_observation_no_delete BEFORE DELETE ON live_transport_observations
  BEGIN SELECT RAISE(ABORT,'immutable live transport observation'); END;
INSERT INTO schema_migrations VALUES (11, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version=409;
