#include "wls/offline_runtime.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
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
std::string base_sql;
std::string migration_sql;

std::string text(const Json& value) { return std::string(value.as_string()); }
std::int64_t number(const Json& value) { return value.is_int64() ? value.as_int64() : static_cast<std::int64_t>(value.as_uint64()); }
Json fixture(const std::string& name) { return parse_json_strict(read_runtime_file(source / "tests/fixtures" / name)); }
std::vector<FakeModelStep> initial_steps() { return fake_model_steps_from_json(fixture("s1_fake_initial.json")); }
std::vector<FakeModelStep> amended_steps() { return fake_model_steps_from_json(fixture("s1_fake_amended.json")); }
FrozenRequestConfig config() {
    return load_config_catalog(source / "config/examples/system.yaml", source / "config/examples/functions", source / "prompts").freeze("general");
}
AdmissionInput input(const FrozenRequestConfig& frozen = config()) {
    AdmissionInput result;
    result.tenant_id = "test-tenant"; result.project_id = "test-project"; result.root_id = "test-root";
    result.function_id = frozen.function_id(); result.idempotency_key = new_id("key");
    result.original_text = read_runtime_file(source / "tests/fixtures/s1_request.txt");
    result.config_snapshot = frozen.json();
    result.constraint_catalog = fixture("s1_constraints.json");
    const auto grants = frozen.worker_tools();
    result.tool_allowlist.assign(grants.begin(), grants.end());
    return result;
}
std::string admit(Store& store) { return text(store.admit(input()).at("request_id")); }
std::string task_id(Store& store, const std::string& id) { return text(store.tasks(id).front().at("task_id")); }
void amend(Store& store, const std::string& id) {
    static_cast<void>(store.amend(store.host_actor(Role::Root, id), number(store.request(id).at("state_version")),
        read_runtime_file(source / "tests/fixtures/s1_amendment.txt"), fixture("s1_amended_constraints.json"), new_id("amend")));
}
void expect(ErrorCode code, const std::function<void()>& action) {
    bool caught = false;
    try { action(); } catch (const Error& error) { CHECK(error.code() == code); caught = true; }
    CHECK(caught);
}
void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary); stream << bytes; CHECK(static_cast<bool>(stream));
}

void complete_offline_chain_and_persisted_input() {
    test::TempDir directory;
    Store store(directory.path / "request.db", base_sql, migration_sql);
    const auto id = admit(store);
    const auto result = OfflineRuntime(store, *schemas).run(id, initial_steps());
    CHECK(result.at("calls").as_array().size() == 3);
    CHECK(result.at("business_delivered") == false);
    CHECK(store.request(id).at("status") == "RUNNING");
    CHECK(store.budget(id).at("attempts") == 3);
    CHECK(store.budget(id).at("spent_input") == 300);
    CHECK(store.budget(id).at("spent_output") == 60);
    CHECK(store.budget(id).at("reserved_input") == 0);
    CHECK(store.budget(id).at("unknown_cost") == 0);
    const auto& worker_call = result.at("calls").as_array().back();
    const auto attempt = store.attempt(text(worker_call.at("attempt_id")));
    const auto saved = model_request_from_json(attempt.at("request_envelope"));
    CHECK(saved.context.dynamic_suffix.find("保留原始需求") != std::string::npos);
    CHECK(saved.context.dynamic_suffix.find("issue_work_order") == std::string::npos);
    CHECK(saved.context.dynamic_suffix.find("只通过已声明的 note") != std::string::npos);
    CHECK(saved.context.stable_prefix.find("checks_executed") != std::string::npos);
    CHECK(saved.config_snapshot_id == text(result.at("config_snapshot_sha256")));
    CHECK(worker_call.at("result").at("status") == "SUCCEEDED");
    CHECK(!std::filesystem::exists(directory.path / "out"));
}

void invalid_plan_is_accounted_without_plan_mutation() {
    test::TempDir directory;
    Store store(directory.path / "request.db", base_sql, migration_sql);
    const auto id = admit(store);
    auto steps = initial_steps();
    auto& brief = steps.front().tool_calls.front().arguments.at("nodes").as_array().front().at("brief");
    brief.as_object()["constraint_refs"] = Array{};
    brief.as_object()["constraints_verbatim"] = Array{};
    const FakeModel fake(steps);
    OfflineRuntime runtime(store, *schemas);
    const auto call = runtime.reserve_call(fake, id, Role::Dispatcher);
    const auto version = store.request(id).at("state_version");
    const auto response = fake.complete(call.request);
    expect(ErrorCode::InvalidInput, [&] { (void)runtime.accept_response(call, response); });
    CHECK(store.request(id).at("plan_revision") == 0);
    CHECK(store.request(id).at("state_version") == version);
    CHECK(store.tasks(id).empty());
    CHECK(store.budget(id).at("spent_input") == 100);
    CHECK(store.attempt(call.request.attempt_id).at("response") == to_json(response));
    // This field records current-binding eligibility, not successful adoption.
    CHECK(store.attempt(call.request.attempt_id).at("accepted") == 1);
}

void host_metadata_forgery_and_undeclared_tool_keep_usage() {
    test::TempDir directory;
    Store store(directory.path / "request.db", base_sql, migration_sql);
    const auto id = admit(store);
    auto steps = initial_steps();
    steps.front().tool_calls.front().arguments.as_object()["plan_revision"] = 999;
    OfflineRuntime runtime(store, *schemas);
    const FakeModel fake(steps);
    const auto call = runtime.reserve_call(fake, id, Role::Dispatcher);
    expect(ErrorCode::InvalidInput, [&] { (void)runtime.accept_response(call, fake.complete(call.request)); });
    CHECK(store.request(id).at("plan_revision") == 0);
    CHECK(store.budget(id).at("attempts") == 1);
    CHECK(store.budget(id).at("spent_input") == 100);

    static_cast<void>(runtime.run(id, initial_steps()));
    steps = initial_steps(); steps.back().tool_calls.front().name = "run";
    const FakeModel forbidden(steps);
    const auto worker_call = runtime.reserve_call(forbidden, id, Role::Worker, task_id(store, id));
    const auto before = store.task(task_id(store, id)).at("state_version");
    expect(ErrorCode::InvalidInput, [&] { (void)runtime.accept_response(worker_call, forbidden.complete(worker_call.request)); });
    CHECK(store.task(task_id(store, id)).at("state_version") == before);
    CHECK(store.budget(id).at("spent_input") == 500);
}

void late_response_after_amend_and_explicit_keep() {
    test::TempDir directory;
    Store store(directory.path / "request.db", base_sql, migration_sql);
    const auto id = admit(store);
    OfflineRuntime runtime(store, *schemas);
    static_cast<void>(runtime.run(id, initial_steps()));
    const auto task = task_id(store, id);
    const FakeModel fake(initial_steps());
    const auto old = runtime.reserve_call(fake, id, Role::Worker, task);
    amend(store, id);
    const auto before = store.task(task).at("state_version");
    const auto late = fake.complete(old.request);
    expect(ErrorCode::Stale, [&] { (void)runtime.accept_response(old, late); });
    CHECK(store.task(task).at("state_version") == before);
    CHECK(store.attempt(old.request.attempt_id).at("status") == "STALE");
    CHECK(store.attempt(old.request.attempt_id).at("accepted") == 0);
    CHECK(store.budget(id).at("spent_input") == 400);
    expect(ErrorCode::InvalidInput, [&] { (void)runtime.run(id, amended_steps()); });
    CHECK(store.budget(id).at("attempts") == 4);
    const auto result = runtime.run(id, amended_steps(), "keep");
    CHECK(result.at("calls").as_array().size() == 3);
    const auto current = store.task(task);
    CHECK(current.at("task_revision") == 2);
    CHECK(current.at("based_on_requirements_revision") == 2);
    CHECK(current.at("plan_revision") == 2);
    CHECK(current.at("instruction_revision") == 2);
    CHECK(current.at("applied_instruction_revision") == 2);
    CHECK(current.at("applied_task_revision") == 2);
    CHECK(store.request_snapshot(id).at("texts").as_array().size() == 2);
}

void interrupted_usage_stays_unknown_and_cancel_has_capacity() {
    test::TempDir directory;
    Store store(directory.path / "request.db", base_sql, migration_sql);
    auto admission = input(); admission.budget.max_attempts = 4;
    const auto id = text(store.admit(admission).at("request_id"));
    OfflineRuntime runtime(store, *schemas);
    static_cast<void>(runtime.run(id, initial_steps()));
    auto steps = initial_steps();
    steps.back().status = ModelResultStatus::Interrupted;
    steps.back().error_code = "stream_closed";
    steps.back().usage = {};
    const FakeModel fake(steps);
    const auto call = runtime.reserve_call(fake, id, Role::Worker, task_id(store, id));
    expect(ErrorCode::NotReady, [&] { (void)runtime.accept_response(call, fake.complete(call.request)); });
    CHECK(store.attempt(call.request.attempt_id).at("input_tokens").is_null());
    CHECK(store.attempt(call.request.attempt_id).at("unknown_cost") == 1);
    CHECK(store.budget(id).at("unknown_input") == call.request.reserved_input_tokens);
    CHECK(store.budget(id).at("reserved_output") == call.request.reserved_output_tokens);
    const FakeModel complete(initial_steps());
    expect(ErrorCode::Quota, [&] { (void)runtime.reserve_call(complete, id, Role::Worker, task_id(store, id)); });
    static_cast<void>(store.cancel_request(store.host_actor(Role::Root, id), number(store.request(id).at("state_version")), new_id("cancel")));
    CHECK(store.request(id).at("status") == "CANCELLED");
}

void restart_fences_pending_call_and_preserves_frozen_config() {
    test::TempDir directory;
    const auto db = directory.path / "request.db";
    std::string id;
    std::string hash;
    std::optional<CapturedModelCall> pending;
    const FakeModel fake(initial_steps());
    {
        Store store(db, base_sql, migration_sql);
        id = admit(store);
        hash = text(store.request(id).at("config_snapshot_sha256"));
        OfflineRuntime runtime(store, *schemas);
        static_cast<void>(runtime.run(id, initial_steps()));
        pending = runtime.reserve_call(fake, id, Role::Worker, task_id(store, id));
    }
    {
        Store inspect(db, base_sql, migration_sql, StoreOpenMode::ReadOnly);
        CHECK(inspect.attempt(pending->request.attempt_id).at("status") == "PENDING");
        CHECK(inspect.request(id).at("dispatcher_epoch") == pending->request.binding.dispatcher_epoch);
    }
    Store store(db, base_sql, migration_sql);
    CHECK(store.attempt(pending->request.attempt_id).at("status") == "UNKNOWN");
    CHECK(number(store.request(id).at("dispatcher_epoch")) > pending->request.binding.dispatcher_epoch);
    CHECK(store.budget(id).at("unknown_input") == pending->request.reserved_input_tokens);
    OfflineRuntime runtime(store, *schemas);
    expect(ErrorCode::Stale, [&] { (void)runtime.accept_response(*pending, fake.complete(pending->request)); });
    CHECK(store.budget(id).at("unknown_input") == 0);
    const auto resumed = runtime.run(id, initial_steps());
    CHECK(resumed.at("calls").as_array().size() == 2);
    CHECK(text(resumed.at("config_snapshot_sha256")) == hash);
    CHECK(store.task(task_id(store, id)).at("instruction_revision") == 2);
}

void published_plan_resumes_before_any_new_model_call() {
    test::TempDir directory;
    const auto db = directory.path / "request.db";
    std::string id;
    {
        Store store(db, base_sql, migration_sql);
        id = admit(store);
        static_cast<void>(OfflineRuntime(store, *schemas).run(id, initial_steps()));
        Json plan = store.plan(id).at("body");
        plan.as_object()["plan_revision"] = 2;
        plan.as_object()["summary"] = "Host-authorized plan, published before restart";
        static_cast<void>(store.publish_plan(store.host_actor(Role::Dispatcher, id), number(store.request(id).at("state_version")), 1, plan, new_id("plan")));
        CHECK(store.task(task_id(store, id)).at("plan_revision") == 1);
    }
    Store store(db, base_sql, migration_sql);
    const auto result = OfflineRuntime(store, *schemas).run(id, initial_steps());
    CHECK(result.at("calls").as_array().size() == 2);
    CHECK(store.request(id).at("plan_revision") == 2);
    CHECK(store.task(task_id(store, id)).at("plan_revision") == 2);
    CHECK(store.task(task_id(store, id)).at("applied_plan_revision") == 2);
}

void explicit_cancel_disposition_has_no_worker_attempt() {
    test::TempDir directory;
    Store store(directory.path / "request.db", base_sql, migration_sql);
    const auto id = admit(store);
    OfflineRuntime runtime(store, *schemas);
    static_cast<void>(runtime.run(id, initial_steps()));
    amend(store, id);
    const auto result = runtime.run(id, amended_steps(), "cancel");
    CHECK(result.at("calls").as_array().size() == 1);
    CHECK(store.budget(id).at("attempts") == 4);
    CHECK(store.task(task_id(store, id)).at("status") == "CANCELLED");
    CHECK(result.at("business_delivered") == false);
}

void runtime_file_reads_reject_symlink_and_special_input() {
    test::TempDir directory;
    std::filesystem::create_directory(directory.path / "real");
    write(directory.path / "real/data.txt", "hello");
    write(directory.path / "outer.txt", "outer");
    std::filesystem::create_directory_symlink(directory.path / "real", directory.path / "link");
    CHECK(read_runtime_file(directory.path / "real/data.txt") == "hello");
    expect(ErrorCode::InvalidInput, [&] { (void)read_runtime_file(directory.path / "link/data.txt"); });
    expect(ErrorCode::InvalidInput, [&] { (void)read_runtime_file(directory.path / "link/../outer.txt"); });
    expect(ErrorCode::InvalidInput, [&] { (void)read_runtime_file(directory.path / "real/data.txt", 4); });
    expect(ErrorCode::InvalidInput, [&] { (void)read_runtime_file(directory.path / "real"); });
    std::filesystem::create_symlink(directory.path / "real/data.txt", directory.path / "leaf");
    expect(ErrorCode::InvalidInput, [&] { (void)read_runtime_file(directory.path / "leaf"); });
}
} // namespace

int main() {
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    base_sql = read_runtime_file(source / "db/schema.sql");
    migration_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    return test::run({
        {"offline chain persists exact model input, instructions and note evidence", complete_offline_chain_and_persisted_input},
        {"invalid plan constraints keep accounting but do not change the plan", invalid_plan_is_accounted_without_plan_mutation},
        {"forged host fields and undeclared Worker tools never apply", host_metadata_forgery_and_undeclared_tool_keep_usage},
        {"AMEND fences late output and keep requires fresh instruction and ack", late_response_after_amend_and_explicit_keep},
        {"interrupted usage holds reservation while cancellation retains capacity", interrupted_usage_stays_unknown_and_cancel_has_capacity},
        {"read-only access is inert and restart fences pending call before resume", restart_fences_pending_call_and_preserves_frozen_config},
        {"published plan resumes task binding after process restart", published_plan_resumes_before_any_new_model_call},
        {"explicit cancel disposition does not create a Worker model attempt", explicit_cancel_disposition_has_no_worker_attempt},
        {"bounded input reads reject parent/leaf symlinks and special files", runtime_file_reads_reject_symlink_and_special_input}
    });
}
