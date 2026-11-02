# N5 Windows provenance identity correction

This report records the follow-up to diagnostic commit
`8d4bf5894527d2083b28caa69a00dfa9507d758d` on
`feat/n5-3-readonly-identity-20261102-csj777e9`, Draft PR #17. Native Windows
observations identify a real Python stat-source discrepancy. The correction
retains change detection using comparable native metadata. At this report's
pre-publication cutoff, the corrected Python reader has passed Linux selfchecks;
its new native Windows run and the Windows C++ build remain unverified.

## Original native evidence

The unchanged reader in diagnostic commit `8d4bf58` still fails. Both original
workflow runs completed with failure before the C++ build step:

| Event | Run / job / attempt | Actual checkout from original log |
| --- | --- | --- |
| push | [41444791925](https://github.com/wmqfl861/wls861/actions/runs/41444791925) / 125275111729 / 1 | `8d4bf5894527d2083b28caa69a00dfa9507d758d` |
| pull_request | [41444800533](https://github.com/wmqfl861/wls861/actions/runs/41444800533) / 125275141061 / 1 | `3908c0818f797cb4f106905425f76eff0a204cca` |

Both runs report branch head `8d4bf58`; the PR merge checkout above is distinct.
Read-only GET captures, original full job logs, their receipts, and extracted
probe objects are preserved outside Git in
`evidence/windows-stat-native-results-01`. Original log SHA-256 values:

- push: `f46d733bb286db0472a15c00ab658592e3ad9ef074b8458765c6fc25ad02de74`
- PR: `cb689c6c5dafa3b755928afb577820670a5292b19c2ccb7184392d1b605d9abd`

Both native jobs used Python 3.12.10, 64-bit pointers, Windows build 20348 and
product type 3. In the PR job's `explicit_ns_timestamp` synthetic file, the
four Python observations and two same-HANDLE native observations show:

| Observation | Exact value |
| --- | --- |
| path-before and path-after `st_ctime_ns` | `1793622456565738000` |
| descriptor-before and descriptor-after `st_ctime_ns` | `1793622456581354300` |
| all four `st_birthtime_ns` | `1793622456565738000` |
| all four `st_mtime_ns` | `1700000000123456700` |
| all four device / inode / size / links | `3488864977864881806` / `3659174698253848` / `37` / `1` |
| native CreationTime, unchanged | `134380960565657380` |
| native ChangeTime, unchanged | `134380960565813543` |
| native LastWriteTime, unchanged | `133444736001234567` |
| native volume serial, unchanged | `3488864977864881806` |
| native full 128-bit file ID, unchanged | `187e0f0000000d000000000000000000` |

Converting the native 100 ns times to Unix ns exactly matches path `ctime` to
CreationTime and descriptor `ctime` to ChangeTime. The object and each source's
before/after observations are stable; the previous cross-source ctime equality
rejects this stable file. This is an observed behavior of this Python/Windows
combination, not a claim that every supported Python release maps ctime this way.

The earlier `540fd7d` failures, the diagnostic report, and their original
captures remain unchanged. The separate prior runner-image version conflict
also remains UNRESOLVED; repairing the file reader cannot resolve that conflict.

## Correction and retained boundaries

Only `platform/native-foundation/tools/windows_image_provenance.py` and its
existing selfcheck gain executable changes. The Linux reader keeps cross-source
device/inode/size/mtime/ctime equality and adds mode/link stability. On Windows:

1. A type/size/link preflight precedes a metadata-only native path open. Native
   path observation uses `FILE_FLAG_OPEN_REPARSE_POINT`, checks disk type, rejects
   reparse/directory/deletion-pending objects and requires one link. Missing
   native information raises an error; no fallback weakens the comparison.
2. Native FILE_ID_INFO, FILE_BASIC_INFO and FILE_STANDARD_INFO are observed before
   the bound path lstat, on the opened CRT descriptor, after its read, and on the
   final path. All four must agree on the full volume/file ID, length,
   CreationTime, LastWriteTime, ChangeTime, attributes and link count. Last access
   is excluded because reading can change it. Temporary native handles close on
   every path; the CRT descriptor remains owned by its original stream. The
   native-before/opened comparison also runs before the first content read;
   an already-observed change refuses before reading bytes.
3. Python path-before/path-after tuples still compare ctime, as do descriptor
   before/after tuples. Comparable Python device/inode/size/mtime/type/link fields
   also match across path and descriptor. Exact bounded read length and SHA-256
   reporting remain in place.

Using birthtime alone in place of cross-source ctime would miss a same-length
write between the path observation and open if the writer restored mtime. The
additional native ChangeTime observation explicitly covers that interval. The
reader does not retry on mismatch and does not grant a writer lease. These
checks concern saved offline evidence; they are not Windows sandbox acceptance
or cryptographic runner-image attestation.

## Regressions and actual local execution

The original eight `SourceComparison` methods remain byte-for-byte unchanged.
Four additional methods operate on isolated real temporary files:

- stable bytes with explicit historical write time; native Windows additionally
  sets distinct actual CreationTime/ChangeTime and verifies their values;
- real same-length rewrite after bound path observation but before `os.open`,
  restoring mtime and proving that actual change time differs before expecting
  reader refusal;
- the same real rewrite after the first descriptor observation;
- same-byte, same-mtime replacement after descriptor close, with a verified
  different inode and required refusal.

Test hooks only choose the interleaving of real file operations. They return
the real descriptor/stat objects; no security metadata or read result is forged.
Windows fixtures use SetFileInformationByHandle on their own files to establish
and verify a distinct ChangeTime. Both rewrite methods additionally observe and
require zero reader content-read calls on Windows. Native setters and refusals still
require execution on Windows; Linux does not stand in for that result.

The first Linux run (`evidence/windows-stat-fix-01`) exited 1: ten methods passed,
and the two rewrite fixtures failed their prerequisite `ctime changed` assertion
because adjacent ext4 writes occupied the same kernel timestamp tick. This was
not reported as a product counterexample. The original failure remains intact.
The Linux-only fixture now waits 20 ms before its single injected rewrite and
still requires the real ctime to change; failure to establish that fault fails
the test. No original assertion, test deadline or reader timeout was changed.

The corrected fixture ran once at 2026-11-02T12:44:08.263131+00:00 in
`windows-stat-fix-02`: exit 0, 12 methods, 0.303 seconds. Non-author `/root`
then read the complete two-file diff and requested the pre-content-read
comparison described above. After this actual source change, one final local
invocation started at 2026-11-02T12:48:05.651304+00:00:

```sh
TMPDIR=/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/tmp/windows-stat-fix-03 \
  /usr/bin/python3 -B platform/native-foundation/tools/test_windows_image_provenance.py
```

Result: actual exit 0, 12 methods in one unittest invocation, 0.300 seconds;
outer capture 1.108 seconds. UID/GID 1000, existing 30-second capture bound.
Python syntax checks pass. Source hashes before/after agree. Original stdout,
stderr, exact argv/cwd and hash receipt are preserved in
`evidence/windows-stat-fix-03`. This is one tool selfcheck, not 12 product suites.
No local C++ build, sanitizer or product matrix was rerun for this Python change.

Executed reader SHA-256:
`ff1fe356c053108f41e7aebdc953945ca3483c68392a5f4c32d3bc6c7ac06a0c`.
Executed selfcheck SHA-256:
`517732b5629f8e621cb852d2daf4727c4e1c3a6c890c42f9ec747dd1aa027bfb`.

## Publication scope and remaining validation

Immediately before preparing this follow-up, remote main remained
`a6b8559018a599d7a806b05942857a56d69a00ce` and the remote feature branch remained
`8d4bf5894527d2083b28caa69a00dfa9507d758d`. Final commit, push/ref identity,
non-author review and that commit's actual CI are recorded in a separate
post-publication evidence receipt to avoid a self-referential commit.
The non-author review covers these Python sources and actual Linux selfcheck
evidence; it is not Windows 11 acceptance or a formal GitHub PR approval.

Native Windows corrected-reader execution is pending at this cutoff. The
native C++ build/test, complete Windows 11 ordinary-user ancestry acceptance,
and deployment SID/build approval remain separate open gates. In particular,
Windows 11 acceptance is NOT_RUN and the deployment policy is POLICY_PENDING.
No old report, SQL, third-party byte, handoff archive, deadline, product test
catalog, C++ source or n5-0 worktree is changed by this correction.
