# N5 test Gate stop-wakeup repair — 2026-11-03

## Input and scope

This is a test-helper fix on the exact integrated input
`f15bf1a944a1ea5f6b9e5e9696b921caa48f04cc` (Draft PR #31), on branch
`fix/n5-stop-gate-20261103-74bnxwj5`. The production source, public headers,
SQL, resources, original failure records, CMake registrations and deadlines are
unchanged. The candidate changes only `tests/persistent_event_runtime_tests.cpp`
and this new report/machine index. Its test-source SHA-256 is
`3e660d205faa7fdaaa1b8bae92a2480d8e72025377f92d6a65119a000b2bb0aa`.

The report freezes prepublication facts at `2026-11-03T07:48:20.034865+00:00`. Final commit, publication,
PR and remote checks are separate postpublication receipts; these are not
self-referential claims about a commit that had not yet been made.

## Observed failure and cause boundary

PR #30 Push Clang-18 run [41592497997](https://github.com/wmqfl861/wls861/actions/runs/41592497997),
job [125772192904](https://github.com/wmqfl861/wls861/actions/runs/41592497997/job/125772192904),
attempt 1, actually checked out `3a2dd55d51e32e7e49ed546ccb4c441d3bd96782`.
It finished with 93 passed / 1 timeout out of that input's 94 registrations.
The original 180-second runtime suite timed out at 180.10 seconds after eleven
method PASS lines and `START destructor joins the live callback before releasing
the actual Store writer lease`, with no following PASS/FAIL. The original console
is retained privately (65,894 bytes, SHA-256
`b771a5dbebc0abaadf5a8f0f8e9633dc428e2bf2c1dc55ce9b212a90fbd7807c`).
Other successful runs of that same method remain separate evidence and do not
convert this timeout into success. No rerun or cancellation was used to replace it.

The inherited test `Gate` used a `condition_variable` predicate that checked
`stop_requested()`, while its stop callback called `notify_all()` without the
predicate mutex. A stop after the predicate captured false and before blocking
could therefore be lost. The host could join a callback still sleeping in the
fixture, while the fixture's later cleanup release could not run until the join
returned. The remote console does not contain a stack trace proving its exact
instruction at timeout. The deterministic helper counterexample below proves the
real lost-wakeup mechanism; it does not invent that missing stack trace or
explain the original historical ASan/Make failures.

## Minimal repair and permanent regression

The test helper now uses C++20 `condition_variable_any.wait(lock, stop, predicate)`.
It continues to return on either explicit release or stop, leaving the existing
model response, Store actor, accounting, late-evidence and actual writer-lease
assertions intact. A one-shot optional hook exists only in this anonymous test
helper. Normal model gates leave it empty; no production API exposes test controls.

A new permanent method requests a real stop from that hook after a false
predicate was captured and before blocking. It samples completion with the
existing five-second bound, then always releases and joins the callback before
asserting `completed_from_stop`. Thus the old helper yields an ordinary target
assertion failure with orderly cleanup, rather than an outer timeout. The fixed
helper must complete before cleanup releases it. All original fourteen methods
remain; the runtime suite now has fifteen methods. The integrated product still
registers 95 CTest suites, including the previously added Store compatibility
suite. This change adds no registration, changes no existing five-second wait,
and preserves the runtime suite's 180-second CTest deadline.

## Same-condition extracted-helper red/green

The old helper was extracted from the exact f15 source. Its private derived copy
adds only the optional timing hook after the original predicate captured false;
it retains the original `condition_variable` and `stop_callback` algorithm.
The fixed helper is copied from the candidate. Both private translation units
contain the byte-identical permanent regression function. Each uses strict
`-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
-pthread`. GCC is Ubuntu 13.3.0 and Clang is Ubuntu 18.1.3.

| Compiler | Helper | Compile exit | Run exit | Duration | Actual result |
| --- | --- | --- | --- | --- | --- |
| gcc13 | old | 0 | 1 | 5.050 s | target CHECK failure |
| gcc13 | fixed | 0 | 0 | 0.005 s | stop-only completion |
| clang18 | old | 0 | 1 | 5.055 s | target CHECK failure |
| clang18 | fixed | 0 | 0 | 0.004 s | stop-only completion |

Both old runs emitted exactly `FAIL CHECK failed: completed_from_stop` after
release and join; both fixed runs emitted `PASS stop completed before cleanup
release`. No outer watchdog timed out. Source and ELF identities match before
and after every run. These four standalone diagnostics are not product CTest
registrations or sanitizer gate passes. The two intentional failures are real
red examples for this test-helper defect, not new production vulnerabilities.

## Full affected runtime validation

A candidate GCC 13 strict Release test translation unit was compiled against the
unaltered f15 GCC Release libraries, with original f15 production headers and
resources in the isolated candidate worktree. This is a directed derived-input
check, not the final full-product matrix or a full Clang production build.

| Check | Result |
| --- | --- |
| Strict candidate test TU compile | exit 0, 62.90 s |
| Original runtime registration, 180-second deadline | 1 PASS / 0 FAIL / 0 TIMEOUT / 0 SKIPPED / 0 NOTRUN, CTest exit 0, 15.04 s |
| Method output | 15 PASS / 0 FAIL; all original 14 methods and the new stop-gap method appear in complete LastTest |
| 223 source/header/vendor/config/schema/prompt/SQL/fixture inputs, two original archives and resulting ELF | unchanged before/after |

| Executed artifact | SHA-256 |
| --- | --- |
| Original f15 `libwls_s1.a` | `6cf0984c28956d3c5a4d18df0ac1e5d1385cd77bce31d25b6dc9aa9d8207c9ff` |
| Original f15 `libwls_kernel.a` | `a780803a40cb241a40454fad86b7ec19da7e28babb66110aa49ce6f0f2e1767f` |
| Candidate runtime ELF (5,042,264 bytes) | `3d1e0dfdd69c72e812f210626aaf7f64fff281b6834c96818e989a8759fdb33e` |
| Complete private LastTest (3,191 bytes) | `508edb9db097e432a8be4a4c06f886cbe3d8b560b896b8867fcddd5b6005796d` |
| Private JUnit (1,473 bytes) | `c01828892eb8000b0fdf936de2eef0e38584d574254092d60ed02becab755d04` |

## Evidence and review

All builds, sources, temporary fixtures and evidence are on the verified ext4
`/mnt/d` data disk, running as uid 1000 with private task TMPDIRs. Raw logs are
retained under
`/mnt/d/codex-workspace/wls861-n5-review-20261103-74bnxwj5/evidence/`:

- `linux-remote-watch-03/125772192904.log`: immutable original remote failure.
- `stop-gate-sensitivity-01/`: exact original helper/source, old/fixed probe
  inputs, commands, versions, exits, source/ELF identities and raw streams.
  `executed.json` SHA-256 is `74ecae0d33299ded72c7d4ae0f7889ff1c8c8cb35816ce92ba71bce9b409fae0`.
- `stop-gate-runtime-release-01/`: copied final test inputs, original archive
  identities, strict build, complete LastTest/JUnit, original/candidate method
  lists and 223-input before/after manifests. `summary.json` SHA-256 is
  `8de7b3ec399c52d7da146aa80f256ca52ad6bafba7a844ad1146e050ad6c4371`.

Non-author reviewer `/root/compatibility_sidecars` read the exact static source
and derivation and found no source blocker. That original receipt SHA-256 is
`0e0b62a881b89fd701ce66203b13d287c7cb78ba51d9a8e267510cd46bcb2c17`.
An immutable addendum corrects its author metadata to `/root/persistent_hardening`
and verifies the four executed probe receipts and original outputs, while
preserving the original receipt bytes; addendum SHA-256 is
`22acf6f8fb1b932f3f33948c35292d2e23a6a2747fc595f944925f273aef0c69`.
The reviewer did not independently compile or run the tests. This is engineering
cross-review, not second-model assessment or main-coordinator formal acceptance.

## Remaining gates and publication state

The new input still needs the integrated full Release checks for both umasks,
strict Clang, UBSan, shared GCC ASan/LSan, Make-ASan and its own remote CI.
The parent owns those matrices and keeps original f15 and new-input results
separate. The original local sanitizer/Make diagnoses remain OPEN/UNRESOLVED;
this helper fix does not close them. Restricted remote raw LastTest/JUnit hosting
remains BLOCKED; local originals are retained. Windows 11 x64 ordinary-user NTFS
acceptance remains NOT_RUN. No Windows scope, DAG or 014/Store 412 implementation
is added.

At this prepublication freeze: `IMPLEMENTATION_COMPLETE=true`,
`AFFECTED_LOCAL_VALIDATION_COMPLETE=true`, `LOCAL_VALIDATION_COMPLETE=false`,
`PUSH_COMPLETE=false`, `REMOTE_CI_COMPLETE=false`, `PR_REVIEW_COMPLETE=false`,
`MERGED=false`. Normal publication and later exact-input validation are recorded
outside this immutable freeze. No PR or main was merged and no real supplier or
paid model call was made.
