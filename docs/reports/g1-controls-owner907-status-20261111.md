# Controls owner907 experimental status — 2026-11-11

This is an unfinished, non-PR preservation candidate on
`experimental/controls-partial-20261110-40f7e303`, prepared from commit
`5325f68f838d67b9ef3f91e284dfa70a34f92743`. The actual continuation hard review
concluded **FAIL / OPEN**. Its limited source-identity, exact-patch/count and
retained-evidence checks passed; those checks do not establish product success,
complete source qualification, a completed paired reproduction, or PR approval.

The reviewed repairs are installed, including the real `request_runtime` config
identity lookup, legacy-absence handling, exact settlement-query actor binding,
final-summary sizing, known-send lower bounds and legal maximum wall time.
The earlier [2026-11-10 report](g1-controls-partial-experimental-20261110.md)
remains unchanged and describes the previous 797 / no-run snapshot. Its former
“fixes uninstalled” and compile-failed/no-RED statements are historical, not the
state of this candidate.

| State | Value |
| --- | --- |
| Reviewed patch installation | COMPLETE, experimental source only |
| `IMPLEMENTATION_COMPLETE` | `false`; whole Controls functionality remains unfinished |
| `LOCAL_VALIDATION_COMPLETE` | `false` |
| Source preservation | `PUSH_PENDING`; this report and save remain subject to fresh verification |
| Remote CI | `REMOTE_CI_NOT_RUN_THIS_UNPUBLISHED` |
| `PR_REVIEW_COMPLETE` | `false`; no PR qualification or new PR is claimed |
| `MERGED` | `false` |

The cumulative production delta against the **same physical repair03-after
baseline** is **+663/-244 = 907**, within the owner-specific allowance for this
reviewed repair. That does not waive the separate **whole-PR 800** limit. The
whole-candidate diagnostic against main commit
`4987f2c6a6fc0da4b91e53d3689019ba3f0e8631` is **4,931** changed production
lines; actual lawful PR base/head and dependency coverage remain **UNBOUND**.
It is a diagnostic, not a PR qualification or a different repair baseline.

The original 797 input's settlement-actor run failed with a SQL/API error because
the config digest was queried from a table without that column. That is not a
business RED. The actual paired RED input was explicitly **807**, retaining all
five behavior defects while adding the reviewed compiler and real-table SQL
prerequisites. Both approved Release test targets then compiled and linked.

| Direct RED selector | Actual evidence and limit |
| --- | --- |
| `legacy_absence` | True marker absence reached the expected missing-member exception; budget unchanged. Later legacy-pump assertions were not reached. |
| `final_summary_bound` | Final summary was 8,231 bytes including newline, over the 8,192-byte limit; the final bounded-size assertion failed. |
| `settlement_actor` | Original-settlement observer subcase PASS; the separately registered exact-actor subcase FAIL because the authorized wrong same-team actor returned a snapshot (`refused=false`). Budget/send/effect counts remained unchanged; this is a query-contract defect, not proof of a new-send bypass. |
| `partial_network_lower_bound` | Helper FAIL: known 31 plus partial known 5 still reported 31 instead of 36, with completeness false. Later helper assertions were not reached. The separately registered driver failed an earlier prerequisite assertion, as described below. |
| `legal_max_wall` | Valid maximum `1,000,000,000,000` minutes produced `60,000,000,000,000` seconds with matching config and ordered anchors, then failed the open-gate assertion. |

All five direct selector commands exited 1 without a command timeout or
infrastructure failure. They produced **seven registered test-binary case
results: one PASS and six FAIL**; these are not CTest runs or full-suite counts.
The original author accepted five distinct production-defect RED witnesses,
using the network helper witness for that repair. The network driver's intended
lower-bound RED target remains **NOT_REACHED**, independently of that decision.
Its first `missing` variant recorded callback 1, attempt 1 and zero completed
batches, then failed the prerequisite expecting two attempts; raw stops were
`error` and `report_capacity`. Cause is **UNKNOWN**. The `null` and `wrong_type`
variants were not reached. No inference from the summary fix or helper supplies
those missing integration observations.

The original command shapes were the same for RED and GREEN, with only source,
build and evidence paths substituted:

```text
cmake -S <SOURCE> -B <BUILD> -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DBUILD_TESTING=ON -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build <BUILD> --target live_model_store_tests controls_detail_tests --parallel 2
live_model_store_tests <SOURCE> --controls-regression <LIVE_CASE> --evidence-dir <NEW_EVIDENCE_DIR>
controls_detail_tests --controls-regression <DETAIL_CASE> --evidence-dir <NEW_EVIDENCE_DIR>
```

The 695-file source identities were frozen separately: final source matches
GREEN, while RED differs in exactly three production paths. All **127 shared
test/fixture files** match across frozen T, RED, GREEN and final source.
Recorded tools, normalized compile/link recipes, source/vendored headers and
runtime manifests were checked.
Compilation-time **system-header contents remain UNPROVEN**: no retained complete
header-content manifest exists. Identical T and recipes do not establish a
completed GREEN executable or its loader identity.

GREEN configure exited **0**. Its build was safely interrupted at the unchanged
original soft deadline with **SIGTERM / exit -15**, after about 530.19 seconds.
Only `libwls_kernel.a` completed; `libwls_s1.a` and both target test executables
were not completed. **All five GREEN selectors are NOT_RUN_TIME_BOUND**. No
product assertion failure or successful GREEN is inferred from this interrupted
build. The full 5,400-second window included setup, source preparation, author
and review waits, classification and sealing; it was not extended or restarted.

Execution order has a separate **FAIL_RETAINED** finding. The precise author
FINAL required installation checkpoint, then recorded matched outcomes, then
public test promotion. Actual order was checkpoint, public test promotion, then
GREEN admission while results were explicitly `NOT_YET_RUN`. The original author
confirmed the departure and did not retrospectively approve it. Exact installed
bytes and T identities remain separately verified; that does not cure the order
failure. The experimental candidate is retained without cosmetic rollback or rerun.

Retained bounded runtime evidence includes **162 independent resource samples**,
a maximum adjacent gap of **30.382032301975414 seconds**, and a larger recorded
peak growth of **216,481,081 bytes**, below the **536,870,912-byte** growth cap.
The separate 536,870,912-byte reserve floor was met without deletion credit.
Native session **62744** was actually awaited with exit **1** before the original
hard deadline; its exact return monotonic time is UNKNOWN and the post-tool
capture is recorded without backdating. The terminal records
`cleanup_complete=true`, `remaining_owned=[]`, `ownership_gaps=[]` and
`observer_stopped=true`; root recorded the owned supervisor absent. The independent
observer continued through the native terminal. Later audit/sealing files are
post-stage artifacts, not bytes retroactively included in the runtime samples.
The earlier **599.6875378259574-second historical sampling gap remains FAIL**.

The full **26 runtime rows were not run**. These original **15 row qualifications
remain OPEN**, even where a targeted source repair and RED now exist: F2.2, F2.3,
F3_A.2, F3_A.3, F3_A.4, F3_B.1, F3_B.2, F3_B.4, F4_COUNTS.2,
F4_COUNTS.4, F4_FACTS.1, F4_FACTS.2, F4_FACTS.3, F4_BOUNDS.2 and F4_BOUNDS.3.
Copied older gap descriptions about uninstalled fixes or uncompiled selectors
remain historical projections, not current claims. After-effect allocation,
conversion and recovery-insertion faults; complete terminal/whole/output bounds;
restoring actions; continuation/pending and UNKNOWN/history variants; artifact
and receipt field combinations; qualified Correction dependencies; and full
runtime/release/sanitizer validation remain unproved. No new missing matrix
fixtures or extra product runs were added in this preservation step.

The exact-patch/static prerequisite review was **PASS**; its original prepared-T
readiness finding was **FAIL**, then closed by a separate targeted **PASS** on
the two real-table fixture corrections. The actual-result hard review remains
**FAIL** overall: source scope **PASS_LIMITED**, complete reproduction
**FAIL/UNPROVEN**, functional GREEN **NOT_RUN**, formal qualification **FAIL/OPEN**,
execution order **FAIL_RETAINED**, and bounded timing/resource/cleanup evidence
**PASS**. Public-summary/save review is a separate pending check.
`review.md` for that actual-result review has SHA-256
`f3c5b0b3782a2ba960d0a96cb3fbbb5415ed8bbba2b43b3b711044e7d5f749e0`;
`author-public-test-promotion-order-disposition01.json` has SHA-256
`8d2ef7a8da4d829be46b49edfbc2dfb00260de9e31d68507c5b48b420a01dd11`.
These are content references; private originals remain local.

No new/fourth PR, main write, force push, merge/close or real/paid provider call is
part of this save. Windows 11 remains **NOT_RUN**, private evidence hosting
**BLOCKED**, and real-provider/paid acceptance **NOT_RUN**. Private rules,
receipts, databases, raw logs, build trees and fixture identities remain local.
