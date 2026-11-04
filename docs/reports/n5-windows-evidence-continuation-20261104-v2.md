# N5 Windows evidence continuation (v2): run provenance, method completeness, readonly coverage

Versioned evidence continuation report, 2026-11-04, version 2. It documents the
frozen candidate `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` (Draft PR #32, base
PR #31 `f15bf1a944a1ea5f6b9e5e9696b921caa48f04cc`) and is a read-only
verification and reporting increment: no production source, published SQL,
migration, workflow, shared build file, historical report or original log was
changed. A same-day earlier draft of this report was never published and
contained the method-count breakdown and coverage-classification errors
corrected here; this version supersedes it. The companion safe index is
`n5-windows-evidence-continuation-20261104-v2.json`.

## What was verified and how (this round)

The evidence chain was re-verified in place from saved originals: all eight run
archives were re-hashed and their per-method `LastTest` logs and in-archive
method registries re-extracted (zip members read in memory; nothing unpacked
into an executable location, no archived code executed); 22
saved run/job record files were read to re-bind run, job, attempt, artifact and
Runner-Image header values for every row; 3 Linux native
validation log files were re-read for the portable rows. The prior session's
311-file inventory is retained as a mapping; those 311 originals were **not**
re-hashed this round and this report does not claim they were (the prior
session's receipts remain the evidence for that re-hash). No remote API
request, CI rerun, archived-code execution or product build occurred
(0/0/0/0).

## W1 — Run/job provenance matrix (all rows from saved same-job originals)

| Row | Group | Event | Run | Job | Attempt | Checkout | Methods | Image identity |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | historical | push | 41416271880 | 125180768812 | 1 | `aa2c592` (head) | 27 | UNRESOLVED |
| 2 | historical | pull_request | 41416279414 | 125180794615 | 1 | `ecc2d4b` (merge) | 27 | UNRESOLVED |
| 3 | module | push | 41591301054 | 125768414375 | 1 | `8ec950f` (head, PR #28) | 34 | UNRESOLVED |
| 4 | module | pull_request | 41591416087 | 125768769642 | 1 | `349464b` (merge; tree == head tree) | 34 | UNRESOLVED |
| 5 | integrated | push | 41593459570 | 125775278062 | 1 | `f15bf1a` (head, PR #31) | 34 | UNRESOLVED |
| 6 | integrated | pull_request | 41593557500 | 125775579900 | 1 | `a400da9` (merge; tree == head tree) | 34 | UNRESOLVED |
| 7 | PR #32 | push | 41598903763 | 125792707157 | 1 | `3a872558` (exact head) | 34 | UNRESOLVED |
| 8 | PR #32 | pull_request | 41599060437 | 125793198983 | 1 | `8390b97` (merge f15bf1a+3a872558; tree == head tree) | 34 | UNRESOLVED |

Every row records repository `wmqfl861/wls861`, workflow job key
`native-foundation`, its artifact ID/digest and the private original hashes.
This round the `##[group]Runner Image` header block of each row's saved job log
was re-read: image `windows-2022`, header version `20261025.346.1` on all
eight, with emitted timestamps equal to the recorded matrix. Run/job/attempt
and artifact identities for rows 3–8 were re-read from the saved
`run.json`/`inputs.json`; checkout values for the merge rows and rows 1–2 are
carried unchanged from the prior verified receipts (their comparison inputs
re-verified byte-identical in that session; the archives re-hashed this round).
All eight saved comparisons bind `MATCHED_SAVED_SAME_JOB_EVIDENCE` with
comparator exit 1 (conflict with binding evidence), and no prior audit bytes
were rewritten.

Image-source channels per row: header `20261025.346.1` (job-log Runner Image
block); inherited process `ImageOS=win22, ImageVersion=20261026.347.1`
(rows 1–8); HKLM 64-bit Session Manager Environment `20261026.347.1`
(rows 3–8; `NOT_COLLECTED` on rows 1–2 because no observation existed then).
Process/HKLM agreement on rows 3–8 is `CONSISTENT_OBSERVATIONS_NOT_HOST_ATTESTATION`
only. Generation paths were re-verified from pinned upstream sources in the
prior session (runner 2.338.2 prints the header from its root `.setup_info`;
`Configure-ImageDataFile.ps1` writes the imagedata; `Configure-SystemEnvironment.ps1`
sets machine registry and process environment; the header-version tag query
returned HTTP 404, retained). These facts establish how the fields are
generated, not which value the historical VMs were built with: the conflict
stays **UNRESOLVED**, `preferred_source=null`, cause `NOT_ESTABLISHED`. No
offline replay was re-run; rows 7–8 bind the current head `3a872558`
explicitly.

## W2 — Complete original logs and method completeness

Per-method completeness was recomputed this round from raw LastTest bytes for
all eight archives and equals each archive's in-registry method list, with zero
missing, duplicate or skipped methods and all methods passing. The two input
groups are reported separately:

- current group (rows 3–8, inputs `8ec950f`/`f15bf1a`/`3a872558`): 5 suites,
  34 methods = native_modules 3 + native_platform 4 + **native_readonly 7** +
  native_cli 14 + native_bundle_capture 6;
- historical group (rows 1–2, inputs `aa2c592`/`ecc2d4b`): 4 suites,
  27 methods = native_modules 3 + native_platform 4 + native_cli 14 +
  native_bundle_capture 6; the `native_readonly` suite did not exist at that
  input and no current-group method is evidenced by those rows.

The seven `native_readonly` method names, verbatim from the current test
registration and identical in all six current-group registries and their
LastTest logs: *missing explicit policy refuses before root open*; *duplicate
or unsupported policy entries reject*; *historical compiled public capture
stays compatible*; *native complete owner and DACL allow deny policy*; *native
unknown ACE and inheritance boundaries*; *native same handle identity ACL and
replacement barrier*; *native thread impersonation refuses before root open*.

The suite catalog equals the current candidate's five CTest registrations with
deadlines 30/30/30/60/30 s. Five green suites or a suite-level JUnit were never
used as a substitute for per-method output (the archives themselves record
that JUnit method output may be truncated, which is why raw LastTest is the
per-method evidence). All eight runs ended in the normal completed-PASS state;
no failure/timeout/interrupted sample exists among these Windows runs, so
retention for those states is not claimed. The 30-method Linux native runs
(two umask variants, readonly = the 3 portable methods) are a different input
and were not cross-filled into the Windows counts.

## W3 — readonly SID/ACL boundary coverage (17 rows, corrected classification)

Every row below is derived as requirement → code branch → executed test
subcase → same-input run evidence; the executed claim is bounded to what the
named methods actually exercise, and claims wider than that are listed as
row-bound gaps that are **not** rows. Summary counts are computed from this
same table: PORTABLE_EXECUTED 3
(C1, C2, C3);
WINDOWS_SERVER_COMPONENT_EXECUTED 11
(C4, C5, C7, C8, C9, C10, C11, C12, C13, C14, C15);
SOURCE_ONLY 2
(C6, C16); WINDOWS11_NOT_RUN
1 (C17).

| ID | Classification | Executed claim (bounded) |
| --- | --- | --- |
| C1 | PORTABLE_EXECUTED | missing/duplicate/old-build policy entries refused before path access |
| C2 | PORTABLE_EXECUTED | Builtin Administrators SID refused as only/additional trusted owner-writer before path access |
| C3 | PORTABLE_EXECUTED | Store 411 public bundle identity verified; non-Windows refusal without fallback executed on Linux |
| C4 | WINDOWS_SERVER_COMPONENT_EXECUTED | canonical SID conversion executed; S-1-1-0 broad-trust entry refused on the inspect path |
| C5 | WINDOWS_SERVER_COMPONENT_EXECUTED | thread impersonation refused before root open (caller-token protection) |
| C6 | SOURCE_ONLY | none executed; code present, requires an ordinary non-elevated token |
| C7 | WINDOWS_SERVER_COMPONENT_EXECUTED | complete owner+DACL acceptance with descriptor hash |
| C8 | WINDOWS_SERVER_COMPONENT_EXECUTED | admin owner/writer and dangerous allow ACEs refused; deny never erases an unsafe allow; null and empty DACL refused |
| C9 | WINDOWS_SERVER_COMPONENT_EXECUTED | read-only and deny-only controls stay interpretable, owner retained |
| C10 | WINDOWS_SERVER_COMPONENT_EXECUTED | object ACE refused (type pre-verified); owner-rights refused; inherited/inherit-only accepted |
| C11 | WINDOWS_SERVER_COMPONENT_EXECUTED | FILE_ID_128/volume identity equality; invalid handle Unsupported |
| C12 | WINDOWS_SERVER_COMPONENT_EXECUTED | same-handle owner/DACL observation; non-inheritable handle required |
| C13 | WINDOWS_SERVER_COMPONENT_EXECUTED | file-level concurrent-rename replacement barrier executed |
| C14 | WINDOWS_SERVER_COMPONENT_EXECUTED | junction, hard-link, directory-for-asset, same-size tamper, oversized, self-reported manifest, no cwd/env fallback, relocation/unicode refusals executed |
| C15 | WINDOWS_SERVER_COMPONENT_EXECUTED | bounded EOF/size capture with read-back identity recheck executed on every bundle read |
| C16 | SOURCE_ONLY | none executed; code present, require_host restricts the chain to Windows 11 workstation builds |
| C17 | WINDOWS11_NOT_RUN | not run this round; no Windows 11 ordinary-user machine |

Row-bound gaps (not counted as rows): C1 policy bounds and non-canonical SID
spellings; C4 the remaining broad/dynamic SID spellings and non-canonical SID
input; C8 malformed SDDL strings, DACL-absent descriptors and the remaining
dangerous-mask spellings; C10 conditional ACE strings, IO/NP without OI/CI and
the generic-mask mapping bound; C13 ancestor-directory replacement race; C6
the whole ordinary-token full profile; C16 the whole policy-bound root chain
including a mid-capture token-LUID-change negative and the retained deployment
limitation that default NTFS Administrators ancestor grants can refuse a
complete root capture; C17 the Windows 11 machine acceptance itself.

The two administrator protections are kept distinct: the shared preflight
rejecting Administrators as a *resource owner/writer trust entry* (C2,
portable, executed) is not the `ordinary_token` check rejecting an *elevated
caller* (C5 entry executed; C6 full profile source-only pending an ordinary
token), and neither was used as evidence for the other or for any
PR-integration history. Subcases inside existing methods are not new
methods/suites: the catalog remains 5 suites / 34 current-group methods
(27 historical) with unchanged deadlines; no host ACL, `trusted_sids` entry or
assertion was weakened.

## W4 — Defect assessment and record corrections

This version exists because the unpublished prior draft contained
record-integrity errors that are corrected here: (1) its coverage summary
counts (3/10/3/1) contradicted its own 17-row classifications (3/11/2/1) and
counted a gap item as a third SOURCE_ONLY row; (2) its method-completeness
breakdown placed `native_readonly` "absent" inside the current-group 34-method
breakdown instead of separating the two input groups; (3) a private receipt
listed only 4 of the 7 readonly method names; (4) a private receipt misnamed
`test_native_cli.py` as a `.cpp` file; (5) its files carried recorded
timestamps later than the writing process had actually finished (retained
unchanged in the originals, never replaced with fabricated past values). No
production or tool code defect was found and no code was changed:
`code_change_required=false`; the deterministic counterexample for a code fix
is `NOT_APPLICABLE`. Tool selfchecks were not re-run (no tool code changed).

## States and boundaries

| State | Value |
| --- | --- |
| IMPLEMENTATION_COMPLETE | true — limited evidence-continuation scope of this report and index only |
| LOCAL_VALIDATION_COMPLETE | false — Windows 11 ordinary-user acceptance NOT_RUN; no new local product matrix run |
| PUSH_COMPLETE | false at report freeze; the owner publishes this branch after independent review |
| REMOTE_CI_COMPLETE | true for frozen head `3a872558` per saved PR #32 rollup receipts (Windows native push/PR and the recorded check set, all SUCCESS, captured 2026-11-03); not inherited by this report's new files |
| PR_REVIEW_COMPLETE | false — coordinator owns acceptance |
| MERGED | false |

Historical image identity stays UNRESOLVED; Windows 11 stays NOT_RUN;
restricted private hosting stays BLOCKED. No N5-2, 014/Store 412, Windows
Workspace write, file/process/Store/VFS/sandbox/cleanup capability, real model
or paid call, production operation, Git history rewrite, main write or merge
is added or implied by this report.

Private verification receipts for this round are retained outside the
repository; this report references no private paths or receipt names. Raw
LastTest/JUnit bytes and machine environments stay unpublished; the per-method
facts above are checked projections, not the originals.

Report generated at 2026-11-04T16:32:36.608339+00:00 (tool clock at write time).
