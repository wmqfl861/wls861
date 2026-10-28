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
    auto catalog = load_config_catalog(directory.path / "config/system.yaml", directory.path / "config/functions", source / "prompts");
    auto system = catalog.freeze("general").json().at("system");
    system.at("context").as_object()["request_format"] = "structured-v2";
    if (!logical.is_null()) system.at("context").as_object()["logical_agent"] = logical;
    else system.at("context").as_object().erase("logical_agent");
    { std::ofstream output(directory.path / "config/system.yaml"); output << canonical_json(system); CHECK(output.good()); }
    catalog = load_config_catalog(directory.path / "config/system.yaml", directory.path / "config/functions", source / "prompts");
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
    explicit Fixture(Json logical = policy()) : input(admission(logical)), store(assets->open(database)) {
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
    expect(ErrorCode::InvalidInput, [&] { (void)original.accept_independent_response(captured, model.complete(captured.request)); });
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/pwn.txt"));
    CHECK(!std::filesystem::exists(f.directory.path / "beta/pwn.txt"));
    const auto history = f.history(); CHECK(history.size() == 8);
    CHECK(history.back().at("feedback").at("result").at("runtime_error") == true);
    const auto next = f.worker();
    const auto& last = next.request.context.logical_input.at("messages").as_array().back();
    CHECK(last.at("role") == "user" && last.at("tool_calls").as_array().empty());
    CHECK(text(parse_json_strict(std::string(last.at("content").as_string())), "attempt_id") == captured.request.attempt_id);
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
    auto snapshot = admission().config_snapshot;
    snapshot.at("role_resolutions").at("worker").as_object()["reasoning"] = Object{{"mode", "off"}};
    const auto bytes = canonical_json(snapshot);
    ::test::throws([&] { (void)FrozenRequestConfig::restore(bytes, sha256_hex(bytes)); });
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
