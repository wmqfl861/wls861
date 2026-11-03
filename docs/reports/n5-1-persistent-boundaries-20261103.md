# N5-1 persistent transaction and lifecycle boundary coverage — 2026-11-03

## Scope and input identity

This report records test hardening against the unmodified production input
`c2a5cea64aeea261b214cdfd9a968b53f491110a` (PR #25). The coordinator handoff is
`d843493fafe158f3a6443bafaa0de95cbf6c05c2`. The test worktree is
`test/n5-review-persistent-20261103-74bnxwj5` on the verified `/mnt/d` data disk.

The changed files are `tests/persistent_event_faults.hpp`,
`tests/persistent_event_store_tests.cpp`, and
`tests/persistent_event_runtime_tests.cpp`, plus this report. Production source,
public headers, CMake registration, migration 013, historical reports and crash
helpers are unchanged by this module. This is coverage of existing guards, not
a claim of a historical production vulnerability.

The local targeted tests link the existing GCC 13 Release archives from
`/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/final-candidate/build-gcc-release-20261102T171141Z`:

| Input | SHA-256 |
| --- | --- |
| `libwls_s1.a` | `7e0ff1e85a2d832d546c8c67aaf03a0ead995e2060bc1de106a169d98eb272d5` |
| `libwls_kernel.a` | `a780803a40cb241a40454fad86b7ec19da7e28babb66110aa49ce6f0f2e1767f` |

The archive hashes match the original c2 Release provenance. Actual source,
public headers, migration assets, schemas, configuration and runtime resources
were checked against that attempt's `source-before.json`. Test-only wrapper
translation units select the added methods, or invoke the complete original
suite entry point. They use the original production headers and resources,
`-O3 -DNDEBUG`, and the strict warnings including `-Werror`. They do not rebuild
or modify the original archives.

## Added checks

Five methods are added to the existing Store suite (25 → 30), and one to the
runtime suite (13 → 14). The actual c2 CTest plan has 94 registered tests. This
change adds methods inside two existing registrations; it does not add CTest
registrations or alter their 120/180 second limits or the existing five second
barriers.

- Six T4 failures cover plan, instruction and AMEND helpers, each with an I/O
  failure at feedback and at CLOSE. A single-snapshot hash of every table's
  actual columns verifies rollback of business effects, events, feedback,
  receipts, projections, claims and budgets. After reopening only the original
  request, the old actor is stale and a new actor retains the unresolved hold.
  Raw read-only sibling rows verify isolation without acquiring another
  request's Store authority.
- Four commit-outcome cases inject `SQLITE_IOERR_WRITE` from SQLite's WAL hook
  after the real T1/T2/T3/T4 transaction commits. A test-only trigger installs
  the hook on the actual writer connection during the targeted receipt insert.
  The test requires one hook installation, one actual callback and positive WAL
  pages, then independently reads the durable outcome. Exact replay cannot
  create another dispatch, settlement, feedback or CLOSE. Reopening preserves
  UNKNOWN and reservations for unfinished observations, settled original
  responses, stale-session rejection and sibling isolation. This simulates an
  API reporting a committed transaction as failed; it does not claim to
  reproduce a physical device failure. Installing a SQLite WAL hook also
  replaces that connection's default automatic-checkpoint hook; the fixture
  therefore does not establish unchanged checkpoint timing. It leaves journal
  mode, synchronous mode and all Store production code intact.
- A partial-ID CLOSED link is replayed after Store restart while a new claim
  owns the omitted ID and later feedback. Exact replay cannot finish the new
  claim or add original events or receipts. A changed late response conflicts;
  the original response, attempt and budget remain fixed.
- Seven fixture-local corruptions follow a previously successful read: claim,
  link, item and receipt fingerprints; encoded byte counter; business and
  control tail facts. Original immutable triggers first refuse the mutation;
  only disposable fixture triggers are temporarily removed to inject damage,
  then restored before any production reader runs. The damaged whole-table
  snapshot must differ from its original before calling a guard. Later status, listing and
  dispatch all reject corruption without writing or dispatching.
- Capacity checks freeze a real metadata limit at the measured original size
  and one byte below it, using fresh fixed-width-ID fixtures. Accepted exact
  replay is free; refusal leaves the complete table snapshot and budget
  unchanged. The business-event boundary preserves the original reservation
  and its two business-tail positions, complementing the existing one-position
  shortage case. No frozen policy or metadata counter is rewritten.
- Runtime destruction is observed from a blocked callback using the actual
  Linux writer-lock file and a second `flock(LOCK_EX | LOCK_NB)`. The writer
  lease must remain held until stop and callback collection. The future then
  refuses business completion, while a reopened Store contains the original
  settled response and no business effect or feedback. The old actor remains
  stale, and exact late observation replay cannot call the adapter again.

## T0–T4 coverage mapping

The existing process-crash fixtures remain byte-identical. New fault cases
complement them; they do not replace SIGKILL/reap evidence with exceptions or
change a crash phase into an expected timeout.

| Boundary | Existing permanent coverage on c2 | Additional coverage in this module |
| --- | --- | --- |
| Pre-T0 and T0 snapshot | `process_crash_claim_reservation_and_dispatch`: W00 before T0, W01 before snapshot INSERT, W02 after the claim commits. Runtime preflight, target selection and empty-S methods retain the original claims and zero-attempt checks. | Claim fingerprint corruption after an earlier successful snapshot is rejected by status, listing and dispatch. |
| T1 reservation/link | Same crash method: W03 between original attempt/budget and link INSERT; W04 after T1. `t1_t2_t3_failure_rolls_back_both_sides` retains the original I/O rollback case. | Real committed WAL error at T1, restart → UNKNOWN/retained reservation and no dispatch; exact metadata and business-tail admission boundaries. |
| T2 unique dispatch | Same crash method: W05 before DISPATCH receipt; `process_crash_callback_and_observation`: W06 committed before adapter entry. Existing exact replay checks retain one dispatch. | Real committed WAL error at T2 with exactly one dispatch, no guessed response and no renewed dispatch authority; link/item/receipt/counter/tail corruption refuses dispatch after a prior read. |
| T3 observation/usage | `process_crash_callback_and_observation`: W07 inside adapter, W08 in-memory response, W09 before SETTLEMENT receipt, W10 after T3. Existing UNKNOWN and exact observation methods retain nullable usage and original response identities. | Real committed WAL error at T3 preserves the original known settlement; exact replay after restart is free and changed late response conflicts. |
| T4 effect/feedback/proof/finish/CLOSE | `process_crash_atomic_action_tail`: W11_prepare, W11_start, W12 before feedback, W13 after feedback, W14_first_id, W14_close, W15 committed. Existing note I/O faults reject partial effects. | Plan/instruction/AMEND × feedback/CLOSE failures hash all table columns across rollback; real committed WAL error at T4 has exactly one closed outcome; old CLOSED partial-ID replay cannot finish a different claim after restart. |
| AMEND, cancel and restart | `process_crash_control_after_observation`: W16_amend/W16_cancel; existing partial AMEND, nullable UNKNOWN, restart, old Actor and late-cancel runtime methods. Existing W19 retains the real legacy unlinked effect/feedback gap rather than inventing associations. | All new rollback/commit-outcome fixtures reopen the original request, reject dead-session actors and verify durable holds or the original closed result. Partial-ID replay observes a new current claim and immutable old receipts. |
| Close/callback/Store ownership | Existing runtime `close_joins_callback_and_stores_only_original_observation` and `post_t1_storage_failure_retains_slot_and_flush_never_reapplies_t4` cover stop, collection and evidence-only retry. | Destruction, without an explicit caller `close()`, proves the actual exclusive writer lease is retained while the stopped callback is still alive, then proves successful reopening and the original settled response. |

## Local execution ledger

Private raw evidence is under:

`/mnt/d/codex-workspace/wls861-n5-review-20261103-74bnxwj5/evidence/`

Each compile/run directory contains the exact argument array, timestamps, exit
code, raw stdout/stderr and SHA-256, source and ELF identity before/after, and
the production archive identity. Temporary databases use a task-private
`TMPDIR` on the same data disk. The tests execute as uid 1000. Raw logs are not
published as public CI artifacts by this module.

| Attempt | Check | Result | Meaning |
| --- | --- | --- | --- |
| `persistent-coverage-release-01` | Store strict compile | PASS, 23.55 s | Original archives unchanged. |
| `persistent-coverage-release-01/targeted-new-store-run-01` | First four added methods | 1 PASS / 3 FAIL, 2.32 s | Fixture defects retained: sibling reads attempted outside request-only scope; early WAL-hook installation did not fire. These are not production red examples. |
| `persistent-coverage-release-02/compile-store` | Store strict compile | PASS, 26.06 s | Original archives and inputs unchanged. |
| `persistent-coverage-release-02/run-store` | Five added methods | 4 PASS / 1 FAIL, 5.49 s | All six T4 helper faults, four postcommit outcomes, partial CLOSED replay and seven corruptions passed. The additional metadata calibration started below the reserved receipt requirement; the failure is retained. |
| `persistent-coverage-release-02/compile-runtime` | Runtime strict compile | PASS, 14.72 s | Original archives and inputs unchanged. |
| `persistent-coverage-release-02/run-runtime` | Added destructor method | 1 PASS / 0 FAIL, 0.27 s | Actual writer lease retained during callback collection; original response persisted after reopening. |
| `persistent-coverage-release-03/compile-store` | Store strict compile | PASS, 24.48 s | Original archives and inputs unchanged. |
| `persistent-coverage-release-03/targeted-new-store` | Five added Store methods | 5 PASS / 0 FAIL, 8.28 s | Exact metadata size is 939956 bytes; the frozen calibration limit is 999999. All added fault, replay, corruption and boundary methods pass. |
| `persistent-coverage-release-03/full-store-runtime` | Complete Store and runtime method lists in two derived CTest registrations | 2 PASS / 0 FAIL / 0 TIMEOUT / 0 SKIPPED / 0 NOTRUN, 33.61 s | Store 30/30 (28.60 s), runtime 14/14 (4.94 s). The full raw `LastTest.log` and JUnit are retained. This is the affected pair, not the full 94-test plan. |
| `persistent-coverage-release-04/compile-store` | Final Store strict compile | PASS, 24.96 s | Final sibling-budget and damage-injection assertions compiled; original production archives and inputs unchanged. |
| `persistent-coverage-release-04/full-store-runtime` | Final complete Store/runtime lists | 2 PASS / 0 FAIL / 0 TIMEOUT / 0 SKIPPED / 0 NOTRUN, 33.25 s | Store 30/30 (28.56 s), runtime 14/14 (4.65 s), all 21 original SIGKILL/reap phases retained. Final source and ELF identities unchanged. |

The metadata fixture starts with the legal maximum, measures through the public
API, then recalibrates with a matching decimal-width limit before testing the
exact boundary. Earlier fixture failures and raw logs remain intact. After
release-03 completed, three explicit before/after checks of the sibling's raw
`request_budgets` row were added to the existing sibling request/attempt checks,
and the corruption fixture now explicitly checks that injection changed its
whole-table snapshot before invoking a guard. These final assertion additions
passed in release-04 on their own final input. All 184 recorded production
source/header/resource inputs match before and after that run. The final test
input was frozen at `2026-11-03T06:47:54.979860+00:00`; this report and machine
index were finalized after execution, without changing the test input.

| Final local artifact | SHA-256 |
| --- | --- |
| Store suite ELF | `70a25da0334abef3dde758475c34151bf052282b649e973972d89f5607fc2ae7` |
| Runtime suite ELF | `c2208eff1aa31164215305e115b4477238f33f7d5e65104f01ea5002d06ff243` |
| Complete private `LastTest.log` (14248 bytes) | `020e61c6ab0c84302a2665d1583782d6bd102ff9c1debdae5c3324881a3d8886` |
| Private JUnit (2751 bytes) | `8eec502835bb5950217bb65631b63a965029e5c298017c650839d0edcebc008e` |

## Non-author source review

`/root/compatibility_sidecars` reviewed the final three test files without
authoring them or running another product test. The review found no source
blocker and confirmed original limits, guards and the unchanged crash helper.
The review's two suggestions are addressed: explicitly prove injected damage
changed the fixture before calling production guards, and state the WAL-hook
automatic-checkpoint limitation. The source-bound private review receipt is
`evidence/persistent-nonauthor-review-01/review.json`, SHA-256
`5fbec605a1cab041ede7f012aedf900a01303f3e76f91825b79618f79df74e05`.
Its reviewed bytes are the bytes executed in release-04. This engineering
review is not a second-model evaluation or the coordinator's formal approval.

## Limits and delivery state

The added-method runs are targeted coverage on a new test input and the original
c2 production library. They are not a complete Release run, a new sanitizer
gate, final integrated-candidate validation, or coordinator approval. Original
ASan/LSan, Make-ASan and historical runtime failures remain open under N5-0;
these checks do not explain or replace them. Full candidate matrices and CI are
owned by the integration agent. No real model or paid API is used.

| Status | State for this module at report creation |
| --- | --- |
| `IMPLEMENTATION_COMPLETE` | COMPLETE for the assigned test-only module; non-author source review has no blocker. |
| `LOCAL_VALIDATION_COMPLETE` | Affected Release Store/runtime COMPLETE; full two-umask Release, Clang, UBSan and ASan matrices remain integration work. |
| `PUSH_COMPLETE` | NOT_COMPLETE at this prepublication report freeze; final SHA/PR are recorded separately after publication. |
| `REMOTE_CI_COMPLETE` | NOT_RUN at this prepublication report freeze; final-SHA CI is recorded separately. |
| `PR_REVIEW_COMPLETE` | NOT_COMPLETE: the main coordinator retains formal review authority. |
| `MERGED` | NO. |
