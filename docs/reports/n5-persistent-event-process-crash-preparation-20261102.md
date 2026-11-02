# N5 Store process-crash regression preparation — 2026-11-02

Five methods added to the existing `persistent_event_store` suite prepare 21
independent, real child-process kills. The existing 17 methods, CTest
registration, and 120-second deadline remain. Production Store code,
migration 013, resource identity, and public interfaces are unchanged. This
is a source checkpoint: the new C++ crash methods are **NOT_RUN** by this
author. The integration owner controls the single local build and test window.

The fixtures close the parent's Store before `fork`, so no Store connection
or writer lease is inherited. Each child owns its sole Store, captures the
original Worker call before T0, and executes the real T0–T4 methods. A
test-only SQLite function, registered through `sqlite3_auto_extension`, blocks
at the selected trigger boundary; post-commit boundaries block directly after
the real method returns. No fault API is added to Store. The parent receives
a bounded framed JSON message naming the exact phase and actual target
adapter callback count, sends `SIGKILL` to the known child PID, and verifies
that `waitpid` reaped that PID because of `SIGKILL`. A missing boundary, child
exception, early exit, or five-second handshake timeout fails the method.

The target adapter's counter increments inside its actual `complete` method
before FakeModel runs. W06 records zero target callbacks; W07 and later
record one. The two earlier fixture calls establish plan and instruction and
are outside this target callback count. No paid or real model call is made.

For W00–W16, the child records the hash of all committed application-table
rows at the previous complete step. After the kill, the parent opens Store
read-only before takeover and requires this same hash, verifying rollback of
all writes inside the interrupted transaction. It also checks the exact
original attempt, dispatch, settlement, effect, feedback, CLOSE and open-ID
counts. Immutable claim, link, item, committed event and receipt rows retain
their original bytes across recovery. Captured envelopes, bindings, response,
usage and settlement are compared directly. Pending attempts recover as
UNKNOWN with their reservation and unknown budget retained; settled attempts
retain their original known observation. Dead-session actors fail the real
Store authorization gates. Unresolved actors cannot claim another batch.
W15 checks both captured IDs' individual source/binding/op proofs, the later
feedback ID remaining pending, and replay without another operation,
dispatch, event, receipt or budget change.

Only the named test trigger is dropped, after read-only inspection and before
takeover. The dead writer's ephemeral session travels only through the pipe
to test its rejection; it is neither printed in the phase receipt nor saved
as durable authority.

| Design window | Executable case and kill boundary | Principal oracle |
| --- | --- | --- |
| W00 | `W00`, before T0 | No new claim snapshot, attempt, link or budget |
| W01 | `W01`, before immutable claim INSERT, after original claim changes | Whole T0 equals the baseline |
| W02 | `W02`, T0 returned | Original C retained; takeover releases old claim; fresh capture spends nothing |
| W03 | `W03`, before link INSERT, after original attempt/budget changes | Whole T1 rolls back; T0 survives |
| W04 | `W04`, T1 returned | Original reservation/link/items/open IDs; no dispatch |
| W05 | `W05`, before DISPATCH receipt INSERT | Original dispatch event and receipt roll back together |
| W06 | `W06`, T2 returned, before adapter entry | Exactly one durable dispatch, zero target callbacks |
| W07 | `W07`, inside actual adapter entry | One target callback; no durable observation; UNKNOWN after takeover |
| W08 | `W08`, actual Fake response returned, before T3 | No invented durable response or usage |
| W09 | `W09`, before SETTLEMENT receipt INSERT | Original settlement and receipt both roll back |
| W10 | `W10`, T3 returned | Original known response/accounting; no effect, feedback or completed ID |
| W11 | `W11_prepare` / `W11_start`, after original prepare/start event INSERT | No op, intent, event or task half-write survives |
| W12 | `W12`, before feedback INSERT, after successful note work inside T4 | No durable effect-without-feedback gap |
| W13 | `W13`, after feedback INSERT | Feedback and all later proof work roll back |
| W14 | `W14_first_id` / `W14_close`, after first DONE update / before CLOSE INSERT | Both C IDs and open projection roll back together |
| W15 | `W15`, T4 returned | Exact original CLOSED proof and free replay; later ID remains pending |
| W16 | `W16_amend` / `W16_cancel`, real control committed after T3 | Control and original observation survive; no Worker effect or feedback |
| W17 | Runtime `close_joins_callback_and_stores_only_original_observation` | Existing shutdown/join interleaving; this Store patch does not claim an additional SIGKILL case |
| W18 | Store `t4_faults_never_commit_note_without_feedback_and_proof`; runtime `post_t1_storage_failure_retains_slot_and_flush_never_reapplies_t4` | Actual `SQLITE_IOERR`, live-host retained result, evidence-only flush; this is deliberately a storage-error case |
| W19 | `W19`, legacy 410 before real feedback INSERT | Real committed note E=1/F=0, retained through read-only inspection and 411 upgrade |

W19 builds its complete history through the original legacy runtime: real
reservation, dispatch, Fake response, settlement, plan, instruction, public
owner acknowledgement, and note prepare/start/finish. The feedback trigger
blocks only after the old path has durably committed its effect. The parent
then kills the child, inspects Store 410 without upgrading, and performs an
explicit all-request 411 upgrade. The original attempt/envelope/response,
successful operation, intent, original event prefix, feedback gap and budget
remain. Every new association table stays empty; retroactive enablement is
rejected. No accepted row, source, operation result or historical association
is forged by SQL. There is currently no public named
`UNLINKED_HISTORY/HOLD_MISSING_FEEDBACK` classifier API, so this test asserts
the preserved facts and lack of invented links; named-classifier coverage
remains an explicit limitation.

W00 here checks the Store pre-T0 boundary. Runtime result-slot ownership and
script preflight have their separate runtime tests; the Store fixture does
not manufacture runtime scheduler capacity. W15 checks the committed Store
boundary before a hypothetical external notification and does not claim that
the Store-only fixture owns a runtime future. W17/W18 require the integrated
runtime methods above; adding their names to this mapping is not a PASS.

The immediately preceding test fixture correction was diagnosed by an actual
integration rerun. A subsequent compile exposed a test namespace collision:
introducing `wls::test` made existing unqualified `test` names ambiguous in the
runtime test. Commit `daa35d33ac6f2ee255dc07f843e52f5ec9f017a5` changed the
helper to the existing global `test::persistent_events` namespace and made
Store's two references explicit. The I/O function and exact error assertions
were unchanged. The original compile diagnostic log is retained at
`evidence/n5-integrated-targeted-build-05/build.stderr.log`, SHA-256
`5ce1b65f6a5d46811a70a66fb8d20088f023c54db0c45caa59de0d6ae3669ee6`.

The integration owner's next run compiled successfully and executed all 13
runtime methods successfully. Store executed all 17 methods: 16 passed, and
the T4 method failed because its fixture compared a joined `Store::tasks`
object with the raw `Store::task` object. The original execution log is
`evidence/n5-integrated-targeted-build-06/ctest.log`, SHA-256
`4dc98db0120c513d6da76cd2396a0d0d3268f5f881a66e0f7c7bdf3249b24ade`.
Commit `c8de76a679ba54eac8e4ca0a01b3dc468f66d688` captures both full
objects before injection, prints their distinct key sets, and requires both
objects to remain exactly unchanged after the fault. Every original op,
feedback, budget and open-ID check remains. This is a fixture correction,
not a weakened field subset or a claimed production defect. Its subsequent
execution is not claimed at this report's preparation checkpoint.

Static source review and `git diff --check` support handoff only. Actual
compilation, all 22 Store methods, the 21 known-PID kill receipts, integrated
runtime W17/W18, broader regressions and sanitizers must be reported from
their execution logs. The original source, compile-correction and fixture
diagnosis reports retain their publication-time status.
