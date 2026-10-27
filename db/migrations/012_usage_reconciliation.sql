-- R3.3b: original settlement stays immutable; new evidence updates only this
-- projection and the original request_budgets in the same CAS transaction.
CREATE TABLE usage_reconciliation_metadata (
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  store_version INTEGER NOT NULL CHECK(store_version=12),
  migration_sha256 TEXT NOT NULL CHECK(length(migration_sha256)=64)
) STRICT;
CREATE TABLE live_usage_projection (
  attempt_id TEXT PRIMARY KEY NOT NULL REFERENCES live_attempt_execution(attempt_id),
  settlement_sha256 TEXT NOT NULL CHECK(length(settlement_sha256)=64),
  input_tokens INTEGER CHECK(input_tokens IS NULL OR input_tokens>=0),
  output_tokens INTEGER CHECK(output_tokens IS NULL OR output_tokens>=0),
  cache_read_tokens INTEGER CHECK(cache_read_tokens IS NULL OR cache_read_tokens>=0),
  cache_write_tokens INTEGER CHECK(cache_write_tokens IS NULL OR cache_write_tokens>=0),
  cost_micro_usd INTEGER CHECK(cost_micro_usd IS NULL OR cost_micro_usd>=0),
  disputed INTEGER NOT NULL CHECK(disputed IN (0,1)),
  over_reserved INTEGER NOT NULL CHECK(over_reserved IN (0,1)),
  state_version INTEGER NOT NULL CHECK(state_version>0),
  chain_sha256 TEXT NOT NULL CHECK(length(chain_sha256)=64)
) STRICT;
CREATE TABLE usage_reconciliations (
  reconciliation_id TEXT PRIMARY KEY NOT NULL,
  attempt_id TEXT NOT NULL REFERENCES live_usage_projection(attempt_id),
  idempotency_key TEXT NOT NULL,
  expected_projection_version INTEGER NOT NULL CHECK(expected_projection_version>0),
  record_sha256 TEXT NOT NULL CHECK(length(record_sha256)=64),
  evidence_sha256 TEXT NOT NULL CHECK(length(evidence_sha256)=64),
  source_kind TEXT NOT NULL CHECK(source_kind IN ('loopback_fixture','host_verified_provider','host_no_dispatch')),
  source_reference TEXT NOT NULL,
  source_bytes_hex TEXT NOT NULL CHECK(length(source_bytes_hex)%2=0),
  statement TEXT NOT NULL CHECK(json_valid(statement)),
  result TEXT NOT NULL CHECK(json_valid(result)),
  created_at TEXT NOT NULL,
  UNIQUE(attempt_id,idempotency_key),
  UNIQUE(attempt_id,expected_projection_version)
) STRICT;
CREATE TRIGGER usage_projection_cas BEFORE UPDATE ON live_usage_projection
  WHEN NEW.state_version<>OLD.state_version+1 OR
    NEW.attempt_id<>OLD.attempt_id OR NEW.settlement_sha256<>OLD.settlement_sha256 OR
    NEW.disputed<OLD.disputed OR NEW.over_reserved<OLD.over_reserved OR
    (OLD.input_tokens IS NOT NULL AND NEW.input_tokens IS NOT OLD.input_tokens) OR
    (OLD.output_tokens IS NOT NULL AND NEW.output_tokens IS NOT OLD.output_tokens) OR
    (OLD.cache_read_tokens IS NOT NULL AND NEW.cache_read_tokens IS NOT OLD.cache_read_tokens) OR
    (OLD.cache_write_tokens IS NOT NULL AND NEW.cache_write_tokens IS NOT OLD.cache_write_tokens) OR
    (OLD.cost_micro_usd IS NOT NULL AND NEW.cost_micro_usd IS NOT OLD.cost_micro_usd)
  BEGIN SELECT RAISE(ABORT,'immutable known usage or projection CAS'); END;
CREATE TRIGGER usage_projection_no_delete BEFORE DELETE ON live_usage_projection
  BEGIN SELECT RAISE(ABORT,'immutable usage projection identity'); END;
CREATE TRIGGER usage_reconciliation_no_update BEFORE UPDATE ON usage_reconciliations
  BEGIN SELECT RAISE(ABORT,'immutable usage reconciliation'); END;
CREATE TRIGGER usage_reconciliation_no_delete BEFORE DELETE ON usage_reconciliations
  BEGIN SELECT RAISE(ABORT,'immutable usage reconciliation'); END;
INSERT INTO schema_migrations VALUES (12, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version=410;
