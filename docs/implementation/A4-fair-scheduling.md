# A4 S1 request/team scheduling

The implementation refines [A3](A3-async-coordinator.md) through the existing M1
Scheduler. See [ADR-093](../adr/093-request-team-fair-scheduling.md). It is an
opt-in offline S1 library path; the synchronous event pump, CLI behavior, Store
schema, old frozen snapshots and original live send protocol remain compatible.

`Scheduler::submit_fair(job_id, resource_key, request_key, team_key, class, work)`
uses request/team round robin inside the original class round robin. Both group
keys are bounded and nonempty. New groups join the tail; queued/running groups
remain until their last job leaves. The first runnable team in the next runnable
request receives a turn. Matching resource keys retain FIFO across classes.
Legacy `submit` retains its original anonymous-group FIFO behavior. This is one
M1 scheduling implementation with an opt-in grouping API, not a replacement
pool or a second global scheduler.

The S1 coordinator sends each captured binding's request and team keys directly
to M1; a Dispatcher uses its request as its team key. It retains a shared host
resource order across management/Worker pools. Store first-claim validation is
still required after a callback becomes eligible, and original response/usage
settlement precedes final business application. Queue fairness never grants a
new Actor, updates old CAS values or clears AMEND, waits or UNKNOWN.

`AsyncCoordinatorLimits` describes host capacity. Defaults are management 4,
Worker 8, review reserves 1/1, 64 business/result slots and 16 control slots.
Construction rejects invalid or excessive bounds. Submission compares host
totals/reserves to the original request's frozen system limits: host maxima may
be smaller, and host reserves may be larger, but neither may weaken the frozen
policy. Refusal happens before a new attempt is persisted. No implicit request
configuration migration occurs. TeamLead shares management capacity. These
counts are offline callback slots, not a live endpoint concurrency guarantee.

Ordinary global admission is bounded by `business_capacity - management_reserved
- worker_reserved`. Each M1 pool also preserves its own admission and execution
reserves. Thus ordinary saturation cannot consume the room intended for both
roles. Review/Handoff share the reserve; being scheduled as Review provides no
independent-review business authority. Already admitted results retain their
own slots and controls have their own independent bounded queue.

Snapshots expose each pool's total, queued, running, ordinary running and
reserved counts, plus host queued/awaiting/executing/ready/retained counts. Shared
resource blocking and a coarse `blocked_reason` distinguish resource conflict,
execution capacity, settlement failure and closure. `dispatches` counts granted
original permissions; `max_wait_dispatches` is the largest number of such grants
since admission among currently queued/awaiting calls. It is a finite observed
work measure and does not promise elapsed-time fairness for blocked resources or
uncooperative callbacks. The snapshot contains no original model context.

`async_coordinator_tests` remains one suite. A4 adds actual S1 coverage for
simultaneous ordinary saturation and both Review reserves; busy-request then
team rotation under a deterministic one-slot release; cross-pool resource FIFO;
and explicit host bounds without frozen-byte changes. Existing M1 scheduler
tests remain the compatibility guard. The narrow fairness counterexample uses
the same source and business conditions with A3 and A4 libraries, compiled
against each corresponding header to avoid an ABI mismatch from the new
diagnostic fields. Executed outcomes, raw commands, exit codes and identities
belong in the validation report, separate from these implemented contracts.
