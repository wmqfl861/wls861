-- 无量山 v0.4 SQLite 结构
-- 语义见 docs/01_实施规格_v0.4.md 第 5、10、14 节；记忆部分见 docs/03_记忆系统v1规格.md。
-- 每个连接打开后设置：
--   PRAGMA journal_mode = WAL;  PRAGMA foreign_keys = ON;
--   PRAGMA busy_timeout = 5000; PRAGMA synchronous = NORMAL;
-- 时间统一用 UTC ISO-8601 文本（如 2026-10-17T11:32:00Z）。
-- JSON 列存文本，内容按 schemas/ 中对应 Schema 校验后写入。
-- 迁移按 schema_migrations.version 递增：1 = S1 核心表，2 = S3 记忆表。

CREATE TABLE IF NOT EXISTS schema_migrations (
  version     INTEGER PRIMARY KEY,
  applied_at  TEXT NOT NULL
);

-- =========================== 迁移 1：S1 核心 ===========================

CREATE TABLE requests (
  request_id          TEXT PRIMARY KEY,                          -- req_<ULID>
  project_id          TEXT NOT NULL DEFAULT 'default',
  function_id         TEXT NOT NULL,
  origin              TEXT NOT NULL CHECK (origin IN ('cli','api','mcp','capability_call')),
  caller_task_id      TEXT,                                      -- capability_call：调用方任务
  parent_request_id   TEXT,                                      -- 对已结束请求的后续请求
  replaces_request_id TEXT,                                      -- REPLACE 产生的新请求
  idempotency_key     TEXT NOT NULL UNIQUE,
  content_sha256      TEXT NOT NULL,
  title               TEXT,
  status              TEXT NOT NULL CHECK (status IN ('RECEIVED','QUEUED','PLANNING','RUNNING','WAITING_INPUT',
                                                      'VERIFYING','DELIVERING','DELIVERED','FAILED','CANCELLED','SUPERSEDED')),
  status_before_wait  TEXT,                                      -- WAITING_INPUT 结束后回到的状态
  requirements_rev    INTEGER NOT NULL DEFAULT 1,
  plan_rev            INTEGER NOT NULL DEFAULT 0,
  dispatcher_epoch    INTEGER NOT NULL DEFAULT 0,
  config_snapshot     TEXT NOT NULL,                             -- JSON：本请求冻结的功能配置
  budget              TEXT NOT NULL,                             -- JSON：token / 金额 / 墙钟上限
  usage               TEXT NOT NULL DEFAULT '{}',                -- JSON：累计用量
  delivery_status     TEXT NOT NULL DEFAULT 'NONE' CHECK (delivery_status IN ('NONE','STORED','CONFIRMED')),
  cleanup_status      TEXT NOT NULL DEFAULT 'NONE' CHECK (cleanup_status IN ('NONE','PENDING','RUNNING','DONE','FAILED')),
  state_version       INTEGER NOT NULL DEFAULT 1,
  created_at          TEXT NOT NULL,
  updated_at          TEXT NOT NULL
);
CREATE INDEX requests_status ON requests(status);

-- 请求原文与每次修改原文，只增不改（原始需求直达）
CREATE TABLE request_texts (
  request_id  TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  rev         INTEGER NOT NULL,                                  -- 1 = 原文，2.. = 第 n 次修改后的需求版本
  kind        TEXT NOT NULL CHECK (kind IN ('ORIGINAL','AMEND')),
  text        TEXT NOT NULL,
  created_at  TEXT NOT NULL,
  PRIMARY KEY (request_id, rev)
);

CREATE TABLE plans (
  request_id  TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  plan_rev    INTEGER NOT NULL,
  body        TEXT NOT NULL,                                     -- JSON：plan.schema.json
  rationale   TEXT,
  created_at  TEXT NOT NULL,
  PRIMARY KEY (request_id, plan_rev)
);

CREATE TABLE tasks (
  task_id             TEXT PRIMARY KEY,                          -- tsk_<ULID>
  request_id          TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  node_id             TEXT NOT NULL,
  plan_rev            INTEGER NOT NULL,                          -- 最近一次涉及该节点的计划版本
  kind                TEXT NOT NULL CHECK (kind IN ('team_task','capability_call')),
  template_id         TEXT,
  capability          TEXT,
  status              TEXT NOT NULL CHECK (status IN ('PENDING','READY','RUNNING','REVIEWING','SUBMITTED','ACCEPTED',
                                                      'BLOCKED','FAILED','CANCELLED','SKIPPED','SUPERSEDED')),
  required            INTEGER NOT NULL DEFAULT 1 CHECK (required IN (0,1)),
  requirements_rev    INTEGER NOT NULL DEFAULT 1,
  applied_rev         INTEGER NOT NULL DEFAULT 0,
  review_config       TEXT,                                      -- JSON：{capability, max_rounds}
  review_round        INTEGER NOT NULL DEFAULT 0,
  accepted_with_issues INTEGER NOT NULL DEFAULT 0 CHECK (accepted_with_issues IN (0,1)),  -- 用户豁免了审核问题
  brief               TEXT,                                      -- JSON：task_brief.schema.json（team_task）
  capability_input    TEXT,                                      -- JSON（capability_call）
  team_id             TEXT,
  callee_request_id   TEXT,                                      -- capability_call：对方请求
  latest_submission   TEXT,                                      -- JSON：submission.schema.json
  latest_verdict      TEXT,                                      -- JSON：review_verdict.schema.json
  state_version       INTEGER NOT NULL DEFAULT 1,
  created_at          TEXT NOT NULL,
  updated_at          TEXT NOT NULL,
  UNIQUE (request_id, node_id)
);
CREATE INDEX tasks_request ON tasks(request_id, status);

CREATE TABLE task_deps (
  task_id     TEXT NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,
  depends_on  TEXT NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,
  PRIMARY KEY (task_id, depends_on)
);

CREATE TABLE change_orders (
  change_id        TEXT PRIMARY KEY,                             -- chg_<ULID>
  request_id       TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  task_id          TEXT NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,
  from_role        TEXT NOT NULL CHECK (from_role IN ('dispatcher','team_lead','runtime')),
  parent_change_id TEXT,
  target_rev       INTEGER NOT NULL,
  body             TEXT NOT NULL,                                -- JSON：change_order.schema.json
  acked_at         TEXT,                                         -- Worker ack_change 的时间（仅发给 Worker 的变更）
  created_at       TEXT NOT NULL
);
CREATE INDEX change_orders_task ON change_orders(task_id, created_at);

CREATE TABLE teams (
  team_id           TEXT PRIMARY KEY,                            -- team_<ULID>
  request_id        TEXT NOT NULL REFERENCES requests(request_id) ON DELETE CASCADE,
  task_id           TEXT NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,
  template_id       TEXT NOT NULL,
  status            TEXT NOT NULL CHECK (status IN ('STARTING','ACTIVE','DRAINING','STOPPED','CLEANED','CLEANUP_FAILED')),
  execution_epoch   INTEGER NOT NULL DEFAULT 1,
  paused_for_change INTEGER NOT NULL DEFAULT 0 CHECK (paused_for_change IN (0,1)),
  work_order        TEXT,                                        -- JSON：work_order.schema.json
  lead_config       TEXT NOT NULL,                               -- JSON：冻结的 EffectiveModelConfig
  worker_config     TEXT NOT NULL,
  workspace_path    TEXT NOT NULL,
  cgroup_path       TEXT,
  tool_calls_used   INTEGER NOT NULL DEFAULT 0,                  -- 当前 WorkOrder / ChangeOrder 版本内的计数
  internal_rework_rounds INTEGER NOT NULL DEFAULT 0,             -- 团队内退回次数（上限 3）
  state_version     INTEGER NOT NULL DEFAULT 1,
  created_at        TEXT NOT NULL,
  updated_at        TEXT NOT NULL
);
CREATE INDEX teams_request ON teams(request_id, status);

-- 事件日志：与状态变更同事务写入；command_id 唯一，保证重放不重复生效
CREATE TABLE events (
  seq           INTEGER PRIMARY KEY AUTOINCREMENT,
  event_id      TEXT NOT NULL UNIQUE,                            -- evt_<ULID>
  request_id    TEXT NOT NULL,                                   -- 不设外键：清理时按请求整体删除
  task_id       TEXT,
  team_id       TEXT,
  type          TEXT NOT NULL,                                   -- 见主规格 12.4
  actor         TEXT NOT NULL CHECK (actor IN ('runtime','root','dispatcher','team_lead','worker','user')),
  command_id    TEXT,
  causation_id  TEXT,
  payload       TEXT NOT NULL DEFAULT '{}',
  created_at    TEXT NOT NULL
);
CREATE UNIQUE INDEX events_command ON events(command_id) WHERE command_id IS NOT NULL;
CREATE INDEX events_request ON events(request_id, seq);

CREATE TABLE artifacts (
  artifact_id  TEXT PRIMARY KEY,                                 -- art_<ULID>
  request_id   TEXT NOT NULL,
  task_id      TEXT,
  kind         TEXT NOT NULL CHECK (kind IN ('input','candidate','accepted_output','review_report','experience_report',
                                             'trace_digest','delivery','other')),
  path         TEXT NOT NULL,                                    -- 相对 $WLS_DATA/artifacts/
  sha256       TEXT NOT NULL,
  size_bytes   INTEGER NOT NULL,
  created_at   TEXT NOT NULL
);
CREATE INDEX artifacts_request ON artifacts(request_id);

-- Worker 的工具操作
CREATE TABLE ops (
  op_id            TEXT PRIMARY KEY,                             -- op_<ULID>
  request_id       TEXT NOT NULL,
  team_id          TEXT NOT NULL,
  task_id          TEXT NOT NULL,
  tool_id          TEXT NOT NULL,
  args_sha256      TEXT NOT NULL,
  execution_epoch  INTEGER NOT NULL,
  applied_rev      INTEGER NOT NULL,
  side_effect      TEXT NOT NULL CHECK (side_effect IN ('none','workspace','external')),
  status           TEXT NOT NULL CHECK (status IN ('RUNNING','SUCCEEDED','FAILED','TIMED_OUT','CANCEL_REQUESTED','CANCELLED','UNKNOWN')),
  exit_code        INTEGER,
  error_signature  TEXT,                                         -- 归一化错误签名（记忆的报错触发用）
  output_dir       TEXT,                                         -- 相对团队 ops/ 目录
  started_at       TEXT NOT NULL,
  ended_at         TEXT
);
CREATE INDEX ops_team ON ops(team_id, started_at);

CREATE TABLE questions (
  question_id     TEXT PRIMARY KEY,                              -- q_<ULID>
  request_id      TEXT NOT NULL,
  task_id         TEXT,
  team_id         TEXT,
  asked_by        TEXT NOT NULL CHECK (asked_by IN ('worker','team_lead','dispatcher','runtime')),
  holder          TEXT NOT NULL CHECK (holder IN ('team_lead','dispatcher','user','answered')),
  body            TEXT NOT NULL,                                 -- JSON：question.schema.json（原文不改）
  created_at      TEXT NOT NULL,
  answered_at     TEXT
);
CREATE INDEX questions_open ON questions(request_id, holder);

-- Worker 执行中的随手笔记（经验提炼用；请求清理时删除）
CREATE TABLE notes (
  note_id     TEXT PRIMARY KEY,
  request_id  TEXT NOT NULL,
  team_id     TEXT NOT NULL,
  task_id     TEXT NOT NULL,
  kind        TEXT NOT NULL CHECK (kind IN ('tool_usage','pitfall','method','fact')),
  text        TEXT NOT NULL,
  evidence    TEXT,
  created_at  TEXT NOT NULL
);

CREATE TABLE llm_calls (
  call_id            TEXT PRIMARY KEY,                           -- call_<ULID>
  request_id         TEXT NOT NULL,
  team_id            TEXT,
  role               TEXT NOT NULL CHECK (role IN ('dispatcher','team_lead','worker','compactor','curator')),
  model_ref          TEXT NOT NULL,
  prefix_sha256      TEXT,                                       -- 上下文 ①② 的指纹
  input_tokens       INTEGER,                                    -- 未知记 NULL，不记 0
  cache_read_tokens  INTEGER,
  cache_write_tokens INTEGER,
  output_tokens      INTEGER,
  cost_micro_usd     INTEGER,
  latency_ms         INTEGER,
  status             TEXT NOT NULL CHECK (status IN ('ok','retried','error')),
  created_at         TEXT NOT NULL
);
CREATE INDEX llm_calls_request ON llm_calls(request_id);

-- 清理后保留的最小回执（默认 30 天）
CREATE TABLE receipts (
  request_id       TEXT PRIMARY KEY,
  idempotency_key  TEXT NOT NULL,
  content_sha256   TEXT NOT NULL,
  function_id      TEXT NOT NULL,
  project_id       TEXT NOT NULL,
  final_status     TEXT NOT NULL,
  delivery_path    TEXT,
  manifest_sha256  TEXT,
  usage_summary    TEXT NOT NULL,                                -- JSON：各角色 token、缓存读取、费用
  cleanup_status   TEXT NOT NULL,
  closed_at        TEXT NOT NULL,
  expires_at       TEXT NOT NULL
);
CREATE INDEX receipts_key ON receipts(idempotency_key);
CREATE INDEX receipts_expiry ON receipts(expires_at);

INSERT INTO schema_migrations(version, applied_at) VALUES (1, strftime('%Y-%m-%dT%H:%M:%SZ','now'));

-- =========================== 迁移 2：S3 记忆 ===========================
-- 记忆仓库（Git）是唯一事实来源，以下各表都可以由 `wls memory reindex` 重建（memory_feedback 除外）。

CREATE TABLE memory_meta (
  memory_id         TEXT PRIMARY KEY,                            -- exp-20261017-k3f9 / skill:<name>
  type              TEXT NOT NULL CHECK (type IN ('rule','skill','experience','fact')),
  status            TEXT NOT NULL CHECK (status IN ('draft','active','archived','blocked')),
  scope             TEXT NOT NULL,                               -- global | function:<id> | project:<id>
  proposed_scope    TEXT,
  title             TEXT NOT NULL,
  description       TEXT NOT NULL,
  outcome           TEXT CHECK (outcome IN ('success','failure','mixed')),
  intent            TEXT,
  tags              TEXT NOT NULL DEFAULT '[]',                  -- JSON 数组
  applies_to        TEXT NOT NULL DEFAULT '{}',                  -- JSON
  enforcement       TEXT CHECK (enforcement IN ('guidance','policy')),
  path              TEXT NOT NULL,                               -- 相对记忆仓库
  revision          TEXT NOT NULL,                               -- 最近一次修改的 Git commit
  content_sha256    TEXT NOT NULL,
  scripts_approved  INTEGER NOT NULL DEFAULT 0 CHECK (scripts_approved IN (0,1)),
  updated_at        TEXT NOT NULL
);
CREATE INDEX memory_meta_lookup ON memory_meta(status, type, scope);
CREATE INDEX memory_meta_intent ON memory_meta(intent);

CREATE TABLE memory_error_signatures (
  memory_id  TEXT NOT NULL REFERENCES memory_meta(memory_id) ON DELETE CASCADE,
  signature  TEXT NOT NULL,
  PRIMARY KEY (memory_id, signature)
);
CREATE INDEX memory_error_signatures_sig ON memory_error_signatures(signature);

-- 全文索引：优先用中文分词扩展（tokenize = 'simple'），加载不了时用 trigram（如下）
CREATE VIRTUAL TABLE memory_fts USING fts5(
  memory_id UNINDEXED,
  title, description, tags, body,
  tokenize = 'trigram'
);

-- 评价：每个请求对每条记忆最多一条（独立性），请求清理后仍保留
CREATE TABLE memory_feedback (
  feedback_id  TEXT PRIMARY KEY,
  memory_id    TEXT NOT NULL,
  revision     TEXT NOT NULL,
  request_id   TEXT NOT NULL,
  task_id      TEXT NOT NULL,
  loaded       INTEGER NOT NULL CHECK (loaded IN (0,1)),         -- 运行时核对 memory_loads 的结果
  verdict      TEXT NOT NULL CHECK (verdict IN ('worked','failed','irrelevant')),
  assessed_by  TEXT NOT NULL CHECK (assessed_by IN ('team_lead','dispatcher','user')),
  evidence     TEXT,                                             -- 自足的证据摘要
  source_key   TEXT NOT NULL,                                    -- = request_id
  created_at   TEXT NOT NULL,
  UNIQUE (memory_id, source_key)
);
CREATE INDEX memory_feedback_memory ON memory_feedback(memory_id);

-- 本次请求里哪些记忆被出示或加载过（请求清理时先生成 feedback，再删除）
CREATE TABLE memory_loads (
  request_id  TEXT NOT NULL,
  task_id     TEXT NOT NULL,
  agent_role  TEXT NOT NULL CHECK (agent_role IN ('dispatcher','team_lead','worker')),
  memory_id   TEXT NOT NULL,
  revision    TEXT NOT NULL,
  how         TEXT NOT NULL CHECK (how IN ('rule','card','recall','load','error_trigger')),
  loaded_at   TEXT NOT NULL,
  PRIMARY KEY (task_id, agent_role, memory_id, how)
);

-- 打分用的统计视图（只计 loaded = 1 的评价）
CREATE VIEW memory_stats AS
SELECT memory_id,
       SUM(CASE WHEN loaded = 1 AND verdict = 'worked' THEN 1 ELSE 0 END) AS successes,
       SUM(CASE WHEN loaded = 1 AND verdict = 'failed' THEN 1 ELSE 0 END) AS failures,
       COUNT(*) AS assessments
FROM memory_feedback
GROUP BY memory_id;

INSERT INTO schema_migrations(version, applied_at) VALUES (2, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
