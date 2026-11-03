# N5 review follow-up: complete static method projection

This new record follows coordinator handoff
`d843493fafe158f3a6443bafaa0de95cbf6c05c2`, against product baseline
`c2a5cea64aeea261b214cdfd9a968b53f491110a`. It preserves the original
ASan/LSan, Make-ASan and historical Clang/live_workflow failures. This is a
runner privacy/observability correction, not a production sanitizer fix.

The old public projection discovered inline `test::run({` registrations but
missed grouped/vector registrations. The coordinator identified 59 omitted
method labels in nine suites. The new reviewed static registry contains the
59 labels from their four original sources: doctor diagnostics 32, single-Agent
history/runtime 14, recovery predicates 6 and recovery concurrency 7. The entire
source SHA-256 binds each label set. No method names are learned from run output.

`public_method_names` validates registry format, source confinement, duplicate
paths/labels, full source hashes and the original quoted source literals before
adding labels to the existing allowlist. Existing source/binary identity and
publication checks remain. Only exact static START/PASS labels survive; unknown
labels and dynamic failure details remain redacted. Python unittest labels are
already discovered from fixed `def test_...` and class declarations, separately
from this C++ registry. A source edit in one of these four files requires an
explicit registry review/update; stale hashes fail closed.

The same new grouped-label selfcheck was first run against the original runner.
It exited 1 at the intended assertion, `'static grouped method' not found in
set()`. This is a deterministic **runner selfcheck red**, not a product defect,
build-error counterexample or a removed-guard mutation. With the candidate
runner, the complete selfcheck entry completed **60/60, exit 0**, including
source-hash/literal/path/privacy negatives and the existing real CTest success,
failure and interrupt evidence-persistence checks. Source identities were equal
before and after that run. There was no sanitizer product execution in this
tool selfcheck, and its internal synthetic CTest runs are not product suites.

Reproduce the runner selfcheck with the project's actual test-venv interpreter:

```sh
python3 -B tests/test_run_gcc_asan.py
```

The private data-disk task root is
`/mnt/d/codex-workspace/wls861-n5-review-20261103-74bnxwj5`. New originals:

| Evidence relative to its `evidence/` directory | Result |
| --- | --- |
| `public-method-registry-red-01/` | Same new grouped test, original runner, actual assertion failure |
| `public-method-registry-green-01/` | Complete 60-method candidate tool run, original stdout/stderr and before/after identities |
| `windows-nonauthor-public-method-review-01/review.json` | Non-author `/root/windows_policy_provenance` checked all 59 labels, four full hashes, actual grouped registration sites and retained privacy boundaries; no blocking finding |

The non-author review receipt SHA-256 is
`74ccc2727f700532707223857b080bf3a1f3a5dd0ec30bae026e247b7b0fb62f`.
This engineering review is not the coordinating Agent's formal approval or a
second-model evaluation. Private LastTest/JUnit remain local under the existing
user decision. Hosted private originals remain **BLOCKED**; public projections
are correlated summaries, never substitutes for private originals.

At this pre-publication scope, `IMPLEMENTATION_COMPLETE=true` for the static
projection correction and `LOCAL_VALIDATION_COMPLETE=true` for its 60-method
tool selfcheck. `PUSH_COMPLETE=false`, `REMOTE_CI_COMPLETE=false`,
`PR_REVIEW_COMPLETE=false`, `MERGED=false`. Later publication receipts bind the
actual commit/PR/CI without rewriting historical evidence. Full product local
validation remains blocked independently of this tool result. No deadlines,
assertions, SQL, business permissions, sanitizer settings or provider behavior
were changed.
