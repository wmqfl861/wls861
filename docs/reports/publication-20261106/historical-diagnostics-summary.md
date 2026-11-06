# Historical diagnostics (S0–S3, 2026-11-04) — minimal factual summary

This file is a safe summary of private diagnostic stage receipts produced on
2026-11-04 under the N5 continuation diagnostics task (each receipt records
its own workspace head; the diagnostics worktree stood at
`3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123`). The private originals are **not**
published; they are retained locally with the hashes and sizes listed in
`publication-index.json` (`old_files`, disposition
`WITHHOLD_PRIVATE_ORIGINAL_HASH_AND_SUMMARY_ONLY`). These were diagnostics of
their time: this publication backfill did not re-run, extend or close any of
them, and no product gate is closed by this summary. No launch commands for
these historical probes are published here.

## S0 — identity and precondition receipt (`S0-identity-receipt-01.json`)

Read-only verification, before any diagnostic execution, that the S1/S2 tool
and input preconditions held: attachment-gate identities (values withheld),
workspace head/branch/porcelain state, resource window (data-disk free space,
memory available, load average, no product jobs running), and input-hash
matches for the S1/S2 plan, runners, helpers and summary identities.
Conclusion recorded in the original: S1 and S2 actual preconditions were
satisfied; both tools were read in full including signal cleanup and terminal
codes; no historical artifact was modified by these checks (read-only
hashing).

## S1 — old/new UBSan staged run (`S1-ubsan-old-new-01-receipt.json`)

Classification in the original:
`DIAGNOSTIC_EVIDENCE_NOT_COMPLETE_ONE_INPUT_ONLY_NOT_PRODUCT_GATE`.

- First input (old c2 ELF, event_runtime suite): actual CTest exit 0;
  14 methods started, 14 passed, 0 failed; JUnit time 38.15 s; the
  historical 90 s registration timeout was NOT hit under then-current host
  conditions.
- Evidence completeness: FALSE. Recorded causes: (a) DSO capture incomplete —
  7 of 9 direct DSO paths were absent from the observed process maps (the
  observed maps contained only CTest's own DSOs, i.e. the event_runtime_tests
  ELF process itself was never tracked by the observer; single-path child
  discovery does not see children spawned from non-main threads); (b) an
  exact ALLOWED_METHODS label match was impossible for the old input — one
  method label was renamed between old-c2 and the frozen 3a list, the other
  13 labels were equal.
- Second input (new 3a): NOT_STARTED by runner rule — the first input's
  evidence was incomplete and the plan and runner forbid starting the second
  input (attempts per input = 1, retry disallowed).
- Hypothesis reading recorded in the original: "current host conditions
  reproduce the old 90 s timeout" NOT_REPRODUCED in this single observation;
  "input differences" NOT_TESTED; the original formal-matrix timeout REMAINS
  OPEN.

## S2 — passive Make-IPC process sampling (`S2-make-passive-01-receipt.json`)

Classification in the original: `PASSIVE_PROC_SAMPLE_DIAGNOSTIC_NOT_PRODUCT_GATE`.

- One derived-input run (v4 passive observer sharing the crash-fixture
  source): CTest exit 8 after 180.88 s; 25 methods started, 22 passed,
  3 failed — the crash-preservation methods (W11–W15 class), the W16
  AMEND/cancel crash retention method and the W19 legacy-crash upgrade
  method.
- Failure signature: all three failures hit the fixture parent's 5 s poll on
  the crash pipe (`crash_receive`), a fixture-side wait, not a Store IPC
  poll; poll returned 0 (timeout).
- Discriminating observations: children stayed in R (running) state,
  CPU-busy, not blocked and not exited; the fixture-side full-table
  checkpoint hash spans exceeded the 5 s fixture pipe deadline (W11_prepare
  5.68 s; W16_amend final checkpoint still unfinished after 6 samples; W07
  2.79 s passed); store-open/capture/reserve/settlement spans were
  sub-second (0.04–0.69 s).
- Confirmed class recorded in the original: "no barrier packet because the
  child was busy computing the fixture checkpoint hash under ASan" — a
  fixture-side 5 s pipe deadline exceeded by fixture-instrumentation CPU time
  under that sanitizer configuration. Root cause: OPEN per the analyzer
  rule; the original Make four failures remain OPEN on their own input and
  this diagnostic does not close them.

## S3 — isolated single-suite probe on the original ASan ELFs (`S3-asan-probe-01-receipt.json`)

Classification in the original:
`ISOLATED_SINGLE_SUITE_PROBE_ON_ORIGINAL_ASAN_ELFS_NOT_PRODUCT_GATE`.

- Fixture fidelity: the original test-plan entries were copied exactly
  (command argv, 180 s registration timeout, original working directory);
  the `--show-only` comparison passed for both tests; the probe used its own
  private TMPDIR; the historical ASan build tree was content-hashed before
  and after — 9,877 files byte-identical, 0 errors on both passes.
- `persistent_event_runtime`: FAIL, 12 methods passed / 3 failed, 47.31 s.
  The two gated failing sites reproduced exactly; the third failing site
  drifted between runs (isolated :387 vs shared-matrix :668), i.e. the
  failing-site set varies with timing; the stop-gate methods passed again.
  Sampler: 91 samples of the ELF showed 85 running + 6 disk-sleep, CPU-
  saturated during the wait windows. Interpreting those observations at the
  time, the original S3 receipt read them as consistent with fixed 5 s
  wall-clock bounds versus ASan Debug CPU pace and not evidence of the
  previously fixed lost-wakeup defect; this summary repeats that reading
  only as the receipt's own historical inference, not as a causal finding
  made or confirmed by this backfill, and the runtime root cause remains
  OPEN.
- `live_model_store_tls`: PASS in isolation, 31.59 s. The shared-matrix
  failures (driver 18 s subprocess deadline exceeded; `large_escaped`
  accepted=0) did not reproduce isolated. At the time, the original S3
  receipt characterized those shared-matrix failures as load/contention
  dependent with no semantic regression indicated; this summary repeats that
  characterization only as the receipt's own historical inference, not as a
  semantic-regression closure made or confirmed by this backfill, the TLS
  root cause remains OPEN, and the original shared-matrix failures stay
  open on their own input.

## P — failure-stage and coverage mapping (projection published)

The P-stage report is published as a redacted projection at
`historical-reports/diagnostics-20261104/P-failure-stages-and-coverage-mapping-v1.md`.
Its minimal facts: the then-current shared-ASan terminal state
(`95 = 64P/2F/29T`, exit 8) was mapped to three 5 s `until()` wait failures
in `persistent_event_runtime` (12P/3F), a `persistent_event_store` TIMEOUT at
its 120 s registration (its 30 methods passed in Release/Clang), and
`live_model_store_tls` FAIL under the shared matrix (PASS in the S3 isolated
probe); the 21 defined crash-window scenarios and the non-crash T0–T4 /
lifecycle coverage were mapped to already executed coverage; coverage gaps
G1–G5 were recorded as proposals for the planner, not implemented. Root-cause
attributions in the P/S receipts are historical hypotheses; the current root
causes for the sanitizer failures remain OPEN.

## Withheld originals

The 16 private originals are enumerated with SHA-256, size, category and
reason in `publication-index.json` (`old_files`): the four stage receipts
(S0–S3), the private implementation manifest, one 2,412,873-byte build-tree
inventory content stored as two files sharing a single SHA-256, three empty
files sharing the empty-byte SHA-256 (two `.errors` files and one
`.stderr`), the probe's JUnit XML, the fixture listing (json+stderr), the
18-byte probe-launch record, the 105,033-byte passive process-observation
sampler output (`sampler.py` itself is published as an exact `.txt` copy
under `diagnostics/asan-probe-20261104/`), and two CTest fixture/cost files.
Nothing in this summary substitutes for those originals.
