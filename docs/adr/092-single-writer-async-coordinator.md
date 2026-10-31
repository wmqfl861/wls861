# ADR-092: One Store host with bounded asynchronous offline calls

Date: 2026-10-31. Authorized A3 slice; implementation baseline
`772feaee62086ddb0e7f605a7472721d3bc61406`, blueprint v0.4.1, Store 410.
Actual validation and publication are recorded separately. This decision does
not authorize supplier calls, paid APIs, a daemon or concurrent Store writers.

The selected design is option A from the v0.5.0 concurrency proposal: a single
host owns Store writes while model waits occur outside transactions. The new
`AsyncCoordinator` owns its Store and shared immutable SchemaRegistry. Only its
constructing host thread can capture, submit, poll, drain or borrow the Store.
M1 Scheduler callbacks receive an original captured ModelRequest and an offline
adapter; they cannot access Store through the coordinator. The synchronous
OfflineRuntime and EventRuntime entry points retain their default behavior.

Admission first allocates a bounded result holder, then reserves the complete
original call through `OfflineRuntime::reserve_captured_call`. No Actor, CAS,
revision, workflow target or epoch is refreshed while the call is queued. The
existing budget and frozen-input validation remain in Store. Model execution
requires `Store::claim_offline_call` in a separate short writer transaction.
It validates the original pending attempt and current authority, records a
unique `offline-dispatch:<attempt_id>` event, and returns `dispatch_claimed=true`
only once. An exact claim replay returns false; a reservation replay never
authorizes another invocation. Published SQL and historical bytes are unchanged.

The coordinator uses the existing M1 Scheduler with 4 management and 8 Worker
threads, reserving one thread and one admission position in each pool for
Review/Handoff. TeamLead calls use the management pool. Review denotes capacity
only, and cannot create a review ticket, verdict or independently reviewed
business output. The bounded host queue also reserves one position from ordinary
admission and gives controls their own bounded queue. Every admitted call keeps
one result position until its original response/feedback is durable; result
arrival cannot be rejected by a full business queue. A new claim uses the normal
Store event pool and may refuse after reservation if that pool is exhausted.
It never takes reserved cancellation or settlement capacity.

The host services controls, permission requests and ready results on each poll.
Controls carry a previously captured Root and CAS; cancellation/AMEND commit
before stop is requested from matching callbacks. Late results retain their
original tuple and nullable usage. Applying them uses the existing
OfflineRuntime final Store guards. Shared resource keys remain FIFO through
result commit, including conflicts between the two pools. No host lock or
transaction is held while waiting for a model.

Storage/feedback failures retain the original response in its admission slot
and permanently fence new business in that coordinator. The explicit evidence
retry calls original settlement/feedback recording only; it cannot repeat a
business tool, even if the first attempt committed an effect before feedback
failed. Adapter exceptions are explicit host observations with unknown usage,
not invented successful responses. Callback escape is communicated without
allocating another response on the worker, and the host either retains original
bytes or keeps the durable attempt unresolved.

Close requests stop and joins all Scheduler callbacks before releasing Store
ownership. Owner-thread close records late observations without business
application. Persistence failure remains visible and leaves export/retry data.
Foreign-thread destruction also stops and joins, but makes no unauthorized
Store calls: original pending attempts are left for normal UNKNOWN recovery.
It cannot guarantee persistence of response bytes never successfully written;
hosts that require those bytes must explicitly close/flush on the owner thread.
An adapter that ignores stop must still meet its declared finite timeout; the
host cannot safely destroy objects used by an unbounded callback.

This increment is a call-level S1 integration, not an automatic event pump. It
does not claim/ack inbox IDs; the existing event pump's captured completion rules
remain intact. It supports real S1 plan/order/ack/note and management transitions
with FakeModel. No WorkerTools object is attached; file/process actions and
file submission are explicitly unavailable. In particular, synchronous `run`
cannot occupy this host and invalidate the control-reachability claim. Live
network execution, long-tool phase splitting and automatic workflow selection
need separate integration and evidence. Fair request/team selection is the
subsequent A4 change inside M1 Scheduler, rather than a second scheduler.
