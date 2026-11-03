# N5-0: original-input sanitizer observations, 2026-11-03

This is a new diagnostic record against the unchanged product
`c2a5cea64aeea261b214cdfd9a968b53f491110a`, following frozen handoff
`d843493fafe158f3a6443bafaa0de95cbf6c05c2`. No historical report, original log,
ELF, library, SQL, deadline, assertion or sanitizer option was changed. This
report does **not** close the original local failures or replace a final-input
formal gate. All execution used UID 1000 and the verified `/mnt/d` ext4 data disk.

The private task root is
`/mnt/d/codex-workspace/wls861-n5-review-20261103-74bnxwj5`. Private originals,
plans, commands, actual exits and before/after source/binary identities are
retained under its `evidence/`. None of those raw logs is uploaded publicly.

## Original failures retained

The original c2 complete GCC Debug ASan/LSan attempt registered and attempted all
94 tests: **54 passed, 38 timed out, 2 failed**, no skipped/notrun. The two
non-timeout failures were persistent-event runtime (10 methods pass / 3 fail at
the original five-second wait) and loopback TLS Store (9/10 methods, large
escaped output 17.144784929 seconds against the unchanged 16-second assertion).
The 38 timeout names and original JUnit locations are independently extracted in
`evidence/asan-original-failure-groups.json`; this extraction is not another run.

Original Make-ASan exited 2 (outer launcher 1), entered 20 recipes with the first
19 successful, then Store reported **21 methods pass / 4 fail** at the original
five-second `CHECK(polled == 1)`; 74 recipes were unentered. A coordinator report
has an earlier conflicting recipe-success count; this record uses the original
Make execution log, separates entered recipes from Store methods and preserves
both prior records. Make produces no native CTest LastTest/JUnit.

## Make: declared subset and full-prefix comparison

The first diagnostic links a derived test-only translation unit to the original
Make production archives. A numeric-only observer records poll return/revents/
errno, peer read/write, child stages, parent/child PID, kill and reap. It retains
every original five-second boundary and assertion. No payloads, sessions or raw
business packets enter the numeric observer; original private stderr is separate.

The selected four originally failing groups, in their original order, passed
**4/4**, outer CTest 0, 79.65 seconds. All 30 observed polls returned 1/POLLIN.
This does not reproduce or explain the original four full-suite failures.

The next frozen experiment changes only selection/prefix: its test translation
unit and complete 25-method `main()` are byte-for-byte original c2, with the exact
same observer `.inc`, production archives and sanitizer flags. The 420-second
diagnostic outer bound is labeled separately: original Make has no overall suite
deadline, and it is not the CMake Store's 120-second product gate. A non-author
review checked the plan and input identities before execution.

That full-prefix diagnostic exits **CTest 8 / wrapper 1**, 160.38 seconds, with
**24 methods pass / 1 fail**. W00 fails at the unchanged `polled == 1` assertion:
the actual return is **0**, revents 0. The child has not sent the target packet;
parent cleanup sends SIGKILL and successfully reaps it. The original four failing
groups pass in this experiment, so they are not relabeled as reproduced.

For the failed W00 child, observed Store-open span is 4.013 seconds, capture span
0.984 seconds; checkpoint starts just before poll expires. Passive `/proc`
samples show lower-bound user CPU 0.53 seconds and system CPU 4.01 seconds over
4.56 observed seconds, with about 1 GiB resident memory and sampled running
state. The following W06 child opens Store in about 0.107 seconds. These facts
support investigating inherited sanitizer/fork memory, faults and repeated
validation; they do **not** establish allocator, kernel, disk or compiler as the
unique cause. No production optimization was made from that hypothesis.

| Private attempt | Original artifacts |
| --- | --- |
| `make-ipc-stage-01/` | Frozen plan, compile, derived ELF, original LastTest/JUnit, commands, numeric phases and passive process observations |
| `make-ipc-full-prefix-02/` | Same categories, explicit failed poll, `analysis.json`, unchanged source/archive/derived-ELF identities |
| `make-ipc-full-prefix-review-01/review.json` | Non-author `/root/compatibility_sidecars`, no blocker for the declared diagnostic; not coordinator approval |

The full-prefix plan SHA-256 is
`ed3c7cef5905ac22e40f9c4d183a1f25288f5da42d9160125b0e91260d85b92c`;
its ELF is
`1d120233bdccf37eb99cccb130fa18ac592cf375cfe14731e78828552a317a50`.
Original production archives and all measured inputs remain equal before/after.

## Shared GCC ASan: four representative original suites

The next predeclared observation executes the **original c2 CMake ASan ELFs,
headers, tests and resources**, without recompilation. Store, runtime, TLS and
structured CLI are selected from the original 94-test plan. Each retains its
original working directory and exact 120/180/180/120-second CTest deadline.
The only diagnostic additions are passive PID/CPU/fault/wait-channel samples,
timestamp/hash association for raw output arrivals and the existing opt-in,
allowlisted TLS phase observer. No task build or other sanitizer overlaps it.

| Suite | Result in this one representative diagnostic |
| --- | --- |
| `persistent_event_store` | TIMEOUT at original 120 seconds; method progress survives in original LastTest |
| `persistent_event_runtime` | FAIL, 32.72 seconds; 12 methods pass / 1 fail. Original five-second wait in the first two-request/two-node scenario fails at its original assertion. |
| `live_model_store_tls` | PASS, 10/10 methods, 38.83 seconds; previous large-output failure is NOT_REPRODUCED here |
| `r1_structured_cli_smoke` | PASS, 6/6 methods, 58.72 seconds; prior full-suite timeout is NOT_REPRODUCED here |

Overall **2 pass / 1 fail / 1 timeout / 0 skipped / 0 notrun**, CTest 8 / outer
launcher 1, 252.23 seconds. Complete private LastTest/JUnit and source/ELF
before/after identities remain in `evidence/asan-original-representatives-01/`.
Every measured input is unchanged. This is a selected diagnostic, not a full
94-test rerun, a new-candidate gate or acceptance of the TLS/CLI historical cases.

The Store parent has sampled lower-bound user/system CPU 98.65/7.53 seconds
over 119.86 seconds, 1,442,466 minor faults and 0 major faults; 402/446 samples
are running. Runtime has 29.44/1.96 CPU seconds over 32.26 seconds, mostly running.
These executions are substantially CPU-active, with some observed journal waits;
the evidence does not support declaring a global idle-lock deadlock or storage
as a sole cause. Sampling is not a complete accounting profile. Existing older
restore/canonical/parse instrumentation remains separate and is not combined
arithmetically with these new input timings.

## State and next discriminating work

The original Make four failures, c2 complete ASan failures, historical Clang
timeouts and old live_workflow cause remain **OPEN / UNRESOLVED**. The evidence
gap is reduced: an actual poll=0 and child stage are captured, and original
Store/runtime failure conditions remain observable with exact ELFs. No deadline,
business guard or sanitizer was weakened to obtain logs or a passing result.

A next authorized diagnostic can correlate the full-prefix child's minor faults
and syscall phases with the observed high system CPU, or instrument bounded
validation spans in a separately identified derived ELF. It must freeze the
hypothesis/input first, preserve the original ELF and not count instrumentation
as a formal gate. Repeating whole matrices without changed input or new evidence
is not a root-cause strategy.

The independently corrected static public projection and local private evidence
preservation are separate tool deliverables. Hosted private persistence remains
**BLOCKED**, and final-input product regression/CI are separately recorded.
This report grants no formal PR approval, merge, Store 412/DAG implementation,
real supplier/paid request or production operation.
