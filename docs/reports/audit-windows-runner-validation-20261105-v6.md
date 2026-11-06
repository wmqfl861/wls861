# Windows runner validation — configure pre-termination owned-Job diagnostics (v6)

- generated_at_utc: 2026-11-05T23:47:30+00:00 (tool clock at write time; not independently verified)
- report_version: 6 (plan version is independent: this batch follows plan-final-v7)
- scope: local synthetic validation of the configure-only read-only owned-Job membership
  diagnostic added to the Windows CI runner; not a native Windows run, not a product
  validation, not a fix of the configure failure itself
- source identity at generation:
  - `platform/native-foundation/tools/run_windows_ci.py` sha256
    `24bf18ddf2a08ee81ec3bd549a0a569550227f1a08833ed9afccad73703408e7`
  - `platform/native-foundation/tools/test_windows_ci_evidence.py` sha256
    `79861e51aaa4dab42da8237e7cd9fc7f5534684fbf67d31a190292bada5a2cc1`
  - `platform/native-foundation/tools/windows_process.py` unchanged
    `a1fa6e7a4477bc819b53c82edfc0c62133c04f52485a5e5a23eba100187e2c02`
  - `platform/native-foundation/tools/test_windows_process_supervisor.py` unchanged
    `367af8d18c19fd95b40c784397b21f6b723c16a35fdd84a65e4f895db51d6bd4`
- repository state at generation: branch `fix/audit-windows-evidence-20261105-adp35vg4`,
  HEAD `2f22823a0794ceec7dd2f330fa64f3d6511b5c29`, only the two owned tool files modified.

## 1. What this batch changes

`run_windows_ci.py` gains a configure-only diagnostic observer
(`_ConfigureJobDiagnosticsProxy`) around the unchanged real
`windows_process.Win32ProcessAPI`:

- wired only for the `configure` command; every other command entry (build, ctest,
  discovery, source snapshots, capture-only) keeps the original unsupervised-by-proxy
  call with the same deadlines;
- the proxy and the same `run_owned` call share one clock object (the original
  `_RealClock` semantics on the real path, one injected clock in the synthetic path);
- it records the created-and-successfully-assigned Job handle, the root handle/PID and
  every original accounting query, each forwarded exactly once with its original value
  or exception;
- exactly once, immediately before forwarding an already-decided `TerminateJobObject`
  for that exact Job, it takes one bounded read-only membership sample:
  - 200 ms cooperative budget checked against the shared clock before every new Win32
    query; the budget consumes the original 5 s cleanup window and is never reset or
    extended; overrun is recorded (`TIME_BUDGET`) without any deadline change;
  - one `job_process_ids` list query on the exact assigned Job handle (fixed 64-entry
    capacity, original error semantics; a real query failure records `null` +
    `QUERY_ERROR`, never `[]`; a successful empty query records `[]`);
  - at most 8 returned PIDs inspected in return order; duplicates recorded and never
    re-opened; the full returned list (up to 64) is retained and the un-inspected count
    is explicit; the list is declared a non-atomic snapshot;
  - per member: `open_process_snapshot` (QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
    inherit=false), immediate same-handle `GetProcessId` re-confirmation and an
    `IsProcessInJob(process, exact non-NULL Job, BOOL*)` check; only after that
    same-Job confirmation are creation FILETIME, `QueryFullProcessImageNameW(flags=0)`
    and a zero-millisecond `WaitForSingleObject` state read;
  - only two extra Win32 bindings are declared
    (`IsProcessInJob`, `QueryFullProcessImageNameW`) with pointer-width HANDLEs, a
    32-bit BOOL/DWORD and a 4096-WCHAR image buffer whose size parameter counts
    characters; a truncated buffer is never published as a complete path;
  - every observation handle is closed in `finally` with no TERMINATE right and no
    diagnostic PID kill; the original `TerminateJobObject` is forwarded exactly once
    with the same Job handle and exit code, and its original exceptions, type and
    Win32 error are preserved unchanged;
- the receipt gains one additive field `configure_job_diagnostics` under the existing
  `wls-owned-process-receipt-v1` (both the normal return and exception-carried
  receipts). **Consumption contract: the field is optional; a missing
  `configure_job_diagnostics` means NOT_COLLECTED — never an empty list, zero, a
  NOT_REQUESTED claim or any success proof.** `NOT_REQUESTED` is written only when the
  observer actually ran and no TerminateJobObject was forwarded for its assigned Job.
  Historical receipts are not back-filled.

`test_windows_ci_evidence.py` gains exactly three tests (31 → 34), driving the actual
runner configure branch and the actual `run_owned`/`_closeout` with low-level injected
Win32-API/clock fakes (the observer itself is never stubbed):
`test_configure_cleanup_member_observation`,
`test_configure_diagnostic_partial_and_bounds`,
`test_configure_diagnostic_binding_and_exception_fidelity`.

## 2. Observed native status carried forward (unchanged facts)

Two real windows-2022 runs of HEAD `2f22823a0794ceec7dd2f330fa64f3d6511b5c29`
(push 42093312173, pull_request 42093324882, both attempt 1, same tree
`2a0909ea8d9c99d7c037e6a5a3b6a64dac7e5e44`):

- steps 4–8 succeeded; step 9 failed; step 10 retained the evidence (artifact_count 1
  per run); native supervisor selfcheck 11/11 PASS in both runs;
- in both runs the `configure` command exited 0, its 1 s natural settle expired with
  owned descendants still present, the owned Job was terminated
  (`TerminateJobObject`, exit 1) and the same-Job ActiveProcesses query then observed
  zero: cleanup `PROVEN_ZERO`, `tree_exit_proven=true`, outcome
  `FAILED_DESCENDANTS_AFTER_ROOT_EXIT`;
- that cleanup proof is not execution success and does not convert the failure into
  COMPLETED;
- the pre-existing receipts retained no pre-termination member identity
  (F7-DIAGNOSTIC-GAP); which program remained, its origin and why it did not exit
  remain UNKNOWN (F7-CAUSE). Nothing in this batch identifies the survivor or changes
  its behavior: no /nr, no /p, no server switch, no generator/toolset change, no
  global shutdown, no extended deadline, no breakaway;
- the CMake `pthread` probe text in configure.stdout is toolchain probing noise, not
  the top-level failure cause;
- product build/ctest/install/host-capabilities/detached stayed NOT_STARTED;
  build_completed=false with complete/unchanged source pairs and incomplete binary
  pairs.

This batch adds local synthetic evidence only; the diagnostic has not yet run on real
Windows (PENDING a future natural CI run of the published candidate; no re-run of the
same SHA is planned here).

## 3. New local synthetic results (this batch)

Private evidence root `validation-configure-diagnostics-01` (data disk; 0700/0600;
`--no-optional-locks` git reads; raw unstripped porcelain bytes around every command;
interpreter identity recorded per receipt).

RED (baseline runner `759aaf45…` unchanged, frozen test file identical across
RED/GREEN):

- attempt-1 RED (oracle `3354467d…`): 34 ran — 31 pass, 1 fail, 2 errors; the failing
  new flow test hit an oracle fixture defect (`_FlowFakeAPI.open_output_spool` did not
  create the spool file like the real `CREATE_ALWAYS`, so `source-snapshot-before`
  capture was incomplete before configure ran). Classified OTHER_FAILURE; the attempt
  is preserved whole (`attempt-1-oracle-defect-note.json`, `red/`) and not re-bound.
- attempt-2 RED (oracle `1223b810…`): 34 ran — 31 pass, 1 fail, 2 errors with the
  intended shapes:
  - `test_configure_cleanup_member_observation` — **BEHAVIORAL**: the original
    configure failure was fully reproduced through the real runner branch and the real
    `run_owned`/`_closeout` (root exit 0, settle EXPIRED, TerminateJobObject forwarded
    and succeeded, cleanup PROVEN_ZERO, outcome FAILED_DESCENDANTS_AFTER_ROOT_EXIT,
    MSBUILDDISABLENODEREUSE task-level, 180 s deadline) and the receipt lacked
    `configure_job_diagnostics` — exactly the missing pre-termination member identity;
  - the other two — **MISSING_SYMBOL** (`_ConfigureJobDiagnosticsProxy` /
    `_declare_configure_diagnostic_bindings` absent in the baseline runner); these do
    not count as behavioral counterexamples.
- attempt-2 first GREEN (34 ran — 31 pass, 1 fail, 2 errors) exposed two further oracle
  defects (a non-portable `patch.object(ctypes, 'WinDLL')` target without `create=True`;
  a wrong expected buffer value 8192 instead of the returned 4096) and one runner
  defect (the triggered record omitted `observer_ran`). All preserved
  (`attempt-2-oracle-defect-note.json`, first-GREEN receipts); the oracle was repaired
  under a new number.
- attempt-3 RED (oracle `79861e51…`): 34 ran — 31 pass, 1 BEHAVIORAL failure, 2
  MISSING_SYMBOL errors (`red-3/red-classification.json`).
- GREEN-3 (34 ran — 33 pass, 1 fail) exposed one runner defect (the extra-binding
  branch keyed on the constructor argument instead of the actually created underlying
  API, so a production-constructed proxy over a patched class missed the injected
  stubs); fixed on the runner side.
- **GREEN-4 (final): runner 34/34 OK (exit 0)** with the attempt-3 oracle bytes
  unchanged (RED/GREEN test-file identity diff 0 bytes; runner GREEN sha
  `24bf18ddf2a08ee81ec3bd549a0a569550227f1a08833ed9afccad73703408e7`).

Other local suites re-run at GREEN-4 with the same sources (exit 0 each):
supervisor synthetic **20/20 PASS**; image provenance **17 OK**; bundle capture
**6 OK**. Expected totals 20/34/17/6 hold. The tool-test count 34 does not change the
Linux 95-suite registration or the native product 5 suites / 34 methods.

## 4. Prior local results this builds on (not re-run here)

implementation-05/hard-review-05 recorded local 20/31/17/6 for the fixture-entry
repair batch; those numbers belong to that batch's sources and are superseded here
only by re-execution, not by reinterpretation.

## 5. Preserved disclosures and limitations

- H6-1: result.json `owned_process_supervision` remains a pre-finalization projection;
  final command coverage still requires identity-finalization plus the per-command
  originals including source-snapshot-after.
- H6-2: historical report timestamps are nominal, not verified write times.
- H6-4: the previously published malformed README hash remains excluded from identity
  evidence; the verified README digest is unchanged.
- D03-1..4 (system-temp staging, environment metadata replacement, manifest
  transcription typo, report mode normalization) and D04-1..7 (failed fixture attempt
  preserved and rebuilt, hand-transcribed precheck, empty-directory recovery,
  temporary copy loss, unverified timestamps, non-raw historical porcelain, freeze
  scope) remain open process facts; later disciplined evidence does not erase them.
- hard-review-04's per-receipt disclosure limitation and implementation-05's attempt-1
  oracle defect + session sidecar + frozen private `__pycache__` findings remain
  disclosed.
- This batch's own deviations: three numbered oracle attempts (attempt-1 fixture
  defect, attempt-2 two oracle defects + one runner field omission, GREEN-3 one
  runner binding-branch defect), each preserved and repaired under the plan's
  new-attempt rule; no failure original was deleted.

## 6. Release states (kept separate)

- IMPLEMENTATION_COMPLETE: source changes for this batch only (local, uncommitted).
- LOCAL_VALIDATION_COMPLETE: synthetic 20/34/17/6 at GREEN-4.
- PUSH_COMPLETE: false (no Git writes in this task).
- REMOTE_CI_COMPLETE: false — the diagnostic candidate has not run on real Windows;
  future natural CI is PENDING, and an un-triggered diagnostic must be read as
  NOT_REQUESTED with a missing field read as NOT_COLLECTED.
- PR_REVIEW_COMPLETE: false (no formal review or acceptance).
- MERGED: false.

Standing states: Windows 11 ordinary-user NTFS NOT_RUN; historical image conflict
UNRESOLVED; private original hosting BLOCKED; full native runtime NOT_IMPLEMENTED;
historical Clang/live_workflow findings remain open with the coordinator. This report
is not a PR acceptance or release approval. Engineering model request counts are
recorded by the stage entry receipts of this batch; unknown values stay unknown. No
private local absolute paths or credentials appear in this report.
