# Future Store header refusal: execution-side scope review, 2026-11-03

Reviewed input: production baseline
`c2a5cea64aeea261b214cdfd9a968b53f491110a`, with only the 40-line addition to
`src/store.cpp` consisting of `reject_future_database_header()` and its one
constructor call. The reviewed candidate file SHA-256 is
`8725b78b2c52d248a6cec0a539428b6a5a718f27a2244e3a31acddbf3519e1f6`.
This is an uncommitted-input review, not a final-SHA product test or approval.

Reviewer: local execution sub-agent `/root/compatibility_sidecars`. This agent
supplied the initial helper suggestion. Consequently this review is an explicit
execution-side safety and scope check, **not non-author independent review**.
Another non-author reviewer and the main coordinator must assess the final
input before any acceptance claim. The local executor grants no merge approval.

## Source scope and preserved gates

A read-only in-memory comparison removed exactly the added helper and its single
call from the candidate source, then compared the remaining complete bytes with
the baseline Git blob. The bytes matched exactly, including all pre-existing
constructor, transaction, authority, recovery and close code. The private receipt
is `evidence/header-preflight-scope-review-01.json`. No SQL, migration, public
header or resource asset was changed by this production difference.

The preflight performs no Store transaction or business action. It opens the
resolved leaf read-only with `O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK`, checks that the
opened object is regular, and reads at most 100 bytes with positional reads.
Interrupted reads retry; every successful descriptor is closed on both normal
and exception paths. It decodes the four header-version bytes as an unsigned
big-endian value only when the complete header and SQLite magic are present.
A value greater than the existing 411 upper bound refuses before SQLite open,
writer-lock creation and database chmod. Missing files, incomplete headers,
invalid magic and supported header versions fall through to the existing
constructor checks; none is accepted by the preflight.

The unchanged subsequent code still enforces:

- The declared recovery scope and durable-path preconditions; OneRequest cannot
  create or initialize a missing/empty target or perform populated upgrade.
- For a takeover, a private, current-UID, single-link writer-lock file and
  `flock(LOCK_EX | LOCK_NB)`, followed by the original regular/single-link/owner
  checks on the database. The helper does not issue a writer lease.
- Real `sqlite3_open_v2` with `SQLITE_OPEN_NOFOLLOW`, original read-only/write
  flags, the same busy timeout and length bounds, foreign keys and FULL sync.
- Actual SQLite `PRAGMA user_version` between 401 and 411, original base
  contract metadata, and every distributed migration hash/metadata check.
- Original migration/recovery `BEGIN IMMEDIATE` transactions, constructor
  exception cleanup, SQLite close and inherited lease-descriptor handling.
- Original StoreActor session, role, request/team ownership, epoch and CAS
  authorization. The added code does not create or renew any Actor or claim.

This establishes textual preservation of the gates; it does not replace their
applicable regression execution. The original nine-method regression already
demonstrated three assertion failures on the unchanged c2 ELF, as recorded in
[the sidecar report](../reports/n5-review-sidecar-refusal-20261103.md). The same
frozen test bytes must run on the new candidate before green is claimed.

## TOCTOU and compatibility limits

The helper pins its own leaf descriptor; it does **not** pin that same object
through the later independent SQLite open. It does not add a pinned ancestor
directory chain or prevent a trusted-host peer from changing the file/header
between the pre-read and SQLite open. A concurrent rewrite may lead to an early
conservative refusal or let the path reach the existing real SQLite guards.
It cannot turn a supported header into permission, because real version,
metadata, ownership, writer lock and recovery validation still run. It also
cannot promise no SQLite sidecar effects for every racing or active-WAL input.

`O_NOFOLLOW` covers the opened leaf; it is not a new guarantee against ancestor
replacement. The existing canonical-parent/path boundary is unchanged. The
helper checks type after opening with read-only/nonblocking flags and performs
no header read on non-regular objects. It is not a reusable fully pinned resource
capture API, a hostile-host sandbox or a whole-file integrity proof. File reads
can update access time; this work claims preservation of content, identity,
permissions and sidecar inventory in the tested refusal fixtures, not a universal
absence of all filesystem metadata activity.

A valid SQLite main file can still contain an older version while a newer
committed `user_version` resides in WAL. For that input the helper must fall
through, and the real SQLite version guard must reject it. The existing test
`test_main_header_never_authorizes_newer_committed_wal` checks this and unchanged
main/WAL bytes. It deliberately makes no claim about immutable SHM coordination
bytes while another SQLite connection remains open.

The helper uses the same existing 411 upper bound; it does not implement 014 or
Store 412. It cannot change the already-built old 410 executable. Historical
Compatibility02/03 raw sidecar failures therefore remain open under their
original inputs even if the updated program's future-header refusal passes.

## Review result and next evidence

No authorization or lock bypass was found within this 40-line difference.
The mechanism addresses the observed quiescent future-header refusal path and
keeps actual SQLite validation authoritative. The limitations above prohibit
describing it as universal strict compatibility or a fix to the frozen old ELF.

Required next evidence is the same frozen nine-method suite on the candidate,
all applicable full Linux gates on the final input, and a non-author source
review. At this review's time scope candidate product validation, final-SHA CI,
formal coordinator review and merge are all pending. Existing sanitizer,
Compatibility02/03 and Windows target-machine statuses are unchanged.
