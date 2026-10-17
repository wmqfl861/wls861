# WLS861 development contract

Read `docs/implementation/M1-contract.md` and the handoff entry point in
`docs/handoff/v0.3/README_先读.md` before changing runtime behavior. The v0.3
design and later explicit user decisions take precedence over v0.1/v0.2.

- Keep the core in C++ and isolate Linux platform code. Do not introduce
  production model calls, credentials, or hardcoded provider endpoints.
- Preserve Root -> Dispatcher -> TeamLead -> Worker. Only Workers execute
  business tools. Each independent request has its own fenced Dispatcher.
- Keep requirements, plans, applied requirements, state versions, and lease
  epochs distinct. Check identity, scope, permissions, and versions at commit
  and operation dispatch. A model response is never an authorization.
- Models and reasoning inherit each unspecified field directly from the
  request's frozen Root snapshot. Tool permissions use role/task policies.
- Do not edit the handoff originals or turn planned acceptance into results.
  Record actual tests and limitations separately under `docs/reports/`.
- Use only synthetic fixtures for M1. No production tools, remote model calls,
  automatic knowledge publication, or deletion of user data is implemented.
- Use `make test` for the dependency-free Linux build and test entry point.
  Keep generated builds, journals, secrets, and local reports out of Git.
- Changes must include the relevant contract/documentation and meaningful
  regression coverage for state, concurrency, persistence, and permissions.
