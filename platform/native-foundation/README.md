# WLS861 native foundation slice

This separate C++20 project compiles the original `src/config.cpp` and
`src/scheduler.cpp`. It implements a bounded offline command boundary, strict
UTF-8/UTF-16 conversion and capture of the existing 35 public runtime resources.
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
