# Billing E2E entrypoint: immediate subtest failure evidence

The signed-billing E2E script now selects the existing `ImmediateTextRunner`.
An already registered subtest failure is written and flushed before the test
continues into later fixture work. This closes an observation gap; it does not
change billing, settlement, authorization, assertions, test order or deadlines,
and does not resolve the underlying business failures or sanitizer timeouts.

The exact parent is `08dad7e815b9c3c84d3bd2393818b2030a6e0cde`.
The isolated branch is `fix/billing-subtest-diagnostics-20261101`, directly
dependent on Draft PR #12. No earlier PR is merged or retargeted by this work.
Source, exported inputs, temporary files and evidence are on verified
`/mnt/d` (`/dev/vdb2`, ext4); execution uses ordinary UID/GID 1000.
The test interpreter is the existing data-disk venv, Python 3.12.3.

## Scope and independent review

Local ZCode authored only two files: the import and `unittest.main` runner
selection in `tests/test_billing_baseline_e2e.py`, and one entrypoint selfcheck
in `tests/test_immediate_unittest.py`. The shared helper is unchanged.
No production source, SQL, CMake registration, test catalog, permission or
sanitizer setting changes. The product catalog remains 92 CTest registrations.

The new selfcheck uses `runpy` to execute the actual billing script's
`__main__` entrypoint, replacing only unittest's loader with one synthetic
subtest. It verifies that the failure has been registered, then parks the
child on stdin. A reader barrier, rather than a sleep, establishes that state.
The parent checks the original traceback before terminating and reaping the
child; no end-of-suite summary exists to supply it later. Child audit hooks
reject subprocess creation, shell execution, socket connect and socket bind.
No billing fixture, Store, CLI, model request or transport test is executed.

The non-author `/root` reviewed the exact patch, child lifecycle, completed
receipts and raw tool logs. Removing precisely the import and runner argument
produces the original billing AST; removing precisely the new method produces
the original four selfchecks' AST. Both versions use the unchanged helper.
The bounded review found no blocker and verified 28 recorded input identities.
This is engineering review, not the main coordinator's acceptance or a claim
of an independent second-model evaluation.

## Actual red and green results

The new selfcheck itself was copied unchanged into a separate export of the
parent. All billing/import dependencies in that red tree retain their own
parent bytes. The red failed at the missing-traceback assertion, after the
synthetic subtest had registered one failure and no error. It did not fail
because of import, fixture construction, compilation or an outer timeout.

| Completed run | Actual result | Exit / elapsed seconds |
| --- | --- | --- |
| Original helper selfchecks on parent | 4/4 tool methods passed; the helper itself already worked. | 0 / 1.231224 |
| Frozen entrypoint probe on parent | Failure registered, but traceback/witness/AssertionError absent before termination. | 1 / 0.897646 |
| Same new selfcheck on parent entrypoint | 1 tool method failed at `assertIn("Traceback (most recent call last):", before_termination)`. | 1 / 1.638867 |
| Same frozen entrypoint probe on candidate | Original traceback, witness and AssertionError present before termination. | 0 / 0.700421 |
| Same new selfcheck on candidate entrypoint | 1/1 tool method passed. | 0 / 1.443146 |
| Complete candidate diagnostics selfchecks | 5/5 tool methods passed; no skipped tests. | 0 / 1.401093 |

The two frozen probe runs use identical probe SHA-256
`8b1c69c63ac1a7a89c5874907026fa4efc14ed4330b26aea165a694266da6357`.
Both children deliberately exit via TERM (`-15`), are reaped, and have identical
pre/post-termination stderr. Their drivers' exits 1 and 0 are the diagnostic
red and green, respectively. No listed run has an outer timeout.

The same new test file in red and green has SHA-256
`ffbb28163c577c7731775583502c5bf7225ff0146f11482864e5976790dcfb87`.
Parent billing script SHA-256 is
`0c3d0f0d8d5c61b58f4e781cea3fa983b6f99d07603a8a59c2902638ca43a159`;
candidate billing script SHA-256 is
`b5c057d3ece899f9e3375050874b18714a4720b1a78c9ef592e505824187d880`.
The helper SHA-256 remains
`5ca64485292d7acba95a098f2459375a2349ad396e548038a417cbe2660cca57`.

The selfcheck commands use the existing venv interpreter and are:

```sh
python -B tests/test_immediate_unittest.py \
  DiagnosticsContracts.test_billing_script_main_entrypoint_emits_subtest_failure_before_termination
python -B tests/test_immediate_unittest.py
```

These counts describe test-tool methods, not additional product suites.
The existing CI diagnostics step already executes this selfcheck file.

## Author session and evidence boundary

The fresh task-private ZCode session started at 06:28:42 UTC on 2026-11-01.
After writing the candidate it continued wait/sleep calls without source
progress. Its own incomplete internal tool invocation is not counted as a
successful test. At 06:42:46 UTC the supervising Agent interrupted only that
verified private process group with SIGINT. The launcher reaped it with exit
130 after 844.524024 seconds, `outer_timeout=false`; TERM/KILL escalation was
unnecessary. Candidate source hashes before and after interruption match.
The independently executed red/green and five passing tool methods above are
separate completed executions. The formal product matrix was not interrupted
or modified.

Private original evidence is under
`/mnt/d/codex-workspace/wls861-zcode-followup-20261101-wvm7aclp/evidence/`:
`billing-entrypoint-red-green-01/`, `billing-root-review-01/` and
`zcode-billing-round-01/`. Coding-session contents, prompts, settings,
credentials, databases and binaries are not public payloads.
The adjacent versioned evidence JSON preserves explicitly selected original
tool receipts/logs and the non-author review, with original hashes.

## Delivery state at this report's freeze

| State | Accurate scope |
| --- | --- |
| IMPLEMENTATION_COMPLETE | TRUE for the test-only entrypoint wiring and selfcheck. |
| LOCAL_VALIDATION_COMPLETE | TRUE for the bounded diagnostic tool red/green; **not** a full product/sanitizer acceptance claim. |
| PUSH_COMPLETE | PENDING at report freeze; actual commit/ref and Draft PR are recorded after normal publication. |
| REMOTE_CI_COMPLETE | PENDING for this new commit; it cannot inherit parent 08's CI. |
| PR_REVIEW_COMPLETE | PENDING; main coordinator acceptance is separate. |
| MERGED | FALSE. |

Current and historical billing/live-workflow failures, local GCC/Clang
sanitizer roots and Windows 11 ordinary-user acceptance retain their existing
OPEN / NOT_RUN status. No real WLS provider call or paid WLS runtime model
request, production operation, main write, force push or merge is performed by
this work. The separately authorized ZCode authoring session is outside that
product-execution statement; no zero-usage claim is made for it.
