# Windows runner ABI order correction — validation report (v3)

Versioned report, 2026-11-05, version 3. It exists because the independent
hard review of the v2 candidate (`hard-review-02`, scope
`SOURCE_AND_LOCAL_SYNTHETIC`) returned **FAIL** with blocker **B-1**: the
`JOBOBJECT_BASIC_ACCOUNTING_INFORMATION` declaration written by the v2 round
placed the four `LARGE_INTEGER` fields in a non-official order
(`ThisPeriodTotalUserTime` first), and that wrong order was labeled "official"
in the production comment, fixed into the regression oracle, and asserted in
the v2 report text. **Version 2 is therefore not the latest acceptance
report.** The v2 pair (SHA-256 `fb315b7c3a3585c3f943bc80b63d69fa550987ed9f4cb83ad13bb0f7b064e676`
md, `c2190666d93347133d67498d1f98af82358a91b5ce4305c29c411651b3c28898`
json) is preserved byte-for-byte as a historical candidate original: its local
green runs are real runs of what was then a wrong-order oracle, and they do
not prove the official SDK order. Specifically withdrawn: the v2 statement
that "the official declaration is four LARGE_INTEGER values
(`ThisPeriodTotalUserTime`, `ThisPeriodTotalKernelTime`, `TotalUserTime`,
`TotalKernelTime`)" and the v2 JSON `ABI-2` claim "official field order
restored", together with any implication that the v2 candidate was
acceptable. This v3 records the B-1 correction, its dedicated pre-fix red, the
fresh full local green, and the unchanged open native states. When v2
conclusions are quoted anywhere (including the PR 39 context), this B-1
correction must be quoted with them.

## B-1: what was wrong, who introduced it, and what is now true

The published 966d23c baseline already declared the four time fields in the
correct order (`TotalUserTime` first); that part of the baseline was right.
The v2 implementation round rewrote the struct, kept the correct DWORD tail,
but reversed the four `LARGE_INTEGER` names — then described that reversed
order as the "official SDK layout" in the source comment, asserted it as the
expected official order in `abi-declaration-sdk-layout`, and re-stated it in
the v2 report. That is a static SDK-order regression introduced by the
implementation, found by review, not by runtime. No source code read the four
time fields, so no runtime misread existed — that mitigation was recorded by
the review and does not waive the ABI, oracle or report defect.

The official declaration (Microsoft Learn,
`JOBOBJECT_BASIC_ACCOUNTING_INFORMATION`) is:

```c
typedef struct _JOBOBJECT_BASIC_ACCOUNTING_INFORMATION {
  LARGE_INTEGER TotalUserTime;
  LARGE_INTEGER TotalKernelTime;
  LARGE_INTEGER ThisPeriodTotalUserTime;
  LARGE_INTEGER ThisPeriodTotalKernelTime;
  DWORD TotalPageFaultCount;
  DWORD TotalProcesses;
  DWORD ActiveProcesses;
  DWORD TotalTerminatedProcesses;
} JOBOBJECT_BASIC_ACCOUNTING_INFORMATION;
```

x64: size **48**; offsets **0/8/16/24** for the four `LARGE_INTEGER` values
(Total first) and **32/36/40/44** for the DWORD counters. The correction this
round restores exactly that first-four order in
`platform/native-foundation/tools/windows_process.py` and corrects the
comment; everything the v2 round had fixed correctly stays: size 48, DWORD
segment order `TotalPageFaultCount/TotalProcesses/ActiveProcesses/
TotalTerminatedProcesses`, the module `ULONG_PTR = DWORD_PTR = ctypes.c_size_t`
carriers, `HANDLE = c_void_p`, `DWORD = c_uint32`, the two-DWORD PID-list
header with `job_process_ids` reading the copied count, the fixed 64-entry
buffer with explicit over-limit rejection, preserved query-failure behavior,
and the read-only diagnostic boundary (no exit proof, no kill authority, no
PID identity). No other production function changed.

## Correction delta and file ownership

This round writes exactly four repository files:

| File | Change |
| --- | --- |
| `platform/native-foundation/tools/windows_process.py` | first four accounting fields restored to the official order; comment corrected |
| `platform/native-foundation/tools/test_windows_process_supervisor.py` | literal official-order oracle with expected/actual reporting, four distinct time-value decode assertions, all-eight accounting offsets (0/8/16/24/32/36/40/44) in the synthetic case and in `native_startup_checks` |
| `docs/reports/audit-windows-runner-validation-20261105-v3.md` (new) | this report |
| `docs/reports/audit-windows-runner-validation-20261105-v3.json` (new) | machine twin of this report |

The two v2 report files are inherited candidates, preserved unmodified.
Against the 966d23c HEAD the cumulative candidate set is six files (two
modified Python files, the preserved v2 pair, this new v3 pair); the four
files above are this round's writes and the six-file total must not be
presented as four. No workflow, CMake/Make, catalog, README, aggregate, SQL,
rule or other-worktree file was touched; the eleven protected files listed by
the round-02 mechanical check were re-hashed unchanged this round. No budget,
deadline or case-count constant changed: still 16 synthetic supervisor cases
(13 original + 3 ABI), N1–N10 unchanged, 55 s local case budget, 60 s CI
synthetic step, 25 s scenarios, 280 s internal total, 15 s startup/finalize,
300 s native step, 900 s job. The 16 supervisor names, and the 31/17/6
engineering-tool counts, remain separate from the frozen native 5-suite /
34-method catalog and the Linux 95-suite registration.

## Mandatory pre-fix red (B-1 specific) and fresh green

Before any production edit, the corrected test was run against the unmodified
v2-round production file (SHA-256
`77b184076b1574b654e90208253bfe7b30c2545700ef0d1adae5e22203748693`):

- **RED** — `--mode synthetic`, exit **1**, 2026-11-05T14:51:25Z–14:51:26Z.
  16 cases: **15 PASS and exactly `abi-declaration-sdk-layout` FAIL**. The
  retained case original shows the order assertion with both sides:
  expected `['TotalUserTime', 'TotalKernelTime', 'ThisPeriodTotalUserTime',
  'ThisPeriodTotalKernelTime', 'TotalPageFaultCount', 'TotalProcesses',
  'ActiveProcesses', 'TotalTerminatedProcesses']`, actual
  `['ThisPeriodTotalUserTime', 'ThisPeriodTotalKernelTime', 'TotalUserTime',
  'TotalKernelTime', …]`. This is the local declaration/decode counterexample
  for B-1 — a static-order red, not a Win32 API execution, not the E002
  dynamic reproduction and not the earlier `c_ulongptr` red.

Then the production file alone was corrected (final SHA-256
`169110205983ea0fb6dece48c6a5b2ca1a69c85a11207930c723138e2813293d`) and all
four commands were re-run on the final bytes with the **same test bytes as the
red** (test SHA-256
`c97bcaf5b946355933bd968403ecb106ca5c0d1f6c817f3fec558b5f838c8c27` at every
run, red and green):

| Check | Result |
| --- | --- |
| Supervisor synthetic selfcheck (`--mode synthetic`) | PASS — **16/16** cases, exit 0, 14:52:46Z–14:52:47Z; the ABI case facts record `accounting_size=48`, `pid_list_size=520`, pointer width 8 |
| Runner synthetic selfcheck (`test_windows_ci_evidence.py`) | PASS — **31** tests, exit 0, 14:52:47Z–14:53:15Z, all method names in the retained original output |
| Image provenance source-binding selfcheck | PASS — **17** tests, exit 0, 14:53:16Z–14:53:17Z |
| Bundle capture selfcheck (`--source-root .`) | PASS — **6** tests, exit 0, 14:53:17Z–14:53:19Z |

All runs used `/usr/bin/python3` (Python 3.12.3) with `-B`,
`PYTHONDONTWRITEBYTECODE=1`, a private data-disk TMPDIR outside the checkout
and a fresh private evidence directory; per-command receipts (actual argv,
cwd, interpreter identity, source-file SHA-256 at run time including the
runner's `run_windows_ci.py`, UTC start/end, exit code, raw stdout/stderr
bytes and hashes, per-case results and full method names) are retained in the
private round evidence, which this public report does not carry. A
byte-exact `before-correction` snapshot of the four inherited candidate files
was taken and verified before any write. No intermediate failure occurred
between this round's red and green; prior-round failures (the empty first-red
receipt, the partial note, the `get_last_error` shim failure and the
wrong-oracle green) remain preserved as history and are not re-run or
rewritten.

## The observed native failure of 966d23c (unchanged history)

Push run **42028520881**, job **127248705818** (`windows-2022`), attempt **1**,
head and checkout both the full SHA `966d23c3d38b6ac29981b3b87efad0d5a81ad195`.
Step 4 "Capture inherited image environment before selfchecks" failed
2026-11-05T12:38:19Z–12:38:22Z, exit 1, with the trace
`capture_only_git_head → run_owned → Win32ProcessAPI.__init__ →
_declare_structures → JOBOBJECT_BASIC_LIMIT_INFORMATION →
('Affinity', ctypes.c_ulongptr)` ending in
`AttributeError: module 'ctypes' has no attribute 'c_ulongptr'`. Steps 5–9
were skipped in `jobs.json`, not executed and not recorded as passed; the run
produced zero artifacts. The saved Actions logs and API JSON originals
(SHA-256 `run.json` `994e812d…8052`, `jobs.json` `d4d69134…5f56`,
`artifacts.json` `244563f9…856c`) are preserved; the remote capture-error
spool directory and any native-result artifact were never downloaded and are
recorded as missing — never reconstructed.

## Proof levels (kept separate)

| Item | Level |
| --- | --- |
| Original E002 supervisor leak of descendant processes | STATIC_CONFIRMED / STATIC_CONTROL_FLOW_EVIDENCE; Windows dynamic reproduction NOT_RUN |
| `c_ulongptr` initialization failure | NATIVE_OBSERVED_INITIALIZATION_FAILURE (966d CI trace; also locally red before the v2 fix) |
| Accounting DWORD segment + PID dual-DWORD header corrections (v2 round) | STATIC_CONFIRMED_SDK_LAYOUT_MISMATCH corrected; local synthetic oracle only; not exercised by a real Windows query |
| Accounting four-LARGE_INTEGER order correction (this round, B-1) | STATIC_CONFIRMED_IMPLEMENTATION_REGRESSION corrected; dedicated pre-fix local red + same-oracle green; not exercised by a real Windows query |

No layout on this page is claimed as verified by the Win32 API: there is no
new Windows execution of this code yet. Local green, source static
correctness, independent hard review, native Windows runs, final-SHA CI and
coordinator acceptance are separate states and are tracked separately below.

## Not run / open states

| Item | State |
| --- | --- |
| Windows capture step through the real `Win32ProcessAPI` on the corrected bytes | PENDING_CI — no hosted run of this candidate yet |
| Supervisor native scenarios N1–N10 + startup ABI checks (now including all eight accounting offsets) | PENDING_CI |
| Existing native slice (5 suites / 34 methods) on the new SHA | PENDING_CI — 5/34 is the unchanged frozen expected catalog; the new run must be checked against actual original method logs for names, counts, duplicates and skips, without lowering the expectation or counting tool cases into the 34 |
| install / relocated / detached executions of the corrected code | PENDING_CI |
| Linux six-way matrix and the 95-suite catalog on the new SHA | PENDING_CI — results cannot be borrowed from an older SHA |
| Windows 11 x64 ordinary-user NTFS acceptance | NOT_RUN — no machine |
| Original E002 Windows dynamic reproduction | NOT_RUN |
| Historical image conflict | UNRESOLVED (unchanged) |
| Restricted private original hosting | BLOCKED (unchanged) |
| Full native Windows runtime | NOT_IMPLEMENTED (unchanged) |
| Historical local sanitizer/Clang/live_workflow limitations | unchanged, remain open for their own evidence |

## Disclosures carried from the post-hard-review disposition

Carried per `post-hard-review-disposition-v1` and required in the PR 39
context alongside this report; none of them claims a code fix in this round:

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
  and `12:20` must not be used to date any execution or CI fact.
- **H6-4** — the identity of `platform/native-foundation/README.md` is its
  actual 64-character SHA-256
  `b1334000a93a8ef49eaf3da5ac65cfe97bbe8274df7d607386d5ac16134a3427`;
  the malformed hash string in the earlier implementation response text is
  excluded from identity evidence and the README itself is unchanged.

## States and boundaries

| State | Value |
| --- | --- |
| IMPLEMENTATION_COMPLETE | true — limited to this round's 4-file B-1 order correction delta |
| LOCAL_VALIDATION_COMPLETE | true — limited to the local synthetic/tool checks listed above |
| PUSH_COMPLETE | false at report freeze — no Git writes were performed by the implementation stage |
| REMOTE_CI_COMPLETE | false — Windows hosted and Linux results for this candidate are PENDING_CI |
| PR_REVIEW_COMPLETE | false — the fresh independent hard review of this corrected delta has not run yet; coordinator owns acceptance |
| MERGED | false |

`windows_11_machine_acceptance=NOT_RUN`, `ordinary_user_security_acceptance=
NOT_RUN` and `full_native_runtime=NOT_IMPLEMENTED` are unchanged. The Job
Object remains a supervision custody design, not a sandbox; native exit proof
still requires all three facts together (same-Job `ActiveProcesses == 0`,
root HANDLE signaled, actual exit code read); unknowns are never written as
0, true or PASSED. No real model or paid product call occurred in any round
recorded here; engineering ZCode model requests are counted per stage receipt,
not cumulatively (implementation-02: 43; hard-review-02: 9; this round's own
count is left to its entry receipt and is not pre-written here). The final
SHA, its CI and its adoption will be recorded by a new external versioned
receipt after review and publication, avoiding a self-referential follow-up
commit. This report describes dirty-worktree candidate bytes on HEAD
`966d23c3d38b6ac29981b3b87efad0d5a81ad195`, not a future final Git SHA.

Report generated at 2026-11-05T14:54:52+00:00 (real clock at write time).
