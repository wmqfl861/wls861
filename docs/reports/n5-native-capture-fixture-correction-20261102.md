# N5 native resource fixture correction

Time scope: 2026-11-02 UTC, after the original PR #22 Windows failure and
before this correction's commit, publication or CI. The fixed input is
`79654ca9cca80d5ae8f5204df580797b04963460`, tree
`2243d75d78669a6a292a1059d50033b5cfa93d21`. The dedicated branch is
`fix/n5-native-capture-fixture-20261102-csj777e9`, based on
`work/n5-integrated-candidate-20261102-csj777e9` / [PR #22](https://github.com/wmqfl861/wls861/pull/22).
This bounded correction implements the coordinator-assigned N5 task; formal
acceptance and merge remain separate.

## Observed failure

The [original push run 41461451274](https://github.com/wmqfl861/wls861/actions/runs/41461451274),
[job 125331237861](https://github.com/wmqfl861/wls861/actions/runs/41461451274/job/125331237861),
actually checked out the exact `79654ca...` input. Its 14:24:45–14:25:54 UTC
execution compiled the native slice and ran all five native CTest registrations.
`native_readonly` reported six passed methods and one failed method:

```text
FAIL historical compiled public capture stays compatible: line 57: captured.count() == 35
RESULT 6 passed, 1 failed
```

CTest reported four passed registrations and one failure, with exit **8**.
The existing Python runner preserved this failure as
`RuntimeError: ctest failed with exit 8`; the outer Actions command exited **1**.
The actual CTest command was:

```text
ctest --test-dir D:\a\wls861\wls861\native-work\build -C Release --output-on-failure --output-junit D:\a\wls861\wls861\native-work\evidence\ctest.xml
```

The configure log independently reported **36 assets / 166684 bytes**, manifest
`f03bce7bb55b02f7740c81473c12301ecc437b35d980d629fb2d896196d4dc30`.
The new readonly compatibility fixture still expected the earlier 35 assets,
160549 bytes and manifest `b9f5468c6858ebd467fb4e6905d2ef4b8bd51a006e4fd2a636f8e4db1fde55cf`.
This is an integration fixture omission after the approved resource addition;
the evidence does not establish a defect in the Windows permission mechanism.
No test was rerun to replace this original red result.

## Exact source of the corrected expectations

The already integrated Store change
`88c8259776a0b2773287fbe03abc7049db4c2c36` registered the explicitly approved
`db/migrations/013_event_call_association.sql`. Against the original native
baseline `aa2c592b8aed6509189710cc6f6d69e0959098a8`, a read-only comparison of
fixed Git objects establishes:

- All original **35** manifest entries retain their original paths, kinds,
  SHA-256 values and byte counts.
- The sole new asset is migration 013, **6135 bytes**, SHA-256
  `141d80fe5bbdf67e91081d88a8405e2cf4a306fcef1d3fe1a029eaa9c183ac18`.
  Thus the current exact set has **36 assets / 166684 bytes**.
- Every one of the 36 actual committed files matches its manifest size/hash.
  The ordered entries in `src/runtime_resource_catalog.inc` match every field
  of the manifest, including its fixed `std::array<ResourceAsset, 36>` length.
- The failing integrated input's `platform/native-foundation/manifest.sha256` pin equals the
  actual SHA-256 of `resources/runtime-manifest.json`:
  `f03bce7bb55b02f7740c81473c12301ecc437b35d980d629fb2d896196d4dc30`.
  CMake passes that exact pin to bundle capture and configures the compiled
  identity from it. The existing native CLI fixture already expects 36/166684.

The sole test change updates the three literal expectations in
`platform/native-foundation/tests/readonly_tests.cpp` and explains their Store
411 provenance. The existing method still calls the historical public capture
API and checks an exact count, byte total and digest. It does not derive an
expected value from the capture under test or accept an arbitrary current
manifest. The Linux Unsupported check and every Windows policy/identity
assertion remain intact. No production source, resource byte, published SQL,
compiled catalog, deadline, CMake registration or permission policy changes.

The earlier N5-3 implementation document and validation report retain their
original `aa2c592...` input and 35-resource historical scope; they are not
rewritten to imply that the old run used this later 36-resource input.

## Original evidence and static verification

All evidence paths below are relative to the data-disk task root
`/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/` and remain outside Git.
The original job log is preserved at
`evidence/n5-pr19-pr20-pr22-remote-status-01/125331237861-job.raw.log`
(41769 bytes, SHA-256
`0f8f4a8697ef960c4fa43b91e76cfe00403307a3b6f841ef314fdc4de2489f7b`).
Its checkout witness is on original lines 103–104, failure on line 305 and
outer exit on line 323.

The original [failure artifact 13149561362](https://github.com/wmqfl861/wls861/actions/runs/41461451274/artifacts/13149561362)
is archived under `evidence/n5-native-capture-fixture-failure-01/`.
Its ZIP SHA-256 is
`ad482a97405e34dacc089fd4a6bab4f1aae5c9ad40ee7bea3225134201c80a49`,
matching both the API digest and the original same-job upload log. Archive
members were treated only as data; no archived code was run.

| Retained evidence | Exact identity / result |
| --- | --- |
| `member-014.raw`: complete original `ctest-logs/LastTest.log` | 7452 bytes; `036cc0ebdb8f4af8c54d5573748f05c60fc526ff8423646234371eed14bee26e` |
| `member-016.raw`: original `ctest.command.json` | exit 8; `f9ea663536c7d7737fe1e52dba7ef49a308ccfb1139e219fcd7df26ac2cfc619` |
| `member-032.raw`: original `source-inputs-before.json` | all 867 paths, byte counts and hashes equal the exact committed tree; `0fa057568d6a1304c95ceffc01eff72a234814f06743ee589d6bb27ca77095f8` |
| `resource-source-verification.json` | 36 committed asset bytes and catalog/pin correspondence verified; `f7fc3da2d5c8f67ba625afcd6e642f6c07a4d47555c73a8816189c0383142c9d` |
| Failure evidence `FROZEN-SHA256SUMS.json` | `d6721baec8aaea49c741a0022739b6c3ce6326cdf62cd85e77a50f932d39bc25` |
| `evidence/n5-native-capture-fixture-static-01/review.json` | exact three-constant/comment diff and original 867-file source binding; `f31b53e54d9ce849ff7e18bead068e300b0238aa7beed7621f295d6d36cb24e6` |
| Static evidence `FROZEN-SHA256SUMS.json` | `9d19463a6bd7f3ba5f6d720b12da944f8b99650c46c0648c2dbfe7e509798875` |
| Corrected readonly test source | `5f8c620eb3948d714e833dd37ccf133b11c812746ca77e0962e2c224fe2df14e` |

The failure artifact does **not** contain a post-test source manifest or binary
before/after identities. That evidence gap remains explicit; the source
snapshot does not establish unchanged runtime binaries. The read-only collectors
and static audit exited 0, retaining their commands, raw outputs and receipts.
Those results are evidence checks, not passing native product tests.

## Remaining validation and delivery

No local compiler or native/product test is run by this correction: the local
execution Agent reserved that machine for the original frozen integrated Linux
matrix. The existing Windows CI must execute all five native suites against
the correction's actual new commit. A passing parent or different SHA cannot
stand in for this result. The original Linux 94-registration matrix and the
separate PR #20 Make-ASan failure retain their own inputs and outcomes.

At this report's pre-publication time scope, `IMPLEMENTATION_COMPLETE` is true
only for this literal fixture correction. `LOCAL_VALIDATION_COMPLETE`,
`PUSH_COMPLETE`, `REMOTE_CI_COMPLETE`, `PR_REVIEW_COMPLETE` and `MERGED` are false.
Independent review, actual publication SHA, push/PR checkouts and the new CI
outcome belong in a later external receipt. Windows 11 ordinary-user acceptance
remains `NOT_RUN`, deployment policy remains `POLICY_PENDING`, and the historical
image-source discrepancy remains open.
