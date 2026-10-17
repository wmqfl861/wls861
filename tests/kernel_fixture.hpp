#pragma once

#include "test_support.hpp"
#include "wls/task_engine.hpp"

namespace fixture {
using namespace wls;
inline RequestInput input(std::string key = "first") {
    return {"tenant", "project", "root", std::move(key), "Build a synthetic artifact",
            {"mock-root", {ReasoningMode::Explicit, "medium"}}, {"test.echo"}};
}
inline Actor root(const RequestInput& in) { return {Role::Root, in.root_id, "", "", in.root_id}; }
inline Actor actor(const RequestSnapshot& q, Role role, const std::string& team = "") {
    std::string principal = q.original_input.root_id;
    if (role == Role::Dispatcher) principal = q.dispatcher_id;
    if (role == Role::TeamLead) principal = q.teams.at(team).lead_id;
    if (role == Role::Worker) principal = q.teams.at(team).worker_id;
    return {role, q.original_input.root_id, q.request_id, team, principal};
}
inline Command command(const RequestSnapshot& q, Role role, const std::string& key,
                       const std::string& team = "") {
    return {actor(q, role, team), key, q.state_version, q.requirements_revision, q.lease_epoch};
}
template<class Function> void error(ErrorCode expected, Function&& function) {
    bool caught = false;
    try { function(); } catch (const KernelError& e) { CHECK(e.code == expected); caught = true; }
    CHECK(caught);
}
struct Kernel {
    test::TempDir directory;
    TaskEngine engine;
    std::uint64_t counter{};
    explicit Kernel(EngineLimits limits = {}) : engine(directory.path / "state.journal", limits, {"test.echo"}) {}
    RequestSnapshot submit(const std::string& key = "first") { auto in = input(key); return engine.submit(root(in), in); }
    Command cmd(const RequestSnapshot& q, Role role, const std::string& team = "") {
        return command(q, role, "cmd-" + std::to_string(++counter), team);
    }
    RequestSnapshot prepare(RequestSnapshot q, bool review = false) {
        q = engine.plan(q.request_id, cmd(q, Role::Dispatcher), "Build and validate fixture");
        q = engine.create_team(q.request_id, cmd(q, Role::Dispatcher), "builder");
        q = apply(q, "builder");
        if (review) {
            q = engine.create_team(q.request_id, cmd(q, Role::Dispatcher), "reviewer", true);
            q = apply(q, "reviewer");
        }
        return q;
    }
    RequestSnapshot apply(RequestSnapshot q, const std::string& team = "builder") {
        q = engine.instruct_team(q.request_id, cmd(q, Role::TeamLead, team), "Stop obsolete work; apply this plan");
        return engine.acknowledge_applied(q.request_id, cmd(q, Role::Worker, team));
    }
    RequestSnapshot operation(RequestSnapshot q, const std::string& key = "effect-1") {
        return engine.prepare_operation(q.request_id, cmd(q, Role::Worker, "builder"), key, "test.echo", "fixture payload");
    }
    Candidate candidate(const RequestSnapshot& q, Revision version = 1) {
        return {"artifact", version, std::string(64, 'a'), "builder", q.requirements_revision};
    }
    RequestSnapshot approved(RequestSnapshot q) {
        q = engine.submit_candidate(q.request_id, cmd(q, Role::Worker, "builder"), candidate(q));
        q = engine.record_review(q.request_id, cmd(q, Role::Worker, "reviewer"), *q.candidate, true, "fixture-review");
        return engine.accept(q.request_id, cmd(q, Role::Dispatcher));
    }
};
} // namespace fixture
