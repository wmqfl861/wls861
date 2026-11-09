# Controls development snapshot — BLOCKED

Status: **SOURCE_PREPARED_NOT_EXECUTED / BLOCKED**. This snapshot preserves
unfinished Controls work. It is not a completed Controls feature, a product
acceptance result, or a claim that the tests pass.

The source baseline is `4987f2c6a6fc0da4b91e53d3689019ba3f0e8631`.
The eight files below are one complete source/build/test snapshot. Their
content hashes identify the source under discussion; this report is additional
documentation and is not included in production-line accounting.

| Source path | SHA-256 |
|---|---|
| `src/controls_detail.hpp` | `68daf61f94b7f7851a2d055e7b09ab3c32cb52897dfde89a278dbd9e66c4017a` |
| `src/controls_clock_posix.cpp` | `4e9692a20f5e3ea5aa7f4b42a8e7e8430b86e4d0d6ff708fb425fd1fd4e9f8a4` |
| `CMakeLists.txt` | `e3404ffce3112ff0e192a0c79f30c4ac868ea832618c9f20a53c07b0aa9545d1` |
| `Makefile` | `f0567a3d321f7fbe07b86be3bbf2a9f32eac675d8d2545364d7ef2a634aeef1b` |
| `tests/controls_detail_tests.cpp` | `113b81921d5e862825c5aa9747a83248610b02af7a03511811e2cf48dbbd1890` |
| `tests/live_model_store_tests.cpp` | `4dd4668de67a89e49f8111043456f6d2460cd7b2b39a2a8c3e6e573f4122c20d` |
| `tests/event_runtime_tests.cpp` | `b6a3e5d253aa64b2a38f6753f73aa891c3c2393af3af5f64496c34f0e95517de` |
| `tests/test_live_workflow_cli.py` | `4a1165b9ef796cc3680f1cf9f1e72fed9eb0f49ede2451a0c4d0e1b91e685e52` |

The recorded whole-branch source delta is 790 production additions plus
4 production deletions (794), and 772 test additions with no deletions
(1566 total source/test additions plus deletions). The four-test preparation
added 303 of those test lines, with no deletion of an earlier assertion or
registration. The prepared test counts are 24→27 for controls_detail,
19→21 for live_model_store, 14→15 for event_runtime, and **11→14 Python
test methods** for test_live_workflow_cli. These are source counts, not executed
test results. The current branch count does not prove that a future complete
Controls or combined product PR fits the 800-production-line limit; every
actual product PR must still be counted over its complete fixed-base candidate.

## Known defect and unexecuted validation

`remaining_milliseconds_floor` still adds the millisecond tail without a
checked addition after the checked seconds multiplication. With now={0,0}
and bound seconds=9223372036854775, a nanosecond tail of 807000000 should
produce INT64_MAX, while tails of 808000000 and 999000000 must return typed
overflow. The latter two currently expose signed-overflow undefined behavior.
Three separately registered regression inputs are prepared. The minimum
checked-add repair has **not** been applied to this snapshot.

No Controls configure, build, test, arithmetic RED/GREEN, sanitizer validation,
or W/D/S/R business validation has been executed for these prepared changes.
An independent S0 validation route is planned: preserve the unmodified source
and test bytes, obtain actual assertion RED, apply only the checked-tail repair,
and rerun the same 27-case test with the same Release toolchain/flags followed
by affected UBSan validation. It still requires an exclusive product slot and
actual time/space/toolchain bindings for the bounded attempt. A plan or a
limited source review is not runtime evidence.

## Remaining Controls work

- **W / budget warning:** the B=125, E=100 crossing input is prepared; durable
  warning behavior and the full accounting/capacity/replay regressions remain
  unimplemented or unqualified. Original ledger and budget guards remain binding.
- **D / request deadline:** the one-minute input is prepared. Its comment's
  61.5-second wait estimate does not prove that the entire suite fits its
  unchanged 90-second timeout; the loop permits 75 seconds of sleeps plus
  other work. A generic caught Error and zero attempts do not prove the
  required typed deadline stop or the original admission deadline identity.
- **S / stagnation:** Correction qualification is still absent. The scripted
  `../outside.txt` failure and a FAILED feedback field alone do not prove an
  eligible failure class. Permission and other stronger stops cannot become
  stagnation. This input must not be counted as Controls RED or acceptance
  before the real classifier, frozen source and feedback are qualified.
- **R / bounded driver and clean31:** old-entry compatibility and new-entry
  rejection inputs are prepared. Missing run-live/--scripts-file behavior is
  an API gap. The exact pricing-41 30-step/1-step bodies and new report
  assertions remain incomplete. An old-command rejection does not establish
  a single-run 30+1/1271 result, three completed source-bound experiences,
  no third batch, or business_delivered=false.

Full W/D/S/R behavior, Correction integration and the complete non-root
Release/dual-umask/Clang/ASan/UBSan/Make/CMake-CTest/legacy/SingleAgent/clean31
matrix remain open. No existing permission, UNKNOWN, claim, budget, deadline,
Store 410, historical SQL or safety-assertion requirement is waived.

## Evidence limits and corrected source facts

Earlier local source-preparation reviews were limited in scope. They do not
combine into a complete snapshot review or a product GREEN. Publication of
this report and the complete eight-file snapshot requires a new independent
source review over that exact object; the accepted scope may only be the
integrity and factual accuracy of this explicitly BLOCKED development snapshot.

The local history retains the earlier source stage's 22.691521-second deadline
overrun, unreliable future timestamp fields, inherited shell umask 0077 and
an in-place evidence correction. Later successful timing does not erase those
facts. A later source-preparation stage used a differently named evidence
directory through an explicit route mapping and had a missing review-session
field linked externally rather than rewriting the old receipt. Its inherited
shell umask and per-command umask are distinct facts.

Two source-accounting corrections are retained in this report: the clock
source and original author records have the correct aa7f hash shown above;
the aa7b value was in the implementer's expected-hash list, not in those author
records. A subsequent review's claim that the list had no aa7b was contradicted
by the retained original bytes. The CLI test-method count is 11→14; earlier
12→15 reports were inaccurate. Original local records remain preserved; this
public report includes only their safe factual disposition.

Windows 11 remains **NOT_RUN**. Private evidence hosting remains **BLOCKED**.
Real or paid WLS provider calls for this work remain **0**. Private plans,
workflow records, receipts, raw databases/wire output, credentials, generated
builds and user data are excluded from the repository.

This is a development-branch snapshot only. A normal push does not establish
product acceptance, waive final-commit CI, create a new PR boundary, or grant
merge authority. CI results must be reported for their actual commit and
status. Open PRs, including drafts, remain limited to three; merging remains
with the owner. Finish the already authorized work and publication handoff,
then stop without adding features.
