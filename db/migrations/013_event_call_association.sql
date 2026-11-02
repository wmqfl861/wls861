-- N5: explicit persistent event-call protocol. Historical attempts and inbox
-- rows are retained verbatim; this migration synthesizes no associations.
CREATE TABLE event_association_metadata (
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  store_version INTEGER NOT NULL CHECK(store_version=13),
  migration_sha256 TEXT NOT NULL CHECK(length(migration_sha256)=64),
  activation_barrier_seq INTEGER NOT NULL CHECK(activation_barrier_seq>=0)
) STRICT;
CREATE TABLE n5_event_requests (
  request_id TEXT PRIMARY KEY NOT NULL REFERENCES requests(request_id),
  command_id TEXT NOT NULL,
  max_links INTEGER NOT NULL CHECK(max_links BETWEEN 1 AND 64),
  max_claim_snapshots INTEGER NOT NULL CHECK(max_claim_snapshots BETWEEN 1 AND 128),
  max_metadata_bytes INTEGER NOT NULL CHECK(max_metadata_bytes BETWEEN 1 AND 16777216),
  fact TEXT NOT NULL CHECK(json_valid(fact)),
  fact_sha256 TEXT NOT NULL CHECK(length(fact_sha256)=64),
  encoded_bytes INTEGER NOT NULL CHECK(encoded_bytes>0),
  created_at TEXT NOT NULL
) STRICT;
CREATE TABLE n5_event_claims (
  claim_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL REFERENCES n5_event_requests(request_id),
  role TEXT NOT NULL CHECK(role IN ('dispatcher','team_lead','worker')),
  actor_task_id TEXT NOT NULL,
  generation INTEGER NOT NULL CHECK(generation>0),
  fact TEXT NOT NULL CHECK(json_valid(fact) AND length(CAST(fact AS BLOB))<=65536),
  fact_sha256 TEXT NOT NULL CHECK(length(fact_sha256)=64),
  encoded_bytes INTEGER NOT NULL CHECK(encoded_bytes>0),
  created_at TEXT NOT NULL,
  UNIQUE(request_id,role,actor_task_id,generation)
) STRICT;
CREATE TABLE n5_event_links (
  link_id TEXT PRIMARY KEY NOT NULL,
  request_id TEXT NOT NULL REFERENCES n5_event_requests(request_id),
  claim_id TEXT NOT NULL UNIQUE REFERENCES n5_event_claims(claim_id),
  attempt_id TEXT NOT NULL UNIQUE REFERENCES call_attempts(attempt_id),
  call_id TEXT NOT NULL UNIQUE,
  action TEXT NOT NULL CHECK(action IN ('plan','instruction','note','amendment')),
  business_tail INTEGER NOT NULL CHECK(business_tail BETWEEN 2 AND 3),
  control_tail INTEGER NOT NULL CHECK(control_tail BETWEEN 2 AND 3),
  fact TEXT NOT NULL CHECK(json_valid(fact)),
  fact_sha256 TEXT NOT NULL CHECK(length(fact_sha256)=64),
  encoded_bytes INTEGER NOT NULL CHECK(encoded_bytes>0),
  created_at TEXT NOT NULL
) STRICT;
CREATE TABLE n5_event_items (
  link_id TEXT NOT NULL REFERENCES n5_event_links(link_id),
  inbox_id INTEGER NOT NULL REFERENCES event_inbox(inbox_id),
  fact TEXT NOT NULL CHECK(json_valid(fact)),
  fact_sha256 TEXT NOT NULL CHECK(length(fact_sha256)=64),
  encoded_bytes INTEGER NOT NULL CHECK(encoded_bytes>0),
  PRIMARY KEY(link_id,inbox_id)
) STRICT;
CREATE TABLE n5_event_receipts (
  link_id TEXT NOT NULL REFERENCES n5_event_links(link_id),
  kind TEXT NOT NULL CHECK(kind IN ('RESERVED','DISPATCH','SETTLEMENT','CLOSE','ERROR','RECOVERY')),
  ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 7),
  fact TEXT NOT NULL CHECK(json_valid(fact) AND length(CAST(fact AS BLOB))<=65536),
  fact_sha256 TEXT NOT NULL CHECK(length(fact_sha256)=64),
  encoded_bytes INTEGER NOT NULL CHECK(encoded_bytes>0),
  created_at TEXT NOT NULL,
  PRIMARY KEY(link_id,kind,ordinal),
  UNIQUE(link_id,ordinal)
) STRICT;
CREATE TABLE n5_event_open_items (
  inbox_id INTEGER PRIMARY KEY NOT NULL REFERENCES event_inbox(inbox_id),
  link_id TEXT NOT NULL REFERENCES n5_event_links(link_id),
  FOREIGN KEY(link_id,inbox_id) REFERENCES n5_event_items(link_id,inbox_id)
) STRICT;
CREATE INDEX n5_event_links_request ON n5_event_links(request_id,link_id);
CREATE INDEX n5_event_claims_request ON n5_event_claims(request_id,claim_id);
CREATE INDEX n5_event_open_link ON n5_event_open_items(link_id,inbox_id);
CREATE TRIGGER n5_event_requests_no_update BEFORE UPDATE ON n5_event_requests
  BEGIN SELECT RAISE(ABORT,'immutable N5 request policy'); END;
CREATE TRIGGER event_association_metadata_no_update BEFORE UPDATE ON event_association_metadata
  BEGIN SELECT RAISE(ABORT,'immutable N5 migration activation barrier'); END;
CREATE TRIGGER event_association_metadata_no_delete BEFORE DELETE ON event_association_metadata
  BEGIN SELECT RAISE(ABORT,'immutable N5 migration activation barrier'); END;
CREATE TRIGGER n5_event_requests_no_delete BEFORE DELETE ON n5_event_requests
  BEGIN SELECT RAISE(ABORT,'immutable N5 request policy'); END;
CREATE TRIGGER n5_event_claims_no_update BEFORE UPDATE ON n5_event_claims
  BEGIN SELECT RAISE(ABORT,'immutable N5 claim snapshot'); END;
CREATE TRIGGER n5_event_claims_no_delete BEFORE DELETE ON n5_event_claims
  BEGIN SELECT RAISE(ABORT,'immutable N5 claim snapshot'); END;
CREATE TRIGGER n5_event_links_no_update BEFORE UPDATE ON n5_event_links
  BEGIN SELECT RAISE(ABORT,'immutable N5 call association'); END;
CREATE TRIGGER n5_event_links_no_delete BEFORE DELETE ON n5_event_links
  BEGIN SELECT RAISE(ABORT,'immutable N5 call association'); END;
CREATE TRIGGER n5_event_items_no_update BEFORE UPDATE ON n5_event_items
  BEGIN SELECT RAISE(ABORT,'immutable N5 item source'); END;
CREATE TRIGGER n5_event_items_no_delete BEFORE DELETE ON n5_event_items
  BEGIN SELECT RAISE(ABORT,'immutable N5 item source'); END;
CREATE TRIGGER n5_event_receipts_no_update BEFORE UPDATE ON n5_event_receipts
  BEGIN SELECT RAISE(ABORT,'immutable N5 receipt'); END;
CREATE TRIGGER n5_event_receipts_no_delete BEFORE DELETE ON n5_event_receipts
  BEGIN SELECT RAISE(ABORT,'immutable N5 receipt'); END;
CREATE TRIGGER n5_event_receipts_bound BEFORE INSERT ON n5_event_receipts
  WHEN (SELECT COUNT(*) FROM n5_event_receipts WHERE link_id=NEW.link_id)>=8
  BEGIN SELECT RAISE(ABORT,'N5 receipt capacity exhausted'); END;
CREATE TRIGGER n5_event_items_bound BEFORE INSERT ON n5_event_items
  WHEN (SELECT COUNT(*) FROM n5_event_items WHERE link_id=NEW.link_id)>=64
  BEGIN SELECT RAISE(ABORT,'N5 member capacity exhausted'); END;
CREATE TRIGGER n5_event_open_no_update BEFORE UPDATE ON n5_event_open_items
  BEGIN SELECT RAISE(ABORT,'N5 open identity cannot be reassigned'); END;
INSERT INTO schema_migrations VALUES (13, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
PRAGMA user_version=411;
