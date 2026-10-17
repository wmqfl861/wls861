#include "kernel_fixture.hpp"
#include "wls/journal.hpp"

#include <atomic>
#include <barrier>
#include <fstream>
#include <thread>

using namespace wls;
using namespace fixture;

namespace {
void admission_is_durable_and_scoped() {
    test::TempDir dir; auto path = dir.path / "state.journal";
    auto in = input(); std::string id;
    {
        TaskEngine engine(path, {}, {"test.echo"}); auto q = engine.submit(root(in), in); id = q.request_id;
        CHECK(q.state == RequestState::Queued); CHECK(q.resources == ResourceState::Starting);
        CHECK(engine.events().size() == 1); CHECK(engine.submit(root(in), in).request_id == id);
        auto different = in; different.goal = "other goal";
        error(ErrorCode::Conflict, [&] { engine.submit(root(different), different); });
        auto amended = engine.amend(id, command(q, Role::Root, "amend"), "new goal");
        CHECK(amended.requirements_revision == 2);
        CHECK(engine.submit(root(in), in).goal == "new goal");
        auto other_project = in; other_project.project_id = "other-project";
        CHECK(engine.submit(root(other_project), other_project).request_id != id);
        auto other_tenant = in; other_tenant.tenant_id = "other-tenant";
        CHECK(engine.submit(root(other_tenant), other_tenant).request_id != id);
    }
    TaskEngine restarted(path, {}, {"test.echo"}); auto restored = restarted.submit(root(in), in);
    CHECK(restored.request_id == id); CHECK(restored.goal == "new goal");
    CHECK(restored.original_input == in); CHECK(restarted.requests().size() == 3);
    CHECK(restored.lease_epoch == 2); CHECK(restored.requirements_revision == 2);
}

void command_idempotency_and_atomic_events() {
    Kernel f; auto q = f.submit(); auto cmd = f.cmd(q, Role::Dispatcher);
    auto result = f.engine.plan(q.request_id, cmd, "first plan"); auto count = f.engine.events().size();
    CHECK(f.engine.plan(q.request_id, cmd, "first plan").state_version == result.state_version);
    CHECK(f.engine.events().size() == count);
    error(ErrorCode::Conflict, [&] { f.engine.plan(q.request_id, cmd, "different plan"); });
    auto stale = f.cmd(q, Role::Dispatcher);
    error(ErrorCode::Conflict, [&] { f.engine.plan(q.request_id, stale, "stale plan"); });
    CHECK(f.engine.get(q.request_id).state_version == result.state_version);
    CHECK(f.engine.events().back().kind == "CommandRejected");
    auto events = f.engine.events(q.request_id, 1); CHECK(events.size() == 2);
    CHECK(events[0].state_version == result.state_version);
    CHECK(events[0].request_id == result.request_id); CHECK(events[0].lease_epoch == result.lease_epoch);
}

void role_and_team_boundaries() {
    Kernel f; auto q = f.prepare(f.submit());
    for (auto role : {Role::Root, Role::Dispatcher, Role::TeamLead}) {
        auto cmd = f.cmd(q, role, role == Role::TeamLead ? "builder" : "");
        error(ErrorCode::Unauthorized, [&] { f.engine.prepare_operation(q.request_id, cmd, "op", "test.echo", "payload"); });
    }
    error(ErrorCode::Unauthorized, [&] { f.engine.create_team(q.request_id, f.cmd(q, Role::Worker, "builder"), "illegal"); });
    auto wrong = f.cmd(q, Role::Worker, "builder"); wrong.actor.principal_id = "forged-worker";
    error(ErrorCode::Unauthorized, [&] { f.engine.prepare_operation(q.request_id, wrong, "op", "test.echo", "payload"); });
    auto foreign = f.prepare(f.submit("second"));
    error(ErrorCode::Unauthorized, [&] { f.engine.prepare_operation(foreign.request_id, f.cmd(q, Role::Worker, "builder"), "op", "test.echo", "payload"); });
    error(ErrorCode::Unauthorized, [&] { f.engine.prepare_operation(q.request_id, f.cmd(q, Role::Worker, "builder"), "op", "shell.exec", "payload"); });
    CHECK(f.operation(q).operations.size() == 1);
}

void amend_requires_new_plan_explanation_and_ack() {
    Kernel f; auto q = f.prepare(f.submit()); auto old = q;
    q = f.operation(q); auto operation_id = q.operations.begin()->first;
    q = f.engine.amend(q.request_id, f.cmd(q, Role::Root), "changed requirements");
    CHECK(q.requirements_revision == 2); CHECK(q.teams.at("builder").applied_revision == 1);
    CHECK(q.operations.at(operation_id).state == OperationState::Cancelled);
    CHECK(q.dispatcher_id == old.dispatcher_id); CHECK(f.engine.requests().size() == 1);
    error(ErrorCode::StaleRequirements, [&] { f.operation(q, "new-op"); });
    error(ErrorCode::InvalidState, [&] { f.engine.instruct_team(q.request_id, f.cmd(q, Role::TeamLead, "builder"), "use old plan"); });
    auto stale = f.cmd(old, Role::Dispatcher);
    error(ErrorCode::StaleRequirements, [&] { f.engine.plan(q.request_id, stale, "late model response"); });
    q = f.engine.plan(q.request_id, f.cmd(q, Role::Dispatcher), "plan revised for new requirements");
    CHECK(q.plan_revision == 2); CHECK(q.plan_requirements_revision == 2);
    error(ErrorCode::StaleRequirements, [&] { f.engine.acknowledge_applied(q.request_id, f.cmd(q, Role::Worker, "builder")); });
    q = f.engine.instruct_team(q.request_id, f.cmd(q, Role::TeamLead, "builder"), "stop A; keep B; add C");
    error(ErrorCode::StaleRequirements, [&] { f.operation(q, "new-op"); });
    q = f.engine.acknowledge_applied(q.request_id, f.cmd(q, Role::Worker, "builder"));
    CHECK(q.teams.at("builder").applied_revision == 2); CHECK(f.operation(q, "new-op").operations.size() == 2);
}

void instruction_change_fences_old_operations() {
    Kernel f; auto q = f.operation(f.prepare(f.submit())); auto old_op = q.operations.begin()->first;
    q = f.engine.instruct_team(q.request_id, f.cmd(q, Role::TeamLead, "builder"), "different instructions, same overall plan");
    CHECK(q.teams.at("builder").instructions_version == 2);
    CHECK(q.operations.at(old_op).state == OperationState::Cancelled);
    error(ErrorCode::StaleRequirements, [&] { f.operation(q, "second"); });
    q = f.engine.acknowledge_applied(q.request_id, f.cmd(q, Role::Worker, "builder"));
    error(ErrorCode::InvalidState, [&] { f.engine.start_operation(q.request_id, f.cmd(q, Role::Worker, "builder"), old_op); });
    q = f.operation(q, "second");
    auto next_op = q.request_id + ":op:2";
    q = f.engine.start_operation(q.request_id, f.cmd(q, Role::Worker, "builder"), next_op);
    q = f.engine.instruct_team(q.request_id, f.cmd(q, Role::TeamLead, "builder"), "stop an in-flight action");
    CHECK(q.operations.at(next_op).state == OperationState::Unknown);
    error(ErrorCode::UnknownOutcome, [&] { f.engine.acknowledge_applied(q.request_id, f.cmd(q, Role::Worker, "builder")); });
}
void replace_and_new_have_distinct_semantics() {
    Kernel f; auto old = f.prepare(f.submit());
    auto new_request = f.submit("independent");
    CHECK(f.engine.get(old.request_id).state == RequestState::Running);
    CHECK(new_request.replaces.empty());
    auto cmd = f.cmd(old, Role::Root); auto replacement_input = input("replacement");
    replacement_input.goal = "replacement goal";
    auto replacement = f.engine.replace(old.request_id, cmd, replacement_input);
    auto stopped = f.engine.get(old.request_id);
    CHECK(stopped.state == RequestState::Superseded); CHECK(stopped.resources == ResourceState::Draining);
    CHECK(stopped.replaced_by == replacement.request_id); CHECK(replacement.replaces == old.request_id);
    CHECK(replacement.dispatcher_id != stopped.dispatcher_id); CHECK(stopped.lease_epoch == old.lease_epoch + 1);
    CHECK(f.engine.replace(old.request_id, cmd, replacement_input).request_id == replacement.request_id);
    CHECK(f.engine.requests().size() == 3);
    error(ErrorCode::StaleEpoch, [&] { f.engine.plan(old.request_id, f.cmd(old, Role::Dispatcher), "late"); });
}

void cancellation_does_not_hide_unknown_outcomes() {
    Kernel f; auto q = f.operation(f.prepare(f.submit())); auto op = q.operations.begin()->first;
    q = f.engine.start_operation(q.request_id, f.cmd(q, Role::Worker, "builder"), op); auto before_cancel = q;
    auto cancel_cmd = f.cmd(q, Role::Root); q = f.engine.cancel(q.request_id, cancel_cmd);
    CHECK(q.state == RequestState::Cancelled); CHECK(q.operations.at(op).state == OperationState::Unknown);
    CHECK(f.engine.cancel(q.request_id, cancel_cmd).state_version == q.state_version);
    error(ErrorCode::StaleEpoch, [&] { f.engine.finish_operation(q.request_id, f.cmd(before_cancel, Role::Worker, "builder"), op, OperationState::Succeeded, "late receipt"); });
    error(ErrorCode::UnknownOutcome, [&] { f.engine.advance_resources(q.request_id, q.state_version, "stop-early", ResourceState::Stopped); });
    q = f.engine.reconcile_operation(q.request_id, q.state_version, "reconciled", op, OperationState::Succeeded, "fixture reconciliation");
    CHECK(q.state == RequestState::Cancelled);
    q = f.engine.advance_resources(q.request_id, q.state_version, "stopped", ResourceState::Stopped);
    CHECK(q.resources == ResourceState::Stopped); CHECK(q.operations.at(op).state == OperationState::Succeeded);
}

void operation_idempotency_is_not_automatic_retry() {
    Kernel f; auto q = f.operation(f.prepare(f.submit())); auto op = q.operations.begin()->first;
    auto original_size = q.operations.size(); q = f.operation(q);
    CHECK(q.operations.size() == original_size);
    error(ErrorCode::Conflict, [&] { f.engine.prepare_operation(q.request_id, f.cmd(q, Role::Worker, "builder"), "effect-1", "test.echo", "different payload"); });
    q = f.engine.start_operation(q.request_id, f.cmd(q, Role::Worker, "builder"), op);
    error(ErrorCode::InvalidState, [&] { f.engine.start_operation(q.request_id, f.cmd(q, Role::Worker, "builder"), op); });
    q = f.engine.finish_operation(q.request_id, f.cmd(q, Role::Worker, "builder"), op, OperationState::Unknown, "receipt lost");
    q = f.operation(q); CHECK(q.operations.size() == 1); CHECK(q.operations.at(op).state == OperationState::Unknown);
    error(ErrorCode::UnknownOutcome, [&] { f.operation(q, "new-key"); });
    error(ErrorCode::InvalidState, [&] { f.engine.finish_operation(q.request_id, f.cmd(q, Role::Worker, "builder"), op, OperationState::Succeeded, "self-reported"); });
    q = f.engine.reconcile_operation(q.request_id, q.state_version, "host-reconcile", op, OperationState::Succeeded, "host-verified-fixture");
    q = f.operation(q); CHECK(q.operations.at(op).state == OperationState::Succeeded); CHECK(q.operations.size() == 1);
    CHECK(f.operation(q, "new-key").operations.size() == 2);
}

void restart_automatically_fences_dispatch_and_requires_reapplication() {
    test::TempDir dir; auto path = dir.path / "state.journal"; auto in = input(); RequestSnapshot saved;
    std::string op;
    {
        TaskEngine e(path, {}, {"test.echo"}); auto q = e.submit(root(in), in);
        q = e.plan(q.request_id, command(q, Role::Dispatcher, "plan"), "fixture plan");
        q = e.create_team(q.request_id, command(q, Role::Dispatcher, "team"), "builder");
        q = e.instruct_team(q.request_id, command(q, Role::TeamLead, "instruct", "builder"), "fixture instructions");
        q = e.acknowledge_applied(q.request_id, command(q, Role::Worker, "apply", "builder"));
        q = e.prepare_operation(q.request_id, command(q, Role::Worker, "prepare", "builder"), "key", "test.echo", "payload");
        op = q.operations.begin()->first;
        saved = e.start_operation(q.request_id, command(q, Role::Worker, "start", "builder"), op);
    }
    TaskEngine e(path, {}, {"test.echo"}); auto q = e.get(saved.request_id);
    CHECK(q.lease_epoch == saved.lease_epoch + 1); CHECK(q.operations.at(op).state == OperationState::Unknown);
    error(ErrorCode::StaleEpoch, [&] { e.finish_operation(q.request_id, command(saved, Role::Worker, "late-finish", "builder"), op, OperationState::Succeeded, "late"); });
    error(ErrorCode::UnknownOutcome, [&] { e.acknowledge_applied(q.request_id, command(q, Role::Worker, "too-early", "builder")); });
    q = e.reconcile_operation(q.request_id, q.state_version, "settled", op, OperationState::Succeeded, "known receipt");
    error(ErrorCode::StaleEpoch, [&] { e.prepare_operation(q.request_id, command(q, Role::Worker, "too-early-2", "builder"), "next", "test.echo", "payload"); });
    q = e.instruct_team(q.request_id, command(q, Role::TeamLead, "reinstruct", "builder"), "resume after reconciliation");
    error(ErrorCode::StaleRequirements, [&] { e.prepare_operation(q.request_id, command(q, Role::Worker, "too-early-3", "builder"), "next", "test.echo", "payload"); });
    q = e.acknowledge_applied(q.request_id, command(q, Role::Worker, "reapply", "builder"));
    q = e.prepare_operation(q.request_id, command(q, Role::Worker, "next", "builder"), "next", "test.echo", "payload");
    CHECK(q.operations.size() == 2); CHECK(q.original_input.root_defaults == in.root_defaults);
    auto epoch = q.lease_epoch; auto expected = q.state_version;
    q = e.recover(q.request_id, expected, "explicit-recover"); CHECK(q.lease_epoch == epoch + 1);
    CHECK(e.recover(q.request_id, expected, "explicit-recover").lease_epoch == q.lease_epoch);
}

void review_and_new_candidate_invalidate_old_acceptance() {
    Kernel f; auto q = f.prepare(f.submit(), true);
    auto candidate_a = f.candidate(q);
    q = f.engine.submit_candidate(q.request_id, f.cmd(q, Role::Worker, "builder"), candidate_a);
    error(ErrorCode::Unauthorized, [&] { f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "builder"), candidate_a, true, "self review"); });
    q = f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "reviewer"), candidate_a, false, "valid issues report");
    CHECK(q.reviews.size() == 1); CHECK(!q.accepted); CHECK(q.state == RequestState::Running);
    error(ErrorCode::InvalidState, [&] { f.engine.accept(q.request_id, f.cmd(q, Role::Dispatcher)); });
    q = f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "reviewer"), candidate_a, true, "passing fixture review");
    q = f.engine.accept(q.request_id, f.cmd(q, Role::Dispatcher));
    q = f.engine.prepare_handoff(q.request_id, f.cmd(q, Role::Dispatcher), "package-a");
    auto candidate_b = candidate_a; candidate_b.artifact_version = 2; candidate_b.content_hash = std::string(64, 'b');
    q = f.engine.submit_candidate(q.request_id, f.cmd(q, Role::Worker, "builder"), candidate_b);
    CHECK(q.state == RequestState::Submitted); CHECK(!q.accepted); CHECK(q.package_ref.empty());
    error(ErrorCode::Conflict, [&] { f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "reviewer"), candidate_a, true, "old"); });
    error(ErrorCode::Conflict, [&] { f.engine.confirm_delivery(q.request_id, f.cmd(q, Role::Root), candidate_a, "package-a", "old receipt"); });
    error(ErrorCode::InvalidState, [&] { f.engine.accept(q.request_id, f.cmd(q, Role::Dispatcher)); });
}

void artifact_versions_survive_switches_and_amendments() {
    Kernel f; auto q = f.prepare(f.submit()); auto a = f.candidate(q);
    q = f.engine.submit_candidate(q.request_id, f.cmd(q, Role::Worker, "builder"), a);
    auto b = a; b.artifact_id = "another-artifact";
    q = f.engine.submit_candidate(q.request_id, f.cmd(q, Role::Worker, "builder"), b);
    a.content_hash = std::string(64, 'c');
    error(ErrorCode::Conflict, [&] { f.engine.submit_candidate(q.request_id, f.cmd(q, Role::Worker, "builder"), a); });
    q = f.engine.amend(q.request_id, f.cmd(q, Role::Root), "new direction"); CHECK(!q.candidate);
    q = f.engine.plan(q.request_id, f.cmd(q, Role::Dispatcher), "new plan"); q = f.apply(q);
    a.requirements_revision = q.requirements_revision;
    error(ErrorCode::Conflict, [&] { f.engine.submit_candidate(q.request_id, f.cmd(q, Role::Worker, "builder"), a); });
    a.artifact_version = 2;
    CHECK(f.engine.submit_candidate(q.request_id, f.cmd(q, Role::Worker, "builder"), a).candidate->artifact_version == 2);
}

void all_assigned_reviewers_are_required() {
    Kernel f; auto q = f.prepare(f.submit(), true);
    q = f.engine.create_team(q.request_id, f.cmd(q, Role::Dispatcher), "reviewer-two", true); q = f.apply(q, "reviewer-two");
    q = f.engine.submit_candidate(q.request_id, f.cmd(q, Role::Worker, "builder"), f.candidate(q));
    q = f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "reviewer"), *q.candidate, false, "blocking evidence");
    q = f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "reviewer-two"), *q.candidate, true, "pass two");
    error(ErrorCode::InvalidState, [&] { f.engine.accept(q.request_id, f.cmd(q, Role::Dispatcher)); });
    q = f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "reviewer"), *q.candidate, true, "pass one");
    CHECK(f.engine.accept(q.request_id, f.cmd(q, Role::Dispatcher)).accepted);
}
void adverse_review_revokes_pending_delivery_even_at_quota() {
    EngineLimits limits; limits.max_events = 10; Kernel f(limits);
    auto q = f.approved(f.prepare(f.submit(), true));
    q = f.engine.prepare_handoff(q.request_id, f.cmd(q, Role::Dispatcher), "approved-package");
    auto candidate = *q.candidate;
    error(ErrorCode::QuotaExceeded, [&] { f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "reviewer"), candidate, true, "redundant positive review"); });
    q = f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "reviewer"), candidate, false, "new blocking evidence");
    CHECK(!q.accepted); CHECK(q.package_ref.empty()); CHECK(q.state == RequestState::Running);
    CHECK(f.engine.events().back().kind == "AcceptanceRevoked");
    error(ErrorCode::Conflict, [&] { f.engine.confirm_delivery(q.request_id, f.cmd(q, Role::Root), candidate, "approved-package", "obsolete receipt"); });
    error(ErrorCode::QuotaExceeded, [&] { f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "reviewer"), candidate, false, "another report"); });
    CHECK(f.engine.cancel(q.request_id, f.cmd(q, Role::Root)).state == RequestState::Cancelled);
}

void review_instruction_changes_require_fresh_review() {
    Kernel f; auto q = f.approved(f.prepare(f.submit(), true));
    q = f.engine.instruct_team(q.request_id, f.cmd(q, Role::TeamLead, "reviewer"), "check newly discovered issue before release");
    CHECK(!q.accepted); CHECK(q.state == RequestState::Verifying);
    error(ErrorCode::InvalidState, [&] { f.engine.accept(q.request_id, f.cmd(q, Role::Dispatcher)); });
    q = f.engine.acknowledge_applied(q.request_id, f.cmd(q, Role::Worker, "reviewer"));
    q = f.engine.record_review(q.request_id, f.cmd(q, Role::Worker, "reviewer"), *q.candidate, true, "fresh evidence");
    CHECK(f.engine.accept(q.request_id, f.cmd(q, Role::Dispatcher)).accepted);
}

void delivery_and_cleanup_are_separate_state_dimensions() {
    Kernel f; auto q = f.approved(f.prepare(f.submit(), true));
    error(ErrorCode::InvalidState, [&] { f.engine.advance_resources(q.request_id, q.state_version, "early-stop", ResourceState::Stopped); });
    q = f.engine.prepare_handoff(q.request_id, f.cmd(q, Role::Dispatcher), "fixture-package"); auto original = q;
    error(ErrorCode::Conflict, [&] { f.engine.confirm_delivery(q.request_id, f.cmd(q, Role::Root), *q.candidate, "wrong-package", "receipt"); });
    CHECK(f.engine.get(q.request_id).package_ref == original.package_ref);
    CHECK(f.engine.get(q.request_id).resources == ResourceState::Active);
    q = f.engine.confirm_delivery(q.request_id, f.cmd(q, Role::Root), *q.candidate, "fixture-package", "synthetic-receipt");
    CHECK(q.state == RequestState::Completed); CHECK(q.resources == ResourceState::Draining);
    q = f.engine.advance_resources(q.request_id, q.state_version, "stop", ResourceState::Stopped);
    q = f.engine.advance_resources(q.request_id, q.state_version, "clean", ResourceState::Cleaning);
    q = f.engine.advance_resources(q.request_id, q.state_version, "failure", ResourceState::CleanupFailed);
    CHECK(q.state == RequestState::Completed); CHECK(q.receipt_ref == "synthetic-receipt");
    q = f.engine.advance_resources(q.request_id, q.state_version, "retry-clean", ResourceState::Cleaning);
    q = f.engine.advance_resources(q.request_id, q.state_version, "closed", ResourceState::Closed);
    CHECK(q.state == RequestState::Completed); CHECK(q.resources == ResourceState::Closed);
    CHECK(std::filesystem::exists(f.directory.path / "state.journal")); // No real deletion by the kernel.
}

void ordinary_quota_never_blocks_settlement_cancel_or_reopen() {
    EngineLimits limits; limits.max_events = 7; test::TempDir dir; auto path = dir.path / "state.journal";
    RequestSnapshot saved;
    {
        TaskEngine e(path, limits, {"test.echo"}); auto in = input(); auto q = e.submit(root(in), in);
        q = e.plan(q.request_id, command(q, Role::Dispatcher, "plan"), "plan");
        q = e.create_team(q.request_id, command(q, Role::Dispatcher, "team"), "builder");
        q = e.instruct_team(q.request_id, command(q, Role::TeamLead, "instruct", "builder"), "instructions");
        q = e.acknowledge_applied(q.request_id, command(q, Role::Worker, "apply", "builder"));
        q = e.prepare_operation(q.request_id, command(q, Role::Worker, "prepare", "builder"), "key", "test.echo", "payload");
        q = e.start_operation(q.request_id, command(q, Role::Worker, "start", "builder"), q.operations.begin()->first);
        CHECK(e.events().size() == 7);
        error(ErrorCode::QuotaExceeded, [&] { e.plan(q.request_id, command(q, Role::Dispatcher, "another-plan"), "more planning"); });
        // A diagnostic flood has its own bound and cannot consume settlement capacity.
        auto bad = command(q, Role::Dispatcher, "stale"); bad.lease_epoch = 0;
        for (int i = 0; i < 20; ++i) error(ErrorCode::StaleEpoch, [&] { e.plan(q.request_id, bad, "late"); });
        CHECK(e.events().size() == 14);
        q = e.finish_operation(q.request_id, command(q, Role::Worker, "finish", "builder"), q.operations.begin()->first,
                               OperationState::Unknown, "receipt uncertain");
        saved = q;
    }
    TaskEngine e(path, limits, {"test.echo"}); auto q = e.get(saved.request_id);
    CHECK(q.lease_epoch > saved.lease_epoch);
    q = e.reconcile_operation(q.request_id, q.state_version, "reconcile", q.operations.begin()->first,
                              OperationState::Succeeded, "verified fixture");
    q = e.cancel(q.request_id, command(q, Role::Root, "cancel"));
    q = e.advance_resources(q.request_id, q.state_version, "stop", ResourceState::Stopped);
    q = e.advance_resources(q.request_id, q.state_version, "clean", ResourceState::Cleaning);
    q = e.advance_resources(q.request_id, q.state_version, "closed", ResourceState::Closed);
    CHECK(q.state == RequestState::Cancelled); CHECK(q.resources == ResourceState::Closed);
}

void quotas_reject_new_work_without_losing_existing_state() {
    EngineLimits limits; limits.max_requests = 1; limits.max_teams_per_request = 1; limits.max_operations_per_request = 1;
    Kernel f(limits); auto q = f.prepare(f.submit());
    CHECK(f.submit().request_id == q.request_id);
    error(ErrorCode::QuotaExceeded, [&] { f.submit("second"); });
    error(ErrorCode::QuotaExceeded, [&] { f.engine.create_team(q.request_id, f.cmd(q, Role::Dispatcher), "second-team"); });
    q = f.operation(q); error(ErrorCode::QuotaExceeded, [&] { f.operation(q, "second-op"); });
    CHECK(f.engine.get(q.request_id).operations.size() == 1);
    auto replacement = input("replacement");
    error(ErrorCode::QuotaExceeded, [&] { f.engine.replace(q.request_id, f.cmd(q, Role::Root), replacement); });
    CHECK(f.engine.get(q.request_id).state == RequestState::Running);
    auto no_tools = input("no-grants"); no_tools.allowed_tools.clear();
    test::TempDir dir; TaskEngine e(dir.path / "state.journal", {}, {});
    auto blocked = e.submit(root(no_tools), no_tools);
    CHECK(blocked.original_input.allowed_tools.empty());
}

void concurrent_state_commits_use_compare_and_swap() {
    Kernel f; auto q = f.submit(); std::barrier gate(2); std::atomic<int> success{0}; std::atomic<int> conflicts{0};
    std::vector<std::thread> writers;
    for (int i = 0; i < 2; ++i) writers.emplace_back([&, i] {
        auto cmd = command(q, Role::Dispatcher, "racing-" + std::to_string(i)); gate.arrive_and_wait();
        try { f.engine.plan(q.request_id, cmd, "plan-" + std::to_string(i)); ++success; }
        catch (const KernelError& e) { if (e.code == ErrorCode::Conflict) ++conflicts; }
    });
    for (auto& thread : writers) thread.join();
    CHECK(success == 1); CHECK(conflicts == 1); CHECK(f.engine.get(q.request_id).state_version == q.state_version + 1);
    CHECK(f.engine.events().size() == 3);
}

void independent_requests_progress_from_parallel_callers() {
    Kernel f; constexpr int count = 12; std::vector<RequestSnapshot> requests;
    for (int i = 0; i < count; ++i) requests.push_back(f.submit("key-" + std::to_string(i)));
    std::barrier gate(count); std::atomic<int> success{0}; std::vector<std::thread> threads;
    for (int i = 0; i < count; ++i) threads.emplace_back([&, i] {
        auto q = requests[static_cast<std::size_t>(i)]; gate.arrive_and_wait();
        try { f.engine.plan(q.request_id, command(q, Role::Dispatcher, "same-scoped-command"), "plan-" + std::to_string(i)); ++success; }
        catch (...) {}
    });
    for (auto& thread : threads) thread.join();
    CHECK(success == count);
    for (int i = 0; i < count; ++i) {
        auto q = f.engine.get(requests[static_cast<std::size_t>(i)].request_id);
        CHECK(q.state == RequestState::Running); CHECK(q.plan == "plan-" + std::to_string(i));
        CHECK(f.engine.events(q.request_id).size() == 2);
    }
}

void interrupted_replace_replays_neither_half() {
    test::TempDir dir; auto path = dir.path / "state.journal"; std::uintmax_t boundary{};
    auto in = input(); std::string id;
    {
        TaskEngine e(path, {}, {"test.echo"}); auto q = e.submit(root(in), in); id = q.request_id;
        boundary = std::filesystem::file_size(path); auto next = input("replacement");
        e.replace(id, command(q, Role::Root, "replace"), next);
    }
    std::filesystem::resize_file(path, boundary + 30); // A valid frame header, incomplete transaction payload.
    TaskEngine e(path, {}, {"test.echo"}); auto restored = e.get(id);
    CHECK(e.requests().size() == 1); CHECK(restored.state == RequestState::Queued);
    CHECK(restored.replaced_by.empty()); CHECK(e.events().size() == 2);
    CHECK(e.events()[1].kind == "RuntimeRestarted");
}

void unknown_task_schema_fails_without_rewriting_file() {
    test::TempDir dir; auto path = dir.path / "state.journal";
    { DurableJournal journal(path); journal.append("unknown task schema"); }
    auto size = std::filesystem::file_size(path);
    test::throws<KernelError>([&] { TaskEngine e(path, {}, {"test.echo"}); });
    CHECK(std::filesystem::file_size(path) == size);
}

void root_snapshot_is_frozen_and_unicode_roundtrips() {
    test::TempDir dir; auto path = dir.path / "state.journal"; auto original = input();
    original.goal = "中文目标\nwith quotes: \"hello\" and delimiters : | "; std::string id;
    {
        TaskEngine e(path, {}, {"test.echo"}); auto q = e.submit(root(original), original); id = q.request_id;
        auto changed = original; changed.idempotency_key = "new-root-snapshot"; changed.root_defaults.model = "mock-new";
        changed.root_defaults.reasoning = {ReasoningMode::Disabled, ""}; e.submit(root(changed), changed);
        CHECK(e.get(id).original_input.root_defaults.model == "mock-root");
    }
    TaskEngine e(path, {}, {"test.echo"}); auto q = e.get(id);
    CHECK(q.goal == original.goal); CHECK(q.original_input.root_defaults == original.root_defaults);
    CapabilityRegistry registry; registry.add("mock-root", {{"medium"}, true, true});
    registry.add("mock-dispatcher", {{"medium"}, true, true});
    auto dispatcher = resolve_model(q.original_input.root_defaults, {"mock-dispatcher", {}}, registry);
    auto worker = resolve_model(q.original_input.root_defaults, {}, registry);
    CHECK(dispatcher.model == "mock-dispatcher"); CHECK(worker.model == "mock-root");
    CHECK(worker.reasoning.effort == "medium");
}

void oversized_state_is_rejected_before_poisoning_journal() {
    EngineLimits limits; limits.max_transaction_bytes = 128U * 1024U; Kernel f(limits); auto q = f.submit();
    bool rejected = false;
    for (int i = 0; i < 10; ++i) {
        auto before = std::filesystem::file_size(f.directory.path / "state.journal");
        try { q = f.engine.plan(q.request_id, f.cmd(q, Role::Dispatcher), std::string(16384, 'x')); }
        catch (const KernelError& e) {
            CHECK(e.code == ErrorCode::QuotaExceeded);
            CHECK(std::filesystem::file_size(f.directory.path / "state.journal") == before);
            rejected = true; break;
        }
    }
    CHECK(rejected); auto event_count = f.engine.events().size();
    q = f.engine.cancel(q.request_id, f.cmd(q, Role::Root));
    CHECK(q.state == RequestState::Cancelled); CHECK(f.engine.events().size() == event_count + 1);
}
} // namespace

int main() {
    return test::run({
        {"admission/durable-scoped-idempotency", admission_is_durable_and_scoped},
        {"commands/idempotency-CAS-atomic-events", command_idempotency_and_atomic_events},
        {"roles/Worker-only-cross-team-grants", role_and_team_boundaries},
        {"amend/requirements-plan-instructions-application", amend_requires_new_plan_explanation_and_ack},
        {"instructions/fence-prepared-and-running-operations", instruction_change_fences_old_operations},
        {"replace/distinct-from-independent-NEW", replace_and_new_have_distinct_semantics},
        {"cancel/late-result-and-unknown-outcome", cancellation_does_not_hide_unknown_outcomes},
        {"operations/idempotency-without-blind-retry", operation_idempotency_is_not_automatic_retry},
        {"restart/automatic-fencing-and-reapplication", restart_automatically_fences_dispatch_and_requires_reapplication},
        {"review/new-candidate-invalidates-approval", review_and_new_candidate_invalidate_old_acceptance},
        {"artifacts/immutable-version-history", artifact_versions_survive_switches_and_amendments},
        {"review/all-assigned-reviewers-must-pass", all_assigned_reviewers_are_required},
        {"review/adverse-evidence-revokes-even-at-quota", adverse_review_revokes_pending_delivery_even_at_quota},
        {"review/changed-instructions-require-fresh-evidence", review_instruction_changes_require_fresh_review},
        {"delivery/receipt-and-cleanup-separation", delivery_and_cleanup_are_separate_state_dimensions},
        {"quota/control-settlement-cancel-recovery", ordinary_quota_never_blocks_settlement_cancel_or_reopen},
        {"quota/new-work-is-bounded-and-atomic", quotas_reject_new_work_without_losing_existing_state},
        {"concurrency/same-request-CAS", concurrent_state_commits_use_compare_and_swap},
        {"concurrency/independent-requests-progress", independent_requests_progress_from_parallel_callers},
        {"persistence/interrupted-REPLACE-is-atomic", interrupted_replace_replays_neither_half},
        {"persistence/unknown-schema-fails-closed", unknown_task_schema_fails_without_rewriting_file},
        {"config/persisted-Root-snapshot-and-unicode", root_snapshot_is_frozen_and_unicode_roundtrips},
        {"quota/oversized-frame-preserves-live-journal", oversized_state_is_rejected_before_poisoning_journal},
    });
}
