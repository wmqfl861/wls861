# A5 native foundation: initial implementation and local evidence

Time scope: 2026-10-31 UTC, before the first draft publication of this module.
Baseline: `772feaee62086ddb0e7f605a7472721d3bc61406`, branch
`feat/windows-native-foundation-20261031`. No final commit or remote CI result
is claimed by this prepublication report. Later observations belong to the
actual submitted SHA and its publication receipt.

## Actual change and limits

The new standalone `platform/native-foundation` project compiles the original
`src/config.cpp` and `src/scheduler.cpp` directly. Its controlled argv resolver
and tool-grant calculator invoke those real modules. It adds strict Unicode
conversion, Windows executable discovery/token/filesystem observation, BCrypt
SHA-256 and complete capture of the existing 35 public resources. Native
Store, Workspace tools, process supervision, sandbox and cleanup remain
unsupported; model calls are denied. Unsupported business commands fail with
exit 2 before target effects. The detailed boundary and POSIX inventory are in
[the new contract](../implementation/windows-native-foundation.md).

The existing Linux runtime entry, published SQL, original resource catalog and
manifest, third_party, historical reports, blueprints and docs/handoff have not
been edited in this worktree. A separate Windows workflow is added at
`.github/workflows/windows-native.yml`; it does not edit the six existing Linux CI lanes. The native
candidate uses preinstalled VS 2022 17.14, x64 toolset 14.44.35207, SDK
10.0.26100.0, static CRT and Windows system libraries, with no dependency download.
Missing pinned tools must fail rather than silently substitute.

## Local environment and executed checks

The executor is Linux user `ubuntu`, UID/GID 1000. Source, captured resources,
raw logs and temporary fixtures are on ext4 data disk `/mnt/d`; roughly 50 GiB
was free at takeover. Git 2.43.0, CMake 3.28.3, Ninja 1.11.1, GCC 13.3.0,
Clang 18.1.3 and Python 3.12.3 were observed. An unrelated formal GCC validation
was already running; the coordinator reserved that local execution window, so
this initial stage performs reading, edits and lightweight Python checks only.

The raw local evidence is retained outside Git in the task's `evidence/a5`:
each executed command has argv, cwd, UID, times, exit code and a corresponding
unaltered combined-output log. Test TMPDIR is `evidence/a5/tmp` on the data disk.

| Actually executed | Result |
| --- | --- |
| Python AST parse of the new Python sources | passed; no generated pycache in source |
| `python3 platform/native-foundation/tools/prepare_bundle.py --source-root <windows-worktree> --destination <evidence/a5/light-captured-bundle> --expected-manifest-sha256 b9f5468c6858ebd467fb4e6905d2ef4b8bd51a006e4fd2a636f8e4db1fde55cf` | exit 0; 35 assets, 160549 bytes, original manifest identity |
| `python3 platform/native-foundation/tests/test_bundle_capture.py --source-root <windows-worktree>` | exit 0; one Python suite, six cases |
| `git diff --check` | exit 0 at the local text-check checkpoint |

The six build-capture cases verify all original asset bytes/hashes, actual missing
resource, equal-size tamper, changed manifest, changed catalog and wrong source
object type. Each negative case verifies that no capture destination was created.
They are six cases within one suite, not six independent suites. They use only
temporary copies of public source files. This is a new platform slice; there is
no previous Windows implementation or reproduced old Windows defect to report.

## Prepared tests and evidence still required

Four CTest processes are registered: `native_modules`, `native_platform`,
`native_cli`, `native_bundle_capture`. The first two cover actual resolver/grants,
real scheduler reserve/key ordering, Unicode/error/refusal and complete captured
bytes, including capture retained after source resource removal. CLI integration
has controlled configuration and rejection inputs, Unicode model/path/relocation,
all assets, absent/bad resources, a bad first bundle despite a valid second,
cwd/environment non-selection, hard links, wrong types, Windows junctions and
Linux symlink/FIFO checks. These C++/CLI cases are **NOT_RUN** at this time scope.

The native CI driver records exact source SHA-256 before/after, executable hashes,
original commands/stdout/stderr/exit codes, OS, toolchain source and versions, actual
token elevation and executable-volume filesystem. It performs the four tests,
install, Unicode relocation and an installed-binary run while the original build
source path is actually absent. The prepared workflow and driver are not remote
execution evidence. No Windows 11 machine is present locally; hosted Windows
Server CI, if later successful, remains limited to its observed build/format slice.
Ordinary-user ACL security, secure SQLite/native business FS, process/exit proof,
strong sandbox/network denial, cleanup and dual-platform business parity remain
unimplemented or not accepted. Token observation does not complete those gates.

No new C++ build, sanitizer or full Linux suite has run for this input yet.
The coordinator's other worktrees and their gates cannot be relabelled as this
module's results. No production data/service, real provider, credentials or paid
API was used. Independent engineering review is performed by the non-author
`/root/evidence_audit`; its exact scope/hash/findings are retained externally.
It is a same-model engineering review, not a coordinator approval or provider test.

## Status at this report's cutoff

| State | Actual status |
| --- | --- |
| `IMPLEMENTATION_COMPLETE` | pending native compile/required review follow-up; bounded code and tests are present |
| `LOCAL_VALIDATION_COMPLETE` | false; only the lightweight checks above have completed |
| `PUSH_COMPLETE` | false in this worktree; coordinator may publish a clearly labelled draft |
| `REMOTE_CI_COMPLETE` | false; no native workflow run has yet been observed |
| `PR_REVIEW_COMPLETE` | pending coordinator decision for an actual SHA |
| `MERGED` | false; no merge requested or performed |
