# A5 Windows native foundation contract

Baseline: `772feaee62086ddb0e7f605a7472721d3bc61406`, runtime
`0.11.1-r3.3c-audit`, Store 410, implementation blueprint v0.4.1. This is an
independent native build/format slice under the authorized A5 task. It does not
upgrade those runtime/schema versions. v0.5.0 W-FS/W-PROC/W-SBX/W-CLEAN/W-PARITY
remain separate capabilities and acceptance gates.

## Reused real modules and build boundary

`platform/native-foundation/CMakeLists.txt` directly compiles the current
`src/config.cpp` and `src/scheduler.cpp` with their existing public headers.
It does not copy/freeze a replacement algorithm. The shared scheduler's later
compatible APIs remain available when integration updates that source.

`resolve-config` takes bounded operator argv, including explicit symbolic
capability declarations, then calls the original `resolve_model`. It retains
direct frozen-Root field inheritance, all distinct reasoning modes and explicit
unknown/unsupported rejection. `tool-grants` uses the original Worker-only
three-allowlist intersection and labels the result `calculation_only=true`.
The scheduler tests run real callbacks, a controlled barrier, same-key ordering
and the original normal/control capacity reserve. They do not represent Store
Actor scheduling, global Dispatcher/Worker lifetime quotas or durable claims.

The standalone target never links Store, TaskEngine/Journal, Workspace,
WorkerTools, ProcessRunner, delivery, cleanup, credentials or model transport.
The existing top-level Linux entry and its six CI lanes retain their scope.

## Native dependencies, text and observations

The initial candidate build set is Windows x64, VS 2022 17.14, preinstalled
MSVC toolset 14.44.35207, SDK 10.0.26100.0 and static CRT. CMake checks the
generator, explicitly requested toolset, actual compiler directory, SDK and
pointer width. The MSBuild .props selector may differ from the compiler directory
version and is recorded separately. CMake 3.27+ pins the SDK through the generator
platform. CI checks the preinstalled source
and records actual versions; missing versions fail rather than download or
substitute. BCrypt SHA-256 and Advapi32 token observation come from that SDK
and the runner OS. There are no additional vendored or downloaded dependencies.
Python 3.10+ is local build/test tooling, not part of the executable.

Windows `wmain` converts original UTF-16 argv to UTF-8 without replacement;
logical strings use strict Unicode scalars. Native paths convert back to UTF-16.
Malformed UTF-8, overlong encodings, surrogate scalars, lone UTF-16 surrogates,
NUL argv/path bytes and bounded-input overflow reject before command effects.
JSON output escapes controls and preserves accepted Unicode bytes.

`GetModuleFileNameW` locates the actual executable. `OpenProcessToken` and
`GetTokenInformation(TokenElevation)` report actual elevation when available;
`GetVolumeInformationW` reports the executable volume's filesystem. Failure
to observe a fact yields `not_checked`. Those observations do not establish
ordinary-user ACL security or sandbox readiness. Hosted Windows Server CI is
not Windows 11 machine acceptance, even if compilation/tests pass.

## Public distribution resources

The slice compiles the original unmodified `src/runtime_resource_catalog.inc`:
35 SQL/Schema/public-prompt assets, 160549 bytes. The original manifest's exact
SHA-256 is pinned in `platform/native-foundation/manifest.sha256`. Build capture
validates the exact manifest, complete original catalog, asset sizes/hashes and
the complete bounded set before destination writes. The installer copies that
captured build bundle, not live source paths. It excludes external configuration,
credentials, databases and user data.

Runtime capture accepts an explicit absolute normalized public bundle root or
the two actual executable-relative locations `../share/wls861` and
`share/wls861`. It never chooses cwd, PATH, WLS_SOURCE_ROOT, WLS_RESOURCE_ROOT
or a source-tree fallback. A first present invalid manifest fails without
trying another candidate. The manifest must match exact bytes, which is stricter
than the Linux runtime's semantic manifest comparison; this isolated public
distribution slice deliberately does not accept reserialized manifests.

Windows public capture requires a fixed local drive path. UNC, device namespaces,
drive-relative paths, traversal, ADS/control/trailing-dot/trailing-space and
reserved device components reject. Every ancestor directory is opened with
`FILE_FLAG_OPEN_REPARSE_POINT`, inspected for disk/directory type and held
without write/delete sharing. Each asset is opened without write/delete sharing,
checked as a non-reparse disk file with one hard link, read through actual EOF
within its bound, and checked for stable original identity/size/times and SHA-256.
Read-only sharing conflicts fail closed. Handles are never cast to POSIX fds.
The Linux verification adapter uses pinned O_PATH/no-follow descriptors and
OpenSSL SHA-256; it is separate from the production Linux resource loader.

Only a complete capture is returned, and all consumers use the captured bytes.
Deleting source assets afterward does not change them. These are public resource
integrity checks: no Windows business FS adapter, private DACL/owner validation,
SQLite VFS/locking, durable publication or deletion authority is supplied. The
build capture script assumes trusted developer input/destination and is not a
production secure installer or a global atomic directory transaction.

## Per-capability refusal

| Capability | Slice status | Meaning |
| --- | --- | --- |
| Configuration / grant calculation / bounded callback scheduler / Unicode / public byte capture | available | These actual linked modules can run within this slice |
| Store / Workspace / process supervision / sandbox / cleanup | unsupported | The executable and library refusal boundary reject; no weak execution path |
| Model calls | denied | Offline task authorization and executable omit provider calls |
| Network isolation / ordinary-user security | not_checked | Observation/configuration cannot substitute for acceptance tests |

Business commands return JSON errors and nonzero exit 2 before target effects.
Nothing issues a StoreActor, changes epochs/CAS, settles UNKNOWN or proves exit.

## POSIX inventory and unimplemented adapters

The bounded source scan for platform includes identifies these existing coupling
points; semantic dependencies are wider than includes and are not claimed complete:

| Existing path | Platform dependency / future work |
| --- | --- |
| `src/journal_posix.cpp`, `src/store.cpp` | fd locks, secure DB open, WAL/SHM and sync/lifetime |
| `src/workspace_posix.cpp`, `src/cleanup_posix.cpp`, `src/delivery.cpp` | openat2, identity, no-overwrite rename, ownership, inventory and deletion |
| `src/sandbox_posix.cpp`, `src/process_evidence.cpp`, `src/worker_tools.cpp` | namespaces, cgroup, process trees, signals, fds and actual exit observations |
| `src/runtime_types.cpp` | getrandom-backed IDs, outside this slice |
| `src/runtime_resources.cpp`, `src/config_loader.cpp`, `src/offline_runtime.cpp` | Linux pinned path/type reads and retained Workspace guards |
| `src/https_transport.cpp` | system socket-specific implementation outside this offline target |
| `tests/test_support.hpp`, original Make/CMake | POSIX fixtures/umask and Linux production platform guard |

No production file is renamed or weakened to claim native support. Future native
business adapters require separate design/ordinary-user/security/compatibility
evidence under the v0.5.0 platform gates.

## Verification boundary

Four CTest processes cover real config/scheduler modules, strict text/resource
unit cases, CLI/resource integration and build-capture refusal. Individual cases
are not separate suites. CLI integration includes Unicode relocation, resource
loss/same-size tamper/self-reported replacement hashes, no ambient fallback,
hard links, wrong types and a real Windows junction (Linux symlink/FIFO).
`tools/verify_detached.py` additionally requires the original build-source path
to be absent while an installed/relocated executable resolves resources and real
configuration and rejects business execution.

The platform is new; there is no old Windows implementation to claim a reproduced
production defect against. Missing-capability and tampered-resource negative
tests check this design. Actual environments, commands, exit codes, input/binary
identities and NOT_RUN boundaries belong in the versioned validation report.
Windows 11, W-FS, W-PROC, W-SBX, W-CLEAN and W-PARITY stay unaccepted until their
own evidence exists. Local Linux tests cannot mark remote native CI complete.
