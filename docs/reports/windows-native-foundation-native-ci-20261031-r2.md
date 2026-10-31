# A5 native Windows CI validation, revision 2

Observed on 2026-10-31. The native build/format slice passed its actual hosted
Windows Server 2022 Push and PR workflows for
`594a916a7a96d77cf8e354a3bbd1d0882250ff4e`. Both runs completed configuration,
Release compilation, all four CTest processes, installation and detached Unicode
relocation. This report preserves the preceding configuration failures and adds
execution evidence to the [initial report](windows-native-foundation-validation-20261031.md).
Windows 11 and ordinary-user security acceptance remain **NOT_RUN**.

## Source and task scope

- Original task baseline: `772feaee62086ddb0e7f605a7472721d3bc61406`.
- Initial native slice: `6d49be2801c05036c7af9afc5967d3f4b4dea6d0`.
- Corrected and executed source: `594a916a7a96d77cf8e354a3bbd1d0882250ff4e`.
- Executed tree: `7b1995d5eb0c2b603ceb5b3e097b11c2b2505035`.
- Branch: `feat/windows-native-foundation-20261031`.
- Draft PR: <https://github.com/wmqfl861/wls861/pull/5>, based on
  `fix/r2-4-gcc-asan-alignment`.
- Remote `main` was fetched and remained
  `a6b8559018a599d7a806b05942857a56d69a00ce` before this evidence update.

The scope remains the isolated native configuration, grant-calculation,
in-process scheduler, Unicode and public-resource foundation. Runtime
`0.11.1-r3.3c-audit`, Store 410 and blueprint v0.4.1 are unchanged. No Store,
Workspace, process, sandbox, cleanup or provider implementation is added here.
No production data, real model or paid API was used.

## Original failure and minimal correction

The initial source failed native configuration in both actual workflows:

| Event | Run | Source head | Actual result |
| --- | --- | --- | --- |
| Push | [41181837012](https://github.com/wmqfl861/wls861/actions/runs/41181837012) | `6d49be2801c05036c7af9afc5967d3f4b4dea6d0` | Configure exit 1; workflow failure |
| PR | [41182014616](https://github.com/wmqfl861/wls861/actions/runs/41182014616) | `6d49be2801c05036c7af9afc5967d3f4b4dea6d0` | Configure exit 1; workflow failure |

Both logs show MSVC `19.44.35229.0` and the pinned compiler directory, followed
by the combined `Initial Windows build set` CMake guard failure. The original
guard incorrectly required `CMAKE_VS_PLATFORM_TOOLSET_VERSION` to equal the
three-component compiler-directory version. CMake documents that variable as
the MSBuild `.props` selector and converts three-component requests; it is not
a reliable directory-version identity. The successful corrected runs record
an empty `.props` selector alongside the actual pinned compiler path. The first
combined failure did not report every predicate separately.

The coordinator's minimal correction in `594a916...`:

- Retains the MSVC, VS 2022, x64 pointer width, exact toolset-directory and exact
  SDK requirements; checks the explicitly requested toolset and actual compiler
  path instead of treating the `.props` selector as a compiler identity.
- Requires CMake 3.27+ and passes `-A x64,version=10.0.26100.0` to select the exact
  SDK. Under CMP0149 NEW, `CMAKE_SYSTEM_VERSION` alone does not select that SDK.
- Passes `-T v143,host=x64,version=14.44.35207` and retains the explicit
  `CMAKE_GENERATOR_INSTANCE` selected by `vswhere`.
- Writes actual toolchain identity before the custom guard and preserves the
  generated identity/cache on configuration failure when those files exist.

No timeout or assertion was relaxed. The subsequent native runs required no
additional source fix. The initial failure logs and both artifact ZIPs remain
preserved in the task evidence; they were not replaced by successful reruns.

## Actual native environment and commands

Both successful runs recorded Windows Server 2022 build `20348`, AMD64, NTFS,
runner image `win22` / `20261026.347.1`, VS installation `17.14.37824.6`,
MSVC `19.44.35229.0`, toolset directory `14.44.35207`, SDK `10.0.26100.0`,
CMake `3.31.6` and Python `3.12.10`. The runtime reported token elevation as
`elevated`. No ordinary-user Windows security acceptance is inferred from this
hosted build.

The native driver recorded each argv, cwd, start/end and exit status. The
following are the actual configure/build/test/install commands, shown using
PowerShell quoting. Their cwd was `D:\a\wls861\wls861\native-work`:

```powershell
cmake -S "D:\a\wls861\wls861\source\platform\native-foundation" `
  -B "D:\a\wls861\wls861\native-work\build" `
  -G "Visual Studio 17 2022" -A "x64,version=10.0.26100.0" `
  -T "v143,host=x64,version=14.44.35207" `
  "-DCMAKE_GENERATOR_INSTANCE=C:\Program Files\Microsoft Visual Studio\2022\Enterprise" `
  -DCMAKE_SYSTEM_VERSION=10.0.26100.0 `
  "-DPython3_EXECUTABLE=C:\hostedtoolcache\windows\Python\3.12.10\x64\python.exe"
cmake --build "D:\a\wls861\wls861\native-work\build" --config Release --parallel 2
ctest --test-dir "D:\a\wls861\wls861\native-work\build" -C Release --output-on-failure `
  --output-junit "D:\a\wls861\wls861\native-work\evidence\ctest.xml"
cmake --install "D:\a\wls861\wls861\native-work\build" --config Release `
  --prefix "D:\a\wls861\wls861\native-work\installed"
```

All four commands exited 0 in each run. The driver then renamed the installed
tree to `relocated 无量山 🏔`, moved the original source directory away, and ran
`verify_detached.py` against that relocated executable. That command also exited
0. Its original JSON confirms source-path absence, exact public resources,
original configuration resolution and nonzero unsupported-business refusal.
The ten recorded driver commands in each successful artifact all exited 0;
the full workflow conclusion was independently queried as `success`.

## Executed native results

| CTest process | Push 41182761180 | PR 41182763977 |
| --- | --- | --- |
| `native_modules` | Passed | Passed |
| `native_platform` | Passed | Passed |
| `native_cli` | Passed | Passed |
| `native_bundle_capture` | Passed | Passed |

These are four CTest processes repeated in two workflows. Their internal cases
are not additional independent suites. Both JUnit files contain the same four
executed names with no failures. Push CTest elapsed time was 0.92 seconds; PR
CTest elapsed time was 1.49 seconds.

The [Push workflow 41182761180](https://github.com/wmqfl861/wls861/actions/runs/41182761180)
completed successfully at `2026-10-31T08:00:36Z`. The
[PR workflow 41182763977](https://github.com/wmqfl861/wls861/actions/runs/41182763977)
completed successfully at `2026-10-31T08:01:35Z`. Both original logs and artifacts
were downloaded after completion and inspected, including `ctest.xml`,
`ctest.stdout`, `detached.stdout`, command receipts and toolchain identities.
The CI script starting or exiting 0 was not used as a substitute for those
individual results.

Each detached run verified all 35 original public assets, 160549 bytes, with
manifest SHA-256
`b9f5468c6858ebd467fb4e6905d2ef4b8bd51a006e4fd2a636f8e4db1fde55cf`.
Store/Workspace/process/sandbox/cleanup remained unsupported; model calls
remained denied; network isolation and ordinary-user security remained
`not_checked`.

## Original-source and binary identities

The Push checkout was exactly `594a916a7a96d77cf8e354a3bbd1d0882250ff4e`.
The PR checkout was synthetic merge
`c2c81e54039906b30eed4de0b6a3c906a7036dd4`, whose parents are the task baseline
`772feaee...` and `594a916...`. GitHub's commit API reports the same executed
tree `7b1995d5eb0c2b603ceb5b3e097b11c2b2505035`.

For each run, all 737 tracked files, 27250048 bytes, were independently
rehashed and compared with the corrected commit's Git blobs. Both native
`source-inputs-before.json` and `source-inputs-after.json` matched every expected
path, byte count and SHA-256. The canonical sorted `sha256 + two spaces + path +
newline` manifest SHA-256 is
`9385d72c41035edac839fc00a4d8cffb48f74abe06c18f3e304b37ed534ccfb4`.

| Built binary | Push SHA-256 | PR SHA-256 |
| --- | --- | --- |
| `native_modules_tests.exe` | `d34c3ad8eef7e740416d684c98b82b616b5f136b29913c97cddec46605622f76` | `2d365cbdfc56cadd7bada67713f392267962af8e76c5205a1495da4e2f3d3bd7` |
| `native_platform_tests.exe` | `dd66dbdd56b38a62e5903d84b876e2cd66f3d90674152be73d29156d7d277cf3` | `9ba46d418830b8f1e23601f5ee310b1a92345b24f9392a4f0bed85f1d53a6271` |
| `wls-native-foundation.exe` | `d06640fbb1fa7e61b841c331dd84ff0cd3a1debce8247c28a9d95d13c6b23b9f` | `608fa90595bbb2609c0605aef230b10760de10ac47b25489d6d7ccef68f668ff` |

Each run's binary identities are preserved separately. No reproducible-binary
equality between these independent builds is claimed.

## Evidence inventory and review

The task evidence root on the data disk is
`/mnt/d/codex-workspace/wls861-next-20261031-065247/evidence/`:

| Evidence | Location / SHA-256 |
| --- | --- |
| Original failed logs / first artifact | `a5-native-01/` |
| Independently checked failed run/artifact metadata, including both ZIPs | `a5-native-01-supplement/verified-original-failures.json` |
| Push success artifact 13035154272 | `a5-native-02/41182761180/`, ZIP SHA-256 `2fa4dd74ec3fe9bd5ea8a9ad049a52f3e938fb247a6508bc426c538dd4c2ecc2` |
| PR success artifact 13034979178 | `a5-native-02/41182763977/`, ZIP SHA-256 `562cb1afadb65029d943f61f1998e8f75e28e0728e55acef469be60ca2af29a3` |
| Full source/result verification receipt | `a5-native-02/verified-inputs-and-results.json`, SHA-256 `c771deb4dceae8f47738b9f1270d0991759c7c96a2307c39fa4bc3d3f167c4fc` |
| Success evidence manifest, 112 files | `a5-native-02/evidence-manifest.sha256`, SHA-256 `6ea9fe01e152f9321862d29bcca3140196566a93f54538017c477301a26b5264` |

Both success artifacts contain 40 text/JSON/XML members. Downloaded ZIP hashes
match GitHub's advertised SHA-256 digests. Original commands, exits, logs,
member hashes, before/after input lists, generated CMake identities, host
observations and binary hashes are retained. No archived program was executed
to inspect this evidence.

The initial A5 non-author engineering review is
`evidence/a5-independent-review.json` by `/root/evidence_audit`. It found no
blocker after the generator-instance correction while explicitly leaving native
execution open at its own observation time. The later toolchain source receipt
is `evidence/a5-toolchain-minimal-fix-source-review.json`. Its reviewer authored
the initial A5 slice and therefore does not replace the whole-module non-author
review. These receipts are engineering evidence, not coordinator acceptance or
a different-model evaluation.

This document has a fixed source/time scope. A later documentation commit does
not acquire the earlier SHA's CI status automatically. Its actual pushed SHA,
remote branch identity and native CI must be recorded separately in an external
post-publication receipt, avoiding a self-referential report update.

## Remaining boundaries and truthful status

- Native Windows Server build/format CI for `594a916...`: **COMPLETE**, based on
  both actual completed workflows and their original test/artifact evidence.
- Dedicated branch publication for that SHA: **PUSH_COMPLETE**.
- This executor started no local compiler or test rerun during this native CI
  continuation; the coordinator's ongoing Linux GCC validation was left alone.
- Full Linux Release/umask/sanitizer/Clang and final branch-wide CI completion
  are tracked by the coordinator. This report does not mark global
  `LOCAL_VALIDATION_COMPLETE` or `REMOTE_CI_COMPLETE`.
- Windows 11 machine acceptance, ordinary-user Windows ACL/owner security,
  W-FS/W-PROC/W-SBX/W-CLEAN/W-PARITY: **NOT_RUN / NOT_IMPLEMENTED** as applicable.
- Runtime integration, formal PR approval, coordinator acceptance and merge
  remain separate decisions. The PR stays draft; no merge is performed here.
