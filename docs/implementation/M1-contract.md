# M1 — executable kernel contract

Status: executable **0.1.1-m1** contract, updated 2026-10-17. This is a
**Linux, single-host, synthetic execution prototype**. The target blueprint is
[v0.4.1](../spec/v0.4.1/01_实施规格_v0.4.1.md); its SQLite, role-runner, tool,
memory and deployment choices are future integrations. This contract describes
the current C++ implementation, not a completed S1 daemon or a permission to
perform real model calls, knowledge publication or user-data cleanup.

## Scope and sources

The four roles and request semantics follow v0.3 sections 03–06 and 11–12,
the handoff implementation batches, and v0.2 sections 09–11. Complete original
documents remain unchanged under `../handoff/v0.3/`; the later audit source is
preserved under `../handoff/v0.4/`. The owner authorized its review corrections,
documentation/code changes and GitHub push. M1 implements request
admission, durable state/events, explicit changes, fenced dispatch, simulated
operations, version-bound acceptance/delivery, and resource state. S1–S5
continue from this kernel, as mapped in [the roadmap](../ROADMAP.md). There are no LLM calls, shell tool, MCP/A2A adapters,
memory retrieval, production receipt, or actual workspace deletion in M1.

## Host trust boundary

`Actor` is a trusted-host context, not a network credential. The kernel checks
its role and persisted root/request/team/principal binding on each applicable
mutation and dispatch. External authentication and credential issuance belong
to future adapters; exposing the C++ API directly to untrusted arguments is
unsupported. Runtime-only recovery/resource reconciliation APIs must not be
registered as agent business tools. There are four Agent roles; the runtime
is a host component, not a fifth Agent.

## Persistence and concurrency

M1 uses a small versioned, checksummed, append-only journal, not a general
database. Each frame is one atomic logical transaction. It is synced before
success is returned; a restart replays only complete verified frames. An
incomplete final frame can be truncated during recovery; a corrupt complete
frame fails closed. An exclusive OS file lock prevents two runtime instances
from writing the same journal. Linux file handling is kept in a platform
translation unit. No network or model execution occurs under a state lock.

The journal is intended for bounded M1 fixtures and retains history until the
caller removes its fixture directory. It has no compaction, database query,
production retention, backup, encryption, or media-erasure guarantees. Do not
store real user tasks here until production data policy/storage work is done.

Request state commits are short and serialized by the kernel mutex; execution
callbacks run in parallel outside it. This is a single-host prototype, not a
distributed scheduler. Queues and dispatch slots have explicit finite limits.
The separate scheduler reserves capacity for review/handoff work and serializes
callbacks sharing a resource key while allowing independent keys to progress.

## Identity and distinct versions

- Each NEW request receives one stable Dispatcher identity and independent
  teams. A team is exactly one TeamLead plus one Worker; Workers cannot spawn.
- Admission idempotency is scoped to tenant, project and Root. Reuse of a key
  with different immutable input is an error. Retries after AMEND still match
  the original admission input.
- `requirements_revision` changes with AMEND, `plan_revision` with a new
  plan, `applied_revision` only after the Worker acknowledges the new plan and
  requirements, `state_version` on each successful state mutation, and
  `lease_epoch` when the runtime fences a Dispatcher or cancels/replaces work.
- Mutations check expected state version. Dispatcher/team results also check
  lease and required revision. A stale result is rejected without overwriting
  live state; the original committed history remains in the journal.
- Construction after replay automatically fences every nonterminal request before the API becomes available. Explicit host recovery of an active request also increments its epoch and retires former team
  execution bindings. In-flight simulated operations become UNKNOWN and must
  be reconciled; the kernel never automatically retries a side effect.

## Commands and operation semantics

NEW creates a request. AMEND routes to the existing request/Dispatcher, clears
obsolete acceptance and prevents new operations until a current plan is
applied. REPLACE atomically links a new request to the old one and fences the
old one; it is not an alias of NEW. CANCEL fences old execution and enters
resource draining; it does not assert that in-flight side effects disappeared.

Management commands are role-bound. Business operation preparation is Worker
only and checks current requirements, plan/application, lease, task status,
explicit request tool grants, and operation idempotency. An operation key can
be returned on an exact retry but may not be reused with different content.
UNKNOWN outcomes block new operations in that team until runtime reconciliation.
Permission is checked again before dispatch; a prepared record is not proof
that an operation executed. M1 only changes simulation state.

## Acceptance, delivery and resources

Candidate identity/version/hash and current requirements bind the acceptance
record. Request-scoped artifact version high-water marks survive candidate switches
and amendments. Reviewer instruction versions also bind review evidence. An updated candidate invalidates an older approval. Before delivery completes, a
current negative review also revokes approval, even at the ordinary event budget. A valid review
report saying changes are needed is distinct from product acceptance. M1's
synthetic acceptance uses all separately assigned review teams and nonempty
evidence references; substantive review and workflow orchestration remain S2.
An accepted instruction change for the active candidate's producer clears the
current candidate and acceptance before the new instruction can be applied.
Historical review records and artifact version high-water marks remain intact,
so old content cannot be reintroduced under a reused version. Direct producer
instruction changes in Accepted/HandoffPending are rejected by the active-state
guard; this API does not reopen those states. The review-team instruction path
has its separate transition back to Verifying.

Business completion requires accepted current candidate and a matching,
explicit synthetic delivery receipt from the owning Root. Missing/mismatched
receipts keep the unique candidate reachable. Resource states are independent:
ACTIVE → DRAINING → STOPPED → CLEANING → CLOSED or CLEANUP_FAILED. Runtime
reconciliation must resolve pending operations before STOPPED. Cleanup failure
does not rewrite successful business completion. No M1 transition kills a real
process or removes a user directory.

## Model and policy configuration

Model and reasoning fields resolve independently against the **request's
frozen Root defaults**, never through Dispatcher or TeamLead overrides.
Reasoning inherit, disabled, provider default and explicit effort are distinct.
The capability registry rejects unsupported values rather than silently
mapping them. Test models are symbolic; there is no provider endpoint.

Tool grants are the intersection of role capability, system allowlist and
request allowlist. Model configuration does not grant tool permissions. M1
does not freeze a production model configuration file format.

## Verification boundary

Tests use generated temporary directories, symbolic models and deterministic
callbacks. Map executed tests to the handoff matrix only with partial/full
scope stated. Do not edit its original “未执行” cells. Build/runtime results,
environment, commands, outstanding risks and next batch belong in a separate
report. A passing M1 test is not proof of M2 isolation, M3 full workflows,
M4 memory behavior, or real-world delivery/deletion.

## Admission and settlement capacity

`max_events` bounds ordinary state changes; rejection diagnostics have a separate
same-size cap and stop being recorded after that cap (rejection still applies).
Acknowledgment of a genuinely pending instruction, operation settlement,
reconciliation, operation/request cancellation, completed review acceptance,
matching delivery, one-time adverse-review revocation, restart fencing, and host
resource cleanup remain writable. New candidates and ordinary reviews consume
the ordinary budget; they cannot bypass it by repeatedly using fresh command IDs.

Task transactions have a configurable 128 KiB–16 MiB bound (default 16 MiB).
Ordinary transactions use at most half and additionally reserve 64 KiB per
updated request plus 8 KiB per unresolved operation for bounded settlement.
Evidence, package and receipt references are at most 512 bytes; admitted
operations are bounded (default 256, configurable up to 1,024). Deterministic
size rejection occurs before append and does not poison the live journal.
This is admission protection for ordinary work and normal settlement, not an
unlimited-storage guarantee for repeated host recovery or cleanup retries.

A plan records the requirements revision it addresses. Each TeamLead instruction
update has its own version and fences that team's pending operations; the Worker
must acknowledge the current instruction version before continuing. Requirements
and plan changes clear the active candidate while preserving immutable version
history and past evidence.


## v0.4.1 control operations and execution barriers

`acknowledge_applied` verifies the current Worker identity, request/team binding,
CAS, epoch and current TeamLead instructions. It does not require the applied
version it is about to advance, and it may acknowledge while an operation is
UNKNOWN. Acknowledgment never settles that UNKNOWN: prepare/start remain blocked
by unknown outcomes in their own team, and candidate submission requires all
request operations to be settled. A fresh command ID
cannot acknowledge already-applied instructions again. Only an actual pending
instruction consumes the reserved control path; an exact command retry retains
normal durable command-id idempotency.

`cancel_operation` is a Worker control API for an operation in its own team.
A prepared operation becomes Cancelled; a running operation becomes Unknown.
It checks current request/CAS/epoch and ownership without requiring business
readiness. An already Unknown or terminal operation cannot repeatedly generate
new cancellation events under fresh command IDs. Exact retries remain
idempotent, and only the host reconciliation API may settle Unknown. This API
changes synthetic state; a future executor must separately request actual
process cancellation and then reconcile the effect.

AMEND remains a global request barrier in M1: an omitted second team cannot
resume merely because another team has applied the new plan. Each team must
apply current instructions bound to current requirements and plan. The target
S1 DAG's explicit update/keep/cancel disposition adds per-task scope while
preserving this fail-closed property; it is not implemented by the M1 string
plan API.

The SQL/Schema protocol uses explicit `*_revision` names and task/instruction
bindings; M1 keeps its existing public fields and Journal layout. In particular,
`applied_revision` is M1's applied request requirement, `instructions_version`
and `applied_instructions_version` are independent instruction counters, and
`lease_epoch` fences execution. Future Store adapters must map and validate
these semantics rather than rename serialized bytes in place.
