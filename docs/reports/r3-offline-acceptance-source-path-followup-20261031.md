# A2 original delivery source-path assertion — 2026-10-31

This supplement preserves the next real failed candidate and corrects one test
expectation. No production code, evaluator, model fixture, file-verification
assertion, test count or deadline changes.

PR run [41188833840](https://github.com/wmqfl861/wls861/actions/runs/41188833840)
at head `a35866ebd94a6c5b61a7d37e33a1c50353ff3f63` produced the completed
[Clang-18 job 124501222565](https://github.com/wmqfl861/wls861/actions/runs/41188833840/job/124501222565).
Its complete CTest invocation recorded 90/91 and exit 8. The CLI suite ran ten
methods: nine passed, and `test_03` failed with:

```text
AssertionError: '/work/result.txt' != 'result.txt'
Ran 10 tests in 9.588s
FAILED (failures=1)
```

`test_01` again passed all ten real Store/evaluator/accounting cells.
`test_02` and `test_04` through `test_10` passed. In `test_03`, the multi-Agent
delivery's manifest, all file bytes/hashes/sizes and exact file set checks had
completed before this path assertion. The corresponding single-Agent checks
were not reached in that failed method. The suite and CI job remain failures;
their temporary Store directories were not exported.

The original job log is retained as
`evidence/a2-candidate-ci-a358/pr-clang-failure-log.stdout` under the data-disk
task root, SHA-256
`67488fcf00b9912dfe1815efcf519614c43b6da2848948c7364c4c4fe8e90da5`.
Original source, model arguments and the failure/input receipt are frozen in
`evidence/a2-source-path-20261031/`. The prior reports and failed logs remain
unchanged.

The original versioned `scripts/offline_acceptance/v1/model-arguments.json`
declares `/work/result.txt` in `submit.deliverables`. Its SHA-256 is
`b511e4d02416cc15ac794ab3ae5660ca109ba008aef71d77b04b53ae60d7ab00`.
`OfflineRuntime::submission_snapshot` requires a unique `/work/` path and keeps
that complete path in the captured artifact. `Store::prepare_delivery` copies
it unchanged into `manifest.artifacts[].source_path`. Existing workflow Store
tests use this same full virtual path. The A2 test's relative literal was wrong.

The corrected assertion compares the manifest's source-path list exactly with
the original package fixture's `submit.deliverables` list. It neither strips
`/work/` nor accepts alternative spellings. The complete package and original
candidate byte checks remain intact. Coordinator `/root` directed this exact
scope after the actual source/fixture investigation; this does not constitute
successful business execution or formal A2 acceptance.

The CLI test source changes from SHA-256
`7c038c6a725de33d6c26335fe7f00d580eada514786467ed69c2adc26860d583`
to `f54522421500f49359c4e019bcc06d2ebd329420dd04a6dca11bdc0d78db6b71`.
Only one assertion site changes. As UID 1000 with Python 3.12.3 and data-disk
`TMPDIR`, `python3 -B tests/r3_offline_acceptance_tests.py SOURCE_ROOT` passed
9/9, exit 0; the exact command and raw output are retained in `contract-after.*`.
`git diff --check` returned 0. This lightweight import/lifecycle check is not the
ten-case native CLI suite. The pre-commit remote check still reports main at
`a6b8559018a599d7a806b05942857a56d69a00ce`.

At this report's source freeze the new business assertion is **NOT_RUN**.
The combined C++ Release build remains frozen at
`b8a5d7d8edec483e7923ed1eba4aa5e9656040ae`; this separate A2 checkout does not
modify that running build. A later dedicated window must run and retain the
actual ten-cell package and complete CLI suite, followed by full validation
and final-head CI. A2 `IMPLEMENTATION_COMPLETE`, `LOCAL_VALIDATION_COMPLETE`
and `REMOTE_CI_COMPLETE` remain false. Publication is recorded separately.
No live supplier, paid request, production operation or merge is claimed.
