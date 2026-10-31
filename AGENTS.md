# WLS861 development contract

Current implementation: **0.11.1-r3.3c-audit**, implementation blueprint baseline **v0.4.1**, Store **410**.
The planning revision is [v0.5.0](docs/spec/v0.5.0/README.md). Its assigned
A0–A5 execution and B0–B6 follow-up boundaries are recorded in
[the current task dependencies](docs/spec/v0.5.0/10-authorized-cycle-and-followups.md). It does not upgrade the runtime, Store schema, frozen request
contracts or the completion status of planned capabilities. Preserve every
published file under `docs/spec/v0.4.1/`.

## Coordination, execution and delivery authority

These are development responsibilities, separate from the runtime's Root,
Dispatcher, TeamLead and Worker permission scopes.

- The main coordinating Agent owns overall architecture, blueprint decisions,
  task order and decomposition, independent review, defect adjudication and
  final acceptance. The local execution Agent implements the assigned scope;
  it must not approve its own design changes or start later task cards merely
  because they appear in the roadmap.
- The local execution Agent owns local source edits, builds, tests, sanitizers,
  available Linux/Windows machine validation, isolated Git branches, normal
  commits/pushes and evidence archiving. Routine reversible work already
  authorized for the current task proceeds without repeated permission requests.
  A proposed scope change must first be made concrete for coordinator review.
- An independent engineering reviewer examines actual source, evidence and
  failure conditions. The executor's own passing tests are execution evidence,
  not independent review or product acceptance. Another Agent's review records
  its identity, scope and limitations; it is not automatically a second-model
  evaluation or the coordinator's formal PR approval.
- Every important module records its original baseline, deterministic failing
  counterexample where a defect exists, minimal fix/design, independent code
  review, complete applicable regression, final full SHA, that SHA's remote CI,
  and actual publication state. For documentation-only work without a reproduced
  defect, record the counterexample as not applicable; never manufacture one or
  relabel a proposed negative test as an executed failure.
- Keep `IMPLEMENTATION_COMPLETE`, `LOCAL_VALIDATION_COMPLETE`, `PUSH_COMPLETE`,
  `REMOTE_CI_COMPLETE`, `PR_REVIEW_COMPLETE` and `MERGED` separate. A branch push,
  green CI or local reviewer finding no blocker does not imply formal PR review
  or merge. Formal acceptance requires the coordinator's recorded decision for
  the reviewed SHA; merge additionally requires authorization and actual GitHub
  merge/target-branch evidence. This task authorizes PR creation, not merging.

Use the project data disk for checkouts, builds, evidence and temporary test
databases. Check mounts, free space, worktrees, uncommitted changes and running
jobs before taking over. Set a task-specific `TMPDIR` on that disk for tests.
Fetch and record exact remote heads before publishing; preserve other worktrees
and branches. Do not hard-reset/clean away work, force-push or write directly to
main. Use a separate fix branch if a confirmed production defect is discovered
during a planning-only task. Stacked documentation PRs must name their base and
dependency; retarget/rebase only within an explicitly authorized integration.

Published SQL, historical blueprints/reports and original logs are immutable.
Use new versioned reports for corrections and later results, preserving failed,
timed-out and interrupted runs. Check input and binary identities, exit codes,
ordinary-user permissions and complete test termination. Do not commit build
outputs, temporary databases, real credentials or environment-secret files.
Reviewed text-only evidence archives may contain original commands/logs and
manifests, with member/hash and credential checks; never execute archived code
to inspect its evidence. Committed reports state their time scope; an external
post-publication receipt may record the final SHA/CI without a self-referential
follow-up commit.

The v0.5.0 roadmap requires native Windows support without WSL as a cross-stage
platform workstream. Current Linux behavior and security gates remain the
reference. Windows capabilities are planned until native build and machine
evidence exists; Job Objects alone are not a sandbox. Preserve handle identity,
reparse-point/ACL/SID boundaries, restricted-token/AppContainer and network
controls, process-tree cancellation, exit proof and owned cleanup. Missing
capabilities must refuse the affected operation, not silently weaken isolation.
Keep platform changes within the assigned slice; a native component does not
constitute a complete Windows runtime port or an accepted ordinary-user sandbox.

Concurrent writer leases, real supplier quality/cost acceptance, a complete native
Windows runtime and S2–S5 extensions remain separately assigned task cards. Existing
SQLite single-writer exclusion and actor sessions do not constitute those leases.
FakeModel/loopback tests do not authorize real provider calls. A real run needs
explicit approved endpoint/model, capability and billing provenance, bounded
budget and call authorization; this task grants no real or paid calls.

Release states, dependency rules and evidence requirements are specified in
[v0.5.0 governance](docs/spec/v0.5.0/09-release-governance.md).

## Assigned asynchronous, offline and platform slices

A3/ADR-092 keeps one owned Store and its original database lifecycle lock.
Only the constructing host thread accesses Store; model callbacks run outside
Store transactions and retain the captured Actor/session/request/task/version/
epoch. An in-memory queue is never permission: the original reservation and
transactional `claim_offline_call` must precede the new coordinator callback.
Replays cannot invoke that callback again. Preserve the synchronous API defaults;
the call-level offline coordinator is explicitly enabled and does not own an
automatic inbox pump, live transport or file/process executor.

A4/ADR-093 connects this boundary to M1 request/team fairness. Preserve both
management and Worker review capacities, bounded control and retained-result
storage, resource FIFO and original frozen maxima/reserves. Waiting for a user
can release an execution slot without releasing unknown budget or live resources.
Review scheduling categories are not independent business review verdicts.
Results retain original bindings; shutdown/exception paths join callbacks before
Store release, and evidence retry never reapplies a business effect.

The A2 acceptance package reuses the original R3 v3 ten slots and evaluator;
failed, unknown, canceled and unexecuted samples remain in the denominator.
Dry-run cannot resolve credentials, send or admit Store work. Real execution
requires externally configured user relay endpoint, exact model, protocol,
billing source, credential reference, budget and explicit call authorization;
there is no automatic fallback to an official endpoint.

`platform/native-foundation` is an isolated native config/Scheduler/resource
component. Windows Server hosted CI, Windows 11 ordinary-user NTFS acceptance,
and the full Store/filesystem/process/sandbox/cleanup runtime are distinct scopes.
Unsupported operations must keep failing before effects. Its four native tests
are separate from the Linux product CTest catalog.

Use the existing shared GCC gate and its reviewed test-name/deadline catalog.
The integrated collection is 92 registrations: original 89 plus the async suite
and two offline acceptance suites. Never accept a new current count without
reviewing names and deadlines. Update catalog, default, CI and selfchecks together.
Runner selfchecks and diagnostic probes are not product sanitizer results. Keep
historical Clang/live_workflow failures open until their own evidence closes them;
`historical_clang_asan_failures_closed=false` must not be edited into success.

## Existing implementation contracts

The whole-code review and minimal fixes are specified in ADR-090 and
`docs/reviews/code-audit-20261027.md`; actual results belong in
`docs/reports/code-audit-20261027-validation.md`. Preserve inherited writer
leases across child destruction, one read snapshot during ledger reconstruction,
effective five-dimensional settlement capacity, original pre-spawn cancellation
evidence, pinned type-before-read and EOF checks, and evaluation source identity
bound to the original frozen configuration and live admission. Preserve captured
C++ number spellings when reconstructing original facts without changing the
historical Python plan/report fingerprints. Never relabel a local fixture as
provider evidence or rewrite original nullable usage to release capacity.
Keep request restoration and canonical-history reuse local to one function
invocation. Public mutable-request APIs and every Store admission/send boundary
must retain full validation; prefix reuse must preserve aggregate array depth,
UTF-8, finite numbers and the exact original byte budget.
The preceding two-module continuation is specified in
`docs/implementation/two-module-billing-baseline.md`. Read ADR-088/089 and
`docs/implementation/R3-signed-billing.md` plus
`docs/implementation/R3-single-agent-baseline.md` before modifying these paths.
Billing is an explicit versioned trusted-gateway contract, not proof that a real
vendor supports it. Freeze the unique challenge opt-in, bind captured policy bytes
to v2 host authorization, verify independent Ed25519 receipts against the original
wire identity and settlement, and enforce one source owner inside the Store transaction.
Read all authorization, policy and receipt files only after full-Store workspace
path guards. Exact late reconciliation replay is read-only; unknown values remain
unknown, and accounting never restores business authority or sends again.
The single-Agent runner requires a frozen single-agent-role-broker-v1 policy,
one model/reasoning, complete cross-role original response/feedback history and
transactional continuity validation. Store authority roles remain separate scopes;
shared names or labels alone do not establish one Agent. R3 v3 independently checks
actual native input and original history. Preserve v1/v2 historical identity rules.
Actual results for this continuation belong in
`docs/reports/billing-baseline-validation.md`; earlier reports retain their source scope.

The preceding 0.10.0 two-module continuation is specified in
`docs/implementation/two-module-continuation.md`. Read ADR-085/086/087 and
`docs/implementation/R3-live-workflow.md` plus
`docs/implementation/runtime-resources-and-migrations.md` before modifying these paths.
The controlled live event pump uses original captured targets, Store reservations,
unique sends, original settlement and model feedback. CLI `pump-live` requires an
explicit request/config/profile-bound host authorization; `live-status` only returns
a disabled template. Defaults remain offline and no actual provider spending is
authorized by this development task. Tests use disposable SQLite/files and loopback
TLS with synthetic credentials. Unknown accounting stops the entire batch; the CLI
cannot import a generic JSON billing attestation. A trusted library evidence hook
must match the original attempt and settlement before any application or next call.
The versioned resource bundle captures verified SQL, Schema and common prompt bytes;
configuration and credentials remain external. Keep old Store constructor/API and
published SQL bytes. Resource discovery is executable-relative, never ambient cwd.
R3 v2 evaluation requires original model/target/feedback provenance. Preserve v1's
structural-only meaning and its existing single-Worker identity rule; do not rename
multiple Store Workers to claim a completed single-Agent baseline.
That earlier batch's results belong in `docs/reports/two-module-validation.md`.
Earlier reports retain their original source scope. Check final-SHA CI; implemented
interfaces and planned acceptance cases are not evidence of a completed live product.

Preserve the R3.3a transport contract: exact original encoding, explicit trust/route,
one monotonic deadline, bounded cancellation, private raw evidence and separate safe
summaries. Preserve original Store attempts and settlements, immutable unique send
claims, explicit host observations after response loss, and independently versioned
usage projections. Missing observations and zero-sized unknown reservations remain
unknown. Reconciliation cannot restore business authority, dispatch tools, erase
UNKNOWN, overwrite original usage or grant another send. All new attempts consume
original request budgets. Original fake settlements and old snapshot interpretation
remain compatible; populated upgrades need explicit all-request maintenance.

R1.4 only projects completed history under a frozen management policy. Current
contracts, questions, UNKNOWN, captured targets and CAS bindings stay complete.
Controls consume ordinary attempts and persistent query limits. New owned capacity
is file-tools-only and must cover temporary stages and inventory encoding; it never
expands deletion authority or silently narrows existing requests. A retained oversized
legacy tree is an explicit capacity exit, not a completed cleanup.

Read ADR-079 for this follow-up: inspect runtime sources and shared configuration/input files before a read-mode open,
prove EOF within actual read budgets before reporting complete file identity,
preserve Workspace-accepted UTF-8 names through owned cleanup without relaxing
resource identifiers or deletion authority, keep default SSE obfuscation outside
semantic output, and validate CLI argv before Store effects with bounded error
reporting. The original R3.3b/c and long-task prerequisites are specified in
`docs/implementation/R3-controlled-live-plan.md`; transport alone cannot open a
live path. Additional usage evidence must not rewrite original settlements or
restore old business authority. CLI can serve S1 acceptance; daemon/API stay later.

Read `docs/reviews/claude-2026-10-24-disposition.md`,
`docs/adr/064-audit24-recovery.md`, `docs/implementation/S1-epoch-recovery.md`
and `docs/implementation/S1-process-settlement.md` for the current recovery/settlement
contracts. Original instructions stay immutable; a recovery event authorizes only
an epoch projection. A definite run failure requires complete isolation, network,
exit and pre/post workspace observations. Root reconciliation preserves original
executor evidence and never substitutes for cleanup exit proof. Frozen capacity
windows and their tested compatibility/settlement reserve are defined in
`docs/implementation/S1-capacity-windows.md`. For the R1 evidence prerequisite,
read `docs/adr/067-paged-history.md` and `docs/implementation/R1-history-evidence.md`.
Do not mistake paginated host reads for integrated model context windowing.
Read `docs/adr/068-structured-model-input.md` and
`docs/implementation/R1-structured-context.md` for frozen v1/v2 selection, logical
input/request fingerprints and native tool-turn provenance. Missing old policy
means framed-v1; never rewrite old snapshots or fabricate wire receipts.
For R1.3 read ADR-069 and docs/implementation/R1-worker-history-window.md.
Missing worker_history retains full history. Only explicitly frozen window-v1
may project whole old/large turns into references; current contracts and unresolved
facts must remain complete. Readbacks require current scope/policy, normal model
accounting and immutable feedback. Never substitute a present file for old evidence.

For R2.1 read `docs/adr/070-rejected-task-resolution.md` and
`docs/implementation/R2-rejected-task-resolution.md`. Only a captured current
rejected submission may be corrected or escalated. Preserve original task and
instruction constraints, share the frozen TeamLead rework counter, and never
consume an unanswered correction wakeup as handled. User answers do not grant
more rework, accept a candidate, or clear UNKNOWN. Scoped recovery applies to
these management paths too. Model AMEND uses the separate ADR-073 contract.

Read `docs/adr/071-request-scoped-recovery.md` and
`docs/implementation/S1-request-recovery.md` for scoped writer recovery. CLI
request writes take over only their target; admission-only cannot execute. The
database writer lock remains exclusive. Recover a selected request before issuing
its actors; preserve all unknown/late/AMEND/wait and post-terminal job fences.
Populated structural upgrades require explicit all-request init maintenance.
Read ADR-072 for the integration: corrective commands and exact replay obey the
same scope guards; owner answers cannot perturb sibling requests or clear fences.
R2.4 refines OneRequest with ADR-091 and `docs/implementation/R2-demand-driven-recovery.md`:
decide recovery within the original writer transaction, preserve idle epochs,
and check the issuing Store session even before exact replay. AllRequests and
explicit recovery remain forced; ephemeral sessions never enter original actor
fingerprints. Preserve genuine in-flight, UNKNOWN, cleanup CAS and exit guards.

Read `docs/adr/073-model-amendment-dispositions.md` and
`docs/implementation/R2-model-amendments.md` for model-produced AMEND decisions.
Capture target tasks before a Dispatcher call; both request CAS and affected
task CAS/team epochs must match inside the committing transaction. Missing
decisions remain paused and pending. A same-plan follow-up must not revise or
fence already-current siblings. Keep the explicit host disposition path and
original response/usage replay behavior compatible.

Read `docs/adr/074-bounded-directory-tool.md` and
`docs/implementation/R2a-directory-tool.md` for fs_mkdir. Create only one absent
0700 leaf under an existing parent; validate projected traversal depth/entry
capacity before mkdir. Preserve the first-dispatch/UNKNOWN/replay boundary and
old frozen grants/execution policies. Business mkdir does not grant runtime-owned
allocation or cleanup authority; PREPARED allocation recovery is a separate slice.

Read `docs/adr/075-owned-allocation-recovery.md` and
`docs/implementation/R2a-allocation-recovery.md` for Store 408 allocation.
Persist v2 staging receipt before no-overwrite publication; validate original
filesystem generations and resource CAS on recovery. Abandonment is metadata
only for unbound PREPARED records, including terminal/retired/held requests.
Preserve all history, protect both retained paths, select active cleanup targets,
and keep lifetime limits separate from windowed event settlement reserves.
Populated upgrades need explicit all-request init; never edit published SQL.

Read `docs/adr/076-doctor-deployment-diagnostics.md` and
`docs/implementation/R2a-deployment-diagnostics.md` for R2a.3. Doctor uses current
explicit configuration, not an existing request snapshot. Preserve input origins,
typed reasons, per-object observations, shared read budgets and explicit truncation.
It never opens Store, executes launcher/environment checks, writes cgroup controls
or adopts allocations. A future operation control path is only a not_checked
pattern. Complete/readiness flags do not replace real isolation or exit evidence;
keep all original production path, permission, frozen-byte and strong-only guards.

Read `docs/adr/077-responses-offline-codec.md`,
`docs/adr/078-responses-sse-replay.md` and
`docs/implementation/R3-offline-responses.md` for R3.1/R3.2. Provider profiles,
HTTP coding and bounded SSE replay are offline. The independent encode-model and
replay-model CLI commands only read local fixtures: no Store, network, credential
resolution, paid attempts or tool dispatch. Keep real_calls_enabled=false and
max_paid_retries=0. Preserve the original structured-v2 host tuple, raw bytes,
separate local/wire fingerprints and nullable usage; never retag a FakeModel
reservation as real protocol evidence. Schemas and decoded Completed do not grant
business authority. Partial streams, cancellation, deadline and protocol errors
must not release partial tools. R3.3a/b now add transport and original Store integration. Approved real small tasks
and provider quality/cost acceptance remain open; the codec itself grants no authority.

Read `docs/STATUS.md` and `docs/reviews/claude-disposition.md` for the current
review fixes and priority changes. Model contract/context/rework and an approved
small real loop now precede expanding private-data purge or daemon/API. Keep
paid calls disabled until the endpoint, capabilities and budget are configured.
New code must preserve ordinary-user, permissive-umask, ASan/UBSan and Clang
regressions; private fixtures do not relax production workspace permissions.


Read the current blueprint at `docs/spec/v0.4.1/01_实施规格_v0.4.1.md`,
`docs/ROADMAP.md`, and `docs/implementation/M1-contract.md` before changing
runtime behavior. Also read `docs/implementation/S1-foundation.md` for the new
foundation, and `docs/implementation/S1-worker-tools.md` for the file/process boundary,
then `docs/implementation/S1-event-runtime.md` for the event foundation and
`docs/implementation/S1-workflow-runtime.md` for the submission foundation,
then `docs/implementation/S1-acceptance-delivery.md` for acceptance and
`docs/implementation/S1-owned-cleanup.md` for the current 0.7.0-s1.6 library/CLI. The blueprint defines the target; implementation
contracts and actual reports define what this checkout can execute. M1 remains
its original kernel; S1.2 adds bounded real files and a strong-only process
entry point; S1.3 adds a bounded durable event pump, isolated independent tasks,
explicit waits and original cross-run feedback. S1.4 adds Worker questions,
upward routing/answers, immutable bounded submission bytes and local TeamLead
checking/rework. S1.5 adds explicit Dispatcher acceptance, immutable local
delivery, exact receipts and recoverable experience records. Actual
private-row purge, a completed daemon and positive deployment sandbox acceptance remain
separate task cards. S1.6 only deletes explicitly allocated runtime-owned workspaces
after the exact success-receipt, experience, accounting and original process-exit gates.

The owner approved review corrections, documentation/code updates and GitHub
push on 2026-10-17, then explicitly requested development under the new plan.
Implement routine development, corrections and fixes within that
scope without requiring a new approval for each task card. Record decisions
and changes; do not infer model spending or deployment credentials from this
development authorization. Explicit user instructions take precedence.

- Keep the core in C++20 and isolate Linux platform code. Keep provider
  endpoints, credentials, model aliases and budgets in external configuration.
  Tests use FakeModel and self-created temporary fixtures, including real
  file effects. Do not call real models or operate production data/services.
- Preserve Root -> Dispatcher -> TeamLead -> Worker. Only Workers execute
  business tools. Each independent request has its own fenced Dispatcher.
- Keep request requirements, task revisions, plans, TeamLead instructions,
  applied versions, state versions, and execution epochs distinct. Check their
  bindings at state commit and operation dispatch; a model response is not an
  authorization. Unhandled AMEND tasks remain blocked.
- Control acknowledgments, cancellation, queries and outcome reconciliation
  have their own guards. Do not put the business readiness gate on the control
  action needed to restore readiness. Preserve bounded settlement capacity.
- Unknown non-idempotent outcomes require reconciliation, including effects
  inside a workspace. Never infer that cancellation undid an effect.
- Real tools must obtain a durable first-dispatch claim. Command replay is
  never execution permission. Persist original operation facts and raw output;
  a settlement failure must stop new business, even if storage is unavailable.
- Consume only the explicitly completed IDs from a captured event claim.
  Omitted AMEND subjects, errors and exhausted batches retain pending work.
  Worker waits are Store dispatch gates; waking cannot clear AMEND, applied
  versions or UNKNOWN. A takeover releases claims without executing them.
- Preserve every original attempt/feedback binding when rebuilding history.
  Missing observations stay explicit; never reread a file to invent old output
  or recursively embed prior compiled contexts. Context overflow blocks before
  another reservation. Independent plan allocation is one bounded transaction.
- Capture each question/submission target and its version before the management
  model call. Preserve original question text, file bytes and bindings; forwarding
  only appends notes. An answer ends only its own exact wait, and neither ordinary
  wake nor automatic instruction reissue may bypass pending workflow state.
- Worker submissions require current grants, complete captured file bytes and
  settled operations. TeamLead checking uses the original immutable snapshot;
  contradictory self-check/evidence cannot pass. Rework is bounded by the frozen
  team limit, increments task/instruction revisions and requires a new ack.
  CANDIDATE_READY is not independent review, business acceptance or delivery.
- Dispatcher acceptance captures one current candidate with its task/submission
  CAS and complete original evidence. Root only assembles already accepted bytes
  and handles technical publication/receipts. Required independent review cannot
  be replaced by a local check or a model manifest.
- Persist exact delivery intent before filesystem publication. Verify every
  original byte, file set and pinned root identity; atomically publish without
  overwrite, then commit the Store reference. Inspecting is not acknowledgment.
  Receipt identity includes package/hash/versions/recipient/method/key.
- Acceptance creates an original-source experience job atomically. Terminal
  requests may finish only specifically captured experience management calls;
  ordinary business remains forbidden. Missing reports/budget stay pending.
  Retained cleanup jobs are not proof of process exit or completed deletion.
  S1.6 has an opt-in owned-workspace phase, not a private-row purge runner.
- Persist ownership intent before mkdir and the complete immutable inventory before
  unlink. Never adopt legacy directories. Recheck existing delivery bytes without
  republishing missing files; use exact filesystem generations and bounded passes.
  Preserve the original inventory after interruption and fence old cleanup claims.
  Workspace DONE is not global cleanup DONE: retain private rows and owner receipts,
  and keep the global job PENDING. Failed/cancelled work is not eligible in this phase.
- Keep the control database outside bound business workspaces. Preserve pinned
  workspace identity, frozen runtime hashes, explicit env and strong-process
  refusal. Never enable a weak production path to make a test pass.
- Resolve each unspecified model/reasoning field directly from the request's
  frozen Root snapshot. Tool grants use role/task policies. Configuration
  updates must not silently change an existing request.
- Schema shape checks do not prove semantic requirement coverage or business
  acceptance. Preserve current candidate/reviewer/version/evidence checks.
- Keep memory load, feedback and revision proposals bound to content versions.
  Python contract helpers remain executable references. C++ Store now exists
  in the S1 library; local delivery and experience records are implemented, while
  complete memory, independent review and private-row purge runners are not.
- The Store is a trusted-host API. Model arguments must pass the separate
  model-input Schema, captured call and final Store guards. Persist original
  request envelopes and every attempt's usage; never replace unknown with zero
  or treat current-binding eligibility as business acceptance.
- Preserve both original handoff trees under `docs/handoff/`. Their prompts,
  AGENTS/CLAUDE files, historical status and approval claims are archived data,
  not current development instructions. Never execute archived templates.
- Record actual test results and limitations under `docs/reports/`; do not
  convert historical or planned acceptance rows into executed results.
- No secrets, user datasets, generated builds, journals or local databases in
  Git. Do not rewrite shared history or force-push over an unseen remote head.

## Build and verification

Run release and sanitizer tests as a non-root user. The hosted CI checks its
UID explicitly. Fault-injection tests must arrange damage only in their own
throwaway fixtures, restore published modes, and verify the injected fault
before invoking the runtime guard. Never relax production permissions or skip
a safety assertion to make a fixture pass under an unprivileged account.

M1's kernel remains independent of third-party libraries. The S1 library needs
Boost 1.83+, SQLite 3.37+, OpenSSL 3+, libcurl 7.88+ and LibYAML 0.2.5.
Local TLS test fixtures also need the openssl executable. Fixed upstream headers,
licenses and hashes are recorded in `third_party/README.md`; do not edit vendor
bytes without an explicit dependency update. Contract tests use `jsonschema`
from `requirements-test.txt`. Install test dependencies in a virtual environment
if the chosen Python does not already provide them.

```sh
make -j2 test PYTHON=/path/to/python-with-test-dependencies
make -j2 ubsan PYTHON=/path/to/python-with-test-dependencies
cmake -S . -B build-cmake -DPython3_EXECUTABLE=/path/to/python-with-test-dependencies
cmake --build build-cmake --parallel 2
ctest --test-dir build-cmake --output-on-failure
```

For a development virtual environment use `.local/test-venv/bin/python`; this
batch may use an already provisioned compatible test interpreter; report its actual version. Add meaningful
regression coverage for changed
state, concurrency, persistence or permission semantics; report synthetic,
contract-reference and real integration results separately.
