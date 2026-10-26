# WLS861 development contract

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
Boost 1.83+, SQLite 3.37+, OpenSSL 3+ and LibYAML 0.2.5. Fixed upstream headers,
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
