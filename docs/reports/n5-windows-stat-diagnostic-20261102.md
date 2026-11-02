# N5 Windows evidence-reader field diagnostic

Input: `540fd7dd2bc8c0e0d4a03409ed05d0b732f58f78` on
`feat/n5-3-readonly-identity-20261102-csj777e9`, Draft PR #17. This follow-up
adds an observation only. **The Windows reader discrepancy is OPEN; no fix or
new native acceptance is claimed.** The earlier implementation report retains
its original time scope and local Linux results.

Two real native Windows workflow attempts failed at the new eight-method
provenance selfcheck:

| Event | Run / job / attempt | Original result |
| --- | --- | --- |
| push | [41442494083](https://github.com/wmqfl861/wls861/actions/runs/41442494083) / 125267525169 / 1 | unittest 5 failures, 1 error; native build step skipped |
| pull_request | [41442590688](https://github.com/wmqfl861/wls861/actions/runs/41442590688) / 125267846331 / 1 | unittest 4 failures, 1 error; native build step skipped |

These failure counts include subcases, not separate suites. Both runs name
branch head `540fd7d`; actual PR merge checkout is a separate identity. The
original run/job responses, failed logs and capture receipt are retained in
`evidence/windows-ci-first-failure-01` outside Git. Original failed-log SHA-256:
push `75eacdb6b7650a4ecfba8809aa8f06631c09874ffef80306bfe6aa1b43676ac9`,
PR `9572af47d008d72813e1294d4d99f198ae5f6b823bab16c1c072be0c3c1d4a2b`.

The failing boundary is `read_file` comparing path lstat, opened fstat,
post-read fstat and final lstat tuples of device, inode, size, mtime_ns and
ctime_ns. The saved tracebacks do not show which field differs. Platform field
semantics/precision and a real observed change are distinct hypotheses; neither
is accepted from the traceback alone. In particular, this change does not remove
inode/ctime, ignore precision, retry until success or weaken any expected error.

`tools/windows_stat_probe.py` observes three dedicated synthetic temporary files:
new bytes, same-length overwrite, and an explicit nanosecond timestamp. It emits
the four Python stat observations, the original guard's differing field names,
and pre/post same-HANDLE FILE_BASIC_INFO and FILE_ID_INFO values using the CRT
descriptor's actual Windows handle. Original native 100 ns timestamps and exact
integer Unix-nanosecond conversions are kept. No real path or data content is
printed, no input artifact is opened, and the fixture is removed afterward.

The existing selfcheck calls this probe only on native Windows before running
the same eight unittest methods. The probe is not an added product suite, not
a replacement reader and not an assertion that a given identity is safe.
Unavailable observations are explicitly marked unavailable. All production
reader guards, selfcheck assertions, expected failures and time limits remain
unchanged. Linux can verify Python syntax and those same eight methods; only
a subsequent native CI run can supply the requested Windows field values.

The Linux syntax check and unchanged eight-method tool selfcheck were executed
once for this diagnostic candidate: actual exit 0, 8 tests in 0.273 seconds;
source hashes before/after match. Native probe execution is NOT_RUN_ON_LINUX.
The exact command, original output and source hashes are retained in
`evidence/windows-stat-diagnostic-01/selfcheck.receipt.json` outside Git. This
check did not rebuild C++ or run the product matrix.

The next decision depends on that actual observation. A correction must preserve
object identity, change observation, bounded EOF/size/hash and before/after path
binding, using comparable platform facts. New native output will be bound to its
own run/job/attempt/checkout and saved separately from both original failures.

No C++ source, build configuration, native method catalog, n5-0 product source,
old report or frozen evidence was changed. Windows 11 ordinary-user acceptance
remains NOT_RUN and the deployment SID/build policy remains POLICY_PENDING.
