# Changelog

## 0.8.3-r2a.1 — bounded directory creation

- Add Worker fs_mkdir for a single absent empty directory with an existing parent and fixed 0700 mode.
- Check projected entry/depth limits before creation; pin directory identities and retain child/parent synchronization evidence.
- Route directory effects through existing durable first-dispatch, accounting, exact replay, UNKNOWN and settlement-failure fences.
- Grant the new tool only through new frozen templates; keep the shared policy version unchanged and preserve existing requests’ bound policy hashes and grants.
- Add private syscall-fault and real CLI/SQLite regressions; keep PREPARED owned-allocation recovery and cleanup authority separate.

## 0.8.2-r2.3 — captured model AMEND decisions

- Let Dispatcher models publish an amended plan with explicit update/keep/cancel choices, then resolve omitted nodes on the same plan.
- Persist original AMEND targets and check request CAS plus affected task CAS/team epochs in the committing Store transaction.
- Keep omitted nodes pending, require fresh TeamLead instructions/Worker acknowledgments, and preserve UNKNOWN, usage and shared rework limits.
- Preserve host disposition paths, exact response replay and scoped sibling-request isolation without changing published SQL or frozen snapshots.
- Add C++/CLI regressions in both build systems and complete the missing R2.2 baseline validation record.
- FakeModel verifies runtime behavior only; live protocol/quality, strong deployment and R2a allocation recovery remain separate.

## 0.8.1-r2.2 — integrate request-scoped recovery with bounded rejection correction

- Preserve the original unpushed R2 correction commits and merge the request-recovery candidate.
- Apply scoped writer guards to corrective decisions, owner escalation/answers and original-result replay.
- Verify complete corrective chains and admission do not change sibling request rows, waits or terminal jobs.
- Reuse ADR-070 for correction; assign ADR-071 to recovery and ADR-072 to integration decisions.
- Keep one exclusive database writer and all UNKNOWN/AMEND/late-response/accounting fences.
- Model-produced AMEND dispositions, general task redefinition and deployment acceptance remain open.

## 0.7.6-r2.1 — bounded rejection correction and owner escalation

- Capture original failed acceptance/check and exact task/submission CAS for Dispatcher decisions.
- Preserve TaskBrief and previous instruction constraints; require a new TeamLead order and Worker ack.
- Share the frozen rework counter; persist at-most-once user escalation and exact wait/answer.
- Retain pending rejection wakeups across missing scripts, invalid responses and restart.
- Preserve UNKNOWN, budget, policy, AMEND and cancellation guards, original SQL/vendor bytes.
- Request-scoped recovery and model-produced AMEND dispositions remain separate work.

## 0.7.6-r2.1 recovery candidate — request-scoped writer recovery (integrated above)

- Add trusted-host OneRequest and AdmissionOnly scopes without changing the global exclusive writer lease.
- Scope CLI execution/control recovery to its target; new admission and exact replay preserve all existing requests.
- Preserve operation/late-response fences, immutable instruction projection, WAITING, AMEND, accounting and closeout jobs.
- Reject cross-scope writes, actorless settlements, scoped empty-database initialization and implicit populated upgrades.
- Keep the default all-request Store and explicit init maintenance compatible; no SQL or frozen-snapshot changes.


## 0.7.5-r1.3 — frozen Worker windows and original-evidence controls

- Add explicit structured-v2/window-v1 selection; preserve old frozen snapshots and legacy full-history behavior.
- Load bounded scoped recent attempts, retain whole tool pairs or explicit hash-pinned references, and disclose omitted history.
- Pin unresolved operations and missing/uncertain attempts outside the recent tail; never prune authoritative task contracts.
- Integrate history_page/history_read with both offline runners, normal call accounting, original feedback replay and persistent rate checks.
- Verify repeated large reads across restart, original evidence readback, policy/AMEND/UNKNOWN/wait fences and malformed configuration.
- Keep real transport, wire receipts, model-quality evaluation and semantic summarization separate.


## 0.7.4-r1.2 — frozen structured model input

- Add opt-in structured-v2 system sections, native tool schemas and validated ID-paired messages to the actual offline runtime.
- Bind canonical input and complete captured request separately; do not fabricate wire fingerprints.
- Preserve legacy-v1 snapshots/serialized frames and enforce frozen format at Store reservation.
- Preserve original attempt provenance across reused provider IDs, errors, missing feedback and reopening.
- Windowing/compaction, real transport and paid calls remain separate milestones.


## 0.7.3-r1.1 — bounded original-history evidence access

- Add actor-scoped, high-watermark attempt metadata pages without loading request
  envelopes or large feedback bodies. Preserve the original binding and missing-feedback state.
- Add hash-bound response/result byte reads, verifying both the feedback digest and
  its settled original attempt; return canonical JSON in base64 slices with full-section hashes.
- Expose read-only history-page/history-read CLI commands; retain legacy history semantics,
  original SQL/vendor bytes, epoch checks and every business readiness guard.
- Add 10 C++ and 7 actual CLI checks for 270-attempt pagination, interleaving, append races,
  actor isolation, immutable original bytes, tampering, UTF-8/binary content and no database effects.
- This is the R1 evidence prerequisite, not integrated context windowing or a real model adapter.
  Full-history Worker overflow and paid-call readiness remain explicitly open.

## 0.7.2-s1.6 — audit24 first runtime fixes

- Define ADR-064/065/066 and the second review disposition before runtime changes.
- Rebind unchanged instructions through immutable recovery-event projections; require
  a fresh Worker acknowledgment without another TeamLead attempt. Preserve quotas,
  waits, AMEND, original rows and stale-operation/model fences.
- Observe bounded complete pre/post workspace trees under the process lock; settle
  failed strong runs only with full namespace/network/cgroup/observation evidence.
- Add Root-only trusted-local-host reconcile-op with read-only preflight, final CAS,
  immutable evidence, exact replay and rollback on event failure.
- Keep long-context and escalation gaps explicit; capacity recovery is recorded below. No new provider calls,
  migration/vendor edits, weak isolation or claimed strong-deployment acceptance.


### 同批容量恢复（P1-2）

- system.limits.store_capacity冻结存储上限；旧快照缺块按旧常量解释，新准入做工具/团队/返工一致性校验。
- 独立业务/控制计数、必要控制预留、持久告警与准入暂停；Root CAS检查点续进，不清审计、不重置业务预算。
- 新增capacity/capacity-checkpoint、pending容量状态；事件计数缓存随回滚/外部数据版本变化失效。
- 原event_cap复现转成长流量回归，另加SQLite/CLI的窗口恢复、未知效果/费用、AMEND、配置冻结和故障注入。
- 真实模型、上下文压缩、否定验收返工、按请求租约及强环境正向不由本补丁外推为已完成。

## 0.7.1-s1.6 — Claude audit follow-up

- Fix two test JSON-reference lifetime defects; replace simple config regex format checks.
- Make private fixtures independent of ambient umask; restrict child-only umask; retain negative permission tests.
- Preserve mandatory Make includes with user CPPFLAGS and fix Clang sign conversion.
- Share exact decimal money parsing and catalog shape validation across CLI/Store; preflight wait/wake read-only.
- Report unenforced configuration controls; classify context compile capacity as quota without claiming compaction.
- Add ASan/Clang/permissive-umask CI and focused CLI regressions.
- Centralize status; prioritize structured model requests, recoverable history/events/rework and a controlled real loop.


## 0.7.0-s1.6 — 2026-10-21 (UTC)

- Add SQLite 407 / migration 009, durable runtime-owned workspace allocation and
  immutable identity/inventory/removal facts. Preserve all earlier migration bytes.
- Require frozen cleanup opt-in, exact success receipt, completed experience and
  accounting, settled operations and original hashed process-exit observations.
- Verify and sync existing delivery packages without recreating missing packages.
- Delete only bounded original inventories through Linux dirfds; reject links,
  special files, replacement generations, changed bytes and new inventory entries.
- Recover bounded passes, stale claims, SIGKILL and database failures after unlink.
- Add explicit allocation/query/cleanup CLI commands and real temporary-file tests.
- Fix non-root fault injection for missing/corrupt read-only delivery packages;
  restore original modes and verify the actual corruption before checking guards.
- Test that cleanup preserves every original delivery byte and read-only mode;
  require non-root identity in hosted CI, without weakening runtime permissions.
- Retain ownership envelopes, original packages and private database rows. Workspace
  completion is a phase checkpoint; global cleanup remains PENDING. S1–S5 are not
  complete. See [contract](docs/implementation/S1-owned-cleanup.md) and
  [release revalidation](docs/reports/s1-owned-cleanup-followup-validation.md).

## 0.6.0-s1.5 — 2026-10-20

Continue the v0.4.1 blueprint from the S1.4 immutable submission foundation.

- Add SQLite migration 008 / schema 406 while preserving earlier assets and
  original acceptance, experience source, package and receipt facts.
- Capture current candidates for explicit Dispatcher business acceptance;
  atomically create version-specific experience jobs and retain negative controls.
- Assemble bounded delivery packages from accepted original bytes, including
  requirements, plan, evidence, questions, usage and experience snapshots.
  Persist intent before atomic no-overwrite filesystem publication and recovery.
- Require exact receipt identity and freshly verified durable publication;
  recheck late blocking questions and retire sibling delivery intents atomically.
- Recover explicit TeamLead experience reports after business completion while
  retaining budgets, source hashes, job CAS and ordinary business fences.
- Add publication, verification, receipt, experience and closeout CLI commands,
  retained cleanup jobs, synchronized planning documents and actual evidence.
- Pass strict Make, UBSan and independent CMake/CTest gates: S1 C++ 263 tests,
  actual S1 CLI 55 tests, original M1 55 tests, reference contracts 36 tests,
  and 29 CTest groups; build modes are not additional product acceptance cases.

S1 is still in progress. Actual resource deletion, process-exit proof, private-row
purge, full daemon/API, real providers and independent review remain separate
increments. See [the implementation contract](docs/implementation/S1-acceptance-delivery.md)
and [the validation report](docs/reports/s1-acceptance-delivery-validation.md).

## 0.5.0-s1.4 — 2026-10-19

Continue the v0.4.1 blueprint from the S1.3 durable event runtime.

- Add SQLite migration 007 / schema 405 while preserving the earlier migration
  assets and original question, submission, check and model-call bindings.
- Route Worker questions through TeamLead and Dispatcher to an explicit user
  answer. Keep original text and options immutable, and end only the question's
  exact wait; unrelated tasks continue.
- Capture complete bounded workspace file bytes, hashes and sizes when a Worker
  submits. TeamLead views use the saved snapshot even after live files change.
- Add captured-target local checks, evidence consistency gates, frozen rework
  limits and new task/instruction revisions requiring a fresh Worker ack.
- Preserve current-policy control recovery, reject stale responses and keep
  pending work when scripts, calls, budgets or context capacity run out.
- Add read-only questions/submissions and explicit answer CLI commands, actual
  workflow fixtures, C++/CLI regression groups and synchronized planning docs.

TeamLead success is CANDIDATE_READY. Independent review, business acceptance,
delivery packages, receipts and recoverable cleanup remain separate increments.
See [the implementation contract](docs/implementation/S1-workflow-runtime.md)
and [the validation report](docs/reports/s1-workflow-runtime-validation.md).
The S1.1–S1.3 reports retain their original evidence and scope.

## 0.1.1-m1 — 2026-10-17

Reconcile the v0.4 review into the v0.4.1 target blueprint while continuing the
existing M1 implementation and preserving both original handoff packages.

- Document explicit AMEND task disposition, separate requirement/task/plan/
  instruction bindings, control-only guards, all-effect UNKNOWN reconciliation,
  review capacity, request-frozen configuration and recoverable cleanup.
- Make Worker instruction acknowledgment a bounded control transition that can
  progress at ordinary quota or with UNKNOWN effects without releasing the
  business execution barrier; reject fresh duplicate acknowledgments.
- Add owned Worker operation cancellation: prepared work becomes Cancelled,
  running work becomes Unknown until host reconciliation. Preserve journal
  format, exact-command retry semantics and settlement capacity.
- Invalidate an active candidate when its producer's TeamLead instructions
  change, while preserving artifact version high-water marks and historical
  review evidence.
- Add multi-team amendment/plan, cancellation durability, bounded control and
  saturated review/handoff regressions; clarify host-driven dependency waits.
- Add corrected JSON Schemas, examples, SQLite schema and Python transaction
  references for versioned memory, operation identity and recoverable cleanup.
- Include cross-contract and preserved-source checks in Make, CTest and CI.
  `jsonschema` is a test-only dependency; the C++ runtime remains independent.

S1 model/tool execution, the C++ SQLite Store, the memory runtime, daemon and
MCP remain planned integrations. Source and contract checks do not represent
real provider, sandbox, delivery or product-quality acceptance.

## 0.1.0-m1 — 2026-10-17

First executable implementation following the supplied v0.3 design handoff.

- Preserve all 33 original handoff files and add independent checksum verification.
- Add the C++20 four-role TaskEngine with scoped admission/command idempotency,
  explicit changes, independent versions and immutable candidate version history.
- Persist state and event envelopes atomically in a Linux checksummed journal;
  recover partial tails, reject complete corruption, and automatically fence
  old runtime epochs before accepting new commands after restart.
- Add bounded concurrent scheduling with resource ordering and review/handoff
  capacity, frozen Root model/reasoning resolution and explicit Worker grants.
- Model synthetic operations, unknown-outcome reconciliation, independent
  review, approval revocation, matching handoff receipts and separate cleanup state.
- Preserve settlement capacity when ordinary event/frame budgets are exhausted;
  reject oversized ordinary state before entering the journal append path.
- Add a two-request CLI demonstration, Make/CMake builds, regression tests,
  UBSan entry point, CI configuration, implementation contracts and roadmap.

This release is a synthetic kernel prototype. Real model/tool execution,
OS isolation, MCP/A2A, general workflows and memory remain later milestones.
