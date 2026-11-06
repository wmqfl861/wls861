> **HISTORICAL PROJECTION (REDACTED) — NOT CURRENT ACCEPTANCE.**
> Source: `docs/reports/n5-compatibility-strict-20261104-v1.md` as it existed uncommitted in its
> worktree at head `efcd4547bd7627fa821a4b7bce097e633b514982` (branch `test/n5-compatibility-20261104-_7wxnkyn`); original 13867 bytes,
> SHA-256 `7bc3a8dce5632cf87015e69778a5439a39229a24e4a2e05f26ee1ca5e5a84cba`. Redacted public projection created 2026-11-06T10:21:09Z under
> `docs/reports/publication-20261106/`. This is a new version, not a byte copy
> of the original; the original is retained locally and is not in Git.
> Redaction scope: absolute local workspace/evidence-root paths, local task plan / version-record / plan-review references, internal session identifiers.
> No engineering result, status, failure, timestamp or six-state value was
> changed; PUSH/CI states remain the historical values of the report's date.
> **Adjacent correction notice (mandatory reading):** the v2 report in this same
directory (`n5-compatibility-strict-20261104-v2.{md,json}`) corrects this v1
report: C4's controlled-kill capability is NOT_ESTABLISHED and C4 is NOT_RUN
by design from the corrected runner on; v1's aggregate claims hold only with
the per-row / observed-sample scoping restated in v2. The v1 statements below
are preserved as history and are superseded where v2 corrects them.

# N5 strict compatibility observation and lifecycle matrix, 2026-11-04 (v1)

New bounded delivery for the frozen task `strict-compatibility-observer-and-lifecycle`
(produced under a frozen approved local task plan; plan/version-record and
plan-review references redacted).
It does **not** close Compatibility02/03 and does not relabel any historical result.
All private full evidence, fixtures, copies, traces and manifests are local under
`[task evidence root, redacted]`
(data disk, ext4, `rw,relatime`); this report is a projection,
not the originals.

## Scope, baseline and publication state

- Baseline HEAD `3a8725584fa5d8ea0f8fe3e108e0d4fbcb5ec123`, branch
  `test/n5-compatibility-20261104-_7wxnkyn`, worktree porcelain contained only the
  five new files of this delivery at the end (no tracked file modified).
- PR dependency unchanged from the FINAL plan: candidate PR32 depends on PR31
  `f15bf1a944a1ea5f6b9e5e9696b921caa48f04cc` and includes PR29
  `3888d95a53572e06825d774aae732d01f7996903`. The root agent must re-fetch and
  record exact remote heads before any publication; this report claims nothing
  about the current remote state.
- Registration: `IMPLEMENTATION_COMPLETE=true` (this direction's tools and
  evidence), `LOCAL_VALIDATION_COMPLETE=true` for the executed evidence only,
  `PUSH_COMPLETE=false`, `REMOTE_CI_COMPLETE=false`, `PR_REVIEW_COMPLETE=false`,
  `MERGED=false`. No Git write, commit, push or PR was performed by this task.

## Identity chain

| Identity | Value |
| --- | --- |
| Original 410 ELF | `[frozen 410 build artifact retained locally, path redacted]`, SHA-256 `b048a449ecdc22cd53ccf2813804bd36c720a26870732f03ebe9c066ed5d407e`, 4,733,672 bytes; re-verified before and after every use |
| Old source root | `[old source worktree retained locally, path redacted]` (its own fixtures were used for populated-410 construction) |
| Current 3a candidate build | cmake target `wls` at HEAD `3a872558…`, SHA-256 `fab236410c7e39cef14e5f18733aefe85b291a72cb5b30fc74726425d5bd9673`, 5,117,440 bytes, built out-of-tree in the evidence root |
| Interpreter | `/usr/bin/python3` → `/usr/bin/python3.12`, 3.12.3, file SHA-256 `e50d468e8b0adfb0…` (full hash in machine index) |
| SQLite | runtime 3.45.1; `/lib/x86_64-linux-gnu/libsqlite3.so.0` → `libsqlite3.so.0.8.6`, SHA-256 `85265a9d4afca6f4b325ceb078b669c754fb881abed4cafe91ccebe9d625d975` (ldd, holder `/proc/self/maps` and direct hash agree; see `records/identity-supplement.json`) |
| Execution user | UID 1000, umask `0077`, mount regime `rw,relatime` on ext4 recorded per observation |

## New evidence tools (not product, not CTest)

Three manually invoked Python tools; **no new CTest suite was registered**, and the
catalog/default/CI/selfcheck test names, deadlines and the 95-registration count are
unchanged. The existing nine-method suite ran under its original 20-second CLI bound.

| File | SHA-256 |
| --- | --- |
| `tests/store_compatibility_observer.py` | `50d8efa0c595c63c759f45318dbfd668537b5edc2e31f823c09b5e74bb0d47f6` |
| `tests/store_compatibility_observer_selfcheck.py` | `38fa9422d164a010ce21e3c40e287db6fff800d5530fe1861404ca01563f9999` |
| `scripts/run_store_compatibility_lifecycle.py` | `fa090d81eb5843e132dfa2c106c122541c194c7a932d6a1eb0e9878a1add2de7` |

The observer records full stat/statx (birth time, mount id) metadata, xattrs, atime/
mtime/ctime in nanoseconds and complete SHA-256 content for every database-prefix
member (main, `-wal`, `-shm`, `-journal`, `.writer-lock`, any unknown prefix member),
using `O_NOATIME|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK` read-only descriptors and an
`O_NOATIME` directory fd (`getdents64`), with an lstat→fd→EOF→fd/path identity chain.
Frozen copies are verified by an independent readback (size+SHA, exactly one mapping
per member) before any SQLite open; logical dumps then cover **every** real table
(76 tables in populated 410 fixtures, 82–83 in 411 fixtures) with explicit
null/int/real-bits/text/blob encodings, `PRAGMA` metadata and `quick_check`, and an
explicit close result. Originals are never opened with SQLite, never checkpointed
and never read `immutable=1`.

### Observer selfcheck (precondition for all product rows)

- First run: 17/22 pass, 5 fail — the failures exposed a real observer defect
  (member-path `realpath` resolution followed a symlink member and defeated
  `O_NOFOLLOW`) plus two selfcheck ordering bugs. All evidence retained
  (`records/observer-selfcheck.json`).
- Second run after fixes: **22/22 PASS** (`records/observer-selfcheck-v2.json`,
  0 fail, 0 not-established). Positive: observer-only stability including all
  timestamps; clean frozen-copy readback; the uniquely committed WAL row visible
  through copy semantics while absent from the copy's main bytes. Negatives:
  same-length copy byte change, truncation, missing member, swapped mapping,
  WAL/SHM/main same-length changes with restored times, inode replacement,
  sidecar add/remove, chmod, mtime-only, atime-only, same-mode `fchmod` ctime
  (actually hit on this kernel), symlink/non-regular/oversized member refusal,
  read-time identity-chain rejection (size/mtime/inode), WAL truncated mid-commit,
  WAL frame byte flip, WAL header flip (each proven to remove or error the
  committed fact, never a legally ignored tail), and recorded connection-close
  failure. Conditional atime control: `O_NOATIME` observation left atime
  untouched while a plain read updated it under `relatime`.

## Execution matrix (each row a fresh fixture, single execution, no retry to green)

Phases per the FINAL plan (PREPARED → WAL_READY → WRITER_QUIESCED → BEFORE →
OLD_OPEN → AFTER → WRITER_RESUME → POST_WRITE → HOLDER_CLOSE) with holder PID,
start ticks, fds, handshake sequence numbers and monotonic anchors recorded per
phase (`records/<row>-rN/row.json`).

| Row | Input and operation | Status | Key outcome |
| --- | --- | --- | --- |
| C0 | Online 411 fixture, holder quiesced, **no product operation** | **PASS** | Two-snapshot windows stable; originals byte+metadata identical before/after; frozen-copy readback equal; all-table logical dumps equal — observer non-perturbing |
| C1a | Static populated 411, old410 `pending` | STRICT_FAIL | Correct `unsupported` (exit 2); main bytes identical; **adds empty `-wal` + 32,768-byte `-shm`** (historical mechanism reproduced); main atime changed by the old program's own read; business rows identical |
| C1b | Static populated 411, old410 `init` | STRICT_FAIL | Correct `unsupported`; no net sidecar change (created then unlinked during the run); main atime **and ctime** changed (same-mode `fchmod`); writer-lock ctime changed; business rows identical |
| C2a | Online state 1 (main 411, non-empty committed WAL), old410 `pending` | STRICT_FAIL | Correct `unsupported`; main/WAL/SHM **content bytes identical**; only atime of WAL/SHM changed (old program read them); no sidecar inventory change; business rows identical |
| C2b | Online state 2 (main user_version **410**, committed 411 only in WAL), old410 `pending` | STRICT_FAIL | State constructed via the **real 013 migration** under a live keeper (`wls init` exit 0, main SHA unchanged at user_version 410, WAL 234,872 bytes, copy reads 411; migration connection was not the last, no explicit checkpoint); old410 refused `unsupported` from the WAL-visible version; content identical, atime deltas only; business rows identical |
| C3a | Online fixture, real Store lifecycle `flock` held, old410 `init` | STRICT_FAIL | Correct **`conflict`** before any user_version execution (not relabeled as a version refusal); main/WAL/SHM bytes and metadata identical; only writer-lock ctime changed by the old program's lock-file `fchmod`; business rows identical |
| C3b | Online fixture, no flock, old410 `init` (real path, its own column) | STRICT_FAIL | Correct `unsupported`; main/WAL/SHM content identical; main ctime (fchmod) and atime, WAL/SHM atime, lock ctime changed; sidecars **retained** (holder connection prevented the close-time unlink); business rows identical |
| C4receipt | Traced vs untraced old410 `init` refusal on isomorphic fixtures | **PASS** | `-shm` open visible in the streamed trace; classifications identical (exit 2, `unsupported`, same added/removed/changed sets, business equal) — tracing does not perturb the effect classification |
| C4 | Traced old410 `init`, controlled termination after SHM open | RECORDED | SIGSTOP 34 µs after the traced `-shm` open, stopped state confirmed, SIGKILL, exit −9, reaped; WAL/SHM **retained** (killed before cleanup unlink); main/WAL/SHM content identical (main ctime already changed by the pre-SQLite `fchmod`); business rows identical; process kill never classified as `unsupported` |
| C5 | One authorized holder commit inside the old program's presence window | RECORDED_OK | Commit monotonic timestamp provably between child start and exit (SIGSTOP-anchored rendezvous); old410 still refused `unsupported`; product business rows unchanged; only the one authorized `compat_controlled_facts` increment (1→2); shared-file changes retained and **no raw-equality claim made** for this row |
| C7a | Old410 on its own populated legal 410 (2 requests, texts, tasks, events; 33 non-empty tables) | **SUPPORT_CONTROL_PASS** | `pending` exit 0, business rows identical — binary, source root and fixtures proven usable, so the refusal rows above are not resource failures |
| C7b | Current build on populated 411 (34 non-empty tables) | **SUPPORT_CONTROL_PASS** | `pending` exit 0, business rows identical |
| C6 | Existing nine-method suite at the current SHA | **SUITE_PASS** | Single execution, exit 0, `Ran 9 tests … OK`, all nine methods ok (see deviation note for the parsing correction) |

Denominators: 22 selfcheck cases; 12 matrix executions (C0, C1a, C1b, C2a, C2b,
C3a, C3b, C4receipt, C4, C5, C7a, C7b) plus the nine-method suite; zero product
CTest registrations added; catalog remains 95 with the store-open compatibility
120-second suite and its 20-second CLI bound unchanged.

## Overall strict-compatibility determination

`STRICT_COMPATIBILITY=BLOCKED/NOT_COMPLETE`, on honest grounds and with a precise,
reproducible mechanism: every necessary old-410 row produced the correct refusal
(`unsupported`, and `conflict` under a held lifecycle lock) and preserved **all
business rows and all main/WAL/SHM content bytes**, but the frozen old ELF always
leaves metadata deltas — atime through its own normal reads under `relatime`,
ctime through same-mode `fchmod` on the main database and writer lock — and on
static fixtures it creates/removes WAL and SHM sidecars around its refusal. The
strict raw+metadata equality required by Compatibility02/03 is therefore not met
by that frozen program, and no current-product minimal fix can change it (the
current candidate already passes all nine methods at SHA `fab23641…`, including
the 40-line header preflight). Per the FINAL plan this is delivered as a
verifiable BLOCKED, not as a compatibility PASS; expectedFailure, skip, time
restoration, mount changes or field removal were not used to obtain green rows.

## Deviations, defects and their handling

1. Two infrastructure aborts before any product execution (runner reused a
   run-id, whose holder file is anti-overwrite): recorded in `tmp/*.stdout`;
   no matrix row had started.
2. Observer selfcheck first run failed 5 cases (real symlink-following defect
   via `realpath` in the containment check, plus two case-ordering bugs); fixed
   and re-run to 22/22. First-run evidence retained.
3. C6 verdict parsing initially inspected stdout only (unittest reports on
   stderr) and recorded `method_count 0` although the suite itself passed. The
   suite was **not** re-run: the corrected verdict derives from the same
   captured bytes (`records/c6-r5/row-corrected.json`, stderr SHA-256
   `54afa6ea4d203f5c6ddcb6afa097a74116c150a25afa9933208e6de52cc2382b`), and the
   runner's parser was fixed afterwards.
4. C2 was executed once per online state (state 1 and state 2) as sub-rows
   C2a/C2b, since the FINAL plan establishes both states as fixtures of the
   non-empty-WAL class.
5. C4/C5 used SIGSTOP positioning (never any ELF modification or error-code
   injection); all rendezvous timestamps and stop/kill evidence are in the row
   records and raw traces.
6. The environment auto-capture of `libsqlite3` maps ran before any SQLite open
   and returned an empty list; identity is established in
   `records/identity-supplement.json` instead (recorded as a tool limitation,
   not silently dropped).

## Limitations and end states

- Phase snapshots prove sampled boundaries only; the raw traces bound the C4/C5
  windows but do not prove the absence of unsampled instantaneous writes.
  Observer equality claims are therefore scoped to the frozen windows and
  copies actually verified.
- No independent engineering review has examined this delivery yet; the executor's
  own evidence is execution evidence, not acceptance. Hard review by a fresh
  session remains required before any publication step by the root agent.
- Windows 11 ordinary-user NTFS acceptance: `NOT_RUN`. Restricted private hosting
  of original artifacts: `BLOCKED` (originals retained locally).
  `historical_clang_asan_failures_closed=false` remains untouched.
  N5-2 items 014/412 remain unauthorized; no new compatibility mechanism, VFS,
  WAL parser, migration or second production writer was introduced.
- Machine index: `docs/reports/n5-compatibility-strict-20261104-v1.json` and the
  fuller local `records/machine-index.json` (190 inventoried evidence files).
