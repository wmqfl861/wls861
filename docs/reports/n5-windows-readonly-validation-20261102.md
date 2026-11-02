# N5 Windows read-only mechanism and image-source validation

Time scope: 2026-11-02 UTC, local execution through
`2026-11-02T11:54:53.534868+00:00`. Base:
`aa2c592b8aed6509189710cc6f6d69e0959098a8`; branch
`feat/n5-3-readonly-identity-20261102-csj777e9`.
This reports the assigned N5-3 source mechanism and N5-0 Windows source-conflict
slice. **Deployment policy remains POLICY_PENDING and complete N5-3 target
acceptance remains NOT_RUN.** It does not close the prior N0–N4 local sanitizer
failures, private hosted evidence gap, or Windows image discrepancy.

## Changes and scope

- `include/wls/native_readonly.hpp`, `src/readonly_internal.hpp`,
  `src/readonly_resources.cpp`, `src/readonly_windows_internal.hpp`, and
  `src/readonly_windows.cpp` under `platform/native-foundation` add an explicit
  policy, move-only root, separate Windows volume/128-bit object observation,
  same-HANDLE owner/DACL checks and immutable compiled-resource capture.
- `src/system_windows.cpp` reuses the original RAII reader, retained ancestor
  chain, no write/delete sharing, type/reparse/link checks and bounded EOF read.
  Only the new policy entry enables the added strict token/volume/ACL/alias
  observations. Ancestors and files are checked before and after capture.
- `src/internal.hpp` and `src/public_resources.cpp` share the unchanged compiled
  catalog, manifest hash and byte budgets between new and historical entrypoints.
  The old public reader and CLI do not acquire an implicit ACL-security claim.
- `tests/readonly_tests.cpp` adds one component CTest suite: three methods on
  Linux and seven on Windows. Windows-specific methods are actual SDK descriptor,
  temporary HANDLE, replacement barrier and impersonation checks, not simulated
  successful secure-root opens.
- `tools/windows_image_provenance.py` and its eight-method tool test compare
  saved same-job sources and preserve conflicts. The root executor integrated
  the corresponding CMake, CI, original method collector and its 16-method
  selfcheck changes through one reviewed patch.

The [implementation contract](../implementation/n5-windows-readonly-identity.md)
describes the policy, threat model, refusal rules and remaining target tests.
No accepted build/SID deployment policy is supplied. No default current-user,
SYSTEM or Administrators exception exists. No Store/VFS, arbitrary path reader,
Workspace write, process, sandbox, allocation or cleanup operation is implemented.
Published SQL, original resources, third_party, docs/handoff and historical
reports were not edited by this slice.

There was no prior ACL-secure Windows root API at the base, and no reproduced
production permission vulnerability is claimed. The new guard tests establish
mechanism behavior; inability to compile a new API against old headers is not
reported as a baseline defect. The image discrepancy is an existing real
observation and was rechecked against its original immutable inputs.

## Local execution and identities

Execution used ordinary `ubuntu`, UID/GID 1000, data disk task root
`/mnt/d/codex-workspace/wls861-n5-20261102-csj777e9`, and private
`tmp/windows` for temporary fixtures. Compiler: Ubuntu GCC 13.3.0
(`13.3.0-6ubuntu2~24.04.1`); CMake 3.28.3; Ninja Release; existing strict native
warning options remained active. No Windows SDK/toolchain installation or
host/global permission changes were performed.

The actual input was the dirty Windows branch based on `aa2`, including the
root executor's CMake/CI/method-collector integration. It contained **832 source
files**, recorded before and after the run. This new report is written after
execution and therefore is not claimed to be one of those execution inputs.
The source manifests match exactly, as do all **20 ELF/library/object/build
identity entries**. A later commit SHA is recorded by the external handoff;
this report does not invent a self-referential final SHA.

| Execution | Actual result |
| --- | --- |
| GCC 13 Release configure and build, `--parallel 2` | exit 0 |
| Full native CTest, umask 0022 | 5 Passed / 0 Failed / 0 Timeout / 0 Skipped / 0 NotRun; exit 0 |
| Full native CTest, umask 0002 | 5 Passed / 0 Failed / 0 Timeout / 0 Skipped / 0 NotRun; exit 0 |
| Original Windows runner tool selfcheck | 16 methods; exit 0 |
| New offline image provenance tool selfcheck | 8 methods; exit 0 |
| Small native validation collector | completed; actual outer exit 0 |
| Windows MSVC compilation and seven native readonly methods | NOT_RUN at this report's time scope |
| Win11 ordinary-user full root capture/security acceptance | NOT_RUN; POLICY_PENDING |
| New-slice Clang/ASan/LSan/UBSan and Linux full product matrix | NOT_RUN by this Windows subtask; root executor owns final integration validation |

The five native registrations are `native_modules`, `native_platform`,
`native_readonly`, `native_cli` and `native_bundle_capture`. Their deadlines are
30/30/30/60/30 seconds. The original four deadlines did not change. The Linux
product catalog stays 92; native suites and tool methods are not added to it.
Windows' reviewed native method catalog is now 34, including the seven new
methods, but a synthetic runner selfcheck is not evidence those methods executed
on Windows. Each Linux native run executed the three portable new methods.

The actual commands, full stdout/stderr, exit receipts, CTest JUnit and complete
original `Testing/Temporary` logs are retained under
`evidence/windows-native-linux-01` outside Git. Representative original argv:

```text
cmake -S /mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/windows/platform/native-foundation -B /mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/builds/windows-native-linux-release-01 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build /mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/builds/windows-native-linux-release-01 --parallel 2
ctest --test-dir /mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/builds/windows-native-linux-release-01 --output-on-failure --output-junit /mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/evidence/windows-native-linux-01/ctest-0022.xml
ctest --test-dir /mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/builds/windows-native-linux-release-01 --output-on-failure --output-junit /mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/evidence/windows-native-linux-01/ctest-0002.xml
/usr/bin/python3 /mnt/d/codex-workspace/wls861-n5-20261102-csj777e9/windows/platform/native-foundation/tools/test_windows_ci_evidence.py
```

The corresponding command receipts retain each actual umask, timeout, working
directory, interpreter, UTC start/end and environment overrides. The new
eight-method selfcheck has its own `evidence/windows-provenance-selfcheck-01`
command and raw output, exit 0, with 8 tests in 0.126 seconds.

| Frozen local record | SHA-256 |
| --- | --- |
| `windows-native-linux-01/summary.json` | `de90821e3f8f8e97f2cd0191c3aeed61a8546222651f73e9d06d85609d472a53` |
| `windows-native-linux-01/source-before.json` and `source-after.json` | `62bea34197e45aa78dcd2f03b573a1f02e7debe9d644039d8f4b40ca3f794901` |
| `windows-native-linux-01/binaries-before.json` and `binaries-after.json` | `a80e4b2cb6a0586c04fd9934a1decd86c2ea86f2676f373aa326b485680800a0` |
| `windows-provenance-selfcheck-01/receipt.json` | `644891efec4af693a1073d36fb3ee5c6a3d93afac16fd2a6313dd567bfa39fac` |

## Original Windows image conflict, still unresolved

The new tool was executed once against each original saved run, job, log,
artifact metadata and ZIP, using original successful GET receipts and hashes.
No network request, product execution, archive-code execution, Actions rerun or
old-file rewrite was performed. Both same-job bindings passed; both comparisons
actually exited **1** with **UNRESOLVED**. A wrapper's successful collection does
not change either audit exit code.

| Event / run / numeric job / attempt | Actual checkout | Header emission UTC | Environment capture UTC |
| --- | --- | --- | --- |
| push / [41416271880](https://github.com/wmqfl861/wls861/actions/runs/41416271880) / 125180768812 / 1 | `aa2c592b8aed6509189710cc6f6d69e0959098a8` | `2026-11-02T08:52:59.8866543Z` | `2026-11-02T08:53:11.278871+00:00` |
| pull_request / [41416279414](https://github.com/wmqfl861/wls861/actions/runs/41416279414) / 125180794615 / 1 | `ecc2d4b9b9ad1f49b84bbb813926912007c3046a` | `2026-11-02T08:53:11.3764958Z` | `2026-11-02T08:53:22.381319+00:00` |

Both original Runner Image groups state `Image: windows-2022` and
`Version: 20261025.346.1`. Both inherited process captures state `ImageOS=win22`
and `ImageVersion=20261026.347.1`. Header metadata comes from runner initialization
before checkout. Environment values remain workflow-controlled, although the
original artifact and structured output printed in the same job agree exactly.
Neither source is independent signed host attestation. The earlier/later times
do not explain the cause of the difference. **No causal evidence is available;
no preferred source or invented explanation is supplied.**

| Original file or new comparison | SHA-256 |
| --- | --- |
| Push original full job log | `82be17fae15e0167df4d5735491a40f8f6a52d5aa2078a8b921bd8198a669d31` |
| Push original environment member | `426b766c2ee1a9873cee5aaacc2303af03aef12cfc3dc61bd67d37a5677f7655` |
| PR original full job log | `85246e0fc75bca478c4edd657c294a26a4d405c58e68206644773d5f39d4cedc` |
| PR original environment member | `b387d16129f9cadb327779c87a681c32f087eedf462db5c21aaf88e3323e79ef` |
| New Push `comparison.json` | `afc6367678e99199f1403f14dcfd19e60f77c8fe6ca7e78cb8108bea86f35015` |
| New PR `comparison.json` | `007c4537c638fe06e761018d548dfea65849c8654f8e3854eac038b13c44ff00` |

New comparison inputs, exact commands, original stdout/stderr, exits and results
are retained in `evidence/windows-image-conflict-01/<run-id>/`. Each fresh result
also records API/log/archive download times, raw file/member hashes, original
header lines, numeric context and explicit source-trust limitations. Prior
Windows audit exit 1 records remain unchanged and valid. The original Server
2022/elevated success is not relabeled as Win11 or normal-user acceptance.

## Review, publication and remaining gates

The independent non-author Agent `/root/phase_observation_review` reviewed the
actual core paths read-only and reported no definite safety bypass in that
bounded review. It also checked the later constructor ancestor recheck and
CMake source wiring. Its final source-hash/commit binding is an external review
receipt, not this executor's own approval or a second-model claim.

The single temporary-leaf rename barrier checks a sharing property only. Full
root/ancestor path replacement, junction/reparse changes, second-user ACL
mutation, unsupported volume/alias behavior, complete root success/EOF/hash and
handle lifetime on an approved Win11 ordinary-user machine remain **NOT_RUN**.
The full root API cannot pass on hosted Server/elevated CI and must not be
weakened to manufacture such a pass. The coordinator must still adopt an exact
support/trusted-SID policy and decide how the target machine will be provided.

At this report's time scope:

| State | Value and scope |
| --- | --- |
| `IMPLEMENTATION_COMPLETE` | true for the assigned bounded source mechanism and comparison tool; complete N5-3 target acceptance remains pending |
| `LOCAL_VALIDATION_COMPLETE` | false; only the listed Linux component and tool checks completed |
| `PUSH_COMPLETE` | false; root executor owns publication of the subsequent commit |
| `REMOTE_CI_COMPLETE` | false for this new candidate; historical aa2 runs are source inputs only |
| `PR_REVIEW_COMPLETE` | false |
| `MERGED` | false |

No main/PR merge, production operation, real provider call or paid request was
performed or authorized by this work. Later integration/CI/machine results must
be recorded with their own input SHA and original evidence.
