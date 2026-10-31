# ADR-093: Reuse M1 scheduling for fair S1 offline calls

Date: 2026-10-31. A4 follows the A3 candidate
`a2dfb46322d217f32eb5cc2516de6107c10db4b8` and [ADR-092](092-single-writer-async-coordinator.md).
Validation, independent review and publication remain separate evidence. No
live supplier, paid operation, daemon or second Store writer is authorized.

Add `Scheduler::submit_fair` to the existing M1 pool. Each work class has a ring
of outstanding requests; each request has a ring of outstanding teams. A newly
admitted group joins the tail. Selection traverses runnable classes, then
requests, then teams, rotating only the selected request/team. Blocked resource
keys consume no turn. Class round robin, ordinary execution/admission limits,
Review/Handoff reserves and cross-class resource FIFO retain their old rules.
The legacy `submit` uses one anonymous request/team group and therefore preserves
its existing class-local FIFO behavior.

The group rings are part of M1 Scheduler, not another queueing executor in S1.
Ring nodes are created with admission and removed when their last outstanding
queued/running job ends or is cancelled. Selection rotates existing list nodes
without allocation. Metadata is bounded by outstanding work. New requests cannot
continually cut ahead of a waiting request by resetting a service counter.

For currently runnable groups in one work class, a request receives at most one
selection before the next waiting runnable request. Teams rotate the same way
within that request. Each class receives a turn while it has runnable work and
its execution quota permits it. Shared-resource FIFO has precedence: a blocked
group cannot claim a hard elapsed-time bound. Running callbacks must finish or
cooperate with cancellation; a parent callback must not wait for its own child
on this pool. These are selection bounds, not a real-time service guarantee.

`AsyncCoordinator` submits the original captured request/team keys to this API.
It keeps A3's single host, original durable reservation, unique first claim and
result recording. The existing host guard additionally serializes matching
resource keys across management and Worker pools through result commit. No
Actor, task CAS, epoch, target, tool grant or UNKNOWN condition is refreshed to
make queued work runnable. Review remains a capacity class, not a business
review conclusion or a replacement for independently bound reviewer evidence.

New `AsyncCoordinatorLimits` are explicit host upper bounds, defaulting to 4
management threads, 8 Worker threads and 1/1 permanent Review/Handoff reserves.
Construction validates finite positive bounds and reserve relationships.
Admission checks that these host totals do not exceed the original request's
frozen active maxima and do not reduce its frozen review reserves. Incompatible
requests refuse before reservation. A smaller explicitly selected host is
allowed; old snapshots are neither upgraded nor rewritten. TeamLead calls share
the management pool. Live endpoint quotas remain outside this offline slice.

The single bounded business/result capacity also excludes the sum of both role
reserves from ordinary admission. This refines A3's one global reserved place:
ordinary saturation must leave room for both a management and a Worker review
call. Once accepted, every call owns its result capacity until durable settlement
and feedback, including retained failures. Review/Handoff compete within their
reserved class; the system does not promise unlimited review admission.
Control capacity remains independent, and all Store storage/settlement limits
and captured command guards remain unchanged.

Diagnostics report bounded queue, awaiting-claim, executing, ready-result and
resource-blocked counts; original pool totals/reserves; a coarse block reason;
and how many granted dispatches a currently queued call has waited through.
Counters saturate instead of wrapping. Diagnostics contain no captured model
text, tool arguments or filesystem paths. They are observations, not authority.

The S1 tests keep real Store reservations, responses, feedback and operation
counts. Three ordinary managers and seven ordinary Workers occupy their pools
while a review in each role starts; another normal call in each role stays
queued. A deterministic one-slot release observes request rotation followed by
team rotation, and another test proves a shared key serializes across pools.
Synthetic Error responses in the fairness test preserve task CAS while still
consuming original attempts and recording actual S1 outcomes; they are explicitly
authorized attempts, not automatic retries. The same narrow fairness test can
link against A3 to demonstrate FIFO ordering without conflating missing APIs,
ABI changes or model quality with the scheduling property.
