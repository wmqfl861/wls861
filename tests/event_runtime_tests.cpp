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
std::string base_sql, store_sql, tools_sql, events_sql;

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
} // namespace

int main() {
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    base_sql = read_runtime_file(source / "db/schema.sql");
    store_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
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
        {"legacy single-task runner cannot supersede a durable Worker wait", legacy_single_task_runner_cannot_supersede_a_durable_wait}
    });
}
