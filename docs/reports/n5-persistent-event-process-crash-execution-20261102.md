# N5 persistent event process-crash execution — 2026-11-02

The integration owner's actual targeted run passed all **22 Store methods**
and all **13 persistent runtime methods**. Its complete original output
contains **21 distinct known-PID SIGKILL/reap receipts**, covering W00–W16
with the prepared variants and the real legacy W19 feedback gap. Separately,
the Store source head passed all **93 registered suites** in the remote
strict push job and in the strict PR job. This report records those actual
executions; the earlier preparation and failure reports remain immutable.

The Store author reviewed the original logs and JUnit rather than launching
another local build or product test. This publication adds documentation
only. Production Store source, tests, migration 013 and resource identities
retain their executed bytes.

| Executed input | Identity or result |
| --- | --- |
| Store source commit | `bef9cec095e972f7d5c7ff7e14ff917a9c43c4fe` |
| Store source tree | `df06e15ecb5648ee75baa6b6fd64e37b9052a7aa` |
| Integrated targeted commit | `0458806227a7d76698428c197f2b9f74de57c954` |
| Integrated tree | `c3473482be2086955ac7dfd511afb67ac2f65f0f` |
| Local configuration | Release, `/usr/bin/g++-13`; ASan and UBSan OFF |
| Local configure / build / CTest | All exited 0 |
| Registered / selected suites | 94 integrated registrations; exactly two selected |
| Store execution | 22 passed, zero failed; 29.0172 seconds |
| Runtime execution | 13 passed, zero failed; 5.87515 seconds |
| Original suite deadline | Store 120 seconds, unchanged |
| Source capture | All 867 recorded source entries identical before and after |

The integrated commit contains the Store source commit in its ancestry. The
recorded Store production and test hashes were checked against both that
integrated commit and the Store worktree. The executed Store binary SHA-256
is `f17fc59b71f773b82da040c604eb8ad3d50a20e3c46af7bd3d5d90b6ba9b662c`;
the runtime binary SHA-256 is
`f506c3b3648f7d110dff2c8850eeab3b916496eefb5ffe86855d4c16cd1e9511`.
The complete source and binary manifests remain in the original private
execution evidence.

Each row below is an actual receipt from the complete `LastTest.log`, not a
prepared case or an inferred callback count. Every child PID is positive and
distinct; every receipt says `signal=SIGKILL reaped=true`. The target callback
count increments inside the actual adapter's `complete` entry. Earlier plan
and instruction fixture calls are outside that target count.

| Actual phase | Target callbacks | Observed boundary and passing oracle |
| --- | ---: | --- |
| W00 | 0 | Before T0; no new claim, attempt, link or budget |
| W01 | 0 | Original claim changed inside T0, before snapshot INSERT; whole rollback |
| W02 | 0 | T0 committed; original C retained, old token rejected, fresh capture free |
| W03 | 0 | Attempt/budget changed inside T1, before link INSERT; whole T1 rollback |
| W04 | 0 | T1 committed; reservation, link, selected sources and open projection retained |
| W05 | 0 | Original dispatch event inside T2, before receipt; both roll back |
| W06 | 0 | T2 committed before adapter entry; one durable dispatch, no observation |
| W07 | 1 | Actual adapter entry before return; no durable response, UNKNOWN after takeover |
| W08 | 1 | Original response in memory before T3; no fabricated durable observation |
| W09 | 1 | Original settlement inside T3, before receipt; whole T3 rollback |
| W10 | 1 | T3 committed; original known observation retained, no effect or ID completion |
| W11_prepare | 1 | Original prepare event inside T4; no op/intent/event half-write survives |
| W11_start | 1 | Original start event inside T4; same whole-transaction rollback |
| W12 | 1 | Note work succeeded inside T4 before feedback; no durable E=1/F=0 gap |
| W13 | 1 | Feedback row inserted inside T4; effect, feedback and proof all roll back |
| W14_first_id | 1 | First captured ID changed to DONE inside T4; neither ID partially completes |
| W14_close | 1 | Original finish/open removal inside T4, before CLOSE INSERT; whole rollback |
| W15 | 1 | T4 committed; both individual source/binding/op proofs, later pending ID, free replay |
| W16_amend | 1 | Real AMEND committed after T3; control and original observation survive |
| W16_cancel | 1 | Real cancel committed after T3; no Worker effect, feedback or completion |
| W19 | 1 | Real legacy 410 note committed before feedback; E=1/F=0, zero links after upgrade |

All five crash methods completed and passed. Their assertions verified the
previous complete database state before takeover, original immutable claim,
link, member, event and receipt bytes, original envelope/binding/response/
usage/settlement, budget retention, stale dead-session Actor rejection,
unresolved claim fences, and no extra effect or dispatch on CLOSED replay.
For W19, real legacy public runtime calls created the history and successful
note before the kill; the 411 upgrade retained that history and created no
association policy, claim, link, item, receipt or open projection.

W17's runtime method `close_joins_callback_and_stores_only_original_observation`
also actually passed. W18's runtime method
`post_t1_storage_failure_retains_slot_and_flush_never_reapplies_t4` and the
Store T4 I/O-fault method actually passed with the test-only `SQLITE_IOERR`
injector. These are shutdown/join and live-host storage-error scenarios,
respectively; they are not counted as additional SIGKILL cases.

The JUnit file records two executed suites, zero failures and zero skips. Its
stdout is capped at 1024 bytes per suite, so individual method and kill
receipts were verified against the complete 10,688-byte `LastTest.log`. The
copied `LastTestsFailed.log` is a stale 26-byte file byte-identical in runs
06, 07 and 08; it was preserved. Current CTest exit 0, current JUnit and the
complete current PASS output establish the result of run 08.

The strict remote push run checked out exactly the Store source head. The PR
run checked out GitHub's synthetic merge commit
`7db66d540b8e906f703c9904c6c254ea84f90229`; fetching that ref and comparing
trees confirmed the same `df06e15...` tree as the Store source head.

| Actual strict CI | Complete registered suite result | Store suite |
| --- | --- | --- |
| [Push run 41459049325, job 125323084664](https://github.com/wmqfl861/wls861/actions/runs/41459049325/job/125323084664) | 93 passed, zero failed; 344.04 seconds | Passed, 10.81 seconds |
| [PR run 41459058440, job 125323113864](https://github.com/wmqfl861/wls861/actions/runs/41459058440/job/125323113864) | 93 passed, zero failed; 207.57 seconds | Passed, 6.30 seconds |

These 93-suite remote results belong to the Store branch. The integrated
branch has 94 registrations because it adds the separate persistent runtime
suite. The local two-suite run does not establish a full 94-suite integrated
regression result. This report also does not establish complete sanitizer
acceptance, final integration-head CI, formal PR approval or merge.

Original evidence is retained on the task data disk; raw logs are not
published in this repository. The private execution-review receipt checks all
35 method names and results, all 21 phase/PID/callback receipts, JUnit,
registration/deadline, source ancestry and hashes, binary identities and both
complete strict logs. Its path is
`evidence/n5-1-store-process-crash-execution-01/execution-review.json`, SHA-256
`70ddba66f391c16feabec5e8ecfbae4104b352f7e04e6f0920e8ddc5a69c3a7a`.

| Private original evidence path | Bytes | SHA-256 |
| --- | ---: | --- |
| `evidence/n5-integrated-targeted-build-08/ctest-logs/LastTest.log` | 10688 | `b41d0d4d95b61df10e030a14928a9fc902e99c6bdf8a2b159d65e1604ad299cd` |
| `evidence/n5-integrated-targeted-build-08/ctest.xml` | 2772 | `9c5d86733712b700bcecaa8a774c416b84715ba125c3bbaf8a1266525a3670d2` |
| `evidence/n5-integrated-targeted-build-08/ctest.log` | 526 | `5cee502b9cf28f22796f19201b084508e921b977cdf23554c56c05403269536c` |
| `evidence/n5-integrated-targeted-build-08/source-before.json` | 131987 | `5a4072bcb67f693ae14ed6ef05a7212d3fb4c014d80f32acee45adbf35c06df3` |
| `evidence/n5-integrated-targeted-build-08/source-after.json` | 131987 | `5a4072bcb67f693ae14ed6ef05a7212d3fb4c014d80f32acee45adbf35c06df3` |
| `evidence/n5-1-store-process-crash-source-01/remote-bef9-push-strict-job.log` | 80457 | `40d4444afb4c212d8edc15f4efc182c9cc0226478d813db7b219a81d74994912` |
| `evidence/n5-1-store-process-crash-source-01/remote-bef9-strict-job-raw-retry.log` | 81250 | `69e749729229bf279ecc799e548f1e263ffbd6d7afc56310f0a3feadfddf65a3` |

The first PR-log download was rejected by the CLI because the response
contained terminal escape sequences. Its empty output and original error
are retained separately. The explicit raw retry saved the complete original
log to a private file; selected readable lines are a separate derivative.

The preparation report's remaining limits still apply: Store W00 covers its
pre-T0 boundary, runtime slot ownership/preflight has separate runtime
coverage, W15 is the committed Store boundary rather than a future owned by
the Store fixture, and there is no named
`UNLINKED_HISTORY/HOLD_MISSING_FEEDBACK` public classifier API. W19 proves
the original facts and absence of invented associations. The earlier failed
compile and fixture execution checkpoints remain part of the retained
evidence history; this successful run does not rewrite them.
