# N5 strict compatibility report, 2026-11-04 (v3, self-contained public projection)

This pair — `docs/reports/n5-compatibility-strict-20261104-v3.md` and
`docs/reports/n5-compatibility-strict-20261104-v3.json` — is the single public
report of the frozen N5 strict-compatibility evidence task and is written to
stand alone: the statuses, identities, corrections and end states below do not
depend on any unpublished file. Full private evidence (fixtures, frozen copies,
raw traces, holder journals, correction-harness records and the derived
adjudication) is retained outside the repository on the project data disk; this
report is a projection, not the originals. Earlier draft projections of the
same results remain local-only, byte-unmodified, and are excluded from the
published set.

**Headline: `STRICT_COMPATIBILITY=BLOCKED_NOT_COMPLETE`; the C4 controlled-kill
capability is `NOT_ESTABLISHED` and C4 is no longer executed (`NOT_RUN` by
design); the aggregate assertions of the original results statement are
corrected below to row-level, observation-scoped facts.** This projection round
executed no product code, no compilation and no harness; it performed static
identity, AST/diff, semantic and privacy checks only.

## Scope and baseline

- Frozen task: strict Store-410/411 compatibility observation and lifecycle
  matrix — the frozen old 410 program against the current 411 candidate.
- Baseline HEAD `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123`, branch
  `test/n5-compatibility-20261104-_7wxnkyn`. No tracked repository file is
  modified by this delivery; the published set is exactly five files: the three
  evidence tools plus this report pair.
- The three tools are manually invoked evidence tools, deliberately not
  registered as CTest suites. The product catalog remains 95 suites with
  unchanged names and deadlines; the existing nine-method store-open
  compatibility suite keeps its 120-second suite deadline and 20-second CLI
  bound. Nothing in this delivery changes product behavior.
- Store 411 through the approved 013 migration remains approved and implemented,
  unchanged; N5-2 items 014/412 remain unauthorized. No new compatibility
  mechanism, VFS, WAL parser, migration or second production writer is
  introduced, and no follow-on implementation planning is opened here.

## Identity chain

| Identity | Value |
| --- | --- |
| Old 410 ELF (frozen program under test) | SHA-256 `b048a449ecdc22cd53ccf2813804bd36c720a26870732f03ebe9c066ed5d407e`, 4,733,672 bytes — fresh read-only rehash this round (match); never executed this round |
| Current candidate binary | cmake target `wls` at HEAD `3a872558…`, built out-of-tree; SHA-256 `fab236410c7e39cef14e5f18733aefe85b291a72cb5b30fc74726425d5bd9673`, 5,117,440 bytes — fresh read-only rehash this round (match); never executed this round |
| Runner that executed the historical matrix | SHA-256 `e83beb5593b83d47956da4b6623253343474de61573589b45b1c1eaa1f29a1e7` |
| Runner, first delivery (C6 verdict parsing fixed after capture) | SHA-256 `fa090d81eb5843e132dfa2c106c122541c194c7a932d6a1eb0e9878a1add2de7` |
| Runner, corrected delivery (C4/kill disabled, scoped notes; mock-verified) | SHA-256 `3ca64e26d8816961e77907e6585ac4b90f31c351151f276ab68ba6c01ac330fc` |
| Runner, this delivery (two docstring report references updated; all executable code byte-identical to the corrected delivery) | SHA-256 `fd8b70ea710919a6ca8afa3c7ebd04afe9fdb34df8748f7abd00b26d31b24895` |
| Observer | SHA-256 `50d8efa0c595c63c759f45318dbfd668537b5edc2e31f823c09b5e74bb0d47f6` (unchanged throughout) |
| Observer selfcheck | SHA-256 `38fa9422d164a010ce21e3c40e287db6fff800d5530fe1861404ca01563f9999` (unchanged throughout) |
| Environment | Python 3.12.3; SQLite runtime 3.45.1 (`libsqlite3.so.0.8.6`, SHA-256 `85265a9d4afca6f4b325ceb078b669c754fb881abed4cafe91ccebe9d625d975`, recorded identity comparison — not re-hashed this round); UID 1000, umask 0077; ext4 mounted `rw,relatime`, recorded per observation |

Identity methods: the two ELF entries above were re-verified this round by a
read-only SHA-256 and size check of the retained originals; the binaries were
not executed. The SQLite library identity is a recorded value, not a fresh
rehash. The historical matrix was executed by runner `e83beb55…` and must not
be represented as a run of any later runner; this delivery's runner differs
from the corrected runner `3ca64e26…` only in two docstring report references
(module docstring and the `row_c4` docstring), verified by a
docstring-stripped AST comparison plus a verbatim diff.

## The evidence tools

`tests/store_compatibility_observer.py` records full stat/statx metadata
(birth time, mount id), xattrs, nanosecond atime/mtime/ctime and complete
SHA-256 content for every database-prefix member (main, `-wal`, `-shm`,
`-journal`, `.writer-lock`, any unknown prefix member), using read-only
`O_NOATIME|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK` descriptors and an
lstat→fd→EOF→fd/path identity chain. Frozen copies are verified by an
independent readback (size+SHA, one mapping per member) before any SQLite
open; logical dumps cover every real table (76 tables in populated 410
fixtures, 82–83 in 411 fixtures) with explicit null/int/real-bits/text/blob
encodings, PRAGMA metadata and `quick_check`. Originals are never opened with
SQLite, never checkpointed and never read `immutable=1`.
`tests/store_compatibility_observer_selfcheck.py` is its selfcheck (positive
and negative cases). `scripts/run_store_compatibility_lifecycle.py` drives the
matrix rows: each row uses one fresh fixture directory under the private
evidence root, executes once, and failures are recorded, never retried to
green; a test-only holder subprocess (one SQLite connection,
`wal_autocheckpoint=0`, bounded pipe handshake, phase and handshake sequence
numbers, monotonic anchors) is the only fixture writer inside its window and is
an experiment participant, never a second production Store writer. The old ELF
is verified before and after every use and never modified; the runner refuses
to run as root. Each row proceeds through recorded phases (PREPARED →
WAL_READY → WRITER_QUIESCED → BEFORE → OLD_OPEN → AFTER → WRITER_RESUME →
POST_WRITE → HOLDER_CLOSE) with holder PID, start ticks, open fds, handshake
sequence numbers and monotonic anchors per phase.

## Row results: recorded status, limited conclusion, observed deltas

| Row | Input and operation | Recorded status | Limited conclusion and observed deltas |
| --- | --- | --- | --- |
| C0 | Online 411 fixture, holder quiesced, no product operation | **PASS** | Two-snapshot windows stable; originals byte- and metadata-identical before/after; frozen-copy readback equal; all-table logical dumps equal — the observer is non-perturbing. |
| C1a | Static populated 411; old410 `pending` | STRICT_FAIL | Correct `unsupported` refusal (exit 2); original main content equal, main atime changed by the old program's own read; member set changed — a new empty `-wal` (0 bytes) and a new 32,768-byte `-shm` were added; business rows identical. |
| C1b | Static populated 411; old410 `init` | STRICT_FAIL | Correct `unsupported`; no net sidecar difference across the sampled before/after windows (transient actions between samples are not directly observed by this row); main atime+ctime and writer-lock ctime changed — the same-mode `fchmod` ctime pattern observed on this machine and mount regime during these runs; business rows identical. |
| C2a | Online state 1 (main 411, non-empty committed WAL); old410 `pending` | STRICT_FAIL | Correct `unsupported`; main/WAL/SHM content bytes equal on all pre-existing members; only WAL/SHM atime changed (the old program read them); no member-set change; business rows identical. |
| C2b | Online state 2 (main user_version 410, committed 411 only in WAL); old410 `pending` | STRICT_FAIL | State 2 constructed by the real approved 013 migration under a live keeper connection (`wls init` exit 0; main SHA and user_version 410 unchanged; WAL 234,872 bytes; frozen copy reads 411; the migration connection was not the last to close; no explicit checkpoint); the old program refused `unsupported` from the WAL-visible version; all member contents equal; atime deltas only; business rows identical. |
| C3a | Online fixture, real Store lifecycle flock held; old410 `init` | STRICT_FAIL | Correct `conflict` (exit 3) before any user_version execution — an independent refusal condition, not a version refusal; main/WAL/SHM content and metadata equal; only the writer-lock ctime changed (lock-file `fchmod` pattern observed); business rows identical. |
| C3b | Online fixture, no flock; old410 `init` (real path, own column) | STRICT_FAIL | Correct `unsupported`; all member contents equal; metadata deltas observed in this window (main atime+ctime, WAL/SHM atime, writer-lock ctime); sidecars retained (the holder connection prevented the close-time unlink); business rows identical. |
| C4receipt | Traced vs untraced old410 `init` refusal, isomorphic fixtures | **PASS** | Mechanism receipt, normal-comparison scope only: the `-shm` open is visible in the streamed trace; exit code, error classification, added/removed/changed sets, main equality and business equality are identical between the traced and untraced runs. It verifies no kill rendezvous, identifies no tracee, must never gate controlled-kill execution, and equivalence of normal runs does not imply non-perturbation under interruption. Historical row record SHA-256 `c8a027f6be3df533650f2cbc205607102a096fbf91b3689b0acf9da730321bdf`. |
| C4 | Traced old410 `init`; historical controlled-termination attempt | RECORDED (historical row bytes retained) | Corrected adjudication: the historical attempt only ever signalled, state-confirmed and reaped the **strace tracer** (argv[0]=`strace`, tracer pid 830993, exit −9); the old-ELF tracee's identity, syscall-stop position, exit and reap are UNKNOWN / NOT_ESTABLISHED. The 34,419 ns figure (1766747342156736−1766747342122317) is the host's interval between receiving trace text containing the SHM path and sending the signal to the tracer — not a tracee syscall-stop position and not a rendezvous proof. Sidecar retention is equally explained by the online holder keeping its connection open for the whole row (HOLDER_CLOSE only after the AFTER window), so "killed before cleanup unlink" is NOT_ESTABLISHED. The captured Unsupported JSON (`error_classification=unsupported`) proves only that the refusal text was seen — nothing about the tracee's final outcome. Qualified controlled abnormal-exit rows: **0**. From the corrected runner on, `row_c4` returns a structured `NOT_RUN` before any fixture, holder, product, signal or observer effect and reads no receipt, and `_trace_old` rejects its kill mode before any effect; in this delivery C4 stays explicitly NOT_RUN. Historical row record SHA-256 `46c958f8267e1a6359766167e975c515ace0e5745b877c7942f28e1732e15eab`; historical trace file SHA-256 `58b7859b3601df112ff1702b2f803b9d3eeb1907180bfa04b18388da099ed3e4` (tracer-side output, ends at the SHM open). |
| C5 | One authorized holder commit inside the old program's presence window | RECORDED_OK (historical row bytes retained) | Supported fact — old-program process-lifetime overlap: one authorized holder commit (`compat_controlled_facts` 1→2) was provably committed while the direct old-ELF child (pid 831032, no tracer involved) was SIGSTOP-confirmed, before SIGCONT and its exit 2 / `unsupported`. Original business rows unchanged; the WAL/SHM content changes correspond to that authorized commit (WAL 20,632→24,752 bytes; SHM SHA changed; `raw_equality_claimed=false`); main raw-equal. NOT_ESTABLISHED: which SQLite/DB-access stage the stopped process had reached; simultaneous SQLite access by both sides; any raw trace for this row. Historical row record SHA-256 `9bdb03dfbda65a9320fec054defa6429cf68b33dc61df7df4b5ab8ddb1def0ec`. |
| C7a | Old410 `pending` on its own populated legal 410 (2 requests; 33 non-empty tables) | **SUPPORT_CONTROL_PASS** | Exit 0, business rows identical — binary, source root and fixtures proven usable, so the refusal rows above are not resource failures. |
| C7b | Current build `pending` on populated 411 (34 non-empty tables) | **SUPPORT_CONTROL_PASS** | Exit 0, business rows identical. |
| C6 | Existing nine-method store-open suite at the current SHA | SUITE_PASS (derived verdict; see note) | Single execution, exit 0, `Ran 9 tests … OK`, all nine methods ok. The original verdict record mis-parsed (it inspected stdout only; unittest reports on stderr) and recorded `method_count 0`; the original record is retained unchanged and the corrected verdict is a new derived record computed from the same captured stderr bytes (stderr SHA-256 `54afa6ea4d203f5c6ddcb6afa097a74116c150a25afa9933208e6de52cc2382b`; derived record SHA-256 `0fd2a15e58eab715ea516d1997545a797b080cf33cc937531829ad7b5a528f21`). The suite itself was not re-run; this is a derived correction, not an in-place edit of the original row. |

## Corrections to the original results statement

The original execution's results statement contained assertions that overstate
what the recorded evidence supports. They are corrected below; the original row
records, traces and receipts are unchanged, and the historical statuses above
are preserved. The source of each quote is the earlier (unpublished) statement
of these same results, not a published file.

| # | Earlier assertion (quoted) | Correction (observation-scoped fact) |
| --- | --- | --- |
| 1 | "SIGSTOP 34 µs after the traced `-shm` open" | The 34,419 ns value is the host's gap between receiving trace text containing the SHM path and sending SIGSTOP **to the strace tracer**; not a tracee syscall-stop position, not a rendezvous proof. |
| 2 | "stopped state confirmed, SIGKILL, exit −9, reaped" | The stopped/confirmed/killed/reaped process was the strace tracer (pid 830993, exit −9). Tracee identity, exit and reap: UNKNOWN / NOT_ESTABLISHED. |
| 3 | "WAL/SHM retained (killed before cleanup unlink)" | Sidecar retention is equally explained by the online holder keeping its connection open for the whole row; kill-before-cleanup-unlink is NOT_ESTABLISHED. |
| 4 | "process kill never classified as unsupported" | The same row records the complete Unsupported JSON on stderr with `error_classification=unsupported`; the sentence contradicts the row. The capture proves only that the refusal text was seen — nothing about the tracee's outcome. |
| 5 | "preserved all business rows and all main/WAL/SHM content bytes" | Holds only per-row, on pre-existing members: C1a **added** a new empty WAL and a new 32,768-byte SHM (member set changed; only the original main content equal); C5's WAL/SHM content changed (authorized holder commit, WAL 20,632→24,752, SHM SHA changed) with `raw_equality_claimed=false`; C2a/C2b/C3a/C3b and the historical C4 attempt are content-equal on all pre-existing members. |
| 6 | "the frozen old ELF always leaves metadata deltas" / "unavoidable metadata deltas" | Deltas were observed on this machine (ext4, `rw,relatime`), these fixture initial states and these sampled windows; C5's main had no metadata delta; sampled boundaries do not exclude unsampled instants. "Always"/"unavoidable" is not established. |
| 7 | "the raw traces bound the C4/C5 windows" | C5 has no raw trace at all — its window is bounded by row timestamps only. C4's trace is tracer-side output that ends at the SHM open and bounds nothing about the tracee. |

## Verification layering — what was actually run, and what was not

1. **Observer selfcheck (precondition for the product rows).** First run 17/22:
   five failures exposed a real observer defect (member-path `realpath`
   resolution followed a symlink member and defeated `O_NOFOLLOW`) plus two
   case-ordering bugs; fixed, second run **22/22 PASS**. Both runs retained.
   Positive coverage includes observer-only stability including timestamps,
   clean frozen-copy readback and the uniquely committed WAL row visible
   through copy semantics; negatives include same-length copy byte change,
   truncation, missing member, swapped mapping, WAL/SHM/main same-length
   changes with restored times, inode replacement, sidecar add/remove, chmod,
   mtime-only, atime-only, same-mode `fchmod` ctime, symlink/non-regular/
   oversized member refusal, read-time identity-chain rejection, WAL truncated
   mid-commit, WAL frame byte flip, WAL header flip (each removes or errors the
   committed fact, never a legally ignored tail) and recorded connection-close
   failure; conditional atime control under `relatime` (`O_NOATIME` observation
   left atime untouched while a plain read updated it).
2. **Historical matrix.** 12 single-shot attempts — C0, C1a, C1b, C2a, C2b,
   C3a, C3b, C4receipt, C4, C5, C7a, C7b (C1a/b counted separately; the
   C4receipt receipt run and the C4 attempt both counted) — plus one run of the
   nine-method suite (9/9 methods ok). No row was retried to green. C4
   qualified controlled-abnormal-exit coverage: **0** (NOT_ESTABLISHED).
3. **Runner-correction round (local, mock-only).** Zero product matrix rows,
   zero compilations, zero old-ELF invocations, zero strace/ptrace experiments,
   zero real SQLite fixtures, zero CTest changes. A deterministic in-memory
   harness — forged receipts never written to disk, all
   subprocess/signal/socket/SQLite/open effects intercepted before any real
   effect — produced: **red 12/12** against the pre-correction runner (the old
   `row_c4` read a forged `status=PASS` receipt and proceeded toward fixture
   construction; the old kill mode immediately attempted a strace launch; both
   intercepted, no real effect); a **first green run 17/23** (the core
   rejection assertions passed; the six failures were static-check definitions
   — line-number block keys and string hits in docstrings); **green 23/23**
   after the check definitions were fixed; and **44/44 data-level checks**
   (original row/trace/manifest hashes recomputed and preserved; the derived
   adjudication matches the actual row values; four transcribed-hash typos were
   caught and corrected in the derived records; all failed iterations
   retained). Intermediate harness versions are retained only as embedded
   digests plus result records — no full source snapshots of intermediate
   versions exist — so no claim is made that all stages used one identical
   harness or that intermediate versions were verbatim re-verified; harness
   paths and digests are private and are not listed here. These mock checks are
   tool-correction evidence only: not product registration, not a product
   regression, not C4 coverage. The 35 canonical checks (red 12 + green 23)
   are two selected tool-assertion counts, not 35 independent product
   regressions.
4. **This projection round.** Static checks only: existence/identity/hashing of
   the delivery set, JSON parsing, a docstring-stripped AST comparison of the
   pre-change and post-change runner (equal after removing the module docstring
   and the `row_c4` docstring) plus a verbatim diff confirming exactly the two
   report-reference replacements (all other code byte-identical), a semantic
   cross-check of this md/json pair, and a privacy screen of the published set
   and the intended PR title/body. No product code, archived code or harness
   was imported or executed; no compilation; zero new runs of any kind; no Git
   writes.

## Overall determination

`STRICT_COMPATIBILITY=BLOCKED_NOT_COMPLETE`, on honest grounds and with a
precise, reproducible mechanism: every necessary old-410 row produced the
correct refusal (`unsupported`, and `conflict` under a held lifecycle lock — an
independent condition) and preserved every business row. Content equality
holds per-row on pre-existing members (C1a added new sidecars; C5's WAL/SHM
changed only through the authorized controlled commit). Metadata deltas (atime
through the old program's own reads under `relatime`; ctime through the
same-mode `fchmod` pattern observed on this machine) were observed on these
fixtures and sampled windows and are not claimed as universal behavior. The
strict raw+metadata equality required by Compatibility02/03 is therefore not
met by the frozen old program, and no current-product minimal fix changes it
(the current candidate passes all nine store-open methods at SHA
`fab23641…`). This is delivered as a verifiable BLOCKED, not a compatibility
PASS; expectedFailure, skip, time restoration, mount changes and field removal
were not used to obtain green rows.

## Registration states and end states

Six states with exact scope, as of 2026-11-04T22:20:58.670936+00:00:

- `IMPLEMENTATION_COMPLETE=true` — scope: the evidence-tool delivery (observer,
  selfcheck, runner with C4/kill disabled and scoped notes) and this corrected
  self-contained public projection only.
- `LOCAL_VALIDATION_COMPLETE=true` — scope: the historically executed evidence
  (selfcheck, 12 matrix attempts, nine-method suite) plus the local mock and
  static verification of the correction and of this projection; no product
  regression was executed or claimed this round.
- Immediately adjacent, same object: `original_matrix_coverage_complete=false`,
  `c4_product_crash_coverage=NOT_ESTABLISHED` (qualified controlled
  abnormal-exit rows: 0), `overall_strict_compatibility=BLOCKED_NOT_COMPLETE`.
  This projection's static checks passing does not make matrix coverage
  complete and does not close Compatibility02/03.
- `PUSH_COMPLETE=false`, `REMOTE_CI_COMPLETE=false`, `PR_REVIEW_COMPLETE=false`
  and `MERGED=false` — no git write, commit, push or PR was performed by this
  task. Formal PR review and merge are separate states, not implied by anything
  above; local tool checks are not mixed with formal acceptance.

End states: Windows 11 ordinary-user NTFS acceptance `NOT_RUN`; restricted
private hosting of original artifacts `BLOCKED` (originals retained locally);
`historical_clang_asan_failures_closed=false` unchanged; Store 411 via the
approved 013 migration remains approved and implemented, unchanged; N5-2 items
014/412 remain unauthorized. The missing controlled-tracee stop mechanism and
stronger simultaneous-SQLite-access evidence remain explicit limitations.
Evidence from other tasks and platform runs is not mixed into this report.

## Machine projection

`docs/reports/n5-compatibility-strict-20261104-v3.json` carries the same
content in structured form with identical statuses, identities, denominators,
scopes and end states.
