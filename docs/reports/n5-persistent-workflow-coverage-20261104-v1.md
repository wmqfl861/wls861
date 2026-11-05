# N5 persistent workflow coverage report v1 (2026-11-04)

Generated 2026-11-05T04:34:45Z (UTC), after all validation attempts stopped and owned processes were reaped.
Revised 2026-11-05T05:41:43Z (UTC; documentation-only revision: historical-source provenance, no-rerun wording scope, whole-row timings and count clarifications; no new test execution).

**Scope.** N5-1 persistent async workflow continuation on the clean test candidate: G1/G2 baseline coverage, the G2 fixture-cleanup red/green correction, and the frozen six-configuration local validation matrix, every configuration attempted exactly once at its first formal run. One additional same-configuration rebuild was interrupted before any second CTest and is recorded separately; it is not a matrix cell.

## Test candidate

- HEAD `f6141569e35a4cd1068701431cd88629b1046de6` (tree `a6d2eb13795478e3eb10e1a850698cf9d934685f`, parent `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123`)
- The formal validation matrix below executed at that HEAD on a clean checkout; this report's own commit does not exist at generation time.
- The G1/G2 baseline coverage and the fixture-cleanup red/green correction are earlier pre-commit historical evidence recorded at prior content revisions of `tests/persistent_event_runtime_tests.cpp`: baseline `2426e7079da60d2ccada997a931e351c037c09f6ba34c8c58e16db9f849f38c2`, correction red `0dbdf94fc9630c7997f0264e6b5d329f948f5ec6491f45f248b1b75d188fec16`, correction green `2e21bc8d41eb08942d3ff50643650e28e7ef7df86ac86a807277652bc84b0e5f` (identical to this HEAD's file content); they are not part of the formal matrix.
- Test source `tests/persistent_event_runtime_tests.cpp` SHA-256 `2e21bc8d41eb08942d3ff50643650e28e7ef7df86ac86a807277652bc84b0e5f`.

## Product bindings (relative path → SHA-256)

- `tests/persistent_event_runtime_tests.cpp`: `2e21bc8d41eb08942d3ff50643650e28e7ef7df86ac86a807277652bc84b0e5f`
- `tests/persistent_event_faults.hpp`: `1b0d5696bfd91420f8c14ccff193a76ec96f4fb74278e4f7d3d2c8033d4c62aa`
- `src/async_coordinator.cpp`: `038019d74a77b70f15c3fc0aeae90bf612d8cb5196a9c781dbb413d1f50610ae`
- `src/persistent_event_runtime.cpp`: `d5cd6fcab321d680ae726c37ca1faba533aa2b4517bd1ccf737915a790f701ba`
- `src/store_event_association.inc`: `47548c5bc602ad9c6e4a80224889bc12b09013ad49cfd3e0acf039e728d66030`
- `src/runtime_resources.cpp`: `3f23a349d15783ddeab14aa94c732724915d47538062dcc28b987d6c37dfff86`
- `tests/test_doctor_diagnostics_cli.py`: `8875d641cbf8d19c2c632f96fed2098a03532699c3561501891007274dbad54e`
- `tests/review_cli_smoke.py`: `dfb424f9c682ce67b0d280fc3f26ef8037df34614ee5ecdc723616992466a98c`
- `tests/gcc_asan_expected_tests.json`: `620fe97b1d0b502b26f6ffd6497769e7292423b78283467fd49ffe2816427efd`
- `CMakeLists.txt`: `2d69686302a4458e842314507892b8173c9a7584d04fe04967fa52727fe8ff8e`
- `Makefile`: `8a3b629296d9cfa807923f100abb5b0fdb73abbf123183d2d68726e89ee3fdf8`
- `scripts/run_gcc_asan.py`: `70f860bd3e0952a8620ce958a17c66d70e3d8ccd0405cc41a7492abb1aed6e14`
- `resources/runtime-manifest.json`: `f03bce7bb55b02f7740c81473c12301ecc437b35d980d629fb2d896196d4dc30`

Managed catalog `tests/gcc_asan_expected_tests.json` (620fe97b1d0b502b26f6ffd6497769e7292423b78283467fd49ffe2816427efd) registers 95 tests with deadline overrides async_coordinator=180s, persistent_event_runtime=180s, persistent_event_store=120s, store_open_compatibility=120s; names and deadlines were unchanged in every run of this report.

## Toolchain

GCC 13.3.0, Clang 18.1.3, CMake/Ctest 3.28.3, GNU Make 4.3, Python 3.12.3 (jsonschema 4.10.3), OpenSSL 3.0.13; ASAN_OPTIONS=halt_on_error=1:detect_leaks=1, UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1, no LSan suppression or preload.

## G1/G2 coverage

- **G1 `committed_wal_errors_preserve_async_original_outcomes`** — four commit-outcome cases (RESERVED/DISPATCH/SETTLEMENT/CLOSE) each verify the durable original transaction outcome is preserved when the API reports failure; result: persistent_event_runtime 17/17 (12.94s), persistent_event_store 30/30 (58.75s), async_coordinator 31/31 (31.80s), sync regressions passed — historical baseline recorded at test-source `2426e7079da60d2ccada997a931e351c037c09f6ba34c8c58e16db9f849f38c2`, before this HEAD. Baseline guard coverage on unmodified production code; no production defect reproduced.
- **G2 `foreign_destructor_joins_event_callback_before_releasing_writer_lease`** — event-mode callback parks behind a two-stage gate; foreign-thread destruction only stops and joins it while the real writer lease is held; recovery=UNKNOWN preserves the original PENDING reservation. Correction (test-fixture cleanup only, production unchanged; red recorded at test-source `0dbdf94fc9630c7997f0264e6b5d329f948f5ec6491f45f248b1b75d188fec16`, green at `2e21bc8d41eb08942d3ff50643650e28e7ef7df86ac86a807277652bc84b0e5f`, the latter identical to this HEAD's content): red 16/1 bounded rescued failure of `finished_on_own` after the old two-step cleanup, green 17/17 after one shared-path gate release before join; method catalog stays 17 and CTest registrations stay 95.

## Validation matrix (each configuration = its single first formal attempt)

| # | Configuration | Layout | Result | Detail |
| --- | --- | --- | --- | --- |
| 1 | GCC Release / test umask 0022 | outside_checkout | **FAIL (preserved)** | 93/95 passed; `doctor_diagnostics_cli` (1 subtest) and `review_cli_smoke` (3 subtests) failed with `unsupported: no compiled-compatible executable-relative runtime bundle`; ctest exit 8, 767.73s |
| 1b | same configuration, additional rebuild | checkout_child | **INTERRUPTED** | configure 0, build interrupted (130), no second CTest; not a matrix cell and not used for any gate |
| 2 | GCC Release / test umask 0002 | checkout_child | **PASS** | 95/95 in 763.23s (build 567s; whole row 1334.805121467s, within the 2400s budget, overrun 0); both CLI suites that failed in the outside_checkout layout pass here |
| 3 | Clang 18.1.3 strict Release | checkout_child | **PASS** | 95/95 in 1416.15s (build 538s; whole row 1962.887438016s, within the 2400s budget, overrun 0) |
| 4 | GCC Debug UBSan | checkout_child | **TIMEOUT** | build 2062s left 331s of the shared 2400s budget; CTest was stopped by the original timeout mechanism at 289s with 1/95 decided (r3_offline_acceptance passed, r3_offline_acceptance_cli started then interrupted, 93 not started; no JUnit produced); whole row 2440.890673581s including post-timeout evidence capture, exceeding the 2400s whole-row budget by 40.890673581s — an actual execution deviation; the original budget was not adjusted |
| 5 | shared GCC 13.3 Debug ASan/LSan runner | checkout_child | **INTERRUPTED** | configure/build/test-plan all exit 0 (build 1876.4s; 95-name validation passed); under ASan 7 tests hit their frozen deadlines (`r3_offline_acceptance_cli`, `store`, `workflow_store`, `acceptance_store`, `event_runtime`, `workflow_runtime`, `acceptance_runtime`); 14 passed, 7 Timeout, worker_tools (#22) started then interrupted, 73 not started; the outer timeout returned 124 and CTest received SIGTERM at ctest 1385.3s; runner summary NOT_COMPLETE; whole row 3340.206349293s including post-stop evidence capture/archiving, exceeding the 3300s whole-row budget by 40.206349293s — an actual execution deviation; the original budget was not adjusted |
| 6 | original complete Make asan recipe | checkout_child | **FAIL** | make step 2696s, whole row 2724.946123538s within the 3300s budget (overrun 0), recipe exit 2: ASan -O0 build complete (96 compile lines carried the explicit CPPFLAGS define; 60 top-level artifacts); the log's 16 RESULT lines comprise 15 native suite results plus 1 Python demo_smoke result (not 16 native suites); the sequential test phase stopped at `async_coordinator` (30 passed / 1 failed: `cross-pool resource heads progress before slot admission` exceeded its original 5s internal wait); `persistent_event_store`, `persistent_event_runtime` and all later recipe items were not reached — no ASan G-coverage pass and no CTest95 result is claimed |

**Layout classification.** The preserved first run used an outside_checkout build directory; the additional interrupted rebuild and all five configurations of this continuation used checkout_child directories. Layout and test umask differ between these runs at the same time, so cell 2 is not a single-variable umask comparison against cell 1, and no later result replaces the preserved cell-1 failure. Executable-relative runtime resource discovery covers only the executable directory and its parent; the two CLI suites failing in the outside_checkout layout and passing in the checkout_child layouts is consistent with that mechanism, though no matched control or fix verification exists here.

## Artifact digests (neutral ids)

- `row1.ctest-console`: `38817bf7d9dc1375010219505fb2c76609123994d696d70a32fe1fc68620cb9d`
- `row1.ctest-junit`: `aec3b4dc2c76d182145d73a0d489254ebbd98ab6e556f331c65e22cc513a6d01`
- `row1.ctest-lasttest`: `80dafdb82db853cf3f786e4a02646d17b3e6977e3e72686db70e6a86445a80c1`
- `row1-extra.exits`: `884189b8125090b290db01d77e1dfb3a9da3fabdaf4e5259d67b582e80ebfb4f`
- `row1-extra.build-log`: `d2e0b27d5bfbe3185d2aae279fab2893bd5c5ef7e872016b0ab61585fe0417cb`
- `gcc-release-0002.ctest-console`: `93733604ef4f5ca3e83d909a39e1f661a264c2ffe46e453cc4512c189941b4d4`
- `gcc-release-0002.ctest-junit`: `efae2b8c6b520f2a6d11da59d7b632133cdd7a5e9662f6769eb9c78349d1d91f`
- `gcc-release-0002.ctest-lasttest`: `2b983abf78c3c6ef89bc6748a81744f314399283d19469ff5de430dba74742ab`
- `clang18-strict.ctest-console`: `d318aa2115ec224dcafda054c8720663ce6885496bc91102050102d8835e3fc0`
- `clang18-strict.ctest-junit`: `2cf368d4051eee8e0c88a4c554d7f7f044e50fd1ec78820597dbb483fc62c21a`
- `clang18-strict.ctest-lasttest`: `b10d78484a8cd392c00488bd559806fae58fae7bb184ef49c9184fc5e6870acd`
- `gcc-ubsan.ctest-console`: `541e9342066ce58d40e95ac703d66cf4653489ad0b197b033c4b4a5108c4c8d5`
- `gcc-ubsan.ctest-lasttest`: `b6a9235008683695fdf152d4975934a609db284db6f4ff1255032b33ff9e8175`
- `row5.runner-summary`: `d79bfa24461576e7972802a4cf88f84dcb8b57646ec25b54304dd662f343aba3`
- `row5.ctest-console`: `1b8f83887b796726ca070c8095cfc1b715e23db081fd540220d22374cc7a470a`
- `row5.ctest-lasttest-partial-tmp`: `b74a605dfc496a9fee1bd97e6674411db03bf38a2d11f87581f8d9e1d0dfc2a4`
- `row5.ctest-evidence`: `abf294fd2d2927cd72213dde1d6881728ad3b229f5d492b0acc387b310a935e4`
- `row6.make-console`: `0158a151fe4174735c78bbac9966f6457768b519a874ea21958c8f3a161a3b2c`
- `row6.make-tail`: `6e33ec925a42d178e247f94c83d7a6055d71e501af3c36ce5fd93bd547a13c21`

ELF digests (checkout_child builds): row2 wls `fab236410c7e39cef14e5f18733aefe85b291a72cb5b30fc74726425d5bd9673`, wls861 `d0451ca45efef24c37d78353801fdcd046bdff71247b1089a2f8109700071d60`; row3 wls `af76ccfedc7f822573f05677e594e834e3cd6af0c85da4c9e2d29108b817ab31`, wls861 `268e74ac7640ca2c7b6cb50a965a48821dc8fc555decca468cd9e9c4c1f3ce18`; row6 wls `5aa5ce9461dc6e34c5d08c6e2b3dfed11aa5ba9826c311d435e7cbecdcb20814`, wls861 `0a4b4ad75de70868d6d15a267e525abe1d4fb6a147a566dec70aca7ec2bba37d`.

## Gates

- IMPLEMENTATION_COMPLETE: **true** — G1/G2 test implementation and this evidence/report closure only; an earlier interrupted implementation attempt's own conclusion remains unknown and is not restated here.
- LOCAL_VALIDATION_COMPLETE: **false** — full matrix not passed (preserved cell-1 FAIL plus TIMEOUT/INTERRUPTED cells).
- full_matrix_passed: **false** (2 PASS, 1 TIMEOUT, 1 INTERRUPTED, 1 FAIL, plus the preserved first-cell FAIL).
- PUSH_COMPLETE / REMOTE_CI_COMPLETE / PR_REVIEW_COMPLETE / MERGED: all **false** (not run).
- windows11: **NOT_RUN**; private_hosting: **BLOCKED**; historical_clang_asan_failures_closed: **false**.

## Notes

- Each of the five continuation configurations above is its single first formal run; none was rerun, reconfigured, targeted-retested or repaired after its recorded outcome, and the preserved first GCC Release/test-0022 run had no second CTest. The additional same-configuration rebuild (row 1b) completed configure and was interrupted at build (130) before any CTest; it is not a matrix cell, not a seventh configuration and not used for any gate.
- Row budgets were shared per-configuration deadlines (2400s normal, 3300s sanitizer) from each row's first step; exhaustion is the recorded outcome with the completed tail preserved. Whole-row elapsed times include post-stop evidence capture and archiving: rows 2, 3 and 6 stayed within their whole-row budgets, while row 4 exceeded its 2400s whole-row budget by 40.890673581s and row 5 exceeded its 3300s whole-row budget by 40.206349293s — recorded execution deviations, with no retrospective budget adjustment and no rerun.
- Directory-level checks (static catalog, runner test-plan, ctest --show-only=json-v1) are validations of the registration set and are never counted as test passes.
- This report contains only project Git identities, relative product paths and content hashes, toolchain/configuration facts, test names/deadlines/results and neutral artifact digests.
