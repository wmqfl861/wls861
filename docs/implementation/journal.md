# M1 Linux durable journal

Revision note: 0.1.1-m1 keeps the existing journal bytes and transaction layout.
The v0.4.1 SQL schema is a future Store contract and a Python-tested reference;
the current C++ TaskEngine has not switched to SQLite. A Store migration must
preserve the identity, atomicity, fencing and replay guarantees described here.

`DurableJournal` stores one logical transaction per frame. It is a single-host,
Linux implementation for bounded synthetic M1 fixtures. Transactions are opaque
byte strings; empty strings and embedded NUL bytes are allowed. The caller creates
the containing directory. The journal creates only its named file.

## Version 1 wire format

Frames are concatenated with no padding or separate file header. All integers
use little-endian byte order; no C++ structure layout is serialized.

| Byte offset | Length | Field |
| --- | --- | --- |
| 0 | 8 | Magic bytes `57 4c 53 4a 4e 4c 0d 0a` (`WLSJNL\r\n`) |
| 8 | 2 | Format version, exactly `1` |
| 10 | 2 | Reserved flags, exactly `0` |
| 12 | 4 | Payload byte length, from 0 through 16,777,216 inclusive |
| 16 | 4 | CRC32C of the payload |
| 20 | 4 | CRC32C of header bytes 0 through 19 |
| 24 | payload length | Opaque transaction bytes |

CRC32C uses the reflected Castagnoli polynomial `0x82f63b78`, initial state
`0xffffffff`, and final XOR `0xffffffff`. An empty payload has checksum zero;
the standard byte string `123456789` has checksum `0xe3069283`.
The separate header checksum prevents an ordinary corrupt length field from
turning a complete damaged frame into an apparently incomplete tail.

There is no migration: unknown versions, nonzero flags, invalid magic, abnormal
lengths, or invalid checksums fail closed. CRC32C detects accidental corruption;
it provides no authentication against an actor able to rewrite the file.

## Durability and recovery

`append()` serializes callers with an internal mutex. It allocates its replay
cache entry first, writes the entire frame with checked positional writes, and
calls `fdatasync()` before returning success. Short writes and interrupted
syscalls are handled. Once any append throws, the object rejects later appends
and replay calls; callers must close it and recover with a new instance. An
exception from the append does not establish that its frame was absent: a
complete frame can have reached storage even when a later operation failed.
Applications need their own transaction identities and reconciliation.

Opening obtains an exclusive, nonblocking `flock` and reads each complete frame.
The returned replay is an ordered snapshot of verified transactions. A partial
final header or a final payload shorter than its validated declared length is
truncated to the last complete frame. Available fixed header bytes and any fully
available length field must still be structurally valid; arbitrary trailing junk
is not silently discarded. A complete invalid header or payload causes failure
without truncation. The repaired file is synced before opening succeeds.

Every successful open also syncs the file and its pinned parent directory. This
makes a newly created directory entry durable and covers a concurrent creator /
open-existing race. The implementation does not create or durably establish
ancestor directories. File and directory synchronization errors are reported.

New files use mode `0600` (further restricted by the caller's umask). Descriptors
use `O_CLOEXEC`, the final file is opened with `O_NOFOLLOW`, and the pinned parent
directory's final component also rejects symlinks. Only regular files are
accepted. Existing permission bits are preserved. Earlier ancestor components
and directory ownership remain the trusted host's responsibility.

The lock lasts until the owning descriptor closes, including process death. The
destructor closes it without an explicit unlock, so destruction of a descriptor
inherited by a child cannot unlock the parent's file. An inherited journal
object rejects calls in the child by checking its original PID before locking
its mutex. Forking a multithreaded C++ process still requires the usual host
discipline: the child must exec before invoking general C++ code.

## Boundaries

Calls to `append()` and `replay()` may run concurrently in one process. Destroying
the object concurrently with either method is unsupported. The file lock is
advisory and assumes cooperating runtimes and a local filesystem with working
Linux lock and synchronization semantics. An administrator or unrelated process
can still replace or modify the pathname. The caller must keep the directory
and file identity stable while the journal is in use.

The maximum payload is 16 MiB; total history and the in-memory replay cache grow
with the number of transactions. M1 supplies no compaction, retention, database,
distributed locking, encryption, secure deletion, or crash-consistent snapshots.
OS-reported sync success is the durability boundary; physical power loss,
storage-controller behavior, and filesystem failures require separate system
testing. A checksum cannot detect a complete, correctly checksummed frame being
removed, reordered, or maliciously rewritten outside the journal API.

## Module tests

`tests/journal_tests.cpp` contains standalone regression cases for the on-disk
format, binary and empty transactions, every byte position of a torn final frame,
complete corruption, oversized lengths, exclusive writers, inherited handles,
recovery after a child is killed with `SIGKILL`, real partial-write failure using
`RLIMIT_FSIZE`, the maximum payload boundary, symlink/type rejection, and
concurrent append/replay. Run these through `make test`; actual execution results
belong in the separate implementation report.
