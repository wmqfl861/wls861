# A4 same-source fairness evidence

`a4_fairness_same_source.cpp` preserves the original frozen offline counterexample
byte for byte (SHA-256
`48ae8f4db3d29835403bc41daba2554d51a2c05d5a7c3901ee901193fd25aad6`).
Its business case also matches the corresponding `async_coordinator_tests` case.
It uses FakeModel, private temporary SQLite databases and the original five-second
barriers; no provider call or production service is involved.

Run `python3 -B tests/test_a4_fairness_gate.py` for lightweight classification
checks only. They prove that the evidence parser rejects unrelated failures;
they do not execute or establish the native red/green result.

For actual execution, prepare two clean checkouts: old A3 at the exact commit
`a2dfb46322d217f32eb5cc2516de6107c10db4b8`, and the candidate A4 commit. Supply a new
output directory outside both checkouts, on the data disk for local execution:

```sh
python3 -B scripts/run_a4_fairness_red_green.py \
  --baseline-source /data/a3-baseline \
  --candidate-source /data/a4-candidate \
  --candidate-sha FULL_CANDIDATE_COMMIT_SHA \
  --output /data/a4-fairness-run-UNIQUE \
  --compiler g++-13
```

The script requires an ordinary Linux user and GCC 13.3.0. Each version builds
the same copied CPP through a separate CMake wrapper, using that version's own
headers, static libraries, SQL, schemas and configuration. Release `-O3 -DNDEBUG`
and the existing strict warnings remain enabled. Both builds finish before
either probe starts; the two probes run serially under the original 180-second
outer limit and independent private TMPDIRs. This Release diagnostic does not
replace any sanitizer or full regression gate.

An old-version red result requires exit 1, exactly one original request/team
order assertion (`a1-first,b1` or `a1-first,b1,a2`), and the exact original
`RESULT 0 passed, 1 failed` accounting. Compile failures, other assertions,
five-second fixture deadlines, outer timeouts, signals and unexpected output
fail the diagnostic. The new version must exit 0 with its original sole PASS
and `RESULT 1 passed, 0 failed`, with empty stderr. Only that pair yields
`RED_GREEN_VERIFIED` and runner exit 0.

`output/evidence/` retains every subprocess argv, cwd, allowlisted environment,
umask, exit, raw stdout/stderr, source commit and before/after tracked-input
hashes, probe and binary/library identities, compiler/build commands, process
cleanup observations, final summary and artifact SHA-256 inventory. Failed or
interrupted runs retain their own directory; a rerun must use a new output path.
The CI evidence directory should be uploaded with `if: always()`.

This is one separate diagnostic with two version executions. It does not add
entries to the reviewed product CTest set, change existing test deadlines, or
replace the original six Linux CI configurations. The summary preserves the
candidate's `tests/gcc_asan_expected_tests.json` names, deadlines, count and hash
as companion scope only; this diagnostic does not execute that product set.
A CI execution is remote evidence and must never be reported as a local
execution. Implementation and classification checks alone do not establish a
reproduced native counterexample.
