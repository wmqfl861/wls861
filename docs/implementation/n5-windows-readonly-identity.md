# N5 Windows read-only object identity mechanism

Input: `aa2c592b8aed6509189710cc6f6d69e0959098a8`, following the N4-C design and
the two N5 handoff inputs frozen by the coordinator. This implements a bounded
mechanism for review. Deployment policy is **POLICY_PENDING**; actual Windows 11
ordinary-user acceptance remains **NOT_RUN**. It does not change Store 410,
runtime `0.11.1-r3.3c-audit`, historical SQL or the Linux business interfaces.

## Interface and explicit policy

`platform/native-foundation/include/wls/native_readonly.hpp` adds a separate
move-only `ReadOnlyRoot` and immutable `ReadOnlyPublicCapture`. Root construction
holds every ancestor HANDLE until destruction. `capture_registered()` has no
caller-selected file path: the shared original compiled catalog selects the
manifest and assets. The capture exposes those immutable bytes and bounded
Windows observations. No HANDLE is returned, converted to `int`, or serialized.

`WindowsReadOnlyPolicy` has no defaults. The trusted host must provide a bounded
approval reference, exact Windows build list, and canonical owner/writer SID
list from an adopted external policy. Empty, duplicate, malformed or broad/dynamic
trust entries reject. The current user must be explicitly listed; neither SYSTEM
nor Administrators is implicitly added. This repository supplies no accepted
deployment policy, policy file loader, or CLI opt-in. The reference string is a
binding field, not a signature or proof that a coordinator approved the policy.
The trusted-host API cannot defend against arbitrary malicious same-process code.

The policy digest binds `windows-readonly-policy-v1`, JSON-escaped approval
reference, listed builds, and sorted canonical SID strings. The OS/token/volume
requirements are fixed by the implementation, so policy cannot switch off ACL,
reparse, EOF, ordinary-user or NTFS checks. This is not a general configuration
or credential reader. Historical `PublicResources::from_bundle/discover` and the
`resources` CLI preserve their original public-byte integrity contract and do
not claim the new ACL guarantee.

## Host, handles, object identity and alias refusal

The mechanism observes the actual primary token. Any thread impersonation or
unknown token state refuses, including a same-user impersonation token. The
primary token must be non-elevated, `TokenElevationTypeDefault`, non-member of
Administrators, medium-integrity, and owned by an explicitly trusted user SID.
Every subsequent capture rechecks TokenId, AuthenticationId and ModifiedId.
`IsWow64Process2` requires native AMD64; the system `RtlGetVersion` export must
report a Windows 11 workstation build explicitly listed in the policy. Server,
emulation and unavailable observations refuse before root opening.

The reader retains its original `FILE_FLAG_OPEN_REPARSE_POINT`, directory
`FILE_FLAG_BACKUP_SEMANTICS`, and `FILE_SHARE_READ`-only access. New directory
checks additionally request READ_CONTROL. Files use the same existing
GENERIC_READ handle for security observations and bounded data capture. All
handles are RAII-owned and non-inheritable. The real volume root is checked so
a substituted drive rooted below that volume cannot omit necessary ancestors.
Every opened layer must have the same observed volume serial as the root.

`GetFileInformationByHandleEx(FileIdInfo)` records the complete unsigned 64-bit
VolumeSerialNumber and all 16 FILE_ID_128 bytes in their returned order. The type
`WindowsObjectIdentity` is independent of POSIX device/inode. Object observations
also retain attributes/tag, link count, size, creation/write times, owner SID and
security-descriptor SHA-256. These are live-handle observations; IDs may be
reused after deletion and serialized observations never permit reopening.

Each same-handle volume query must report NTFS with persistent ACLs. Directory
case-sensitivity must be explicitly observed as disabled. Normalized long names
from `GetFinalPathNameByHandleW` must preserve the requested exact component;
short/case aliases refuse. That name comparison only rejects aliases: it is not
a string-prefix containment proof. The existing retained, non-replaceable
ancestor handles and checked ACL chain remain the containment mechanism. Native
parent/reparse/alias races still require the separately listed target-machine
tests before accepting this support profile.

## Owner/DACL policy

`GetSecurityInfo` reads owner and DACL from the already opened HANDLE. Unavailable
READ_CONTROL, malformed/non-self-relative descriptors, absent/NULL/empty DACLs,
unknown ACE types, unsupported inheritance flags and unknown access bits refuse.
The owner must be in the explicit trusted set because ownership itself can
permit DACL changes. Only complete basic ACCESS_ALLOWED/ACCESS_DENIED ACEs with
valid canonical SIDs are interpreted. Conditional, object-specific and effective
creator/owner-rights ACEs are outside this first profile.

Generic masks are expanded using the Windows file generic mapping. A dangerous
effective allow includes write/append, EA/attribute writes, DELETE,
FILE_DELETE_CHILD, WRITE_DAC or WRITE_OWNER; its SID must be trusted. A deny ACE
does not cancel a dangerous allow for this conservative policy. Inherit-only
ACEs grant no rights on the current object; every existing child is inspected
again. This implements a restrictive interpretable policy, not a replacement
for Windows AccessCheck or an assertion that every rejected ACL is exploitable.
Mandatory-integrity restrictions are not used to excuse unsafe DACL allows.

Both security and normal file observations are checked again before returning
captured bytes. All ancestors are rechecked after construction and before/after
each file capture. Checks never take ownership, alter DACLs, enable backup
privileges, loosen sharing or repair a path. The threat model trusts the current
controlled host and only explicitly approved management SIDs; it does not claim
to resist malicious same-SID code, approved administrators, SYSTEM or the kernel.
Default drive/temp ACLs may therefore reject; tests must not weaken the policy
or alter global host ACLs to make a positive fixture possible.

## Captured resources and compatibility

`capture_registered_public_bundle` in `public_resources.cpp` is shared by the
historical and new readers. It preserves exact compiled manifest digest,
35 resources / 160549 asset bytes, 64 KiB manifest, 1 MiB per asset, and 8 MiB
aggregate bound. Original path limits remain 4096 bytes for roots and 512 bytes
for internal registered relative paths. The original reader still proves real
EOF and original size/time/identity before returning a complete string; the
shared catalog checks the captured string hash. A failed asset returns no
complete capture or prefix. Consumers use retained bytes after handles close.
The collection is not advertised as a snapshot of unrelated directory entries.

This slice does not link Store/VFS, Workspace, file writes, allocation, arbitrary
paths, process execution, sandbox, publication or cleanup. It issues no Actor,
claim, CAS, process exit observation or recovery permission. Its capability
table and production Linux guards remain unchanged.

## Tests and unresolved acceptance

The new `native_readonly` suite has three portable methods and four additional
Windows methods. Portable checks exercise policy refusal before path access,
duplicate/unsupported policy entries, old capture compatibility, and Linux
Unsupported refusal for the Windows-only API. Windows checks use actual SDK
security descriptors and owned temporary file HANDLEs, including ACL masks,
inheritance/object ACEs, full file ID, same-handle GetSecurityInfo, noninheritance,
a barrier-controlled rename refusal, and real current-thread impersonation.
The descriptors and leaf-handle checks do not mock a successful full root open.

Server CI can exercise these mechanisms and refusals. It cannot run a successful
new root capture, because that path deliberately requires a Win11 normal user
and an adopted policy. Full positive root capture/Unicode relocation, ordinary
second-user ACL negatives, every ancestor rename/reparse/ACL race, volume/alias
cases and target-machine handle lifecycle remain **NOT_RUN** until the exact
machine/build/toolchain/trusted SID policy is adopted and actually exercised.
No successful native or machine execution is inferred from Linux compilation,
new test source, synthetic descriptor checks or green workflow summaries.

No old Windows ACL-secure root implementation exists in the base, so these new
mechanism tests are not a reproduced production vulnerability. Existing raw
source/image audit failures retain their original classification and bytes.
Execution evidence belongs in the new N5 validation report.

## Offline image-source comparison

`tools/windows_image_provenance.py` reads explicit saved run/job/artifact API
responses, original job log and artifact ZIP, each bound to its original GET
receipt and file size/hash. It checks numeric run/job/attempt, branch head,
actual checkout command, upload artifact ID, API archive digest, original
environment member and identical structured capture printed in the same job.
The Runner Image Provisioner group is distinct from the Runner Image group.
Header emission and environment capture times must lie within the job interval
and preserve their observed order. API download times remain separate facts.

The fresh private result retains original header lines, environment values,
source paths/hashes/times and explicit trust limitations. A matched binding with
different versions exits 1 and remains UNRESOLVED. No source is selected as more
favorable and no causal explanation is invented. Matching version fields exit 0
only as consistent observations, never signed host attestation. Invalid binding
exits 2. All inputs and prior audit results stay untouched; no network requests
or archived code execution occur. Eight synthetic tool methods verify this
evidence logic and are not product CTest registrations or Windows execution.
