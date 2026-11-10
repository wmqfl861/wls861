# Controls partial experimental snapshot — 2026-11-10

This branch preserves unfinished Controls work, its combined dependency sources,
synthetic fixtures, and existing reports. Independent whole-Controls source review
concluded **FAIL**. Its scoped approval permits an accurately labelled experimental
save only; feature acceptance, complete runtime validation, and PR qualification
remain open. Earlier reports retain their original source and execution scopes.

Three actual configure attempts exited 0; all three subsequent builds exited 2.
The final attempt linked the `wls_s1` static library and `live_model_store_tests`.
`controls_detail_tests`, which includes the actual CLI implementation, still has
CLI string/JSON conversion and unused-variable errors, plus test comparison errors.
Compilation errors are not business RED results.

| Targeted selector | Actual result |
| --- | --- |
| `legacy_absence` | NOT_RUN_TIME_BOUND; linked binary, no test process launched |
| `settlement_actor` | NOT_RUN_TIME_BOUND; linked binary, no test process launched |
| `final_summary_bound` | COMPILE_ERROR / NOT_RUN |
| `partial_network_lower_bound` | COMPILE_ERROR / NOT_RUN |

Preparation and identity binding left insufficient time for the full original
90-second live-test limit plus cleanup. Neither live selector started; this was
not a product timeout. Business **RED=0, GREEN=0**, and all 26 runtime rows are
**NOT_RUN**. All four targeted business fixes remain uninstalled: legacy absence,
exact original settlement-query actor, final summary size, and partial-network
known-send lower bounds. The query actor finding does not prove a new-send bypass.
Independent static review also found a legal maximum wall-duration marker can be
rejected by its stored-seconds bound; that additional finding is deferred to the
coordinator without a source change or runtime claim.

Fifteen rows retain missing or unproved subcases: F2.2, F2.3, F3_A.2, F3_A.3,
F3_A.4, F3_B.1, F3_B.2, F3_B.4, F4_COUNTS.2, F4_COUNTS.4, F4_FACTS.1,
F4_FACTS.2, F4_FACTS.3, F4_BOUNDS.2, and F4_BOUNDS.3. The other eleven rows
have no additional named source gap in the retained map, but are also unrun.

The fixed repair03 cumulative production delta remains **+594/-203 = 797/800**.
That is a fixed repair baseline, not a whole-branch allowance. The private next
compiler proposal is 805/800 and remains uninstalled and uncompiled. A previous
whole-candidate diagnostic against main was 4,910 changed production lines; it is
historical and does not establish a legal PR base/head or an 800-line PR qualification.

The original source phase ended before its soft deadline. The separate observer
then exited 0 after 142 resource samples; the maximum interval through its actual
stop was 30.0655 seconds, and recorded growth/floor checks passed. The earlier
599.6875-second sampling gap remains **FAIL**. Private rules, receipts, databases,
builds, raw logs, and old generated Python files remain local. This publication
performs no product build, test, business repair, or real/paid model call.
Windows 11 remains **NOT_RUN**; private evidence hosting remains **BLOCKED**.
