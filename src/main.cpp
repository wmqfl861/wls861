#include "wls/config.hpp"
#include "wls/scheduler.hpp"
#include "wls/task_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace wls;
constexpr std::string_view version = "0.1.1-m1";
constexpr std::string_view fixture_payload = "hello";
// SHA-256 of the five UTF-8 bytes in fixture_payload. This is a fixed synthetic
// artifact, not a model response or a receipt from an external delivery system.
constexpr std::string_view fixture_hash =
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

void require_demo(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryWorkspace {
public:
    TemporaryWorkspace() {
        const std::string pattern =
            (std::filesystem::temp_directory_path() / "wls861-demo-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        const char* created = ::mkdtemp(name.data());
        if (!created) throw std::runtime_error("could not create private demo directory");
        path_ = created;
    }
    ~TemporaryWorkspace() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    TemporaryWorkspace(const TemporaryWorkspace&) = delete;
    TemporaryWorkspace& operator=(const TemporaryWorkspace&) = delete;

    const std::filesystem::path& path() const { return path_; }
    void cleanup() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) throw std::runtime_error("could not remove private demo directory: " + error.message());
        path_.clear();
    }
private:
    std::filesystem::path path_;
};

// The host derives trusted actor bindings and all fencing fields from the
// latest persisted snapshot before each command. Model output supplies none
// of these authority fields. Demo phases serialize changes within a request.
class Commands {
public:
    explicit Commands(TaskEngine& engine) : engine_(engine) {}

    Command current(const std::string& request_id, Role role,
                    const std::string& team_id = "") {
        const auto snapshot = engine_.get(request_id);
        Actor actor{role, snapshot.original_input.root_id, snapshot.request_id, team_id, ""};
        switch (role) {
        case Role::Root:
            require_demo(team_id.empty(), "Root command cannot name a team");
            actor.principal_id = snapshot.original_input.root_id;
            break;
        case Role::Dispatcher:
            require_demo(team_id.empty(), "Dispatcher command cannot name a team");
            actor.principal_id = snapshot.dispatcher_id;
            break;
        case Role::TeamLead:
            actor.principal_id = snapshot.teams.at(team_id).lead_id;
            break;
        case Role::Worker:
            actor.principal_id = snapshot.teams.at(team_id).worker_id;
            break;
        }
        return {std::move(actor), next_id(), snapshot.state_version,
                snapshot.requirements_revision, snapshot.lease_epoch};
    }

    std::string next_id() {
        return "demo-command-" + std::to_string(sequence_.fetch_add(1) + 1);
    }
private:
    TaskEngine& engine_;
    std::atomic<std::uint64_t> sequence_{};
};

class ParallelActivity {
public:
    ParallelActivity(std::atomic<int>& running, std::atomic<int>& peak)
        : running_(running) {
        const int active = running_.fetch_add(1) + 1;
        int previous = peak.load();
        while (previous < active && !peak.compare_exchange_weak(previous, active)) {}
    }
    ~ParallelActivity() { running_.fetch_sub(1); }
private:
    std::atomic<int>& running_;
};

struct RequestEvidence {
    RequestSnapshot snapshot;
    std::vector<EventEnvelope> events;
    ResolvedModel worker;
    ResolvedModel dispatcher;
};

struct DemoSummary {
    int peak_parallel{};
    std::size_t event_count{};
    bool all_completed{true};
    bool all_resources_closed{true};
    bool worker_inherits_frozen_root{true};
    bool dispatcher_override_is_independent{true};
    std::vector<RequestEvidence> requests;
};

void wait_all(std::vector<std::future<void>>& futures) {
    // Get every accepted future even if an earlier callback failed; scheduler
    // destruction then has no hidden successful work still awaiting inspection.
    std::exception_ptr first_failure;
    for (auto& future : futures) {
        try { future.get(); }
        catch (...) { if (!first_failure) first_failure = std::current_exception(); }
    }
    if (first_failure) std::rethrow_exception(first_failure);
    futures.clear();
}

DemoSummary run_synthetic_workflow(const std::filesystem::path& workspace) {
    CapabilityRegistry registry;
    registry.add("mock-root", {{"medium", "high"}, true, true});
    registry.add("mock-dispatcher", {{"medium", "high"}, true, true});
    const ModelSelection frozen_root{"mock-root", {ReasoningMode::Explicit, "medium"}};
    const ModelSelection dispatcher_override{
        "mock-dispatcher", {ReasoningMode::Explicit, "high"}};
    validate_root_defaults(frozen_root, registry);

    TaskEngine engine(workspace / "synthetic.journal", EngineLimits{}, {"test.echo"});
    Commands commands(engine);
    const Actor root{Role::Root, "demo-root", "", "", "demo-root"};
    std::vector<std::string> request_ids;
    for (int index = 1; index <= 2; ++index) {
        RequestInput input{"demo-tenant", "demo-project", "demo-root",
            "demo-request-" + std::to_string(index),
            "Produce and independently review the synthetic hello fixture",
            frozen_root, {"test.echo"}};
        request_ids.push_back(engine.submit(root, input).request_id);
    }
    require_demo(request_ids[0] != request_ids[1], "independent admissions must have different request IDs");
    require_demo(engine.get(request_ids[0]).dispatcher_id != engine.get(request_ids[1]).dispatcher_id,
                 "independent requests must have different Dispatcher identities");

    std::atomic<int> running{};
    std::atomic<int> peak{};
    std::latch both_build_callbacks(2);
    std::vector<std::future<void>> pending;
    pending.reserve(request_ids.size());
    // Callback captures outlive the scheduler even if admission or a future
    // throws. A partially admitted pair must also be released from its latch.
    Scheduler scheduler(3, 16, 1);
    std::size_t admitted_builds = 0;
    try {
    for (const auto& request_id : request_ids) {
        pending.push_back(scheduler.submit(request_id + ":build-job", request_id,
            WorkClass::Normal, [&, request_id] {
                ParallelActivity activity(running, peak);
                both_build_callbacks.arrive_and_wait();
                const auto inherited = engine.get(request_id).original_input.root_defaults;
                const auto worker = resolve_model(inherited, {}, registry);
                const auto dispatcher = resolve_model(inherited, dispatcher_override, registry);
                require_demo(worker == inherited && worker.model != dispatcher.model,
                             "Worker must inherit the frozen Root, independently of Dispatcher override");

                engine.plan(request_id, commands.current(request_id, Role::Dispatcher),
                    "Build hello fixture; separate review team checks it; handoff requires a synthetic Root receipt");
                engine.create_team(request_id, commands.current(request_id, Role::Dispatcher), "build", false);
                engine.create_team(request_id, commands.current(request_id, Role::Dispatcher), "review", true);
                engine.instruct_team(request_id, commands.current(request_id, Role::TeamLead, "build"),
                    "Apply this plan and simulate test.echo with the literal hello fixture");
                engine.acknowledge_applied(request_id, commands.current(request_id, Role::Worker, "build"));
                engine.instruct_team(request_id, commands.current(request_id, Role::TeamLead, "review"),
                    "Check the current candidate hash, version, and succeeded synthetic operation");
                engine.acknowledge_applied(request_id, commands.current(request_id, Role::Worker, "review"));
                const auto prepared = engine.prepare_operation(request_id,
                    commands.current(request_id, Role::Worker, "build"),
                    "echo-once", "test.echo", std::string(fixture_payload));
                require_demo(prepared.operations.size() == 1, "demo expects exactly one synthetic operation");
                const auto operation_id = prepared.operations.begin()->first;
                engine.start_operation(request_id, commands.current(request_id, Role::Worker, "build"), operation_id);
                // Only the kernel's simulation state changes: no shell, MCP,
                // external service, network, or production business tool runs.
                engine.finish_operation(request_id, commands.current(request_id, Role::Worker, "build"),
                    operation_id, OperationState::Succeeded, "synthetic:echo:" + request_id + ":hello");
                const auto snapshot = engine.get(request_id);
                Candidate candidate{request_id + ":hello-artifact", 1, std::string(fixture_hash),
                    "build", snapshot.requirements_revision};
                engine.submit_candidate(request_id, commands.current(request_id, Role::Worker, "build"), candidate);
            }));
        ++admitted_builds;
    }
    } catch (...) {
        both_build_callbacks.count_down(static_cast<std::ptrdiff_t>(request_ids.size() - admitted_builds));
        throw;
    }
    wait_all(pending);
    require_demo(peak.load() >= 2, "two independent build callbacks must overlap");

    // The host advances phases after all prerequisite futures have completed.
    // No callback waits for a future submitted to this same bounded pool.
    for (const auto& request_id : request_ids) {
        pending.push_back(scheduler.submit(request_id + ":review-job", request_id,
            WorkClass::Review, [&, request_id] {
                const auto snapshot = engine.get(request_id);
                require_demo(snapshot.candidate.has_value(), "review requires a candidate");
                require_demo(snapshot.candidate->content_hash == fixture_hash &&
                             snapshot.candidate->artifact_version == 1 &&
                             snapshot.candidate->requirements_revision == snapshot.requirements_revision,
                             "synthetic candidate does not match current fixture");
                require_demo(snapshot.operations.size() == 1 &&
                             snapshot.operations.begin()->second.payload == fixture_payload &&
                             snapshot.operations.begin()->second.state == OperationState::Succeeded,
                             "synthetic operation must have succeeded before review");
                engine.record_review(request_id, commands.current(request_id, Role::Worker, "review"),
                    *snapshot.candidate, true, "synthetic:review:" + request_id);
                engine.accept(request_id, commands.current(request_id, Role::Dispatcher));
            }));
    }
    wait_all(pending);

    for (const auto& request_id : request_ids) {
        pending.push_back(scheduler.submit(request_id + ":handoff-job", request_id,
            WorkClass::Handoff, [&, request_id] {
                const auto package = "synthetic:package:" + request_id;
                engine.prepare_handoff(request_id, commands.current(request_id, Role::Dispatcher), package);
                const auto snapshot = engine.get(request_id);
                require_demo(snapshot.candidate.has_value(), "handoff requires an accepted candidate");
                engine.confirm_delivery(request_id, commands.current(request_id, Role::Root),
                    *snapshot.candidate, package, "synthetic:receipt:" + request_id);
                for (const auto target : {ResourceState::Stopped, ResourceState::Cleaning, ResourceState::Closed}) {
                    const auto current = engine.get(request_id);
                    engine.advance_resources(request_id, current.state_version, commands.next_id(), target);
                }
            }));
    }
    wait_all(pending);
    scheduler.close(true);

    DemoSummary summary;
    summary.peak_parallel = peak.load();
    summary.event_count = engine.events().size();
    for (const auto& snapshot : engine.requests()) {
        auto worker = resolve_model(snapshot.original_input.root_defaults, {}, registry);
        auto dispatcher = resolve_model(snapshot.original_input.root_defaults, dispatcher_override, registry);
        summary.all_completed = summary.all_completed && snapshot.state == RequestState::Completed;
        summary.all_resources_closed = summary.all_resources_closed && snapshot.resources == ResourceState::Closed;
        summary.worker_inherits_frozen_root = summary.worker_inherits_frozen_root &&
            worker == snapshot.original_input.root_defaults;
        summary.dispatcher_override_is_independent = summary.dispatcher_override_is_independent &&
            worker.model != dispatcher.model && worker.reasoning.effort == "medium" && dispatcher.reasoning.effort == "high";
        summary.requests.push_back({snapshot, engine.events(snapshot.request_id), std::move(worker), std::move(dispatcher)});
    }
    require_demo(summary.all_completed && summary.all_resources_closed,
                 "synthetic requests did not finish their business/resource state transitions");
    return summary;
}

void json_string(std::ostream& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (const char byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) output << "\\u00" << hex[character >> 4] << hex[character & 0xf];
            else output << static_cast<char>(character);
        }
    }
    output << '"';
}

std::string_view reasoning_mode_name(ReasoningMode mode) {
    switch (mode) {
    case ReasoningMode::Inherit: return "inherit";
    case ReasoningMode::Disabled: return "disabled";
    case ReasoningMode::ProviderDefault: return "provider_default";
    case ReasoningMode::Explicit: return "explicit";
    }
    throw std::runtime_error("invalid reasoning mode in demo summary");
}

void write_model(std::ostream& output, const ModelSelection& model) {
    output << "{\"model\":";
    json_string(output, model.model);
    output << ",\"reasoning\":{\"mode\":";
    json_string(output, reasoning_mode_name(model.reasoning.mode));
    output << ",\"effort\":";
    json_string(output, model.reasoning.effort);
    output << "}}";
}

void write_candidate(std::ostream& output, const Candidate& candidate) {
    output << "{\"artifact_id\":";
    json_string(output, candidate.artifact_id);
    output << ",\"artifact_version\":" << candidate.artifact_version << ",\"content_hash\":";
    json_string(output, candidate.content_hash);
    output << ",\"producer_team_id\":";
    json_string(output, candidate.producer_team_id);
    output << ",\"requirements_revision\":" << candidate.requirements_revision << '}';
}

void write_request(std::ostream& output, const RequestEvidence& evidence) {
    const auto& snapshot = evidence.snapshot;
    output << "{\"request_id\":";
    json_string(output, snapshot.request_id);
    output << ",\"root_id\":";
    json_string(output, snapshot.original_input.root_id);
    output << ",\"dispatcher_id\":";
    json_string(output, snapshot.dispatcher_id);
    output << ",\"state\":";
    json_string(output, to_string(snapshot.state));
    output << ",\"resources\":";
    json_string(output, to_string(snapshot.resources));
    output << ",\"requirements_revision\":" << snapshot.requirements_revision
           << ",\"plan_revision\":" << snapshot.plan_revision
           << ",\"state_version\":" << snapshot.state_version
           << ",\"lease_epoch\":" << snapshot.lease_epoch
           << ",\"accepted\":" << snapshot.accepted
           << ",\"model_configuration\":{\"frozen_root\":";
    write_model(output, snapshot.original_input.root_defaults);
    output << ",\"worker\":";
    write_model(output, evidence.worker);
    output << ",\"dispatcher\":";
    write_model(output, evidence.dispatcher);
    output << "},\"teams\":[";
    bool first = true;
    for (const auto& [team_id, team] : snapshot.teams) {
        if (!first) output << ',';
        first = false;
        output << "{\"team_id\":";
        json_string(output, team_id);
        output << ",\"task_id\":";
        json_string(output, team.task_id);
        output << ",\"lead_id\":";
        json_string(output, team.lead_id);
        output << ",\"worker_id\":";
        json_string(output, team.worker_id);
        output << ",\"review_team\":" << team.review_team
               << ",\"applied_revision\":" << team.applied_revision
               << ",\"applied_plan_revision\":" << team.applied_plan_revision
               << ",\"instructions_version\":" << team.instructions_version
               << ",\"applied_instructions_version\":" << team.applied_instructions_version << '}';
    }
    output << "],\"operations\":[";
    first = true;
    for (const auto& [operation_id, operation] : snapshot.operations) {
        if (!first) output << ',';
        first = false;
        output << "{\"operation_id\":";
        json_string(output, operation_id);
        output << ",\"team_id\":";
        json_string(output, operation.team_id);
        output << ",\"tool_id\":";
        json_string(output, operation.tool_id);
        output << ",\"payload\":";
        json_string(output, operation.payload);
        output << ",\"state\":";
        json_string(output, to_string(operation.state));
        output << ",\"evidence_ref\":";
        json_string(output, operation.evidence_ref);
        output << '}';
    }
    output << "],\"candidate\":";
    if (snapshot.candidate) write_candidate(output, *snapshot.candidate);
    else output << "null";
    output << ",\"reviews\":[";
    first = true;
    for (const auto& review : snapshot.reviews) {
        if (!first) output << ',';
        first = false;
        output << "{\"reviewer_team_id\":";
        json_string(output, review.reviewer_team_id);
        output << ",\"passed\":" << review.passed << ",\"evidence_ref\":";
        json_string(output, review.evidence_ref);
        output << ",\"candidate\":";
        write_candidate(output, review.candidate);
        output << '}';
    }
    output << "],\"package_ref\":";
    json_string(output, snapshot.package_ref);
    output << ",\"receipt_ref\":";
    json_string(output, snapshot.receipt_ref);
    output << ",\"event_kinds\":[";
    first = true;
    for (const auto& event : evidence.events) {
        if (!first) output << ',';
        first = false;
        json_string(output, event.kind);
    }
    output << "]}";
}

void write_summary(const DemoSummary& summary) {
    std::cout << std::boolalpha
              << "{\"project\":\"wls861\",\"version\":";
    json_string(std::cout, version);
    std::cout << ",\"execution_mode\":\"synthetic/no_model\",\"model_calls\":0"
              << ",\"requests\":" << summary.requests.size()
              << ",\"peak_parallel\":" << summary.peak_parallel
              << ",\"all_completed\":" << summary.all_completed
              << ",\"all_resources_closed\":" << summary.all_resources_closed
              << ",\"event_count\":" << summary.event_count
              << ",\"worker_inherits_frozen_root\":" << summary.worker_inherits_frozen_root
              << ",\"dispatcher_override_is_independent\":" << summary.dispatcher_override_is_independent
              << ",\"temporary_workspace_removed\":true,\"request_states\":[";
    bool first = true;
    for (const auto& evidence : summary.requests) {
        if (!first) std::cout << ',';
        first = false;
        write_request(std::cout, evidence);
    }
    std::cout << "]}\n";
}

void print_help() {
    std::cout <<
        "wls861: M1 synthetic multi-agent kernel\n\n"
        "Usage:\n"
        "  wls861 demo       Run two independent synthetic requests and print JSON\n"
        "  wls861 --version  Print the version\n"
        "  wls861 --help     Print this help\n\n"
        "The demo exercises Root -> Dispatcher -> TeamLead -> Worker, separate\n"
        "review, synthetic delivery receipts and resource state transitions.\n"
        "It makes no model or network calls and executes no production tools.\n"
        "It creates its own private temporary directory and removes it on exit.\n"
        "Persistent journals and additional demo options are not supported.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        print_help();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "wls861 " << version << '\n';
        return 0;
    }
    if (argc != 2 || std::string_view(argv[1]) != "demo") {
        std::cerr << "Unsupported command or arguments. Use wls861 --help.\n";
        return 2;
    }
    try {
        TemporaryWorkspace workspace;
        const auto summary = run_synthetic_workflow(workspace.path());
        // Scheduler and journal have already closed. Only the directory created
        // by this invocation is removed, before success is reported.
        workspace.cleanup();
        write_summary(summary);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Synthetic demo failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Synthetic demo failed with a non-standard exception.\n";
        return 1;
    }
}
