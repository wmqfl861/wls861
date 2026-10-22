-- Additive S1.6 owned-workspace lifecycle. Never adopt legacy workspace paths.
-- The workspace phase is distinct from retained private-row purge.
CREATE TABLE owned_cleanup_metadata (
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  store_version INTEGER NOT NULL CHECK(store_version=9),
  migration_sha256 TEXT NOT NULL CHECK(length(migration_sha256)=64)
) STRICT;
CREATE TABLE owned_workspaces (
  resource_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  team_id TEXT NOT NULL UNIQUE,
  command_key TEXT NOT NULL,
  intent TEXT NOT NULL CHECK(json_valid(intent)),
  identity TEXT CHECK(identity IS NULL OR json_valid(identity)),
  inventory TEXT CHECK(inventory IS NULL OR json_valid(inventory)),
  proof TEXT CHECK(proof IS NULL OR json_valid(proof)),
  status TEXT NOT NULL CHECK(status IN ('PREPARED','OWNED','DELETING','REMOVED')),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK(state_version>0),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  UNIQUE(request_id,command_key)
) STRICT;
CREATE INDEX owned_workspaces_request ON owned_workspaces(request_id);
CREATE TRIGGER owned_workspace_cas BEFORE UPDATE ON owned_workspaces WHEN NEW.state_version<>OLD.state_version+1
  BEGIN SELECT RAISE(ABORT,'owned workspace CAS'); END;
CREATE TRIGGER owned_workspace_original BEFORE UPDATE ON owned_workspaces
  WHEN NEW.resource_id<>OLD.resource_id OR NEW.request_id<>OLD.request_id OR NEW.team_id<>OLD.team_id OR
       NEW.command_key<>OLD.command_key OR NEW.intent<>OLD.intent OR NEW.created_at<>OLD.created_at OR
       (OLD.identity IS NOT NULL AND NEW.identity IS NOT OLD.identity) OR
       (OLD.inventory IS NOT NULL AND NEW.inventory IS NOT OLD.inventory) OR
       (OLD.proof IS NOT NULL AND NEW.proof IS NOT OLD.proof)
  BEGIN SELECT RAISE(ABORT,'immutable owned workspace evidence'); END;
CREATE TABLE workspace_cleanup_runs (
  request_id TEXT PRIMARY KEY NOT NULL REFERENCES receipts(request_id),
  manifest TEXT NOT NULL CHECK(json_valid(manifest)),
  manifest_sha256 TEXT NOT NULL CHECK(length(manifest_sha256)=64),
  status TEXT NOT NULL CHECK(status IN ('RUNNING','DONE')),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK(state_version>0),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
) STRICT;
CREATE TRIGGER workspace_cleanup_cas BEFORE UPDATE ON workspace_cleanup_runs WHEN NEW.state_version<>OLD.state_version+1
  BEGIN SELECT RAISE(ABORT,'workspace cleanup CAS'); END;
CREATE TRIGGER workspace_cleanup_original BEFORE UPDATE ON workspace_cleanup_runs
  WHEN NEW.request_id<>OLD.request_id OR NEW.manifest<>OLD.manifest OR NEW.manifest_sha256<>OLD.manifest_sha256 OR NEW.created_at<>OLD.created_at
  BEGIN SELECT RAISE(ABORT,'immutable cleanup authorization'); END;
INSERT INTO schema_migrations VALUES (9, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version=407;
