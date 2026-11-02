# N5 persistent Store fixture correction — 2026-11-02

The strict remote run for Store commit
`28cf4b58d790984e691b59fb41959a1631173712` compiled and executed all 93
registered suites: 85 passed and eight failed. The new persistent Store suite
executed all 17 methods, with 14 passed and three failed. This report records
the subsequent diagnosis and test-only correction; it does not change the
earlier published source or compilation checkpoint reports.

Diagnostic-only commit `9031772e7e3738844fe1f13d28002b1b1a39ea41` retained
the exact expected error-code assertions and added the actual code, message,
and caller location. The integration owner's targeted rerun then observed:

| Method | Expected | Observed | Cause |
| --- | --- | --- | --- |
| T1/T2/T3 fault rollback | `storage` | `conflict`, `step: T1 fault` | SQLite `RAISE(ABORT)` reports `SQLITE_CONSTRAINT` |
| T4 fault rollback | `storage` | `conflict`, `step: W11` | Same constraint injector |
| 410 history upgrade | `not_ready` | `invalid_input`, `invalid typed ID prefix` | Fixture passed `enable-old` to `new_id` |

The original integration log is retained outside the repository at
`evidence/n5-integrated-targeted-build-04/ctest.log`, SHA-256
`6cf170050184b196c55209acc6a24813e67e81ffb817b16e243ba90f758b6433`.
The full remote failure log remains at
`evidence/n5-store-28cf-ci-failure-02/job.log`.

`tests/persistent_event_faults.hpp` now registers a test-only, zero-argument
SQLite function through `sqlite3_auto_extension` before a fixture Store opens.
The function calls `sqlite3_result_error_code(SQLITE_IOERR)`. The T1/T2/T3
and T4 triggers invoke this function at their original boundaries. They still
require `ErrorCode::Storage` and retain every rollback, response-retention,
budget, open-ID, and halted-application assertion. The upgrade test uses the
valid `enable` ID prefix and still requires `ErrorCode::NotReady`.

No production source, API, migration, asset hash, error mapping, or CTest
registration changes. Local compilation and product-test execution are
reserved to the integration owner; the corrected fixtures are **NOT_RUN** at
this report's checkpoint. `git diff --check` is a static formatting check,
not execution evidence.
