# Changelog

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
