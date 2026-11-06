# Windows runner validation — provenance correction and evidence disclosure correction (v7)

- generated_at_utc: 2026-11-06T00:54:06+00:00 (tool clock at write time; not independently verified)
- report_version: 7 (plan version is independent: this batch follows plan-final-v8)
- scope: a follow-up public correction to the v6 report and implementation-06, covering both
  unrecoverable historical evidence gaps and this batch's limited API-provenance source fix;
  not a native Windows run, not a product validation, not a fix of the configure failure
  itself, and not an identification of the unknown survivor process
- v6 files are preserved byte-for-byte (`docs/reports/audit-windows-runner-validation-20261105-v6.md`
  sha256 `03e76c835824813efd5f1bc95e25d4336f8ec5b435bdd86822e218a1b288b92a`, and the matching
  `.json` sha256 `43a30944271480ae71a9271ed3e1d42e48fb6144b606763cc14dfc9dd6f80f7e`); they
  record their generation-time state, and where a statement below conflicts with them, this
  report is the correction.

## 1. Corrections to v6 and the implementation-06 response

The same author disposition after hard-review-06 (`NARROW_CORRECTION_REQUIRED_BEFORE_PUBLICATION`)
established four facts; they are corrected here publicly, without editing any original bytes:

1. **Command receipt count.** implementation-06 produced **9** command receipts under
   `validation-configure-diagnostics-01/commands/`; the implementation-06 response text
   claimed 8. The originals (including all stdout/stderr files) are retained unchanged.
2. **Missing per-command before state.** All 9 historical receipts recorded
   `head_before: null` and `porcelain_before: null`; only a session-level before manifest
   and per-command after bytes exist. Identical first, last and per-command after snapshots
   cannot prove the missing per-command befores or exclude changes between observations, and
   they are not backfilled. The v6 §3 wording "raw unstripped porcelain bytes around every
   command" was stronger than the evidence and is corrected by this statement.
3. **Interpreter identity.** All 9 historical receipts recorded the interpreter as
   `path: "python3"`, `version: "Python 3.12.3"`, `sha256: null`, with the version probe
   executed by the helper after the command; `watched_sources_at_run` hashes were likewise
   collected after each command, not as an independent pre-execution capture. The
   implementation-06 response claim of a `/usr/bin/python3` identity with binary SHA was
   not true. The historical resolved executable and binary hash remain UNKNOWN; today's
   measurements are not substituted for them.
4. **api_source mislabel (source defect, now fixed rather than only disclosed).** The
   retained synthetic configure receipt from the implementation-06 GREEN fixtures carried
   `api_source: "native-win32"` at both the top level and inside `configure_job_diagnostics`
   while the actually used underlying was the injected `_FlowFakeAPI` (`is_injected = true`).
   Cause: the runner's diagnostic proxy froze `is_injected` from the constructor argument
   (`underlying=None` on the runner wiring) at construction time, before the lazy factory
   created the real underlying. The same defect could mark any fresh synthetic run as
   native; disclosure alone was therefore insufficient.

## 2. What this batch changes (targeted source correction)

`platform/native-foundation/tools/run_windows_ci.py` (sha256 after this batch
`8c3b71f2f4317d2690355b31276bd29e020b1749c1a0ecf41d960ce4a1c39b3f`; the frozen baseline
remains `24bf18ddf2a08ee81ec3bd549a0a569550227f1a08833ed9afccad73703408e7`):

- the constructor-time frozen `is_injected` assignment is removed; a read-only
  `is_injected` property resolves the actual underlying through the existing
  `_require_underlying()` lazy cache and returns `bool(getattr(underlying, "is_injected",
  False))`, preserving the original missing-attribute default convention;
- `run_owned` snapshots `api.is_injected` before `create_job`, so the first source read now
  materializes the actual underlying and reports it truthfully: explicitly injected and
  lazy-factory injected paths read `injected`; the real `Win32ProcessAPI` keeps its class
  convention `is_injected = False` (native-win32);
- import and proxy construction still materialize nothing and load no Win32 library; the
  underlying is constructed at most once successfully and every later read or operation
  reuses that same cached object;
- a bare source read is not an observer operation: it never marks the observer as run and
  never attaches a diagnostic field;
- **declared initialization-timing correction:** with the previous runner, an injected-proxy
  run materialized the underlying inside the `run_owned` try block at `create_job` (a
  construction failure there produced a LAUNCH_ERROR receipt). The source read now
  materializes it earlier — at the supervisor snapshot before the try block — which matches
  the original unproxied `run_owned`, which also constructs the real API before any
  supervision state exists. An underlying-construction failure therefore keeps its original
  exception type and instance, escapes `run_owned` with **no** `process_receipt` and no
  fabricated diagnostic. This timing change is intentional and is stated here exactly;
- every other diagnostic semantic is unchanged: one shared clock object, the 200 ms
  cooperative budget inside the unchanged 5 s cleanup window, the 64/8/4096 limits,
  same-Job/same-handle re-verification, the single original terminate forward with original
  exceptions, the optional-field contract (missing `configure_job_diagnostics` reads
  NOT_COLLECTED; NOT_REQUESTED only when the observer actually ran un-triggered),
  non-configure entries unwired, and all deadlines (180 s configure / 1 s settle / 5 s
  cleanup) untouched.

`platform/native-foundation/tools/test_windows_ci_evidence.py` (sha256 after this batch
`37ebc029f006c278bd1b0e6cfab13230eed05e4ea90e153328da09857e767cf9`) changes only the
existing three `ConfigureJobDiagnosticsChecks` methods (still 34 runner methods in total;
no new top-level methods):

- the configure-flow check now asserts the top-level and diagnostic `api_source` pair reads
  `injected` with a self-identifying
  `provenance mislabel: top-level api_source=…; configure_job_diagnostics.api_source=…`
  message carrying both actual values;
- the binding/exception check now proves: explicit injection reads True; proxy construction
  neither materializes the underlying nor loads any Win32 library (factory and WinDLL
  spies); the first source read resolves exactly one cached injected underlying reused by
  later reads; a real `run_owned` drive through the lazy factory labels the receipt
  `injected` at the supervisor snapshot with exactly one factory construction; a bare
  source read does not run the observer or attach fields; the real API class convention
  (`is_injected` False) and a clearly labeled `TEST_DOUBLE` false-marker underlying are
  checked as synthetic property-contract facts only, never published as native evidence;
  an underlying-construction failure propagates as the identical pre-created exception
  instance with no `process_receipt` and no fabricated diagnostic; and the original real
  API still refuses unsupported platforms before any WinDLL load (platform-conditional,
  as in the supervisor selfcheck). The previous assertion that an unmaterialized
  production-shaped proxy reads `is_injected` False — the frozen-constructor behavior this
  batch removes — was deleted and replaced by the construction-does-not-materialize check.

## 3. Targeted validation this batch (new private evidence root)

Private evidence root `validation-configure-provenance-01` (data disk; 0700/0600;
`--no-optional-locks` git reads). The new capture helper is frozen before first use and
bound by path and hash inside every command receipt; each receipt captures, **before the
command starts**, the resolved absolute interpreter path, its binary sha256 and version
probe, the full raw HEAD and unstripped porcelain bytes, and the watched source hashes,
then captures the same identities again after the command exits. A failed before capture
aborts the command. Exactly **2** validation commands were run:

- **RED** (baseline runner `24bf18dd…` unchanged, frozen oracle identical to GREEN):
  `ConfigureJobDiagnosticsChecks.test_configure_cleanup_member_observation` — ran and
  failed (exit 1) with exactly one failure, classified **BEHAVIORAL**: the real runner
  configure branch and real `run_owned`/`_closeout` fully reproduced the original synthetic
  failure (root exit 0, settle EXPIRED, TerminateJobObject forwarded once and succeeded,
  cleanup PROVEN_ZERO, outcome FAILED_DESCENDANTS_AFTER_ROOT_EXIT, membership SUCCESS
  [7001, 7002, 7003]) and the assertion failed with
  `provenance mislabel: top-level api_source=native-win32; configure_job_diagnostics.api_source=native-win32`.
- **GREEN** (only the runner changed; whole test file byte-identical to RED — identity diff
  0 bytes): the four selected existing methods all passed (exit 0):
  `test_configure_cleanup_member_observation`,
  `test_configure_diagnostic_partial_and_bounds`,
  `test_configure_diagnostic_binding_and_exception_fidelity`, and
  `WindowsEvidence.test_owned_supervision_boundary_covers_all_command_entry_classes`.
  The green configure receipt now reads `api_source: injected` at both locations with the
  original failure semantics preserved verbatim.

Explicit unrun complement: **4 of the 34 runner methods were rerun this batch; 30 runner
methods were not rerun.** The supervisor (20), image (17) and bundle (6) suites were **not**
rerun this batch; the implementation-06 totals 20/34/17/6 remain historical results of
those sources. Coverage roles: the binding method's WinDLL/factory spies are the direct
laziness/no-load evidence; the entry method is entry-level regression protection only (it
stubs `run_owned`, so on Linux it would catch constructor-time materialization via the
platform refusal, but it is not independent direct WinDLL evidence). The local
native-marker property checks remain synthetic contract validation, never native evidence.

This batch's own deviation, preserved in the evidence root: the first frozen before-snapshot
helper (`snapshot-before-v1.py`) contained seven hand-transcribed protected hashes that were
mistyped; its own mismatch check refused to proceed and exited non-zero **before any
validation command ran**, an independent programmatic comparison against the FINAL record
confirmed all 26 protected files byte-identical on disk, and a second helper
(`snapshot-before-v2.py`) that reads the record programmatically was frozen under a new
number. The defective helper and its failed manifest are preserved unchanged; no validation
output was rewritten.

## 4. Observed native status carried forward (unchanged facts)

Two real windows-2022 runs of HEAD `2f22823a0794ceec7dd2f330fa64f3d6511b5c29` (push
42093312173, pull_request 42093324882, both attempt 1, same tree
`2a0909ea8d9c99d7c037e6a5a3b6a64dac7e5e44`): steps 4–8 succeeded, step 9 failed, step 10
retained artifacts; native supervisor selfcheck 11/11 PASS in both runs; `configure`
root-exit-0 with settle EXPIRED, TerminateJobObject success and same-Job ActiveProcesses
==0 after termination (cleanup PROVEN_ZERO, tree_exit_proven=true, outcome
FAILED_DESCENDANTS_AFTER_ROOT_EXIT); the survivor identity, origin and cause remain
UNKNOWN; product build/ctest/install/host-capabilities/detached stayed NOT_STARTED. The
corrected diagnostic has still not run on real Windows: it stays PENDING a future natural
CI run, where an un-triggered diagnostic reads NOT_REQUESTED and a missing field reads
NOT_COLLECTED. Nothing in this batch fixes configure or identifies the survivor; no /nr,
/p, server switch, generator/toolset change, global shutdown, extended deadline or
breakaway was added.

## 5. Preserved disclosures and limitations

- H6-1 (result.json aggregate is a pre-finalization projection), H6-2 (historical report
  timestamps nominal), H6-4 (malformed README hash excluded from identity evidence);
- D03-1..4 and D04-1..7 process facts; hard-review-04's per-receipt disclosure limitation;
  implementation-05's attempt-1 oracle defect, session sidecar and frozen private
  `__pycache__` findings;
- implementation-06's three numbered oracle/runner repair attempts, all preserved;
- the four implementation-06 evidence gaps corrected in §1 above are permanent: the
  historical receipts, outputs and the v6 pair are never rewritten or backfilled;
- this batch's helper-transcription deviation in §3.

## 6. Release states (kept separate)

- IMPLEMENTATION_COMPLETE: this batch's source changes only (local, uncommitted).
- LOCAL_VALIDATION_COMPLETE: the targeted set only — 1-method BEHAVIORAL RED plus the
  4-method GREEN under the identical whole test file.
- PUSH_COMPLETE: false (no Git writes in this task).
- REMOTE_CI_COMPLETE: false — the corrected-provenance candidate has not run on real
  Windows; future natural CI is PENDING.
- PR_REVIEW_COMPLETE: false (no formal review or acceptance).
- MERGED: false.

Standing states: Windows 11 ordinary-user NTFS NOT_RUN; historical image conflict
UNRESOLVED; private original hosting BLOCKED; full native runtime NOT_IMPLEMENTED;
historical Clang/live_workflow findings remain open with the coordinator. This report is
not a PR acceptance or release approval. Engineering model request counts live in the stage
entry receipts (implementation-06: 62; hard-review-06: 4; this batch: its own stage
receipt; unknown values stay unknown). No private local absolute paths or credentials
appear in this report; events after its generation are recorded by stage receipts or a new
external addendum, never by editing this frozen pair.
