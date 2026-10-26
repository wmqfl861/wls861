-- R2a.2: preserve original ownership records while adding an explicit retained
-- abandonment state and immutable pre-publication allocation receipts.
CREATE TABLE owned_allocation_metadata (
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  store_version INTEGER NOT NULL CHECK(store_version=10),
  migration_sha256 TEXT NOT NULL CHECK(length(migration_sha256)=64)
) STRICT;
CREATE TABLE owned_workspaces_v2 (
  resource_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL,
  team_id TEXT NOT NULL,
  command_key TEXT NOT NULL,
  intent TEXT NOT NULL CHECK(json_valid(intent)),
  identity TEXT CHECK(identity IS NULL OR json_valid(identity)),
  inventory TEXT CHECK(inventory IS NULL OR json_valid(inventory)),
  proof TEXT CHECK(proof IS NULL OR json_valid(proof)),
  status TEXT NOT NULL CHECK(status IN ('PREPARED','OWNED','DELETING','REMOVED','ABANDONED')),
  state_version INTEGER NOT NULL DEFAULT 1 CHECK(state_version>0),
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  allocation_receipt TEXT CHECK(allocation_receipt IS NULL OR json_valid(allocation_receipt)),
  abandonment TEXT CHECK(abandonment IS NULL OR json_valid(abandonment)),
  UNIQUE(request_id,command_key),
  CHECK((status='ABANDONED')=(abandonment IS NOT NULL))
) STRICT;
INSERT INTO owned_workspaces_v2(resource_id,request_id,team_id,command_key,intent,identity,inventory,proof,status,state_version,created_at,updated_at)
  SELECT resource_id,request_id,team_id,command_key,intent,identity,inventory,proof,status,state_version,created_at,updated_at FROM owned_workspaces;
DROP TABLE owned_workspaces;
ALTER TABLE owned_workspaces_v2 RENAME TO owned_workspaces;
CREATE INDEX owned_workspaces_request ON owned_workspaces(request_id);
CREATE UNIQUE INDEX owned_workspaces_active_team ON owned_workspaces(team_id) WHERE status<>'ABANDONED';
CREATE TRIGGER owned_workspace_cas BEFORE UPDATE ON owned_workspaces WHEN NEW.state_version<>OLD.state_version+1
  BEGIN SELECT RAISE(ABORT,'owned workspace CAS'); END;
CREATE TRIGGER owned_workspace_original BEFORE UPDATE ON owned_workspaces
  WHEN NEW.resource_id<>OLD.resource_id OR NEW.request_id<>OLD.request_id OR NEW.team_id<>OLD.team_id OR
       NEW.command_key<>OLD.command_key OR NEW.intent<>OLD.intent OR NEW.created_at<>OLD.created_at OR
       (OLD.identity IS NOT NULL AND NEW.identity IS NOT OLD.identity) OR
       (OLD.inventory IS NOT NULL AND NEW.inventory IS NOT OLD.inventory) OR
       (OLD.proof IS NOT NULL AND NEW.proof IS NOT OLD.proof) OR
       (OLD.allocation_receipt IS NOT NULL AND NEW.allocation_receipt IS NOT OLD.allocation_receipt) OR
       (OLD.abandonment IS NOT NULL AND NEW.abandonment IS NOT OLD.abandonment)
  BEGIN SELECT RAISE(ABORT,'immutable owned workspace evidence'); END;
CREATE TRIGGER owned_workspace_lifecycle BEFORE UPDATE ON owned_workspaces
  WHEN OLD.status='ABANDONED' OR
       (OLD.status='PREPARED' AND NEW.status NOT IN ('PREPARED','OWNED','ABANDONED')) OR
       (OLD.status='OWNED' AND NEW.status NOT IN ('OWNED','DELETING')) OR
       (OLD.status='DELETING' AND NEW.status NOT IN ('DELETING','REMOVED')) OR
       (OLD.status='REMOVED' AND NEW.status<>'REMOVED')
  BEGIN SELECT RAISE(ABORT,'owned workspace lifecycle'); END;
INSERT INTO schema_migrations VALUES (10, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version=408;
