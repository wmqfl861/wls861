#include "legacy_shell_config.hpp"
#include "wls/single_agent.hpp"
#include "wls/worker_tools.hpp"
#include "live_test_support.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
using namespace wls;
using namespace wls::s1;
using Object = boost::json::object;
using Array = boost::json::array;
namespace support = wls::test::live;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::unique_ptr<support::Assets> assets;
std::unique_ptr<SchemaRegistry> schemas;

std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t number(const Json& value, std::string_view key) {
    const auto& item = value.at(key);
    return item.is_int64() ? item.as_int64() : static_cast<std::int64_t>(item.as_uint64());
}
Json fixture(std::string_view name) { return parse_json_strict(read_runtime_file(source / "tests/fixtures" / name)); }
Json policy(std::int64_t calls = 128, std::int64_t bytes = 524288) {
    return Object{{"mode", single_agent_contract}, {"max_attempts", calls}, {"max_history_bytes", bytes}};
}
AdmissionInput admission(const Json& logical = policy()) {
    ::test::TempDir directory;
    std::filesystem::copy(source / "config/examples", directory.path / "config", std::filesystem::copy_options::recursive);
    auto catalog = ::test::legacy_shell_catalog(directory.path / "config/system.yaml", directory.path / "config/functions", source / "prompts");
    auto system = catalog.freeze("general").json().at("system");
    system.at("context").as_object().erase("tool_policy"); system.at("context").as_object().erase("model_projection");
    system.at("context").as_object().erase("worker_history");
    system.at("context").as_object()["request_format"] = "structured-v2";
    if (!logical.is_null()) system.at("context").as_object()["logical_agent"] = logical;
    else system.at("context").as_object().erase("logical_agent");
    { std::ofstream output(directory.path / "config/system.yaml"); output << canonical_json(system); CHECK(output.good()); }
    catalog = ::test::legacy_shell_catalog(directory.path / "config/system.yaml", directory.path / "config/functions", source / "prompts");
    const auto frozen = catalog.freeze("general");
    AdmissionInput input;
    input.tenant_id = "single-test"; input.project_id = "single-project"; input.root_id = "single-root";
    input.function_id = frozen.function_id(); input.idempotency_key = new_id("key");
    input.original_text = read_runtime_file(source / "tests/fixtures/s13_request.txt");
    input.constraint_catalog = fixture("s13_constraints.json"); input.config_snapshot = frozen.json();
    const auto grants = frozen.worker_tools(); input.tool_allowlist.assign(grants.begin(), grants.end());
    input.budget = BudgetLimits{128, 10000000, 1000000, 0};
    return input;
}
void expect(ErrorCode code, const std::function<void()>& action) { support::error(code, action); }

FrozenRequestConfig history_config(const Json& logical = policy()) {
    const auto bytes = canonical_json(admission(logical).config_snapshot);
    return FrozenRequestConfig::restore(bytes, sha256_hex(bytes));
}
Json complete_prefix_oracle(const std::string& request_id, const FrozenRequestConfig& config,
                           const std::vector<Json>& history) {
    const auto seed = sha256_hex(canonical_json(Object{{"format", "wls-single-agent-chain-v1"},
        {"request_id", request_id}, {"config_snapshot_sha256", config.sha256()},
        {"policy", config.logical_agent_policy()}}));
    auto chain = seed;
    Array prefix;
    for (const auto& record : history) {
        chain = sha256_hex(canonical_json(Object{{"format", "wls-single-agent-link-v1"},
            {"previous_sha256", chain}, {"fact", record}}));
        prefix.push_back(record);
    }
    // Independent oracle: encode the complete aggregate, without concatenating
    // or reusing any record bytes from the production transcript builder.
    return Object{{"contract", single_agent_contract}, {"logical_agent_id", "logical_" + seed},
        {"global_step", static_cast<std::int64_t>(history.size()) + 1},
        {"history_count", history.size()}, {"history_sha256", sha256_hex(canonical_json(prefix))},
        {"previous_chain_sha256", chain}};
}
Json observation_record(const std::string& request_id, const FrozenRequestConfig& config,
                        const std::vector<Json>& history, const Json& content) {
    const auto suffix = std::to_string(history.size() + 1);
    const std::string attempt_id = "attempt_observation_" + suffix;
    const Object binding{{"request_id", request_id}};
    const Object response{{"original_observation", content}};
    const Object result{{"runtime_error", true}, {"preserved_original", true}};
    const auto fingerprint = sha256_hex(canonical_json(Object{{"attempt_id", attempt_id},
        {"original_binding", binding}, {"response", response}, {"result", result}}));
    return Object{{"request_id", request_id}, {"config_snapshot_sha256", config.sha256()},
        {"model_ref", config.resolve(Role::Dispatcher).model}, {"attempt_id", attempt_id},
        {"call_id", "call_observation_" + suffix}, {"frozen_binding", binding}, {"accepted", 0},
        {"response", response}, {"settlement_sha256", sha256_hex("synthetic original settlement")},
        {"feedback", Object{{"original_binding", binding}, {"response", response},
            {"result", result}, {"record_sha256", fingerprint}}},
        {"logical_agent_execution", complete_prefix_oracle(request_id, config, history)}};
}

void transcript_prefix_matches_complete_canonical_oracle() {
    const auto config = history_config();
    const std::string request_id = "req_prefix_oracle";
    std::vector<Json> history;
    CHECK(single_agent_transcript(request_id, config, history).execution ==
          complete_prefix_oracle(request_id, config, history));
    for (const auto value : {0.1, -0.0, 1.0, 1e20}) {
        history.push_back(observation_record(request_id, config, history,
            Object{{"number", value}, {"text", "Unicode 雪, quote \" and slash \\ with newline\n"},
                {"nested", Object{{"history_sha256", "original field remains present"}}}}));
        const auto actual = single_agent_transcript(request_id, config, history);
        CHECK(actual.execution == complete_prefix_oracle(request_id, config, history));
        CHECK(actual.messages.size() == history.size());
        for (std::size_t index = 0; index < history.size(); ++index) {
            CHECK(actual.messages[index].role == "user");
            CHECK(actual.messages[index].content == canonical_json(history[index]));
            CHECK(actual.messages[index].tool_calls.empty());
        }
    }
}

void transcript_keeps_aggregate_depth_and_json_guards() {
    const auto config = history_config();
    const std::string request_id = "req_prefix_depth";
    auto record = observation_record(request_id, config, {}, Object{});
    Json nested = 0;
    for (std::size_t depth = 0; depth < 126; ++depth) nested = Array{std::move(nested)};
    record.as_object()["depth_probe"] = nested;
    CHECK(!canonical_json(Array{record}).empty());
    CHECK(single_agent_transcript(request_id, config, {record}).execution ==
          complete_prefix_oracle(request_id, config, {record}));
    record.as_object()["depth_probe"] = Array{std::move(nested)};
    CHECK(!canonical_json(record).empty());
    expect(ErrorCode::InvalidInput, [&] { (void)canonical_json(Array{record}); });
    expect(ErrorCode::InvalidInput, [&] { (void)single_agent_transcript(request_id, config, {record}); });
    for (const auto& invalid : std::vector<Json>{Json(std::string(1, static_cast<char>(0xff))),
            Json(std::numeric_limits<double>::infinity()), Json(std::numeric_limits<double>::quiet_NaN())}) {
        auto malformed = observation_record(request_id, config, {}, Object{});
        malformed.as_object()["invalid_value"] = invalid;
        expect(ErrorCode::InvalidInput, [&] { (void)single_agent_transcript(request_id, config, {malformed}); });
    }
}

void transcript_byte_limit_counts_the_complete_array() {
    const auto config = history_config(policy(128, 4096));
    const std::string request_id = "req_prefix_bytes";
    auto record = observation_record(request_id, config, {}, Object{});
    record.as_object()["padding"] = "";
    const auto original_size = canonical_json(Array{record}).size();
    CHECK(original_size < 4096);
    record.as_object()["padding"] = std::string(4096 - original_size, 'x');
    CHECK(canonical_json(Array{record}).size() == 4096);
    CHECK(single_agent_transcript(request_id, config, {record}).execution ==
          complete_prefix_oracle(request_id, config, {record}));
    record.at("padding").as_string().push_back('x');
    expect(ErrorCode::Quota, [&] { (void)single_agent_transcript(request_id, config, {record}); });
}

AttemptReservation reservation(const ModelRequest& request) {
    AttemptReservation result;
    result.call_id = request.call_id; result.attempt_id = request.attempt_id;
    result.config_snapshot_sha256 = request.config_snapshot_id; result.model_ref = request.model.model;
    result.reserved_input_tokens = request.reserved_input_tokens; result.reserved_output_tokens = request.reserved_output_tokens;
    result.reserved_cost_micro_usd = request.reserved_cost_micro_usd; result.max_output_tokens = request.max_output_tokens;
    result.request_envelope = to_json(request); return result;
}
void change_input(ModelRequest& request, const std::function<void(Json&)>& edit) {
    auto encoded = to_json(request.context);
    edit(encoded.at("input"));
    encoded.as_object()["logical_input_sha256"] = sha256_hex(canonical_json(encoded.at("input")));
    request.context = compiled_context_from_json(encoded);
    request.reserved_input_tokens = static_cast<std::int64_t>(request.context.prompt().size());
}

struct Fixture {
    ::test::TempDir directory;
    std::filesystem::path database{directory.path / "control.sqlite"};
    AdmissionInput input;
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> alpha, beta;
    std::string id;
    explicit Fixture(Json logical = policy(), std::int64_t attempts = 128) : input(admission(logical)), store(assets->open(database)) {
        input.budget.max_attempts = attempts;
        id = text(store->admit(input), "request_id");
        std::filesystem::create_directory(directory.path / "alpha");
        std::filesystem::create_directory(directory.path / "beta");
        tools();
    }
    void tools() {
        alpha = std::make_unique<WorkerTools>(*store, directory.path / "alpha");
        beta = std::make_unique<WorkerTools>(*store, directory.path / "beta");
    }
    SingleAgentRuntime runtime() { return SingleAgentRuntime(*store, *schemas, {{"alpha", alpha.get()}, {"beta", beta.get()}}); }
    Json run() {
        EventPumpOptions options; options.max_calls = 32; options.max_worker_calls_per_task = 2;
        return runtime().pump(id, event_fake_script_from_json(fixture("s13_fake_initial.json")), options);
    }
    Json task(std::string_view node) const {
        for (const auto& value : store->tasks(id)) if (value.at("node_id").as_string() == node) return value;
        throw std::runtime_error("synthetic task missing");
    }
    std::vector<Json> history() const { return store->logical_agent_history(store->host_actor(Role::Dispatcher, id)); }
    CapturedModelCall worker() {
        OfflineRuntime original(*store, *schemas, beta.get());
        return original.capture_call(id, Role::Worker, text(task("beta"), "task_id"));
    }
};

Json plain_policy() { auto value = policy(); value.at("mode") = plain_single_agent_contract; return value; }
EventFakeScript plain_script() {
    EventFakeScript result;
    const auto source_script = event_fake_script_from_json(fixture("s14_fake_submit.json"));
    for (const auto& step : source_script.by_node.at("alpha"))
        if (step.role == Role::Worker) result.by_node["plain"].push_back(step);
    return result;
}
struct PlainFixture : Fixture {
    explicit PlainFixture(std::int64_t attempts = 128) : Fixture(plain_policy(), attempts) {}
    SingleAgentRuntime runtime() { return SingleAgentRuntime(*store, *schemas, {{"plain", alpha.get()}}); }
    Json pump(const EventFakeScript& script = {}) {
        EventPumpOptions options; options.max_worker_calls_per_task = 2;
        return runtime().pump(id, script, options);
    }
    CapturedModelCall worker() {
        return OfflineRuntime(*store, *schemas, alpha.get()).capture_call(id, Role::Worker, text(task("plain"), "task_id"));
    }
    void reopen() {
        alpha.reset(); beta.reset(); store.reset();
        store = assets->open(database, StoreRecoveryScope::request(id)); tools();
    }
};

void plain_submission_preserves_originals_and_requires_acceptance() {
    PlainFixture f; CHECK(f.pump().at("calls").as_array().empty());
    const auto captured = f.worker();
    for (const auto& grant : f.input.tool_allowlist)
        CHECK(std::find(captured.request.context.tool_names.begin(), captured.request.context.tool_names.end(), grant) != captured.request.context.tool_names.end());
    const auto report = f.pump(plain_script());
    CHECK(report.at("execution_contract") == plain_single_agent_contract);
    CHECK(report.at("model_loop_complete") == true && report.at("business_acceptance_pending") == true);
    CHECK(report.at("business_delivered") == false && f.store->experience_jobs(f.id).empty());
    const auto task_id = text(f.task("plain"), "task_id");
    const auto submission = f.store->current_submission(task_id);
    CHECK(f.task("plain").at("status") == "SUBMITTED" && submission.at("checks").as_array().empty());
    CHECK(submission.at("artifact_snapshot").as_array().front().at("sha256") == sha256_hex("alpha candidate version 1\n"));
    const auto history = f.history(); CHECK(history.size() == 2);
    for (const auto& turn : history) {
        CHECK(turn.at("role") == "worker" && turn.at("task_id") == task_id);
        CHECK(turn.at("principal_id") == history.front().at("principal_id"));
    }
    const auto config = restore_request_config(f.store->request(f.id));
    for (const auto* field : {"role", "principal_id"}) {
        auto corrupt = history; corrupt.back().at(field) = "other";
        expect(ErrorCode::Corrupt, [&] { (void)single_agent_transcript(f.id, config, corrupt); });
    }
    { std::ofstream output(f.directory.path / "alpha/result.txt"); output << "later workspace bytes\n"; CHECK(output.good()); }
    f.reopen(); CHECK(f.pump(plain_script()).at("calls").as_array().empty());
    CHECK(f.store->current_submission(task_id) == submission && f.history() == history);
    const auto instruction = f.store->instruction(task_id);
    (void)f.store->amend(f.store->host_actor(Role::Root, f.id), number(f.store->request(f.id), "state_version"),
        "Changed original request", f.input.constraint_catalog, new_id("amend"));
    const auto amended = f.pump(plain_script());
    CHECK(amended.at("model_loop_complete") == false && amended.at("business_acceptance_pending") == false);
    CHECK(f.history() == history && f.store->instruction(task_id) == instruction);
}

void plain_interrupted_setup_recovers_without_duplicate_allocation() {
    for (const std::string table : {"tasks", "instructions"}) {
        PlainFixture f;
        support::sql(f.database, "CREATE TRIGGER fail_setup BEFORE INSERT ON " + table + " BEGIN SELECT RAISE(ABORT,'setup interrupted'); END;");
        expect(ErrorCode::Conflict, [&] { (void)f.pump(); });
        CHECK(number(f.store->budget(f.id), "attempts") == 0);
        CHECK(f.store->tasks(f.id).size() == (table == "tasks" ? 0U : 1U));
        CHECK(support::sql_text(f.database, "SELECT count(*) FROM plans") == (table == "tasks" ? "0" : "1"));
        CHECK(support::sql_text(f.database, "SELECT count(*) FROM teams") == (table == "tasks" ? "0" : "1"));
        CHECK(support::sql_text(f.database, "SELECT count(*) FROM instructions") == "0");
        const auto allocated = f.store->tasks(f.id);
        f.reopen(); support::sql(f.database, "DROP TRIGGER fail_setup"); (void)f.pump();
        const auto task = f.task("plain");
        if (!allocated.empty()) CHECK(task.at("task_id") == allocated.front().at("task_id"));
        const auto plan = f.store->plan(f.id), instruction = f.store->instruction(text(task, "task_id"));
        f.reopen(); (void)f.pump();
        CHECK(f.store->plan(f.id) == plan && f.store->instruction(text(task, "task_id")) == instruction);
        CHECK(f.store->tasks(f.id).size() == 1 && number(f.store->budget(f.id), "attempts") == 0);
        CHECK(support::sql_text(f.database, "SELECT count(*) FROM instructions") == "1");
    }
}

void plain_rejects_manager_and_multiple_worker_bypasses() {
    PlainFixture f; (void)f.pump(); const auto captured = f.worker();
    for (const auto role : {Role::Dispatcher, Role::TeamLead}) {
        expect(ErrorCode::Unauthorized, [&] { (void)OfflineRuntime(*f.store, *schemas, f.alpha.get()).capture_call(
            f.id, role, role == Role::Dispatcher ? "" : captured.actor.task_id); });
        auto forged = captured.request; auto context = wls::test::protocol::context();
        context.role = role; context.model = forged.model; context.adapter_version = forged.context.adapter_version;
        context.protocol_version = forged.context.protocol_version;
        forged.role = role; forged.context = compile_structured_context(context);
        if (role == Role::Dispatcher) forged.binding = f.store->request_binding(f.id);
        const auto actor = f.store->host_actor(role, f.id, forged.binding.team_id);
        expect(ErrorCode::Unauthorized, [&] { (void)f.store->reserve_attempt(actor, forged.binding, reservation(forged)); });
    }
    const auto missing = SingleAgentRuntime(*f.store, *schemas).pump(f.id, plain_script());
    CHECK(missing.at("calls").as_array().empty());
    CHECK(std::any_of(missing.at("blocked").as_array().begin(), missing.at("blocked").as_array().end(),
        [](const Json& item) { return item.at("code") == "workspace_missing"; }));
    expect(ErrorCode::NotReady, [&] { (void)OfflineRuntime(*f.store, *schemas).capture_call(f.id, Role::Worker, captured.actor.task_id); });
    const auto request = f.store->request(f.id);
    (void)f.store->create_task(f.store->host_actor(Role::Dispatcher, f.id), number(request, "state_version"),
        number(request, "requirements_revision"), number(request, "plan_revision"), "second", f.task("plain").at("brief"), new_id("second"));
    expect(ErrorCode::Unauthorized, [&] { (void)f.store->reserve_attempt(captured.actor, captured.request.binding, reservation(captured.request)); });
    CHECK(number(f.store->budget(f.id), "attempts") == 0);
}

void plain_keeps_wait_unknown_feedback_and_budget_guards() {
    for (const std::string guard : {"wait", "unknown", "feedback", "budget"}) {
        PlainFixture f(guard == "budget" ? 1 : 128); (void)f.pump();
        const auto captured = f.worker(); Json original = nullptr;
        if (guard == "wait")
            original = f.store->wait_worker(captured.actor, captured.request.binding,
                number(f.task("plain"), "state_version"), "explicit input required", new_id("wait"));
        else if (guard == "unknown") {
            auto op = f.store->prepare_operation(captured.actor, captured.request.binding, number(f.task("plain"), "state_version"),
                new_id("effect"), "note", Object{{"text", "original effect"}}, "none", new_id("prepare"));
            (void)f.store->start_operation(captured.actor, text(op, "op_id"), number(op, "state_version"), new_id("start"));
            f.reopen(); original = f.store->operation(text(op, "op_id")); CHECK(original.at("status") == "UNKNOWN");
        } else if (guard == "feedback") {
            FakeModel model(plain_script().by_node.at("plain"));
            (void)f.store->reserve_attempt(captured.actor, captured.request.binding, reservation(captured.request));
            const auto response = model.complete(captured.request);
            original = f.store->settle_attempt(captured.request.attempt_id, response.binding, response.config_snapshot_id,
                "COMPLETED", to_json(response.usage), to_json(response));
        } else (void)f.pump(plain_script());
        const auto report = f.pump(plain_script());
        CHECK(report.at("model_loop_complete") == false);
        CHECK(number(f.store->budget(f.id), "attempts") == ((guard == "feedback" || guard == "budget") ? 1 : 0));
        if (guard == "wait") CHECK(f.store->worker_wait(captured.actor.task_id) == original);
        if (guard == "unknown") CHECK(f.store->operation(text(original, "op_id")) == original);
        if (guard == "feedback") {
            CHECK(f.store->attempt(captured.request.attempt_id) == original);
            CHECK(f.store->tool_feedback(captured.request.attempt_id).is_null());
        }
    }
}

void shared_native_history_crosses_roles_and_tasks() {
    Fixture f; const auto report = f.run();
    CHECK(report.at("mode") == "single_agent_fake");
    CHECK(report.at("logical_agent_count") == 1 && report.at("calls").as_array().size() == 7);
    CHECK(read_runtime_file(f.directory.path / "alpha/r1.txt") == "alpha original\n");
    CHECK(read_runtime_file(f.directory.path / "beta/r1.txt") == "beta original\n");
    const auto history = f.history(); CHECK(history.size() == 7);
    const auto captured = f.worker();
    const auto contract = parse_json_strict(compiled_task_contract(captured.request.context));
    CHECK(number(contract.at("logical_agent_execution"), "global_step") == 8);
    CHECK(number(contract.at("logical_agent_execution"), "history_count") == 7);
    const auto& messages = captured.request.context.logical_input.at("messages").as_array();
    CHECK(messages.size() == 14);
    bool dispatcher = false, other_worker = false, lead = false;
    for (const auto& message : messages) {
        const auto& origin = message.at("source");
        dispatcher = dispatcher || origin.at("role") == "dispatcher";
        lead = lead || origin.at("role") == "team_lead";
        other_worker = other_worker || (origin.at("role") == "worker" && origin.at("task_id") == f.task("alpha").at("task_id"));
    }
    CHECK(dispatcher && lead && other_worker);
    CHECK(std::find(captured.request.context.tool_names.begin(), captured.request.context.tool_names.end(), "submit_plan") ==
          captured.request.context.tool_names.end());
    for (const auto& item : history) CHECK(!item.as_object().contains("request_envelope"));
}

void omitted_original_pair_is_refused_before_reservation() {
    Fixture f; (void)f.run(); auto captured = f.worker();
    const auto before = f.store->budget(f.id);
    change_input(captured.request, [](Json& logical) {
        auto& messages = logical.at("messages").as_array(); messages.erase(messages.begin(), messages.begin() + 2);
    });
    expect(ErrorCode::Conflict, [&] { (void)f.store->reserve_attempt(captured.actor, captured.request.binding, reservation(captured.request)); });
    CHECK(f.store->budget(f.id) == before);
}

void reordered_original_pairs_are_refused() {
    Fixture f; (void)f.run(); auto captured = f.worker();
    change_input(captured.request, [](Json& logical) {
        auto& messages = logical.at("messages").as_array();
        std::swap(messages[0], messages[2]); std::swap(messages[1], messages[3]);
    });
    const auto before = f.store->budget(f.id);
    expect(ErrorCode::Conflict, [&] { (void)f.store->reserve_attempt(captured.actor, captured.request.binding, reservation(captured.request)); });
    CHECK(f.store->budget(f.id) == before);
}

void changed_or_missing_logical_identity_is_refused() {
    for (const bool remove : {false, true}) {
        Fixture f; (void)f.run(); auto captured = f.worker();
        change_input(captured.request, [remove](Json& logical) {
            auto contract = parse_json_strict(std::string(logical.at("task_contract").as_string()));
            if (remove) contract.as_object().erase("logical_agent_execution");
            else contract.at("logical_agent_execution").as_object()["logical_agent_id"] = "invented-same-label";
            logical.as_object()["task_contract"] = canonical_json(contract);
        });
        const auto before = f.store->budget(f.id);
        expect(ErrorCode::Conflict, [&] { (void)f.store->reserve_attempt(captured.actor, captured.request.binding, reservation(captured.request)); });
        CHECK(f.store->budget(f.id) == before);
    }
}

void concurrent_capture_cannot_fork_the_history() {
    Fixture f; (void)f.run(); auto first = f.worker(), second = f.worker();
    const auto initial_count = number(f.store->budget(f.id), "attempts");
    (void)f.store->reserve_attempt(first.actor, first.request.binding, reservation(first.request));
    expect(ErrorCode::NotReady, [&] { (void)f.store->reserve_attempt(second.actor, second.request.binding, reservation(second.request)); });
    auto script = event_fake_script_from_json(fixture("s13_fake_initial.json")).by_node.at("beta").back();
    script.role_call_index = 1;
    FakeModel model({script});
    OfflineRuntime original(*f.store, *schemas, f.beta.get());
    (void)original.accept_independent_response(first, model.complete(first.request));
    expect(ErrorCode::Conflict, [&] { (void)f.store->reserve_attempt(second.actor, second.request.binding, reservation(second.request)); });
    const auto repeated = f.store->reserve_attempt(first.actor, first.request.binding, reservation(first.request));
    CHECK(text(repeated, "attempt_id") == first.request.attempt_id);
    CHECK(number(f.store->budget(f.id), "attempts") == initial_count + 1);
}

void settled_without_feedback_cannot_advance() {
    Fixture f;
    const auto script = event_fake_script_from_json(fixture("s13_fake_initial.json"));
    FakeModel model(script.default_steps);
    OfflineRuntime original(*f.store, *schemas);
    const auto captured = original.reserve_call(model, f.id, Role::Dispatcher);
    const auto response = model.complete(captured.request);
    const auto settled = f.store->settle_attempt(captured.request.attempt_id, response.binding, response.config_snapshot_id,
        "COMPLETED", to_json(response.usage), to_json(response));
    CHECK(f.store->tool_feedback(captured.request.attempt_id).is_null());
    expect(ErrorCode::NotReady, [&] { (void)original.capture_call(f.id, Role::Dispatcher); });
    CHECK(f.store->attempt(captured.request.attempt_id) == settled);
    CHECK(number(f.store->budget(f.id), "attempts") == 1);
}

void historical_worker_tools_do_not_authorize_a_manager() {
    Fixture f; (void)f.run();
    FakeModelStep step; step.role = Role::Dispatcher; step.role_call_index = 1;
    step.tool_calls = {{"forbidden-write", "fs_write", Object{{"path", "pwn.txt"}, {"content", "not authorized"}}}};
    step.usage = {1, 1, 0, 0, 0};
    FakeModel model({step}); OfflineRuntime original(*f.store, *schemas);
    const auto captured = original.reserve_call(model, f.id, Role::Dispatcher);
    CHECK(original.accept_independent_response(captured, model.complete(captured.request)).at("status") == "FAILED");
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/pwn.txt"));
    CHECK(!std::filesystem::exists(f.directory.path / "beta/pwn.txt"));
    const auto history = f.history(); CHECK(history.size() == 8);
    CHECK(history.back().at("feedback").at("result").at("correction_feedback").at("reason") == "tool_selection");
    const auto next = f.worker();
    const auto& last = next.request.context.logical_input.at("messages").as_array().back();
    CHECK(last.at("role") == "tool" && last.at("tool_calls").as_array().empty());
    CHECK(text(last.at("source"), "attempt_id") == captured.request.attempt_id);
    CHECK(parse_json_strict(std::string(last.at("content").as_string())).at("status") == "FAILED");
}

void normal_restart_keeps_one_chain_and_old_bindings() {
    Fixture f; (void)f.run(); const auto before = f.history();
    const auto old_id = before.front().at("logical_agent_execution").at("logical_agent_id");
    f.alpha.reset(); f.beta.reset(); f.store.reset();
    f.store = assets->open(f.database, StoreRecoveryScope::request(f.id)); f.tools();
    EventFakeScript script;
    const auto original = event_fake_script_from_json(fixture("s13_fake_initial.json"));
    for (const auto* node : {"alpha", "beta"}) {
        auto read = original.by_node.at(node).back(); read.role_call_index = 1;
        script.by_node[node] = {read};
    }
    EventPumpOptions options; options.max_calls = 16; options.max_worker_calls_per_task = 1;
    const auto report = f.runtime().pump(f.id, script, options);
    CHECK(report.at("calls").as_array().size() == 2);
    const auto after = f.history(); CHECK(after.size() == 9);
    for (std::size_t i = 0; i < before.size(); ++i) CHECK(before[i] == after[i]);
    CHECK(after.back().at("logical_agent_execution").at("logical_agent_id") == old_id);
    CHECK(after.back().at("frozen_binding").at("execution_epoch") != before.back().at("frozen_binding").at("execution_epoch"));
}

void sibling_request_history_is_never_adopted() {
    Fixture f; (void)f.run(); const auto before = f.history();
    auto other_input = f.input; other_input.idempotency_key = new_id("other");
    const auto other = text(f.store->admit(other_input), "request_id");
    OfflineRuntime original(*f.store, *schemas);
    const auto script = event_fake_script_from_json(fixture("s13_fake_initial.json")); FakeModel model(script.default_steps);
    const auto captured = original.reserve_call(model, other, Role::Dispatcher);
    (void)original.accept_independent_response(captured, model.complete(captured.request));
    CHECK(f.history() == before);
    const auto history = f.store->logical_agent_history(f.store->host_actor(Role::Dispatcher, other));
    CHECK(history.size() == 1 && text(history[0], "request_id") == other);
    CHECK(history[0].at("logical_agent_execution").at("logical_agent_id") != before[0].at("logical_agent_execution").at("logical_agent_id"));
}

void frozen_attempt_and_byte_caps_stop_before_new_attempts() {
    for (const auto& limits : {policy(1), policy(128, 4096)}) {
        Fixture f(limits); (void)f.run();
        CHECK(number(f.store->budget(f.id), "attempts") == 1);
        CHECK(f.store->tasks(f.id).size() == 2);
        OfflineRuntime original(*f.store, *schemas);
        expect(ErrorCode::Quota, [&] { (void)original.capture_call(f.id, Role::TeamLead, text(f.task("alpha"), "task_id")); });
        CHECK(number(f.store->budget(f.id), "attempts") == 1);
    }
}

void explicit_policy_and_same_model_are_required() {
    Fixture old(nullptr);
    expect(ErrorCode::Unauthorized, [&] { (void)old.run(); });
    CHECK(number(old.store->budget(old.id), "attempts") == 0);
    for (const auto& logical : {policy(), plain_policy()}) {
        auto snapshot = admission(logical).config_snapshot;
        snapshot.at("role_resolutions").at("worker").as_object()["reasoning"] = Object{{"mode", "off"}};
        const auto bytes = canonical_json(snapshot);
        ::test::throws([&] { (void)FrozenRequestConfig::restore(bytes, sha256_hex(bytes)); });
    }
    for (const auto& invalid : {policy(0), policy(129), policy(128, 4095), policy(128, 524289)})
        ::test::throws([&] { (void)admission(invalid); });
}
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::string_view group;
    if (argc != 1) {
        if (argc != 3 || std::string_view(argv[1]) != "--group" ||
            (std::string_view(argv[2]) != "history" && std::string_view(argv[2]) != "runtime")) {
            std::cerr << "usage: single_agent_tests [--group history|runtime]\n";
            return 2;
        }
        group = argv[2];
    }
    if (::geteuid() == 0) { std::cerr << "single-Agent tests require an ordinary user\n"; return 2; }
    assets = std::make_unique<support::Assets>(source);
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    struct GroupedCase { std::string_view group; ::test::Case test; };
    const std::vector<GroupedCase> cases{
        {"runtime", {"plain submission preserves originals pending acceptance", plain_submission_preserves_originals_and_requires_acceptance}},
        {"runtime", {"plain setup recovers atomic allocation and independent instruction", plain_interrupted_setup_recovers_without_duplicate_allocation}},
        {"runtime", {"plain rejects management and additional Worker model calls", plain_rejects_manager_and_multiple_worker_bypasses}},
        {"runtime", {"plain preserves wait UNKNOWN feedback and budget gates", plain_keeps_wait_unknown_feedback_and_budget_guards}},
        {"history", {"transcript prefixes equal complete canonical arrays", transcript_prefix_matches_complete_canonical_oracle}},
        {"history", {"transcript keeps aggregate depth and JSON guards", transcript_keeps_aggregate_depth_and_json_guards}},
        {"history", {"transcript byte limit includes the complete array", transcript_byte_limit_counts_the_complete_array}},
        {"history", {"shared native history crosses roles and tasks", shared_native_history_crosses_roles_and_tasks}},
        {"history", {"omitted original pair refuses reservation", omitted_original_pair_is_refused_before_reservation}},
        {"history", {"reordered original pairs refuse reservation", reordered_original_pairs_are_refused}},
        {"history", {"changed or missing logical identity refuses reservation", changed_or_missing_logical_identity_is_refused}},
        {"runtime", {"concurrent captures cannot fork original history", concurrent_capture_cannot_fork_the_history}},
        {"history", {"settled turn without immutable feedback blocks continuation", settled_without_feedback_cannot_advance}},
        {"runtime", {"historical Worker tools do not authorize manager actions", historical_worker_tools_do_not_authorize_a_manager}},
        {"runtime", {"ordinary restart continues one chain without rewriting bindings", normal_restart_keeps_one_chain_and_old_bindings}},
        {"runtime", {"sibling request history never enters the shared transcript", sibling_request_history_is_never_adopted}},
        {"runtime", {"frozen attempt and byte capacities stop before reservation", frozen_attempt_and_byte_caps_stop_before_new_attempts}},
        {"history", {"explicit frozen policy and common model are required", explicit_policy_and_same_model_are_required}}
    };
    std::vector<::test::Case> selected;
    for (const auto& item : cases)
        if (group.empty() || item.group == group) selected.push_back(item.test);
    return ::test::run(selected);
}
