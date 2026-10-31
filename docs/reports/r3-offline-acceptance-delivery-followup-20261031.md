# A2 complete delivery assertion correction — 2026-10-31

This is a new time-scoped supplement after the lifecycle correction in
`6fbcc48d7d8ea1c467effcf5bbade08860b2c747`. It records a second incorrect test
expectation, its original remote counterexample and a bounded test correction.
The candidate's full native execution is **NOT_RUN** at this report's freeze.
No production Store, delivery implementation, evaluator or fixture is changed.

## Actual failure after unittest dispatch was restored

Push run [41187210586](https://github.com/wmqfl861/wls861/actions/runs/41187210586)
uses head `6fbcc48d7d8ea1c467effcf5bbade08860b2c747`. Its completed
[Clang-18 job 124496453312](https://github.com/wmqfl861/wls861/actions/runs/41187210586/job/124496453312)
ran all ten CLI test methods: nine passed, and
`test_03_rework_exhaustion_and_exact_delivery_keep_original_bytes` failed.
The CTest invocation completed with 90/91 and exit 8.

The original failed assertion required every decoded member of
`frozen_plan.files` to equal `b'alpha candidate version 1\n'`. The first sorted
member was the original acceptance evidence JSON, producing:

```text
AssertionError: b'[{"acceptance":{"acceptance_id":"acc_01M6[9690 chars]"}}]' != b'alpha candidate version 1\n'
Ran 10 tests in 6.806s
FAILED (failures=1)
```

In this same original job, `test_01` passed its ten real Store/evaluator cells,
accounting and original source/Agent identity assertions. `test_02` and
`test_04` through `test_10` also passed. That is one CLI suite with ten methods,
not ten independent suites; it remains a failed suite. The exported job log
establishes these executed assertions, but the ten temporary Store directories
and per-cell command reports were not exported. Local execution and full A2
product acceptance are still separate gates.

The raw job log is preserved at:

```text
/mnt/d/codex-workspace/wls861-next-20261031-065247/evidence/a2-ci-41183590624-20261031/new-clang-job-log-02.stdout
```

Its SHA-256 is
`93202855b4516546248dd98f7a11c222ec0f35a68fcb2a505d8cb5233e95c843`.
GitHub CLI first refused terminal escape sequences; that exit 1 and empty
stdout were preserved. The successful retry used
`gh api --allow-escape-sequences repos/wmqfl861/wls861/actions/jobs/124496453312/logs`
with stdout captured directly to a file, exit 0. Archived log text was never
executed.

## Contract and correction

The existing [S1 delivery contract](../implementation/S1-acceptance-delivery.md)
requires original requirements, plan, accepted artifacts, acceptance evidence,
usage, questions, experience, side effects and delivery explanation. Original
`Store::prepare_delivery` creates these files, sorts them, and records the
accepted payload mappings separately in `manifest.artifacts`. The existing
`tests/s15_cli_smoke.py` also distinguishes complete bundle verification from
its accepted artifact assertion. Thus the failure is an A2 expectation defect,
not evidence that production should discard metadata or alter original bytes.

The corrected `test_03`:

- Retains its original rework, exhaustion, receipt, frozen-file size and SHA-256
  assertions.
- Reads the actual published `manifest.json`, verifies its original recorded
  SHA-256 and exact frozen structure, and checks the complete file metadata list.
- Compares every published file with its original frozen base64 bytes, size and
  hash, rejects duplicate names and requires the exact published file set.
- Requires all eight original metadata/explanation files to remain present.
- Requires exactly one accepted artifact, maps it through `manifest.artifacts`
  to original `result.txt`, and checks its original candidate bytes, size and
  hash.

Every file, including metadata, is still checked. The erroneous blanket payload
assertion is replaced by the contract's exact artifact mapping. The other nine
business methods and all helpers are byte-identical to the prior source.
The only additional import is `base64`. Test count and all deadlines remain
unchanged; no assertion is skipped and no production guard is relaxed.

Structural navigation used the `codebase-memory` baseline graph
`wls861-next-cycle`, generation `2026-10-31T06:55:42Z`, followed by direct reads
of the original contract, `Store::prepare_delivery`, delivery code and existing
smoke test. The new A2 test is absent from that baseline graph and was read
directly. No whole-repository completeness claim follows from the index.

## Candidate evidence and review

Original failed source, exact diff, input hashes, command receipts and the
candidate are frozen under:

```text
/mnt/d/codex-workspace/wls861-next-20261031-065247/evidence/a2-delivery-20261031/
```

| Input | SHA-256 |
| --- | --- |
| Original CLI test at `6fbcc48` | `8e17406dd8b662749f3de5c3aaa0284ccb4f1e4b8b8f9e882700e7517b1d5ea5` |
| Corrected CLI test | `7c038c6a725de33d6c26335fe7f00d580eada514786467ed69c2adc26860d583` |
| Exact correction diff | `da46579825dc233539559afbcfbb77c26c7123c27ec5731ea6170b55dcdcf4fa` |
| Unchanged `src/store.cpp` | `f196be42cd740f9484bf2f8b39f8ea668dd35ce6779ade31d4c59145216b3d7c` |
| Unchanged `src/delivery.cpp` | `bf4fe4f2d362054678f7bf85c3c99af7b8956542e14db0a666946e180472ae2c` |
| Original delivery smoke test | `42aebf57ae9c0cc1672a04589ba7c5eb976816906602cfd4b95d889db8c1c490` |

As UID 1000 with Python 3.12.3 and the existing data-disk `TMPDIR`, the unchanged
preparation command was rerun after this edit:

```sh
python3 -B tests/r3_offline_acceptance_tests.py \
  /mnt/d/codex-workspace/wls861-next-20261031-065247/offline
```

It passed 9/9, exit 0; `git diff --check` also returned 0. This exercises module
loading and real unittest lifecycle, not the corrected business assertion.
No local native run or rebuild was started while the original GCC baseline
gate owns the execution window.

Author `/root/evidence_audit` did not independently review this change.
`/root` read the full correction and its contract scope, confirmed that all
metadata remain checked and other business assertions are preserved, and
reported no confirmed source blocker. That non-author review covers this diff
only; it is neither successful business execution nor formal A2 acceptance.

At this source freeze the correction is a reviewed candidate. Its actual
business test is **NOT_RUN**, and A2 `IMPLEMENTATION_COMPLETE`,
`LOCAL_VALIDATION_COMPLETE` and `REMOTE_CI_COMPLETE` remain false. Publication
and final-SHA source/CI checks must be recorded separately. No real supplier,
paid request, production operation, semantic quality acceptance or merge is
claimed or authorized.
