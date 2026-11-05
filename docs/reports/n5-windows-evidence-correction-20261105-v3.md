# N5 Windows evidence correction (v3): C8 WRITE_DAC/WRITE_OWNER subcase presence

Versioned correction report, 2026-11-05, version 3. It corrects exactly one
record-integrity error in the C8 row of the immutable v2 continuation report
and states the provable historical execution granularity for the WRITE_DAC /
WRITE_OWNER subcases. The v2 files are immutable inputs and are referenced,
never rewritten:

- `docs/reports/n5-windows-evidence-continuation-20261104-v2.md`, SHA-256
  `f72a97026658175526db2574b68ea4d6094182b8dc0a430eacfa9158e57c604f`
- `docs/reports/n5-windows-evidence-continuation-20261104-v2.json`, SHA-256
  `032535badb62f1a148c3a74cee68adee02423be93dec939e678f2558b78d0eb9`

This round re-verified the saved originals read-only (archive re-hashed, ZIP
members read in memory; nothing unpacked into an executable location, no
archived code executed, no Windows rerun, no remote request). The original
`readonly_tests.cpp` is unchanged in this checkout
(`267e02e27ecbd45b814796013b273ffcff18bf32441d59c7d1236338b081d54a`, 13984
bytes) and is byte-identical to the source-input entry of the run archive
cited below.

## What was wrong in v2

The v2 C8 row-bound gap lists the "remaining dangerous-mask spellings" as
"(FILE_APPEND_DATA, FILE_WRITE_EA, FILE_WRITE_ATTRIBUTES, FILE_DELETE_CHILD,
DELETE, WRITE_DAC, WRITE_OWNER individually) not individually fed". That is
incorrect for the last two entries. WRITE_DAC and WRITE_OWNER **are**
individually fed, as SDDL **rights-slot** spellings `WD` and `WO`, in four
unconditional subcases inside the historically passing method
`native complete owner and DACL allow deny policy`:

| Subcase (SDDL) | Right | Trustee | Source line |
| --- | --- | --- | --- |
| `(A;;WD;;;BU)` | WRITE_DAC | BUILTIN\Users | `readonly_tests.cpp:125` |
| `(A;;WO;;;WD)` | WRITE_OWNER | Everyone (WD in the **trustee** slot) | `readonly_tests.cpp:126` |
| `(A;;WD;;;BA)` | WRITE_DAC | BUILTIN\Administrators | `readonly_tests.cpp:147` |
| `(A;;WO;;;BA)` | WRITE_OWNER | BUILTIN\Administrators | `readonly_tests.cpp:148` |

Both groups are plain initializer-list range-for bodies feeding
`rejects_code(ErrorCode::Denied, ...)` inside `complete_acl_policy`; there is
no skip path around them. Slot discipline is kept exact: `WD`/`WO` in the
rights slot mean WRITE_DAC/WRITE_OWNER; `WD` in the trustee slot means
Everyone; `FW` is the file-generic write mask and is not evidence of a
WRITE_DAC input; `DC` is the directory-CONTAINER-inherit-style right named in
SDDL and is **not** equal to FILE_DELETE_CHILD, so the DC gap item is not
affected by this correction.

## Evidence chain actually re-verified (row 7, saved same-job original)

Bound to the v2 row 7 record: run `41598903763`, job `125792707157`, attempt
1, workflow job key `native-foundation`, checkout
`3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` (exact frozen head). The saved
archive re-hashed to its recorded value
`0135c9c5cdd5536940eb4b693ce7a6911cb7152fb50d0756ba907e389d97c3e0`; its
`native-work/evidence/ctest-logs/LastTest.log` hashes to `a4ca05218dbe39f41d705bf1ecd43981aa37390fffb975f16cb48486655e7041`
and `native-work/evidence/ctest-methods.json` to
`80985563acb67e7fe69a98c5360c4439c00902268a208eab921ea0c3d49d76b8`, both
equal to the v2 row 7 values. Inside that LastTest block the method
`native complete owner and DACL allow deny policy` appears exactly once as a
`PASS` line, with no other mention, no duplicate, no failure marker and no
truncation (`RESULT 7 passed, 0 failed`, `Test Passed.` trailer); the seven
`native_readonly` PASS lines equal the in-archive registry list verbatim. The
in-archive source-input snapshots (before == after) list
`platform/native-foundation/tests/readonly_tests.cpp` with the SHA-256 above,
which equals the current checkout file.

## Corrected statement and its judgment level

The corrected C8 executed claim is: *admin owner/writer and dangerous allow
ACEs refused — including individually fed SDDL rights-slot WRITE_DAC (`WD`)
and WRITE_OWNER (`WO`) allow ACEs to non-trusted trustees (BU, WD=Everyone,
BA) — deny never erases an unsafe allow; null and empty DACL refused.* The
judgment level for each of the four subcases is
`SOURCE_SUBCASE_PRESENT + HISTORICAL_METHOD_PASS_VERIFIED_FROM_SAVED_ORIGINAL`:
the original LastTest contains **per-method** PASS lines only — there is no
per-mask PASS granularity in the original log and none is claimed — so the
execution of the four subcases is inferred from the once-passing method at
the same frozen SHA whose unconditional source contains them. This is not a
rerun of Windows (`windows_rerun=NOT_RUN`) and not new acceptance; the 17-row
3/11/2/1 historical classification and time ranges of v2 are retained, C8
remains `WINDOWS_SERVER_COMPONENT_EXECUTED`, and no old result is inherited
as a new test pass. Subcases inside existing methods are not new methods; the
catalog remains 5 native suites / 34 current-group methods (27 historical)
with unchanged deadlines.

As a cross-check of availability only, the adjacent PR-merge archive of v2
row 8 (run `41599060437`, job `125793198983`, attempt 1) was re-hashed to its
recorded value `93f671f9feb5b8f11cba831db4912b74e186df680be9928313b0c6ae38e7fe25`;
its member-level extraction was not redone this round and its merge-checkout
status stays exactly as recorded in v2.

## Gaps that remain open (unchanged by this correction)

C8 still keeps: malformed SDDL input strings have no executed subcase; the
DACL-absent (present=FALSE) descriptor is not directly fed (null and empty
DACL remain the executed variants); FILE_APPEND_DATA, FILE_WRITE_EA,
FILE_WRITE_ATTRIBUTES, FILE_DELETE_CHILD and DELETE remain without
individually fed subcases. `DC` remains explicitly **not** FILE_DELETE_CHILD.
The BA protections stay distinct: BA as a *writer/trustee* ACE is refused by
the C8 subcases above, while the shared preflight rejecting Administrators as
a trusted owner/writer entry (C2) and the `ordinary_token` elevated-caller
rejection (C5 entry executed; C6 full profile source-only) are separate
claims that were never evidence for one another. Image identity stays
UNRESOLVED, Windows 11 stays NOT_RUN and restricted private hosting stays
BLOCKED.

## Defect assessment and states

No product or test code changed for E003 (`code_change_required=false`; the
deterministic counterexample is `NOT_APPLICABLE` — this is a record-integrity
correction of a published evidence index, and the source/index contradiction
itself was already statically confirmed). This v3 file pair is the complete
correction; the v2 originals are never edited. No private paths, raw log
bytes or credentials appear here; the private verification receipts mapping
these conclusions to the original files are retained outside the repository.

| State | Value |
| --- | --- |
| IMPLEMENTATION_COMPLETE | true — this documentation-only correction |
| LOCAL_VALIDATION_COMPLETE | true — offline read-only re-verification of saved originals |
| PUSH_COMPLETE | false at report freeze |
| REMOTE_CI_COMPLETE | false — not applicable to this offline correction; not inherited |
| PR_REVIEW_COMPLETE | false — coordinator owns acceptance |
| MERGED | false |

Report frozen at 2026-11-05T12:20:00+00:00 (tool clock at write time).
