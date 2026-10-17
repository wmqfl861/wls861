# Changelog

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
