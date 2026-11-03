# N5 private originals: storage boundary and acceptance proposal, 2026-11-03

This is the N5-0 follow-up requested by the frozen coordinator handoff
`d843493fafe158f3a6443bafaa0de95cbf6c05c2`. The user's selected policy remains
**local private originals only; hosted persistence BLOCKED**. No remote
destination, remote reviewer list, remote retention period, upload credential or
storage expenditure has been authorized. This document makes those prerequisites
reviewable; it does not implement an upload or change the selected policy.

The implemented local boundary remains
[N5-private-evidence-tools.md](N5-private-evidence-tools.md) and
`scripts/private_evidence.py`. This proposal does not supersede their actual
capture, pack, verify or preflight behavior. A complete local bundle and a
successful local verify are not hosted readback evidence.

## Assets, principals and trust boundaries

Private originals include complete CTest LastTest and JUnit, stdout/stderr,
command/exit receipts, input and executable identities, registration catalogs,
and any complete failure, timeout or interruption observations that exist.
Paths, account identifiers, captured fixture payloads and exception text may
contain private material even when a test uses no real credentials. Originals
remain immutable; any public report or log is a separately reviewed projection.

The trusted local collection boundary is the ordinary executing UID and its
approved development host. Files are collected under an exclusively created
0700 task directory on the verified data disk, with 0600 copied files and
strict member/identity/hash checks. Host administrators, storage administrators
and malicious code running as the same UID are not excluded by these file
modes. Hash manifests detect corruption and association mistakes; they do not
authenticate a malicious administrator or conceal content.

A future remote boundary would distinguish:

| Principal | Required authority | Authority it must not inherit |
| --- | --- | --- |
| Trusted same-repository CI job for an approved exact input | Write a new run-scoped evidence object | Read unrelated evidence, change ACL/retention, overwrite another run |
| Explicitly named coordinator/reviewer | Authenticated read of the accepted scope | Implicit access for all public-repository readers |
| Storage lifecycle operator | Enforce approved retention and deletion audit | Implicit product approval or ability to rewrite test outcomes |
| Fork, untrusted PR, anonymous/public reader | None over private originals | Access through public artifacts, logs, job summaries or reusable credentials |

The repository being public and the current operator having GitHub write/admin
access do not establish any of the remote authorities above. No secret is read
to test whether a hypothetical destination works.

## Required controls and discriminating evidence

| Threat or failure | Required control | Evidence before hosted completion |
| --- | --- | --- |
| Public disclosure through Actions artifacts or log output | Keep raw bundles outside public upload globs; review the public projection independently | Exact uploaded member list and negative canary check; anonymous and unauthorized reads denied |
| Fork/untrusted job obtains upload authority | Separate trusted collection/publishing boundary; least-privilege identity limited to the approved job/input | Fork/untrusted context denied without receiving secrets; authorized job identity recorded |
| Another job overwrites or substitutes evidence | New immutable run/job/attempt/input namespace; exclusive creation; manifest binds member bytes, sizes and input/ELF identities | Duplicate/conflicting write denied; exact candidate SHA and actual checkout SHA both retained |
| Partial upload or delayed object mistaken for completion | Commit a manifest only after all members are present; independent authenticated readback | Read every member with reviewer-equivalent credentials, verify full file set, size and SHA-256; missing/extra/tampered member rejects |
| Retention silently expires before review | Coordinator-approved duration and start event; enforceable lifecycle policy | Readback during retention; observable policy; expiry/deletion audit and denied retrieval after approved expiry |
| Credential or sensitive exception leaks during error handling | Avoid secret values in argv/logs; bounded public error code, private original traceback | Deliberate denied upload/readback produces no credential or private payload in public output |
| Failure/timeout/interruption loses the only raw logs | Always-run bounded finalization independent of product exit, preserving available originals and explicit missing members | Four separate terminal-state exercises below; forced termination remains incomplete when finalization cannot run |
| A public redacted log is treated as the raw original | Separate namespaces and receipts, public-to-private manifest association | Public receipt names projection schema, original digest and omitted-content policy; never reports byte equality |

The authenticated readback must occur through the actual chosen storage API
and authorization boundary. A local copy, an upload response, a manually entered
URL, a matching filename or an unsigned success flag does not suffice. Readback
failure keeps `hosted_persistence=BLOCKED` or `NOT_COMPLETE` even if the product
test itself passed. Conversely, a complete failure bundle retains the original
test failure and can satisfy collection completeness without passing the gate.

## Terminal-state contract

| Observed execution | Required retained facts | Collection/product distinction |
| --- | --- | --- |
| Success | Uninterrupted actual child exit 0, complete LastTest/JUnit, catalog, exact input and ELF before/after, all command logs | Product success is assessed by its runner; hosted completion additionally requires authenticated remote readback |
| Failure | Actual nonzero child/CTest exit and original failure output, including immediate traceback | Complete collection does not change the failed product status |
| Timeout | Original configured deadline, actual CTest timeout/exit, available LastTest/JUnit and full captured stderr | Preserve TIMEOUT; do not extend deadlines or turn partial tests into passes |
| Recoverable interruption | Received signal, forwarded control, actual reap/exit, full available LastTest/JUnit or `.tmp`, missing-member inventory | An incomplete JUnit or missing final receipt is INCOMPLETE; a `.tmp` is never renamed into a complete original |

SIGKILL, machine loss and storage outage can prevent finalization. A start receipt
and partial objects preserve the attempt as `NOT_COMPLETE`; the absence of a
completed manifest must never become an inferred success. This design does not
promise recovery of bytes that the OS never persisted. It does not change any
business timeout, Store lock, cancellation, UNKNOWN or budget rule.

## Options for coordinator selection

| Option | Present state | Conditions and tradeoffs |
| --- | --- | --- |
| Continue local private originals | Selected; implemented local tools; hosted BLOCKED | No new service or upload; survives task-directory removal only when an independent bundle was actually retained, not loss of the host |
| Existing approved private repository or private storage service | Proposal only | Requires exact existing destination/owner, named reader ACL, minimal job writer, enforceable retention, denied-access checks, authenticated readback and availability/cost responsibility |
| Encrypted bytes in a public artifact | Not selected; requires separate design decision | Ciphertext would remain publicly downloadable; key distribution, revocation, metadata leakage and retention require separate acceptance; this is not a private GitHub artifact |

No option creates resources, purchases capacity, enables a new workflow secret
or grants model-call authority. The next decision, when the coordinator is ready,
must identify a concrete destination, access principals, retention duration and
ownership before implementation. There is no reason to ask the user to repeat
the current local-only decision while independent authorized development proceeds.

## Publication and remaining state

For the current work, keep raw originals and bundles on the data disk. Publish
only reviewed summaries, sanitized projections and a correspondence receipt that
binds each projection to its private source manifest and exact collection attempt.
Do not overwrite historical LastTest, JUnit, run summaries or failure reports.
Any later storage adapter must reuse the existing collection format and test
all four terminal states before its own completion claim.

This document is a design deliverable, not executed storage acceptance.
`hosted_persistence=BLOCKED`; authenticated remote readback, remote ACL/retention
tests and deletion verification are `NOT_RUN`. No secret, raw log, database,
binary or private bundle is committed by this change.
