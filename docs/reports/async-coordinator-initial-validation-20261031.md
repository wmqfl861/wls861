# A3 single-writer asynchronous coordinator: initial candidate

The production candidate was frozen at `a2dfb46322d217f32eb5cc2516de6107c10db4b8`, based on `772feaee62086ddb0e7f605a7472721d3bc61406`. This record precedes its first runtime compilation. The later branch also merges the A1 test-tool dependency; neither that merge nor passing tool selfchecks is product validation.

## Bounded implementation

`AsyncCoordinator` owns one Store and confines all Store access to its constructing host thread. Existing M1 Scheduler callbacks execute offline ModelAdapter waits outside the Store transaction. Captured Actor/session/request/task/version/epoch remain original through completion. A new transactional `claim_offline_call` verifies current scope/CAS and original Fake reservation, then records a unique first-dispatch event. Replay cannot invoke the adapter again. The database lifecycle writer lock and published SQL are unchanged.

Admission, control and retained-result storage are bounded. Cancellation/AMEND use captured Root and CAS; control cannot refresh stale authority. Persistence failures fence new admission and retain original responses; explicit evidence flush never retries business application. Close requests stop and joins callbacks before Store destruction. Foreign-thread destruction performs no Store writes, leaving durable pending reservations for recovery.

This is an explicitly enabled S1 call-level offline library slice. Plans, orders, acknowledgement and notes use the existing OfflineRuntime result path. No automatic inbox pump, live adapter, arbitrary file/process/submit executor, daemon or multi-writer lease is claimed. Review is a scheduling class, not an independent business review verdict. Existing synchronous APIs/CLI remain the default.

## Actual checks and pending gates

The source is in the private data-disk `coordinator` worktree under `/mnt/d/codex-workspace/wls861-next-20261031-065247`, ordinary user ubuntu UID1000. Author checks of C++ syntax passed with strict warnings. One new `async_coordinator` CTest suite contains 12 cases with deterministic barriers for overlap, retained results, ownership/CAS/replay, cancellation/AMEND and shutdown/recovery. It has its own initial 180-second bound; all original 89 test deadlines remain unchanged. At this cutoff no native runtime case or sanitizer was executed, because the baseline formal GCC gate reserves the build window.

The A1 shared runner catalog is extended explicitly to 90 registrations, with matching default and CI count. Runner selfchecks now pass 24/24, exit0; original stdout/stderr/command metadata are in `evidence/coordinator-runner-24`. These are tool tests, not C++ product tests. The production test suite remains NOT_RUN at this cutoff.

Non-author `/root/independent_recovery_review` reviewed the exact a2dfb463 candidate and its Store/OfflineRuntime/Scheduler dependencies. Its 719 tracked file bytes matched Git blobs; the tree was `3f8310c0a9926bb133d36d2eb8e4df7af7afa108`. Receipt `evidence/a3-independent-review.json` SHA256 `927e9f2aa206bef45696365bf765092ea6e1237d63d6917ba9bfad7d3da4b4d5` records no confirmed blocker in that bounded source review and explicitly excludes dynamic execution. This is same-model engineering review, not coordinator approval.

The branch is a candidate for Draft publication with base `fix/sanitizer-followup-20261031` / PR #6. Runtime validation, candidate CI, final coordinator review and merge are incomplete. Historical Clang/live_workflow failures remain unresolved and cannot be waived by this experimental runtime slice. No real/paid model call or production effect was authorized or performed.
