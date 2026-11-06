> **HISTORICAL PROJECTION (REDACTED) — NOT CURRENT ACCEPTANCE.**
> Source: `docs/reports/n5-windows-evidence-continuation-20261104-v1.md` as it existed uncommitted in its
> worktree at head `9cd526130609a8eb0187f1ee65cf0b28aa4e182c` (branch `fix/n5-windows-evidence-20261104-_7wxnkyn`); original 12893 bytes,
> SHA-256 `e89f8f32da1672b84886af6ec17df26314185fbf1438954900c73f0aa6394bf7`. Redacted public projection created 2026-11-06T10:21:09Z under
> `docs/reports/publication-20261106/`. This is a new version, not a byte copy
> of the original; the original is retained locally and is not in Git.
> Redaction scope: absolute local workspace/evidence-root paths, local task plan / version-record / plan-review references, internal session identifiers.
> No engineering result, status, failure, timestamp or six-state value was
> changed; PUSH/CI states remain the historical values of the report's date.
> **Adjacent qualification:** the later correction report
`n5-windows-evidence-correction-20261105-v3.md` (published on the PR #39 branch
`fix/audit-windows-evidence-20261105-adp35vg4`) qualifies this continuation's
defect assessment and method-breakdown framing, and the latest two native runs
of that later branch fail at configure. This projection preserves the
2026-11-04 statements; it is not current acceptance.

# N5 Windows evidence continuation: run provenance, method completeness, readonly coverage

Versioned evidence continuation report, 2026-11-04. This is a read-only
verification and reporting increment on frozen candidate
`3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123` (Draft PR #32, base PR #31
`f15bf1a944a1ea5f6b9e5e9696b921caa48f04cc`). It changes no production source,
published SQL, migration, workflow, shared build file, historical report or
original log. The companion safe index is
`n5-windows-evidence-continuation-20261104-v1.json`.

Scope was fixed by an approved frozen local task plan (plan and version-record
references redacted). All conclusions
below stay inside that plan's evidence boundaries.

## What was verified and how

Every referenced private original was re-hashed on the data disk this session
(311 unique files; 0 mismatches against their recorded identities), including:

- `windows-final-handoff-01.json` evidence list: 22/22 files byte-identical.
- `windows-official-source-01/INDEX.json`: 57/57 pinned upstream files identical.
- `windows-original-conflict-recheck-01`: both run receipts' 13 input files each
  identical; both saved comparisons and summary identical.
- `stop-gate-windows-readback-handoff-01.json` evidence list: 15/15 identical.
- The public index `docs/reports/n5-review-windows-evidence-index-20261103.json`:
  28/28 private entries identical; its 9 `source_delta` files equal the current
  candidate's bytes.
- The four in-scope tool/selfcheck files and three readonly source files equal
  the previously reviewed stop-gate bytes (7/7 plus `AGENTS.md` and two docs,
  10/10), so all saved receipts that used comparator
  `windows_image_provenance.py` (SHA-256
  `73be4f01acde7395d79d1ef6f5604d72d6f2f8b3d79afc3b5874d7a324a792e2`)
  remain bound to the current candidate's identical comparator.

No remote API request was needed or made this session (0 network requests, 0
CI reruns, 0 archived code executed, 0 product builds). The previously missing
PR #32 receipts were located locally (root-provided), so nothing had to be
marked `NOT_AVAILABLE` and no supplementary collection was required.

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
`native-foundation`, its artifact ID/digest/size, capture windows, and the
private original hashes; the full per-row detail (including merge parents and
capture timestamps) is in the private receipts listed at the end. All eight
saved comparisons bind `MATCHED_SAVED_SAME_JOB_EVIDENCE` with comparator
exit 1 (conflict with binding evidence), and no prior audit bytes were
rewritten.

Image-source channels per row: header `20261025.346.1` (job-log `##[group]`
Runner Image block, line 16 of each job log); inherited process
`ImageOS=win22, ImageVersion=20261026.347.1` (rows 1–8); HKLM 64-bit
Session Manager Environment `20261026.347.1` (rows 3–8; `NOT_COLLECTED` on
rows 1–2 because no observation existed then). Process/HKLM agreement on
rows 3–8 is `CONSISTENT_OBSERVATIONS_NOT_HOST_ATTESTATION` only.

Generation paths were re-verified from the pinned upstream sources (never
executed): the runner (2.338.2, tag commit `d1eb12d4…`) prints the header from
its root `.setup_info` file (`HostContext` → `JobExtension`); the imagedata
file behind `.setup_info` is written by
`images/windows/scripts/build/Configure-ImageDataFile.ps1` from build-time
`IMAGE_VERSION`; machine registry and inherited process values are set by
`Configure-SystemEnvironment.ps1` from the same-named build-time variables.
The observed environment tag is `win22/20261026.347` (`36de032a…`); the
header-version tag query returned HTTP 404 and that failure is retained.
These facts establish how the three fields are generated, not which value the
historical VMs were built with: their `.setup_info`, provisioning records and
machine registry were never captured. The conflict therefore stays
**UNRESOLVED**, `preferred_source=null`, cause `NOT_ESTABLISHED`. No offline
replay was re-run: all eight saved receipts used comparator bytes identical
to the current candidate and their inputs re-verified byte-identical, which
the plan accepts in place of a re-run; rows 7–8 already bind the current head
`3a872558` explicitly.

## W2 — Complete original logs and public projections

For all eight saved archives (read in place, never unpacked into execution):
LastTest and JUnit bytes were independently re-hashed and, for rows 3–8,
equal the saved verified-results records exactly. Per-method completeness was
recomputed from raw LastTest bytes: C++ suite `PASS <name>` lines plus
`native_cli`/`native_bundle_capture` unittest `… ok` lines sum to the
in-archive method registry with zero missing, duplicate or skipped methods —

- rows 3–8: 34 = native_modules 3 + native_platform 4 + native_readonly 7 +
  native_cli 14 + native_bundle_capture 6;
- rows 1–2 (historical input): 27 = 3 + 4 + 14 + 6; the `native_readonly`
  suite did not exist at that input.

The suite catalog equals the current candidate's five CTest registrations
with unchanged deadlines (30/30/30/60/30 s), and the method names in the
row 7/8 registries equal the current candidate's test sources. Five green
suites or a suite-level JUnit were never used as a substitute for per-method
output; the archives themselves record that JUnit method output may be
truncated, which is why raw LastTest is the per-method evidence. All eight
runs ended in the normal completed-PASS state; no failure/timeout/interrupted
sample exists among these Windows runs, so retention for those states is not
claimed here (synthetic tool-selfcheck scenarios are separate and are not
host evidence). The 30-method Linux native runs are a different input and
were not cross-filled.

Public projections were checked against the private originals:
`n5-review-windows-evidence-index-20261103.json` (28/28 entries, 9/9
source-delta files equal current candidate bytes) and
`n5-review-windows-validation-20261103.md` (its run/job rows, upstream-chain
statements, 34-vs-30 separation and state lines all match the originals).
A projection is a checked projection, never the private original. Restricted
private hosting remains **BLOCKED** under the recorded user decision. One
observation is routed to the owner without change (out of this direction's
edit ownership): the existing published index carries an absolute local
evidence-root path in `private_evidence_root`.

## W3 — Existing readonly SID/ACL boundary coverage

Sources read in the current candidate: `readonly_resources.cpp`,
`readonly_windows.cpp`, `readonly_tests.cpp`, `system_windows.cpp`,
`test_native_cli.py`, `test_bundle_capture.py`, native `CMakeLists.txt`.
The full 17-row table (requirement → code line → test → executed evidence →
classification) is in the private coverage receipt; summary:

- **PORTABLE_EXECUTED (3)** — bounded explicit policy shape; Builtin
  Administrators `S-1-5-32-544` rejected by the *shared preflight* as only or
  additional trusted owner/writer before any path access (the PR #28 policy
  increment, not a historical vulnerability); historical Store 411 bundle
  compatibility including the no-fallback non-Windows refusal.
- **WINDOWS_SERVER_COMPONENT_EXECUTED (10)** — trusted_sids canonicalization
  and broad/dynamic SID rejection; thread-impersonation refusal before root
  open; the descriptor evaluator (complete owner/DACL acceptance; admin owner
  and admin writer refusal; dangerous allow masks; deny not erasing unsafe
  allow; read-only/deny-only controls; malformed/absent DACL); unknown
  object/conditional ACE and inheritance boundaries; FILE_ID_128 identity;
  same-handle ACL and non-inheritable handle; concurrent-rename replacement
  barrier; the CLI bundle-reader refusals (junction via `mklink /J`, hard
  link, directory-for-asset, same-size tamper, oversized, manifest/catalog
  self-report, no cwd/env fallback) and bounded EOF/size capture with
  read-back identity recheck. Rows 3–8 bind these to the current candidate
  (`8ec950f`/`f15bf1a`/`3a872558` inputs).
- **SOURCE_ONLY (3)** — the full `ordinary_token` profile (non-elevated
  primary token, elevation type, admin-group membership, explicit user SID,
  medium integrity); the policy-bound root chain (volume-root GUID/Subst
  refusal, pinned ancestor observation, NTFS persistent-ACL and
  case-sensitivity refusal, short-name/case-alias leaf check, token-LUID
  rebinding, policy-bound observed reads); executed negative samples for
  ancestor-directory replacement races and mid-capture token changes.
- **WINDOWS11_NOT_RUN (1)** — Windows 11 x64 ordinary non-elevated NTFS
  acceptance of the component; future frozen inputs remain
  build/MSVC/SDK/NTFS/ordinary-token/SID policy.

The two administrator protections are kept distinct: the shared preflight
rejecting Administrators as a *resource owner/writer trust entry* is not the
`ordinary_token` check rejecting an *elevated caller*, and neither was used
as evidence for the other or for any PR-integration history. Subcases added
inside existing methods are not new methods/suites; the default NTFS
ancestor-grant refusal under current policy stays recorded as a deployment
limitation — no host ACL, `trusted_sids` entry or assertion was weakened.

## W4 — Defect assessment

No definite defect or reproducible record-integrity gap was found in the four
in-ownership tool/selfcheck files or the investigated evidence chain:
**`code_change_required=false`**; the counterexample field for a fix is
**`NOT_APPLICABLE`** (documentation/evidence-only increment, nothing
manufactured). Because no tool source changed, the tool selfchecks were not
re-run; the saved 27/17 selfcheck results remain the tool evidence for their
own recorded inputs. No production, shared or out-of-ownership file was
edited; any future gap in those is routed to the owner with the bounded
testcase rather than fixed here.

## States and boundaries

| State | Value |
| --- | --- |
| IMPLEMENTATION_COMPLETE | true — limited evidence-continuation scope of this report and index only |
| LOCAL_VALIDATION_COMPLETE | false — Windows 11 ordinary-user acceptance NOT_RUN; no new local product matrix run |
| PUSH_COMPLETE | false at report freeze; the owner publishes this branch after independent hard review |
| REMOTE_CI_COMPLETE | true for frozen head `3a872558` per saved PR #32 rollup receipts (Windows native push/PR and the recorded check set, all SUCCESS, captured 2026-11-03); not inherited by this report's new files |
| PR_REVIEW_COMPLETE | false — coordinator owns acceptance; local hard review is separate |
| MERGED | false |

Historical image identity stays UNRESOLVED; Windows 11 stays NOT_RUN;
restricted private hosting stays BLOCKED. No N5-2, 014/Store 412, Windows
Workspace write, file/process/Store/VFS/sandbox/cleanup capability, real
model or paid call, production operation, Git history rewrite, main write or
merge is added or implied by this report.

Private receipts for this session (read-back inputs for review):
`implementation-01/w0-baseline-snapshot.json`,
`implementation-01/w0-evidence-inventory-before.json`,
`implementation-01/w1-image-chain-verification.json`,
`implementation-01/w2-archive-method-verification.json`,
`implementation-01/w3-readonly-coverage.json`,
`implementation-01/w4-end-snapshot.json`,
`implementation-01/implement-summary.json` under the task's private evidence
directory (outside Git). This report references them by name only; raw
LastTest/JUnit bytes, private paths and machine environments remain
unpublished.
