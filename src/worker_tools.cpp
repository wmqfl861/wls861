#include "wls/worker_tools.hpp"
#include "wls/process_evidence.hpp"
#include "wls/offline_runtime.hpp"

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;
void require(bool condition, ErrorCode code, const std::string& message) {
    if (!condition) throw Error(code, message);
}
std::string string(const Json& value) {
    require(value.is_string(), ErrorCode::InvalidInput, "expected a string");
    return std::string(value.as_string());
}
std::uint64_t unsigned_number(const Json& value) {
    if (value.is_uint64()) return value.as_uint64();
    require(value.is_int64() && value.as_int64() >= 0, ErrorCode::InvalidInput, "expected an unsigned integer");
    return static_cast<std::uint64_t>(value.as_int64());
}
std::int64_t signed_number(const Json& value) {
    const auto n = unsigned_number(value);
    require(n <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), ErrorCode::InvalidInput, "integer exceeds range");
    return static_cast<std::int64_t>(n);
}
std::string field(const Json& value, std::string_view key) {
    require(value.is_object() && value.as_object().contains(key), ErrorCode::InvalidInput, "missing string field: " + std::string(key));
    return string(value.at(key));
}
Json text_schema(std::int64_t minimum, std::int64_t maximum) {
    return Object{{"type", "string"}, {"minLength", minimum}, {"maxLength", maximum}};
}
Json number_schema(std::int64_t minimum, std::int64_t maximum) {
    return Object{{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}
Json object_schema(const Object& properties, const Array& required) {
    return Object{{"type", "object"}, {"additionalProperties", false}, {"properties", properties}, {"required", required}};
}
const Json& required(const Json& args, std::string_view key) {
    require(args.is_object() && args.as_object().contains(key), ErrorCode::InvalidInput, "missing tool argument: " + std::string(key));
    return args.at(key);
}
void allowed(const Json& args, std::initializer_list<std::string_view> keys) {
    require(args.is_object(), ErrorCode::InvalidInput, "tool arguments must be an object");
    for (const auto& item : args.as_object())
        require(std::find(keys.begin(), keys.end(), item.key()) != keys.end(), ErrorCode::InvalidInput,
                "unknown tool argument: " + std::string(item.key()));
}
void text_argument(const Json& args, std::string_view key, std::size_t minimum,
                   std::size_t maximum, bool optional = false, bool permit_nul = false) {
    if (optional && !args.as_object().contains(key)) return;
    const auto value = string(required(args, key));
    require(value.size() >= minimum && value.size() <= maximum && (permit_nul || value.find('\0') == std::string::npos),
            ErrorCode::InvalidInput, "tool string outside byte limits: " + std::string(key));
}
void number_argument(const Json& args, std::string_view key, std::uint64_t minimum, std::uint64_t maximum) {
    if (!args.as_object().contains(key)) return;
    const auto value = unsigned_number(args.at(key));
    require(value >= minimum && value <= maximum, ErrorCode::InvalidInput, "tool integer outside limits: " + std::string(key));
}
void hash_argument(const Json& args, std::string_view key, bool optional = false) {
    if (optional && !args.as_object().contains(key)) return;
    const auto value = string(required(args, key));
    require(value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }), ErrorCode::InvalidInput, "expected lowercase SHA-256: " + std::string(key));
}
std::string optional_text(const Json& args, std::string_view key, const std::string& fallback) {
    return args.as_object().contains(key) ? field(args, key) : fallback;
}
std::uint64_t optional_number(const Json& args, std::string_view key, std::uint64_t fallback) {
    return args.as_object().contains(key) ? unsigned_number(args.at(key)) : fallback;
}
std::uint64_t memory_bytes(const std::string& text) {
    require(!text.empty(), ErrorCode::Corrupt, "empty memory size");
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    require(error == std::errc{} && value > 0, ErrorCode::Corrupt, "invalid memory size");
    std::uint64_t multiplier = 1;
    if (end != text.data() + text.size()) {
        require(end + 1 == text.data() + text.size(), ErrorCode::Corrupt, "invalid memory suffix");
        if (*end == 'K') multiplier = 1024;
        else if (*end == 'M') multiplier = 1024 * 1024;
        else if (*end == 'G') multiplier = 1024ULL * 1024 * 1024;
        else throw Error(ErrorCode::Corrupt, "unsupported memory suffix");
    }
    require(value <= std::numeric_limits<std::uint64_t>::max() / multiplier, ErrorCode::Corrupt, "memory size overflow");
    return value * multiplier;
}
Json limits_snapshot() {
    const WorkspaceLimits limits;
    return Object{{"max_file_bytes", limits.max_file_bytes}, {"max_total_bytes", limits.max_total_bytes},
        {"max_read_bytes", limits.max_read_bytes}, {"max_write_bytes", limits.max_write_bytes},
        {"max_entries", limits.max_entries}, {"max_list_entries", limits.max_list_entries},
        {"max_search_files", limits.max_search_files}, {"max_search_bytes", limits.max_search_bytes},
        {"max_search_matches", limits.max_search_matches}, {"max_path_bytes", limits.max_path_bytes},
        {"max_depth", limits.max_depth}, {"tool_write_bytes", 65536}, {"tool_read_page_bytes", 65536},
        {"tool_list_page_entries", 256}, {"tool_search_matches", 100}, {"tool_schema_version", "wls-worker-tools-v1"}};
}
// All WorkerTools instances sharing a writer use the same cancellation source.
// A Store's process-wide writer lease prevents a second Store for this database.
struct ActiveRuns {
    std::mutex mutex;
    std::map<std::pair<const Store*, std::string>, std::stop_source> operations;
    std::set<std::pair<std::string, std::int64_t>> failed_settlements;
    bool settlement_capacity_exhausted{false};
};
ActiveRuns& active_runs() { static ActiveRuns value; return value; }
} // namespace

const std::vector<ToolDefinition>& worker_tool_registry() {
    static const auto path = text_schema(1, 4096);
    static const Json hash = Object{{"type", "string"}, {"pattern", "^[a-f0-9]{64}$"}};
    static const auto maximum = std::numeric_limits<std::int64_t>::max();
    static const std::vector<ToolDefinition> entries{
        {"note", "wls-offline-note-v1", ToolClass::Business, "none", object_schema({{"text", text_schema(1, 4000)}}, {"text"})},
        {"fs_list", "wls-fs-list-v1", ToolClass::Diagnostic, "none", object_schema({{"path", path}, {"after_name", text_schema(0, 255)}, {"limit", number_schema(1, 256)}}, {})},
        {"fs_read", "wls-fs-read-v1", ToolClass::Diagnostic, "none", object_schema({{"path", path}, {"offset", number_schema(0, maximum)}, {"max_bytes", number_schema(1, 65536)}}, {"path"})},
        {"fs_search", "wls-fs-search-v1", ToolClass::Diagnostic, "none", object_schema({{"path", path}, {"literal", text_schema(1, 4096)}, {"max_matches", number_schema(1, 100)}}, {"path", "literal"})},
        {"fs_write", "wls-fs-write-v1", ToolClass::Business, "workspace", object_schema({{"path", path}, {"content", text_schema(0, 65536)}, {"expected_sha256", hash}}, {"path", "content"})},
        {"fs_patch", "wls-fs-patch-v1", ToolClass::Business, "workspace", object_schema({{"path", path}, {"expected_sha256", hash}, {"offset", number_schema(0, maximum)}, {"expected", text_schema(1, 65536)}, {"replacement", text_schema(0, 65536)}}, {"path", "expected_sha256", "offset", "expected", "replacement"})},
        {"run", "wls-run-v1", ToolClass::Business, "workspace", object_schema({{"argv", Object{{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", text_schema(1, 4096)}}}, {"cwd", path}, {"timeout_s", number_schema(1, 600)}, {"max_output_bytes", number_schema(1, 65536)}}, {"argv"})},
        {"op_status", "wls-op-status-v1", ToolClass::Diagnostic, "none", object_schema({{"op_id", text_schema(29, 29)}}, {"op_id"})},
        {"op_output", "wls-op-output-v1", ToolClass::Diagnostic, "none", object_schema({{"op_id", text_schema(29, 29)}, {"stream", Object{{"enum", Array{"stdout", "stderr"}}}}, {"offset", number_schema(0, maximum)}, {"limit", number_schema(1, 65536)}}, {"op_id", "stream"})},
        {"op_cancel", "wls-op-cancel-v1", ToolClass::Control, "none", object_schema({{"op_id", text_schema(29, 29)}}, {"op_id"})},
        {"ack_change", "wls-ack-change-v1", ToolClass::Control, "none", object_schema({}, {})}
    };
    return entries;
}

const ToolDefinition& worker_tool_definition(std::string_view name) {
    const auto& all = worker_tool_registry();
    const auto found = std::find_if(all.begin(), all.end(), [&](const auto& item) { return item.name == name; });
    require(found != all.end(), ErrorCode::Unsupported, "Worker tool has no registered implementation: " + std::string(name));
    return *found;
}

void validate_worker_tool_arguments(std::string_view name, const Json& args) {
    static_cast<void>(worker_tool_definition(name));
    require(args.is_object(), ErrorCode::InvalidInput, "tool arguments must be an object");
    // Besides shape, these limits count decoded bytes, not Unicode characters.
    // Strict JSON also rejects invalid strings/numbers in programmatic inputs.
    static_cast<void>(parse_json_strict(canonical_json(args), 256 * 1024));
    if (name == "note") { allowed(args, {"text"}); text_argument(args, "text", 1, 4000); }
    else if (name == "fs_list") {
        allowed(args, {"path", "after_name", "limit"}); text_argument(args, "path", 1, 4096, true);
        text_argument(args, "after_name", 0, 255, true); number_argument(args, "limit", 1, 256);
    } else if (name == "fs_read") {
        allowed(args, {"path", "offset", "max_bytes"}); text_argument(args, "path", 1, 4096);
        number_argument(args, "offset", 0, std::numeric_limits<std::int64_t>::max()); number_argument(args, "max_bytes", 1, 65536);
    } else if (name == "fs_search") {
        allowed(args, {"path", "literal", "max_matches"}); text_argument(args, "path", 1, 4096);
        text_argument(args, "literal", 1, 4096, false, true); number_argument(args, "max_matches", 1, 100);
    } else if (name == "fs_write") {
        allowed(args, {"path", "content", "expected_sha256"}); text_argument(args, "path", 1, 4096);
        text_argument(args, "content", 0, 65536, false, true); hash_argument(args, "expected_sha256", true);
    } else if (name == "fs_patch") {
        allowed(args, {"path", "expected_sha256", "offset", "expected", "replacement"}); text_argument(args, "path", 1, 4096);
        hash_argument(args, "expected_sha256"); static_cast<void>(required(args, "offset"));
        number_argument(args, "offset", 0, std::numeric_limits<std::int64_t>::max());
        text_argument(args, "expected", 1, 65536, false, true); text_argument(args, "replacement", 0, 65536, false, true);
    } else if (name == "run") {
        allowed(args, {"argv", "cwd", "timeout_s", "max_output_bytes"}); const auto& argv = required(args, "argv");
        require(argv.is_array() && !argv.as_array().empty() && argv.as_array().size() <= 64, ErrorCode::InvalidInput, "run needs 1 to 64 argv strings");
        std::size_t total = 0;
        for (const auto& part : argv.as_array()) {
            const auto item = string(part); require(!item.empty() && item.size() <= 4096 && item.find('\0') == std::string::npos, ErrorCode::InvalidInput, "invalid argv string"); total += item.size();
        }
        require(total <= 32768, ErrorCode::InvalidInput, "argv byte capacity exceeded");
        text_argument(args, "cwd", 1, 4096, true); number_argument(args, "timeout_s", 1, 600); number_argument(args, "max_output_bytes", 1, 65536);
    } else if (name == "op_status" || name == "op_cancel" || name == "op_output") {
        if (name == "op_output") allowed(args, {"op_id", "stream", "offset", "limit"}); else allowed(args, {"op_id"});
        require(is_typed_id(field(args, "op_id"), "op"), ErrorCode::InvalidInput, "invalid operation id");
        if (name == "op_output") {
            const auto stream = field(args, "stream"); require(stream == "stdout" || stream == "stderr", ErrorCode::InvalidInput, "invalid output stream");
            number_argument(args, "offset", 0, std::numeric_limits<std::int64_t>::max()); number_argument(args, "limit", 1, 65536);
        }
    } else allowed(args, {});
}

SandboxPolicy worker_sandbox_policy(const FrozenRequestConfig& config, std::string_view template_id, const Json& manifest) {
    require(manifest.is_object(), ErrorCode::InvalidInput, "sandbox manifest must be an object");
    allowed(manifest, {"schema_version", "bwrap_path", "bwrap_sha256", "cgroup_root", "runtime_files", "memory_max_bytes", "pids_max", "cpu_quota_us", "cpu_period_us", "cleanup_timeout_ms", "max_output_bytes"});
    if (!manifest.as_object().empty()) require(unsigned_number(required(manifest, "schema_version")) == 1, ErrorCode::InvalidInput, "unsupported sandbox manifest version");
    const auto& frozen = config.json().at("system").at("sandbox");
    const auto& templates = config.json().at("function").at("team_templates");
    require(templates.is_object() && templates.as_object().contains(template_id), ErrorCode::InvalidInput, "unknown team template");
    const auto& selected = templates.at(template_id);
    require(field(selected, "network") == "none" && field(frozen, "network_default") == "none", ErrorCode::Unsupported, "this executor only implements network none");
    require(field(selected, "isolation") == "strong", ErrorCode::Unsupported, "this executor has no weak process mode");
    SandboxPolicy policy;
    policy.bwrap_path = optional_text(manifest, "bwrap_path", "/usr/bin/bwrap");
    if (manifest.as_object().contains("bwrap_sha256")) {
        hash_argument(manifest, "bwrap_sha256"); policy.expected_bwrap_sha256 = field(manifest, "bwrap_sha256");
    }
    policy.cgroup_root = optional_text(manifest, "cgroup_root", "");
    const auto memory_limit = memory_bytes(field(frozen, "team_memory_max"));
    policy.memory_max_bytes = optional_number(manifest, "memory_max_bytes", memory_limit);
    const auto pids_limit = unsigned_number(frozen.at("team_pids_max"));
    const auto pids = optional_number(manifest, "pids_max", pids_limit);
    require(policy.memory_max_bytes > 0 && policy.memory_max_bytes <= memory_limit && pids > 0 && pids <= pids_limit && pids <= 4096,
            ErrorCode::Unauthorized, "sandbox manifest exceeds frozen resource policy");
    policy.pids_max = static_cast<std::uint32_t>(pids);
    policy.cpu_quota_us = optional_number(manifest, "cpu_quota_us", 100000);
    policy.cpu_period_us = optional_number(manifest, "cpu_period_us", 100000);
    require(policy.cpu_period_us >= 1000 && policy.cpu_period_us <= 1000000 && policy.cpu_quota_us >= 1000 && policy.cpu_quota_us <= 1000000,
            ErrorCode::InvalidInput, "invalid bounded CPU quota");
    const auto cleanup = optional_number(manifest, "cleanup_timeout_ms", 2000);
    const auto output = optional_number(manifest, "max_output_bytes", 65536);
    require(cleanup >= 100 && cleanup <= 10000 && output >= 1 && output <= 65536, ErrorCode::InvalidInput, "invalid cleanup/output bound");
    policy.cleanup_timeout = std::chrono::milliseconds(cleanup);
    policy.max_output_bytes = static_cast<std::size_t>(output);
    policy.max_timeout = std::chrono::seconds(std::min<std::uint64_t>(600, unsigned_number(frozen.at("run_max_timeout_s"))));
    if (manifest.as_object().contains("runtime_files")) {
        const auto& files = manifest.at("runtime_files");
        require(files.is_array() && files.as_array().size() <= 128, ErrorCode::InvalidInput, "runtime files must be a bounded array");
        for (const auto& item : files.as_array()) {
            allowed(item, {"source", "destination", "sha256"});
            text_argument(item, "source", 1, 4096); text_argument(item, "destination", 1, 4096); hash_argument(item, "sha256");
            policy.runtime_files.push_back({field(item, "source"), field(item, "destination"), field(item, "sha256")});
        }
    }
    return policy;
}

Json worker_execution_policy(const FrozenRequestConfig& config, std::string_view template_id, const SandboxPolicy& policy) {
    Array files;
    for (const auto& item : policy.runtime_files) files.push_back(Object{{"source", item.source.string()}, {"destination", item.destination}, {"sha256", item.expected_sha256}});
    return Object{{"version", "wls-worker-execution-v1"}, {"config_snapshot_sha256", config.sha256()}, {"template_id", template_id},
        {"workspace_limits", limits_snapshot()}, {"network", "none"}, {"process_isolation", "strong"},
        {"bwrap_path", policy.bwrap_path.string()}, {"bwrap_sha256", policy.expected_bwrap_sha256},
        {"cgroup_root", policy.cgroup_root.string()}, {"runtime_files", files},
        {"memory_max_bytes", policy.memory_max_bytes}, {"pids_max", policy.pids_max}, {"cpu_quota_us", policy.cpu_quota_us},
        {"cpu_period_us", policy.cpu_period_us}, {"max_timeout_ms", policy.max_timeout.count()},
        {"cleanup_timeout_ms", policy.cleanup_timeout.count()}, {"max_output_bytes", policy.max_output_bytes}};
}

WorkerTools::WorkerTools(Store& store, const std::filesystem::path& root, Json manifest, bool read_only)
    : store_(store), workspace_(root, read_only ? WorkspaceAccess::ReadOnly : WorkspaceAccess::ReadWrite),
      manifest_(std::move(manifest)), read_only_(read_only) {}

void WorkerTools::check_actor(const StoreActor& actor) const {
    require(actor.role == Role::Worker, ErrorCode::Unauthorized, "only the Worker owns these tools");
    const auto current = store_.host_actor(Role::Worker, actor.request_id, actor.team_id);
    require(actor.task_id == current.task_id && actor.principal_id == current.principal_id && actor.request_id == current.request_id && actor.team_id == current.team_id,
            ErrorCode::Unauthorized, "Worker scope does not match the host identity");
    require(actor.dispatcher_epoch == current.dispatcher_epoch && actor.execution_epoch == current.execution_epoch,
            ErrorCode::Stale, "Worker identity belongs to a retired execution");
}

SandboxPolicy WorkerTools::policy_for(const StoreActor& actor) const {
    const auto config = restore_request_config(store_.request(actor.request_id));
    return worker_sandbox_policy(config, field(store_.task(actor.task_id), "template_id"), manifest_);
}

void WorkerTools::bind(const StoreActor& actor) {
    check_actor(actor);
    const auto identity = workspace_.root_identity();
    const auto config = restore_request_config(store_.request(actor.request_id));
    const auto template_id = field(store_.task(actor.task_id), "template_id");
    const auto policy = worker_execution_policy(config, template_id, policy_for(actor));
    if (!read_only_) static_cast<void>(store_.bind_workspace(actor.team_id, field(identity, "path"),
        unsigned_number(identity.at("device")), unsigned_number(identity.at("inode")), policy));
    verify_workspace(actor);
}

void WorkerTools::verify_workspace(const StoreActor& actor) const {
    check_actor(actor);
    const auto stored = store_.workspace(actor.team_id);
    require(stored.is_object(), ErrorCode::NotReady, "team has no bound workspace");
    const auto identity = workspace_.root_identity();
    require(stored.at("canonical_path") == identity.at("path") && stored.at("device") == identity.at("device") && stored.at("inode") == identity.at("inode"),
            ErrorCode::Conflict, "workspace path or directory identity changed");
    const auto config = restore_request_config(store_.request(actor.request_id));
    const auto policy = worker_execution_policy(config, field(store_.task(actor.task_id), "template_id"), policy_for(actor));
    require(stored.at("execution_policy") == policy, ErrorCode::Conflict, "team execution policy changed; explicit migration is required");
}

std::vector<ContextTool> WorkerTools::declarations(const StoreActor& actor) const {
    verify_workspace(actor);
    const auto grants = store_.authorized_worker_tools(actor, store_.binding(actor.task_id));
    std::vector<ContextTool> result;
    for (const auto& entry : worker_tool_registry()) if (std::find(grants.begin(), grants.end(), entry.name) != grants.end())
        result.push_back({entry.name, entry.version, entry.input_schema});
    return result;
}

Json WorkerTools::environment_card(const StoreActor& actor) const {
    verify_workspace(actor);
    const auto binding = store_.workspace(actor.team_id);
    return Object{{"file_executor", "dirfd-openat2"}, {"process_executor", "bwrap-cgroup-v2"}, {"logical_workspace", "/work"},
        {"environment_checks_executed", false}, {"process_isolation_verified", false},
        {"execution_policy_sha256", binding.at("execution_policy_sha256")}, {"note_is_business_delivery", false}};
}

void WorkerTools::rate_limit(const StoreActor& actor, const Json& allowance) {
    // Shared across executor instances in this process, bounded and independent
    // of business admission. Cross-process/global daemon quotas are future work.
    struct Window { std::chrono::steady_clock::time_point start; std::uint64_t count{}; };
    static std::mutex mutex;
    static std::map<std::string, Window> windows;
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard lock(mutex);
    for (auto i = windows.begin(); i != windows.end();) {
        if (now - i->second.start >= std::chrono::minutes(1)) i = windows.erase(i); else ++i;
    }
    require(windows.size() < 4096 || windows.contains(actor.team_id), ErrorCode::Quota, "diagnostic window capacity exhausted");
    auto [entry, created] = windows.try_emplace(actor.team_id, Window{now, 0});
    static_cast<void>(created);
    require(entry->second.count < unsigned_number(allowance.at("max_queries_per_minute")), ErrorCode::Quota, "diagnostic/control rate limit exhausted");
    ++entry->second.count;
}

Json WorkerTools::diagnostic(const StoreActor& actor, const ExecutionBinding& binding, const std::string& name, const Json& args) {
    validate_worker_tool_arguments(name, args);
    require(worker_tool_definition(name).category == ToolClass::Diagnostic, ErrorCode::Unauthorized, "only registered read-only diagnostics use this branch");
    const auto allowance = store_.authorize_diagnostic(actor, binding, name);
    rate_limit(actor, allowance); verify_workspace(actor);
    if (name == "fs_list") return workspace_.list(optional_text(args, "path", "."), optional_text(args, "after_name", ""), static_cast<std::size_t>(optional_number(args, "limit", 100)));
    if (name == "fs_read") return workspace_.read(field(args, "path"), optional_number(args, "offset", 0), static_cast<std::size_t>(optional_number(args, "max_bytes", 65536)));
    if (name == "fs_search") return workspace_.search(field(args, "path"), field(args, "literal"), static_cast<std::size_t>(optional_number(args, "max_matches", 100)));
    const auto id = field(args, "op_id");
    if (name == "op_output") return store_.operation_output(actor, binding, id, field(args, "stream"),
        static_cast<std::int64_t>(optional_number(args, "offset", 0)), static_cast<std::int64_t>(optional_number(args, "limit", 65536)));
    const auto operation = store_.operation(id);
    require(field(operation, "request_id") == actor.request_id && field(operation, "task_id") == actor.task_id && field(operation, "team_id") == actor.team_id,
            ErrorCode::Unauthorized, "operation belongs to another team");
    return Object{{"operation", operation}, {"execution_result", store_.operation_result(id)}};
}

Json WorkerTools::control(const StoreActor& actor, const ExecutionBinding& binding, std::int64_t expected,
        const std::string& name, const Json& args, const std::string& command) {
    require(!read_only_, ErrorCode::Unauthorized, "read-only executor cannot change control state");
    // Current permission controls access, while Store independently recognizes
    // an exact old command before admitting a new control transition. Controls
    // do not require a usable filesystem path to stop or acknowledge work.
    const auto current = store_.host_actor(Role::Worker, actor.request_id, actor.team_id);
    require(actor.role == Role::Worker && actor.principal_id == current.principal_id && actor.task_id == current.task_id,
        ErrorCode::Unauthorized, "control belongs to another Worker");
    const auto allowance = store_.authorize_control(current, store_.binding(current.task_id), name);
    rate_limit(current, allowance);
    const auto result = store_.execute_tool_control(actor, binding, expected, name, args, command);
    if (name == "ack_change") return result;
    const auto id = field(args, "op_id");
    auto& active = active_runs();
    const std::lock_guard lock(active.mutex);
    if (const auto entry = active.operations.find({&store_, id}); entry != active.operations.end()) entry->second.request_stop();
    return result;
}

Json WorkerTools::execute(const StoreActor& actor, const ExecutionBinding& binding, std::int64_t expected,
        const std::string& key, const std::string& name, const Json& args, const std::string& command, std::stop_token stop) {
    validate_worker_tool_arguments(name, args);
    const auto& definition = worker_tool_definition(name);
    if (definition.category == ToolClass::Diagnostic) return diagnostic(actor, binding, name, args);
    if (definition.category == ToolClass::Control) return control(actor, binding, expected, name, args, command);
    require(!read_only_, ErrorCode::Unauthorized, "read-only executor cannot execute a business tool");
    verify_workspace(actor);
    const auto old = store_.operation_for_key(actor, binding, key, name, args);
    if (!old.is_null()) {
        const auto result = store_.operation_result(field(old, "op_id"));
        return Object{{"operation", old}, {"execution_result", result}, {"replayed", true}, {"executed_now", false}};
    }
    {
        auto& active = active_runs();
        const std::lock_guard lock(active.mutex);
        require(!active.settlement_capacity_exhausted && !active.failed_settlements.contains({actor.team_id, actor.execution_epoch}),
            ErrorCode::NotReady, "an execution result could not be persisted; recover and reconcile before new business");
    }
    std::optional<ProcessRequest> process_request;
    std::optional<ProcessRunner> process_runner;
    if (name == "run") {
        const auto policy = policy_for(actor);
        ProcessRequest request;
        for (const auto& value : args.at("argv").as_array()) request.argv.push_back(string(value));
        request.cwd = optional_text(args, "cwd", ".");
        request.env = {{"PATH", "/usr/bin:/bin"}, {"LANG", "C.UTF-8"}};
        const auto config = restore_request_config(store_.request(actor.request_id));
        const auto timeout = optional_number(args, "timeout_s", unsigned_number(config.json().at("system").at("sandbox").at("run_default_timeout_s")));
        request.timeout = std::chrono::seconds(timeout);
        request.output_limit_bytes = static_cast<std::size_t>(optional_number(args, "max_output_bytes", policy.max_output_bytes));
        require(request.timeout <= policy.max_timeout && request.output_limit_bytes <= policy.max_output_bytes,
            ErrorCode::Unauthorized, "run exceeds frozen host limits");
        struct Fd { int value; ~Fd() { ::close(value); } } root{workspace_.dup_root_fd()};
        process_runner.emplace(root.value, policy);
        process_request = std::move(request);
    }
    const auto prepared = store_.prepare_tool_operation(actor, binding, expected, key, name, args, command + ":prepare");
#ifdef WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE
    // The regression TU alone supplies this phase observer; normal library
    // compilation has no callback or test entry point.
    WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE(key, prepared);
#endif
    const auto id = field(prepared, "op_id");
    std::stop_source cancel;
    auto& active = active_runs();
    const auto active_key = std::make_pair(static_cast<const Store*>(&store_), id);
    bool inserted = false;
    {
        const std::lock_guard lock(active.mutex);
        inserted = active.operations.emplace(active_key, cancel).second;
    }
    if (!inserted) {
        return Object{{"operation", store_.operation(id)}, {"execution_result", store_.operation_result(id)},
            {"replayed", true}, {"executed_now", false}};
    }
    struct Remove {
        ActiveRuns& active; const std::pair<const Store*, std::string>& key;
        ~Remove() { const std::lock_guard lock(active.mutex); active.operations.erase(key); }
    } remove{active, active_key};
    std::stop_callback stop_callback(stop, [&] { cancel.request_stop(); });
    Json claim;
    {
        // Settlement and first dispatch share this admission lock. A call that
        // passed the early check before another result failed to persist must
        // not acquire a new dispatch claim after that failure is registered.
        const std::lock_guard lock(active.mutex);
        require(!active.settlement_capacity_exhausted && !active.failed_settlements.contains({actor.team_id, actor.execution_epoch}),
            ErrorCode::NotReady, "an execution result could not be persisted; recover and reconcile before new business");
        claim = store_.start_tool_operation(actor, id, signed_number(prepared.at("state_version")), command + ":start");
    }
    if (!claim.at("dispatch_claimed").as_bool())
        return Object{{"operation", claim.at("operation")}, {"execution_result", store_.operation_result(id)},
            {"replayed", true}, {"executed_now", false}};
    Json result = Object{};
    Json evidence = Object{};
    std::string outcome = "FAILED", stdout_bytes, stderr_bytes;
    bool output_truncated = false;
    try {
        if (cancel.stop_requested()) {
            result = Object{{"error", "cancelled_before_effect"}}; evidence = Object{{"effect_started", false}};
        } else if (name == "note") {
            result = args; evidence = Object{{"kind", "sqlite_note"}, {"sha256", sha256_hex(canonical_json(args))}}; outcome = "SUCCEEDED";
        } else if (name == "fs_write" || name == "fs_patch") {
            WorkspaceMutationResult mutation;
            if (name == "fs_write") {
                std::optional<std::string> hash;
                if (args.as_object().contains("expected_sha256")) hash = field(args, "expected_sha256");
                mutation = workspace_.write_atomic(field(args, "path"), field(args, "content"), hash);
            } else mutation = workspace_.patch(field(args, "path"), field(args, "expected_sha256"), unsigned_number(args.at("offset")), field(args, "expected"), field(args, "replacement"));
            result = mutation.result; evidence = mutation.evidence;
            if (mutation.status == WorkspaceMutationStatus::Succeeded) outcome = "SUCCEEDED";
            else if (mutation.status == WorkspaceMutationStatus::OutcomeUnknown) outcome = "UNKNOWN";
        } else {
            process_request->operation_id = id;
            const auto process = process_runner->run(*process_request, cancel.get_token());
            stdout_bytes = process.stdout_text; stderr_bytes = process.stderr_text; output_truncated = process.output_truncated;
            result = to_json(process); evidence = process.evidence;
            // Raw bytes have their own BLOB storage and bounded paging. JSON
            // escaping of 64 KiB control bytes can exceed the result limit.
            for (const auto* output_key : {"stdout_text", "stderr_text", "stdout_base64", "stderr_base64"}) result.as_object().erase(output_key);
            result.as_object()["stdout_bytes"] = stdout_bytes.size();
            result.as_object()["stderr_bytes"] = stderr_bytes.size();
            result.as_object()["output_location"] = "op_output";
            // Failed commands may have known partial /work effects. Only a
            // complete strong/exit/inventory proof permits definite settlement.
            outcome = process_operation_outcome(process);
        }
    } catch (const Error& error) {
        result = Object{{"error", error_name(error.code())}, {"message", error.what()}};
        evidence = Object{{"kind", "executor_error"}, {"effect_outcome", "not_confirmed"}};
        // Workspace returns effect-aware failures; an unexpected exception after
        // dispatch cannot establish that no external change took place.
        outcome = "UNKNOWN";
    } catch (const std::exception& error) {
        result = Object{{"error", "executor_error"}, {"message", error.what()}};
        evidence = Object{{"effect_outcome", "not_confirmed"}}; outcome = "UNKNOWN";
    }
    Json recorded;
    bool settlement_failed = false;
    {
        const std::lock_guard lock(active.mutex);
        try {
            recorded = store_.record_operation_result(id, binding, outcome, result, evidence, stdout_bytes, stderr_bytes,
                output_truncated, output_truncated);
        } catch (...) {
            // Register the failure before another caller can receive a first
            // dispatch claim, even if all subsequent Store writes also fail.
            settlement_failed = true;
            if (active.failed_settlements.size() >= 4096) active.settlement_capacity_exhausted = true;
            else {
                try { active.failed_settlements.emplace(actor.team_id, actor.execution_epoch); }
                catch (...) { active.settlement_capacity_exhausted = true; }
            }
        }
    }
    if (settlement_failed) {
        // An effect followed by a storage failure must not leave a usable
        // RUNNING row as implicit permission for more work. The small fallback
        // facts are best effort; even a wholly unavailable Store fails closed
        // in every WorkerTools sharing this process and execution epoch.
        try {
            const auto existing = store_.operation_result(id);
            if (existing.is_null()) {
                static_cast<void>(store_.record_operation_result(id, binding, "UNKNOWN",
                    Object{{"error", "execution_result_persistence_failed"}, {"observed_outcome", outcome}},
                    Object{{"kind", "settlement_failure"}, {"original_result_sha256", sha256_hex(canonical_json(result))}},
                    stdout_bytes, stderr_bytes, output_truncated, output_truncated));
            }
        } catch (...) {
            try {
                const auto operation = store_.operation(id);
                if (field(operation, "status") == "RUNNING")
                    static_cast<void>(store_.finish_operation(actor, id, binding, signed_number(operation.at("state_version")),
                        "UNKNOWN", "execution_result_persistence_failed", command + ":settlement-failed"));
            } catch (...) { /* The process-local fault remains; reopening fences unfinished work. */ }
        }
        throw Error(ErrorCode::Storage, "execution result persistence failed for " + id + "; new business is blocked pending recovery");
    }
    return Object{{"operation", store_.operation(id)}, {"execution_result", recorded}, {"replayed", false}, {"executed_now", true}};
}

} // namespace wls::s1
