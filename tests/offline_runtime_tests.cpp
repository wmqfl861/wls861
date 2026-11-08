#include "wls/offline_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"
#include "../src/amendment_disposition.hpp"

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sys/inotify.h>

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
std::string tools_sql;
std::string events_sql;

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
void expect_message(ErrorCode code, const std::string& message, const std::function<void()>& action) {
    bool caught = false;
    try { action(); }
    catch (const Error& error) {
        CHECK(error.code() == code);
        CHECK(error.what() == message);
        caught = true;
    }
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
    CHECK(resumed.at("calls").as_array().size() == 1);
    CHECK(resumed.at("calls").as_array().front().at("role") == "worker");
    CHECK(text(resumed.at("config_snapshot_sha256")) == hash);
    CHECK(store.task(task_id(store, id)).at("instruction_revision") == 1);
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

void fresh_disposition_is_rejected_before_any_model_reservation() {
    test::TempDir directory;
    Store store(directory.path / "request.db", base_sql, migration_sql);
    const auto id = admit(store);
    const auto before = store.request(id);
    const auto budget = store.budget(id);
    for (const auto* bad : {"update", "keep", "cancel", "invalid-enum"}) {
        expect(ErrorCode::InvalidInput, [&] { (void)OfflineRuntime(store, *schemas).run(id, initial_steps(), bad); });
        CHECK(store.request(id) == before);
        CHECK(store.budget(id) == budget);
        CHECK(store.tasks(id).empty());
        CHECK(store.budget(id).at("attempts") == 0);
    }
}

void inapplicable_disposition_is_rejected_before_published_plan_rebind() {
    test::TempDir directory;
    const auto db = directory.path / "request.db";
    std::string id;
    {
        Store store(db, base_sql, migration_sql);
        id = admit(store);
        static_cast<void>(OfflineRuntime(store, *schemas).run(id, initial_steps()));
        Json plan = store.plan(id).at("body");
        plan.as_object()["plan_revision"] = 2;
        plan.as_object()["summary"] = "Host-authorized plan published before the disposition gate";
        static_cast<void>(store.publish_plan(store.host_actor(Role::Dispatcher, id), number(store.request(id).at("state_version")), 1, plan, new_id("plan")));
        CHECK(store.task(task_id(store, id)).at("plan_revision") == 1);
    }
    Store store(db, base_sql, migration_sql);
    const auto task_before = store.task(task_id(store, id));
    const auto request_before = store.request(id);
    const auto budget_before = store.budget(id);
    const auto events_before = store.events(id, 0, 1000).size();
    for (const auto* bad : {"update", "keep", "cancel", "invalid-enum"}) {
        expect(ErrorCode::InvalidInput, [&] { (void)OfflineRuntime(store, *schemas).run(id, initial_steps(), bad); });
        CHECK(store.task(task_id(store, id)) == task_before);
        CHECK(store.request(id) == request_before);
        CHECK(store.budget(id) == budget_before);
        CHECK(store.events(id, 0, 1000).size() == events_before);
    }
    const auto resumed = OfflineRuntime(store, *schemas).run(id, initial_steps());
    CHECK(resumed.at("calls").as_array().size() == 2);
    CHECK(store.task(task_id(store, id)).at("plan_revision") == 2);
    CHECK(store.task(task_id(store, id)).at("applied_plan_revision") == 2);
}

void current_task_disposition_still_requires_empty_value() {
    test::TempDir directory;
    Store store(directory.path / "request.db", base_sql, migration_sql);
    const auto id = admit(store);
    static_cast<void>(OfflineRuntime(store, *schemas).run(id, initial_steps()));
    const auto task_before = store.task(task_id(store, id));
    const auto budget_before = store.budget(id);
    for (const auto* bad : {"keep", "invalid-enum"}) {
        expect(ErrorCode::InvalidInput, [&] { (void)OfflineRuntime(store, *schemas).run(id, initial_steps(), bad); });
        CHECK(store.task(task_id(store, id)) == task_before);
        CHECK(store.budget(id) == budget_before);
    }
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

void runtime_file_rejects_fifo_before_opening_it() {
    test::TempDir directory;
    const auto fifo = directory.path / "input-fifo";
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    const int watcher = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(watcher >= 0);
    CHECK(::inotify_add_watch(watcher, fifo.c_str(), IN_OPEN) >= 0);
    expect(ErrorCode::InvalidInput, [&] { (void)read_runtime_file(fifo); });
    std::array<char, 4096> events{};
    const auto received = ::read(watcher, events.data(), events.size());
    const auto saved_errno = errno;
    CHECK(::close(watcher) == 0);
    CHECK(received < 0 && saved_errno == EAGAIN);
}

void shared_accessors_keep_runtime_integer_error_boundaries() {
    // The shared header is the production accessor: check its exact original
    // runtime spellings for every rejected input shape, not only the code.
    CHECK(integer_at(Json(Object{{"based_on_requirements_revision", 1}}), "based_on_requirements_revision") == 1);
    CHECK(integer_at(Json(Object{{"requirements_revision", std::int64_t{9223372036854775807LL}}}),
                     "requirements_revision") == 9223372036854775807LL);
    CHECK(integer_at(Json(Object{{"plan_revision", std::uint64_t{9223372036854775807ULL}}}),
                     "plan_revision") == 9223372036854775807LL);
    expect_message(ErrorCode::InvalidInput, "integer field missing: requirements_revision",
        [&] { (void)integer_at(Json(Object{{"based_on_requirements_revision", 1}}), "requirements_revision"); });
    expect_message(ErrorCode::InvalidInput, "integer field missing: based_on_requirements_revision",
        [&] { (void)integer_at(Json(Array{}), "based_on_requirements_revision"); });
    for (const Json& bad : {Json("2"), Json(true), Json(1.5), Json(std::uint64_t{9223372036854775808ULL})}) {
        expect_message(ErrorCode::InvalidInput, "invalid integer field: requirements_revision",
            [&] { (void)integer_at(Json(Object{{"requirements_revision", bad}}), "requirements_revision"); });
    }
}

void shared_pending_amendment_predicate_short_circuits_and_validates() {
    const Json request(Object{{"requirements_revision", 2}});
    const Json pending_task(Object{{"based_on_requirements_revision", 1}});
    CHECK(has_pending_amendment({pending_task}, request) == true);
    CHECK(has_pending_amendment({Json(Object{{"based_on_requirements_revision", 2}})}, request) == false);
    // Empty captured tasks never read the request row, even without the field.
    CHECK(has_pending_amendment({}, Json(Object{})) == false);
    expect_message(ErrorCode::InvalidInput, "integer field missing: based_on_requirements_revision",
        [&] { (void)has_pending_amendment({Json(Object{})}, request); });
    expect_message(ErrorCode::InvalidInput, "integer field missing: based_on_requirements_revision",
        [&] { (void)has_pending_amendment({Json(7)}, request); });
    expect_message(ErrorCode::InvalidInput, "integer field missing: requirements_revision",
        [&] { (void)has_pending_amendment({pending_task}, Json(Object{})); });
    expect_message(ErrorCode::InvalidInput, "invalid integer field: requirements_revision",
        [&] { (void)has_pending_amendment({pending_task}, Json(Object{{"requirements_revision", "2"}})); });
}

void shared_pending_amendment_predicate_reads_real_store_rows() {
    test::TempDir directory;
    Store store(directory.path / "request.db", base_sql, migration_sql);
    const auto id = admit(store);
    CHECK(has_pending_amendment(store.tasks(id), store.request(id)) == false);
    static_cast<void>(OfflineRuntime(store, *schemas).run(id, initial_steps()));
    CHECK(has_pending_amendment(store.tasks(id), store.request(id)) == false);
    amend(store, id);
    CHECK(has_pending_amendment(store.tasks(id), store.request(id)) == true);
    static_cast<void>(OfflineRuntime(store, *schemas).run(id, amended_steps(), "cancel"));
    CHECK(has_pending_amendment(store.tasks(id), store.request(id)) == false);
}

void untenable_envelope_settles_and_keeps_reservation_without_retry() {
    test::TempDir directory;
    Store store(directory.path / "request.db", base_sql, migration_sql);
    const auto id = admit(store);
    OfflineRuntime runtime(store, *schemas);
    static_cast<void>(runtime.run(id, initial_steps()));
    const FakeModel fake(initial_steps());
    const auto call = runtime.reserve_call(fake, id, Role::Worker, task_id(store, id));
    ModelResponse empty;
    empty.call_id = call.request.call_id;
    empty.attempt_id = call.request.attempt_id;
    empty.role_call_index = call.request.role_call_index;
    empty.role = call.request.role;
    empty.binding = call.request.binding;
    empty.config_snapshot_id = call.request.config_snapshot_id;
    empty.status = ModelResultStatus::Completed;
    empty.usage = ModelUsage{100, 20, 0, 0, 0};
    // A Completed envelope with neither text nor a tool call cannot be legally
    // serialized, so no settlement is fabricated for it (FINAL v6 §3): the
    // InvalidInput keeps the original reservation held and schedules no retry.
    // The prepared variant compared to_json(empty), which the model layer
    // refuses to produce for this shape by design; see the execute-stage log.
    expect(ErrorCode::InvalidInput, [&] { (void)runtime.accept_response(call, empty); });
    CHECK(store.attempt(call.request.attempt_id).at("response").is_null());
    CHECK(store.attempt(call.request.attempt_id).at("settlement_sha256").is_null());
    CHECK(store.budget(id).at("attempts") == 4);
    CHECK(store.budget(id).at("spent_input") == 300);
    CHECK(number(store.budget(id).at("reserved_input")) > 0);
}

// G1-3 v8 C2 regressions on the OfflineRuntime::run correction host. The
// legacy no-workspace fixtures above stay byte-identical; this fixture
// actually enables the correction host (structured-v2 frozen configuration,
// Store 004/005/006 plus WorkerTools) following the established run-host
// pattern in worker_tools_tests.cpp. The same bytes are behavioral RED on the
// frozen v6 candidate (run() stops silently at exhaustion) and GREEN on the
// C2 repair; the compatibility runner's success output keeps its signature.
FrozenRequestConfig correction_config(const char* schema_retry = "2") {
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
    return load_config_catalog(configuration.path / "system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general");
}
FakeModelStep correction_worker_step(std::int64_t index, const std::string& body, std::vector<ModelToolCall> calls) {
    FakeModelStep step;
    step.role = Role::Worker; step.role_call_index = index; step.text = body;
    step.tool_calls = std::move(calls); step.usage = ModelUsage{100, 20, 0, 0, 0};
    return step;
}
std::vector<FakeModelStep> correction_steps(const std::vector<FakeModelStep>& worker_steps) {
    auto result = initial_steps();
    const auto prototype = result.back();
    result.resize(2);
    std::int64_t index = 0;
    for (const auto& source_step : worker_steps) {
        auto step = prototype;
        step.role_call_index = ++index;
        step.status = source_step.status;
        step.text = source_step.text;
        step.tool_calls = source_step.tool_calls;
        step.usage = source_step.usage;
        step.error_code = source_step.error_code;
        step.error_message = source_step.error_message;
        result.push_back(std::move(step));
    }
    return result;
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
struct CorrectionRunSession {
    test::TempDir directory;
    std::filesystem::path work;
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> tools;
    std::unique_ptr<OfflineRuntime> runtime;
    std::string request_id;
    explicit CorrectionRunSession(const char* schema_retry = "2")
        : work(directory.path / "work") {
        std::filesystem::create_directory(work);
        store = std::make_unique<Store>(directory.path / "run-host.db", base_sql, migration_sql,
            StoreOpenMode::Takeover, tools_sql, events_sql);
        request_id = text(store->admit(input(correction_config(schema_retry))).at("request_id"));
        tools = std::make_unique<WorkerTools>(*store, work);
        runtime = std::make_unique<OfflineRuntime>(*store, *schemas, tools.get());
    }
    std::string task_id() const { return text(store->tasks(request_id).front().at("task_id")); }
};

void run_host_reports_correction_limit_only_at_real_exhaustion() {
    {
        // cap two: initial plus two admitted failures, never a fourth call.
        CorrectionRunSession session("2");
        const auto result = session.runtime->run(session.request_id, correction_steps({
            correction_worker_step(1, "run-host text-only failure one", {}),
            correction_worker_step(2, "run-host text-only failure two", {}),
            correction_worker_step(3, "run-host text-only failure three", {}),
            correction_worker_step(4, "", {{"never-fourth", "fs_write", Object{{"path", "never.txt"}, {"content", "x\n"}}}}),
        }), "", 8);
        const auto& calls = result.at("calls").as_array();
        std::size_t workers = 0;
        for (const auto& call : calls) if (call.at("role").as_string() == "worker") ++workers;
        CHECK(workers == 3);
        // Exhaustion adds a bounded blocked entry plus the dedicated
        // stop_reason; both identify this run's original task.
        bool reported = false;
        for (const auto& entry : result.at("blocked").as_array())
            if (entry.at("code").as_string() == "correction_limit") {
                reported = true;
                CHECK(entry.at("task_id").as_string() == result.at("task_id").as_string());
            }
        CHECK(reported);
        CHECK(result.at("stop_reason").as_string() == "correction_limit");
        const auto& last = calls.at(4);
        expect_correction_feedback(session.store->tool_feedback(text(last.at("attempt_id"))).at("result"),
            "missing_tool_call", "model_schema");
        CHECK(!std::filesystem::exists(session.work / "never.txt"));
        CHECK(number(session.store->budget(session.request_id).at("attempts")) == 5);
    }
    {
        // cap zero: the first ordinary failure is classified and persisted,
        // then the chain ends without any second Worker call.
        CorrectionRunSession session("0");
        const auto result = session.runtime->run(session.request_id, correction_steps({
            correction_worker_step(1, "run-host text-only failure with cap zero", {}),
            correction_worker_step(2, "", {{"never-used", "fs_write", Object{{"path", "never.txt"}, {"content", "x\n"}}}}),
        }), "", 4);
        const auto& calls = result.at("calls").as_array();
        CHECK(calls.size() == 3);
        bool reported = false;
        for (const auto& entry : result.at("blocked").as_array())
            if (entry.at("code").as_string() == "correction_limit") reported = true;
        CHECK(reported);
        CHECK(result.at("stop_reason").as_string() == "correction_limit");
        expect_correction_feedback(session.store->tool_feedback(text(calls.at(2).at("attempt_id"))).at("result"),
            "missing_tool_call", "model_schema");
        CHECK(!std::filesystem::exists(session.work / "never.txt"));
        CHECK(number(session.store->budget(session.request_id).at("attempts")) == 3);
    }
    {
        // An ordinary success keeps the run host's original output shape: no
        // exhaustion happened, so no correction_limit may be reported.
        CorrectionRunSession session("2");
        const auto result = session.runtime->run(session.request_id, correction_steps({
            correction_worker_step(1, "text-only failure before a successful correction", {}),
            correction_worker_step(2, "", {{"fixed-write", "fs_write", Object{{"path", "fixed.txt"}, {"content", "run host fixed\n"}}}}),
        }), "", 2);
        CHECK(result.at("calls").as_array().size() == 4);
        if (const auto* blocked_entries = result.as_object().if_contains("blocked"))
            for (const auto& entry : blocked_entries->as_array())
                CHECK(entry.at("code").as_string() != "correction_limit");
        CHECK(result.at("request_status").as_string() == "RUNNING");
        CHECK(read_runtime_file(session.work / "fixed.txt") == "run host fixed\n");
        CHECK(number(session.store->budget(session.request_id).at("attempts")) == 4);
    }
}

// Admissions already span the whole run() invocation; a success between two
// ordinary failures must not reset that accumulated count, and a host call
// bound that stops the chain before any denial must not be reported as
// schema exhaustion.
void run_host_accumulation_and_host_caps_stay_exact() {
    {
        CorrectionRunSession session("1");
        const auto result = session.runtime->run(session.request_id, correction_steps({
            correction_worker_step(1, "text-only failure before a successful correction", {}),
            correction_worker_step(2, "", {{"fixed-write", "fs_write", Object{{"path", "fixed.txt"}, {"content", "fixed once\n"}}}}),
            correction_worker_step(3, "text-only failure after the admitted round", {}),
            correction_worker_step(4, "", {{"never-fourth", "fs_write", Object{{"path", "fourth.txt"}, {"content", "never\n"}}}}),
        }), "", 8);
        const auto& calls = result.at("calls").as_array();
        CHECK(calls.size() == 5);
        expect_correction_feedback(session.store->tool_feedback(text(calls.at(4).at("attempt_id"))).at("result"),
            "missing_tool_call", "model_schema");
        CHECK(read_runtime_file(session.work / "fixed.txt") == "fixed once\n");
        CHECK(!std::filesystem::exists(session.work / "fourth.txt"));
        CHECK(number(session.store->budget(session.request_id).at("attempts")) == 5);
    }
    {
        // The explicit max_worker_calls bound ends the loop before a third
        // failure is denied admission: the dedicated report must stay absent.
        CorrectionRunSession session("2");
        const auto result = session.runtime->run(session.request_id, correction_steps({
            correction_worker_step(1, "text-only failure under a tighter host cap", {}),
            correction_worker_step(2, "second text-only failure under a tighter host cap", {}),
            correction_worker_step(3, "", {{"unreached", "fs_write", Object{{"path", "unreached.txt"}, {"content", "x\n"}}}}),
        }), "", 2);
        CHECK(result.at("calls").as_array().size() == 4);
        if (const auto* blocked_entries = result.as_object().if_contains("blocked"))
            for (const auto& entry : blocked_entries->as_array())
                CHECK(entry.at("code").as_string() != "correction_limit");
        CHECK(!std::filesystem::exists(session.work / "unreached.txt"));
        CHECK(number(session.store->budget(session.request_id).at("attempts")) == 4);
    }
}
} // namespace

int main() {
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    base_sql = read_runtime_file(source / "db/schema.sql");
    migration_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    return test::run({
        {"offline chain persists exact model input, instructions and note evidence", complete_offline_chain_and_persisted_input},
        {"invalid plan constraints keep accounting but do not change the plan", invalid_plan_is_accounted_without_plan_mutation},
        {"forged host fields and undeclared Worker tools never apply", host_metadata_forgery_and_undeclared_tool_keep_usage},
        {"AMEND fences late output and keep requires fresh instruction and ack", late_response_after_amend_and_explicit_keep},
        {"interrupted usage holds reservation while cancellation retains capacity", interrupted_usage_stays_unknown_and_cancel_has_capacity},
        {"read-only access is inert and restart fences pending call before resume", restart_fences_pending_call_and_preserves_frozen_config},
        {"published plan resumes task binding after process restart", published_plan_resumes_before_any_new_model_call},
        {"explicit cancel disposition does not create a Worker model attempt", explicit_cancel_disposition_has_no_worker_attempt},
        {"fresh request rejects every disposition before any reservation", fresh_disposition_is_rejected_before_any_model_reservation},
        {"inapplicable disposition is rejected before the published plan rebind", inapplicable_disposition_is_rejected_before_published_plan_rebind},
        {"current task without AMEND still requires an empty disposition", current_task_disposition_still_requires_empty_value},
        {"bounded input reads reject parent/leaf symlinks and special files", runtime_file_reads_reject_symlink_and_special_input},
        {"runtime input rejects FIFO before any observable open", runtime_file_rejects_fifo_before_opening_it},
        {"shared accessors keep runtime integer error boundaries", shared_accessors_keep_runtime_integer_error_boundaries},
        {"shared pending-AMEND predicate short-circuits and validates", shared_pending_amendment_predicate_short_circuits_and_validates},
        {"shared pending-AMEND predicate reads real Store rows", shared_pending_amendment_predicate_reads_real_store_rows},
        {"untenable envelope settles and keeps its reservation without retry", untenable_envelope_settles_and_keeps_reservation_without_retry},
        {"run host reports correction_limit only at real exhaustion", run_host_reports_correction_limit_only_at_real_exhaustion},
        {"run host accumulation and host caps stay exact", run_host_accumulation_and_host_caps_stay_exact}
    });
}
