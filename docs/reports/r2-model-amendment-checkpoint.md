# R2.3 implementation checkpoint — final verification pending

Continuation: execution resumed, the old CLI expectation was corrected and the implementation was published. This historical checkpoint remains intact; current results and the remaining local ASan time limit are recorded in [the validation report](r2-model-amendment-validation.md).

Recorded on 2026-10-25 (America/Los_Angeles), 2026-10-26 06:13 UTC.

## Publication boundary

This is a recovery checkpoint, not a release acceptance report. The model-AMEND implementation exists in the current local checkout but has not been published as a code commit. A local execution-service failure prevented confirming the final Python test correction, rerunning the complete gates, reading the final diff for publication, or reconciling the local and remote commit identities. No execution or approval-rejection result was returned for the pending commands; do not describe this as an approval denial.

- Repository: `wmqfl861/wls861` only.
- Starting integration commit: `c975be3e7d30c1208a0a2c19494e2a186c7f77ca` (`work/r2-integrated-recovery`).
- Verified main before publication: `19ea603dd52bbcd0f34d29fb4b07f088d4f2693c`; this checkpoint does not move main.
- Design ADR-073 is published at `b4977fa7fbbd1ba1f5b913447000f8810c2744bb` on `work/r2-model-amend`.
- That design commit has tree `67a737e364cb31057c2d3e0ab57f64cefcc8f0f6`, identical to the local design commit beginning `53d85c4`. GitHub's commit API created different commit metadata, so the commit SHAs are deliberately not reported as equal.
- Target development version is `0.8.2-r2.3`; it is not a completed published release.

## Implemented locally

The current checkout is `/workspace/scratch/cd9af22fb6e7/wls861`. Preserve it before starting another checkout or changing branches.

1. Dispatcher model AMEND calls capture the original task set and either publish a replacement plan through `submit_amendment_plan` or resolve remaining dispositions on the same plan through `resolve_amendment`.
2. Each model response can select only captured pending nodes and `update`, `keep`, or `cancel` with a nonempty rationale. Runtime revisions, task IDs, CAS versions, teams and epochs are host-derived.
3. Store transactions recheck captured task state versions, team identities and execution epochs, including cases where the request CAS did not change. The first plan checks the complete captured task set; a same-plan follow-up checks exactly the selected captured pending tasks.
4. Omitted AMEND subjects remain paused and pending. `keep` still requires a new TeamLead instruction and Worker acknowledgment. UNKNOWN, original evidence, shared rework limits and normal model accounting remain intact.
5. The event pump consumes only completed IDs from the original claim, preserves request-scoped recovery, and retains the explicit host `--dispositions-file` path.
6. New schemas, CMake/Make registration, CLI version, reference-contract coverage, implementation documentation and roadmap/status updates are present locally. Published SQL, vendored bytes and archived original reviews/handoffs were not rewritten.

Primary changed/new execution files:

- `include/wls/offline_runtime.hpp`, `include/wls/store.hpp`
- `src/offline_runtime.cpp`, `src/store.cpp`, `src/event_runtime.cpp`, CLI version/help
- `schemas/submit_amendment_plan.schema.json`, `schemas/resolve_amendment.schema.json`
- `tests/r2_amend_tests.cpp`, `tests/r2_amend_cli_smoke.py`
- `tests/schema_tests.cpp`, `tests/contracts_tests.py`, CMake and Make registration
- One correction to `tests/r2_rework_cli_smoke.py` was submitted but its completion was not confirmed; inspect the file before reapplying it.

Local documentation includes `docs/implementation/R2-model-amendments.md`, ADR-073, README/CHANGELOG/STATUS/ROADMAP, blueprint/task-card references, review dispositions and a fresh R2.2 integration report. The final `docs/reports/r2-model-amendment-validation.md` still needs to be written from completed final gates.

## Results actually observed

| Scope | Observed result | Meaning |
| --- | --- | --- |
| Exact inherited c975be3 Release, UID 65534, umask 0002 | 52/52 CTest groups passed | Fresh baseline evidence only |
| Final nine new CLI cases against c975be3 | 7 failed, 2 compatibility/isolation cases passed | Feature-red baseline; not a harness failure |
| Candidate strict Release, UBSan, ASan, strict Make builds | All four builds succeeded | Compilation only for ASan/Make at this point |
| New candidate C++ amendment suite | 14/14 passed | Captured-target, CAS, replay, accounting, protocol and fence cases |
| New candidate CLI amendment suite | 9/9 passed | Real temporary files, CLI processes and SQLite; FakeModel decisions |
| Initial candidate full Release, UID 65534, umask 0002 | 53/54 groups passed | Sole failure is the obsolete zero-model-call expectation described below |
| Initial candidate full UBSan, UID 65534, umask 0002 | 53/54 groups passed | Same assertion failure; no sanitizer diagnostic or timeout was observed |
| Final Release 0002/0022, UBSan, ASan/leaks and strict Make | Not completed | Must not be marked green |

The exact c975be3 baseline additionally reported 414 S1 C++ checks, 134 S1 CLI checks, 52 M1 C++ checks, 3 M1 demo checks, 37 reference-contract checks and archive integrity passing. Candidate totals must be obtained from the final successful run, not inferred by addition.

The new test-source SHA-256 values, before the pending legacy-test correction, are:

- `tests/r2_amend_tests.cpp`: `658806b2b490b0f78551f69d2ab9d02a05371873423a5ff9af560e62457b2778`
- `tests/r2_amend_cli_smoke.py`: `495ce4c154ab769bde9bf4c7d1d1f7fa6f1c2878b5b5377fb8d9f30903a0c1e0`

The candidate execution manifest covered 231 files and had SHA-256 `31ed569421b88ba31c15b537ba9f509a2bd11b7ab7711edd988ad6c767c53cb2`. It must be regenerated after the legacy-test correction; it is not the final published-input fingerprint.

Preserved log hashes:

- Exact baseline CTest: `31c185e1eaa962cc9ccff9a8d5498378d1c65caa7253868ee340060ddf3a8c6a`
- Candidate new CLI suite: `030560b213fe50a9dad4809c66e50524ad7712c5d05f722fb270e1756398be1c`
- Initial full Release failure log: `5b6806667b7b18f303f33b0ec03cd3e9f6fda2524f3b07fe73df316be92ea5ac`
- Initial full UBSan failure log: `57273ad0c197ef35649413ac1e56dbe864e5573a847933753c1bb86ea441435a`

An initial C++ test compile error comparing a JSON value directly to a string was corrected without weakening the assertion, and its failed build log was retained. An early red-test helper-loading failure was also retained separately and was not counted as a product regression.

## One known old-test expectation to correct

`ReworkCli.test_amendment_requires_explicit_new_disposition` currently expects zero calls after AMEND when supplied an old `change_task` script. The new captured-AMEND entry point correctly makes one Dispatcher attempt declaring `submit_amendment_plan`, then rejects `change_task` as `invalid_input`. This must preserve task readiness gates, original submissions and files, pending AMEND notices and all usage charges.

A single patch was submitted to rename the case to `test_amendment_captures_new_disposition_and_rejects_old_correction_tool` and strengthen the assertions. Its tool call did not return, so first read the actual file and determine whether it applied. Do not blindly submit a duplicate patch.

The intended replacement captures original submission fields (`submission_id,original_binding,body,artifact_snapshot,record_sha256`), workspace bytes, task semantic fields, plans, exact pending AMEND notices, budget spent tokens and attempt count. It checks:

- Exactly one Dispatcher call with `workflow_action.kind == amendment` and `target.plan_required == true`.
- `call.error.code == invalid_input`, settled original response tool name `change_task`, and the original 100 input / 20 output tokens.
- Exactly one additional attempt and budget increases of exactly 100 / 20.
- Identical task semantic fields, original plans, original submission fields and workspace bytes; submission status remains SUPERSEDED.
- No AMEND dispositions, rework count remains zero, and the same AMEND inbox IDs remain pending.

The task semantic comparison intentionally excludes state versions/epochs that may legitimately change during request-scoped takeover. Baseline source inspection confirmed the event type is `amend` and the SQL/result field names used in these assertions.

## Exact continuation

1. Confirm the local execution service responds. Read `git status`, the pending legacy test and the preserved logs before changing anything.
2. Finish only the intended legacy-test correction, inspect its diff and run that test against the existing Release binary. No C++ change should be needed for this expectation correction.
3. Preserve the initial failing Release/UBSan logs before rerunning gates. The attempted log-preservation command did not return; inspect whether its destination files exist rather than assuming success.
4. Synchronize the corrected Python file into `/tmp/wls861-validation-r23/source`, regenerate the 231-file execution manifest, and run the final ordinary-user gates using the already successful builds.
5. Write the final validation report and machine-readable evidence from actual results. Keep the initial failures and the red baseline visible.
6. Commit the implementation/tests and then the final documentation/evidence in small coherent commits. Publish through the connected GitHub tools if CLI push credentials remain unavailable. For every imported tree, compare its SHA with the local committed tree before moving the remote ref.
7. Use `b4977fa7fbbd1ba1f5b913447000f8810c2744bb` and this checkpoint's descendant as the published design lineage. Fetch the current branch head before updating it; do not overwrite an unseen head. Local and GitHub commit metadata may differ until the published commit is fetched and checked out.
8. Verify all six GitHub CI jobs on the published implementation's exact SHA, then fast-forward main only after checking its current head. Reconcile local HEAD/tree with the published revision and record the final SHA.

Existing verification locations:

- `/tmp/wls861-validation-r22/source` and `/tmp/wls861-validation-r22/logs`
- `/tmp/wls861-validation-r23/source`
- `/tmp/wls861-validation-r23/build-release`, `build-ubsan`, `build-asan`, `build-make`
- `/tmp/wls861-validation-r23/logs`
- `/tmp/wls861-validation-venv/bin/python`
- `/tmp/wls861-run-gate.py`

Example final gate (run as the ordinary user, with the same established isolation):

```sh
runuser -u nobody -- /tmp/wls861-validation-venv/bin/python /tmp/wls861-run-gate.py /tmp/wls861-validation-r23 asan-0002 0002 ctest --test-dir /tmp/wls861-validation-r23/build-asan --output-on-failure -V -j2
```

Use at most two concurrent test gates with `-j2` each. Final candidate builds used GCC 13.3 Ubuntu package `13.3.0-6ubuntu2~24.04.1`, Boost 1.83, SQLite 3.45.1, OpenSSL 3.0.13, CMake 3.28.3, Python 3.12.14 and pinned jsonschema 4.10.3. The baseline compile overlapped an incidental compiler-package update; that limitation is documented in the local baseline report and must not be silently erased.

## Remote evidence at the checkpoint

[Design/ancestor CI run 40277764908](https://github.com/wmqfl861/wls861/actions/runs/40277764908) is tied to `b4977fa7fbbd1ba1f5b913447000f8810c2744bb`, whose execution inputs are the inherited R2.2 baseline. At 06:13 UTC, strict Release, strict umask-0002 and Clang-18 were successful; UBSan, ASan and Make-ASan were still running. This is not evidence for the unpublished R2.3 code.

[Committed design source snapshot run 40277764901](https://github.com/wmqfl861/wls861/actions/runs/40277764901) succeeded. No final R2.3 code snapshot has been published.

## Remaining boundaries

The review used independent agents with the same model, not an independent second-model review. No paid/real provider calls or production operations were made. This increment does not establish real-model decision quality, a complete S1/S2–S5 implementation, positive deployment sandbox acceptance, private-data purge, or concurrent-writer leases. The next planned implementation after this increment's final acceptance is R2a: `fs_mkdir` and PREPARED owned-workspace allocation recovery/explicit abandonment, then the single-protocol offline replay prerequisite for R3.
