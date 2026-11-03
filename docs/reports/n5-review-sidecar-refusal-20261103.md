# N5 review: SQLite refusal and sidecar lifecycle, 2026-11-03

This is a new, bounded follow-up to coordinator handoff
`d843493fafe158f3a6443bafaa0de95cbf6c05c2`. It preserves the original
Compatibility02/03 failures and their raw manifests. The examined product input
is `c2a5cea64aeea261b214cdfd9a968b53f491110a`; this report does not substitute a
new result for either historical attempt.

The data disk is `/mnt/d`, backed by `/dev/vdb2`, ext4. The ordinary execution
user is UID 1000. New private fixtures, raw strace output and evidence are under
`/mnt/d/codex-workspace/wls861-n5-review-20261103-74bnxwj5/evidence/`.

## Reproduced mechanism

The old program correctly rejects Store 411, but it opens SQLite before checking
`PRAGMA user_version`. A read-only SQLite connection to a quiescent database in
WAL mode may create the WAL and shared-memory files. A subsequent writable
connection may remove those sidecars during final close even when the Store
version guard rejects the database. Checking the main database alone does not
establish strict file-set preservation.

The earlier Compatibility02 observer performed a Python SQLite read before its
raw inventory; a connection context manager did not itself close that connection.
The earlier Compatibility03 observer avoided its own SQLite side effects and
then detected sidecars created by the old read-only program. Those are distinct
observations; both strict failures remain `NOT_COMPLETE`.

A single new diagnostic retained the original executables and used new fixtures.
It compiled nothing and imported or executed no archived helper. Its three
refusal invocations all exited 2 with the original `unsupported` error:

| Exact execution | Strict raw difference | Main database |
| --- | --- | --- |
| Original 410 ELF, `pending` on a real c2-initialized 411 database | Adds a zero-byte `-wal` and a 32,768-byte `-shm` | Bytes, device/inode, ownership, mode and links unchanged |
| Same original 410 ELF, `init` on that same database after the preceding read-only refusal | Removes those two sidecars; strace records both successful `unlink` calls | Same complete identity and bytes unchanged |
| Original c2 ELF, `pending` on a separate valid WAL database with canary `user_version=999` | Adds `-wal` and `-shm` before refusing | Same complete identity and bytes unchanged |

The final case establishes a current-code refusal defect, rather than relying on
a guard-removal mutation. The version-999 fixture is a disposable negative
canary, not a new Store migration or authorization for Store 412. No SQL asset
was changed. It does not reproduce a business-authorization bypass or lost
business rows.

Original executable identities:

| Input | Bytes | SHA-256 |
| --- | ---: | --- |
| c2 Release `wls` | 5,117,392 | `3c383d0b6625a47c1a79671a8f71331ca37e612078696e2a481dfb242f7e4cfc` |
| Actual original 410 `wls` | 4,733,672 | `b048a449ecdc22cd53ccf2813804bd36c720a26870732f03ebe9c066ed5d407e` |

The original 410 build records commit
`7bf3a16d9683ed6a8de65d88023bae9687abd20d`. Its relevant source bytes were
previously bound to the later unchanged-source checkout `aa2c592…`; this
diagnostic does not relabel that old executable as a fresh build of aa2 or c2.

The diagnostic ran once from 06:17:09.086101 to 06:17:09.876483 UTC. Its outer
exit was 0 because observation completed; **this is not strict compatibility or
product-gate success**. All selected source, executable, plan and diagnostic
script identities were unchanged before/after. The complete raw command,
stdout, stderr, strict inventories and trace are retained in
`evidence/compatibility-lifecycle-probe-01/`. The separately prepared plan is
`evidence/compatibility-probe-plan-01.json`.

## Minimal candidate and permanent regression

A refusal-only preflight reads at most the 100-byte SQLite main-file
header through a no-follow, nonblocking regular-file descriptor. It rejects an
explicitly newer header before opening SQLite or chmodding the database. It
does not authorize any supported version, create a new connection, delete a
sidecar, bypass a writer lock, or use SQLite `immutable` to hide live WAL.

An old main-file version is not sufficient to accept a database: a newer
committed `user_version` can still reside in WAL. The original real SQLite,
metadata, migration, ownership, recovery and transaction guards must continue to
run. A malformed or incomplete header also does not grant acceptance.

The new executable regression entry is:

```sh
python3 -B tests/store_open_compatibility_tests.py /absolute/path/to/wls /absolute/source/root
```

It contains nine independent methods: future read-only refusal, future
takeover refusal, pre-existing empty WAL/SHM preservation, no chmod of an
unrelated future database, a newer committed version in WAL despite an old main
header, supported-411 read compatibility, symlink refusal, invalid-magic and incomplete-header
handling. The WAL-version case verifies the actual SQLite guard and unchanged
main/WAL bytes; it does not claim immutable shared-memory bytes while a live
connection coordinates readers.

The unchanged original c2 Release ELF ran this exact nine-method suite once from
06:24:17.207699 to 06:24:17.875501 UTC. The process exited 1 with six passing
methods and three assertion failures: future read-only refusal created sidecars;
future takeover removed pre-existing WAL/SHM; future takeover changed the main
database mode from 0640 to 0600. No method failed through a build error, timeout,
missing API or removed-guard mutation. The takeover case without pre-existing
sidecars passed because its final inventory was unchanged; the earlier original
410 takeover trace separately records temporary SQLite sidecar creation/removal
during its refused open. A final-inventory assertion does not observe all
intermediate file activity.

The complete immediate method output and original tracebacks remain in
`evidence/compatibility-cli-red-01/stderr.log` (SHA-256
`f599acf39844658df31ab37d19e1c5c93edf3d1f075a91c5bf442e66d42d5299`).
Its `command-before.json`, `receipt.json`, and identical `inputs-before.json` /
`inputs-after.json` bind the command, source, test, manifest and original ELF.
The test source SHA-256 is
`da361e4af14ab12535ba1a3fc2ff01ad496b2d14e90d03b574ff3f71cbe8ce3b`.
This is a standalone product regression, not a sanitizer gate or CTest result.

The integrator then compiled only the candidate Store translation unit with
GCC 13, Release `-O3 -DNDEBUG`, C++20 and the original strict warnings, and linked
it before the retained c2 production archive using the exact original c2 CLI
object and kernel archive. Compilation exited 0 after 61.011 seconds; linking
exited 0. The original object/archive inputs and candidate source identities
were unchanged before/after. This is an explicitly identified derived candidate
ELF, not a clean complete build or a full Release/sanitizer gate.

The **same unchanged nine-method test source**, source-root resources and
business conditions then passed 9/9, with no failed, timed-out or skipped
methods. The invocation ran from 06:36:00.799736 to 06:36:01.416318 UTC, actual
exit 0 (0.617 seconds around the command; unittest reported 0.481 seconds).
The candidate ELF SHA-256 is
`d2e703a87397a79f11cd6b0f67822b1f45c3c90bcd4674d55a94197d2e4eb0a3`.
Complete compile/link/run commands, outputs and identity manifests remain in
`evidence/compatibility-cli-green-01/`. The raw method output `run.stderr`
SHA-256 is `72ac355c1661cad07a9c1cad243db6d82faa0e17c169f0d60c43f7651c228212`;
`receipt.json` SHA-256 is
`b86564a76fbb177ba91379b6a8027530ae9689f9e3345a0549eeaaece41bd3a1`.

| Regression condition | Original c2 ELF | Derived candidate ELF |
| --- | --- | --- |
| Future read-only refusal preserves exact inventory | FAIL: adds WAL/SHM | PASS |
| Future takeover refusal preserves exact inventory with initially absent sidecars | PASS | PASS |
| Future takeover preserves pre-existing empty WAL/SHM | FAIL: removes WAL/SHM | PASS |
| Future takeover preserves unrelated database mode | FAIL: 0640 becomes 0600 | PASS |
| Old main header cannot authorize a newer committed WAL version | PASS | PASS |
| Supported 411 read behavior and main bytes remain compatible | PASS | PASS |
| Symlink cannot supply a future header | PASS | PASS |
| Invalid SQLite magic remains corrupt | PASS | PASS |
| Incomplete header retains the actual corruption guard | PASS | PASS |

The integrator owns production edits and common registration/catalog updates.
The intended new `store_open_compatibility` CTest registration has its own
120-second suite bound. It adds **one** registration, 94 → 95; these nine methods
are not nine registrations. Existing suite bounds and each new method's explicit
20-second CLI subprocess limit remain unchanged. Final clean full regression,
the reviewed catalog and exact final-SHA CI remain separate integrator work.
The integrator's `compatibility-catalog-check-01` separately verifies all 94
previous catalog names and deadlines against the c2 Git object, with only the
new 120-second registration added. The branch's complete runner selfcheck
passes 57/57, exit 0, with unchanged input identities. Those are tool checks,
not another nine-method product run or a complete sanitizer result.

## Remaining boundary

No change to the new program can retroactively change the already-frozen old
410 executable. Compatibility02/03 remain strict failures. The original old
program's refusal and unchanged main bytes remain true, but do not establish
unchanged raw sidecars. A new header preflight can protect the updated program's
future refusal path; it cannot prove a general no-write property for an active
database whose real schema version is only in WAL.

Applicable full regression, sanitizer results, exact final-SHA CI, independent
coordinator acceptance and publication are separately recorded by the integrator.
This follow-up grants no merge, downgrade, model-call or paid-call authority.

The execution-side scope check is
[recorded separately](../reviews/n5-review-future-header-preflight-scope-20261103.md).
It discloses that its author supplied the initial helper suggestion, so it is
not counted as non-author independent review. A different agent's source review
and the coordinator's final acceptance remain separate requirements.

The non-author execution sub-agent `/root/windows_policy_provenance` subsequently
reviewed that exact production-file SHA without editing this worktree or
rerunning the author's tests. It found no blocker in the 40-line difference and
retained the TOCTOU, active-WAL and access-time limitations. Its source-review
receipt is `evidence/windows-nonauthor-header-review-01/review.json`, SHA-256
`ab9940f620dec974423d244e16f04bac24506698a1485baeefc21064e809cb7b`.
This is a non-author local agent review, not a second-model evaluation, a new
9/9 execution, or formal coordinator acceptance.

The separately requested private evidence boundary and four terminal-state
acceptance proposal are recorded in
[the storage threat model](../implementation/n5-review-private-evidence-threat-model-20261103.md).
The user's local-only policy is preserved; hosted private storage remains
`BLOCKED` and no raw LastTest/JUnit is uploaded to public GitHub artifacts.

At this report's time scope, the bounded future-header refusal change and its
nine-method regression have `IMPLEMENTATION_COMPLETE=true` and targeted red/green
coverage. `LOCAL_VALIDATION_COMPLETE=false` until the integrator's complete
applicable final-input matrix is recorded. `PUSH_COMPLETE=false`,
`REMOTE_CI_COMPLETE=false`, `PR_REVIEW_COMPLETE=false`, `MERGED=false` here;
publication receipts may record their later actual states without rewriting
this time-scoped report. Historical strict Compatibility02/03 remain
`NOT_COMPLETE`; hosted private storage remains `BLOCKED`.
