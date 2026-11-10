#include "wls/worker_tools.hpp"
#include "wls/offline_runtime.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <charconv>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
void worker_tool_test_phase(std::string_view key, const wls::s1::Json& prepared);
}

// Compile the actual implementation in this TU to observe a single private
// phase. The static linker uses these definitions instead of pulling the
// library's worker_tools.o; production builds have no observer at all.
#define WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE(key, prepared) ::worker_tool_test_phase(key, prepared)
#include "../src/worker_tools.cpp"
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
std::string events_sql;
Json original_script;
Json constraints;
constexpr std::string_view original_text = "请在自有临时 workspace 测试文件工具，保留原始需求；不要调用外部服务。";

std::function<void(std::string_view, const Json&)> prepared_phase_observer;
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

// G1-3 correction regressions on the OfflineRuntime::run host. The legacy 403
// Session above stays byte-identical; these tests use a separate event-enabled
// structured-v2 fixture so durable feedback exists. The same test bytes run
// against the exact baseline (behavioral RED) and the candidate (GREEN).
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
    host_write(configuration.path / "system.yaml", system);
    return load_config_catalog(configuration.path / "system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general");
}
FakeModelStep worker_step(std::int64_t index, const std::string& body, std::vector<ModelToolCall> calls) {
    FakeModelStep step;
    step.role = Role::Worker; step.role_call_index = index; step.text = body;
    step.tool_calls = std::move(calls); step.usage = ModelUsage{100, 20, 0, 0, 0};
    return step;
}
std::vector<FakeModelStep> correction_steps(const std::vector<FakeModelStep>& worker_steps) {
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
struct EventSession {
    test::TempDir directory;
    std::filesystem::path database = directory.path / "event-runtime.db";
    std::filesystem::path work = directory.path / "work";
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> tools;
    std::unique_ptr<OfflineRuntime> runtime;
    std::string request_id;

    explicit EventSession(const char* schema_retry = "2") {
        std::filesystem::create_directory(work);
        store = std::make_unique<Store>(database, base_sql, migration_sql, StoreOpenMode::Takeover, tools_sql, events_sql);
        const auto correction_frozen = correction_config(schema_retry);
        AdmissionInput admission_input;
        admission_input.tenant_id = "worker-tool-test"; admission_input.project_id = "temporary-fixtures"; admission_input.root_id = "test-root";
        admission_input.function_id = correction_frozen.function_id(); admission_input.idempotency_key = new_id("key");
        admission_input.original_text = original_text; admission_input.config_snapshot = correction_frozen.json();
        admission_input.constraint_catalog = constraints;
        const auto grants = correction_frozen.worker_tools();
        admission_input.tool_allowlist.assign(grants.begin(), grants.end());
        request_id = text(store->admit(admission_input).at("request_id"));
        tools = std::make_unique<WorkerTools>(*store, work);
        runtime = std::make_unique<OfflineRuntime>(*store, *schemas, tools.get());
    }
    std::string task_id() const { return text(store->tasks(request_id).front().at("task_id")); }
    ExecutionBinding binding() const { return store->binding(task_id()); }
    StoreActor actor() const { return store->host_actor(Role::Worker, request_id, binding().team_id); }
};

// G1-3 FINAL v14 3 (D2) local original capture. The enable state is frozen
// by the execution environment's WLS_G1_EVIDENCE_ROOT key and is constant
// for one test process: a key that is completely absent disables only this
// optional capture I/O while every business action, assertion and fixture
// cleanup still runs (such a run never counts as retained evidence); a
// present value must name this run's bound owner-only directory -- absolute,
// a real directory rather than a symlink, owned by the current user, mode
// 0700 -- and any other present value is an explicit evidence failure, never
// a silent fallback to a default location.
std::optional<std::filesystem::path> correction_evidence_root() {
    const char* raw = std::getenv("WLS_G1_EVIDENCE_ROOT");
    if (raw == nullptr) return std::nullopt;
    const std::string value(raw);
    struct stat info {};
    if (value.empty() || value.front() != '/' || ::lstat(value.c_str(), &info) != 0 ||
        !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0700)
        throw std::runtime_error("G1 evidence root is not a bound absolute owner-only 0700 directory: " +
            (value.empty() ? std::string("<empty>") : value));
    return std::filesystem::path(value);
}
struct EvidenceDescriptor {
    int value;
    explicit EvidenceDescriptor(int descriptor) noexcept : value(descriptor) {}
    ~EvidenceDescriptor() { if (value >= 0) ::close(value); }
    EvidenceDescriptor(const EvidenceDescriptor&) = delete;
    EvidenceDescriptor& operator=(const EvidenceDescriptor&) = delete;
};
// Exclusive-create copy: an existing target in any form (plain file or
// symlink) is refused by O_EXCL with O_NOFOLLOW rather than overwritten, the
// origin's exact bytes are preserved, and every new original lands at 0600.
void capture_correction_file(const std::filesystem::path& root, const std::string& case_name,
                             const std::string& name, const std::filesystem::path& origin) {
    const auto target = root / (case_name + "-" + new_id("ev") + "-" + name);
    const EvidenceDescriptor destination(
        ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
    if (destination.value < 0)
        throw std::runtime_error("G1 evidence target cannot be exclusively created: " +
            target.string() + ": " + std::strerror(errno));
    const EvidenceDescriptor origin_descriptor(::open(origin.c_str(), O_RDONLY));
    if (origin_descriptor.value < 0)
        throw std::runtime_error("G1 evidence origin cannot be opened: " + origin.string() +
            ": " + std::strerror(errno));
    char buffer[4096];
    ssize_t chunk = 0;
    while ((chunk = ::read(origin_descriptor.value, buffer, sizeof buffer)) > 0) {
        for (ssize_t written = 0; written < chunk;) {
            const ssize_t sent = ::write(destination.value, buffer + written,
                static_cast<std::size_t>(chunk - written));
            if (sent < 0)
                throw std::runtime_error("G1 evidence target write failed: " + target.string() +
                    ": " + std::strerror(errno));
            written += sent;
        }
    }
    if (chunk < 0)
        throw std::runtime_error("G1 evidence origin read failed: " + origin.string() +
            ": " + std::strerror(errno));
    if (::fchmod(destination.value, 0600) != 0)
        throw std::runtime_error("G1 evidence target mode failed: " + target.string() +
            ": " + std::strerror(errno));
}
// Called only after the fixture's Store and every read/write connection
// closed; copies the physical database plus any WAL/SHM sidecars that still
// exist at this capture point. The absent-key branch returns before any
// capture file I/O runs.
void capture_correction_database(const std::string& case_name, const std::filesystem::path& database) {
    const std::optional<std::filesystem::path> root = correction_evidence_root();
    if (!root.has_value()) return;
    capture_correction_file(*root, case_name, "database", database);
    for (const auto* sidecar : {"-wal", "-shm"}) {
        std::error_code ignored;
        const auto path = std::filesystem::path(database.string() + sidecar);
        if (std::filesystem::exists(path, ignored))
            capture_correction_file(*root, case_name, sidecar + 1, path);
    }
}
// Close the session's runtime, tools and Store in dependency order, then
// preserve the physical originals before the TempDir cleanup runs; the
// capture step itself is the only part an absent evidence key disables.
void close_and_capture(EventSession& session, const std::string& case_name) {
    session.runtime.reset(); session.tools.reset(); session.store.reset();
    capture_correction_database(case_name, session.database);
}
// Failing-assertion retention (FINAL v12 6.4): when a CHECK fails on the
// old candidate, the already-obtained physical original is still closed and
// preserved before the original failure continues; a capture failure on this
// path is reported explicitly and never masks the original failure or turns
// the case PASS.
void retain_originals_on_failure(EventSession& session, const std::string& case_name) {
    try {
        close_and_capture(session, case_name);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "G1 evidence capture failed on the failing path for %s: %s\n",
            case_name.c_str(), error.what());
    }
}

void run_text_only_failure_corrects_within_one_invocation() {
    EventSession session;
    const auto script = correction_steps({
        worker_step(1, "text-only response without a tool call", {}),
        worker_step(2, "", {{"fixed-write", "fs_write", Object{{"path", "corrected.txt"}, {"content", "run host fixed\n"}}}}),
    });
    Json result;
    bool rejected = false;
    try { result = session.runtime->run(session.request_id, script, "", 2); }
    catch (const Error& error) { rejected = true; CHECK(error.code() == ErrorCode::InvalidInput); }
    CHECK(!rejected);
    const auto& calls = result.at("calls").as_array();
    CHECK(calls.size() == 4);
    CHECK(calls[2].at("role").as_string() == "worker" && calls[3].at("role").as_string() == "worker");
    const auto feedback = session.store->tool_feedback(text(calls[2].at("attempt_id")));
    expect_correction_feedback(feedback.at("result"), "missing_tool_call", "model_schema");
    CHECK(host_read(session.work / "corrected.txt") == "run host fixed\n");
    CHECK(number(session.store->budget(session.request_id).at("attempts")) == 4);
    // The corrected capture observes the failure as data, never a paired tool output.
    const auto envelope = model_request_from_json(
        session.store->attempt(text(calls[3].at("attempt_id"))).at("request_envelope"));
    const auto& messages = envelope.context.logical_input.at("messages").as_array();
    CHECK(messages.size() == 1);
    CHECK(messages.front().at("role").as_string() == "user");
    CHECK(messages.front().at("source").at("kind").as_string() == "unapplied_or_missing_observation");
    CHECK(messages.front().at("source").at("not_execution_authority") == true);
}

void run_invalid_arguments_correct_without_dispatch() {
    EventSession session;
    const Json bad_arguments = Object{{"path", "args.txt"}};
    const auto script = correction_steps({
        worker_step(1, "", {{"bad-write", "fs_write", bad_arguments}}),
        worker_step(2, "", {{"fixed-write", "fs_write", Object{{"path", "args.txt"}, {"content", "run args fixed\n"}}}}),
    });
    Json result;
    bool rejected = false;
    try { result = session.runtime->run(session.request_id, script, "", 2); }
    catch (const Error& error) { rejected = true; CHECK(error.code() == ErrorCode::InvalidInput); }
    CHECK(!rejected);
    const auto& calls = result.at("calls").as_array();
    CHECK(calls.size() == 4);
    const auto feedback = session.store->tool_feedback(text(calls[2].at("attempt_id")));
    expect_correction_feedback(feedback.at("result"), "invalid_arguments", "model_schema");
    CHECK(session.store->operation_for_key(session.actor(), session.binding(),
        text(calls[2].at("attempt_id")), "fs_write", bad_arguments).is_null());
    CHECK(host_read(session.work / "args.txt") == "run args fixed\n");
    CHECK(number(session.store->budget(session.request_id).at("attempts")) == 4);
}

void run_missing_read_returns_result_and_corrects() {
    EventSession session;
    host_write(session.work / "present.txt", "present bytes\n");
    const auto script = correction_steps({
        worker_step(1, "", {{"missing-read", "fs_read", Object{{"path", "absent.txt"}, {"max_bytes", 16}}}}),
        worker_step(2, "", {{"present-read", "fs_read", Object{{"path", "present.txt"}, {"max_bytes", 64}}}}),
    });
    Json result;
    bool rejected = false;
    try { result = session.runtime->run(session.request_id, script, "", 2); }
    catch (const Error& error) { rejected = true; CHECK(error.code() == ErrorCode::InvalidInput); }
    CHECK(!rejected);
    const auto& calls = result.at("calls").as_array();
    CHECK(calls.size() == 4);
    const auto feedback = session.store->tool_feedback(text(calls[2].at("attempt_id")));
    expect_correction_feedback(feedback.at("result"), "not_found", "tool_result");
    CHECK(!feedback.at("result").as_object().contains("content"));
    CHECK(calls[3].at("result").at("content") == "present bytes\n");
    // A diagnostic read never creates an operation row by design (Store
    // operation keys exist only for business tools); the SQL-level zero-op
    // assertion for fs_read lives in the TLS L3 regression.
    CHECK(number(session.store->budget(session.request_id).at("attempts")) == 4);
}

void run_precondition_failure_corrects_with_cas() {
    EventSession session;
    host_write(session.work / "exists.txt", "original bytes\n");
    const auto script = correction_steps({
        worker_step(1, "", {{"create-existing", "fs_write", Object{{"path", "exists.txt"}, {"content", "must not overwrite\n"}}}}),
        worker_step(2, "", {{"cas-write", "fs_write", Object{{"path", "exists.txt"}, {"content", "cas corrected\n"},
            {"expected_sha256", sha256_hex("original bytes\n")}}}}),
    });
    const auto result = session.runtime->run(session.request_id, script, "", 2);
    const auto& calls = result.at("calls").as_array();
    CHECK(calls.size() == 4);
    const auto& first = calls[2].at("result");
    CHECK(first.at("operation").at("status") == "FAILED");
    CHECK(first.at("execution_result").at("observed_outcome") == "FAILED");
    CHECK(first.at("execution_result").at("evidence").at("failure_kind").as_string() == "content_precondition");
    CHECK(first.at("correction_feedback").at("reason").as_string() == "content_precondition");
    CHECK(first.at("correction_feedback").at("correction_eligible") == true);
    CHECK(host_read(session.work / "exists.txt") == "cas corrected\n");
    CHECK(number(session.store->budget(session.request_id).at("attempts")) == 4);
    CHECK(session.store->operation(text(first.at("operation").at("op_id"))).at("status") == "FAILED");
}

void run_zero_schema_retry_saves_feedback_without_second_attempt() {
    EventSession session("0");
    const auto script = correction_steps({
        worker_step(1, "text-only failure with retries disabled", {}),
        worker_step(2, "", {{"never-used", "fs_write", Object{{"path", "never.txt"}, {"content", "x\n"}}}}),
    });
    Json result;
    bool rejected = false;
    try { result = session.runtime->run(session.request_id, script, "", 2); }
    catch (const Error& error) { rejected = true; CHECK(error.code() == ErrorCode::InvalidInput); }
    CHECK(!rejected);
    const auto& calls = result.at("calls").as_array();
    CHECK(calls.size() == 3);
    const auto feedback = session.store->tool_feedback(text(calls[2].at("attempt_id")));
    expect_correction_feedback(feedback.at("result"), "missing_tool_call", "model_schema");
    CHECK(!std::filesystem::exists(session.work / "never.txt"));
    CHECK(number(session.store->budget(session.request_id).at("attempts")) == 3);
}

// G1-3 v8 C1 guard on this host: run() already scopes its admission counter
// to the whole invocation, so a success between two ordinary failures must
// not reset the accumulated count, and a host call bound that ends the chain
// before any admission is denied must never be reported as schema
// exhaustion. Both hold on the v6 candidate and must survive the C1/C2 fix.
void run_admissions_survive_success_and_host_caps_report_no_exhaustion() {
    {
        EventSession session("1");
        try {
            const auto result = session.runtime->run(session.request_id, correction_steps({
                worker_step(1, "text-only failure before a successful correction", {}),
                worker_step(2, "", {{"fixed-write", "fs_write", Object{{"path", "fixed.txt"}, {"content", "fixed once\n"}}}}),
                worker_step(3, "text-only failure after the admitted round", {}),
                worker_step(4, "", {{"never-fourth", "fs_write", Object{{"path", "fourth.txt"}, {"content", "never\n"}}}}),
            }), "", 8);
            const auto& calls = result.at("calls").as_array();
            CHECK(calls.size() == 5);
            expect_correction_feedback(session.store->tool_feedback(text(calls.at(4).at("attempt_id"))).at("result"),
                "missing_tool_call", "model_schema");
            CHECK(host_read(session.work / "fixed.txt") == "fixed once\n");
            CHECK(!std::filesystem::exists(session.work / "fourth.txt"));
            CHECK(number(session.store->budget(session.request_id).at("attempts")) == 5);
            close_and_capture(session, "worker-run-host-admissions-accumulate");
        } catch (...) {
            retain_originals_on_failure(session, "worker-run-host-admissions-accumulate");
            throw;
        }
    }
    {
        // The explicit max_worker_calls bound ends the loop before a third
        // failure is denied: the dedicated correction_limit report (asserted
        // exhaustively in offline_runtime_tests.cpp) must stay absent here.
        EventSession session("2");
        try {
            const auto result = session.runtime->run(session.request_id, correction_steps({
                worker_step(1, "text-only failure under a tighter host cap", {}),
                worker_step(2, "second text-only failure under a tighter host cap", {}),
                worker_step(3, "", {{"unreached", "fs_write", Object{{"path", "unreached.txt"}, {"content", "x\n"}}}}),
            }), "", 2);
            CHECK(result.at("calls").as_array().size() == 4);
            if (const auto* blocked_entries = result.as_object().if_contains("blocked"))
                for (const auto& entry : blocked_entries->as_array())
                    CHECK(entry.at("code").as_string() != "correction_limit");
            CHECK(!std::filesystem::exists(session.work / "unreached.txt"));
            CHECK(number(session.store->budget(session.request_id).at("attempts")) == 4);
            close_and_capture(session, "worker-run-host-worker-cap-no-exhaustion");
        } catch (...) {
            retain_originals_on_failure(session, "worker-run-host-worker-cap-no-exhaustion");
            throw;
        }
    }
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
    events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    original_script = parse_json_strict(read_runtime_file(source / "tests/fixtures/s1_fake_initial.json"));
    constraints = parse_json_strict(read_runtime_file(source / "tests/fixtures/s1_constraints.json"));
    return test::run({
        {"static registration and frozen request tool grants", registry_and_request_permissions},
        {"run-host text-only failure corrects within one invocation", run_text_only_failure_corrects_within_one_invocation},
        {"run-host invalid arguments correct without dispatch", run_invalid_arguments_correct_without_dispatch},
        {"run-host missing read returns a result and corrects", run_missing_read_returns_result_and_corrects},
        {"run-host precondition failure corrects with CAS", run_precondition_failure_corrects_with_cas},
        {"run-host zero schema retry saves feedback without a second attempt", run_zero_schema_retry_saves_feedback_without_second_attempt},
        {"run-host admissions survive success and host caps report no exhaustion", run_admissions_survive_success_and_host_caps_report_no_exhaustion},
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
        {"current policy revocation removes tools from new model contexts", revoked_tool_is_removed_from_new_model_context},
        {"SQLite result-event failure after file effect blocks further business", result_commit_failure_blocks_new_business},
        {"prepared concurrent call cannot claim after another result and all fallbacks fail", prepared_call_cannot_claim_after_another_settlement_fails},
        {"exact old ack replay cannot acknowledge a newer instruction", exact_old_ack_replays_without_acknowledging_new_order}
    });
}
