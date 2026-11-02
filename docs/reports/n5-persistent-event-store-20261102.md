# N5-1 persistent event Store source report — 2026-11-02

This report records the Store implementation prepared from design commit
`451b232a0dde08220a5e096d91209087b027b42e`. The user adopted option B and the
complete transaction, capacity, compatibility and rollback contract; the
[adoption record](../implementation/n5-persistent-events-20261102/adoption-and-implementation-20261102.md)
preserves that authorization separately from the published proposal.

Current state is **SOURCE_READY_FOR_REVIEW / C++_BUILD_NOT_RUN /
PRODUCT_TESTS_NOT_RUN**. Static inspection and Python SQLite checks described
below do not establish product acceptance. The coordinating reviewer is
reviewing the transaction and capacity code; a different author is reviewing
the extracted legacy guards. Source commit, push, remote CI, formal PR review
and merge have not been completed at this report's time scope.

## Implemented boundary

The new 013 asset adds Store 411 and six logical relations for immutable request
policy, complete original claims, call associations, selected ID sources,
typed receipts and unresolved ID ownership. It creates no links for historical
attempts. The migration records the existing event high-water mark; only a
request whose original admission follows that barrier can explicitly enable
the protocol. Existing requests retain legacy behavior.

T0 stores the original complete claim and source hashes atomically. T1 reuses
the original reservation, budget and frozen profile checks in the transaction
that creates the association, members, unresolved projection and receipt.
T2 couples the original unique offline dispatch event with its receipt; only
the first committed permission returns `dispatch_claimed=true`. T3 couples
the immutable original observation, nullable accounting and settlement receipt.
Callbacks and model waits remain outside Store.

Before original T1 reservation and first T2 dispatch, AMEND rechecks each
captured task's original request ownership, task/node IDs, state version,
live team ID and execution epoch. T4 retains its original application guards.

T4 reloads the original persisted response, rechecks its original Actor,
complete binding, target source and CAS, and composes the original database
steps for independent plan, instruction or note. Its effects, immutable
feedback, individual ID proofs, original finish subset and projection release
commit together. Note retains the original prepare/start/finish events,
operation intent, tool grant and tool-call accounting. The owner acknowledges
the original instruction before Worker capture. Partial AMEND only completes
subjects with individual committed transitions; a captured omitted subject
requires an unchanged, still-pending proof to receive `DEFERRED_NO_EFFECT`.

Old reserve, dispatch, business, feedback and finish entry points refuse an
enabled request. The original public wrappers retain their transaction and
legacy defaults. Eleven original transaction bodies were extracted without
removing their authorization, replay, CAS, budget, grant or routing checks.
The persistent claim query additionally excludes unresolved IDs. An unresolved
association also fences new calls for its original request/role/task, so a new
recover wake cannot turn the old observation into an automatic retry.

Stored proofs and unresolved projection are checked together on each protocol
read or consume. CLOSED reads return the original proof/result while reporting
current inbox status separately. A later legitimate transition of an originally
unselected or deferred ID does not rewrite the old disposition. Storage or
corruption during application retains the response and halts new Store business;
evidence flush may only repeat T3. Ordinary invalid output or obsolete authority
rolls back business, then records the original error feedback/HOLD. Neither path
manufactures completed IDs or rewrites UNKNOWN.

The protocol freezes lifetime link/claim/metadata ceilings and reserves finite
receipt space. Original business/control event tails are checked before
reservation and while consumed, deducting overlap with the existing pending
call/operation reserves. Checkpointing the old event window cannot reset new
lifetime counts. This is logical reservation, not a promise of physical disk
space. Published SQL, Schema bytes and old fingerprints remain unchanged.

## Prepared verification

The coordinating agent registered one `persistent_event_store` CTest entry with
a 120 second limit. Its 17 prepared methods cover these cases:

- Explicit new-request opt-in and rejection of legacy bypass paths.
- Two requests with two independent nodes each, using original plan,
  instruction, owner ack and note facts, plus CLOSED and dispatch replays.
- Selected versus unselected captured IDs and the later Worker feedback ID.
- Actor, claim, selected set, target and all ten execution-binding mutations,
  with unchanged original budget and attempts after rejection.
- Unique dispatch, full original response identity and settlement replays.
- UNKNOWN accounting, unresolved ID fences and refusal to retry business.
- Reopening at RESERVED, DISPATCHED and SETTLED without reissuing or applying
  the original call.
- T1–T3 faults after one side of the transaction, with neither side committed.
- T4 faults at operation start, feedback and CLOSE insertion, with no durable
  note, feedback or partial completion; later flush only preserves observation.
- Partial AMEND, individual omitted-subject proof and a normally billed later
  call, including a captured but no-longer-pending subject rejected at T1.
- An original AMEND target task CAS advanced through a legal owner ack,
  rejected before T1 reservation or first T2 dispatch with no new permission.
- Lifetime claim/link/metadata limits and original event tail capacity.
- Immutable authority relations and a missing unresolved projection.
- AMEND/cancel after T3, preserving original response and refusing stale effects.
- 410 maintenance upgrade, legacy history, 013 rollback and wrong asset bytes.

The two AMEND negative methods explicitly add a current task to an otherwise
pending-only trusted host capture, then recompile its original framed context.
The pending-membership case retains the normal pending set; the task-CAS case
uses a public owner acknowledgment to advance that added task without changing
the Dispatcher request CAS. This is malformed host-target coverage, not a
claim that the normal capture includes that current task or that the case has
executed. No fake success facts or observation for the rejected call are made.

All 17 methods are **NOT_RUN**. The Store tests are sequential; actual callback
overlap and coordinator lifecycle belong to the separately implemented runtime
suite. The full W00–W19 process-crash matrix, frozen old-binary rejection,
complete regression, sanitizer runs, Windows target execution and final SHA CI
are also **NOT_RUN**. A prepared fault case is not an executed failure or PASS.

## Static evidence and limits

Private evidence resides under the task's data disk, outside the checkout:
`evidence/n5-1-store-static-01/`, `evidence/n5-1-store-static-02/` and the AMEND
guard follow-up `evidence/n5-1-store-static-03/`. Checks used
Python SQLite 3.45.1, disposable private databases and no C++ execution.

The final 013 syntax and injected failure rollback restore 410 without new
tables. The migration activation barrier rejects UPDATE/DELETE, and its final
schema has a clean foreign-key check. An earlier version of this additive DDL
also exercised five immutable authority relations, the 64 member/8 receipt
limits, duplicate unresolved ownership and missing membership/reference
rejection. That earlier evidence retains its original scope; the final version
adds the immutable migration barrier and retains those unchanged constraints.

The original 68 static protocol SQL statements, plus the new AMEND live-team
query, were prepared with SQLite `EXPLAIN` in the follow-up check. All 69
statement parameter counts match their C++ initializer argument counts.
Text comparison preserved all eleven extracted original bodies, with only the
documented unresolved-ID filter in the claim query. 47 historical SQL/Schema
files match the design base byte for byte. All 36 resource assets, the compiled
catalog, registry hash, installer version and manifest agree. The final resource
input at this time is:

| Input | Bytes | SHA-256 |
| --- | ---: | --- |
| `013_event_call_association.sql` | 6135 | `141d80fe5bbdf67e91081d88a8405e2cf4a306fcef1d3fe1a029eaa9c183ac18` |
| Runtime assets | 166684 | Individual hashes in `resources/runtime-manifest.json` |
| Runtime manifest | See file | `f03bce7bb55b02f7740c81473c12301ecc437b35d980d629fb2d896196d4dc30` |

The initial historical-byte check tried to read `schemas/examples` as a file
and failed with `IsADirectoryError`; the corrected check considers regular
files. This was a static-script error, not a product test result. Original
receipts were preserved.

The follow-up static script initially included `db/README.md` and
`db/contract_store.py` in the historical path count, producing 49 rather than
the intended 47. Its path scope was corrected; the original failure record
and private database were retained. This also was a script error with no
C++ or product execution.

The first slice deliberately retains unresolved and unsupported work for explicit
review. Recovery cannot replace original authority, reapply an old settled
response, infer missing model text or claim that cancellation proves external
exit. There are no file/process effects, live/provider calls, production data
changes, second writer, daemon or downgrade SQL in this implementation. After
new effects, rollback must retain 411 facts and use controlled closeout rather
than restore a snapshot that loses those effects.
