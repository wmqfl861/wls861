#include "wls/worker_tools.hpp"
#include "wls/offline_runtime.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <charconv>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace {
void worker_tool_test_phase(std::string_view key, const wls::s1::Json& prepared);
void worker_tool_test_before_prepare(std::string_view key);
}

// Compile the actual implementation in this TU to observe private
// preparation phases. The static linker uses these definitions instead of pulling the
// library's worker_tools.o; production builds have no observer at all.
#define WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE(key, prepared) ::worker_tool_test_phase(key, prepared)
#define WLS_WORKER_TOOLS_PRIVATE_TEST_BEFORE_PREPARE(key) ::worker_tool_test_before_prepare(key)
#include "../src/worker_tools.cpp"
#undef WLS_WORKER_TOOLS_PRIVATE_TEST_BEFORE_PREPARE
#undef WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE

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
std::unique_ptr<FrozenRequestConfig> frozen;
std::string base_sql;
std::string migration_sql;
std::string tools_sql;
Json original_script;
Json constraints;
constexpr std::string_view original_text = "请在自有临时 workspace 测试文件工具，保留原始需求；不要调用外部服务。";

std::function<void(std::string_view, const Json&)> prepared_phase_observer;
std::function<void(std::string_view)> before_prepare_observer;
void worker_tool_test_before_prepare(std::string_view key) {
    if (before_prepare_observer) before_prepare_observer(key);
}
void worker_tool_test_phase(std::string_view key, const Json& prepared) {
    if (prepared_phase_observer) prepared_phase_observer(key, prepared);
}

std::string text(const Json& value) { return std::string(value.as_string()); }
std::int64_t number(const Json& value) { return value.to_number<std::int64_t>(); }
void expect(ErrorCode expected, const std::function<void()>& action) {
    bool caught = false;
    try { action(); } catch (const Error& error) { CHECK(error.code() == expected); caught = true; }
    CHECK(caught);
}
void host_write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(static_cast<bool>(stream));
}
std::string host_read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void fixture_sql(const std::filesystem::path& database, const std::string& sql) {
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open(database.c_str(), &raw) == SQLITE_OK);
    const std::unique_ptr<sqlite3, int(*)(sqlite3*)> connection(raw, sqlite3_close);
    CHECK(sqlite3_busy_timeout(connection.get(), 1000) == SQLITE_OK);
    char* error = nullptr;
    const int status = sqlite3_exec(connection.get(), sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : "";
    sqlite3_free(error);
    if (status != SQLITE_OK) throw std::runtime_error("fixture SQL failed: " + message);
}
std::int64_t fixture_integer(const std::filesystem::path& database, const char* sql,
        const std::string& parameter) {
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open_v2(database.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    const std::unique_ptr<sqlite3, int(*)(sqlite3*)> connection(raw, sqlite3_close);
    CHECK(sqlite3_busy_timeout(connection.get(), 1000) == SQLITE_OK);
    sqlite3_stmt* statement_raw = nullptr;
    CHECK(sqlite3_prepare_v2(connection.get(), sql, -1, &statement_raw, nullptr) == SQLITE_OK);
    const std::unique_ptr<sqlite3_stmt, int(*)(sqlite3_stmt*)> statement(statement_raw, sqlite3_finalize);
    CHECK(sqlite3_bind_text(statement.get(), 1, parameter.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
    CHECK(sqlite3_step(statement.get()) == SQLITE_ROW);
    CHECK(sqlite3_column_type(statement.get(), 0) == SQLITE_INTEGER);
    const auto value = sqlite3_column_int64(statement.get(), 0);
    CHECK(sqlite3_step(statement.get()) == SQLITE_DONE);
    return value;
}
AdmissionInput admission() {
    AdmissionInput result;
    result.tenant_id = "worker-tool-test"; result.project_id = "temporary-fixtures"; result.root_id = "test-root";
    result.function_id = frozen->function_id(); result.idempotency_key = new_id("key");
    result.original_text = original_text; result.config_snapshot = frozen->json(); result.constraint_catalog = constraints;
    const auto grants = frozen->worker_tools();
    result.tool_allowlist.assign(grants.begin(), grants.end());
    return result;
}

std::vector<FakeModelStep> steps(std::vector<ModelToolCall> worker_calls = {{"note-1", "note", Object{{"text", "workspace test initialized"}}}}) {
    auto result = fake_model_steps_from_json(original_script);
    auto& plan = result[0].tool_calls.front().arguments;
    plan.as_object()["summary"] = "在开发夹具中执行有界文件工具，并保留完整需求与结果。";
    auto& node = plan.at("nodes").as_array().front();
    node.as_object()["node_id"] = "workspace-test";
    node.as_object()["title"] = "开发文件工具验证";
    auto& brief = node.at("brief").as_object();
    brief["goal"] = "通过已声明的文件工具验证持久化效果、权限和版本边界。";
    brief["acceptance_criteria"] = Array{Object{{"id", "ac1"}, {"text", "文件和执行证据一致；不声称业务交付完成。"}}};
    auto& order = result[1].tool_calls.front().arguments.as_object();
    order["approach"] = "确认当前说明后，仅使用已经声明的有界 Worker 工具。";
    order["steps"] = Array{Object{{"id", "s1"}, {"text", "读取原始需求，按明确脚本执行开发验证。"}}};
    order["done_definition"] = "记录实际文件结果；业务交付仍待后续验收。";
    const auto prototype = result[2];
    result.resize(2);
    std::int64_t index = 0;
    for (auto& call : worker_calls) {
        auto step = prototype;
        step.role_call_index = ++index;
        step.tool_calls = {std::move(call)};
        result.push_back(std::move(step));
    }
    return result;
}

struct Session {
    test::TempDir directory;
    std::filesystem::path database = directory.path / "runtime.db";
    std::filesystem::path work = directory.path / "work";
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> tools;
    std::unique_ptr<OfflineRuntime> runtime;
    std::string request_id;

    explicit Session(bool initialize = true, std::optional<AdmissionInput> input = std::nullopt) {
        std::filesystem::create_directory(work);
        store = std::make_unique<Store>(database, base_sql, migration_sql, StoreOpenMode::Takeover, tools_sql);
        request_id = text(store->admit(input ? *input : admission()).at("request_id"));
        tools = std::make_unique<WorkerTools>(*store, work);
        runtime = std::make_unique<OfflineRuntime>(*store, *schemas, tools.get());
        if (initialize) static_cast<void>(runtime->run(request_id, steps()));
    }
    std::string task_id() const { return text(store->tasks(request_id).front().at("task_id")); }
    ExecutionBinding binding() const { return store->binding(task_id()); }
    StoreActor actor() const { return store->host_actor(Role::Worker, request_id, binding().team_id); }
    std::int64_t version() const { return number(store->task(task_id()).at("state_version")); }
    Json execute(const std::string& name, const Json& args, const std::string& key = {}, const std::string& command = {}) {
        return tools->execute(actor(), binding(), version(), key.empty() ? new_id("key") : key,
            name, args, command.empty() ? new_id("cmd") : command);
    }
    Json diagnostic(const std::string& name, const Json& args) {
        return tools->diagnostic(actor(), binding(), name, args);
    }
    void amend() {
        static_cast<void>(store->amend(store->host_actor(Role::Root, request_id),
            number(store->request(request_id).at("state_version")),
            "变更后的工作必须依照新说明继续；保留原始需求；不要调用外部服务。", constraints, new_id("amend")));
    }
    void issue_new_instruction() {
        auto next_binding = binding(); ++next_binding.instruction_revision;
        Json order = steps()[1].tool_calls.front().arguments;
        const auto document = to_json(next_binding);
        for (const auto& field : document.as_object()) order.as_object()[field.key()] = field.value();
        static_cast<void>(store->issue_instruction(store->host_actor(Role::TeamLead, request_id, binding().team_id),
            binding(), version(), order, new_id("instruction")));
    }
};

const Json& operation(const Json& result) { return result.at("operation"); }
const Json& execution(const Json& result) { return result.at("execution_result"); }
std::string op_id(const Json& result) { return text(operation(result).at("op_id")); }
void succeeded(const Json& result) {
    CHECK(operation(result).at("status") == "SUCCEEDED");
    CHECK(execution(result).at("observed_outcome") == "SUCCEEDED");
    CHECK(execution(result).at("accepted") == 1);
}
std::set<std::string> declared(const std::vector<ContextTool>& tools) {
    std::set<std::string> names;
    for (const auto& tool : tools) names.insert(tool.name);
    return names;
}

std::vector<Json> feedback_frames(const CompiledContext& context) {
    std::vector<Json> result;
    std::string_view remaining = context.dynamic_suffix;
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        CHECK(newline != std::string_view::npos);
        const auto header = remaining.substr(0, newline);
        const auto separator = header.find(' ');
        CHECK(separator != std::string_view::npos);
        const auto name = header.substr(0, separator);
        const auto encoded_size = header.substr(separator + 1);
        std::size_t size{};
        const auto [end, error] = std::from_chars(encoded_size.data(), encoded_size.data() + encoded_size.size(), size);
        CHECK(error == std::errc{} && end == encoded_size.data() + encoded_size.size());
        remaining.remove_prefix(newline + 1);
        CHECK(size < remaining.size() && remaining[size] == '\n');
        if (name == "message") {
            const auto message = parse_json_strict(remaining.substr(0, size));
            CHECK(message.at("role") == "tool");
            result.push_back(parse_json_strict(text(message.at("content"))));
        } else CHECK(name == "task_contract");
        remaining.remove_prefix(size + 1);
    }
    return result;
}

void registry_and_request_permissions() {
    auto input = admission();
    std::erase(input.tool_allowlist, "fs_write");
    Session session(true, input);
    const auto names = declared(session.tools->declarations(session.actor()));
    CHECK(names.contains("note") && names.contains("fs_read") && !names.contains("fs_write"));
    CHECK(!names.contains("ask") && !names.contains("recall") && !names.contains("load_memory"));
    CHECK(worker_tool_definition("fs_read").category == ToolClass::Diagnostic);
    CHECK(worker_tool_definition("fs_write").category == ToolClass::Business);
    CHECK(worker_tool_definition("fs_write").side_effect == "workspace");
    CHECK(worker_tool_definition("op_cancel").category == ToolClass::Control);
    expect(ErrorCode::Unsupported, [] { (void)worker_tool_definition("arbitrary_shell"); });
    expect(ErrorCode::Unauthorized, [&] { (void)session.execute("fs_write", Object{{"path", "denied"}, {"content", "x"}}); });
    CHECK(!std::filesystem::exists(session.work / "denied"));
}

void durable_write_patch_and_business_replay() {
    Session session;
    const Json arguments = Object{{"path", "result.txt"}, {"content", "alpha beta"}};
    const auto write = session.execute("fs_write", arguments, "file-key", "file-command");
    succeeded(write);
    CHECK(write.at("executed_now") == true && write.at("replayed") == false);
    CHECK(host_read(session.work / "result.txt") == "alpha beta");
    CHECK(execution(write).at("evidence").at("durable") == true);
    const auto read = session.diagnostic("fs_read", Object{{"path", "result.txt"}, {"offset", 6}, {"max_bytes", 4}});
    CHECK(read.at("content") == "beta");
    CHECK(text(read.at("sha256")) == sha256_hex("alpha beta"));
    const auto patch = session.execute("fs_patch", Object{{"path", "result.txt"}, {"expected_sha256", sha256_hex("alpha beta")},
        {"offset", 6}, {"expected", "beta"}, {"replacement", "gamma"}});
    succeeded(patch);
    CHECK(host_read(session.work / "result.txt") == "alpha gamma");
    const auto before = session.store->task(session.task_id());
    const auto budget = session.store->budget(session.request_id);
    const auto replay = session.execute("fs_write", arguments, "file-key", "a-new-command-id");
    CHECK(replay.at("executed_now") == false && replay.at("replayed") == true);
    CHECK(op_id(replay) == op_id(write));
    CHECK(execution(replay) == session.store->operation_result(op_id(write)));
    CHECK(session.store->task(session.task_id()) == before);
    CHECK(session.store->budget(session.request_id) == budget);
    CHECK(host_read(session.work / "result.txt") == "alpha gamma");
    expect(ErrorCode::Conflict, [&] { (void)session.execute("fs_write", Object{{"path", "result.txt"}, {"content", "different"}}, "file-key"); });
    expect(ErrorCode::Conflict, [&] { (void)session.execute("note", Object{{"text", "different tool"}}, "file-key"); });
    CHECK(host_read(session.work / "result.txt") == "alpha gamma");
}

void late_model_response_after_amend_cannot_write() {
    Session session;
    const FakeModel fake(steps({{"late-write", "fs_write", Object{{"path", "late.txt"}, {"content", "obsolete"}}}}));
    const auto call = session.runtime->reserve_call(fake, session.request_id, Role::Worker, session.task_id());
    CHECK(call.request.context.dynamic_suffix.find(std::string(original_text)) != std::string::npos);
    session.amend();
    const auto state = session.store->task(session.task_id());
    const auto spent = number(session.store->budget(session.request_id).at("spent_input"));
    expect(ErrorCode::Stale, [&] { (void)session.runtime->accept_response(call, fake.complete(call.request)); });
    CHECK(!std::filesystem::exists(session.work / "late.txt"));
    CHECK(session.store->task(session.task_id()) == state);
    CHECK(session.store->attempt(call.request.attempt_id).at("accepted") == 0);
    CHECK(session.store->attempt(call.request.attempt_id).at("status") == "STALE");
    CHECK(number(session.store->budget(session.request_id).at("spent_input")) == spent + 100);
    const auto resumed = session.runtime->run(session.request_id, steps(), "keep");
    CHECK(resumed.at("calls").as_array().size() == 3);
    CHECK(session.binding().requirements_revision == 2);
    CHECK(session.store->task(session.task_id()).at("applied_instruction_revision") == session.binding().instruction_revision);
    CHECK(!std::filesystem::exists(session.work / "late.txt"));
}

void old_key_is_only_a_read_during_new_instruction() {
    Session session;
    const Json arguments = Object{{"path", "old.txt"}, {"content", "initial"}};
    const auto first = session.execute("fs_write", arguments, "old-key", "original-command");
    succeeded(first);
    session.issue_new_instruction();
    const auto before = session.store->task(session.task_id());
    host_write(session.work / "old.txt", "new host observation");
    const auto replay = session.execute("fs_write", arguments, "old-key", "later-command");
    CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
    CHECK(op_id(replay) == op_id(first));
    CHECK(number(operation(replay).at("instruction_revision")) < session.binding().instruction_revision);
    CHECK(session.store->task(session.task_id()) == before);
    CHECK(host_read(session.work / "old.txt") == "new host observation");
    expect(ErrorCode::NotReady, [&] { (void)session.execute("fs_write", Object{{"path", "premature"}, {"content", "x"}}); });
    static_cast<void>(session.execute("ack_change", Object{}));
    succeeded(session.execute("fs_write", Object{{"path", "current.txt"}, {"content", "new instruction"}}));
}

void workspace_and_execution_manifest_are_immutable() {
    Session session;
    const auto stored = session.store->workspace(session.binding().team_id);
    WorkerTools same(*session.store, session.work);
    same.bind(session.actor());
    CHECK(session.store->workspace(session.binding().team_id) == stored);
    WorkerTools changed(*session.store, session.work, Object{{"schema_version", 1}, {"cpu_quota_us", 50000}});
    expect(ErrorCode::Conflict, [&] { changed.bind(session.actor()); });
    std::filesystem::create_directory(session.directory.path / "other");
    WorkerTools other(*session.store, session.directory.path / "other");
    expect(ErrorCode::Conflict, [&] { other.bind(session.actor()); });
    CHECK(session.store->workspace(session.binding().team_id) == stored);
    std::filesystem::rename(session.work, session.directory.path / "moved-work");
    std::filesystem::create_directory(session.work);
    test::throws<Error>([&] { (void)session.execute("fs_write", Object{{"path", "new"}, {"content", "x"}}); });
    WorkerTools replaced(*session.store, session.work);
    expect(ErrorCode::Conflict, [&] { replaced.bind(session.actor()); });
    CHECK(!std::filesystem::exists(session.work / "new"));
    CHECK(!std::filesystem::exists(session.directory.path / "moved-work/new"));
}

void role_scope_and_model_authority_forgery() {
    Session session;
    const auto leader = session.store->host_actor(Role::TeamLead, session.request_id, session.binding().team_id);
    expect(ErrorCode::Unauthorized, [&] { (void)session.tools->execute(leader, session.binding(), session.version(),
        "leader-key", "fs_write", Object{{"path", "leader.txt"}, {"content", "x"}}, "leader-command"); });
    for (const auto* field : {"side_effect", "category", "role", "workspace", "policy_epoch", "execution_epoch", "request_id"}) {
        Json args = Object{{"path", "forged.txt"}, {"content", "x"}};
        args.as_object()[field] = "forged";
        expect(ErrorCode::InvalidInput, [&] { (void)session.execute("fs_write", args); });
    }
    auto script = steps({{"model-forgery", "fs_write", Object{{"path", "forged.txt"}, {"content", "x"}, {"side_effect", "none"}}}});
    const FakeModel fake(script);
    const auto call = session.runtime->reserve_call(fake, session.request_id, Role::Worker, session.task_id());
    const auto task_before = session.store->task(session.task_id());
    expect(ErrorCode::InvalidInput, [&] { (void)session.runtime->accept_response(call, fake.complete(call.request)); });
    CHECK(session.store->task(session.task_id()) == task_before);
    CHECK(session.store->attempt(call.request.attempt_id).at("input_tokens") == 100);
    CHECK(!std::filesystem::exists(session.work / "forged.txt") && !std::filesystem::exists(session.work / "leader.txt"));

    const auto other_id = text(session.store->admit(admission()).at("request_id"));
    static_cast<void>(OfflineRuntime(*session.store, *schemas).run(other_id, steps()));
    const auto other_task = text(session.store->tasks(other_id).front().at("task_id"));
    const auto other_binding = session.store->binding(other_task);
    const auto other_actor = session.store->host_actor(Role::Worker, other_id, other_binding.team_id);
    std::filesystem::create_directory(session.directory.path / "second-work");
    WorkerTools second(*session.store, session.directory.path / "second-work");
    second.bind(other_actor);
    expect(ErrorCode::Conflict, [&] { (void)session.tools->diagnostic(other_actor, other_binding, "fs_list", Object{}); });
}

void unavailable_strong_run_is_a_durable_refusal() {
    Session session;
    const Json args = Object{{"argv", Array{"/bin/sh", "-c", "printf should-not-run > forbidden"}}};
    const auto refused = session.execute("run", args, "run-key", "run-command");
    CHECK(operation(refused).at("status") == "FAILED");
    CHECK(execution(refused).at("observed_outcome") == "FAILED");
    CHECK(execution(refused).at("result").at("spawned") == false);
    CHECK(execution(refused).at("result").at("strong_isolation") == false);
    CHECK(!std::filesystem::exists(session.work / "forbidden"));
    const auto replay = session.execute("run", args, "run-key", "run-retry");
    CHECK(replay.at("executed_now") == false && op_id(replay) == op_id(refused));
    CHECK(!std::filesystem::exists(session.work / "forbidden"));
    const auto status = session.diagnostic("op_status", Object{{"op_id", op_id(refused)}});
    CHECK(status.at("operation").at("status") == "FAILED");
    const auto output = session.diagnostic("op_output", Object{{"op_id", op_id(refused)}, {"stream", "stdout"}});
    CHECK(output.at("total_bytes") == 0);
}

void cancellation_before_effect_preserves_original_no_spawn_evidence() {
    Session session;
    std::stop_source cancellation;
    cancellation.request_stop();
    const Json args = Object{{"argv", Array{"/bin/sh", "-c", "printf should-not-run > forbidden"}}};
    const auto cancelled = session.tools->execute(session.actor(), session.binding(), session.version(),
        "cancelled-run", "run", args, "cancelled-run-command", cancellation.get_token());
    CHECK(operation(cancelled).at("status") == "CANCELLED");
    CHECK(execution(cancelled).at("observed_outcome") == "CANCELLED");
    CHECK(execution(cancelled).at("result").at("status") == "CANCELLED");
    CHECK(execution(cancelled).at("result").at("spawned") == false);
    CHECK(execution(cancelled).at("result").at("all_descendants_exited") == true);
    CHECK(execution(cancelled).at("result").at("strong_isolation") == false);
    CHECK(execution(cancelled).at("stdout_bytes") == 0 && execution(cancelled).at("stderr_bytes") == 0);
    CHECK(!std::filesystem::exists(session.work / "forbidden"));
    // Removing the stop request cannot turn an exact retry into another send.
    const auto replay = session.execute("run", args, "cancelled-run", "cancelled-run-replay");
    CHECK(replay.at("executed_now") == false && replay.at("replayed") == true);
    CHECK(execution(replay) == execution(cancelled));

    const auto file = session.tools->execute(session.actor(), session.binding(), session.version(),
        "cancelled-write", "fs_write", Object{{"path", "cancelled-file"}, {"content", "unused"}},
        "cancelled-file-command", cancellation.get_token());
    CHECK(operation(file).at("status") == "CANCELLED");
    CHECK(execution(file).at("observed_outcome") == "CANCELLED");
    CHECK(execution(file).at("evidence").at("effect_started") == false);
    CHECK(!std::filesystem::exists(session.work / "cancelled-file"));
}

void readonly_diagnostics_preserve_epochs_and_budgets() {
    Session session;
    const auto write = session.execute("fs_write", Object{{"path", "read.txt"}, {"content", "readable"}});
    succeeded(write);
    const auto before_request = session.store->request(session.request_id);
    const auto before_task = session.store->task(session.task_id());
    const auto before_budget = session.store->budget(session.request_id);
    const auto before_events = session.store->events(session.request_id);
    Store inspect(session.database, base_sql, migration_sql, StoreOpenMode::ReadOnly, tools_sql);
    WorkerTools reader(inspect, session.work, Object{}, true);
    const auto current = inspect.binding(session.task_id());
    const auto actor = inspect.host_actor(Role::Worker, session.request_id, current.team_id);
    reader.bind(actor);
    CHECK(reader.diagnostic(actor, current, "fs_read", Object{{"path", "read.txt"}}).at("content") == "readable");
    CHECK(reader.diagnostic(actor, current, "fs_list", Object{}).at("entries").as_array().size() == 1);
    CHECK(reader.diagnostic(actor, current, "fs_search", Object{{"path", "."}, {"literal", "read"}}).at("matches").as_array().size() == 1);
    CHECK(reader.diagnostic(actor, current, "op_status", Object{{"op_id", op_id(write)}}).at("operation").at("status") == "SUCCEEDED");
    expect(ErrorCode::Unauthorized, [&] { (void)reader.execute(actor, current, session.version(), "read-only-key", "fs_write",
        Object{{"path", "not-written"}, {"content", "x"}}, "read-only-command"); });
    CHECK(session.store->request(session.request_id) == before_request);
    CHECK(session.store->task(session.task_id()) == before_task);
    CHECK(session.store->budget(session.request_id) == before_budget);
    CHECK(session.store->events(session.request_id) == before_events);
    CHECK(!std::filesystem::exists(session.work / "not-written"));
}

void unknown_allows_diagnostics_and_instruction_control() {
    Session session;
    host_write(session.work / "evidence.txt", "observable");
    const auto captured = session.binding();
    const auto actor = session.actor();
    const auto prepared = session.store->prepare_tool_operation(actor, captured, session.version(), "unknown-key", "fs_write",
        Object{{"path", "uncertain"}, {"content", "unknown"}}, "unknown-prepare");
    const auto id = text(prepared.at("op_id"));
    const auto claim = session.store->start_tool_operation(actor, id, number(prepared.at("state_version")), "unknown-start");
    CHECK(claim.at("dispatch_claimed") == true);
    // The synthetic RUNNING fact has no matching file effect; cancellation must
    // still preserve UNKNOWN until a trusted host reconciles it.
    static_cast<void>(session.execute("op_cancel", Object{{"op_id", id}}));
    CHECK(session.store->operation(id).at("status") == "UNKNOWN");
    expect(ErrorCode::UnknownOutcome, [&] { (void)session.execute("fs_write", Object{{"path", "blocked"}, {"content", "x"}}); });
    CHECK(session.diagnostic("fs_read", Object{{"path", "evidence.txt"}}).at("content") == "observable");
    CHECK(session.diagnostic("op_status", Object{{"op_id", id}}).at("operation").at("status") == "UNKNOWN");
    session.issue_new_instruction();
    static_cast<void>(session.execute("ack_change", Object{}));
    CHECK(session.store->task(session.task_id()).at("applied_instruction_revision") == session.binding().instruction_revision);
    expect(ErrorCode::UnknownOutcome, [&] { (void)session.execute("fs_write", Object{{"path", "still-blocked"}, {"content", "x"}}); });
    static_cast<void>(session.store->reconcile_operation(id, number(session.store->operation(id).at("state_version")),
        "FAILED", "synthetic fixture: no file effect was dispatched", "trusted-reconcile"));
    succeeded(session.execute("fs_write", Object{{"path", "after-reconcile"}, {"content", "allowed"}}));
    CHECK(!std::filesystem::exists(session.work / "blocked") && !std::filesystem::exists(session.work / "still-blocked"));
}

void model_loop_carries_complete_tool_results() {
    Session session(false);
    const Json read_arguments = Object{{"path", "flow.txt"}, {"offset", 3}, {"max_bytes", 7}};
    const auto script = steps({
        {"write-1", "fs_write", Object{{"path", "flow.txt"}, {"content", "context-marker alpha"}}},
        {"read-2", "fs_read", read_arguments},
        {"patch-3", "fs_patch", Object{{"path", "flow.txt"}, {"expected_sha256", sha256_hex("context-marker alpha")}, {"offset", 15}, {"expected", "alpha"}, {"replacement", "beta"}}},
        {"note-4", "note", Object{{"text", "flow facts retained without business delivery"}}}
    });
    const auto result = session.runtime->run(session.request_id, script, "", 4);
    CHECK(result.at("calls").as_array().size() == 6);
    CHECK(result.at("business_delivered") == false && result.at("real_model_calls") == 0);
    CHECK(host_read(session.work / "flow.txt") == "context-marker beta");
    const auto& calls = result.at("calls").as_array();
    const auto third_attempt = session.store->attempt(text(calls[4].at("attempt_id")));
    const auto envelope = model_request_from_json(third_attempt.at("request_envelope"));
    CHECK(envelope.role_call_index == 3);
    CHECK(envelope.context.dynamic_suffix.find(std::string(original_text)) != std::string::npos);
    CHECK(envelope.context.dynamic_suffix.find("context-marker alpha") != std::string::npos);
    CHECK(envelope.context.dynamic_suffix.find(sha256_hex("context-marker alpha")) != std::string::npos);
    CHECK(envelope.context.dynamic_suffix.find("execution_result") != std::string::npos);
    CHECK(envelope.context.dynamic_suffix.find("content_base64") != std::string::npos);
    const auto feedback = feedback_frames(envelope.context);
    CHECK(feedback.size() == 2);
    CHECK(feedback[0].at("role_call_index") == 1 && feedback[1].at("role_call_index") == 2);
    for (std::size_t index = 0; index < feedback.size(); ++index) {
        const auto& preceding = calls[index + 2];
        const auto attempt = session.store->attempt(text(preceding.at("attempt_id")));
        CHECK(feedback[index].at("attempt_id") == preceding.at("attempt_id"));
        CHECK(feedback[index].at("response") == attempt.at("response"));
        CHECK(feedback[index].at("response") == preceding.at("response"));
        CHECK(feedback[index].at("result") == preceding.at("result"));
    }
    const auto& read_call = feedback[1].at("response").at("tool_calls").as_array().front();
    CHECK(read_call.at("name") == "fs_read" && read_call.at("call_id") == "read-2");
    CHECK(read_call.at("arguments") == read_arguments);
    CHECK(read_call.at("arguments").at("max_bytes") == 7);
    CHECK(envelope.context.stable_prefix.find("context-marker alpha") == std::string::npos);
    CHECK(calls[2].at("prefix_sha256") == calls[3].at("prefix_sha256"));
    CHECK(calls[3].at("prefix_sha256") == calls[4].at("prefix_sha256"));
    CHECK(session.store->budget(session.request_id).at("attempts") == 6);
    CHECK(session.store->request(session.request_id).at("status") == "RUNNING");
}

void batch_limits_and_failed_tool_stop_further_calls() {
    Session session(false);
    const auto script = steps({
        {"first", "fs_write", Object{{"path", "first.txt"}, {"content", "first"}}},
        {"second", "fs_write", Object{{"path", "second.txt"}, {"content", "second"}}},
        {"third", "fs_write", Object{{"path", "third.txt"}, {"content", "third"}}}
    });
    expect(ErrorCode::InvalidInput, [&] { (void)session.runtime->run(session.request_id, script, "", 0); });
    expect(ErrorCode::InvalidInput, [&] { (void)session.runtime->run(session.request_id, script, "", 17); });
    CHECK(session.store->budget(session.request_id).at("attempts") == 0);
    const auto limited = session.runtime->run(session.request_id, script, "", 2);
    CHECK(limited.at("calls").as_array().size() == 4);
    CHECK(std::filesystem::exists(session.work / "first.txt") && std::filesystem::exists(session.work / "second.txt"));
    CHECK(!std::filesystem::exists(session.work / "third.txt"));
    const auto before = number(session.store->budget(session.request_id).at("attempts"));
    const auto failure = session.runtime->run(session.request_id, steps({
        {"conflicting-create", "fs_write", Object{{"path", "first.txt"}, {"content", "must not overwrite"}}},
        {"must-not-run", "fs_write", Object{{"path", "after-failure"}, {"content", "x"}}}
    }), "", 2);
    CHECK(failure.at("calls").as_array().size() == 1);
    CHECK(failure.at("calls").as_array().front().at("role") == "worker");
    CHECK(operation(failure.at("calls").as_array().back().at("result")).at("status") == "FAILED");
    CHECK(number(session.store->budget(session.request_id).at("attempts")) == before + 1);
    CHECK(host_read(session.work / "first.txt") == "first");
    CHECK(!std::filesystem::exists(session.work / "after-failure"));
}

void path_payload_and_schema_limits_precede_effects() {
    Session session;
    const auto before = session.store->task(session.task_id());
    expect(ErrorCode::InvalidInput, [&] { (void)session.execute("fs_write", Object{{"path", "oversize"}, {"content", std::string(65537, 'x')}}); });
    expect(ErrorCode::InvalidInput, [&] { (void)session.execute("fs_write", Object{{"path", "null-hash"}, {"content", "x"}, {"expected_sha256", nullptr}}); });
    expect(ErrorCode::InvalidInput, [&] { (void)session.execute("fs_patch", Object{{"path", "patch"}, {"expected_sha256", sha256_hex("")},
        {"offset", -1}, {"expected", "x"}, {"replacement", "z"}}); });
    CHECK(session.store->task(session.task_id()) == before);
    const auto traversal = session.execute("fs_write", Object{{"path", "../escaped"}, {"content", "x"}});
    CHECK(operation(traversal).at("status") == "FAILED");
    CHECK(execution(traversal).at("evidence").at("rename_attempted") == false);
    CHECK(!std::filesystem::exists(session.directory.path / "escaped"));
    CHECK(!std::filesystem::exists(session.work / "oversize") && !std::filesystem::exists(session.work / "null-hash"));
}

void concurrent_exact_commands_have_one_execution_owner() {
    Session session;
    WorkerTools second(*session.store, session.work);
    second.bind(session.actor());
    for (int round = 0; round < 6; ++round) {
        const auto actor = session.actor(); const auto binding = session.binding(); const auto version = session.version();
        const auto key = "concurrent-key-" + std::to_string(round);
        const auto command = "concurrent-command-" + std::to_string(round);
        const auto filename = "concurrent-" + std::to_string(round);
        const Json args = Object{{"path", filename}, {"content", "one dispatch"}};
        std::array<Json, 2> results;
        std::array<std::exception_ptr, 2> errors;
        std::barrier start(2);
        const auto invoke = [&](std::size_t index, WorkerTools& executor) {
            start.arrive_and_wait();
            try { results[index] = executor.execute(actor, binding, version, key, "fs_write", args, command); }
            catch (...) { errors[index] = std::current_exception(); }
        };
        std::jthread first([&] { invoke(0, *session.tools); });
        std::jthread other([&] { invoke(1, round % 2 == 0 ? *session.tools : second); });
        first.join(); other.join();
        for (const auto& error : errors) if (error) std::rethrow_exception(error);
        const int executed = (results[0].at("executed_now").as_bool() ? 1 : 0) + (results[1].at("executed_now").as_bool() ? 1 : 0);
        CHECK(executed == 1);
        CHECK(op_id(results[0]) == op_id(results[1]));
        CHECK(session.store->operation(op_id(results[0])).at("status") == "SUCCEEDED");
        CHECK(host_read(session.work / filename) == "one dispatch");
    }
}

void delayed_prepare_replay_keeps_the_original_dispatch_intent() {
    Session session;
    WorkerTools second(*session.store, session.work);
    second.bind(session.actor());
    const auto actor = session.actor();
    const auto binding = session.binding();
    const auto expected = session.version();
    const std::string key = "delayed-prepare-replay-key";
    const std::string command = "delayed-prepare-replay-command";
    const auto file = session.work / "one-original-effect.txt";
    const Json args = Object{{"path", file.filename().string()}, {"content", "one original dispatch"}};
    const auto tool_calls = [&] {
        return fixture_integer(session.database, "SELECT tool_calls_used FROM teams WHERE team_id=?", actor.team_id);
    };
    const auto operations = [&] {
        return fixture_integer(session.database, "SELECT count(*) FROM ops WHERE request_id=?", session.request_id);
    };
    const auto results = [&] {
        return fixture_integer(session.database,
            "SELECT count(*) FROM operation_results r JOIN ops o USING(op_id) WHERE o.request_id=?", session.request_id);
    };
    const auto attempts = [&] {
        return fixture_integer(session.database, "SELECT count(*) FROM call_attempts WHERE request_id=?", session.request_id);
    };
    CHECK(!std::filesystem::exists(file));
    CHECK(session.store->operation_for_key(actor, binding, key, "fs_write", args).is_null());
    const auto before_tool_calls = tool_calls();
    const auto before_operations = operations();
    const auto before_results = results();
    const auto before_attempts = attempts();
    const auto before_budget = session.store->budget(session.request_id);
    const auto before_events = session.store->events(session.request_id, 0, 1000);
    CHECK(before_events.size() < 997);

    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        unsigned arrivals{0};
        bool released{false};
        void release() {
            const std::lock_guard lock(mutex);
            released = true;
            changed.notify_all();
        }
    } gate;
    CHECK(!before_prepare_observer);
    before_prepare_observer = [&](std::string_view observed_key) {
        if (observed_key != key) return;
        std::unique_lock lock(gate.mutex);
        ++gate.arrivals;
        gate.changed.notify_all();
        // Both calls have observed the key as absent. Only the delayed caller
        // waits here, so the owner can finish and remove its in-memory claim.
        if (gate.arrivals == 1 &&
            !gate.changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate.released; }))
            throw std::runtime_error("delayed prepare test gate was not released");
    };
    struct ResetObserver { ~ResetObserver() { before_prepare_observer = {}; } } reset;
    Json replay;
    std::exception_ptr delayed_error;
    std::jthread delayed([&] {
        try { replay = second.execute(actor, binding, expected, key, "fs_write", args, command); }
        catch (...) { delayed_error = std::current_exception(); }
    });
    struct ReleaseGate { Gate& gate; ~ReleaseGate() { gate.release(); } } release{gate};
    {
        std::unique_lock lock(gate.mutex);
        CHECK(gate.changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate.arrivals == 1; }));
    }
    const auto owner = session.tools->execute(actor, binding, expected, key, "fs_write", args, command);
    succeeded(owner);
    CHECK(owner.at("executed_now") == true && owner.at("replayed") == false);
    CHECK(host_read(file) == "one original dispatch");
    const auto id = op_id(owner);
    const auto original_operation = session.store->operation(id);
    const auto original_result = session.store->operation_result(id);
    const auto settled_task = session.store->task(session.task_id());
    const auto settled_request = session.store->request(session.request_id);
    const auto settled_events = session.store->events(session.request_id, 0, 1000);
    CHECK(number(original_operation.at("state_version")) == 3);
    CHECK(number(settled_task.at("state_version")) == expected + 1);
    CHECK(tool_calls() == before_tool_calls + 1 && operations() == before_operations + 1);
    CHECK(results() == before_results + 1 && attempts() == before_attempts);
    CHECK(session.store->budget(session.request_id) == before_budget);
    CHECK(settled_events.size() == before_events.size() + 3);
    CHECK(std::equal(before_events.begin(), before_events.end(), settled_events.begin()));
    CHECK(settled_events[before_events.size()].at("type") == "prepare_op");
    CHECK(settled_events[before_events.size() + 1].at("type") == "start_tool_op");
    CHECK(settled_events[before_events.size() + 2].at("type") == "record_op_result");
    struct stat original_file {};
    CHECK(::lstat(file.c_str(), &original_file) == 0);

    gate.release();
    delayed.join();
    CHECK(gate.arrivals == 2);
    CHECK(session.store->operation(id) == original_operation);
    CHECK(session.store->operation_result(id) == original_result);
    CHECK(session.store->task(session.task_id()) == settled_task);
    CHECK(session.store->request(session.request_id) == settled_request);
    CHECK(session.store->events(session.request_id, 0, 1000) == settled_events);
    CHECK(session.store->budget(session.request_id) == before_budget);
    CHECK(tool_calls() == before_tool_calls + 1 && operations() == before_operations + 1);
    CHECK(results() == before_results + 1 && attempts() == before_attempts);
    struct stat replayed_file {};
    CHECK(::lstat(file.c_str(), &replayed_file) == 0);
    CHECK(replayed_file.st_dev == original_file.st_dev && replayed_file.st_ino == original_file.st_ino);
    CHECK(replayed_file.st_size == original_file.st_size);
    CHECK(replayed_file.st_mtim.tv_sec == original_file.st_mtim.tv_sec &&
        replayed_file.st_mtim.tv_nsec == original_file.st_mtim.tv_nsec);
    CHECK(replayed_file.st_ctim.tv_sec == original_file.st_ctim.tv_sec &&
        replayed_file.st_ctim.tv_nsec == original_file.st_ctim.tv_nsec);
    CHECK(host_read(file) == "one original dispatch");

    // Keep the Store's original fingerprint guard: a changed expected version
    // is still a different start intent, even though wrapper replay is safe.
    expect(ErrorCode::Conflict, [&] {
        (void)session.store->start_tool_operation(actor, id,
            number(original_operation.at("state_version")), command + ":start");
    });
    auto changed_args = args;
    changed_args.as_object()["content"] = "different intent";
    expect(ErrorCode::Conflict, [&] {
        (void)session.store->prepare_tool_operation(actor, binding, expected, key,
            "fs_write", changed_args, command + ":prepare");
    });
    expect(ErrorCode::Conflict, [&] {
        (void)second.execute(actor, binding, expected, key, "fs_write", changed_args, command);
    });
    CHECK(session.store->operation(id) == original_operation);
    CHECK(session.store->operation_result(id) == original_result);
    CHECK(session.store->task(session.task_id()) == settled_task);
    CHECK(session.store->events(session.request_id, 0, 1000) == settled_events);
    CHECK(session.store->budget(session.request_id) == before_budget);
    CHECK(tool_calls() == before_tool_calls + 1 && operations() == before_operations + 1);
    CHECK(results() == before_results + 1 && attempts() == before_attempts);
    CHECK(host_read(file) == "one original dispatch");
    // On the old wrapper the deterministic late prepare is rejected here;
    // all preceding persisted-state and original-side-effect checks still run.
    if (delayed_error) std::rethrow_exception(delayed_error);
    succeeded(replay);
    CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
    CHECK(operation(replay) == original_operation && execution(replay) == original_result);
}

void revoked_tool_is_removed_from_new_model_context() {
    Session session;
    host_write(session.work / "allowed.txt", "still allowed");
    fixture_sql(session.database, "UPDATE runtime_policy SET denied_tools='[\"run\"]',policy_epoch=policy_epoch+1,state_version=state_version+1 WHERE singleton=1");
    const auto names = declared(session.tools->declarations(session.actor()));
    CHECK(!names.contains("run") && names.contains("fs_read"));
    const FakeModel fake(steps({{"allowed-read", "fs_read", Object{{"path", "allowed.txt"}}}}));
    const auto call = session.runtime->reserve_call(fake, session.request_id, Role::Worker, session.task_id());
    CHECK(std::find(call.request.context.tool_names.begin(), call.request.context.tool_names.end(), "run") == call.request.context.tool_names.end());
    CHECK(session.runtime->accept_response(call, fake.complete(call.request)).at("content") == "still allowed");
    expect(ErrorCode::Unauthorized, [&] { (void)session.execute("run", Object{{"argv", Array{"/bin/true"}}}); });
}

void result_commit_failure_blocks_new_business() {
    Session session;
    fixture_sql(session.database,
        "CREATE TRIGGER fail_execution_result_event BEFORE INSERT ON events "
        "WHEN NEW.type='record_op_result' BEGIN SELECT RAISE(ABORT,'injected result event failure'); END");
    const auto actor = session.actor(); const auto binding = session.binding();
    const Json args = Object{{"path", "effect-happened.txt"}, {"content", "written before SQLite settlement failed"}};
    bool returned = false;
    Json response;
    try { response = session.execute("fs_write", args, "unsettled-key", "unsettled-command"); returned = true; }
    catch (const Error&) {}
    if (returned) {
        CHECK(!response.is_object() || !response.as_object().contains("operation") || operation(response).at("status") != "SUCCEEDED");
    }
    CHECK(host_read(session.work / "effect-happened.txt") == "written before SQLite settlement failed");
    const auto stored = session.store->operation_for_key(actor, binding, "unsettled-key", "fs_write", args);
    CHECK(stored.is_object());
    CHECK(stored.at("status") != "SUCCEEDED");
    bool second_returned = false;
    Json second;
    try { second = session.execute("fs_write", Object{{"path", "must-stay-blocked"}, {"content", "x"}}); second_returned = true; }
    catch (const Error&) {}
    if (second_returned) CHECK(operation(second).at("status") != "SUCCEEDED");
    CHECK(!std::filesystem::exists(session.work / "must-stay-blocked"));
    CHECK(session.diagnostic("fs_read", Object{{"path", "effect-happened.txt"}}).at("content") == "written before SQLite settlement failed");
}

void settlement_fault_is_scoped_to_its_writer_session() {
    for (const bool reconcile_before_reopen : {true, false}) {
        Session session;
        const auto old_actor = session.actor();
        const auto original_binding = session.binding();
        const auto original_task_version = session.version();
        const auto original_file = session.work / "effect-before-settlement-failure.txt";
        const Json original_args = Object{{"path", original_file.filename().string()}, {"content", "original observed file effect"}};
        const auto original_budget = session.store->budget(session.request_id);
        const auto attempts = fixture_integer(session.database,
            "SELECT count(*) FROM call_attempts WHERE request_id=?", session.request_id);
        fixture_sql(session.database,
            "CREATE TRIGGER fail_session_definite_result BEFORE INSERT ON events "
            "WHEN NEW.type='record_op_result' AND json_extract(NEW.result,'$.observed_outcome')<>'UNKNOWN' "
            "BEGIN SELECT RAISE(ABORT,'injected first definite result failure'); END");
        CHECK(fixture_integer(session.database,
            "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name=?", "fail_session_definite_result") == 1);
        expect(ErrorCode::Storage, [&] {
            (void)session.execute("fs_write", original_args, "session-original-key", "session-original-command");
        });
        CHECK(host_read(original_file) == "original observed file effect");
        const auto unknown_operation = session.store->operation_for_key(old_actor, original_binding,
            "session-original-key", "fs_write", original_args);
        const auto id = text(unknown_operation.at("op_id"));
        CHECK(unknown_operation.at("status") == "UNKNOWN");
        const auto immutable_facts = [&] {
            auto result = session.store->operation_result(id);
            result.as_object().erase("operation_status");
            return result;
        };
        const auto original_facts = immutable_facts();
        CHECK(original_facts.at("observed_outcome") == "UNKNOWN" && original_facts.at("accepted") == 0);
        CHECK(original_facts.at("result").at("observed_outcome") == "SUCCEEDED");
        CHECK(original_facts.at("result").at("error") == "execution_result_persistence_failed");
        CHECK(original_facts.at("evidence").at("kind") == "settlement_failure");
        fixture_sql(session.database, "DROP TRIGGER fail_session_definite_result");
        CHECK(fixture_integer(session.database,
            "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name=?", "fail_session_definite_result") == 0);
        const auto original_history = session.store->events(session.request_id, 0, 1000);
        CHECK(original_history.size() < 995);
        struct stat original_identity {};
        CHECK(::lstat(original_file.c_str(), &original_identity) == 0);

        const auto rejected_without_effect = [&](ErrorCode expected_error, const StoreActor& actor, const std::string& label) {
            const Json args = Object{{"path", label + ".txt"}, {"content", "must not execute"}};
            const auto task = session.store->task(session.task_id());
            const auto request = session.store->request(session.request_id);
            const auto events = session.store->events(session.request_id, 0, 1000);
            const auto operation_count = fixture_integer(session.database,
                "SELECT count(*) FROM ops WHERE request_id=?", session.request_id);
            const auto tool_calls = fixture_integer(session.database,
                "SELECT tool_calls_used FROM teams WHERE team_id=?", actor.team_id);
            expect(expected_error, [&] {
                (void)session.tools->execute(actor, session.binding(), session.version(), label,
                    "fs_write", args, label + "-command");
            });
            CHECK(!std::filesystem::exists(session.work / (label + ".txt")));
            CHECK(session.store->operation_for_key(session.actor(), session.binding(), label, "fs_write", args).is_null());
            CHECK(session.store->task(session.task_id()) == task && session.store->request(session.request_id) == request);
            CHECK(session.store->events(session.request_id, 0, 1000) == events);
            CHECK(session.store->budget(session.request_id) == original_budget && immutable_facts() == original_facts);
            CHECK(fixture_integer(session.database, "SELECT count(*) FROM ops WHERE request_id=?", session.request_id) == operation_count);
            CHECK(fixture_integer(session.database, "SELECT tool_calls_used FROM teams WHERE team_id=?", actor.team_id) == tool_calls);
            CHECK(host_read(original_file) == "original observed file effect");
        };
        std::string reconciliation_evidence;
        const auto reconcile = [&] {
            // This is a real file observation, never process-exit or cleanup proof.
            reconciliation_evidence = "Host read original file bytes; sha256=" + sha256_hex(host_read(original_file));
            expect(ErrorCode::Unauthorized, [&] {
                (void)session.store->reconcile_operation(session.actor(), id,
                    number(unknown_operation.at("state_version")), "SUCCEEDED", reconciliation_evidence, "wrong-role-reconciliation");
            });
            const auto result = session.store->reconcile_operation(
                session.store->host_actor(Role::Root, session.request_id), id,
                number(unknown_operation.at("state_version")), "SUCCEEDED", reconciliation_evidence, "session-host-reconciliation");
            CHECK(result.at("status") == "SUCCEEDED");
            CHECK(number(result.at("state_version")) == number(unknown_operation.at("state_version")) + 1);
            CHECK(immutable_facts() == original_facts);
        };

        // Fresh host_actor calls within one Store must retain the failure fence.
        CHECK(session.actor().writer_session == old_actor.writer_session);
        rejected_without_effect(ErrorCode::NotReady, session.actor(), "same-session-before-reconciliation");
        if (reconcile_before_reopen) {
            reconcile();
            session.runtime.reset();
            session.tools.reset();
            session.tools = std::make_unique<WorkerTools>(*session.store, session.work);
            const auto fresh_actor = session.actor();
            session.tools->bind(fresh_actor);
            CHECK(fresh_actor.writer_session == old_actor.writer_session);
            rejected_without_effect(ErrorCode::NotReady, fresh_actor, "same-session-after-reconciliation");
        }
        const auto before_open_request = session.store->request(session.request_id);
        const auto before_open_task = session.store->task(session.task_id());
        const auto before_open_operation = session.store->operation(id);
        const auto before_open_events = session.store->events(session.request_id, 0, 1000);
        session.runtime.reset();
        session.tools.reset();
        session.store.reset();
        session.store = std::make_unique<Store>(session.database, base_sql, migration_sql,
            StoreOpenMode::Takeover, tools_sql, "", "", "", "", StoreRecoveryScope::request(session.request_id));
        session.tools = std::make_unique<WorkerTools>(*session.store, session.work);
        const auto current_actor = session.actor();
        session.tools->bind(current_actor);
        CHECK(current_actor.writer_session != old_actor.writer_session);
        CHECK(current_actor.dispatcher_epoch == old_actor.dispatcher_epoch && current_actor.execution_epoch == old_actor.execution_epoch);
        CHECK(to_json(session.binding()) == to_json(original_binding));
        CHECK(session.store->request(session.request_id) == before_open_request);
        CHECK(session.store->task(session.task_id()) == before_open_task);
        CHECK(session.store->operation(id) == before_open_operation);
        CHECK(session.store->events(session.request_id, 0, 1000) == before_open_events);
        CHECK(immutable_facts() == original_facts);
        rejected_without_effect(ErrorCode::Stale, old_actor, "old-session-after-reopen");
        const auto before_old_replay_operations = fixture_integer(session.database,
            "SELECT count(*) FROM ops WHERE request_id=?", session.request_id);
        const auto before_old_replay_tool_calls = fixture_integer(session.database,
            "SELECT tool_calls_used FROM teams WHERE team_id=?", current_actor.team_id);
        // Even the original prepare fingerprint cannot carry authority from
        // the old Store session into the new Store's exact replay path.
        expect(ErrorCode::Stale, [&] {
            (void)session.store->prepare_tool_operation(old_actor, original_binding, original_task_version,
                "session-original-key", "fs_write", original_args, "session-original-command:prepare");
        });
        CHECK(session.store->request(session.request_id) == before_open_request);
        CHECK(session.store->task(session.task_id()) == before_open_task);
        CHECK(session.store->operation(id) == before_open_operation);
        CHECK(session.store->events(session.request_id, 0, 1000) == before_open_events);
        CHECK(session.store->budget(session.request_id) == original_budget && immutable_facts() == original_facts);
        CHECK(fixture_integer(session.database, "SELECT count(*) FROM ops WHERE request_id=?", session.request_id) == before_old_replay_operations);
        CHECK(fixture_integer(session.database, "SELECT tool_calls_used FROM teams WHERE team_id=?", current_actor.team_id) == before_old_replay_tool_calls);
        CHECK(host_read(original_file) == "original observed file effect");
        if (!reconcile_before_reopen) {
            // A fresh session reaches the persisted UNKNOWN guard. It must not
            // inherit the old NotReady memory fence or gain business authority.
            rejected_without_effect(ErrorCode::UnknownOutcome, current_actor, "new-session-before-reconciliation");
            reconcile();
        }
        const auto reconciled = session.store->operation(id);
        CHECK(!reconciliation_evidence.empty());
        CHECK(text(reconciled.at("evidence_ref")) == "sha256:" + sha256_hex(reconciliation_evidence));
        auto original_projection = reconciled;
        for (const auto* field : {"status", "state_version", "ended_at", "evidence_ref"})
            original_projection.as_object()[field] = unknown_operation.at(field);
        CHECK(original_projection == unknown_operation);
        const auto reconciled_history = session.store->events(session.request_id, 0, 1000);
        CHECK(reconciled_history.size() == original_history.size() + 1);
        CHECK(std::equal(original_history.begin(), original_history.end(), reconciled_history.begin()));
        CHECK(reconciled_history.back().at("type") == "reconcile_op");
        const auto before_new_operations = fixture_integer(session.database,
            "SELECT count(*) FROM ops WHERE request_id=?", session.request_id);
        const auto before_new_tool_calls = fixture_integer(session.database,
            "SELECT tool_calls_used FROM teams WHERE team_id=?", current_actor.team_id);
        const Json new_args = Object{{"path", "after-reconciliation.txt"}, {"content", "one newly authorized effect"}};
        const auto fresh = session.execute("fs_write", new_args, "new-session-business-key", "new-session-business-command");
        succeeded(fresh);
        CHECK(fresh.at("executed_now") == true && fresh.at("replayed") == false);
        const auto after_new_events = session.store->events(session.request_id, 0, 1000);
        CHECK(after_new_events.size() == reconciled_history.size() + 3);
        CHECK(std::equal(reconciled_history.begin(), reconciled_history.end(), after_new_events.begin()));
        const auto replay = session.execute("fs_write", new_args, "new-session-business-key", "new-session-business-replay");
        CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
        CHECK(op_id(replay) == op_id(fresh) && execution(replay) == execution(fresh));
        CHECK(session.store->events(session.request_id, 0, 1000) == after_new_events);
        CHECK(session.store->operation(id) == reconciled && immutable_facts() == original_facts);
        CHECK(session.store->budget(session.request_id) == original_budget);
        CHECK(fixture_integer(session.database, "SELECT count(*) FROM call_attempts WHERE request_id=?", session.request_id) == attempts);
        CHECK(fixture_integer(session.database, "SELECT count(*) FROM ops WHERE request_id=?", session.request_id) == before_new_operations + 1);
        CHECK(fixture_integer(session.database, "SELECT tool_calls_used FROM teams WHERE team_id=?", current_actor.team_id) == before_new_tool_calls + 1);
        CHECK(host_read(session.work / "after-reconciliation.txt") == "one newly authorized effect");
        CHECK(host_read(original_file) == "original observed file effect");
        struct stat final_identity {};
        CHECK(::lstat(original_file.c_str(), &final_identity) == 0);
        CHECK(final_identity.st_dev == original_identity.st_dev && final_identity.st_ino == original_identity.st_ino);
        CHECK(final_identity.st_size == original_identity.st_size);
        CHECK(final_identity.st_mtim.tv_sec == original_identity.st_mtim.tv_sec && final_identity.st_mtim.tv_nsec == original_identity.st_mtim.tv_nsec);
        CHECK(final_identity.st_ctim.tv_sec == original_identity.st_ctim.tv_sec && final_identity.st_ctim.tv_nsec == original_identity.st_ctim.tv_nsec);
        rejected_without_effect(ErrorCode::Stale, old_actor, "old-session-after-new-business");
    }
}

void prepared_call_cannot_claim_after_another_settlement_fails() {
    Session session;
    WorkerTools second(*session.store, session.work);
    second.bind(session.actor());
    const auto actor = session.actor();
    const auto binding = session.binding();
    const auto expected = session.version();
    const std::string waiting_key = "prepared-before-settlement-failure";
    const Json waiting_args = Object{{"path", "must-never-dispatch.txt"}, {"content", "blocked after prepare"}};
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        Json prepared;
        bool reached{false};
        bool released{false};
        void release() {
            const std::lock_guard lock(mutex);
            released = true;
            changed.notify_all();
        }
    } gate;
    CHECK(!prepared_phase_observer);
    prepared_phase_observer = [&](std::string_view key, const Json& prepared) {
        if (key != waiting_key) return;
        std::unique_lock lock(gate.mutex);
        gate.prepared = prepared;
        gate.reached = true;
        gate.changed.notify_all();
        if (!gate.changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate.released; }))
            throw std::runtime_error("prepared-call test gate was not released");
    };
    struct ResetObserver { ~ResetObserver() { prepared_phase_observer = {}; } } reset;
    std::exception_ptr waiting_error;
    std::jthread waiting([&] {
        try {
            (void)second.execute(actor, binding, expected, waiting_key, "fs_write", waiting_args, "waiting-command");
        } catch (...) { waiting_error = std::current_exception(); }
    });
    // Assertion failures also release the worker before jthread joins it.
    struct ReleaseGate { Gate& gate; ~ReleaseGate() { gate.release(); } } release{gate};
    Json waiting_operation;
    {
        std::unique_lock lock(gate.mutex);
        CHECK(gate.changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate.reached; }));
        waiting_operation = gate.prepared;
    }
    const auto waiting_id = text(waiting_operation.at("op_id"));
    CHECK(session.store->operation(waiting_id).at("status") == "PREPARED");
    CHECK(session.store->operation(waiting_id).at("started_at").is_null());

    // Fail the full fact, the small UNKNOWN fact, and the legacy UNKNOWN
    // fallback. Thus only the shared process fence can stop a fresh claim;
    // an UNKNOWN database row cannot accidentally make this test pass.
    fixture_sql(session.database,
        "CREATE TRIGGER fail_all_execution_settlements BEFORE INSERT ON events "
        "WHEN NEW.type IN ('record_op_result','finish_op') "
        "BEGIN SELECT RAISE(ABORT,'injected complete settlement failure'); END");
    const Json effect_args = Object{{"path", "unsettled-effect.txt"}, {"content", "effect exists without a durable result"}};
    expect(ErrorCode::Storage, [&] {
        (void)session.execute("fs_write", effect_args, "failing-settlement-key", "failing-settlement-command");
    });
    CHECK(host_read(session.work / "unsettled-effect.txt") == "effect exists without a durable result");
    const auto failed_operation = session.store->operation_for_key(actor, binding, "failing-settlement-key", "fs_write", effect_args);
    CHECK(failed_operation.at("status") == "RUNNING");
    CHECK(session.store->operation_result(text(failed_operation.at("op_id"))).is_null());
    fixture_sql(session.database, "DROP TRIGGER fail_all_execution_settlements");

    // Storage is healthy again before B resumes. B had already passed the
    // early admission check and committed PREPARED, but must not gain claim.
    gate.release();
    waiting.join();
    CHECK(waiting_error != nullptr);
    expect(ErrorCode::NotReady, [&] { std::rethrow_exception(waiting_error); });
    const auto after = session.store->operation(waiting_id);
    CHECK(after.at("status") == "PREPARED" && after.at("started_at").is_null());
    CHECK(after.at("state_version") == waiting_operation.at("state_version"));
    CHECK(session.store->operation_result(waiting_id).is_null());
    CHECK(!std::filesystem::exists(session.work / "must-never-dispatch.txt"));

    WorkerTools fresh(*session.store, session.work);
    fresh.bind(actor);
    const Json fresh_args = Object{{"path", "fresh-instance-blocked.txt"}, {"content", "also blocked"}};
    expect(ErrorCode::NotReady, [&] {
        (void)fresh.execute(actor, binding, session.version(), "fresh-instance-key", "fs_write", fresh_args, "fresh-instance-command");
    });
    CHECK(session.store->operation_for_key(actor, binding, "fresh-instance-key", "fs_write", fresh_args).is_null());
    CHECK(!std::filesystem::exists(session.work / "fresh-instance-blocked.txt"));
    CHECK(fresh.diagnostic(actor, binding, "fs_read", Object{{"path", "unsettled-effect.txt"}}).at("content") ==
        "effect exists without a durable result");
    const auto replay = fresh.execute(actor, binding, session.version(), waiting_key, "fs_write", waiting_args, "waiting-replay");
    CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
    CHECK(operation(replay).at("status") == "PREPARED" && execution(replay).is_null());
}

void exact_old_ack_replays_without_acknowledging_new_order() {
    Session session;
    session.issue_new_instruction();
    const auto old_actor = session.actor(); const auto old_binding = session.binding(); const auto old_version = session.version();
    const auto first = session.tools->execute(old_actor, old_binding, old_version, "ack-key", "ack_change", Object{}, "exact-ack");
    session.issue_new_instruction();
    const auto before = session.store->task(session.task_id());
    const auto replay = session.tools->execute(old_actor, old_binding, old_version, "ack-key", "ack_change", Object{}, "exact-ack");
    CHECK(replay == first);
    CHECK(session.store->task(session.task_id()) == before);
    CHECK(number(before.at("applied_instruction_revision")) < number(before.at("instruction_revision")));
    expect(ErrorCode::NotReady, [&] { (void)session.execute("fs_write", Object{{"path", "old-ack-cannot-enable"}, {"content", "x"}}); });
    CHECK(!std::filesystem::exists(session.work / "old-ack-cannot-enable"));
}
} // namespace

int main() {
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    frozen = std::make_unique<FrozenRequestConfig>(load_config_catalog(source / "config/examples/system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general"));
    base_sql = read_runtime_file(source / "db/schema.sql");
    migration_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    original_script = parse_json_strict(read_runtime_file(source / "tests/fixtures/s1_fake_initial.json"));
    constraints = parse_json_strict(read_runtime_file(source / "tests/fixtures/s1_constraints.json"));
    return test::run({
        {"static registration and frozen request tool grants", registry_and_request_permissions},
        {"durable real file write/read/patch and side-effect-free business replay", durable_write_patch_and_business_replay},
        {"AMEND rejects old model file effects while retaining usage", late_model_response_after_amend_cannot_write},
        {"old business key only reads its fact during a pending new instruction", old_key_is_only_a_read_during_new_instruction},
        {"workspace directory and execution manifest binding are immutable", workspace_and_execution_manifest_are_immutable},
        {"non-Worker, cross-workspace and model authority forgery are rejected", role_scope_and_model_authority_forgery},
        {"missing strong sandbox gives a durable FAILED result without spawning", unavailable_strong_run_is_a_durable_refusal},
        {"pre-effect cancellation retains exact no-spawn evidence and cannot replay effects", cancellation_before_effect_preserves_original_no_spawn_evidence},
        {"read-only diagnostics do not mutate epochs, task state, budgets or events", readonly_diagnostics_preserve_epochs_and_budgets},
        {"UNKNOWN preserves diagnostics and instruction control while blocking business", unknown_allows_diagnostics_and_instruction_control},
        {"bounded model loop preserves original text and complete tool feedback", model_loop_carries_complete_tool_results},
        {"Worker call bounds and failed tools stop subsequent model effects", batch_limits_and_failed_tool_stop_further_calls},
        {"payload, path and schema restrictions precede filesystem effects", path_payload_and_schema_limits_precede_effects},
        {"concurrent exact commands have at most one actual execution owner", concurrent_exact_commands_have_one_execution_owner},
        {"late prepare replay preserves one original dispatch and Store intent guards", delayed_prepare_replay_keeps_the_original_dispatch_intent},
        {"current policy revocation removes tools from new model contexts", revoked_tool_is_removed_from_new_model_context},
        {"SQLite result-event failure after file effect blocks further business", result_commit_failure_blocks_new_business},
        {"settlement failure fences its writer session while UNKNOWN remains a Store gate", settlement_fault_is_scoped_to_its_writer_session},
        {"prepared concurrent call cannot claim after another result and all fallbacks fail", prepared_call_cannot_claim_after_another_settlement_fails},
        {"exact old ack replay cannot acknowledge a newer instruction", exact_old_ack_replays_without_acknowledging_new_order}
    });
}
