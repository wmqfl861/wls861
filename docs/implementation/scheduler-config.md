# M1 scheduler and configuration contracts

## Bounded synthetic scheduler

`wls::Scheduler(workers, capacity, reserved)` owns a fixed pool of standard C++
threads. `capacity` bounds all accepted, unfinished jobs, including queued and
running callbacks. Normal work is limited to `capacity - reserved` outstanding
jobs and `workers - reserved` running callbacks. Review and handoff share the
protected capacity and may also use otherwise available slots. A full scheduler
rejects explicitly; it never creates unbounded threads or a hidden overflow
queue. `0 < reserved < workers` and `reserved < capacity` are required, so the
default reserve rejects a single-worker construction.

Admission returns a `future<void>`. Empty IDs/keys, missing callbacks, invalid
classes, outstanding duplicate IDs, full capacity and closed admission produce
typed `SchedulerError` codes. A duplicate is tracked only until its original
callback finishes; this scheduler is not the durable operation-idempotency
store. The TaskEngine is responsible for authorization and operation versions.

The scheduler selects among runnable Normal, Review and Handoff queues by
round robin. A continuously runnable class receives a turn after at most two
selections from other classes. Callbacks with the same resource key run in
submission order across all classes; blocked keys do not block unrelated keys.
This per-resource FIFO prevents newer work from repeatedly overtaking an older
blocked job. Selection scans a bounded queue rather than allocating an
unbounded set of resource locks. Workers wait on a condition variable when no
callback can start; no polling loop or sleeps are used for scheduling.

Callbacks execute outside the scheduler mutex. Callback exceptions, including
non-standard exceptions, propagate through the corresponding future. Completion
bookkeeping precedes future readiness. `close(true)` stops admission, drains
accepted work and joins every thread. `close(false)` cancels queued work with
`SchedulerErrorCode::Cancelled`, allows running callbacks to finish, then joins.
Concurrent closes are serialized and the first call chooses the mode. Closing
again is safe. A worker attempting blocking close receives `InvalidShutdown`;
the host must retain the scheduler until all callbacks finish and destroy it
from a non-worker thread.

This is non-preemptive scheduling. A review cannot interrupt a running normal
callback that holds the same resource. An indefinitely blocked callback can
prevent completion; arbitrary callbacks that synchronously wait for jobs in the
same bounded pool can also deadlock. The reserved slot does not promise bounded
latency when running callbacks do not return, when earlier same-key work cannot
run, or when review/handoff already occupy the entire reserve. The contract is
bounded admission, reserved capacity, fairness among runnable work and resource
serialization, not process isolation or forced cancellation. Callbacks in M1
are synthetic fixtures, not actual business tools.

## Frozen model defaults

`ModelSelection` contains independent `model` and `reasoning` fields.
`ReasoningSetting` distinguishes `Inherit`, `Disabled`, `ProviderDefault`, and
`Explicit`; only `Explicit` carries a nonempty effort string. A frozen Root
snapshot must name a model and use a non-inherited reasoning setting. A role
override can omit the model (empty string), the reasoning (`Inherit`), or both.

`resolve_model(frozen_root, role_override, registry)` first validates the Root
snapshot, then resolves each missing field directly from that snapshot. It has
no intermediate-role argument, so a TeamLead/Dispatcher override cannot
implicitly become a Worker's default. The caller must persist and pass the
request's Root snapshot, not current mutable Root settings.

`CapabilityRegistry` is symbolic host-owned configuration. Each model explicitly
declares its supported effort strings and whether disabled/default modes are
supported. Unknown models, invalid settings and unsupported combinations throw
typed `ConfigurationError`; no reasoning mode or effort is silently mapped.
For example, overriding only a model still inherits Root reasoning and rejects
if that reasoning is unsupported by the selected model. The registry has no
provider endpoint, credential or network behavior. Populate it before sharing
it across resolver callers; concurrent registry mutation is not supported.

## Tool authority

`effective_tool_grants` intersects the role capability allowlist, system
allowlist and request allowlist, and grants business tools only to `Role::Worker`.
`tool_allowed` checks an individual tool against the same rule. Empty names and
unrecognized roles deny. Root, Dispatcher and TeamLead always receive an empty
business-tool set even if all supplied lists contain a tool. Changing a model
or its reasoning setting never changes these grants. Trusted host code supplies
the lists; model output is not a grant or a host identity credential.

## Focused verification

`tests/scheduler_tests.cpp` checks same-key FIFO/exclusion, independent-key actual
overlap, admission and worker-slot protection for both control classes, class
rotation with an existing backlog, queued cancellation, draining, exception
propagation, callbacks outside the queue lock and explicit admission failures.
Promise gates establish concurrency ordering; timeout checks are deadlock
watchdogs, not timing-based correctness assertions.

`tests/config_tests.cpp` checks independent inheritance from an unchanged Root
snapshot despite later Root and intermediate-role changes, all four reasoning
modes, invalid/unsupported settings, unknown models, three-way permission
intersection and management-role denials. These are M1 synthetic checks only.
