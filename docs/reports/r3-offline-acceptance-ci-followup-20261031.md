# A2 CLI test lifecycle correction — 2026-10-31

This supplement records a test-harness defect and its bounded correction after
the first A2 PR CI run. It does not replace the initial validation report, claim
a production Store defect, or establish completion of the ten-cell acceptance
package.

## Original failure and evidence identity

PR [#7](https://github.com/wmqfl861/wls861/pull/7) head
`3cd051be7c3a19e9bcdd7241226b2582d3e5cb18` produced failed run
[41183590624](https://github.com/wmqfl861/wls861/actions/runs/41183590624).
GitHub's PR checkout used merge commit
`6166a774b4c0f51c83806774ef318f53dbdb9e03`; the archived GCC source manifest
identifies that checkout. The A2 runner, evaluator and both test files exactly
match the local pre-correction bytes. GCC's source-before/source-after and
binary identity checks report unchanged inputs.

All six jobs failed with the same integration-harness exception:

```text
unittest/case.py: return self.run(*args, **kwds)
TypeError: 'PosixPath' object is not callable
```

| Original job | Job ID | Recorded result |
| --- | --- | --- |
| strict | 124485855982 | CTest 90/91; exit 8 |
| strict-umask-0002 | 124485855778 | CTest 90/91; exit 8 |
| Clang-18 | 124485855939 | CTest 90/91; exit 8 |
| UBSan | 124485855952 | CTest 90/91; exit 8 |
| GCC ASan | 124485855944 | CTest 90/91; exit 8; gate incomplete |
| Make-ASan | 124485856019 | Same CLI exception; make exit 2 |

The only failed CTest entry was `r3_offline_acceptance_cli`. These are failed
complete CI invocations, not successful A2 validation. The original ten business
test methods did not begin. `setUpClass` had returned from `run_fake`, but the
exported artifact contains no ten-cell Store databases, command receipts or
evaluation report. That control-flow fact does not prove the business cells
passed; per-cell failures may be represented in the runner's returned report.

Read-only GitHub retrieval archived run/job/artifact metadata, all 67 original
log members and the sole 37-member GCC evidence artifact under:

```text
/mnt/d/codex-workspace/wls861-next-20261031-065247/evidence/a2-ci-41183590624-20261031/
```

The complete log ZIP SHA-256 is
`d88a40cca1679971eb716720773e9e5bb49fd58a35242866e29b51d798a636ab`.
Artifact `13035948462` is 130,048 bytes with SHA-256
`451f5922fe5526cecdb216c8e0ba35287e06be9c8d99f4bae2a514841766c8de`,
matching GitHub's advertised digest. Extraction rejected absolute paths,
parent traversal and symlinks and recorded each member's size/hash. No archived
code was executed. Raw downloads, command arguments, UTC start/end, exit codes
and hashes are retained outside Git.

## Cause and minimal correction

`OfflineAcceptanceCLI.setUpClass` assigned the output directory to `cls.run`,
shadowing `unittest.TestCase.run`. The class fixture completed, then unittest
tried to call that path as the first test's execution method.

Only that directory field and its 19 references are renamed to
`run_directory`. The ten original integration methods, their assertions,
business scenarios, CLI runner, evaluator, public build registration and
deadlines retain their behavior. No runtime API, SQL, configuration material,
Store guard or accounting rule changes.

A ninth lightweight preparation test imports the actual CLI test class and
drives its real inherited class/case setup, dispatch and cleanup. A single
empty workload stub replaces `run_fake`; subprocess creation and SQLite opens
are explicitly rejected. The empty plan/index files are framework test data,
not fabricated Store or acceptance evidence. The test verifies that its probe
body executes once and both case and class temporary directories are removed.

## Executed local red/green check

User `ubuntu` (UID 1000), Python 3.12.3; checkout, evidence and `TMPDIR` are on
the `/mnt/d` data disk. With the new lifecycle regression added and the original
CLI test file still unchanged, execute:

```sh
TMPDIR=/mnt/d/codex-workspace/wls861-next-20261031-065247/evidence/a2-ci-41183590624-20261031/tmp \
python3 -B tests/r3_offline_acceptance_tests.py \
  /mnt/d/codex-workspace/wls861-next-20261031-065247/offline
```

| Exact check | Result | Original evidence |
| --- | --- | --- |
| Old CLI field, new lifecycle regression | 8 passed, 1 error; exit 1; identical `PosixPath` exception | `lifecycle-red.{json,stdout,stderr}`, `red-inputs.json`, frozen red source copies |
| Directory field renamed, same regression and command | 9 passed; exit 0 | `lifecycle-green.{json,stdout,stderr}` |
| `git diff --check` | exit 0 | final review capture |
| `git ls-remote --heads origin main` | exit 0; still `a6b8559018a599d7a806b05942857a56d69a00ce` | `remote-main-precommit.*` |

This remains one preparation suite, now with nine cases. The CLI integration
suite still has ten cases; the CTest collection remains 91. No build, linked
test, native CLI invocation or local full integration rerun was started during
this correction because the original formal GCC gate owns the local resource
window.

| Source input | SHA-256 |
| --- | --- |
| New lifecycle regression, identical in local red and green | `3be4a2fc092c5752b82310fa9707b697fd8a314210c777d3a2bea1f71c1c7d54` |
| Original CLI test source | `e63a9526e209c5b97d76e623dc5f70c60735b1214cb2ba038981a6e152345a92` |
| Corrected CLI test source | `8e17406dd8b662749f3de5c3aaa0284ccb4f1e4b8b8f9e882700e7517b1d5ea5` |
| Unchanged A2 runner | `d76e33bb48d894249b3218d469ea93a5affc6231ed12359f5164178d0e6357b9` |
| Unchanged original evaluator | `89bc317e52f90471d07e2c6ec6f84bf3c55e11a9f87ec177812c7dd6efd76a47` |

## Scope of completion

The harness correction has an executed deterministic red/green regression.
Its author remains `/root/evidence_audit`; the author is not an independent
reviewer of this correction. Earlier independent A2 review retains its original
source scope. A later review/publication receipt must bind the final commit and
its own CI; this report intentionally does not predict them.

Full ten-cell local execution, the original ten integration assertions, complete
local Release/sanitizer matrices, and successful CI of the corrected commit
remain pending. No claim of provider quality, real cost, live authorization,
semantic acceptance, coordinator approval or merge follows from the nine
lightweight cases.

At this report's source freeze, A2 `IMPLEMENTATION_COMPLETE`,
`LOCAL_VALIDATION_COMPLETE` and `REMOTE_CI_COMPLETE` remain false. Publication of
this follow-up is pending and must be recorded separately; `PR_REVIEW_COMPLETE`
and `MERGED` remain false. Original failed logs and historical reports are
preserved.
