# Windows runner ABI correction — validation report (v2)

Versioned report, 2026-11-05, version 2. It documents the real Windows CI
failure of the published fix candidate `966d23c3d38b6ac29981b3b87efad0d5a81ad195`,
the ctypes ABI corrections applied this round, the new regressions and the
validation actually executed. It continues — and does not replace —
`audit-windows-runner-validation-20261105-v1.md` (SHA-256
`954ae47ff0a13371a6b008b9d53f66b1667fa10ab3084e90aa832f45d93bed81`) and its
JSON twin (`763299dd2e21cce40fb904745ab06ba1d6cfa5f9d975110c1e1b8b85be43b53a`);
everything v1 recorded for its own scope remains valid there.

## The observed native failure (published candidate 966d23c)

Push run **42028520881** (workflow `Windows native foundation slice`, run
number 96, `https://github.com/wmqfl861/wls861/actions/runs/42028520881`),
job **127248705818** (`windows-2022`), attempt **1**, head and checkout both
the full SHA `966d23c3d38b6ac29981b3b87efad0d5a81ad195`. Step 4
"Capture inherited image environment before selfchecks" failed between
2026-11-05T12:38:19Z and 12:38:22Z with exit 1. The captured trace shows
`capture_only_git_head → run_owned → Win32ProcessAPI.__init__ →
_declare_structures → JOBOBJECT_BASIC_LIMIT_INFORMATION →
('Affinity', ctypes.c_ulongptr)` ending in
`AttributeError: module 'ctypes' has no attribute 'c_ulongptr'`. The failure
happens while the Python structure declarations are evaluated, before the
kernel32 loader is reached and before any Job or process is created, so this
round's classification is **NATIVE_OBSERVED_INITIALIZATION_FAILURE** — a
newly observed initialization defect of the E002 supervision tooling, not the
original E002 dynamic counterexample (which stays `STATIC_CONFIRMED` with its
Windows reproduction `NOT_RUN`). Steps 5–9 of the job (runner selfcheck with
synthetic CTest processes, supervisor synthetic, supervisor native N1–N10,
image provenance, product build) were skipped in `jobs.json`; they were not
executed and are not recorded as passed anywhere. The run produced zero
artifacts (`artifacts.json` `total_count=0`; the artifact upload log confirms
no files); the remote capture-error spool directory referenced in the trace
was never downloaded and its content cannot be reconstructed — it is recorded
as missing, not restored. Only the actual Actions logs and the saved API JSON
were preserved (originals, SHA-256: `run.json`
`994e812dcf9aeb8c0ba06a27173d8b609b980ea2f0bad21ad47c44d4e0fb8052`,
`jobs.json` `d4d691345c655e69cc44344a45266be81b62b185911af388dcd3468cae7c5f56`,
`artifacts.json`
`244563f989470892c537c9a4622920af0d228ee4528f7d00c64a06d6cd95856c`,
fetch/checkout log `c95fb8631b3170243b23c1b6017a766641e635c56d8e0862c6fc4040f1ae8e8e`,
traceback log `c3334f23fb4404b5d26966d2139c15c1bfbeb4f0fcd7f29be67a1868716f6e06`,
artifact-upload log `c63fc197e0c9c65565f9fc01d83620addb56ccd3b2ad327a53e26fa96dd5cfde`).

## ABI defects corrected and their proof level

1. **Missing pointer-width integer carrier — observed.** Python's `ctypes`
   has no `c_ulongptr`, and on the LLP64 Windows target an `unsigned long` is
   32 bits, so `c_ulong` is not a valid fallback for `ULONG_PTR`/`DWORD_PTR`.
   `windows_process.py` now declares the module-level carriers
   `ULONG_PTR = ctypes.c_size_t` and `DWORD_PTR = ctypes.c_size_t`; `SIZE_T`
   remains `c_size_t`, `HANDLE` remains `c_void_p` and `DWORD` remains
   `c_uint32`. The `ctypes` module is never monkey patched, no `c_ulong`
   fallback is used and no 32-bit platform support is declared. This defect
   is the one actually observed in the CI trace and in this round's local
   pre-fix red run.
2. **`JOBOBJECT_BASIC_ACCOUNTING_INFORMATION` layout —
   STATIC_CONFIRMED_SDK_LAYOUT_MISMATCH, corrected.** The official
   declaration is four `LARGE_INTEGER` values
   (`ThisPeriodTotalUserTime`, `ThisPeriodTotalKernelTime`, `TotalUserTime`,
   `TotalKernelTime`) followed by the DWORD counters `TotalPageFaultCount`,
   `TotalProcesses`, `ActiveProcesses`, `TotalTerminatedProcesses` — x64 size
   **48** with the DWORD offsets **32/36/40/44**. The previous code placed
   `TotalTerminatedProcesses` at 36 and `ActiveProcesses` at 44 and appended
   three fields (`ActiveProcessesLimit`, `PeakProcessMemoryUsed`,
   `PeakJobMemoryUsed`) that belong to other structures, so a real SDK query
   would have been decoded with wrong counter offsets; the x64 size oracle
   said 72. The correction removes the foreign fields, restores the official
   order, and changes the size expectation 72 → 48. This was confirmed
   statically against the official declaration; it was **not** exercised by a
   real Windows query in this round (that requires the new CI run).
3. **`JOBOBJECT_BASIC_PROCESS_ID_LIST` header —
   STATIC_CONFIRMED_SDK_LAYOUT_MISMATCH, corrected.** The official header is
   `DWORD NumberOfAssignedProcesses` (offset 0) and
   `DWORD NumberOfProcessIdsInList` (offset 4), followed by the
   `ULONG_PTR` array (offset 8). The previous code declared a single-DWORD
   header and read the offset-0 value (the assigned count) as the number of
   copied entries. `job_process_ids` now reads the copied count
   (`NumberOfProcessIdsInList`), keeps the fixed 64-entry read-only buffer
   (x64 structure size 520, now also covered by the x64 size table), rejects
   a copied count above 64 explicitly instead of silently truncating into a
   complete-looking list, and preserves the original query-failure behavior.
   The list remains a read-only diagnostic — it is not exit proof, kill
   authority or PID identity.

The authoritative declarations were read from the official Microsoft Learn
pages for `JOBOBJECT_BASIC_ACCOUNTING_INFORMATION`,
`JOBOBJECT_BASIC_PROCESS_ID_LIST` and `UpdateProcThreadAttribute`
(`Attribute` is `DWORD_PTR`, `cbSize` is `SIZE_T`) during planning; the
corrections are pinned in this repository by independent SDK-order byte
oracles in the new regressions below, never by bytes generated from the
production `_fields_`. No other structure, deadline, permission, capture
flow or CI workflow was changed, and no unrelated aggregate, README or
workflow file was touched.

## Regression additions

`tools/test_windows_process_supervisor.py` keeps the original 13 synthetic
cases and the native N1–N10 scenarios unchanged and adds three synthetic
cases with their own result originals (they are in-memory checks; no process
is created, terminated or queried for real):

- `abi-declaration-sdk-layout` — calls the production `_declare_structures()`
  directly; checks the `ULONG_PTR`/`DWORD_PTR` carriers are pointer-width and
  unsigned with high-bit round-trips; decodes an independent
  `struct.pack('<qqqqIIII', …)` 48-byte SDK oracle through the production
  accounting type (reading back `TotalPageFaultCount=777`, `TotalProcesses=9`,
  `ActiveProcesses=2`, `TotalTerminatedProcesses=7`); verifies the two-DWORD
  PID-list header and, on x64 hosts, the full `X64_STRUCTURE_SIZES` table
  including accounting 48 and PID list 520.
- `abi-constructor-binding-smoke` — runs the real `Win32ProcessAPI.__init__`
  with only the platform identification, the kernel32 DLL loader and the
  last-error source substituted in the test (Linux `ctypes` provides no
  `get_last_error`); the fake DLL functions accept only
  `argtypes`/`restype` assignment and any Win32 call during construction
  fails the case; checks `UpdateProcThreadAttribute`'s `Attribute`
  (`DWORD_PTR`) and `cbSize` (`SIZE_T`) argtypes are pointer-width unsigned
  with high-bit values kept, HANDLE stays pointer-width and DWORD stays
  4 bytes; every patch restores on context exit.
- `abi-pidlist-returned-count` — a test `QueryInformationJobObject` writes
  only specified bytes (assigned=5, returned=2, two non-zero pointer-width
  PIDs) into the production buffer; the real `job_process_ids` must return
  exactly the two copied IDs, never the assigned count and never padding or
  unwritten slots; a returned count over the fixed 64-entry buffer must fail
  explicitly and a failed query must still raise `Win32CallFailure`.

`native_startup_checks` (Windows only, unchanged real constructor, real
nested-Job probe and x64 size table) additionally asserts the official
accounting counter offsets 32/36/40/44, the PID-list header/array offsets
0/4/8 and the pointer-width `DWORD_PTR`. No budget was added or relaxed: the
synthetic case budget (55 s per case locally, 60 s CI step), native 25 s
scenario budgets, 280 s internal total, 15 s startup/finalize, 300 s native
step, 900 s job and all product deadlines are unchanged, and no 11th native
dangerous scenario was introduced. These tool-level case counts stay
separate from the frozen native 5-suite/34-method catalog and from the Linux
95-suite registration.

## Validation actually executed this round (Linux, synthetic only)

All commands ran as ordinary user on the data disk with a private round
evidence directory, a task TMPDIR outside the checkout and
`PYTHONDONTWRITEBYTECODE=1`; per-command receipts (actual argv, cwd,
interpreter identity, source-file SHA-256 at run time, UTC start/end, exit
code, raw stdout/stderr and hashes) are retained in the private round
evidence, which this public report does not carry.

| Check | Result |
| --- | --- |
| New ABI regressions against the unmodified 966d23c production file (pre-fix red) | RED — exit 1; exactly `abi-declaration-sdk-layout`, `abi-constructor-binding-smoke`, `abi-pidlist-returned-count` fail with `AttributeError: module 'ctypes' has no attribute 'c_ulongptr'`; the 13 original cases still pass (two preserved runs: the first has a driver receipt-write defect and a zero-byte leftover receipt file, both retained as originals; the second is complete) |
| Supervisor synthetic selfcheck (`--mode synthetic`) after the fix | PASS — 16/16 cases (13 original + 3 new ABI), exit 0 |
| Runner synthetic selfcheck (`test_windows_ci_evidence.py`) | PASS — 31 tests, exit 0 |
| Image provenance source-binding selfcheck | PASS — 17 tests, exit 0 |
| Bundle capture selfcheck (`--source-root .`) | PASS — 6 tests, exit 0 |

One intermediate failed green attempt is preserved: immediately after the
production fix, the two constructor-path cases still failed because the first
test probe shim delegated `ctypes.get_last_error` to the real module, which
Linux `ctypes` does not provide. The shim then substituted the last-error
source — a substitution the plan explicitly authorizes for these tests — and
the rerun passed 16/16. The production file bytes did not change between
that attempt and the passing rerun; both attempts and their receipts are
retained. The red runs used the pre-fix production SHA
`e2d2f2d5ed3ef83d86ea49421622007c51047ab0505dab7729b41e22e634f965`; all green
runs used the corrected production SHA
`77b184076b1574b654e90208253bfe7b30c2545700ef0d1adae5e22203748693`
(test file at the passing rerun:
`6cc46f270d10ddd2fce48bad8e26e82730021e6d845803508db23e0e88558eee`).
These Linux runs are declaration/binding/layout regressions and supervisor
state-machine checks; they are not native Windows API executions and do not
substitute for native evidence.

## Not run this round (open states)

| Item | State |
| --- | --- |
| Windows capture step through the real `Win32ProcessAPI` on the new SHA | PENDING_CI — the 966d23c run failed before selfchecks; the corrected code has no hosted run yet |
| Supervisor native scenarios N1–N10 + startup ABI checks on the new SHA | PENDING_CI |
| Existing native slice (5 suites / 34 methods) on the new SHA | PENDING_CI — 5/34 is the unchanged frozen expected catalog from prior registration; the new run must be checked against the actual original method logs for names, counts, duplicates and skips, without lowering the expectation or counting tool cases into the 34 |
| Linux six-way matrix and the 95-suite catalog on the new SHA | PENDING_CI — retained by the existing Linux CI; results cannot be borrowed from an older SHA |
| Windows 11 x64 ordinary-user NTFS acceptance | NOT_RUN — no machine |
| Original E002 Windows dynamic reproduction | NOT_RUN — original defect remains STATIC_CONFIRMED / STATIC_CONTROL_FLOW_EVIDENCE |
| Historical image conflict | UNRESOLVED (unchanged) |
| Restricted private original hosting | BLOCKED (unchanged) |

## Disclosures carried from the post-hard-review disposition

These are evidence-interpretation corrections carried forward per
`post-hard-review-disposition-v1`; none of them claims a code fix in this
round:

- **H6-1** — `result.json`'s `owned_process_supervision` is a
  pre-finalization projection that omits the `source-snapshot-after` command.
  Final command-coverage conclusions must use
  `identity-finalization.json`'s `owned_process_supervision` plus the actual
  per-command receipts (including `source-snapshot-after`), cross-checked
  against source/binary identities and the absence of capture errors. The
  stale-aggregate code omission is not fixed by this round.
- **H6-2** — the `2026-11-05T12:20:00+00:00` timestamps in the v1 report pair
  and the E003 v3 pair are nominal values, not verified write times; earlier
  saved records contain identical report hashes, so the actual write time is
  UNKNOWN. The original report bytes and timestamps are preserved unchanged
  and `12:20` must not be used to date any execution or CI fact. This v2
  report records only real-clock times observed by the tooling.
- **H6-4** — the identity of `platform/native-foundation/README.md` is its
  actual 64-character SHA-256
  `b1334000a93a8ef49eaf3da5ac65cfe97bbe8274df7d607386d5ac16134a3427`;
  the malformed hash string in the earlier implementation response text is
  excluded from identity evidence and the README itself is unchanged.

## States and boundaries

| State | Value |
| --- | --- |
| IMPLEMENTATION_COMPLETE | true — limited to this round's 4-file ABI correction delta |
| LOCAL_VALIDATION_COMPLETE | true — limited to the local synthetic/tool checks listed above |
| PUSH_COMPLETE | false at report freeze — no Git writes were performed by the implementation stage |
| REMOTE_CI_COMPLETE | false — Windows hosted and Linux results for the new SHA are PENDING_CI |
| PR_REVIEW_COMPLETE | false — the fresh independent hard review of this delta has not run yet; coordinator owns acceptance |
| MERGED | false |

`windows_11_machine_acceptance=NOT_RUN`, `ordinary_user_security_acceptance=
NOT_RUN` and `full_native_runtime=NOT_IMPLEMENTED` are unchanged. No product
permission, ACL/SID/host assertion, product ProcessRunner or sandbox was
implemented; the Job Object remains a supervision custody design, not a
sandbox; no real model or paid call occurred. Native exit proof still
requires all three facts together (same-Job `ActiveProcesses == 0`, root
HANDLE signaled, actual exit code read); unknowns are never written as 0,
true or PASSED. The final SHA, its CI and its adoption will be recorded by a
new external versioned receipt after review and publication, avoiding a
self-referential follow-up commit.

Report generated at 2026-11-05T13:36:25+00:00 (real clock at write time).
