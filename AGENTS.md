# WLS861 development contract

Current implementation: **0.12.0**, blueprint **v0.4.1**, Store **410**.

Entry point only: the complete, still-binding contract is preserved verbatim in `docs/implementation/development-contract.md` — current, normative, not archived history; read it before changing runtime behavior.

The owner adopted all decisions in [`docs/reviews/claude-2026-11-13/02-decisions.md`](docs/reviews/claude-2026-11-13/02-decisions.md) on 2026-11-13. For process, defaults, priorities and authorization, those decisions supersede conflicting earlier text; the complete contract's safety invariants remain binding. Start with that review directory's `README.md` and read `01` through `04` in order.

## Authority and permissions

- The owner approved review corrections and GitHub push on 2026-10-17, then requested development under the current plan. Routine development, corrections and fixes in that scope need no new approval per task card; record decisions and changes. Never infer model spending or deployment credentials from this; explicit user instructions take precedence.
- Keep the core in C++20 and isolate Linux platform code; provider
  endpoints, credentials, model aliases and budgets stay in external
  configuration. Tests use FakeModel and self-created temporary fixtures,
  including real file effects; tests never call real models. Do not operate
  production data/services. G2 paid runs are separately authorized only for
  OpenAI Responses / `gpt-5-mini`, at most USD 1 per request and USD 10 total,
  with at most two eligible retries under decisions §3.3. Before any real
  request, require both the owner's explicit “密钥已就绪” and the shell
  environment variable `WLS_G2_API_KEY`; never echo or persist its value.
  Windows 11 remains NOT_RUN. D-12 cancels private CI evidence hosting:
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

Blueprint `docs/spec/v0.4.1/01_实施规格_v0.4.1.md`, `docs/ROADMAP.md`,
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
