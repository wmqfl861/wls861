# Windows runner owned-process supervision fix — validation report (v1)

Versioned report, 2026-11-05, version 1. It documents the E002 audit fix for
`tools/run_windows_ci.py` and its new supervision tooling, the validation
actually executed this round, and the states that remain open. The original
finding stays `STATIC_CONFIRMED`: the pre-fix runner started every external
command with `subprocess.run(..., PIPE, timeout=...)` (lines 175–183 of the
original file, SHA-256 `703041ed30529c72d908041da9bc267ecb0e691dff44cced8139c15889528571`),
its `finalize_identities` ran identity snapshots without any owned-subtree
exit proof (original lines 217–254), and three unsupervised subprocess entry
classes existed (build commands at original line 176, `git ls-files` at
original line 193 with **no** timeout, capture-only `git rev-parse` at
original line 414 with 30 s). No original error state was a fabricated PASS,
and the original Windows dynamic counterexample remains `NOT_RUN`; the defect
baseline for this fix is recorded as `STATIC_CONTROL_FLOW_EVIDENCE`, not a
reproduced failure.

## What changed (files and design)

- `platform/native-foundation/tools/windows_process.py` (new): minimal native
  Windows owned-command lifecycle supervision. One unnamed, non-inheritable
  Job per command with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`; creation order
  create-Job → set-limit → stdio handles → `CreateProcessW(CREATE_SUSPENDED |
  CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT)` →
  `AssignProcessToJobObject` → `ResumeThread`, so no target code runs before
  the owned boundary exists. No breakaway configuration; the supervisor never
  joins a Job it terminates. argv is a shell-free list with a resolved
  absolute `lpApplicationName` and `subprocess.list2cmdline` quoting; stdin is
  a per-call NUL device; stdout/stderr are private binary spool files
  inherited through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (exactly three
  handles; no PIPE reader threads that a descendant could block). All Win32
  entry points declare `argtypes`/`restype` with pointer-width HANDLE/SIZE_T
  values; NULL and INVALID_HANDLE_VALUE are checked separately and
  `GetLastError` is captured immediately. Runtime completion is proven only
  by root HANDLE signaled AND actual exit code read AND a successful same-Job
  `ActiveProcesses == 0` query; `TerminateJobObject` success, close-Job, a
  root-only wait or an absent PID are not proof, and `STILL_ACTIVE` (259) is
  never used as a wait state. Bounded closeout (default 5 s) defers a second
  Ctrl-C/SIGBREAK, restores original handlers, closes every owned handle and
  preserves the original `TimeoutExpired`/`KeyboardInterrupt`/`SystemExit`
  exception with a full receipt attached; unproven observations stay
  `UNPROVEN`/`NOT_COMPLETE` with their real errors, never 0/true/PASSED. The
  Job is a supervision custody design under trusted tooling, not a sandbox;
  no runtime claim is made about handles held by external processes. The real
  binding loads only on native Windows; without it `run_owned` refuses
  (tested) — there is no unsupervised fallback, and tests may inject only a
  narrow API/clock interface.
- `tools/run_windows_ci.py`: all three external-command entry classes now go
  through `run_owned` — build commands (git-head/git-status keep the original
  120 s default; configure 180 / build 600 / ctest 300 s and native suite
  deadlines unchanged), the source-snapshot `git ls-files` query with a **new
  explicit 30 s deadline** (a tightening; the original call had none), and the
  capture-only `git rev-parse` query (30 s, private spool outside the
  checkout, originals retained in a fresh error directory on failure with the
  location reported). Each command writes an additional `<name>.process.json`
  receipt (argv/cwd, UTC and monotonic timings, root PID + creation time as
  diagnostics only, establishment phases, exit-code availability, termination
  reason/request, Job-zero observation, root wait result, settle window
  status/timings, cleanup budget/errors, raw log bytes/SHA-256, output
  completeness, `tree_exit_proven` and the proof rule). `configure`/`build`
  set `MSBUILDDISABLENODEREUSE=1` in the subcommand environment only, and the
  receipts record that task-level override. The aggregate over all started
  commands distinguishes `NOT_STARTED`; `result.json` additionally records
  `all_owned_process_trees_exited`; when any started tree is unproven,
  after-snapshots and `result.json` are withheld, retained originals are
  marked `UNSTABLE_PARTIAL`, and `failure.json` keeps its
  TIMEOUT/INTERRUPTED/FAILED distinction plus cleanup/identity availability.
  An interruption during the detached step restores the moved source only
  after a proven cleanup; otherwise the pending-restore path is recorded.
- `tools/test_windows_process_supervisor.py` (new): the supervisor selfcheck.
  `--mode synthetic` (fake Win32 API + fake clock) covers stage failures
  (Job/limit/process/assign/resume), assign failure with no target side
  effects and direct owned-handle reclamation, root exit 0 with live
  descendants (bounded 1 s settle, then termination, proof, command failure),
  non-zero root with descendants, terminate-success-but-query-failure,
  never-zero budget exhaustion, wait failure, a deferred second interrupt
  during cleanup, capture failure not masking the original timeout, platform
  refusal and the job-before-process/assign-before-resume ordering contract.
  `--mode native` runs ten real owned-process scenarios on Windows (N1
  parent/child zero, N2 root-exit-0 with a surviving descendant, N3 real
  timeout, N4 descendants holding and writing stdout/stderr, N5 non-zero root
  with descendants, N6 real Ctrl-C in an isolated `CREATE_NEW_CONSOLE`, N7
  assign-failure injection on a suspended process, N8 an independent sentinel
  in its own Job, N9/N10 full `run_native` timeout/interrupt integrations with
  discovery/build substituted by real supervised commands and the short
  fixture-only ctest timeout recorded as an injection), each within a 25 s
  budget and 280 s internal total; on non-Windows it records NOT_RUN and
  exits 3 — CI treats that as a failure, never a skip.
- `tools/test_windows_ci_evidence.py`: the supervision boundary is injectable
  for synthetic checks; the original 27 test meanings are preserved and four
  regressions added (boundary coverage of all planned commands with the three
  deadline classes, unproven-tree gating of `result.json`/after snapshots,
  `all_owned_process_trees_exited` in `result.json` with the proof rule, and
  the MSBuild task-level override recorded only on configure/build).
- `.github/workflows/windows-native.yml`: the two new selfchecks run after the
  image capture and before the product build (synthetic step 1 minute, native
  step 5 minutes); the runner selfcheck retains its fixtures outside the
  checkout; the `always()` upload adds `native-process-synthetic/`,
  `native-process-native/` and `native-runner-selfcheck/`; the original job
  15-minute limit, pinned actions, guards and every product deadline are
  unchanged, and each touched step exits with the actual failing
  `$LASTEXITCODE`.
- `platform/native-foundation/README.md`: documents the supervision design and
  selfcheck usage and corrects the current drift (36 registered public assets,
  166684 bytes; Linux product catalog 95 registered suites with the Store 411,
  persistent-event runtime and Store-open compatibility additions and the
  120 s compatibility deadline).

## Validation actually executed this round (Linux, synthetic only)

All commands ran as ordinary user on the data disk with a private round
evidence directory, fresh per command, with full stdout/stderr/exit receipts
retained outside the repository:

| Check | Result |
| --- | --- |
| Supervisor synthetic selfcheck (`--mode synthetic`) | PASS — 13/13 cases, exit 0 |
| Runner synthetic selfcheck (`test_windows_ci_evidence.py`) | PASS — 31 tests (27 original meanings + 4 new), exit 0 |
| Image provenance source-binding selfcheck | PASS — 17 tests, exit 0 |
| Bundle capture selfcheck (`--source-root .`) | PASS — 6 tests, exit 0 |

Tool identities (SHA-256 at run time) and the interpreter (system Python
3.12.3, stdlib only) are recorded in the private round evidence together with
the command receipts; this public report carries no private paths or raw logs.

## Not run this round (open states)

| Item | State |
| --- | --- |
| Windows hosted supervisor native scenarios N1–N10 | NOT_RUN — awaiting this branch's hosted CI (step wired, 5-minute budget) |
| Windows hosted runner integration through actual `run_native` | NOT_RUN — covered by N9/N10 in CI |
| Existing native slice (5 suites / 34 methods) on the final SHA | NOT_RUN — awaiting CI on the published branch |
| Windows 11 x64 ordinary-user NTFS acceptance | NOT_RUN — no machine |
| Linux six-way matrix and the 95-suite catalog on the final SHA | NOT_RUN for this change — retained by the existing Linux CI |
| Original E002 Windows dynamic reproduction | NOT_RUN — original defect remains STATIC_CONFIRMED / STATIC_CONTROL_FLOW_EVIDENCE |
| Historical image conflict | UNRESOLVED (unchanged) |
| Restricted private original hosting | BLOCKED (unchanged) |

`windows_11_machine_acceptance=NOT_RUN`, `ordinary_user_security_acceptance=
NOT_RUN` and `full_native_runtime=NOT_IMPLEMENTED` are unchanged by the new
tool fields. The native supervisor's ABI layout checks (structure sizes,
pointer width, nested-Job allowance) execute only on Windows; Linux synthetic
results do not substitute for that evidence.

## States and boundaries

| State | Value |
| --- | --- |
| IMPLEMENTATION_COMPLETE | true — E002 tool-fix scope of this report only |
| LOCAL_VALIDATION_COMPLETE | true — limited to local synthetic/tool checks listed above |
| PUSH_COMPLETE | false at report freeze |
| REMOTE_CI_COMPLETE | false — Windows hosted and Linux results for the final SHA pending |
| PR_REVIEW_COMPLETE | false — coordinator owns acceptance |
| MERGED | false |

`prepublication_source_local_review=NOT_RUN`, `windows_native_validation=
NOT_RUN`, `post_ci_independent_evidence_review=NOT_RUN`. No product permission,
ACL/SID/host assertion, product ProcessRunner or sandbox was implemented; no
Linux runner was refactored; no real model or paid call occurred. The
E003 companion correction is reported separately in
`n5-windows-evidence-correction-20261105-v3.md`.

Report frozen at 2026-11-05T12:20:00+00:00 (tool clock at write time).
