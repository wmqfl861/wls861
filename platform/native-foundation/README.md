# WLS861 native foundation slice

This separate C++20 project compiles the original `src/config.cpp` and
`src/scheduler.cpp`. It implements a bounded offline command boundary, strict
UTF-8/UTF-16 conversion and capture of the 36 compiled public runtime resources.
The bundle includes the registered Store 411 assets; this does not implement a
native Windows Store or enable its filesystem, process or sandbox capabilities.
It is not the native Store, Workspace, process runner or sandbox.

The implementation contract and POSIX inventory are in
[windows-native-foundation.md](../../docs/implementation/windows-native-foundation.md).
The Linux production CMake entry remains independent and retains its platform
and security gates. No third-party bytes or published resources are changed.

The initial Windows build combination selects preinstalled Visual Studio 2022
17.14, x64 MSVC toolset **14.44.35207**, Windows SDK **10.0.26100.0**, the static
MSVC CRT and SDK-provided BCrypt/Advapi32. No NuGet, vcpkg, FetchContent, Boost,
SQLite, YAML, TLS or network download is used by this project. Missing pinned
tools fail configuration; they are not substituted automatically. CMake 3.27+
pins the SDK through the generator platform. Python 3.10+
is build/test tooling only. The workflow records its actual version and the
runner image, compiler, SDK, OS, token elevation and volume filesystem.

```powershell
cmake -S platform/native-foundation -B native-build `
  -G "Visual Studio 17 2022" -A "x64,version=10.0.26100.0" `
  -T "v143,host=x64,version=14.44.35207" `
  -DCMAKE_SYSTEM_VERSION=10.0.26100.0
cmake --build native-build --config Release --parallel 2
ctest --test-dir native-build -C Release --output-on-failure
cmake --install native-build --config Release --prefix native-install
```

On Linux the same module/format boundary can be checked with the existing
system OpenSSL 3 Crypto library. This does not prove Windows execution:

```sh
cmake -S platform/native-foundation -B native-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build native-build --parallel 2
ctest --test-dir native-build --output-on-failure
cmake --install native-build --prefix native-install
```

The executable accepts operator-supplied symbolic model capabilities and
configuration. It calls the original resolver, which independently inherits
each missing field from frozen Root defaults and rejects unknown or unsupported
settings. This calculation has no provider endpoint or attempt:

```sh
native-install/bin/wls-native-foundation resolve-config \
  --root-model local --root-reasoning effort:high \
  --capability local=off,provider_default,effort:high
native-install/bin/wls-native-foundation tool-grants --role worker \
  --role-tools read,write --system-tools read,write --request-tools read
native-install/bin/wls-native-foundation resources
native-install/bin/wls-native-foundation capabilities
```

`store`, `fs-read`, `fs-write`, `run` and `cleanup` return an `unsupported`
error with exit code 2; `call-model` returns `denied` with exit code 2. Grant
calculation does not create an execution capability. Unknown/duplicate options,
invalid encodings and bad public resources also fail with exit code 2.

## Hosted image provenance

`run_windows_ci.py` keeps its native Windows Actions guard. Immediately after
checkout, capture the inherited image variables before running the synthetic
selfcheck, then pass that original file to the build entry:

```powershell
python source/platform/native-foundation/tools/run_windows_ci.py `
  --source-root source --capture-image-environment native-image-environment.json
python source/platform/native-foundation/tools/test_windows_ci_evidence.py
python source/platform/native-foundation/tools/run_windows_ci.py `
  --source-root source --work-root native-work `
  --image-environment-snapshot native-image-environment.json
```

Capture writes a fresh file outside the clean checkout and performs no build or
product execution. Its allowlist contains only `ImageOS`/`ImageVersion`, named
GitHub run/attempt/job/event/SHA/runner fields, the actual checkout SHA and a UTC
capture time. `GITHUB_JOB` is a workflow job key, not the numeric API job ID.
The build entry retains the original capture, records its own observation and
refuses changes to the environment/context/checkout before toolchain discovery.
Upload both `native-image-environment.json` and `native-work/evidence/`, including
failures. The existing complete CTest LastTest and method checks remain required.

These observations describe process environment values. Exact image identity
stays unresolved until an external review binds the same run, numeric job,
attempt, checkout and artifact to the original job log's **Runner Image** group
and compares its Version. Matching two environment captures alone cannot prove
that comparison. Keep the Runner Image Provisioner version separate as well.
In the original PR14 Push and PR jobs, that group records `20261025.346.1` while
the artifacts record inherited `ImageVersion=20261026.347.1`. Their artifact IDs,
upload logs and checkout identities match; the earlier variable assignment and
capture time were not retained. Both original values remain evidence, and the
historical exact-image discrepancy remains unresolved.

## Windows 11 ordinary-user component entry

On an authorized Windows 11 x64 machine, provision the same pinned preinstalled
VS/MSVC/SDK/CMake/Python set through the machine owner's normal process, then use
a clean checkout and a new, separate work directory on fixed local NTFS volumes:

```powershell
python platform/native-foundation/tools/run_windows11.py `
  --source-root C:\work\wls861 --work-root C:\work\native-win11-01
```

Run this from an ordinary non-administrator account without elevation. The entry
reads actual workstation product type/build (22000+), native x64/Python bitness,
token elevation/type/admin membership and the source/work volume filesystems.
Server, ARM/emulation, elevated or split administrator tokens, non-NTFS volumes
and unavailable observations refuse before configure/build/tests. The entry
does not create accounts or change UAC, ACLs, execution policy or privileges.
It shares the pinned build, the original four CTest registrations and deadlines,
full method logs, resource refusal tests and relocated/detached verification.
It additionally checks the installed component's non-elevated NTFS observation.

Results distinguish `windows_11_component_validation=PASSED` for an actual
successful execution from full machine/security acceptance, which stays
`NOT_RUN`; the full native runtime remains `NOT_IMPLEMENTED`. Hosted Server
results and synthetic selfchecks cannot fill in a Windows 11 machine run. No
Windows 11 execution is claimed by adding this entry.
