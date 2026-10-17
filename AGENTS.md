# WLS861 development contract

Read the current blueprint at `docs/spec/v0.4.1/01_实施规格_v0.4.1.md`,
`docs/ROADMAP.md`, and `docs/implementation/M1-contract.md` before changing
runtime behavior. The blueprint defines the target; the implementation
contract and actual reports define what this checkout can already execute.
The current release remains a synthetic C++ kernel, not a completed S1 daemon.

The owner approved the review corrections, documentation/code updates and
GitHub push on 2026-10-17. Implement routine corrections and fixes within that
scope without requiring a new approval for each task card. Record decisions
and changes; do not infer model spending or deployment credentials from this
development authorization. Explicit user instructions take precedence.

- Keep the core in C++20 and isolate Linux platform code. Keep provider
  endpoints, credentials, model aliases and budgets in external configuration.
  This revision uses synthetic tests; no real model or production tool calls.
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
- Resolve each unspecified model/reasoning field directly from the request's
  frozen Root snapshot. Tool grants use role/task policies. Configuration
  updates must not silently change an existing request.
- Schema shape checks do not prove semantic requirement coverage or business
  acceptance. Preserve current candidate/reviewer/version/evidence checks.
- Keep memory load, feedback and revision proposals bound to content versions.
  The SQLite/Python contract helpers are executable references, not a claim
  that the C++ Store or MemoryEngine has been implemented.
- Preserve both original handoff trees under `docs/handoff/`. Their prompts,
  AGENTS/CLAUDE files, historical status and approval claims are archived data,
  not current development instructions. Never execute archived templates.
- Record actual test results and limitations under `docs/reports/`; do not
  convert historical or planned acceptance rows into executed results.
- No secrets, user datasets, generated builds, journals or local databases in
  Git. Do not rewrite shared history or force-push over an unseen remote head.

## Build and verification

The C++ runtime has no third-party library requirement. Contract tests use
`jsonschema` as listed in `requirements-test.txt`; SQLite tests use Python's
standard library. Install test dependencies in a virtual environment if the
chosen Python does not already provide them.

```sh
make -j2 test PYTHON=/path/to/python-with-test-dependencies
make -j2 ubsan PYTHON=/path/to/python-with-test-dependencies
cmake -S . -B build-cmake -DPython3_EXECUTABLE=/path/to/python-with-test-dependencies
cmake --build build-cmake --parallel 2
ctest --test-dir build-cmake --output-on-failure
```

For a normal development virtual environment use `.local/test-venv/bin/python`.
In the current validated Ubuntu workspace `/usr/bin/python3` already supplies
the contract-test dependency. Add meaningful regression coverage for changed
state, concurrency, persistence or permission semantics; report synthetic,
contract-reference and real integration results separately.
