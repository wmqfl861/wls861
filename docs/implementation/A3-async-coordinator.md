# A3 asynchronous offline Store host

The opt-in library API is `wls::s1::AsyncCoordinator` in
`include/wls/async_coordinator.hpp`; design and limits are in [ADR-092](../adr/092-single-writer-async-coordinator.md).
The existing CLI and synchronous event pump remain available. Runtime/Store
versions, published SQL and frozen request formats are unchanged.

The host transfers a `unique_ptr<Store>` and shared immutable schemas into the
coordinator. On that same thread it admits requests through `store()`, captures
one current call, and submits the captured value plus an offline adapter. It
drives `poll()` or `drain()` while model callbacks overlap in the original M1
Scheduler. Public Store borrowing is a trusted-host facility, not a remote or
model authorization interface; aliases must not escape ownership or be used
concurrently. Never run a previously coordinated attempt through a second
synchronous adapter path. Fresh follow-up calls are captured explicitly after
observing durable previous outcomes.

`submit` returns a future of the original S1 business result. Before returning,
it stores the complete reservation and holds a result slot. A callback asks the
host for first-dispatch permission before invoking its adapter. Replayed claims,
old Store sessions, AMEND, cancellation, waits, UNKNOWN and revoked policy cannot
be replaced by a fresh Actor to make the old call executable. The result then
passes the original settlement, Schema, captured target and final Store guards.
Managers never acquire Worker tool authority.

`cancel` and `amend` can be enqueued from another thread. Both take an already
captured Root identity, original CAS and command ID. Their bounded capacity is
independent of queued/running/retained business calls. Completion futures mean
the actual Store control transaction ended, not that a process exited or an
effect was undone. A failed stale control does not refresh its captured values.

The A3 baseline bounds are 64 outstanding calls, including results, and 16
controls, with one global Review/Handoff admission position. In the integrated
[A4 refinement](A4-fair-scheduling.md), ordinary global admission preserves both
management and Worker reserves (two positions with the default 1/1 reserves).
The host limits are explicit and validated against each original frozen policy.
The M1 management/Worker pools have 4/8 threads and 1/1 reserved positions.
Waiting on users or dependencies is durable workflow state and occupies no new
call slot until the host explicitly captures and submits another call. Review
class is not independent business review. Matching resource keys serialize even
across pools; unrelated requests can complete while other models wait.

If settlement or feedback cannot be persisted, `snapshot().settlement_halted`
becomes true and `drain()` reports failure once all running work has returned.
`retained_results()` returns the exact captured request and received response,
or an explicit null when no response was retained. These are private original
evidence, not safe public logs. The result still occupies its admission slot.
After repairing the underlying storage issue, `flush_retained_evidence()` may
save the original observation; it never applies business again or clears the
coordinator's business fence. The caller must observe the failed future and use
the existing recovery/reconciliation paths as appropriate. Unknown usage stays
unknown. A host crash or foreign-thread destruction leaves unresolved original
attempts for normal takeover recovery, without repeating their model execution.

`close()` is an explicit owner-thread operation. It stops admission, requests
callback stop, joins both pools, and saves remaining evidence without applying
business. The Store and adapter lifetimes extend through all callback exits.
Destruction on an unrelated thread joins safely but cannot substitute for
owner-thread evidence persistence. Running callbacks must be bounded and
cooperative; no claim of hard real-time cancellation is made for arbitrary
trusted C++ adapters. Physical storage exhaustion can prevent persistence, so
the raw bytes are retained while the coordinator is alive and unresolved durable
reservations remain after restart.

This slice has no file/process executor or auto inbox consumer. `run`, file
tools and file submission are rejected before reservation when present in a
captured declaration. Existing WorkerTools first-dispatch, workspace, process
exit, cleanup ownership and UNKNOWN rules remain their own boundaries. Nothing
in this API authorizes a real supplier request or proves process exit/cleanup.

`async_coordinator_tests` is one native suite with deterministic gates and real
temporary SQLite state. It observes simultaneous waits and a third committed S1
request; a complete plan/order/ack/note chain; unique model execution and operation
counts; full queues with control and result capacity; AMEND/cancellation and raw
usage; Store sessions, waits, UNKNOWN and first-claim rollback; failed original
settlement and failed feedback after a committed effect; adapter exceptions;
safe close/foreign-thread destruction; and restart preserving UNKNOWN. Actual
commands, exits and binary/source identities belong in the task validation
report, not this capability description.
