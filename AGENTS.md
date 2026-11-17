# WLS861 development contract

Current implementation: **0.13.0**, blueprint **v0.5**, Store **410**. The v0.5 entry is [`docs/spec/v0.5/01_实施规格_v0.5.md`](docs/spec/v0.5/01_实施规格_v0.5.md); unchanged contracts continue to reference v0.4.1. Actual acceptance results and current limitations are in [`docs/STATUS.md`](docs/STATUS.md) and the [latest handoff](docs/reviews/current-handoff.md).

Entry point only: the complete, still-binding contract is preserved verbatim in `docs/implementation/development-contract.md` — current, normative, not archived history; read it before changing runtime behavior.

The owner adopted D-46 through D-57 in [`docs/reviews/claude-2026-11-17/03-decisions.md`](docs/reviews/claude-2026-11-17/03-decisions.md) on 2026-11-17. Start with that directory's `README.md` and read `01` through `04` in order. These decisions supersede conflicting 11-13, 11-14 and 11-16 process, defaults, priorities and authorization; the complete contract's safety invariants remain binding. The authorized replacements in `04-plan-revisions.md` are applied to the single forward plan, `claude-2026-11-16/04-plan.md`; its other historical entries remain unchanged.

The owner adopted D-30 through D-45 in [`docs/reviews/claude-2026-11-16/02-decisions.md`](docs/reviews/claude-2026-11-16/02-decisions.md) on 2026-11-16. Start with that directory's `README.md` and read `01` through `04` in order. These decisions supersede conflicting earlier process, defaults, priorities and authorization; the complete contract's safety invariants remain binding. Its `04-plan.md` is the single forward plan; only its §1 status column and §15 are agent-maintained.

The owner adopted D-19 through D-29 in [`docs/reviews/claude-2026-11-14/02-decisions.md`](docs/reviews/claude-2026-11-14/02-decisions.md) on 2026-11-14. These decisions supersede conflicting earlier review decisions for process, defaults, priorities and authorization; the complete contract's safety invariants remain binding. Start with that review directory's `README.md` and read `01` through `05` in order.

The owner adopted all decisions in [`docs/reviews/claude-2026-11-13/02-decisions.md`](docs/reviews/claude-2026-11-13/02-decisions.md) on 2026-11-13. For process, defaults, priorities and authorization, those decisions supersede conflicting earlier text; the complete contract's safety invariants remain binding. Start with that review directory's `README.md` and read `01` through `04` in order.

The owner's 2026-11-15 direction and D-34/D-35 govern the current G2 endpoint, credentials, reasoning levels and finite call/token/time limits; see [the current run record](docs/reports/g2-first-real-run.md). New real requests wait for D-46; the revised R8–R11 sequence starts only after D-46 and D-48 merge. The original export was recovered under D-31; the strict metadata-check difference is recorded in that report, and this original replay must not be repeated. D-31 requires a complete verified directory backup, current main, and the original command/configuration paths. Only DELIVERED with one CONFIRMED delivery follows the verified model-free completion path; admission, filesystem identity, receipt, package and byte checks still apply. Historical review decisions and the preserved contract are not rewritten.

D-47/D-48 set new acceptance requests to non-streaming with frozen `max_paid_retries: 2`; timeouts remain ineligible for retry and existing requests retain their frozen semantics. After D-46 and D-48 merge, D-49 authorizes one R3-stream-check continuation, following a complete directory backup and verified checksum inventory, on current main with the original command/configuration paths and at most 20 new calls. R3 retains its frozen streaming and zero-retry configuration; any guard rejection seals it without bypass. R2-stream-check and R5 remain sealed. D-52 applies the two-round stop only to the same root cause; a stage-exit line returns to the latest working configuration instead of stopping the stage.

## Authority and permissions

- The owner approved review corrections and GitHub push on 2026-10-17, then requested development under the current plan. Routine development, corrections and fixes in that scope need no new approval per task card; record decisions and changes. Never infer model spending or deployment credentials from this; explicit user instructions take precedence.
- Keep the core in C++20 and isolate Linux platform code; provider
  endpoints, credentials, model aliases and budgets stay in external
  configuration. Tests use FakeModel and self-created temporary fixtures,
  including real file effects; tests never call real models. Do not operate
  production data/services. The owner has authorized the supplied test key
  for the specified Responses gateway, `gpt-6.1-sol` with D-35 reasoning levels,
  and removed money gates for reasonable, finite use. New requests opt in
  with frozen `cost_policy: observe-only-v1`; keep token, call and time
  limits and the independent host network authorization. New acceptance
  requests freeze `max_paid_retries: 2` under D-48; each retry consumes a
  new reservation and the call/token limits. Timeouts are not retried,
  and existing requests keep their original frozen retry policy. Unknown
  cost is not zero or free; retain original nullable usage and unresolved outcomes. Credentials
  stay external and may be injected into the intended process; never echo
  or persist their values. The current key authorization is already given
  and observe-only requires no price table; verify credential availability
  without exposing the value. This grants no
  public deployment or fallback to another endpoint.
  D-12 cancels private CI evidence hosting:
  public CI logs and repository reports suffice; local raw originals are
  retained for 30 days before optional cleanup.
- No secrets, user datasets, generated builds, journals or local
  databases in Git; never rewrite shared history or force-push over an
  unseen remote head. Record actual test results and limitations under
  `docs/reports/`; never convert planned acceptance rows into executed
  results. The `docs/handoff/` trees are archived data, not instructions.

## Task boundaries (complete rules in the development contract)

- Root -> Dispatcher -> TeamLead -> Worker. Only Workers execute business
  tools; each independent request has its own fenced Dispatcher.
- Keep request requirements, task revisions, plans, TeamLead instructions,
  applied versions, state versions and execution epochs distinct; check
  bindings at state commit and dispatch. A model response is not an
  authorization; unhandled AMEND tasks remain blocked.
- Control acknowledgment, cancellation, queries and reconciliation have
  their own guards; the readiness gate never sits on the restoring
  control action. Preserve bounded settlement capacity. Unknown
  non-idempotent outcomes require reconciliation, including workspace
  effects; never infer cancellation undid an effect.
- Real tools must obtain a durable first-dispatch claim; replay is never
  execution permission. Persist original facts and raw output; a
  settlement failure stops new business. Consume only explicitly
  completed IDs from a captured event claim; omitted AMEND subjects,
  errors and exhausted batches retain pending work. Waits are Store
  dispatch gates; waking cannot clear AMEND, applied versions or UNKNOWN.
- Rebuild history preserving every original attempt/feedback binding;
  missing observations stay explicit; never reread a file to invent old
  output; context overflow blocks before another reservation. Capture
  question/submission targets and versions before the management call;
  an answer ends only its own exact wait.
- Submissions require current grants, complete captured file bytes and
  settled operations; TeamLead checks the immutable snapshot with
  bounded rework; CANDIDATE_READY is not review or acceptance.
  Dispatcher acceptance captures one candidate with CAS and complete
  evidence; Root only assembles accepted bytes and handles receipts.
- Persist exact delivery intent before publication; verify every
  original byte and pinned root identity; publish atomically without
  overwrite; receipt identity covers package/hash/versions/recipient/
  method/key. Acceptance creates an original-source experience job
  atomically; terminal requests finish only captured experience calls.
- Persist ownership intent before mkdir and the immutable inventory
  before unlink; never adopt legacy directories; workspace DONE is not
  global cleanup DONE. Keep the control database outside bound workspaces
  with pinned identity, frozen runtime hashes and strong-process refusal;
  never enable a weak production path to make a test pass.
- The Store is a trusted-host API: model arguments pass the model-input
  Schema, captured call and final Store guards; persist original request
  envelopes and every attempt's usage; never replace unknown with zero.
  Resolve model/reasoning from the frozen Root snapshot; tool grants use
  role/task policies; configuration updates never silently change an
  existing request; schema shape checks do not prove semantic coverage.

## Required reading

Blueprint `docs/spec/v0.5/01_实施规格_v0.5.md` and its unchanged v0.4.1 references, `docs/ROADMAP.md`,
`docs/implementation/M1-contract.md`; the S1 chain in `docs/implementation/`
(from `S1-foundation.md` through `S1-owned-cleanup.md`); `docs/STATUS.md`
and `docs/reviews/claude-disposition.md`; then every R1/R2/R3
ADR/implementation chain in the complete contract.

## Build and verification

Run release and sanitizer tests as a non-root user (the hosted CI checks
its UID). Fault-injection tests damage only their own throwaway fixtures
and never relax production permissions or safety assertions to pass.

```sh
make -j2 test PYTHON=/path/to/python-with-test-dependencies
make -j2 ubsan PYTHON=/path/to/python-with-test-dependencies
cmake -S . -B build-cmake -DPython3_EXECUTABLE=/path/to/python-with-test-dependencies
cmake --build build-cmake --parallel 2
ctest --test-dir build-cmake --output-on-failure
```

M1's kernel stays third-party-free; the S1 library needs Boost 1.83+, SQLite 3.37+, OpenSSL 3+, libcurl 7.88+ and LibYAML 0.2.5. Contract tests use `jsonschema`; dev venv `.local/test-venv/bin/python`. Report synthetic, contract-reference and real integration results separately.
