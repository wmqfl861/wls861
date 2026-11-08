#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
using namespace wls;
using namespace wls::s1;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::unique_ptr<SchemaRegistry> schemas;
std::string base_sql, store_sql, tools_sql, events_sql, workflow_sql;

std::string text(const Json& value) { return std::string(value.as_string()); }
std::int64_t number(const Json& value) { return value.is_int64() ? value.as_int64() : static_cast<std::int64_t>(value.as_uint64()); }
Json fixture(const std::string& name) { return parse_json_strict(read_runtime_file(source / "tests/fixtures" / name)); }
EventFakeScript initial_script() { return event_fake_script_from_json(fixture("s13_fake_initial.json")); }
EventFakeScript amended_script() { return event_fake_script_from_json(fixture("s13_fake_amended.json")); }
FrozenRequestConfig config() {
    return load_config_catalog(source / "config/examples/system.yaml", source / "config/examples/functions", source / "prompts").freeze("general");
}
AdmissionInput input() {
    const auto frozen = config();
    AdmissionInput result;
    result.tenant_id = "test-tenant"; result.project_id = "test-project"; result.root_id = "test-root";
    result.function_id = frozen.function_id(); result.idempotency_key = new_id("key");
    result.original_text = read_runtime_file(source / "tests/fixtures/s13_request.txt");
    result.config_snapshot = frozen.json(); result.constraint_catalog = fixture("s13_constraints.json");
    const auto grants = frozen.worker_tools(); result.tool_allowlist.assign(grants.begin(), grants.end());
    result.budget.input_tokens = 10000000; result.budget.output_tokens = 1000000;
    return result;
}
void expect(ErrorCode code, const std::function<void()>& action) {
    bool caught = false;
    try { action(); } catch (const Error& error) { CHECK(error.code() == code); caught = true; }
    CHECK(caught);
}
void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary); stream << bytes; CHECK(static_cast<bool>(stream));
}
void sql(const std::filesystem::path& path, const std::string& statement) {
    sqlite3* database = nullptr;
    CHECK(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{database};
    CHECK(sqlite3_exec(database, statement.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
}
Json task(Store& store, const std::string& id, std::string_view node) {
    for (const auto& value : store.tasks(id)) if (value.at("node_id").as_string() == node) return value;
    throw std::runtime_error("test task missing");
}
void amend(Store& store, const std::string& id) {
    static_cast<void>(store.amend(store.host_actor(Role::Root, id), number(store.request(id).at("state_version")),
        read_runtime_file(source / "tests/fixtures/s13_amendment.txt"), fixture("s13_amended_constraints.json"), new_id("amend")));
}
std::vector<Json> messages(const ModelRequest& request) {
    std::vector<Json> result;
    std::string_view remaining(request.context.dynamic_suffix);
    while (!remaining.empty()) {
        const auto separator = remaining.find(' '), newline = remaining.find('\n');
        CHECK(separator != std::string_view::npos && newline != std::string_view::npos);
        const std::string name(remaining.substr(0, separator));
        const auto length = static_cast<std::size_t>(std::stoull(std::string(remaining.substr(separator + 1, newline - separator - 1))));
        remaining.remove_prefix(newline + 1); CHECK(remaining.size() > length && remaining[length] == '\n');
        const auto frame = parse_json_strict(remaining.substr(0, length));
        if (name == "message") {
            CHECK(frame.at("role") == "tool");
            result.push_back(parse_json_strict(text(frame.at("content")), 8U * 1024U * 1024U));
        }
        remaining.remove_prefix(length + 1);
    }
    return result;
}
bool blocked(const Json& result, std::string_view code, std::string_view node = {}) {
    for (const auto& entry : result.at("blocked").as_array())
        if (entry.at("code").as_string() == code && (node.empty() || entry.at("node_id").as_string() == node)) return true;
    return false;
}
std::vector<FakeModelStep> read_steps(std::string path = "r1.txt") {
    auto step = initial_script().by_node.at("alpha").back();
    step.role_call_index = 1; step.tool_calls.front().arguments = Object{{"path", path}, {"max_bytes", 6}};
    return {step};
}
struct Fixture {
    test::TempDir directory;
    Store store;
    std::unique_ptr<WorkerTools> alpha, beta;
    std::string id;
    explicit Fixture(const AdmissionInput& admission = input())
        : store(directory.path / "control.db", base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql) {
        std::filesystem::create_directory(directory.path / "alpha");
        std::filesystem::create_directory(directory.path / "beta");
        alpha = std::make_unique<WorkerTools>(store, directory.path / "alpha");
        beta = std::make_unique<WorkerTools>(store, directory.path / "beta");
        id = text(store.admit(admission).at("request_id"));
    }
    EventRuntime runtime() { return EventRuntime(store, *schemas, {{"alpha", alpha.get()}, {"beta", beta.get()}}); }
    Json run(std::int64_t worker_calls = 2) {
        EventPumpOptions options; options.max_worker_calls_per_task = worker_calls;
        return runtime().pump(id, initial_script(), options);
    }
};

// G1-3 correction regressions: a structured-v2 frozen request on the event
// Store, an alpha-only plan and explicit per-index Worker steps. The same test
// bytes run unchanged against the exact baseline (behavioral RED) and the
// candidate (GREEN); no candidate-only symbols are referenced.
AdmissionInput structured_input(const char* schema_retry = "2") {
    test::TempDir configuration;
    auto system = read_runtime_file(source / "config/examples/system.yaml");
    const std::string original_format = "request_format: framed-v1";
    const auto format_position = system.find(original_format);
    CHECK(format_position != std::string::npos);
    system.replace(format_position, original_format.size(), "request_format: structured-v2");
    if (std::string_view(schema_retry) != "2") {
        const std::string original_retry = "schema_retry_max: 2";
        const auto retry_position = system.find(original_retry);
        CHECK(retry_position != std::string::npos);
        system.replace(retry_position, original_retry.size(), std::string("schema_retry_max: ") + schema_retry);
    }
    write(configuration.path / "system.yaml", system);
    auto result = input();
    result.config_snapshot = load_config_catalog(configuration.path / "system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general").json();
    return result;
}
FakeModelStep worker_step(std::int64_t index, const std::string& body, std::vector<ModelToolCall> calls) {
    FakeModelStep step;
    step.role = Role::Worker; step.role_call_index = index; step.text = body;
    step.tool_calls = std::move(calls); step.usage = ModelUsage{100, 20, 0, 0, 0};
    return step;
}
EventFakeScript alpha_script(std::vector<FakeModelStep> steps) {
    auto script = initial_script();
    script.default_steps.front().tool_calls.front().arguments.at("nodes").as_array().pop_back();
    auto order = script.by_node.at("alpha").front();
    script.by_node["alpha"] = {order};
    for (auto& step : steps) script.by_node["alpha"].push_back(std::move(step));
    script.by_node.erase("beta");
    return script;
}
std::vector<Json> worker_calls(const Json& result, std::string_view node) {
    std::vector<Json> found;
    for (const auto& call : result.at("calls").as_array())
        if (call.at("role") == "worker" && call.at("node_id").as_string() == node) found.push_back(call);
    return found;
}
void expect_correction_feedback(const Json& result, std::string_view reason, std::string_view layer) {
    CHECK(result.at("format").as_string() == "wls-correction-feedback-v1");
    CHECK(result.at("reason").as_string() == reason);
    CHECK(result.at("failure_layer").as_string() == layer);
    CHECK(result.at("correction_eligible") == true);
    CHECK(result.at("observation_complete") == true);
    CHECK(result.at("effect_count") == 0);
    const auto* runtime_error = result.as_object().if_contains("runtime_error");
    CHECK(!(runtime_error && *runtime_error == true));
}
const Json& native_messages(const ModelRequest& request) {
    return request.context.logical_input.at("messages");
}
const Json* paired_assistant(const ModelRequest& request, std::string_view original_call_id) {
    for (const auto& message : native_messages(request).as_array())
        if (message.at("role").as_string() == "assistant" && message.at("source").is_object() &&
            message.at("source").as_object().contains("original_tool_call_id") &&
            message.at("source").at("original_tool_call_id").as_string() == original_call_id)
            return &message;
    return nullptr;
}
const Json* paired_tool_result(const ModelRequest& request, const Json& assistant) {
    const auto id = text(assistant.at("tool_calls").as_array().front().at("call_id"));
    for (const auto& message : native_messages(request).as_array())
        if (message.at("role").as_string() == "tool" && message.at("tool_call_id").as_string() == id)
            return &message;
    return nullptr;
}

void strict_script_groups_and_host_mapping() {
    CHECK(event_fake_script_from_json(Array{}).default_steps.empty());
    CHECK(event_fake_script_from_json(Object{{"nodes", Object{{"alpha", Array{}}}}}).by_node.at("alpha").empty());
    expect(ErrorCode::InvalidInput, [] { (void)event_fake_script_from_json(Object{{"extra", Array{}}}); });
    expect(ErrorCode::InvalidInput, [] { (void)event_fake_script_from_json(Object{{"nodes", Object{{"alpha", nullptr}}}}); });
    auto invalid = fixture("s13_fake_initial.json");
    invalid.at("default").as_array().push_back(invalid.at("default").as_array().front());
    expect(ErrorCode::InvalidInput, [&] { (void)event_fake_script_from_json(invalid); });
    Fixture f;
    expect(ErrorCode::InvalidInput, [&] { EventRuntime bad(f.store, *schemas, {{"alpha", f.alpha.get()}, {"beta", f.alpha.get()}}); });
    expect(ErrorCode::InvalidInput, [&] { EventRuntime bad(f.store, *schemas, {{"alpha", nullptr}}); });
    CHECK(f.store.budget(f.id).at("attempts") == 0);
}

void independent_real_files_and_original_feedback() {
    Fixture f; const auto result = f.run();
    CHECK(result.at("calls").as_array().size() == 7);
    CHECK(result.at("business_delivered") == false && result.at("real_model_calls") == 0);
    CHECK(read_runtime_file(f.directory.path / "alpha/r1.txt") == "alpha original\n");
    CHECK(read_runtime_file(f.directory.path / "beta/r1.txt") == "beta original\n");
    CHECK(!result.at("pending").as_array().empty());
    for (const auto& node : {"alpha", "beta"}) {
        const auto assigned = task(f.store, f.id, node);
        const auto actor = f.store.host_actor(Role::Worker, f.id, text(assigned.at("team_id")));
        const auto history = f.store.attempt_history(actor);
        CHECK(history.size() == 2);
        CHECK(history.back().at("feedback").at("result").at("content").as_string().size() == 6);
        for (const auto& row : history) {
            CHECK(row.at("task_id") == assigned.at("task_id"));
            CHECK(!row.as_object().contains("request_envelope"));
            CHECK(row.at("feedback_missing") == false);
            CHECK(row.at("frozen_binding") == row.at("feedback").at("original_binding"));
        }
        const auto request = model_request_from_json(f.store.attempt(text(history.back().at("attempt_id"))).at("request_envelope"));
        const auto prior = messages(request);
        CHECK(prior.size() == 1 && prior.front() == history.front());
    }
    for (const auto& call : result.at("calls").as_array()) if (call.at("role") != "worker")
        CHECK(messages(model_request_from_json(f.store.attempt(text(call.at("attempt_id"))).at("request_envelope"))).empty());
}

void partial_amend_and_late_keep_reuse_current_plan() {
    Fixture f; (void)f.run();
    const auto a = task(f.store, f.id, "alpha");
    const FakeModel fake(read_steps()); OfflineRuntime runtime(f.store, *schemas, f.alpha.get());
    const auto captured = runtime.reserve_call(fake, f.id, Role::Worker, text(a.at("task_id")));
    amend(f.store, f.id);
    expect(ErrorCode::Stale, [&] { (void)runtime.accept_response(captured, fake.complete(captured.request)); });
    const auto original_error = f.store.tool_feedback(captured.request.attempt_id);
    CHECK(original_error.at("original_binding").at("requirements_revision") == 1);
    CHECK(original_error.at("result").at("runtime_error") == true);
    EventPumpOptions options; options.amendment_dispositions = fixture("s13_update_alpha.json");
    const auto updated = f.runtime().pump(f.id, amended_script(), options);
    CHECK(updated.at("calls").as_array().size() == 3);
    CHECK(blocked(updated, "amendment_pending", "beta"));
    CHECK(read_runtime_file(f.directory.path / "alpha/r2.txt") == "alpha revision 2\n");
    CHECK(!std::filesystem::exists(f.directory.path / "beta/r2.txt"));
    const auto alpha_instruction = task(f.store, f.id, "alpha").at("instruction_revision");
    CHECK(task(f.store, f.id, "beta").at("based_on_requirements_revision") == 1);
    auto followup = amended_script(); followup.by_node["alpha"].clear();
    options.amendment_dispositions = fixture("s13_keep_beta.json");
    const auto kept = f.runtime().pump(f.id, followup, options);
    CHECK(kept.at("calls").as_array().size() == 2);
    for (const auto& call : kept.at("calls").as_array()) CHECK(call.at("node_id") == "beta");
    CHECK(f.store.request(f.id).at("plan_revision") == 2);
    CHECK(task(f.store, f.id, "alpha").at("instruction_revision") == alpha_instruction);
    const auto beta = task(f.store, f.id, "beta");
    CHECK(beta.at("task_revision") == 2 && beta.at("instruction_revision") == 2);
    CHECK(beta.at("applied_task_revision") == 2 && beta.at("applied_instruction_revision") == 2);
    CHECK(read_runtime_file(f.directory.path / "beta/r2.txt") == "beta revision 2\n");
    CHECK(f.store.inbox(f.store.host_actor(Role::Dispatcher, f.id)).empty());
}

void waiting_task_releases_the_batch_for_its_sibling() {
    Fixture f; EventPumpOptions options; options.max_calls = 1;
    CHECK(f.runtime().pump(f.id, initial_script(), options).at("calls").as_array().size() == 1);
    auto alpha = task(f.store, f.id, "alpha");
    const auto binding = f.store.binding(text(alpha.at("task_id")));
    const auto worker = f.store.host_actor(Role::Worker, f.id, binding.team_id);
    const auto waiting = f.store.wait_worker(worker, binding, number(alpha.at("state_version")), "wait for explicit host information", new_id("wait"));
    options.max_calls = 12;
    const auto second = f.runtime().pump(f.id, initial_script(), options);
    CHECK(second.at("calls").as_array().size() == 2);
    for (const auto& call : second.at("calls").as_array()) CHECK(call.at("node_id") == "beta");
    CHECK(blocked(second, "waiting", "alpha"));
    CHECK(task(f.store, f.id, "alpha").at("instruction_revision") == 0);
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/r1.txt"));
    (void)f.store.wake_worker(worker, binding, number(waiting.at("generation")), new_id("wake"));
    auto only_alpha = initial_script(); only_alpha.by_node["beta"].clear();
    const auto third = f.runtime().pump(f.id, only_alpha, options);
    CHECK(third.at("calls").as_array().size() == 2);
    CHECK(std::filesystem::exists(f.directory.path / "alpha/r1.txt"));
}

void missing_groups_and_caps_preserve_specific_inbox_items() {
    Fixture f; EventPumpOptions options; options.max_calls = 1;
    (void)f.runtime().pump(f.id, initial_script(), options);
    auto absent = initial_script();
    absent.default_steps.insert(absent.default_steps.end(), absent.by_node.at("alpha").begin(), absent.by_node.at("alpha").end());
    absent.by_node["alpha"].clear(); absent.by_node["beta"].clear();
    const auto no_calls = f.runtime().pump(f.id, absent);
    CHECK(no_calls.at("calls").as_array().empty());
    CHECK(f.store.budget(f.id).at("attempts") == 1);
    CHECK(blocked(no_calls, "missing_script", "alpha") && blocked(no_calls, "missing_script", "beta"));
    auto alpha_only = initial_script(); alpha_only.by_node["beta"].clear();
    const auto order_only = f.runtime().pump(f.id, alpha_only, options);
    CHECK(order_only.at("calls").as_array().size() == 1);
    CHECK(order_only.at("calls").as_array().front().at("role") == "team_lead");
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/r1.txt"));
    CHECK(blocked(order_only, "call_limit", "alpha"));
    const auto worker_only = f.runtime().pump(f.id, alpha_only, options);
    CHECK(worker_only.at("calls").as_array().size() == 1);
    CHECK(worker_only.at("calls").as_array().front().at("role") == "worker");
    for (const auto& pending : worker_only.at("pending").as_array()) CHECK(pending.at("status") == "PENDING");
}

void missing_workspace_never_falls_back_to_note_or_a_sibling() {
    Fixture f;
    EventRuntime partial(f.store, *schemas, {{"alpha", f.alpha.get()}});
    auto script = initial_script();
    for (auto& step : script.by_node["beta"]) if (step.role == Role::Worker)
        step.tool_calls = {{"note-beta", "note", Object{{"text", "must not be implicitly permitted"}}}};
    const auto result = partial.pump(f.id, script);
    CHECK(result.at("calls").as_array().size() == 3);
    CHECK(blocked(result, "workspace_missing", "beta"));
    const auto beta = task(f.store, f.id, "beta");
    CHECK(beta.at("instruction_revision") == 0);
    CHECK(f.store.attempt_history(f.store.host_actor(Role::Worker, f.id, text(beta.at("team_id")))).empty());
}

void claim_failure_and_tool_error_do_not_implicitly_retry() {
    Fixture f; EventPumpOptions options; options.max_calls = 2;
    auto script = initial_script(); script.by_node["beta"].clear();
    (void)f.runtime().pump(f.id, script, options);
    const auto attempts = f.store.budget(f.id).at("attempts");
    sql(f.directory.path / "control.db", "CREATE TRIGGER fail_claim BEFORE UPDATE ON event_inbox WHEN NEW.role='worker' AND NEW.status='CLAIMED' BEGIN SELECT RAISE(ABORT,'injected claim failure'); END");
    options.max_calls = 64; options.max_worker_calls_per_task = 16;
    const auto no_claim = f.runtime().pump(f.id, script, options);
    CHECK(no_claim.at("calls").as_array().empty() && f.store.budget(f.id).at("attempts") == attempts);
    CHECK(!no_claim.at("blocked").as_array().empty());
    sql(f.directory.path / "control.db", "DROP TRIGGER fail_claim");
    script.by_node["alpha"][1].tool_calls.front().arguments.as_object()["path"] = "../outside.txt";
    const auto failed = f.runtime().pump(f.id, script, options);
    CHECK(failed.at("calls").as_array().size() == 1);
    CHECK(!std::filesystem::exists(f.directory.path / "outside.txt"));
    for (const auto& pending : failed.at("pending").as_array()) CHECK(pending.at("status") == "PENDING");
    const auto attempt_id = text(failed.at("calls").as_array().front().at("attempt_id"));
    CHECK(f.store.tool_feedback(attempt_id).at("result").at("operation").at("status") == "FAILED");
    CHECK(blocked(failed, "operation_incomplete", "alpha"));
}

void duplicate_response_returns_original_diagnostic_after_file_and_epoch_change() {
    Fixture f; (void)f.run(1);
    const auto alpha = task(f.store, f.id, "alpha");
    OfflineRuntime runtime(f.store, *schemas, f.alpha.get()); const FakeModel fake(read_steps());
    const auto captured = runtime.reserve_call(fake, f.id, Role::Worker, text(alpha.at("task_id")));
    const auto response = fake.complete(captured.request);
    const auto first = runtime.accept_response(captured, response);
    const auto saved = f.store.tool_feedback(captured.request.attempt_id);
    const auto budget = f.store.budget(f.id);
    write(f.directory.path / "alpha/r1.txt", "different current bytes\n");
    CHECK(runtime.accept_response(captured, response) == first);
    CHECK(f.store.tool_feedback(captured.request.attempt_id) == saved && f.store.budget(f.id) == budget);

    // Two hosts receive one new response at the same time. Both must observe
    // the first immutable receipt, including executed_now and the original
    // result timestamp; a pure operation replay is not an equivalent receipt.
    auto write_step = read_steps().front();
    write_step.tool_calls = {{"concurrent-write", "fs_write", Object{{"path", "concurrent.txt"}, {"content", std::string(65536, 'z')}}}};
    const FakeModel writer({write_step});
    const auto write_call = runtime.reserve_call(writer, f.id, Role::Worker, text(alpha.at("task_id")));
    const auto write_response = writer.complete(write_call.request);
    OfflineRuntime second_host(f.store, *schemas, f.alpha.get());
    std::barrier start(3);
    auto left = std::async(std::launch::async, [&] {
        start.arrive_and_wait(); return runtime.accept_response(write_call, write_response);
    });
    auto right = std::async(std::launch::async, [&] {
        start.arrive_and_wait(); return second_host.accept_response(write_call, write_response);
    });
    start.arrive_and_wait();
    const auto left_result = left.get(), right_result = right.get();
    CHECK(left_result == right_result && left_result.at("executed_now") == true);
    CHECK(read_runtime_file(f.directory.path / "alpha/concurrent.txt") == std::string(65536, 'z'));
    CHECK(f.store.tool_feedback(write_call.request.attempt_id).at("result") == left_result);
    std::size_t receipt_events = 0, operation_result_events = 0;
    for (const auto& event : f.store.events(f.id, 0, 1000)) {
        if (event.at("type") == "tool_feedback" && text(event.at("payload").at("attempt_id")) == write_call.request.attempt_id)
            ++receipt_events;
        if (event.at("type") == "record_op_result" &&
            event.at("result").at("op_id") == left_result.at("operation").at("op_id")) ++operation_result_events;
    }
    CHECK(receipt_events == 1 && operation_result_events == 1);
    amend(f.store, f.id);
    CHECK(runtime.accept_response(captured, response) == first);
    auto forged = response; forged.tool_calls.front().arguments.as_object()["max_bytes"] = 2;
    expect(ErrorCode::Conflict, [&] { (void)runtime.accept_response(captured, forged); });
    CHECK(f.store.tool_feedback(captured.request.attempt_id) == saved);
}

void settled_without_feedback_is_missing_and_cannot_reread() {
    Fixture f; (void)f.run(1);
    const auto alpha = task(f.store, f.id, "alpha");
    OfflineRuntime runtime(f.store, *schemas, f.alpha.get()); const FakeModel fake(read_steps());
    const auto captured = runtime.reserve_call(fake, f.id, Role::Worker, text(alpha.at("task_id")));
    const auto response = fake.complete(captured.request);
    (void)f.store.settle_attempt(captured.request.attempt_id, response.binding, response.config_snapshot_id,
                              "COMPLETED", to_json(response.usage), to_json(response));
    expect(ErrorCode::NotReady, [&] { (void)runtime.accept_response(captured, response); });
    CHECK(f.store.tool_feedback(captured.request.attempt_id).is_null());
    const auto history = f.store.attempt_history(captured.actor);
    CHECK(history.back().at("feedback_missing") == true);
    CHECK(history.back().at("response") == to_json(response));
}

void reopen_recovers_orders_and_rebuilds_original_scoped_history() {
    test::TempDir directory;
    std::filesystem::create_directory(directory.path / "alpha");
    std::filesystem::create_directory(directory.path / "beta");
    std::string id, alpha_id; std::vector<Json> original;
    {
        Store store(directory.path / "control.db", base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql);
        id = text(store.admit(input()).at("request_id"));
        WorkerTools alpha(store, directory.path / "alpha"), beta(store, directory.path / "beta");
        EventPumpOptions options; options.max_worker_calls_per_task = 2;
        (void)EventRuntime(store, *schemas, {{"alpha", &alpha}, {"beta", &beta}}).pump(id, initial_script(), options);
        const auto a = task(store, id, "alpha"); alpha_id = text(a.at("task_id"));
        original = store.attempt_history(store.host_actor(Role::Worker, id, text(a.at("team_id"))));
    }
    write(directory.path / "alpha/r1.txt", "host changed this later\n");
    Store store(directory.path / "control.db", base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql);
    WorkerTools alpha(store, directory.path / "alpha"), beta(store, directory.path / "beta");
    auto script = initial_script();
    for (auto& [node, steps] : script.by_node) {
        auto read = read_steps().front(); read.tool_calls.front().call_id = "read-" + node;
        steps = {steps.front(), read};
    }
    const auto result = EventRuntime(store, *schemas, {{"alpha", &alpha}, {"beta", &beta}}).pump(id, script);
    CHECK(result.at("calls").as_array().size() == 2);
    for (const auto& call : result.at("calls").as_array()) {
        CHECK(call.at("role") == "worker");
        if (call.at("role") == "worker" && call.at("node_id") == "alpha") {
            const auto captured = model_request_from_json(store.attempt(text(call.at("attempt_id"))).at("request_envelope"));
            CHECK(messages(captured) == original);
            CHECK(captured.binding.execution_epoch > number(original.front().at("frozen_binding").at("execution_epoch")));
            CHECK(call.at("result").at("content") == "host c");
        }
    }
    CHECK(store.task(alpha_id).at("instruction_revision") == 1);
    CHECK(store.tool_feedback(text(original.back().at("attempt_id"))) == original.back().at("feedback"));
}

void context_overflow_blocks_before_another_attempt() {
    Fixture f; write(f.directory.path / "alpha/big.txt", std::string(65536, 'x'));
    auto script = initial_script();
    script.default_steps.front().tool_calls.front().arguments.at("nodes").as_array().pop_back();
    script.by_node.erase("beta");
    auto read = read_steps("big.txt").front(); read.tool_calls.front().arguments.as_object()["max_bytes"] = 65536;
    script.by_node["alpha"].resize(1);
    for (std::int64_t index = 1; index <= 16; ++index) {
        read.role_call_index = index; script.by_node["alpha"].push_back(read);
    }
    EventPumpOptions options; options.max_calls = 64; options.max_worker_calls_per_task = 16;
    const auto result = EventRuntime(f.store, *schemas, {{"alpha", f.alpha.get()}}).pump(f.id, script, options);
    CHECK(result.at("calls").as_array().size() > 3 && result.at("calls").as_array().size() < 18);
    CHECK(blocked(result, "quota", "alpha"));
    CHECK(number(f.store.budget(f.id).at("attempts")) == static_cast<std::int64_t>(result.at("calls").as_array().size()));
    const auto before = f.store.budget(f.id).at("attempts");
    const FakeModel fake(read_steps("big.txt"));
    OfflineRuntime runtime(f.store, *schemas, f.alpha.get());
    expect(ErrorCode::Quota, [&] { (void)runtime.reserve_call(fake, f.id, Role::Worker, text(task(f.store, f.id, "alpha").at("task_id"))); });
    CHECK(f.store.budget(f.id).at("attempts") == before);
}

void frozen_manager_capacity_and_budget_keep_unprocessed_work() {
    test::TempDir configuration;
    auto system = read_runtime_file(source / "config/examples/system.yaml");
    const std::string existing = "manager_max_tool_calls_per_wake: 12";
    const auto position = system.find(existing); CHECK(position != std::string::npos);
    system.replace(position, existing.size(), "manager_max_tool_calls_per_wake: 1");
    write(configuration.path / "system.yaml", system);
    auto admission = input();
    admission.config_snapshot = load_config_catalog(configuration.path / "system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general").json();
    Fixture f(admission);
    const auto first = f.runtime().pump(f.id, initial_script());
    CHECK(first.at("calls").as_array().size() == 1 && first.at("manager_calls") == 1);
    CHECK(blocked(first, "manager_limit", "alpha") && blocked(first, "manager_limit", "beta"));
    const auto second = f.runtime().pump(f.id, initial_script());
    CHECK(second.at("manager_calls") == 1 && second.at("calls").as_array().size() == 2);
    CHECK(task(f.store, f.id, "alpha").at("instruction_revision") == 1);
    CHECK(task(f.store, f.id, "beta").at("instruction_revision") == 0);
    auto limited = input(); limited.budget.max_attempts = 2;
    Fixture full(limited);
    const auto exhausted = full.runtime().pump(full.id, initial_script());
    CHECK(exhausted.at("calls").as_array().size() == 2);
    CHECK(full.store.budget(full.id).at("attempts") == 2);
    CHECK(blocked(exhausted, "quota", "alpha") && blocked(exhausted, "quota", "beta"));
    CHECK(!exhausted.at("pending").as_array().empty());
    for (const auto& pending : exhausted.at("pending").as_array()) CHECK(pending.at("status") == "PENDING");
}

void terminal_tasks_remain_visible_as_unprocessed_history() {
    Fixture f; (void)f.run(1);
    const auto before = f.store.request_inbox(f.id);
    CHECK(!before.empty());
    (void)f.store.cancel_request(f.store.host_actor(Role::Root, f.id), number(f.store.request(f.id).at("state_version")), new_id("cancel"));
    const auto result = f.runtime().pump(f.id, EventFakeScript{});
    CHECK(result.at("calls").as_array().empty() && result.at("request_status") == "CANCELLED");
    CHECK(result.at("pending").as_array().size() == before.size());
    for (const auto& item : before) {
        CHECK(std::any_of(result.at("pending").as_array().begin(), result.at("pending").as_array().end(),
            [&](const auto& pending) { return pending.at("inbox_id") == item.at("inbox_id"); }));
    }
}

void legacy_single_task_runner_cannot_supersede_a_durable_wait() {
    Fixture f;
    auto grouped = initial_script();
    grouped.default_steps.front().tool_calls.front().arguments.at("nodes").as_array().pop_back();
    auto steps = grouped.default_steps;
    steps.insert(steps.end(), grouped.by_node.at("alpha").begin(), grouped.by_node.at("alpha").end());
    OfflineRuntime legacy(f.store, *schemas, f.alpha.get());
    CHECK(legacy.run(f.id, steps).at("calls").as_array().size() == 3);
    CHECK(f.store.tasks(f.id).size() == 1);
    const auto alpha = task(f.store, f.id, "alpha");
    const auto binding = f.store.binding(text(alpha.at("task_id")));
    const auto worker = f.store.host_actor(Role::Worker, f.id, binding.team_id);
    const auto waiting = f.store.wait_worker(worker, binding, number(alpha.at("state_version")),
        "explicit host answer still pending", new_id("wait"));
    const auto budget = f.store.budget(f.id);
    const auto before_task = f.store.task(binding.task_id);
    const auto before_events = f.store.events(f.id, 0, 1000);
    for (auto& step : steps) if (step.role == Role::Worker && step.role_call_index == 1)
        step.tool_calls.front().arguments = Object{{"path", "after-wake.txt"}, {"content", "explicit wake permits this new effect\n"}};
    const FakeModel waiting_script(steps);
    expect(ErrorCode::NotReady, [&] {
        (void)legacy.reserve_call(waiting_script, f.id, Role::TeamLead, binding.task_id);
    });
    expect(ErrorCode::NotReady, [&] { (void)legacy.run(f.id, steps); });
    CHECK(f.store.budget(f.id) == budget);
    CHECK(f.store.task(binding.task_id) == before_task);
    CHECK(f.store.worker_wait(binding.task_id) == waiting && waiting.at("status") == "WAITING");
    CHECK(f.store.events(f.id, 0, 1000) == before_events);
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/after-wake.txt"));
    (void)f.store.wake_worker(worker, binding, number(waiting.at("generation")), new_id("wake"));
    const auto resumed = legacy.run(f.id, steps);
    CHECK(resumed.at("calls").as_array().size() == 1);
    CHECK(resumed.at("calls").as_array().front().at("role") == "worker");
    CHECK(number(f.store.budget(f.id).at("attempts")) == number(budget.at("attempts")) + 1);
    CHECK(f.store.task(binding.task_id).at("instruction_revision") == 1);
    CHECK(f.store.task(binding.task_id).at("applied_instruction_revision") == 1);
    CHECK(read_runtime_file(f.directory.path / "alpha/after-wake.txt") == "explicit wake permits this new effect\n");
}

void text_only_response_gets_one_correction_within_the_same_pump() {
    Fixture f(structured_input());
    const auto script = alpha_script({
        worker_step(1, "only text without any tool call", {}),
        worker_step(2, "", {{"write-fixed", "fs_write", Object{{"path", "corrected.txt"}, {"content", "fixed content\n"}}}}),
    });
    EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
    const auto result = f.runtime().pump(f.id, script, options);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 2);
    const auto feedback = f.store.tool_feedback(text(workers.front().at("attempt_id")));
    CHECK(!feedback.is_null());
    expect_correction_feedback(feedback.at("result"), "missing_tool_call", "model_schema");
    CHECK(read_runtime_file(f.directory.path / "alpha/corrected.txt") == "fixed content\n");
    CHECK(number(f.store.budget(f.id).at("attempts")) == 4);
    const auto alpha = task(f.store, f.id, "alpha");
    const auto actor = f.store.host_actor(Role::Worker, f.id, text(alpha.at("team_id")));
    const auto history = f.store.attempt_history(actor);
    CHECK(history.size() == 2);
    CHECK(history.front().at("feedback_missing") == false && history.back().at("feedback_missing") == false);
    // The next capture observes the original failure as data only: no synthetic
    // function_call or orphan function_call_output is invented for text output.
    const auto envelope = model_request_from_json(
        f.store.attempt(text(workers.back().at("attempt_id"))).at("request_envelope"));
    const auto& messages = native_messages(envelope).as_array();
    CHECK(messages.size() == 1);
    const auto& observation = messages.front();
    CHECK(observation.at("role").as_string() == "user");
    CHECK(observation.at("source").at("kind").as_string() == "unapplied_or_missing_observation");
    CHECK(observation.at("source").at("not_execution_authority") == true);
    CHECK(observation.at("content").as_string().find("only text without any tool call") != std::string::npos);
    CHECK(observation.at("content").as_string().find("missing_tool_call") != std::string::npos);
}

void invalid_tool_arguments_settle_as_feedback_without_operation_and_correct() {
    Fixture f(structured_input());
    const Json bad_arguments = Object{{"path", "args.txt"}};
    const auto script = alpha_script({
        worker_step(1, "", {{"bad-write", "fs_write", bad_arguments}}),
        worker_step(2, "", {{"fixed-write", "fs_write", Object{{"path", "args.txt"}, {"content", "second attempt valid\n"}}}}),
    });
    EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
    const auto result = f.runtime().pump(f.id, script, options);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 2);
    const auto feedback = f.store.tool_feedback(text(workers.front().at("attempt_id")));
    expect_correction_feedback(feedback.at("result"), "invalid_arguments", "model_schema");
    const auto alpha = task(f.store, f.id, "alpha");
    const auto binding = f.store.binding(text(alpha.at("task_id")));
    const auto actor = f.store.host_actor(Role::Worker, f.id, text(alpha.at("team_id")));
    CHECK(f.store.operation_for_key(actor, binding,
        text(workers.front().at("attempt_id")), "fs_write", bad_arguments).is_null());
    CHECK(read_runtime_file(f.directory.path / "alpha/args.txt") == "second attempt valid\n");
    CHECK(number(f.store.budget(f.id).at("attempts")) == 4);
    // The schema-invalid original arguments survive verbatim as history data and
    // pair natively with their ordinary failure observation.
    const auto envelope = model_request_from_json(
        f.store.attempt(text(workers.back().at("attempt_id"))).at("request_envelope"));
    const auto* assistant = paired_assistant(envelope, "bad-write");
    CHECK(assistant != nullptr);
    if (assistant) {
        CHECK(assistant->at("tool_calls").as_array().size() == 1);
        CHECK(assistant->at("tool_calls").as_array().front().at("name").as_string() == "fs_write");
        CHECK(assistant->at("tool_calls").as_array().front().at("arguments") == bad_arguments);
        const auto* observation = paired_tool_result(envelope, *assistant);
        CHECK(observation != nullptr);
        if (observation)
            CHECK(parse_json_strict(text(observation->at("content")), 1048576).at("reason").as_string() == "invalid_arguments");
    }
}

void missing_file_read_becomes_ordinary_tool_result_and_corrects() {
    Fixture f(structured_input());
    write(f.directory.path / "alpha/present.txt", "present bytes\n");
    const auto script = alpha_script({
        worker_step(1, "", {{"missing-read", "fs_read", Object{{"path", "absent.txt"}, {"max_bytes", 16}}}}),
        worker_step(2, "", {{"present-read", "fs_read", Object{{"path", "present.txt"}, {"max_bytes", 64}}}}),
    });
    EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
    const auto result = f.runtime().pump(f.id, script, options);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 2);
    const auto feedback = f.store.tool_feedback(text(workers.front().at("attempt_id")));
    const auto& failure = feedback.at("result");
    expect_correction_feedback(failure, "not_found", "tool_result");
    CHECK(!failure.as_object().contains("content"));
    // A diagnostic read never creates an operation row by design (Store
    // operation keys exist only for business tools); the SQL-level zero-op
    // assertion for fs_read lives in the TLS L3 regression.
    CHECK(workers.back().at("result").at("content") == "present bytes\n");
    CHECK(number(f.store.budget(f.id).at("attempts")) == 4);
    // The deterministic missing leaf is an ordinary failed tool turn in history.
    const auto envelope = model_request_from_json(
        f.store.attempt(text(workers.back().at("attempt_id"))).at("request_envelope"));
    const auto* assistant = paired_assistant(envelope, "missing-read");
    CHECK(assistant != nullptr);
    if (assistant) {
        const auto* observation = paired_tool_result(envelope, *assistant);
        CHECK(observation != nullptr);
        if (observation)
            CHECK(parse_json_strict(text(observation->at("content")), 1048576).at("reason").as_string() == "not_found");
    }
}

void settled_creation_precondition_failure_corrects_with_cas() {
    Fixture f(structured_input());
    write(f.directory.path / "alpha/exists.txt", "original bytes\n");
    const auto script = alpha_script({
        worker_step(1, "", {{"create-existing", "fs_write", Object{{"path", "exists.txt"}, {"content", "must not overwrite\n"}}}}),
        worker_step(2, "", {{"cas-write", "fs_write", Object{{"path", "exists.txt"}, {"content", "cas corrected\n"},
            {"expected_sha256", sha256_hex("original bytes\n")}}}}),
    });
    EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
    const auto result = f.runtime().pump(f.id, script, options);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 2);
    const auto& first = workers.front().at("result");
    CHECK(first.at("operation").at("status") == "FAILED");
    CHECK(first.at("execution_result").at("observed_outcome") == "FAILED");
    const auto& evidence = first.at("execution_result").at("evidence");
    CHECK(evidence.at("cause_code").as_string() == "conflict");
    CHECK(evidence.at("rename_attempted") == false);
    CHECK(evidence.at("renamed") == false);
    CHECK(evidence.at("temporary_remaining") == false);
    CHECK(evidence.at("temporary_cleanup_complete") == true);
    CHECK(evidence.at("failure_kind").as_string() == "content_precondition");
    const auto& correction = first.at("correction_feedback");
    CHECK(correction.at("format").as_string() == "wls-correction-feedback-v1");
    CHECK(correction.at("reason").as_string() == "content_precondition");
    CHECK(correction.at("correction_eligible") == true);
    CHECK(read_runtime_file(f.directory.path / "alpha/exists.txt") == "cas corrected\n");
    CHECK(number(f.store.budget(f.id).at("attempts")) == 4);
    CHECK(!blocked(result, "operation_incomplete", "alpha"));
    // The original FAILED operation and its result stay visible and immutable.
    CHECK(f.store.operation(text(first.at("operation").at("op_id"))).at("status") == "FAILED");
}

void schema_retry_exhaustion_keeps_exact_attempts_and_last_feedback() {
    {
        Fixture f(structured_input());
        const auto script = alpha_script({
            worker_step(1, "text-only failure one", {}),
            worker_step(2, "text-only failure two", {}),
            worker_step(3, "text-only failure three", {}),
        });
        EventPumpOptions options; options.max_worker_calls_per_task = 16; options.max_calls = 16;
        const auto result = f.runtime().pump(f.id, script, options);
        const auto workers = worker_calls(result, "alpha");
        CHECK(workers.size() == 3);
        const auto last = f.store.tool_feedback(text(workers.back().at("attempt_id")));
        expect_correction_feedback(last.at("result"), "missing_tool_call", "model_schema");
        CHECK(number(f.store.budget(f.id).at("attempts")) == 5);
        CHECK(!result.at("pending").as_array().empty());
        CHECK(!std::filesystem::exists(f.directory.path / "alpha/corrected.txt"));
    }
    {
        // schema_retry_max=0 still classifies and persists the failure; no retry.
        Fixture f(structured_input("0"));
        const auto script = alpha_script({
            worker_step(1, "text-only failure without retries", {}),
            worker_step(2, "", {{"never-used", "fs_write", Object{{"path", "never.txt"}, {"content", "x"}}}}),
        });
        EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
        const auto result = f.runtime().pump(f.id, script, options);
        const auto workers = worker_calls(result, "alpha");
        CHECK(workers.size() == 1);
        const auto feedback = f.store.tool_feedback(text(workers.front().at("attempt_id")));
        expect_correction_feedback(feedback.at("result"), "missing_tool_call", "model_schema");
        CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
        CHECK(!std::filesystem::exists(f.directory.path / "alpha/never.txt"));
        CHECK(!result.at("pending").as_array().empty());
    }
}

void correction_admission_respects_worker_call_call_and_script_gates() {
    const auto failing_then_fixed = [] {
        return std::vector<FakeModelStep>{
            worker_step(1, "text-only failure before a gate", {}),
            worker_step(2, "", {{"gated-write", "fs_write", Object{{"path", "gated.txt"}, {"content", "never written\n"}}}}),
        };
    };
    {
        // The correction consumes the remaining per-task Worker call slot.
        Fixture f(structured_input());
        EventPumpOptions options; options.max_worker_calls_per_task = 1; options.max_calls = 12;
        const auto result = f.runtime().pump(f.id, alpha_script(failing_then_fixed()), options);
        const auto workers = worker_calls(result, "alpha");
        CHECK(workers.size() == 1);
        expect_correction_feedback(f.store.tool_feedback(text(workers.front().at("attempt_id"))).at("result"),
            "missing_tool_call", "model_schema");
        CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
        CHECK(!std::filesystem::exists(f.directory.path / "alpha/gated.txt"));
    }
    {
        // The pump's total call cap bounds correction admissions identically.
        Fixture f(structured_input());
        EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 3;
        const auto result = f.runtime().pump(f.id, alpha_script(failing_then_fixed()), options);
        const auto workers = worker_calls(result, "alpha");
        CHECK(workers.size() == 1);
        expect_correction_feedback(f.store.tool_feedback(text(workers.front().at("attempt_id"))).at("result"),
            "missing_tool_call", "model_schema");
        CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
        CHECK(!std::filesystem::exists(f.directory.path / "alpha/gated.txt"));
    }
    {
        // A missing next script step stops before reserving a nonexistent action.
        Fixture f(structured_input());
        EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
        const auto result = f.runtime().pump(f.id,
            alpha_script({worker_step(1, "text-only failure without a next step", {})}), options);
        const auto workers = worker_calls(result, "alpha");
        CHECK(workers.size() == 1);
        expect_correction_feedback(f.store.tool_feedback(text(workers.front().at("attempt_id"))).at("result"),
            "missing_tool_call", "model_schema");
        CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
        CHECK(!result.at("pending").as_array().empty());
    }
}

void tight_request_budget_admits_exactly_one_correction_round() {
    auto admission = structured_input();
    admission.budget.max_attempts = 4;
    Fixture f(admission);
    const auto script = alpha_script({
        worker_step(1, "text-only failure inside the last admitted round", {}),
        worker_step(2, "", {{"budgeted-write", "fs_write", Object{{"path", "budgeted.txt"}, {"content", "within budget\n"}}}}),
    });
    EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
    (void)f.runtime().pump(f.id, script, options);
    CHECK(number(f.store.budget(f.id).at("attempts")) == 4);
    CHECK(read_runtime_file(f.directory.path / "alpha/budgeted.txt") == "within budget\n");
}

void recapture_overflow_closes_the_chain_before_any_reservation() {
    Fixture f(structured_input());
    write(f.directory.path / "alpha/big.txt", std::string(65536, 'x'));
    OfflineRuntime probe_host(f.store, *schemas, f.alpha.get());
    // Load successful 64 KiB reads until the next Worker capture sits close to
    // the 1 MiB structured-context limit, so one more bounded observation
    // crosses it during the correction recapture.
    std::size_t captured_bytes = 0;
    for (int round = 0; round < 16; ++round) {
        const auto script = alpha_script({worker_step(1, "", {{"probe-read", "fs_read",
            Object{{"path", "big.txt"}, {"max_bytes", 65536}}}})});
        EventPumpOptions options; options.max_worker_calls_per_task = 1; options.max_calls = 12;
        const auto result = f.runtime().pump(f.id, script, options);
        CHECK(worker_calls(result, "alpha").size() == 1);
        const auto alpha = task(f.store, f.id, "alpha");
        const auto probe = probe_host.capture_call(f.id, Role::Worker, text(alpha.at("task_id")));
        captured_bytes = probe.request.context.prompt().size();
        if (captured_bytes >= 820 * 1024) break;
    }
    CHECK(captured_bytes >= 820 * 1024 && captured_bytes < 1024 * 1024);
    const auto attempts_before = number(f.store.budget(f.id).at("attempts"));
    const auto script = alpha_script({
        worker_step(1, std::string(250 * 1024, 't'), {}),
        worker_step(2, "", {{"overflow-write", "fs_write", Object{{"path", "overflow.txt"}, {"content", "never\n"}}}}),
    });
    EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
    const auto result = f.runtime().pump(f.id, script, options);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 1);
    CHECK(number(f.store.budget(f.id).at("attempts")) == attempts_before + 1);
    CHECK(blocked(result, "quota", "alpha"));
    const auto feedback = f.store.tool_feedback(text(workers.front().at("attempt_id")));
    expect_correction_feedback(feedback.at("result"), "missing_tool_call", "model_schema");
    CHECK(feedback.at("response").at("text").as_string().size() == 250 * 1024);
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/overflow.txt"));
    CHECK(!result.at("pending").as_array().empty());
}

void multi_tool_undeclared_and_management_failures_stay_stopped() {
    {
        // Two declared calls never authorize a subset dispatch.
        Fixture f(structured_input());
        const auto script = alpha_script({worker_step(1, "", {
            {"left-write", "fs_write", Object{{"path", "left.txt"}, {"content", "left\n"}}},
            {"right-write", "fs_write", Object{{"path", "right.txt"}, {"content", "right\n"}}}})});
        EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
        const auto result = f.runtime().pump(f.id, script, options);
        const auto workers = worker_calls(result, "alpha");
        CHECK(workers.size() == 1);
        CHECK(f.store.tool_feedback(text(workers.front().at("attempt_id"))).at("result").at("runtime_error") == true);
        CHECK(!std::filesystem::exists(f.directory.path / "alpha/left.txt"));
        CHECK(!std::filesystem::exists(f.directory.path / "alpha/right.txt"));
        CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
        CHECK(!result.at("pending").as_array().empty());
    }
    {
        // Text plus one legal declared tool keeps the existing single execution.
        Fixture f(structured_input());
        const auto script = alpha_script({worker_step(1, "progress narrative before the tool", {
            {"legal-write", "fs_write", Object{{"path", "legal.txt"}, {"content", "legal once\n"}}}})});
        EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
        const auto result = f.runtime().pump(f.id, script, options);
        const auto workers = worker_calls(result, "alpha");
        CHECK(workers.size() == 1);
        CHECK(read_runtime_file(f.directory.path / "alpha/legal.txt") == "legal once\n");
        const auto& single = workers.front().at("result");
        CHECK(single.at("operation").at("status") == "SUCCEEDED");
        CHECK(!single.as_object().contains("correction_eligible"));
        CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
    }
    {
        // An undeclared tool name stays refused even when text is present.
        Fixture f(structured_input());
        const auto script = alpha_script({worker_step(1, "narrative with a foreign call", {
            {"foreign-call", "submit_plan", Object{{"summary", "x"}}}})});
        EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
        const auto result = f.runtime().pump(f.id, script, options);
        const auto workers = worker_calls(result, "alpha");
        CHECK(workers.size() == 1);
        CHECK(f.store.tool_feedback(text(workers.front().at("attempt_id"))).at("result").at("runtime_error") == true);
        CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
        CHECK(!result.at("pending").as_array().empty());
    }
    {
        // Management-role failures never gain Worker-style correction.
        Fixture f(structured_input());
        auto script = alpha_script({worker_step(1, "", {{"after-order", "fs_write",
            Object{{"path", "after.txt"}, {"content", "x\n"}}}})});
        script.by_node["alpha"].front().tool_calls.front().arguments.as_object()["steps"] = "must be an array";
        EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
        const auto result = f.runtime().pump(f.id, script, options);
        CHECK(worker_calls(result, "alpha").empty());
        CHECK(number(f.store.budget(f.id).at("attempts")) == 2);
        CHECK(!std::filesystem::exists(f.directory.path / "alpha/after.txt"));
    }
}

void duplicate_accept_returns_the_identical_original_receipt() {
    Fixture f(structured_input());
    (void)f.run(1);
    const auto alpha = task(f.store, f.id, "alpha");
    OfflineRuntime runtime(f.store, *schemas, f.alpha.get());
    const FakeModel fake({worker_step(1, "text-only duplicate delivery", {})});
    const auto captured = runtime.reserve_call(fake, f.id, Role::Worker, text(alpha.at("task_id")));
    const auto response = fake.complete(captured.request);
    Json first;
    bool rejected = false;
    try { first = runtime.accept_response(captured, response); }
    catch (const Error& error) { rejected = true; CHECK(error.code() == ErrorCode::InvalidInput); }
    const auto saved = f.store.tool_feedback(captured.request.attempt_id);
    const auto budget = f.store.budget(f.id);
    Json second;
    bool rejected_again = false;
    try { second = runtime.accept_response(captured, response); }
    catch (const Error& error) { rejected_again = true; CHECK(error.code() == ErrorCode::InvalidInput); }
    CHECK(rejected == rejected_again);
    CHECK(f.store.tool_feedback(captured.request.attempt_id) == saved);
    CHECK(f.store.budget(f.id) == budget);
    if (!rejected) {
        CHECK(second == first);
        CHECK(first.at("format").as_string() == "wls-correction-feedback-v1");
    }
}

void sibling_node_keeps_its_own_history_and_budget_during_correction() {
    Fixture f(structured_input());
    auto script = initial_script();
    script.by_node["alpha"] = {
        script.by_node.at("alpha").front(),
        worker_step(1, "alpha text-only failure one", {}),
        worker_step(2, "alpha text-only failure two", {}),
        worker_step(3, "alpha text-only failure three", {}),
    };
    EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 16;
    const auto result = f.runtime().pump(f.id, script, options);
    const auto alpha_workers = worker_calls(result, "alpha");
    const auto beta_workers = worker_calls(result, "beta");
    CHECK(alpha_workers.size() == 3);
    CHECK(beta_workers.size() == 2);
    expect_correction_feedback(f.store.tool_feedback(text(alpha_workers.back().at("attempt_id"))).at("result"),
        "missing_tool_call", "model_schema");
    // The untouched sibling keeps its exact original facts and progress.
    CHECK(read_runtime_file(f.directory.path / "beta/r1.txt") == "beta original\n");
    const auto beta = task(f.store, f.id, "beta");
    const auto beta_actor = f.store.host_actor(Role::Worker, f.id, text(beta.at("team_id")));
    const auto beta_history = f.store.attempt_history(beta_actor);
    CHECK(beta_history.size() == 2);
    for (const auto& row : beta_history) CHECK(row.at("feedback_missing") == false);
    CHECK(f.store.tool_feedback(text(beta_workers.back().at("attempt_id")))
        .at("result").at("content").as_string().size() == 6);
    CHECK(number(f.store.budget(f.id).at("attempts")) == 8);
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/r1.txt"));
    CHECK(!result.at("pending").as_array().empty());
}

void feedback_persistence_failure_stops_the_actor_without_correction() {
    Fixture f(structured_input());
    const auto script = alpha_script({
        worker_step(1, "text-only failure before a feedback outage", {}),
        worker_step(2, "", {{"unwritten", "fs_write", Object{{"path", "unwritten.txt"}, {"content", "x\n"}}}}),
    });
    sql(f.directory.path / "control.db",
        "CREATE TRIGGER fail_feedback BEFORE INSERT ON events WHEN NEW.type='tool_feedback' AND EXISTS ("
        "SELECT 1 FROM call_attempts WHERE 'feedback:'||attempt_id = NEW.command_id AND role='worker') "
        "BEGIN SELECT RAISE(ABORT,'injected feedback failure'); END");
    EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
    const auto result = f.runtime().pump(f.id, script, options);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 1);
    CHECK(f.store.tool_feedback(text(workers.front().at("attempt_id"))).is_null());
    CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/unwritten.txt"));
    CHECK(!result.at("pending").as_array().empty());
}

// G1-3 third Fake entry: a workflow-enabled Store (base schema plus 004/005/
// 006/007, schema 405) routes EventRuntime::pump into pump_workflow, the same
// bounded Worker claim loop pump_live later drives. The same test bytes are a
// behavioral RED on the exact baseline and GREEN on the correction candidate;
// no candidate-only symbol is referenced.
struct WorkflowFixture {
    test::TempDir directory;
    Store store;
    std::unique_ptr<WorkerTools> alpha;
    std::string id;
    explicit WorkflowFixture(const AdmissionInput& admission = structured_input())
        : store(directory.path / "workflow.db", base_sql, store_sql, StoreOpenMode::Takeover,
                tools_sql, events_sql, workflow_sql) {
        std::filesystem::create_directory(directory.path / "alpha");
        alpha = std::make_unique<WorkerTools>(store, directory.path / "alpha");
        id = text(store.admit(admission).at("request_id"));
    }
    EventRuntime runtime() { return EventRuntime(store, *schemas, {{"alpha", alpha.get()}}); }
    Json run(const EventFakeScript& script, std::int64_t worker_calls = 4) {
        EventPumpOptions options;
        options.max_worker_calls_per_task = worker_calls;
        options.max_calls = 16;
        return runtime().pump(id, script, options);
    }
};

void workflow_pump_text_only_failure_corrects_within_one_pump() {
    WorkflowFixture f;
    const auto script = alpha_script({
        worker_step(1, "workflow text-only failure", {}),
        worker_step(2, "", {{"workflow-fixed", "fs_write", Object{{"path", "corrected.txt"}, {"content", "workflow fixed\n"}}}}),
    });
    const auto result = f.run(script);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 2);
    const auto feedback = f.store.tool_feedback(text(workers.front().at("attempt_id")));
    expect_correction_feedback(feedback.at("result"), "missing_tool_call", "model_schema");
    CHECK(read_runtime_file(f.directory.path / "alpha/corrected.txt") == "workflow fixed\n");
    CHECK(number(f.store.budget(f.id).at("attempts")) == 4);
    // The corrected capture observes the failed round as data only: no invented
    // function_call or orphan function_call_output for text output.
    const auto envelope = model_request_from_json(
        f.store.attempt(text(workers.back().at("attempt_id"))).at("request_envelope"));
    const auto& messages = native_messages(envelope).as_array();
    CHECK(messages.size() == 1);
    CHECK(messages.front().at("role").as_string() == "user");
    CHECK(messages.front().at("source").at("kind").as_string() == "unapplied_or_missing_observation");
    CHECK(messages.front().at("source").at("not_execution_authority") == true);
}

void workflow_pump_invalid_arguments_correct_within_one_pump() {
    WorkflowFixture f;
    const Json bad_arguments = Object{{"path", "args.txt"}};
    const auto script = alpha_script({
        worker_step(1, "", {{"workflow-bad-write", "fs_write", bad_arguments}}),
        worker_step(2, "", {{"workflow-fixed-write", "fs_write", Object{{"path", "args.txt"}, {"content", "workflow args fixed\n"}}}}),
    });
    const auto result = f.run(script);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 2);
    expect_correction_feedback(f.store.tool_feedback(text(workers.front().at("attempt_id"))).at("result"),
        "invalid_arguments", "model_schema");
    const auto alpha = task(f.store, f.id, "alpha");
    const auto binding = f.store.binding(text(alpha.at("task_id")));
    const auto actor = f.store.host_actor(Role::Worker, f.id, text(alpha.at("team_id")));
    CHECK(f.store.operation_for_key(actor, binding,
        text(workers.front().at("attempt_id")), "fs_write", bad_arguments).is_null());
    CHECK(read_runtime_file(f.directory.path / "alpha/args.txt") == "workflow args fixed\n");
    CHECK(number(f.store.budget(f.id).at("attempts")) == 4);
    // The schema-invalid original arguments survive verbatim in native history
    // and pair with their ordinary failure observation.
    const auto envelope = model_request_from_json(
        f.store.attempt(text(workers.back().at("attempt_id"))).at("request_envelope"));
    const auto* assistant = paired_assistant(envelope, "workflow-bad-write");
    CHECK(assistant != nullptr);
    if (assistant) {
        CHECK(assistant->at("tool_calls").as_array().front().at("arguments") == bad_arguments);
        const auto* observation = paired_tool_result(envelope, *assistant);
        CHECK(observation != nullptr);
        if (observation)
            CHECK(parse_json_strict(text(observation->at("content")), 1048576).at("reason").as_string() == "invalid_arguments");
    }
}

void workflow_pump_missing_read_corrects_within_one_pump() {
    WorkflowFixture f;
    write(f.directory.path / "alpha/present.txt", "workflow present bytes\n");
    const auto script = alpha_script({
        worker_step(1, "", {{"workflow-missing-read", "fs_read", Object{{"path", "absent.txt"}, {"max_bytes", 16}}}}),
        worker_step(2, "", {{"workflow-present-read", "fs_read", Object{{"path", "present.txt"}, {"max_bytes", 64}}}}),
    });
    const auto result = f.run(script);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 2);
    const auto feedback = f.store.tool_feedback(text(workers.front().at("attempt_id")));
    expect_correction_feedback(feedback.at("result"), "not_found", "tool_result");
    CHECK(!feedback.at("result").as_object().contains("content"));
    CHECK(workers.back().at("result").at("content") == "workflow present bytes\n");
    CHECK(number(f.store.budget(f.id).at("attempts")) == 4);
}

void workflow_pump_precondition_failure_corrects_within_one_pump() {
    WorkflowFixture f;
    write(f.directory.path / "alpha/exists.txt", "workflow original bytes\n");
    const auto script = alpha_script({
        worker_step(1, "", {{"workflow-create-existing", "fs_write", Object{{"path", "exists.txt"}, {"content", "must not overwrite\n"}}}}),
        worker_step(2, "", {{"workflow-cas-write", "fs_write", Object{{"path", "exists.txt"}, {"content", "workflow cas corrected\n"},
            {"expected_sha256", sha256_hex("workflow original bytes\n")}}}}),
    });
    const auto result = f.run(script);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 2);
    const auto& first = workers.front().at("result");
    CHECK(first.at("operation").at("status") == "FAILED");
    CHECK(first.at("execution_result").at("evidence").at("failure_kind").as_string() == "content_precondition");
    CHECK(first.at("correction_feedback").at("reason").as_string() == "content_precondition");
    CHECK(first.at("correction_feedback").at("correction_eligible") == true);
    CHECK(read_runtime_file(f.directory.path / "alpha/exists.txt") == "workflow cas corrected\n");
    CHECK(number(f.store.budget(f.id).at("attempts")) == 4);
    CHECK(!blocked(result, "operation_incomplete", "alpha"));
    CHECK(f.store.operation(text(first.at("operation").at("op_id"))).at("status") == "FAILED");
}

// The workflow-entry correction consumes the same explicit per-task Worker cap
// as an ordinary call; both the baseline stop and the exhausted chain keep one
// worker attempt, durable feedback and unacknowledged pending work.
void workflow_pump_correction_uses_the_same_worker_call_cap() {
    WorkflowFixture f;
    const auto script = alpha_script({
        worker_step(1, "workflow text-only before the cap", {}),
        worker_step(2, "", {{"workflow-capped-write", "fs_write", Object{{"path", "capped.txt"}, {"content", "never\n"}}}}),
    });
    const auto result = f.run(script, 1);
    const auto workers = worker_calls(result, "alpha");
    CHECK(workers.size() == 1);
    CHECK(!f.store.tool_feedback(text(workers.front().at("attempt_id"))).is_null());
    CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/capped.txt"));
    CHECK(!result.at("pending").as_array().empty());
}

void non_precondition_conflicts_and_missing_parents_stay_failed_without_correction() {
    const auto pump_once = [](Fixture& f, const std::vector<FakeModelStep>& steps) {
        EventPumpOptions options; options.max_worker_calls_per_task = 4; options.max_calls = 12;
        return f.runtime().pump(f.id, alpha_script(steps), options);
    };
    {
        // A missing parent directory is a deterministic pre-write failure that
        // is not a creation/CAS/patch-content precondition.
        Fixture f(structured_input());
        const auto result = pump_once(f, {
            worker_step(1, "", {{"orphan-write", "fs_write", Object{{"path", "missing-dir/out.txt"}, {"content", "x\n"}}}}),
            worker_step(2, "", {{"later-write", "fs_write", Object{{"path", "later.txt"}, {"content", "y\n"}}}}),
        });
        const auto workers = worker_calls(result, "alpha");
        CHECK(workers.size() == 1);
        const auto& failed = workers.front().at("result");
        CHECK(failed.at("operation").at("status") == "FAILED");
        CHECK(failed.at("execution_result").at("evidence").at("cause_code").as_string() == "invalid_input");
        CHECK(failed.at("execution_result").at("evidence").at("rename_attempted") == false);
        CHECK(!failed.as_object().contains("correction_feedback"));
        CHECK(blocked(result, "operation_incomplete", "alpha"));
        CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
        CHECK(!std::filesystem::exists(f.directory.path / "alpha/later.txt"));
    }
    {
        // A directory destination is an unauthorized ordinary file target, also
        // never eligible for automatic correction.
        Fixture f(structured_input());
        std::filesystem::create_directory(f.directory.path / "alpha/sub");
        const auto result = pump_once(f, {
            worker_step(1, "", {{"directory-write", "fs_write", Object{{"path", "sub"}, {"content", "x\n"}}}}),
            worker_step(2, "", {{"later-write", "fs_write", Object{{"path", "later.txt"}, {"content", "y\n"}}}}),
        });
        const auto workers = worker_calls(result, "alpha");
        CHECK(workers.size() == 1);
        const auto& failed = workers.front().at("result");
        CHECK(failed.at("operation").at("status") == "FAILED");
        CHECK(failed.at("execution_result").at("evidence").at("cause_code").as_string() == "unauthorized");
        CHECK(!failed.as_object().contains("correction_feedback"));
        CHECK(blocked(result, "operation_incomplete", "alpha"));
        CHECK(number(f.store.budget(f.id).at("attempts")) == 3);
        CHECK(!std::filesystem::exists(f.directory.path / "alpha/later.txt"));
    }
}
} // namespace

int main() {
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    base_sql = read_runtime_file(source / "db/schema.sql");
    store_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    workflow_sql = read_runtime_file(source / "db/migrations/007_workflow_runtime.sql");
    return test::run({
        {"strict grouped scripts reject ambiguity and workspace host aliasing", strict_script_groups_and_host_mapping},
        {"independent tasks persist real files and scoped original feedback", independent_real_files_and_original_feedback},
        {"partial AMEND and later keep retain a single current plan", partial_amend_and_late_keep_reuse_current_plan},
        {"waiting task leaves all model and tool opportunities to its sibling", waiting_task_releases_the_batch_for_its_sibling},
        {"empty groups and bounded calls retain exact pending work", missing_groups_and_caps_preserve_specific_inbox_items},
        {"partial workspace mapping gives no implicit note or sibling access", missing_workspace_never_falls_back_to_note_or_a_sibling},
        {"claim failure reserves nothing and tool error ends only its actor batch", claim_failure_and_tool_error_do_not_implicitly_retry},
        {"duplicate accepted diagnostic returns its exact old observation", duplicate_response_returns_original_diagnostic_after_file_and_epoch_change},
        {"settled attempt without feedback stays explicitly missing", settled_without_feedback_is_missing_and_cannot_reread},
        {"restart rebinds semantic orders without management calls and preserves original history bindings", reopen_recovers_orders_and_rebuilds_original_scoped_history},
        {"full history overflow blocks before creating a new reservation", context_overflow_blocks_before_another_attempt},
        {"frozen manager and attempt budgets preserve unprocessed work", frozen_manager_capacity_and_budget_keep_unprocessed_work},
        {"terminal actors retain visible unprocessed event history", terminal_tasks_remain_visible_as_unprocessed_history},
        {"legacy single-task runner cannot supersede a durable Worker wait", legacy_single_task_runner_cannot_supersede_a_durable_wait},
        {"text-only response gets one bounded correction within the same pump", text_only_response_gets_one_correction_within_the_same_pump},
        {"invalid tool arguments settle as feedback without an operation and correct", invalid_tool_arguments_settle_as_feedback_without_operation_and_correct},
        {"missing file read becomes an ordinary tool result and corrects", missing_file_read_becomes_ordinary_tool_result_and_corrects},
        {"settled creation precondition failure corrects with CAS", settled_creation_precondition_failure_corrects_with_cas},
        {"schema retry exhaustion keeps exact attempts and last feedback", schema_retry_exhaustion_keeps_exact_attempts_and_last_feedback},
        {"correction admission respects worker call, call and script gates", correction_admission_respects_worker_call_call_and_script_gates},
        {"tight request budget admits exactly one correction round", tight_request_budget_admits_exactly_one_correction_round},
        {"recapture overflow closes the chain before any reservation", recapture_overflow_closes_the_chain_before_any_reservation},
        {"multi-tool, undeclared and management failures stay stopped; text plus one tool executes once", multi_tool_undeclared_and_management_failures_stay_stopped},
        {"duplicate accept returns the identical original receipt", duplicate_accept_returns_the_identical_original_receipt},
        {"sibling node keeps its own history and budget during correction", sibling_node_keeps_its_own_history_and_budget_during_correction},
        {"feedback persistence failure stops the actor without correction", feedback_persistence_failure_stops_the_actor_without_correction},
        {"non-precondition conflicts and missing parents stay failed without correction", non_precondition_conflicts_and_missing_parents_stay_failed_without_correction},
        {"workflow pump text-only failure corrects within one pump", workflow_pump_text_only_failure_corrects_within_one_pump},
        {"workflow pump invalid arguments correct within one pump", workflow_pump_invalid_arguments_correct_within_one_pump},
        {"workflow pump missing read corrects within one pump", workflow_pump_missing_read_corrects_within_one_pump},
        {"workflow pump precondition failure corrects within one pump", workflow_pump_precondition_failure_corrects_within_one_pump},
        {"workflow pump correction uses the same worker call cap", workflow_pump_correction_uses_the_same_worker_call_cap}
    });
}
