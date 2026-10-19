# WLS861 development contract

Read the current blueprint at `docs/spec/v0.4.1/01_实施规格_v0.4.1.md`,
`docs/ROADMAP.md`, and `docs/implementation/M1-contract.md` before changing
runtime behavior. Also read `docs/implementation/S1-foundation.md` for the new
foundation, and `docs/implementation/S1-worker-tools.md` for the file/process boundary,
then `docs/implementation/S1-event-runtime.md` for the current 0.4.0-s1.3
library/CLI. The blueprint defines the target; implementation
contracts and actual reports define what this checkout can execute. M1 remains
its original kernel; S1.2 adds bounded real files and a strong-only process
entry point; S1.3 adds a bounded durable event pump, isolated independent tasks,
explicit waits and original cross-run feedback. A completed daemon and positive deployment sandbox acceptance
remain separate task cards.

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
  in the S1 library; complete memory, review, delivery and cleanup runners do not.
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
batch installs the pinned contract-test dependency there. Add meaningful
regression coverage for changed
state, concurrency, persistence or permission semantics; report synthetic,
contract-reference and real integration results separately.
