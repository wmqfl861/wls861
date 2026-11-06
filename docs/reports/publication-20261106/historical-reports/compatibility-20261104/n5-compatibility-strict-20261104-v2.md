> **HISTORICAL PROJECTION (REDACTED) — NOT CURRENT ACCEPTANCE.**
> Source: `docs/reports/n5-compatibility-strict-20261104-v2.md` as it existed uncommitted in its
> worktree at head `efcd4547bd7627fa821a4b7bce097e633b514982` (branch `test/n5-compatibility-20261104-_7wxnkyn`); original 12309 bytes,
> SHA-256 `2bb7758c7a6c9674ce37147569097aaf0f2faf17ab57bc7209b233c0ea7055a3`. Redacted public projection created 2026-11-06T10:21:09Z under
> `docs/reports/publication-20261106/`. This is a new version, not a byte copy
> of the original; the original is retained locally and is not in Git.
> Redaction scope: absolute local workspace/evidence-root paths, local task plan / version-record / plan-review references, internal session identifiers.
> No engineering result, status, failure, timestamp or six-state value was
> changed; PUSH/CI states remain the historical values of the report's date.
> **Historical scope note:** this v2 report is itself the correction of v1 and
supersedes only the v1 claims it lists. The later 2026-11-05 audit-fix batch
(e.g. PR #37 compatibility evidence-tool fixes) is a separate, later scope
and neither inherits nor alters these 2026-11-04 conclusions.

# N5 strict compatibility corrections, 2026-11-04 (v2)

**Headline: C4 controlled-kill capability is NOT_ESTABLISHED and C4 is no longer
executed (NOT_RUN by design); strict compatibility remains
`STRICT_COMPATIBILITY=BLOCKED/NOT_COMPLETE`; the v1 report's aggregate assertions
are corrected below.** This v2 projection corrects the frozen task
`strict-compatibility-observer-and-lifecycle`; the v1 md/json stay in place,
unmodified, as the historical source of the original execution. Private full
correction evidence (derived adjudication, mock harness, red/green/data records)
is local-only under
`[task evidence root, redacted]`
(data disk, ext4, `rw,relatime`); this report is a projection, not the
originals. The machine projection of this report is
`docs/reports/n5-compatibility-strict-20261104-v2.json`.

## What went wrong in v1 and what is corrected

The historical C4 attempt attributed a stop/confirm/kill/reap chain to the old
410 ELF. In fact every signal and observation in that chain acted on the
**strace tracer**: `records/c4-r4/row.json` (`OLD_OPEN`) shows argv[0]=`strace`,
pid 830993, and the old-ELF tracee's PID, start ticks, exit and wait are nowhere
in the row — they are UNKNOWN. C4's controlled-kill coverage is therefore
`NOT_ESTABLISHED` (qualified controlled abnormal-exit rows: **0**), and the
corrected runner now refuses C4 up front (`row_c4` returns a structured
`NOT_RUN` before any fixture, holder, product, signal or observer effect and
reads no receipt; `_trace_old` rejects `kill=True` before any effect). The
historical row and trace bytes are untouched. This correction executed **no
product matrix row, no compilation, no old-ELF invocation, no strace/ptrace
experiment and no real SQLite fixture**; its verification is a pure in-memory
mock harness plus data-level checks, which are **not** product registration,
**not** a product regression and **not** C4 coverage.

### Corrected claims, each with its source and basis

| # | Corrected sentence | Source | Correction and limited facts |
| --- | --- | --- | --- |
| 1 | "SIGSTOP 34 µs after the traced `-shm` open" | v1 md, C4 row | The 34,419 ns value (1766747342156736−1766747342122317) is the host's gap between receiving trace text containing the SHM path and sending SIGSTOP **to the tracer**; not a tracee syscall-stop position, not a ptrace rendezvous proof. |
| 2 | "stopped state confirmed, SIGKILL, exit −9, reaped" | v1 md, C4 row | The stopped/confirmed/killed/reaped process was the strace tracer (pid 830993, exit −9). Tracee identity/exit/reap: UNKNOWN / NOT_ESTABLISHED. |
| 3 | "WAL/SHM **retained** (killed before cleanup unlink)" | v1 md, C4 row | Sidecar retention is equally explained by the online holder keeping its connection open for the whole row (HOLDER_CLOSE only after the AFTER window); kill-before-cleanup-unlink is NOT_ESTABLISHED. |
| 4 | "process kill never classified as unsupported" | v1 md, C4 row | The row itself records the complete Unsupported JSON (`stderr_sha256 acc8151d…`, `error_classification=unsupported`); the sentence contradicts the row. The capture proves only that the refusal text was seen — nothing about the tracee's outcome. |
| 5 | "preserved **all business rows and all main/WAL/SHM content bytes**" | v1 md, aggregate | True only per-row on pre-existing members (C2a/C2b/C3a/C3b/C4). C1a **added** a new empty WAL (0 bytes) and a new 32,768-byte SHM — the member set changed and the new members are `content_equal=false`; only the original main content is equal. C5's WAL/SHM content changed (authorized holder commit; WAL 20632→24752 bytes, SHM sha changed) with `raw_equality_claimed=false`. |
| 6 | "the frozen old ELF always leaves metadata deltas" | v1 md, aggregate | Deltas were observed on this machine (ext4, `rw,relatime`), these fixture initial states and these sampled windows; C5's main had **no** metadata delta; sampled boundaries do not exclude unsampled instants. "Always" is not established. |
| 7 | "unavoidable metadata deltas" | v1 json, `overall_rationale` | Same correction as #6: observed-sample-scoped facts, not a general law. |
| 8 | "relatime 下必改 atime、fchmod 必改 ctime" | **original hard-review wording** (session `[redacted]`), *not* a v1 sentence | Labeled as old-hard-review wording and not adopted: same-mode `fchmod` ctime changes were observed on this machine/window (C1b/C3b/C4 mains and writer locks) and atime deltas under `rw,relatime`; "必改" (must-change) is not established beyond those observations. Neither v1 file ever contained this exact wording. |
| 9 | "the raw traces bound the C4/C5 windows" | v1 md, limitations | C5 has **no raw trace**; its window is bounded by row timestamps only. C4's trace is tracer-side output that ends at the SHM open (line 695, FD 6) and bounds nothing about the tracee. |

## Identity chain (execution, pre-correction delivery, corrected delivery)

| Identity | Value |
| --- | --- |
| Historical executed runner (manifest-r4) | SHA-256 `e83beb5593b83d47956da4b6623253343474de61573589b45b1c1eaa1f29a1e7` |
| Pre-correction delivered runner (this correction's baseline) | `fa090d81eb5843e132dfa2c106c122541c194c7a932d6a1eb0e9878a1add2de7` |
| Corrected runner (this delivery) | `3ca64e26d8816961e77907e6585ac4b90f31c351151f276ab68ba6c01ac330fc` |
| Observer / selfcheck (unchanged) | `50d8efa0c595c63c759f45318dbfd668537b5edc2e31f823c09b5e74bb0d47f6` / `38fa9422d164a010ce21e3c40e287db6fff800d5530fe1861404ca01563f9999` |
| Baseline | HEAD `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123`, branch `test/n5-compatibility-20261104-_7wxnkyn`, no tracked file modified |

The historical matrix was executed by `e83beb55…`; the first delivery was
`fa090d81…` (C6 parser fix after capture); this correction delivers
`3ca64e26…`. The old matrix must not be represented as a run of any corrected
runner.

## Runner correction scope (minimal, plan-bounded)

Only the module docstring and four functions changed; every other handler,
constant, `ROW_HANDLERS`, `main` and the C6 parsing code are byte-identical to
the pre-correction runner (verified per top-level block against the archived
baseline):

- `row_c4` — unconditional structured `NOT_RUN` (tracee identity UNKNOWN;
  crash/exit and reap NOT_ESTABLISHED; no PID/exit/reap facts fabricated; no
  receipt read).
- `_trace_old` — kill mode rejected before any Popen/signal/fixture/trace-file
  effect; the helper's pid/start_ticks/exit_code are now explicitly labeled as
  describing the **tracer** (`child_role` field); normal uninterrupted tracing
  is preserved for C4receipt; the timeout kill is tracer-only cleanup.
- `row_c4receipt` — name, history and PASS logic preserved; an explicit
  `scope` field states it is a normal-comparison receipt only, verifies no kill
  rendezvous, identifies no tracee, must never gate controlled-kill execution,
  and that normal equivalence does not imply non-perturbation under
  interruption.
- `row_c5` — execution logic untouched; the note and a new `coverage` field
  state the supported fact (process-lifetime overlap with one authorized
  commit during a confirmed SIGSTOP of the direct old-ELF child, no tracer) and
  the NOT_ESTABLISHED items (DB-access stage at stop; simultaneous SQLite
  access; any raw trace). Historical status `RECORDED_OK` is not rewritten.

## Verification of this correction (mock-only, local)

Deterministic harness at `evidence/n5-compatibility-corrections-20261104-v1/harness/correction_harness.py`
(in-memory forged C4receipt — never written to disk — path guards excluding the
original v1 evidence tree; all subprocess/signal/socket/SQLite/open effects
intercepted by tripwires that raise before any effect; archived sources hashed,
never executed):

- **Red (12/12 checks, on-site unmodified runner `fa090d81…`)**: with a forged
  `status=PASS` receipt the old `row_c4` read the receipt and proceeded into
  fixture construction (intercepted: `Runner.build_static_411`); the old
  `_trace_old(kill=True)` immediately attempted `Popen(['strace', …, 'init',
  …])` (intercepted). No real effect occurred.
- **Green (23/23 checks, corrected runner `3ca64e26…`)**: `row_c4` returns the
  structured `NOT_RUN` with zero intercepted effects and without reading any
  receipt; `kill=True` raises the explicit rejection before any effect; only
  the module docstring and the four allowed functions differ from the archived
  baseline; `row_c5` is equal after note-sentinel + coverage-removal
  normalization; no caller passes `kill=True`; C6/other handlers byte-identical.
- Failed harness iterations (a harness TypeError, an inverted assertion, an
  invalid red attempt against the already-corrected runner, and first-pass
  green check definitions) are preserved under `records/*-failed*`; harness
  changes after the red run touched green-mode checks only.
- **Data-level checks**: original row/trace/manifest hashes recomputed and
  preserved; the derived adjudication (`adjudication-v1.json`, private)
  distinguishes `original_observation` from `derived_adjudication` and matches
  the actual row values (C4 −9/tracer, C5 direct pid 831032 with overlap vs
  NOT_ESTABLISHED simultaneous access, C1a added members, C3a main/WAL/SHM raw
  equal with only lock ctime, C5 WAL/SHM `content_equal=false`); v1 files
  unchanged; md/json projections consistent with the runner identity chain,
  denominators and the six registration states.

Denominators are not erased: the historical matrix made **12 attempts**
(C0, C1a, C1b, C2a, C2b, C3a, C3b, C4receipt, **C4**, C5, C7a, C7b — C1a/b
counted separately, C4receipt and the C4 attempt both included) plus one
nine-method suite run (9 methods); observer selfcheck 22 cases (17/22 then
22/22, both retained). This correction added **0 product runs, 0 compilations,
0 old-ELF invocations, 0 CTest registrations** (catalog stays 95 with unchanged
deadlines) and **35 mock tool checks** (red 12 + green 23), which are not
product evidence.

## Registration, end states and what remains open

Six states with exact scope, as of 2026-11-04T21:13:52Z:

- `IMPLEMENTATION_COMPLETE=true` — scope: this minimal correction only (runner
  disablement/notes, v2 md/json, private mock verification, private
  adjudication).
- `LOCAL_VALIDATION_COMPLETE=true` — scope: mock red/green on frozen source
  identities, per-function source preservation, syntax parse, original-hash
  preservation, data-level consistency; **no product regression executed or
  claimed**.
- Same object, immediately adjacent: `original_matrix_coverage_complete=false`,
  `c4_product_crash_coverage=NOT_ESTABLISHED`,
  `overall_strict_compatibility=BLOCKED_NOT_COMPLETE`. This correction does not
  turn the matrix green and does not close Compatibility02/03.
- `PUSH_COMPLETE=false`, `REMOTE_CI_COMPLETE=false`, `PR_REVIEW_COMPLETE=false`,
  `MERGED=false` — no git write, commit, push or PR was performed by this task.

The original independent hard review PASS (session
`[session id redacted]`, stdout
`0cc4da27…`) and its independence are preserved; the plan author's disposition
retains it as PASS while not adopting its C4 controlled-kill attribution and
aggregate content/metadata conclusions (items 5–8 above). The correction plan
review PASS (session `[session id redacted]`) is likewise
recorded. **A new independent hard review of this correction has not yet
occurred as of this report's timestamp and is pending; no result is pre-filled
here** — the later actual verdict belongs to a new receipt, not to this file.
Publication remains the root agent's step after that review, under the existing
task authorization.

End states unchanged: Windows 11 ordinary-user NTFS `NOT_RUN`; restricted
private hosting `BLOCKED` (originals retained locally);
`historical_clang_asan_failures_closed=false`; N5-2 items 014/412 remain
unauthorized. The missing controlled-tracee stop mechanism and stronger
simultaneous-SQLite-access evidence remain explicit limitations; no new
compatibility mechanism, VFS, WAL parser, migration or second production
writer was introduced, and no follow-on implementation planning is opened by
this correction.
