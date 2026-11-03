# N5 review follow-up: restricted Windows SID policy and image sources

Review input: `d843493fafe158f3a6443bafaa0de95cbf6c05c2`, product input
`c2a5cea64aeea261b214cdfd9a968b53f491110a`. This is an implementation candidate
for the assigned review follow-up. It does not adopt a deployment SID/build
policy, accept Windows 11, or change Store 411 or the runtime version.

## Owner/writer policy

The candidate rejects Builtin Administrators, `S-1-5-32-544`, as an explicit
trusted owner/writer in `require_readonly_policy_shape`. The Windows descriptor
and root-opening paths share that preflight. Native canonical SID validation,
the existing broad/dynamic-SID refusals, primary-token checks, retained ancestor
HANDLEs, same-handle owner/DACL observations and original resource limits remain.
There is no option to bypass the restriction in this profile.

An ordinary caller's token and a resource's trusted ACL principals are different
facts. Refusing an administrative current token does not prevent a supplied
policy from trusting an administrative resource owner or writer. The new check
addresses the policy gap identified by the coordinator; no reproduced historical
permission vulnerability is claimed.

The two policy choices have these consequences:

| Choice | Consequence and acceptance requirement |
| --- | --- |
| Reject Administrators as a trusted owner/writer, implemented candidate | A basic effective administrative allow containing write, deletion, ownership or DACL rights is an untrusted dangerous allow. Administrative ownership also refuses. Explicitly adding the group to the policy cannot change that result. Default NTFS ancestor ACLs may therefore prevent a successful root capture; this work does not alter them or claim that an accepted ordinary-user deployment exists. |
| Permit the group only through a separately adopted policy, not implemented | All members covered by that group become trusted resource managers, independent of the current caller's ordinary token. The coordinator would need to adopt the changed threat model, versioned policy and full positive/negative target-machine acceptance first. No current-user or administrator fallback is introduced. |

The first choice follows the handoff's recommended conservative policy. Formal
policy/deployment acceptance remains with the coordinator. The mechanism does
not defend against the Windows kernel, malicious administrators using privileges,
SYSTEM, malicious same-SID code or arbitrary malicious code in the trusted host.
Rejecting a broad trustee is not an administrative attacker sandbox claim.

Read-only or deny-only administrative ACEs do not by themselves grant dangerous
access and remain interpretable under the original evaluator. A deny ACE cannot
cancel an unsafe effective allow. The tests cover administrative full/write-DACL/
write-owner allows, deny plus unsafe allow, administrative ownership, explicit
trust attempts, and read-only/deny-only controls. These are additional subcases
inside the original two methods, not new CTest registrations or new method names.

The portable preflight test can run on Linux. Windows descriptor/ACE cases require
native Windows execution. A successful descriptor test is not a successful full
root capture. Windows 11 x64, ordinary non-elevated user, NTFS root/ancestor/ACL/
replacement/EOF acceptance remains **NOT_RUN** while that machine is unavailable.
The Store/VFS, arbitrary Workspace access, process, sandbox and cleanup boundaries
remain unsupported.

## Named image-source capture

`run_windows_ci.py` retains the original inherited-process `ImageOS` and
`ImageVersion` fields, plus a separate, bounded machine observation. The new
observation queries only these two names in the 64-bit view of:

`HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Environment`

Access is `KEY_QUERY_VALUE | KEY_WOW64_64KEY`. The collector neither enumerates
registry/environment values nor writes keys. It does not expand `REG_EXPAND_SZ`,
scan other hives, or replace a missing/denied value with the process environment.
Only bounded nonempty `REG_SZ` strings without controls are retained. Every
channel has its own capture time; the containing environment capture retains
the original run/attempt/job-key/checkout identity.

Machine fields record `OBSERVED`, `MISSING`, `DENIED`, `QUERY_FAILED`,
`UNSUPPORTED_VALUE` or `NOT_CHECKED`. Missing APIs/keys and denied access stay
unobserved. Observations changed between the early workflow capture and runner
entry are recorded and reject that entry before the build; capture timestamps
alone are not treated as a changed value. This evidence adds no business
capability or accepted host identity.

`windows_image_provenance.py` validates the optional machine schema, fixed scope,
field/status consistency and same-job time bound. It preserves original archive
hashes and same-job printed-capture binding. Historical captures without this
field report `NOT_COLLECTED`; no observation is invented. Matching machine and
process values cannot erase a conflicting header. Matching header and process
values cannot erase an observed machine conflict. All observed conflicts remain
`UNRESOLVED`, with no preferred source or causal explanation.

## Upstream field-generation evidence

The official Actions runner source for version `v2.338.2`, observed in the saved
f5 Windows job logs, resolves to commit
`d1eb12d4b115c301201de27d667284e1a80f2ab5`:

- [HostContext.cs](https://github.com/actions/runner/blob/d1eb12d4b115c301201de27d667284e1a80f2ab5/src/Runner.Common/HostContext.cs)
  maps `SetupInfo` to the runner root's `.setup_info`.
- [JobExtension.cs](https://github.com/actions/runner/blob/d1eb12d4b115c301201de27d667284e1a80f2ab5/src/Runner.Worker/JobExtension.cs)
  loads that JSON and emits the groups/details during job initialization.

Official runner-images tag `win22/20261026.347` resolves to
`36de032a97840ee900185b6168c9d63983e8db5a`:

- [Configure-ImageDataFile.ps1](https://github.com/actions/runner-images/blob/36de032a97840ee900185b6168c9d63983e8db5a/images/windows/scripts/build/Configure-ImageDataFile.ps1)
  generates the `Runner Image` details from build-time `IMAGE_VERSION`.
- [Configure-SystemEnvironment.ps1](https://github.com/actions/runner-images/blob/36de032a97840ee900185b6168c9d63983e8db5a/images/windows/scripts/build/Configure-SystemEnvironment.ps1)
  separately writes machine `ImageVersion` from build-time `IMAGE_VERSION`.

These sources establish the generator paths, not the bytes installed in a
historical VM. The header is rendered from persisted metadata; the process value
is an inherited environment observation, and machine registry values are another
separate observation. Independent storage or later overrides are hypotheses,
not a proven cause of the historical conflict. The exact old `.setup_info`,
machine registry values and provisioning records were not captured. The header's
`win22/20261025.346` Git ref query returned HTTP 404 during this investigation;
that result alone proves neither an invalid header nor which image ran.

The raw read-only GET responses, failures, source blob identities and SHA-256
records are preserved privately under the new task's
`evidence/windows-official-source-01`. Downloaded source was inspected as data
and never executed. No image source is selected to overwrite another record.

The current comparator was also applied to the two saved original same-job
inputs (runs `41416271880` and `41416279414`) without new network requests or
executing archived code. Each preserves matched run/job/attempt/checkout binding,
exits 1, and remains **UNRESOLVED / machine NOT_COLLECTED**. Original source and
prior comparison hashes remain unchanged. New execution results belong in the
versioned follow-up report, not the historical reports.
