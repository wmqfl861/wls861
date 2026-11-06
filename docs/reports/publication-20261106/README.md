# Publication backfill (2026-11-06): index and historical public projections

This directory is a one-time documentation backfill for the wls861 repository.
It makes already existing, public-safe engineering artifacts findable in Git
and records an explicit disposition for old local files that are **not**
published. It adds no product code, no tests, no validation runs and no
acceptance. Every projection below is a redacted new version derived from the
original local sources; the three `.txt` files are exact byte copies. See
`publication-index.json` for the complete mapping, original hashes, copy
hashes and withheld-item reasons.

## Contents

| Path | Nature |
| --- | --- |
| `publication-index.json` | Machine-readable index: the 19 previously unpublished commit identities and their remote same-stable-patch-id counterparts; the 8 existing branches whose exact tips cover all 19; a disposition for each of the 18 branches that had no same-named remote ref; per-file disposition of all 29 old local files; permanent references for the 2 reports already public on the remote; a safety-projected snapshot of the current five-module state. |
| `historical-diagnostics-summary.md` | Minimal factual summary of the S0–S3 diagnostic stage receipts (2026-11-04). Private originals are not published; their hashes, sizes, categories and safe summaries are recorded. |
| `historical-reports/` | 8 redacted public projections of historical reports (2026-11-03/04): compatibility strict v1/v2 (md+json), Windows evidence continuation (md+json), N5 review final delivery (md), P failure-stage/coverage mapping (md). Projections are new versions, **not** byte copies; each carries a header with the historical timestamp, source workspace head, original SHA-256, redaction scope and `NOT_CURRENT_ACCEPTANCE`. |
| `diagnostics/` | 3 exact-byte copies of historical diagnostic sources stored as `.txt` (`persistent_event_runtime_tests.cpp.txt`, `persistent_event_runtime_diagnostics.inc.txt`, `sampler.py.txt`): non-executable, not registered in any build, `NOT_ACCEPTED` historical diagnostics. Do not execute, build or register them. |

## Reading rules

- Historical statements, failures, UNKNOWN values, timestamps and six-state
  values in the projections are preserved as they were; nothing was repaired
  to look greener. Where a later report corrected an earlier one, the
  correction is noted adjacent to the projection, not edited into it:
  compatibility v1's C4 row and aggregate claims are corrected by v2 in the
  same directory; the 2026-11-03 delivery report retains its then-unfilled
  final-status block and its NOT_RUN shared-ASan/Make-ASan rows; the Windows
  continuation's assessments are qualified by the later 2026-11-05 correction
  report published on the PR #39 branch; P/S root-cause attributions are
  historical hypotheses and the sanitizer root causes remain OPEN.
- 16 of the 29 old local files remain private by disposition (stage receipts,
  raw process sampling, build-tree inventories, fixtures, launcher records).
  Their SHA-256, sizes, categories and reasons are in `publication-index.json`
  (`old_files`); the originals stay on the local data disk.
- The 8 historical branches listed in the index are pushed (by the publishing
  step, at exact tips) only for archival commit-identity coverage. They get no
  new PRs, their old gate conclusions are not adopted, and CI runs
  auto-triggered by those pushes inherit nothing from other SHAs' results.

## Current five-module snapshot at backfill time (2026-11-06)

Safety-projected fields of the authorized audit-fix batch (the private
execution index is not published; see `publication-index.json` →
`current_modules` for full SHAs, report/PR/CI links and the six states of
each module):

- Draft PRs, all OPEN and unmerged: compatibility
  [#37](https://github.com/wmqfl861/wls861/pull/37) `7cf8216f…`, proc
  [#38](https://github.com/wmqfl861/wls861/pull/38) `60aafac5…`, Windows
  [#39](https://github.com/wmqfl861/wls861/pull/39) `2719f710…`, product
  [#40](https://github.com/wmqfl861/wls861/pull/40) `2326e521…`, validation
  [#41](https://github.com/wmqfl861/wls861/pull/41) `a90d96fc…`.
- Each module's successful Linux CI belongs to its own exact input; it is
  **not** a five-module joint pass. The joint core `aa24f1dd…`
  (`test/audit-core-integration-20261105-adp35vg4`) contains product +
  validation code only and excludes proc/compatibility/Windows.
- Local matrix: GCC Release umask0022, the same-ELF Release umask0002, and
  strict Clang18 each passed 95/95. The original UBSan run was interrupted at
  70 passed / 1 failed / 15 timeouts (1 started without terminal state, 8 not
  started); a separate 9/9 supplement does not combine into one 95/95 round.
  Shared GCC ASan/LSan: 67 passed / 1 failed / 27 timeouts, NOT_COMPLETE. The
  original Make-ASan's 16th RESULT line was 33 passed / 1 failed with later
  targets NOT_RUN. 11 outer wrapper exit/signal/cleanup fields remain UNKNOWN
  and are not back-filled from CI success.
- Windows: the latest two native runs failed at configure; build, CTest,
  install, capabilities and detached stages were NOT_STARTED; a configure
  root-0 is not a gate PASS; the live `vctip.exe` under the same Job Object
  was cleaned up (PROVEN_ZERO). Windows 11 ordinary-user NTFS acceptance is
  NOT_RUN; the historical image-source conflict is UNRESOLVED; the full
  Windows runtime is NOT_IMPLEMENTED.
- STRICT_WAL_SHM compatibility remains BLOCKED_NOT_COMPLETE; restricted
  private hosting of originals remains BLOCKED; historical
  Clang/live_workflow/IPC failures remain OPEN. The five modules' engineering
  review PASSes are scoped to their own modules; formal PR review is not
  complete, nothing is merged, and overall product acceptance is
  VALIDATION_BLOCKED.
- This backfill itself adds 0 product tests, performs 0 product builds or
  sanitizer runs, and makes 0 real or paid model calls to a WLS product
  supplier for acceptance. The engineering assistant model requests that
  produced and checked this documentation did occur and were non-zero; the
  zero above covers only WLS supplier calls, and no free-of-charge or
  zero-model-request claim is made for those engineering requests.
