# N5 native runner identities on failed execution

Time scope: 2026-11-02 UTC through the local tool red/green run at
`2026-11-02T15:06:04.424993+00:00`, before this correction's commit and CI.
Base: `a99ad867a25ad0404bf4b4b7e6a5b76d446022d0`, tree
`f5b49bb7cef4c9f38bd849caa6f439998520dddb`, [PR #23](https://github.com/wmqfl861/wls861/pull/23).
The independent branch is `fix/n5-native-failure-identities-20261102-csj777e9`.
This bounded correction implements the coordinator-assigned N5 evidence task;
formal acceptance and merge remain separate.

## Real evidence gap and bounded correction

The original exact-`79654ca9cca80d5ae8f5204df580797b04963460`
[Windows push job 125331237861](https://github.com/wmqfl861/wls861/actions/runs/41461451274/job/125331237861)
retained its failing CTest command, exit 8 and complete original LastTest log.
Its [artifact 13149561362](https://github.com/wmqfl861/wls861/actions/runs/41461451274/artifacts/13149561362)
also retained the 867-file source-before manifest, but no source-after or
binary-before/after manifests. The original runner captured executable hashes
only after successful method verification, and source-after only at the
successful end. This is a real failure-evidence gap. The separate obsolete
resource fixture was corrected by PR #23; no old log or report is rewritten.

`platform/native-foundation/tools/run_windows_ci.py` now:

- Captures a nonempty `binary-inputs-before.json` after the build succeeds and
  before invoking the original CTest command.
- Collects `source-inputs-after.json` and, when a build completed,
  `binary-inputs-after.json` independently in finalization. Success, nonzero
  exit, timeout and a catchable interruption all reach this collection path.
- Records `identity-finalization.json` with available pairs, explicit unknown
  comparisons, equality results and capture errors. Missing or empty snapshots
  cannot establish complete identities. Available evidence is still retained
  when the other snapshot fails.
- Preserves the original command failure or interruption when writing output,
  command receipts, terminal output or original CTest logs also fails. An
  apparent command success still rejects an evidence capture failure.
  `failure.json` retains the original type/reason and explicit termination kind;
  an unwritable failure destination cannot replace the original exception.
- Writes the successful `result.json` only after complete unchanged source and
  executable identities have been verified. The original `binary-inputs.json`
  remains a compatibility record after successful native method verification.

The existing CTest argv, **300-second** child deadline, five native registrations,
method verification and native method order are unchanged. Configure/build
deadlines remain 180/600 seconds; native registration deadlines remain
30/30/30/60/30 seconds. No native product source, public resource, SQL, platform
policy, Store behavior or Linux 94-registration catalog changes.

Independent source reviewer `/root/concurrency_fix` identified that the normal
nonzero subprocess return path could still be masked by an output-write error.
The final candidate fixes that path as well: stdout, stderr, the command
receipt and terminal writes are attempted independently, while the original
nonzero exit remains the primary failure. A real synthetic CTest failure plus
an injected stdout-file write error verifies this correction. The final
non-author review disposition is recorded separately after the code hashes are
frozen; this report does not substitute for that review.

## Same-condition tool red/green execution

The same new `test_windows_ci_evidence.py` source and same synthetic fixture
source were used with the original runner from the clean PR #23 worktree and
the candidate runner. Discovery and native build/install remain substituted;
the ordinary fixture CTest processes, stdout, JUnit and original LastTest files
are real. Timeout/KeyboardInterrupt and evidence I/O damage are explicit tool
fault injections. No native compiler, native executable, Windows machine,
provider, production data or paid API was exercised.

The existing 16 methods are retained, with success/failure/timeout assertions
strengthened. Six new methods cover interruption, source/binary mutation,
failed source-after capture, failed original-log capture, an empty executable
set, and a failed command-output capture. The source/binary mutation method
contains two subcases; it remains one method. This is **one tool suite with 22
methods**, executed twice, outside both the 94 Linux product registrations and
the five native registrations.

| Input | Actual result |
| --- | --- |
| Original runner at `a99ad867...`, identical new 22-method suite | exit 1; unittest reported 6 failures and 4 errors, including two subcase outcomes within one method; 22 methods ran in 7.964 seconds |
| Candidate runner, identical suite and source fixture | exit 0; all 22 methods passed in 7.739 seconds |
| Both outer collection steps | no timeout; all owned process groups gone; no background run counted as complete |
| Candidate and baseline tracked-source identities | 868 files each, equal before and after their executions |
| Native/Windows/product build or test of this new candidate | NOT_RUN locally by this correction |

The original runner already rejected a changed source on its successful path;
that guard is preserved. Its red source-mutation subcase also shows the missing
final identity receipt. Other old failures prove missing binary-before at
CTest entry, lost failure/timeout identities, acceptance of empty or changed
executable sets, and masking of the original CTest exit by collection errors.
No failure caused by a missing new public product API is used as a counterexample.

Execution used ordinary UID 1000, the existing interpreter
`/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python`, and a private
data-disk `TMPDIR` ending in `tmp/native-evidence-red-green-01`. The root task
directory was `/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9`.
From its `native-evidence-fix` worktree, the exact command pattern was:

```text
/mnt/d/codex-workspace/wls861-r2-4-20261029/test-venv/bin/python -B /mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/native-evidence-fix/platform/native-foundation/tools/test_windows_ci_evidence.py --runner RUNNER --source-root /mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/native-evidence-fix --retain-fixtures DESTINATION
```

`RUNNER` was respectively the `native-capture-fix` and `native-evidence-fix`
worktree's `platform/native-foundation/tools/run_windows_ci.py`.
`DESTINATION` was respectively `baseline-fixtures` and `candidate-fixtures`
under the evidence directory below. The receipts retain both complete argv
arrays, UTC times, real exits, interpreter, UID, fixture paths and hashes.

## Frozen evidence and remaining gates

The complete receipts, raw stdout/stderr and original synthetic fixture trees
are retained outside Git at `evidence/n5-native-evidence-red-green-01/`.
Their input snapshots predate this new report, which is not claimed as an
executed test input.

| Input or evidence | SHA-256 |
| --- | --- |
| Original runner | `3145783ac53afd928f34c2b876121372bbbb16932918f363b1561c1c7752112e` |
| Candidate runner | `68ad5ffe23f12844b64f79e4e8ea046226b2074b4a960b4ae14055e482559aab` |
| Identical test source for both runs | `830a7b5426714b9dc0d20e81d5ebcbd5784d717ce4888dc5617f43846a62e52c` |
| Baseline original stderr | `3c65dbbb3a7f77c20e5e6afc5bbe7d810280044df03946961b88bb0d4d8b3678` |
| Candidate original stderr | `3e124a3d6536f647f7586066e97e3da4205e986da17a1fa30880c178f3a11b5d` |
| Candidate source before and after | `06c9299997619572060b6cc32742f75529da3942fc8b2c22e66fd7db8068b7a6` |
| Baseline source before and after | `8727fd4ccbe20ec155990427d786abfb6f612e4c0c80b7c5d0336db71460df9a` |
| `summary.json` | `a4eaec2d10be09dfbadbd9ace8bf4579a064a45efbe9badaf1712c30e582eec9` |
| `FROZEN-SHA256SUMS.json` | `32721404f8861790da739e866c1e382ba2b4d6dd3ea1858bcee232476e009892` |

The original remote artifact, source binding and exact missing-evidence finding
remain at `evidence/n5-native-capture-fixture-failure-01/`, manifest SHA-256
`d6721baec8aaea49c741a0022739b6c3ce6326cdf62cd85e77a50f932d39bc25`.
No later fix can retroactively create its absent identities.

At this report's pre-publication scope, `IMPLEMENTATION_COMPLETE` is true for
this bounded runner correction and the listed tool validation is complete.
`LOCAL_VALIDATION_COMPLETE` for the full native/product gates, `PUSH_COMPLETE`,
`REMOTE_CI_COMPLETE`, `PR_REVIEW_COMPLETE` and `MERGED` remain false. Actual final
SHA, draft dependency and that SHA's CI require a later publication receipt.
Original PR #22 stays at its original input and failure; this change targets
PR #23 on a separate branch. Windows 11 ordinary-user acceptance, adopted
deployment policy and the historical image-source discrepancy remain open.
