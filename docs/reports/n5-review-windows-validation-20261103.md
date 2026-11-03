# N5 Windows review follow-up: policy and image-source evidence

This is the pre-publication execution record for the assigned Windows/N5-0
source-provenance slice. Product baseline:
`c2a5cea64aeea261b214cdfd9a968b53f491110a`; frozen review:
`d843493fafe158f3a6443bafaa0de95cbf6c05c2`. The worktree is
`windows` under the new data-disk task root, branch
`fix/n5-review-windows-20261103-74bnxwj5`. Final commit/PR/CI identity must be
recorded by the publishing executor after this report is frozen.

The limited implementation is ready for engineering review. **Windows 11 x64,
ordinary non-elevated user, NTFS acceptance remains NOT_RUN.** The historical
image-source conflict remains UNRESOLVED. No formal policy/deployment acceptance,
product release, PR approval or merge is claimed.

## Changes

The new [policy/source contract](../implementation/n5-review-windows-policy-20261103.md)
records both policy choices and their consequences. The conservative candidate
rejects the Builtin Administrators SID `S-1-5-32-544` in the shared read-only
policy preflight. Explicit policy inclusion cannot authorize that group as an
owner/writer. Native canonical SID conversion, ordinary-token checks and the
original HANDLE/ACL/resource safeguards remain. Default NTFS ancestor ACLs may
therefore refuse a complete root capture; no host ACL is modified to avoid that
refusal. This is the coordinator's requested policy tightening, **not a newly
reproduced historical production vulnerability**.

`tests/readonly_tests.cpp` adds subcases inside the original duplicate-policy and
complete-ACL methods: administrator-only/additional trust refuses before path
access; native administrative owners and dangerous allow ACEs refuse; deny does
not erase dangerous allow; read-only/deny-only controls remain interpretable.
No suite, method name, deadline or existing assertion was removed or relaxed.

The image collector additionally reads only machine `ImageOS` and `ImageVersion`
using query-only access to the 64-bit HKLM Environment key. Missing/denied fields
remain unobserved; there is no enumeration, expansion, alternate-hive lookup or
inherited-process fallback. Original process fields are preserved separately.
Early/entry machine values are compared independently of their capture times.
The offline comparator validates the new optional channel without fabricating it
for old jobs. Any observed channel conflict remains unresolved; two matching
channels cannot erase the third. `run_windows11.py`'s help text now correctly
describes the existing five component suites.

## Actual Linux execution and policy increment control

Ordinary user: `ubuntu`, UID 1000. Actual data disk: `/mnt/d`, `/dev/vdb2`, ext4.
All new source, temporary fixtures, builds and private evidence remain there.
Toolchain observed: Ubuntu GCC 13.3.0 (`13.3.0-6ubuntu2~24.04.1`), CMake 3.28.3,
Ninja 1.11.1, Python 3.12.3. No dependency install, global compiler/environment
change, permission broadening or Windows emulation was used.

The native compile/test window was granted after the other worker released its
window. This subtask used at most two compilation jobs and performed the
following sequence exactly once per declared input. The original c2 worktree
remained clean; old reports/logs were not changed.

| Execution | Actual outcome and classification |
| --- | --- |
| Original c2 native library configure/build | Both exit 0; original library, original public headers and captured resource bundle |
| Current same-source `readonly_tests.cpp` linked to that original library | Compile exit 0; execution exit 1, 2 PASS / 1 FAIL among the three Linux methods. The failure is the new Administrators policy condition in `duplicate or unsupported policy entries reject`, `error.code() == code`. Old Linux policy preflight reaches Unsupported; the new contract requires Denied before OS/path access. This is a policy increment control, not a historical security-defect red case. |
| New candidate native configure/full build | Both exit 0, original strict warnings and Release flags |
| New candidate native CTest, umask 0022 | 5 PASS / 0 FAIL / 0 timeout / 0 skipped / 0 notrun; 30 original Linux methods visible in complete LastTest; CTest exit 0 |
| New candidate native CTest, umask 0002 | Same 5/5 and 30/30 outcome; CTest exit 0 |
| Native registration audit | Exact five original names; deadlines 30/30/30/60/30 seconds, unchanged |

Representative commands (paths abbreviated to the actual task root only here;
full argv/cwd/umask/TMPDIR/UID/start/end/exit and output hashes are in receipts):

```text
cmake -S OLD_C2/platform/native-foundation -B TASK/builds/windows-policy-baseline-release-01 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build TASK/builds/windows-policy-baseline-release-01 --parallel 2 --target wls_native_foundation
cmake -S TASK/windows/platform/native-foundation -B TASK/builds/windows-policy-candidate-release-01 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-13 -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build TASK/builds/windows-policy-candidate-release-01 --parallel 2
/usr/bin/g++-13 -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror OLD_INCLUDE_FLAGS TASK/windows/platform/native-foundation/tests/readonly_tests.cpp OLD_C2_NATIVE_LIBRARY -lcrypto -pthread -o BASELINE_PROBE
BASELINE_PROBE OLD_C2_CAPTURED_RESOURCES
ctest --test-dir TASK/builds/windows-policy-candidate-release-01 --output-on-failure --output-junit PRIVATE_DISTINCT_MASK_XML
```

Both source manifests stayed equal before/after: original c2 **871** files;
measured dirty candidate **872** files, including the new implementation
contract but not this later execution report. Build identity sets stayed equal:
baseline **7 entries**, candidate **15 entries**, plus one separately hashed
control executable. Those sets include objects/libraries/executables and are
not described as ELF counts. Private complete `LastTest.log`, JUnit and raw
stdout/stderr for each mask are preserved. The original baseline control's
nonzero exit remains preserved.

These are native component checks on Linux, not the 94-registration Linux
product matrix or a Windows machine run. Windows has four additional readonly
methods, for the existing total 34; none is substituted from the 30 Linux methods.
Actual MSVC build, descriptor/ACE subcases and Windows method execution must be
collected from the new SHA's native CI, separately from Windows 11 acceptance.

## Tool selfchecks

| Input and entry | Actual result |
| --- | --- |
| First completed tool input: `test_windows_ci_evidence.py` | 26/26, exit 0 |
| First completed tool input: `test_windows_image_provenance.py` | 16/16, exit 0 |
| Final runner input after adding independent machine-channel change accounting | 27/27, exit 0; original 26-method result preserved and not reused for this source |
| Final comparator input after preventing a matching pair from hiding a third conflict | 17/17, exit 0; original 16-method result preserved and not reused for this source |

The final two source identities each remained unchanged during their own run.
New synthetic methods cover query-only exact scope, absent/denied keys/values,
unsupported/control/unbounded values, channel time/value distinction, all
three pairwise conflict directions, legacy absence, malformed status/scope and
same-job time boundaries. They build no native product and consume no model.
The runner selfcheck's real CTest executions are controlled synthetic log
fixtures, not evidence of Windows host acceptance. The **44 final tool methods**
are separate from both five native suites and 94 Linux product registrations.

## Image-source investigation

Official source was fetched through read-only GitHub API GETs, pinned and hash
checked as Git blobs, read as data and never executed. Sources and trust limits
are linked in the implementation contract. The installed-runner header reports
version 2.338.2, whose upstream tag is commit
`d1eb12d4b115c301201de27d667284e1a80f2ab5`. Its code emits header groups/details
from the runner root `.setup_info` file. The runner-images environment-version
tag resolves to `36de032a97840ee900185b6168c9d63983e8db5a`: separate scripts
generate image metadata and set machine environment from build-time
`IMAGE_VERSION`. This establishes the generator paths, not historical host
binary or `.setup_info` identity. The header-version tag query actually returned
404; that failure is preserved and is not used as a cause or preferred version.

The current reviewed comparator was run once on each saved original job input:

| Original run | Actual comparator result |
| --- | --- |
| push `41416271880`, numeric job `125180768812`, attempt 1 | exit 1; matched saved same-job binding; UNRESOLVED; machine channel NOT_COLLECTED |
| PR `41416279414`, numeric job `125180794615`, attempt 1 | Same result |

The same original header `20261025.346.1` and inherited process value
`20261026.347.1` remain. Source/receipt/archive/prior-comparison bytes stayed
unchanged. This replay performed no network request, archived-code execution,
Actions rerun or old-record mutation. The old VM's machine registry,
`.setup_info` and provisioning record are unavailable. The discrepancy's cause
remains **OPEN / UNRESOLVED**. New named machine captures can distinguish a
future process/machine mismatch, but cannot retroactively provide those facts.

## Evidence and remaining gates

The new task's private `evidence/` contains:

- `windows-policy-validation-01/`: plan, complete commands/outputs, old-library
  control, source/build identity pairs, both original CTest/JUnit copies,
  exact registrations and separate Linux method verification.
- `windows-tools-selfcheck-01/`, `windows-tools-selfcheck-02/`,
  `windows-provenance-selfcheck-02/`: declared tool inputs, original outputs,
  timestamps, actual exits and unchanged-input receipts.
- `windows-original-conflict-recheck-01/`: both actual unresolved comparisons
  and before/after identities of every original source.
- `windows-official-source-01/INDEX.json`: 57 pinned GET/source/error files with
  sizes and SHA-256; downloaded code is never executed.

No private raw evidence, build output, original database, credential or Windows
machine observation is added to Git. Restricted private evidence hosting stays
BLOCKED under the user's existing decision. A small hash index accompanies this
report; raw files stay on the data disk.

| State at this pre-publication snapshot | Value |
| --- | --- |
| IMPLEMENTATION_COMPLETE | true for this limited conservative policy and source-evidence increment; complete Windows runtime is not implemented |
| LOCAL_VALIDATION_COMPLETE | false; listed Linux component/tool checks completed, Windows 11 target NOT_RUN |
| PUSH_COMPLETE | false at report freeze; owner publishes reviewed module normally |
| REMOTE_CI_COMPLETE | false for the new input; no earlier green result is inherited |
| PR_REVIEW_COMPLETE | false; coordinator owns acceptance |
| MERGED | false |

Parent integration owns the final shared product matrix and accurate new-SHA CI.
Historical local ASan/Make failures are not closed by this component. No 014 /
Store 412, DAG, live/file/process executor, sandbox, production operation, main
write, PR merge, real provider or paid request was implemented or performed.
