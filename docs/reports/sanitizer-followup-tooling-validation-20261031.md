# A1 diagnostic tooling and reviewed registration guard

This record covers the test-tool changes on 2026-10-31 before the initial Draft publication. It does not close the historical Clang failures or declare the currently running local GCC product gate successful. The starting commit is `772feaee62086ddb0e7f605a7472721d3bc61406`; runtime `0.11.1-r3.3c-audit`, Store 410, published SQL and all 89 original product deadlines are unchanged.

## Inputs and environment

The isolated worktree is `/mnt/d/codex-workspace/wls861-next-20261031-065247/sanitizer`, branch `fix/sanitizer-followup-20261031`. `/mnt/d` was verified as `/dev/vdb2`, ext4, separate from the system disk. Ordinary user ubuntu UID/GID 1000, Ubuntu 24.04.1, GCC 13.3.0, Clang 18.1.3, CMake/CTest 3.28.3 and Python 3.12.3 were observed. Fixtures and original logs use private directories on that data disk. No host security or global dependency settings were changed.

PR #4 was freshly checked as an unmerged Draft with head 772feaee and base `test/r2-4-recovery-boundary-coverage` / `1a5ca4f3549e1531066a159b0e410e69b1a57573`. Its six Push lanes in run 41162941508 checked out 772feaee. The six PR lanes in run 41162986326 actually checked out synthetic merge `512ebab34119ecb7c3b66dfbf53c4cd3e3eb1806`; both runs succeeded. Source artifact 13027328455 contains 714 regular files matching local archive bytes and modes, tree `83743319f6287478ebb1ba5543a1c55c2a0bf670`. These results belong to the baseline, not this candidate.

## Changes and deterministic controls

`tests/immediate_unittest.py` prints and flushes the original formatted traceback immediately after unittest records a failure, error or failing subtest. The live workflow CLI test runner uses it. Original assertions, exceptions, order, failfast, exit status and deadlines remain intact. Headers omit subtest parameter values. The helper does not change the imported billing test runner.

`tests/test_immediate_unittest.py` uses a child that fails, then signals that the next test has started and blocks on stdin. The parent terminates it only after that deterministic signal. The original TextTestRunner control loses the traceback before termination; the candidate retains exactly the failure bytes observed before termination. The same script also checks order/accounting/failfast, subtests, expected failures and skip bookkeeping. Tool selfchecks are counted separately from product suites.

`tests/gcc_asan_expected_tests.json` freezes the existing 89 names and per-test timeouts. The existing shared runner now requires this catalog as well as the explicit count. An equally sized substituted registration set was accepted by the original helper; the candidate rejects it. This is a validation-tool coverage gap, not an old runtime vulnerability. New modules must review registration differences and update the catalog, default/CI counts and selfchecks together. No new runner or retry-until-green behavior is introduced.

## Actual executions

Raw commands, timestamps, exit codes and logs remain at `/mnt/d/codex-workspace/wls861-next-20261031-065247/evidence/a1-immediate-01/`; the non-author review receipts are beside that directory.

| Command / control | Actual result |
| --- | --- |
| `python3 -B tests/test_immediate_unittest.py --baseline` | 3 passed, 1 failed; exit 1. Missing original traceback in the interrupted child is the intended red control. |
| `python3 -B tests/test_immediate_unittest.py` | 4 passed; exit 0. Same deterministic child and assertion. |
| `python3 -B tests/test_run_gcc_asan.py` | 23 passed; exit 0. Mocked tool flow and registration contracts, not ASan product execution. |
| Independent repeat of runner selfchecks | 23 passed; exit 0; source hashes unchanged. |
| AST/import/diff checks | exit 0; billing import and subtest/failfast probes passed. |
| Frozen catalog versus baseline CTest JSON | 89/89 names and all original deadlines equal; CMakeLists.txt unchanged. |

Non-author engineering reviewer `/root/evidence_audit` recorded bounded reviews in `a1-immediate-review.json` and `a1-expected-set-review.json`. They found no unresolved blocker in the inspected test-tool scope. This is same-model engineering review, not the main coordinator's formal approval.

## Product validation and unresolved history

The exact baseline's original runner is executing the full GCC gate in the separate, frozen `integration` worktree. Its original selfchecks passed 20/20 and configure/build exited 0. Several product timeouts have already been observed; the run is continuing through all 89 registrations and cannot be labelled complete or passed at this record's cutoff. Final observations belong in the subsequent next-cycle execution report and immutable run evidence.

Historical Clang 18 Debug `-O2 -g` selected 39 tests: 29 passed, 10 timed out, CTest exit 8, 2949.68765 seconds. The original `live_workflow` output reported `test_model_exhaustion_escalates_original_rejected_submission` as FAIL before the 300-second group timeout, without the traceback. A later single-case probe passed in 28.1889 seconds; it does not explain the original FAIL. Both ext4 and RAM event probes timed out. Neither disk, compiler nor optimization is established as the sole cause. Local GCC timeouts further prevent a Clang-only conclusion.

The old raw files are unchanged and indexed with SHA-256 in `evidence/a1-history-review/review-v1.json`. A full original-binary live workflow diagnostic with the new immediate traceback runner and original 300-second bound is still pending the formal GCC resource window. Its eventual result will be labelled diagnostic. The historical 39-test result remains **UNRESOLVED**, and the original single failure remains **NOT_REPRODUCED by the earlier single-case diagnostic**. `historical_clang_asan_failures_closed=false` stays unchanged.

At this cutoff: implementation of the two test-tool changes is present; A1 product validation, historical diagnosis, push, candidate remote CI and formal PR review are not complete. No merge, model call, paid request or production access was performed. Later publication receipts identify the actual full SHA and CI separately.
