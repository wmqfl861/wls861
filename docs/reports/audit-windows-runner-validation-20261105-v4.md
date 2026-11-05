# Windows runner validation — v4 (explicit inherited-handle attribute list fix)

- `generated_at_utc`: 2026-11-05T17:08:00Z (range of this report: the
  implementation-04 session of 2026-11-05 that produced the four files named
  below; all facts outside that range are cited from the preserved originals
  listed in the v5 FINAL plan record, not re-derived here).
- Working tree while this report is written: branch
  `fix/audit-windows-evidence-20261105-adp35vg4`, HEAD
  `a273b971531a0630aa6deccbb3b9bee997b662fe` (unchanged; no Git write of any
  kind was performed by the implementer), with exactly the two modified source
  files and the two new report files of this batch uncommitted:
  `platform/native-foundation/tools/windows_process.py` (final SHA-256
  `a1fa6e7a4477bc819b53c82edfc0c62133c04f52485a5e5a23eba100187e2c02`),
  `platform/native-foundation/tools/test_windows_process_supervisor.py`
  (final SHA-256
  `55b5a6b6e1440da0bfa2bd2ef8a3075fb5a9860afd76cbc2edeb54038a1679be`),
  and this MD/JSON pair. Baseline a273 source SHA-256 was
  `169110205983ea0fb6dece48c6a5b2ca1a69c85a11207930c723138e2813293d` and the
  a273 test SHA-256 was
  `c97bcaf5b946355933bd968403ecb106ca5c0d1f6c817f3fec558b5f838c8c27`.
- Local evidence root for this batch: `validation-attribute-01/` on the
  private data disk (0700/0600, exclusive-create, private TMPDIR for every
  command, `PYTHONDONTWRITEBYTECODE=1`, no system-tmp staging). Private
  absolute paths are deliberately not repeated in this public report; the
  batch receipts, per-case originals, intermediate source archive and freeze
  manifest (`final/freeze-manifest.json`, 1517 evidence files hashed) are the
  binding originals.

## 1. The two new real native failures this batch answers (observed, archived)

Both runs are windows-2022, workflow `windows-native.yml`, attempt 1, both
step 4 ("Capture inherited image environment before selfchecks", UTC
15:37:14–15:37:20), both with the identical trace
`capture_only_git_head:515 → run_owned:988 → create_process_suspended:425 →
Win32CallFailure: UpdateProcThreadAttribute failed with Win32 error 24`,
step exit 1, and the outer `run_owned` preserving and re-raising the failure:

| run | event / job | API head_sha | actual checkout | tree |
| --- | --- | --- | --- | --- |
| 42056774966 (run_number 98) | push / 127347180379 | a273b971… | a273b971… (checkout log) | cb6bb995… |
| 42056786169 (run_number 99) | pull_request / 127347218354 | a273b971… | eb8474c5… (merge of a273b971 into 9cd5261; checkout log) | cb6bb995… |

The PR run's checkout is the merge object, not the API head; same-tree and
same-`windows_process.py`-identity for both checkouts are facts from the
recorded read-only Git-tree crosscheck (`publication-02/checkout-identity-v2.json`),
not from the API head field. In both runs steps 5–9 (supervisor synthetic,
native, image provenance, product build, and the remaining selfchecks) were
skipped and `artifacts.json` had `total_count=0`; the runner-side
capture-error spool referenced by the trace was not downloaded and is not
reconstructed here. This is a new failure mode at the
`UpdateProcThreadAttribute` call — it is not the previous `c_ulongptr`
carrier defect and must not be relabelled as one.

## 2. The two confirmed defects at the same creation boundary

- **ATTR-1 — wrong attribute key (statically confirmed; consistent with the
  observed error 24).** a273 line 66 declared
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST = 0x00020000`. Per the pinned SDK header
  (microsoft/win32metadata commit
  `c16762d41f1bd641ff8c4389ebf16ee50747129f`, WinBase.h), the HandleList
  attribute key is
  `ProcThreadAttributeValue(ProcThreadAttributeHandleList = 2, FALSE, TRUE, FALSE)`
  = `PROC_THREAD_ATTRIBUTE_INPUT (0x00020000) | 2` = **0x00020002**; the bare
  value 0x00020000 is the ParentProcess key. The production call passed a
  three-HANDLE array (cbSize = `3 * sizeof(void*)` = 24 on x64) — the
  ParentProcess contract instead takes one process HANDLE. The wrong
  key/payload-length combination matches ERROR_BAD_LENGTH (24). A controlled
  same-conditions real-API old-key vs correct-key contrast is implemented
  (section 4) and PENDING native execution; the synthetic contract red below
  is a local fixture check, not a Windows error-mapping reproduction.
- **ATTR-2 — the initialized attribute list was never submitted
  (statically confirmed; no native leak or unrestricted-creation observation
  is claimed).** `create_process_suspended` initialized and updated
  `attribute_buffer` but never assigned it to `STARTUPINFOEXW.lpAttributeList`,
  so `CreateProcessW` received a NULL list pointer while
  `EXTENDED_STARTUPINFO_PRESENT` and `bInheritHandles=TRUE` were already set.
  The observed error 24 happens before `CreateProcessW`, so this defect was
  never exercised natively; it independently violates the explicit
  inheritable-handle whitelist design.

Everything else at the boundary was re-verified unchanged by the fresh
plan-review and preserved: `cb = sizeof(STARTUPINFOEXW)` (112),
`STARTF_USESTDHANDLES`, the three owned inheritable stdio handles, the
two-step `InitializeProcThreadAttributeList` NULL-probe protocol with
last-error 122, `DWORD_PTR`/`SIZE_T` argument carriers, buffer/array lifetime
through the call, exactly one `DeleteProcThreadAttributeList` in the finally,
suspended-create → Job-assign → resume order, non-inheritable
KILL_ON_JOB_CLOSE Job, and every host/security guard. The production change
of this batch is exactly the two points plus adjacent comments (26-line diff
total, see `final/windows_process.py.green-vs-a273.diff`):

1. the constant corrected to `0x00020002` with the SDK derivation comment;
2. `startup.lpAttributeList = ctypes.cast(attribute_buffer,
   ctypes.c_void_p).value` after the successful initialization, submitting the
   same initialized/updated list inside the extended STARTUPINFO.

## 3. Local double-red / green at the real production method boundary

New synthetic case `abi-create-process-handle-list-contract` (registry count
16 → 17; the original 16 names and semantics untouched). It constructs the
real `Win32ProcessAPI` under the existing constructor patch (construction
still calls no Win32 function), then replaces — on that instance only — the
low-level entries the method itself calls (Initialize/Update/Delete attribute
list, `CreateProcessW`, `GetProcessTimes`) plus a fixture-local programmable
last-error source (122 after the NULL probe, 24 only for the explicit
FAULT_INJECTION update fault, 0 otherwise). The key oracle is the independent
SDK derivation `0x00020000 | 2`, never the production constant. Three
distinct synthetic non-zero, non-pseudo stdio values; controlled
PROCESS_INFORMATION/FILETIME writes; everything restored in finally. No
Windows kernel call is made: these reds are local contract-fixture
observations at the real method boundary, not native validation.

| stage | production SHA-256 at run | expected and actual result |
| --- | --- | --- |
| RED-1 (a273 bytes) | `16911020…3293d` | exit 1; 16 PASS; only the new case failed: "UpdateProcThreadAttribute attribute key must be 0x20002 (SDK PROC_THREAD_ATTRIBUTE_INPUT \| HandleList number), got 0x20000" |
| RED-2 (constant-only intermediate) | `260cd00bd1ff600b587caf3e6dd15fa076aaf435dbc0b7304d3e1ab0f2f57059` | exit 1; 16 PASS; only the new case failed at the next boundary: "CreateProcessW STARTUPINFOEX.lpAttributeList must submit the initialized/updated attribute list 0x…, got NULL" — the constant alone is proven insufficient, neither red masked the other |
| GREEN (both changes) | `a1fa6e7a…e2c02` | exit 0; **17/17 PASS** incl. the FAULT_INJECTION sub-branch (injected update fault propagates as the original `Win32CallFailure`/24, no CreateProcessW, exactly one Delete, marked FAULT_INJECTION, never presented as a real Windows error reproduction) |

Test SHA-256 identical across all three runs:
`55b5a6b6e1440da0bfa2bd2ef8a3075fb5a9860afd76cbc2edeb54038a1679be`.
Per-command receipts record expanded argv, cwd, interpreter identity
(/usr/bin/python3 3.12.3, binary SHA-256
`e50d468e8b0adfb05733f5b87b3cff34829c4a8c1aea50c865aa8bdfe4bb150f`),
TMPDIR/env, tracked source hashes, UTC start/end, exit code, raw
stdout/stderr bytes+hashes, and HEAD/porcelain immediately before and after
each run. HEAD stayed `a273b971…` in every run; porcelain showed the two owned
files modified from RED-2 on, which is exactly why per-command binding is
recorded instead of trusting HEAD alone.

**Constant-only intermediate disclosure:** the `260cd00b…` state was executed
only in the local synthetic fixture, had **no native Win32 execution of any
kind**, and was **never committed, pushed or published**; its full bytes,
SHA-256 and unified diff are archived (`red-2/intermediate/`), and the state
was byte-identical when re-applied for the rebuilt sequence.

**Attempt-1 disclosure (test-change rule applied):** the first green attempt
(test SHA `620bec7b…`) failed on a fixture authoring typo in the
FAULT_INJECTION sub-branch call (`stdout_handle`/`stdout_value`), after the
main sub-branch had already passed and recorded its facts. Per the FINAL rule
for test changes, that entire sequence (its RED-1/RED-2, which had failed at
the main sub-branch before the typo could execute, and the failed green) is
preserved under `attempt-1/` with its own record, and the red/green binding
above was rebuilt with the corrected test bytes. The typo was a fixture
defect, not a production observation.

## 4. Green local regressions and the native additions (PENDING_CI)

Green re-run of all four commands on this host (Linux, ordinary user; counts
are engineering-tool results, not the Linux product CTest catalog and not the
native product suites):

| command | result |
| --- | --- |
| `test_windows_process_supervisor.py --mode synthetic` | exit 0, 17/17 PASS |
| `test_windows_ci_evidence.py --retain-fixtures …` | exit 0, Ran 31 tests, OK |
| `test_windows_image_provenance.py` | exit 0, Ran 17 tests, OK |
| `test_bundle_capture.py --source-root .` | exit 0, Ran 6 tests, OK |

Before/after hash protection: the before-snapshot re-hashed 20 files (the
protected set from the planner record and the prior same-author disposition,
loaded from those records — never transcribed — including the three read-only
regression scripts `test_windows_ci_evidence.py` (`6ecc3b1a…`),
`test_windows_image_provenance.py` (`153812da…`) and
`tests/test_bundle_capture.py` (`3d2f969f…`)); the final freeze re-hashed the
same set with **zero violations** — every file except the two owned targets is
byte-identical to a273.

Native additions implemented in the test file for the next real Windows run
(**NOT_RUN on this Linux authoring host; status PENDING_CI**; no Linux, Wine
or synthetic substitute was run or claimed):

1. In `native_startup_checks`, before the existing real startup-probe: a
   same-conditions real-API contrast — two freshly initialized attribute
   lists, the same three fixture-owned inheritable handles (one NUL input,
   two private spool files), the same 24-byte payload, flags 0, both reserved
   parameters NULL, count 1; old key `0x00020000` expected FALSE with
   immediate error 24 (any other observation fails and is retained as-is),
   SDK key `0x00020002` (cross-checked equal to the corrected production
   constant) must succeed; each list deleted once and every handle closed in
   finally; `CreateProcessW` is never called by the contrast.
2. The existing startup-probe (same command, timeout 10, Job proof) now runs
   behind a narrow per-instance recorder of
   Initialize/Update/Delete/`CreateProcessW`: actual arguments and order
   recorded, every call forwarded unchanged to the real Win32 function, no
   result/last-error/permission substitution, instance functions restored in
   finally; before the real `CreateProcessW` the submitted
   `STARTUPINFOEX.lpAttributeList` is verified non-NULL and identical to the
   list the successful `Update` targeted, with the three stdio members,
   24-byte payload and required flags recorded.

Budget facts: `STARTUP_BUDGET_SECONDS`/`FINALIZE_BUDGET_SECONDS` remain 15.0
constants with elapsed recording only — the current main has **no** enforced
15-second comparison and none was added; the existing scenario/total checks
(N1–N10 25 s, internal 280 s), synthetic local case budget 55 s / CI step
60 s, native step 300 s / job 900 s, build 600 s / ctest 300 s and the native
suite deadlines (30/30/30/60/30 s) are all unchanged. The native five-suite /
34-method catalogue remains a frozen expectation that must be proven from the
final merged SHA originals; this batch adds no N scenario, no CLI mode, no
product-suite registration and no C++ work.

## 5. Inherited disclosures (unchanged, still open)

- **H6-1**: the runner result aggregate remains a pre-finalization projection
  omitting `source-snapshot-after`; final per-command coverage must be read
  from identity-finalization plus the original per-command receipts. Not
  fixed in this batch.
- **H6-2**: the v1/E003 "12:20" timestamp remains a non-evidence nominal
  value; the actual write time stays UNKNOWN with original bytes preserved.
- **H6-4**: the verified `platform/native-foundation/README.md` SHA-256
  remains `b1334000a93a8ef49eaf3da5ac65cfe97bbe8274df7d607386d5ac16134a3427`;
  the malformed hash string in an old implementation response stays excluded
  from identity evidence.
- **D03-1/D03-2**: the acknowledged system-tmp staging and the replaced
  environment-metadata first version from implementation-03 remain
  NONCOMPLIANT/ORIGINAL_UNAVAILABLE facts with UNKNOWN history; this batch
  used a private data-disk TMPDIR exclusively (D03-1/2 are not closed by
  that). **D03-3**: the mistyped first manifest remains retained and
  explained; this batch loads expected hashes from the saved records
  programmatically and archives every failed/interrupted attempt
  (attempt-1) rather than overwriting. **D03-4**: no mode rewriting of
  history occurred.
- Historical Clang/live_workflow failures remain open; Windows 11
  ordinary-user NTFS acceptance stays **NOT_RUN**, the Windows image conflict
  stays **UNRESOLVED**, private original hosting stays **BLOCKED**, and
  `full_native_runtime` remains **NOT_IMPLEMENTED** (Job Objects are still a
  custody design, not a sandbox).

## 6. Separated release states for this batch

| state | value |
| --- | --- |
| IMPLEMENTATION_COMPLETE (this four-file slice) | yes — the two source points, the test additions and this report pair, per FINAL v5 |
| LOCAL_VALIDATION_COMPLETE | yes — local synthetic only: RED-1/RED-2/GREEN 17 and the 31/17/6 regressions above, same-test-SHA bound |
| PUSH_COMPLETE | no — implementer performed no Git writes; the branch still points at a273 |
| REMOTE_CI_COMPLETE | PENDING — next real windows-native CI run on a future pushed SHA must pass capture first, then the new contrast/probe/N1–N10/suites |
| PR_REVIEW_COMPLETE | PENDING — independent hard review of this batch not yet performed |
| MERGED | no |

No real WLS or paid provider calls occurred; the engineering ZCode model
request count for this session is recorded by the local entry receipts
(unknown to this report). Native old-key→24 causality, the corrected-key
success, the production-path list submission and every native scenario
remain unproven until the PENDING native run produces originals — deleting
error 24 locally, the constant-only fix, local green or this report do not
close the native runnability question. Any native step failing again must
retain its originals and return to the same planning author for minimal
diagnosis.
