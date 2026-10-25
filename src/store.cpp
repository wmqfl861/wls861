#include "wls/store.hpp"
#include "wls/capacity.hpp"
#include "wls/config_loader.hpp"
#include "wls/model.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wls::s1 {
namespace {
using Row = boost::json::object;
using Array = boost::json::array;

[[noreturn]] void fail(ErrorCode code, const std::string& message) {
    throw Error(code, message);
}
void require(bool condition, ErrorCode code, const std::string& message) {
    if (!condition) fail(code, message);
}
void text_field(std::string_view value, std::string_view field, std::size_t maximum = 256) {
    require(!value.empty() && value.size() <= maximum && value.find('\0') == std::string_view::npos,
            ErrorCode::InvalidInput, "invalid " + std::string(field));
}
std::string str(const Row& row, std::string_view key) {
    const auto& v = row.at(key);
    require(v.is_string(), ErrorCode::Corrupt, "expected text field " + std::string(key));
    return std::string(v.as_string());
}
std::int64_t number(const Row& row, std::string_view key) {
    const auto& v = row.at(key);
    if (v.is_int64()) return v.as_int64();
    if (v.is_uint64() && v.as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(v.as_uint64());
    fail(ErrorCode::Corrupt, "expected integer field " + std::string(key));
}
std::int64_t next(std::int64_t n) {
    require(n >= 0 && n < INT64_MAX, ErrorCode::Quota, "revision exhausted");
    return n + 1;
}
std::int64_t add(std::int64_t a, std::int64_t b) {
    require(a >= 0 && b >= 0 && b <= INT64_MAX - a, ErrorCode::Quota, "counter overflow");
    return a + b;
}
bool terminal_request(std::string_view s) {
    return s == "DELIVERED" || s == "FAILED" || s == "CANCELLED" || s == "SUPERSEDED";
}
bool terminal_task(std::string_view s) {
    return s == "ACCEPTED" || s == "FAILED" || s == "CANCELLED" || s == "SKIPPED" || s == "SUPERSEDED";
}
std::string stamp() {
    const auto value = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    require(::gmtime_r(&value, &utc) != nullptr, ErrorCode::Storage, "clock conversion failed");
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}
Json actor_json(const StoreActor& actor) {
    return Row{{"role", role_name(actor.role)}, {"principal_id", actor.principal_id},
        {"request_id", actor.request_id}, {"task_id", actor.task_id}, {"team_id", actor.team_id},
        {"dispatcher_epoch", actor.dispatcher_epoch}, {"execution_epoch", actor.execution_epoch}};
}
Row payload(const StoreActor& actor, std::string_view type, Row fields = {}) {
    fields["type"] = type;
    fields["actor"] = actor_json(actor);
    return fields;
}
Json exposed(Row row) {
    for (const auto* key : {"config_snapshot", "budget", "usage", "tool_allowlist", "denied_tools",
             "body", "brief", "work_order", "lead_config", "worker_config", "frozen_binding",
             "response", "request_envelope", "arguments", "payload", "result", "source_snapshot", "manifest_snapshot",
             "original_binding", "evidence", "execution_policy", "captured_ids", "captured_binding",
             "artifact_snapshot", "check_body", "rework_order", "producer_binding", "decision",
             "frozen_plan", "acceptance_snapshot", "publication"}) {
        if (auto* value = row.if_contains(key); value && value->is_string())
            *value = parse_json_strict(value->as_string(), 16 * 1024 * 1024);
    }
    return row;
}
std::string json_text(const Json& value, std::size_t maximum = 1024 * 1024) {
    const auto encoded = canonical_json(value);
    require(encoded.size() <= maximum, ErrorCode::Quota, "JSON record exceeds byte limit");
    // Objects assembled by trusted C++ callers still must contain valid JSON
    // strings and finite values before their canonical bytes become durable.
    (void)parse_json_strict(encoded, maximum);
    return encoded;
}
Array strings(const std::vector<std::string>& values) {
    Array result;
    std::set<std::string> unique;
    for (const auto& value : values) {
        text_field(value, "tool identifier");
        require(unique.insert(value).second, ErrorCode::InvalidInput, "duplicate tool grant");
    }
    for (const auto& value : unique) result.emplace_back(value);
    return result;
}
bool contains(const Json& list, std::string_view value) {
    require(list.is_array(), ErrorCode::Corrupt, "expected string array");
    return std::any_of(list.as_array().begin(), list.as_array().end(), [&](const Json& item) {
        return item.is_string() && item.as_string() == value;
    });
}
std::string tool_side_effect(std::string_view tool_id) {
    if (tool_id == "fs_write" || tool_id == "fs_patch" || tool_id == "run") return "workspace";
    if (tool_id == "note") return "none";
    fail(ErrorCode::Unsupported, "tool is not a registered S1.2 business implementation");
}
bool diagnostic_tool(std::string_view tool_id) {
    return tool_id == "fs_list" || tool_id == "fs_read" || tool_id == "fs_search" ||
        tool_id == "op_status" || tool_id == "op_output";
}
std::string base64_bytes(std::string_view value) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((value.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < value.size(); i += 3) {
        const auto first = static_cast<unsigned char>(value[i]);
        const auto second = i + 1 < value.size() ? static_cast<unsigned char>(value[i + 1]) : 0U;
        const auto third = i + 2 < value.size() ? static_cast<unsigned char>(value[i + 2]) : 0U;
        const auto word = (static_cast<unsigned int>(first) << 16U) | (second << 8U) | third;
        result.push_back(alphabet[(word >> 18U) & 63U]);
        result.push_back(alphabet[(word >> 12U) & 63U]);
        result.push_back(i + 1 < value.size() ? alphabet[(word >> 6U) & 63U] : '=');
        result.push_back(i + 2 < value.size() ? alphabet[word & 63U] : '=');
    }
    return result;
}
std::string decoded_base64(std::string_view encoded) {
    require(encoded.size() <= 87384 && encoded.size() % 4 == 0,
            ErrorCode::InvalidInput, "invalid bounded snapshot base64");
    const auto digit = [](char c) -> unsigned int {
        if (c >= 'A' && c <= 'Z') return static_cast<unsigned int>(c - 'A');
        if (c >= 'a' && c <= 'z') return static_cast<unsigned int>(c - 'a' + 26);
        if (c >= '0' && c <= '9') return static_cast<unsigned int>(c - '0' + 52);
        if (c == '+') return 62;
        if (c == '/') return 63;
        fail(ErrorCode::InvalidInput, "snapshot base64 contains an invalid character");
    };
    std::string result;
    result.reserve(encoded.size() / 4 * 3);
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        const auto first = digit(encoded[i]); const auto second = digit(encoded[i + 1]);
        const bool third_padding = encoded[i + 2] == '=';
        const bool fourth_padding = encoded[i + 3] == '=';
        require((!third_padding || fourth_padding) &&
                ((!third_padding && !fourth_padding) || i + 4 == encoded.size()),
                ErrorCode::InvalidInput, "snapshot base64 has invalid padding");
        const auto third = third_padding ? 0U : digit(encoded[i + 2]);
        const auto fourth = fourth_padding ? 0U : digit(encoded[i + 3]);
        const auto word = (first << 18U) | (second << 12U) | (third << 6U) | fourth;
        result.push_back(static_cast<char>((word >> 16U) & 255U));
        if (!third_padding) result.push_back(static_cast<char>((word >> 8U) & 255U));
        if (!fourth_padding) result.push_back(static_cast<char>(word & 255U));
    }
    require(result.size() <= 65536 && base64_bytes(result) == encoded,
            ErrorCode::InvalidInput, "snapshot requires canonical base64 and at most 64 KiB");
    return result;
}
void object_fields(const Json& value, std::initializer_list<std::string_view> allowed,
                   std::initializer_list<std::string_view> required = {}) {
    require(value.is_object(), ErrorCode::InvalidInput, "workflow payload must be an object");
    for (const auto& item : value.as_object())
        require(std::find(allowed.begin(), allowed.end(), std::string_view(item.key())) != allowed.end(),
                ErrorCode::InvalidInput, "unsupported workflow field " + std::string(item.key()));
    for (const auto key : required)
        require(value.as_object().contains(key), ErrorCode::InvalidInput, "missing workflow field " + std::string(key));
}
void nonblank(std::string_view value, std::string_view field, std::size_t maximum) {
    text_field(value, field, maximum);
    require(value.find_first_not_of(" \t\r\n") != std::string_view::npos, ErrorCode::InvalidInput,
            "blank " + std::string(field));
}
std::string workflow_text(const Json& value, std::string_view key, std::size_t maximum,
                          bool optional = false) {
    const auto* field = value.as_object().if_contains(key);
    if (!field && optional) return {};
    require(field && field->is_string(), ErrorCode::InvalidInput, "workflow text field " + std::string(key));
    const auto text = std::string(field->as_string()); nonblank(text, key, maximum); return text;
}
bool semantic_binding_equal(ExecutionBinding left, ExecutionBinding right, bool current_policy_control = false) {
    left.dispatcher_epoch = right.dispatcher_epoch;
    left.execution_epoch = right.execution_epoch;
    if (current_policy_control) left.policy_epoch = right.policy_epoch;
    return left == right;
}
Json model_contract(const ModelRequest& request) {
    return parse_json_strict(compiled_task_contract(request.context), 1024U * 1024U);
}
bool overlap(const std::string& left, const std::string& right) {
    return left == right || left.starts_with(right + "/") || right.starts_with(left + "/");
}
void work_path(std::string_view path) {
    text_field(path, "submission path", 500);
    require(path.starts_with("/work/") && path.size() > 6 && path.back() != '/',
            ErrorCode::InvalidInput, "submission path must name a file under /work");
    std::size_t start = 6;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto segment = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        require(!segment.empty() && segment != "." && segment != ".." &&
                std::none_of(segment.begin(), segment.end(), [](unsigned char c) { return c < 32 || c == 127; }),
                ErrorCode::InvalidInput, "submission path is not a canonical workspace path");
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
}
std::set<std::string> acceptance_ids(const Json& brief) {
    require(brief.is_object() && brief.as_object().contains("acceptance_criteria") && brief.at("acceptance_criteria").is_array() &&
            !brief.at("acceptance_criteria").as_array().empty() && brief.at("acceptance_criteria").as_array().size() <= 30,
            ErrorCode::InvalidInput, "workflow requires the current explicit acceptance criteria");
    std::set<std::string> result;
    for (const auto& item : brief.at("acceptance_criteria").as_array()) {
        require(item.is_object(), ErrorCode::InvalidInput, "acceptance criterion must be an object");
        const auto id = workflow_text(item, "id", 8);
        require(id.starts_with("ac") && id.size() >= 3 && id.size() <= 5 &&
                std::all_of(id.begin() + 2, id.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
                result.insert(id).second, ErrorCode::InvalidInput, "acceptance criterion IDs must be valid and unique");
    }
    return result;
}
void criterion_results(const Json& results, const std::set<std::string>& expected,
                       bool passing, bool self_check) {
    require(results.is_array() && !results.as_array().empty() && results.as_array().size() <= 30,
            ErrorCode::InvalidInput, "criterion results must cover the current criteria");
    std::set<std::string> seen;
    for (const auto& result : results.as_array()) {
        if (self_check) object_fields(result, {"criterion_id", "met", "evidence"}, {"criterion_id", "met"});
        else object_fields(result, {"criterion_id", "met", "note"}, {"criterion_id", "met"});
        const auto id = workflow_text(result, "criterion_id", 8);
        const auto met = workflow_text(result, "met", 8);
        require(expected.contains(id) && seen.insert(id).second && (met == "yes" || met == "no" || met == "partial"),
                ErrorCode::InvalidInput, "criterion results contain missing, foreign or duplicate criteria");
        if (passing) require(met == "yes", ErrorCode::InvalidInput, "passed check contradicts an unmet criterion");
        if (self_check) (void)workflow_text(result, "evidence", 8000, !passing);
        else (void)workflow_text(result, "note", 4000, true);
    }
    require(seen == expected, ErrorCode::InvalidInput, "criterion results omit current acceptance criteria");
}
void submission_verification(const Json& body, bool passing) {
    if (const auto* records = body.as_object().if_contains("verification")) {
        require(records->is_array() && records->as_array().size() <= 50, ErrorCode::InvalidInput, "verification records exceed capacity");
        for (const auto& record : records->as_array()) {
            object_fields(record, {"check", "method", "result", "evidence"}, {"check", "method", "result"});
            (void)workflow_text(record, "check", 2000); (void)workflow_text(record, "method", 4000);
            const auto result = workflow_text(record, "result", 8);
            require(result == "pass" || result == "fail" || result == "not_run", ErrorCode::InvalidInput, "invalid verification result");
            if (passing) require(result == "pass", ErrorCode::InvalidInput, "passed check contradicts failed or unrun verification");
            (void)workflow_text(record, "evidence", 8000, !passing);
        }
    }
    if (const auto* issues = body.as_object().if_contains("known_issues")) {
        require(issues->is_array() && issues->as_array().size() <= 30, ErrorCode::InvalidInput, "known issues exceed capacity");
        for (const auto& issue : issues->as_array()) {
            require(issue.is_string(), ErrorCode::InvalidInput, "known issue must be text");
            nonblank(issue.as_string(), "known issue", 4000);
        }
        if (passing) require(issues->as_array().empty(), ErrorCode::InvalidInput, "known issues require a later explicit acceptance policy");
    }
}
void work_order_shape(const Json& body) {
    object_fields(body, {"approach", "steps", "constraints_verbatim", "done_definition", "pitfalls", "notes", "constraint_refs"},
                  {"approach", "steps", "constraints_verbatim", "done_definition", "constraint_refs"});
    (void)workflow_text(body, "approach", 8000); (void)workflow_text(body, "done_definition", 8000); (void)workflow_text(body, "notes", 8000, true);
    require(body.at("steps").is_array() && !body.at("steps").as_array().empty() && body.at("steps").as_array().size() <= 40,
            ErrorCode::InvalidInput, "rework order requires bounded concrete steps");
    std::set<std::string> ids;
    for (const auto& step : body.at("steps").as_array()) {
        object_fields(step, {"id", "text", "check"}, {"id", "text"});
        const auto id = workflow_text(step, "id", 8);
        require(id.starts_with('s') && id.size() >= 2 && id.size() <= 4 &&
                std::all_of(id.begin() + 1, id.end(), [](char c) { return c >= '0' && c <= '9'; }) && ids.insert(id).second,
                ErrorCode::InvalidInput, "rework step IDs must be valid and unique");
        (void)workflow_text(step, "text", 4000); (void)workflow_text(step, "check", 2000, true);
    }
    if (const auto* pitfalls = body.as_object().if_contains("pitfalls")) {
        require(pitfalls->is_array() && pitfalls->as_array().size() <= 20, ErrorCode::InvalidInput, "too many rework pitfalls");
        for (const auto& value : pitfalls->as_array()) {
            require(value.is_string(), ErrorCode::InvalidInput, "rework pitfall must be text"); nonblank(value.as_string(), "rework pitfall", 2000);
        }
    }
    (void)json_text(body, 262144);
}
std::string unhex(std::string_view value) {
    require(value.size() % 2 == 0, ErrorCode::Corrupt, "invalid stored output hex");
    const auto digit = [](char c) -> unsigned int {
        if (c >= '0' && c <= '9') return static_cast<unsigned int>(c - '0');
        if (c >= 'A' && c <= 'F') return static_cast<unsigned int>(c - 'A' + 10);
        fail(ErrorCode::Corrupt, "invalid stored output hex digit");
    };
    std::string result;
    result.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2)
        result.push_back(static_cast<char>((digit(value[i]) << 4U) | digit(value[i + 1])));
    return result;
}
void sqlite_failure(sqlite3* db, int result, std::string_view operation) {
    if (result == SQLITE_OK || result == SQLITE_ROW || result == SQLITE_DONE) return;
    const auto primary = result & 255;
    const auto code = primary == SQLITE_CONSTRAINT ? ErrorCode::Conflict :
        primary == SQLITE_BUSY || primary == SQLITE_LOCKED ? ErrorCode::Conflict :
        primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB ? ErrorCode::Corrupt : ErrorCode::Storage;
    fail(code, std::string(operation) + ": " + (db ? sqlite3_errmsg(db) : sqlite3_errstr(result)));
}
Row reconciliation_command(const StoreActor& root, const std::string& id, std::int64_t expected,
                           const std::string& outcome, const std::string& evidence, const std::string& command_id) {
    require(root.role == Role::Root, ErrorCode::Unauthorized, "only the authenticated Root may reconcile an operation");
    require(expected > 0 && (outcome == "SUCCEEDED" || outcome == "FAILED" || outcome == "CANCELLED" || outcome == "TIMED_OUT"),
        ErrorCode::InvalidInput, "reconciliation requires a positive CAS and a definite outcome");
    text_field(command_id, "reconciliation key"); text_field(id, "operation id"); text_field(evidence, "host evidence", 4096);
    require(evidence.find_first_not_of(" \t\r\n") != std::string::npos, ErrorCode::InvalidInput, "host evidence cannot be blank");
    // Stable authenticated owner + explicit op CAS, not the current ephemeral
    // Dispatcher epoch: exact CLI retries must work after a writer reopen.
    const Row command{{"type", "reconcile_op"}, {"root_id", root.principal_id}, {"request_id", root.request_id},
        {"op_id", id}, {"expected_op_version", expected}, {"outcome", outcome}, {"host_evidence", evidence}};
    (void)canonical_json(command); // Validate UTF-8 before taking over a writer.
    return command;
}
class Statement {
public:
    Statement(sqlite3* db, std::string_view sql, const Array& parameters = {}) : db_(db) {
        require(sql.size() <= static_cast<std::size_t>(INT_MAX), ErrorCode::Storage, "SQL too large");
        sqlite_failure(db_, sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr), "prepare");
        try {
            require(sqlite3_bind_parameter_count(statement_) == static_cast<int>(parameters.size()), ErrorCode::Storage, "SQL parameter count");
            for (std::size_t i = 0; i < parameters.size(); ++i) {
                const auto index = static_cast<int>(i + 1);
                const auto& v = parameters[i];
                int result{};
                if (v.is_null()) result = sqlite3_bind_null(statement_, index);
                else if (v.is_int64()) result = sqlite3_bind_int64(statement_, index, v.as_int64());
                else if (v.is_bool()) result = sqlite3_bind_int(statement_, index, v.as_bool() ? 1 : 0);
                else if (v.is_string()) {
                    const auto& s = v.as_string();
                    require(s.size() <= static_cast<std::size_t>(INT_MAX), ErrorCode::Quota, "SQL text too large");
                    result = sqlite3_bind_text(statement_, index, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
                } else fail(ErrorCode::Storage, "SQL parameters require bounded scalar values");
                sqlite_failure(db_, result, "bind");
            }
        } catch (...) { sqlite3_finalize(statement_); throw; }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    bool step() {
        const auto result = sqlite3_step(statement_);
        sqlite_failure(db_, result, "step");
        return result == SQLITE_ROW;
    }
    Row row() const {
        Row result;
        for (int i = 0; i < sqlite3_column_count(statement_); ++i) {
            const auto* name = sqlite3_column_name(statement_, i);
            switch (sqlite3_column_type(statement_, i)) {
                case SQLITE_NULL: result[name] = nullptr; break;
                case SQLITE_INTEGER: result[name] = static_cast<std::int64_t>(sqlite3_column_int64(statement_, i)); break;
                case SQLITE_TEXT: {
                    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement_, i));
                    result[name] = std::string(bytes, static_cast<std::size_t>(sqlite3_column_bytes(statement_, i)));
                    break;
                }
                default: fail(ErrorCode::Corrupt, "unexpected SQLite value type");
            }
        }
        return result;
    }
private:
    sqlite3* db_{};
    sqlite3_stmt* statement_{};
};
}

class Store::Impl {
public:
    Impl(const std::filesystem::path& database_path, std::string_view base_sql,
         std::string_view migration_sql, StoreOpenMode selected_mode,
         std::string_view tools_migration_sql, std::string_view events_migration_sql,
         std::string_view workflow_migration_sql, std::string_view acceptance_migration_sql,
         std::string_view cleanup_migration_sql) : mode(selected_mode) {
        require(events_migration_sql.empty() || !tools_migration_sql.empty(), ErrorCode::InvalidInput,
                "event-runtime migration requires the worker-tools asset");
        require(workflow_migration_sql.empty() || !events_migration_sql.empty(), ErrorCode::InvalidInput,
                "workflow migration requires the event-runtime asset");
        require(acceptance_migration_sql.empty() || !workflow_migration_sql.empty(), ErrorCode::InvalidInput,
                "acceptance migration requires the workflow asset");
        require(cleanup_migration_sql.empty() || !acceptance_migration_sql.empty(), ErrorCode::InvalidInput,
                "owned cleanup migration requires the acceptance asset");
        require(database_path != ":memory:", ErrorCode::InvalidInput, "Store requires a durable file path");
        const auto absolute = std::filesystem::absolute(database_path).lexically_normal();
        const auto path = (std::filesystem::canonical(absolute.parent_path()) / absolute.filename()).string();
        text_field(path, "database path", 4096);
        database_file_path = path;
        try {
            if (mode == StoreOpenMode::Takeover) {
                const auto lock_path = path + ".writer-lock";
                lease_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
                require(lease_fd >= 0, ErrorCode::Storage, "cannot open Store writer lock: " + std::string(std::strerror(errno)));
                struct stat info{};
                require(::fstat(lease_fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
                        info.st_uid == ::geteuid() && ::fchmod(lease_fd, 0600) == 0,
                        ErrorCode::Storage, "Store lock must be a private regular file with one link");
                require(::flock(lease_fd, LOCK_EX | LOCK_NB) == 0, ErrorCode::Conflict, "another Store writer owns this database");
                const auto database_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
                require(database_fd >= 0, ErrorCode::Storage, "cannot create private SQLite file");
                const bool private_file = ::fstat(database_fd, &info) == 0 && S_ISREG(info.st_mode) &&
                    info.st_nlink == 1 && info.st_uid == ::geteuid() && ::fchmod(database_fd, 0600) == 0;
                ::close(database_fd);
                require(private_file, ErrorCode::Storage, "SQLite database must be a private regular file with one link");
            }
            const int flags = (mode == StoreOpenMode::ReadOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
                | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
            sqlite_failure(nullptr, sqlite3_open_v2(path.c_str(), &db, flags, nullptr), "open SQLite");
            sqlite3_extended_result_codes(db, 1);
            sqlite_failure(db, sqlite3_busy_timeout(db, 5000), "busy timeout");
            sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 16 * 1024 * 1024);
            sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, 1024 * 1024);
            exec("PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
            require(sqlite3_libversion_number() >= 3037000, ErrorCode::Unsupported, "SQLite 3.37+ required");
            auto version = number(one("PRAGMA user_version"), "user_version");
            if (version == 0 && mode == StoreOpenMode::Takeover) {
                require(!optional("SELECT name FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' LIMIT 1"), ErrorCode::Unsupported,
                        "refusing to initialize an existing unknown database");
                require(!base_sql.empty(), ErrorCode::InvalidInput, "base schema asset required");
                exec(base_sql);
                version = number(one("PRAGMA user_version"), "user_version");
            }
            require(version >= 401 && version <= 407, ErrorCode::Unsupported, "unknown database version; explicit migration required");
            require(str(one("SELECT schema_version FROM contract_metadata WHERE singleton=1"), "schema_version") == "0.4.1",
                    ErrorCode::Unsupported, "unsupported base contract metadata");
            const auto migration_hash = sha256_hex(migration_sql);
            const auto tools_migration_hash = sha256_hex(tools_migration_sql);
            const auto events_migration_hash = sha256_hex(events_migration_sql);
            const auto workflow_migration_hash = sha256_hex(workflow_migration_sql);
            const auto acceptance_migration_hash = sha256_hex(acceptance_migration_sql);
            if (version == 401) {
                require(mode == StoreOpenMode::Takeover, ErrorCode::Unsupported, "database needs Store migration before read-only inspection");
                // Python reference rows did not capture a complete configuration.
                // Their original data must not acquire invented frozen settings.
                require(!optional("SELECT request_id FROM requests LIMIT 1"), ErrorCode::Unsupported,
                        "v0.4.1 reference database contains requests without full snapshots; explicit data migration required");
            }
            if (version >= 402) {
                const auto metadata = one("SELECT * FROM store_runtime_metadata WHERE singleton=1");
                require(number(metadata, "store_version") == 4 && str(metadata, "migration_sha256") == migration_hash,
                        ErrorCode::Corrupt, "Store migration asset does not match durable metadata");
            }
            if (version >= 403) {
                require(!tools_migration_sql.empty(), ErrorCode::Unsupported, "403 database requires the distributed worker-tools migration asset");
                const auto metadata = one("SELECT * FROM worker_tools_metadata WHERE singleton=1");
                require(number(metadata, "store_version") == 5 && str(metadata, "migration_sha256") == tools_migration_hash,
                        ErrorCode::Corrupt, "worker-tools migration asset does not match durable metadata");
            }
            if (version >= 404) {
                require(!events_migration_sql.empty(), ErrorCode::Unsupported, "404 database requires the distributed event-runtime migration asset");
                const auto event_metadata = one("SELECT * FROM event_runtime_metadata WHERE singleton=1");
                require(number(event_metadata, "store_version") == 6 && str(event_metadata, "migration_sha256") == events_migration_hash,
                        ErrorCode::Corrupt, "event-runtime migration asset does not match durable metadata");
            }
            if (version >= 405) {
                require(!workflow_migration_sql.empty(), ErrorCode::Unsupported,
                        "405 database requires the distributed workflow migration asset");
                const auto workflow_metadata = one("SELECT * FROM workflow_runtime_metadata WHERE singleton=1");
                require(number(workflow_metadata, "store_version") == 7 &&
                        str(workflow_metadata, "migration_sha256") == workflow_migration_hash,
                        ErrorCode::Corrupt, "workflow migration asset does not match durable metadata");
            }
            if (version >= 406) {
                require(!acceptance_migration_sql.empty(), ErrorCode::Unsupported, "406 database requires the acceptance migration asset");
                const auto info = one("SELECT * FROM acceptance_runtime_metadata WHERE singleton=1");
                require(number(info, "store_version") == 8 && str(info, "migration_sha256") == acceptance_migration_hash,
                        ErrorCode::Corrupt, "acceptance migration asset does not match durable metadata");
            }
            if (version >= 407) {
                require(!cleanup_migration_sql.empty(), ErrorCode::Unsupported, "407 database requires owned cleanup asset");
                const auto info = one("SELECT * FROM owned_cleanup_metadata WHERE singleton=1");
                require(number(info, "store_version") == 9 && str(info, "migration_sha256") == sha256_hex(cleanup_migration_sql),
                        ErrorCode::Corrupt, "owned cleanup migration fingerprint mismatch");
            }
            // Validate ownership/version before any persistent journal-mode
            // change; rejecting an unrelated database preserves its bytes.
            if (mode == StoreOpenMode::Takeover) exec("PRAGMA journal_mode=WAL;");
            require(str(one("PRAGMA journal_mode"), "journal_mode") == "wal", ErrorCode::Unsupported, "durable WAL mode required");
            require(number(one("PRAGMA foreign_keys"), "foreign_keys") == 1 && number(one("PRAGMA synchronous"), "synchronous") == 2,
                    ErrorCode::Storage, "SQLite durability settings unavailable");
            if (version == 401) {
                exec("BEGIN IMMEDIATE");
                try {
                    require(!migration_sql.empty(), ErrorCode::InvalidInput, "migration asset required");
                    exec(migration_sql);
                    execute("INSERT INTO store_runtime_metadata VALUES (1,4,?)", {migration_hash});
                    require(number(one("PRAGMA user_version"), "user_version") == 402,
                        ErrorCode::Corrupt, "Store migration did not produce the expected version");
                    exec("COMMIT");
                    version = 402;
                } catch (...) { rollback(); throw; }
            }
            const auto metadata = one("SELECT * FROM store_runtime_metadata WHERE singleton=1");
            require(number(metadata, "store_version") == 4 && str(metadata, "migration_sha256") == migration_hash,
                    ErrorCode::Corrupt, "Store migration asset does not match durable metadata");
            if (version == 402 && !tools_migration_sql.empty() && mode == StoreOpenMode::Takeover) {
                exec("BEGIN IMMEDIATE");
                try {
                    exec(tools_migration_sql);
                    execute("INSERT INTO worker_tools_metadata VALUES (1,5,?)", {tools_migration_hash});
                    require(number(one("PRAGMA user_version"), "user_version") == 403,
                        ErrorCode::Corrupt, "worker-tools migration did not produce the expected version");
                    exec("COMMIT");
                    version = 403;
                } catch (...) { rollback(); throw; }
            }
            tools_schema = version >= 403;
            if (version == 403 && !events_migration_sql.empty() && mode == StoreOpenMode::Takeover) {
                exec("BEGIN IMMEDIATE");
                try {
                    exec(events_migration_sql);
                    execute("INSERT INTO event_runtime_metadata VALUES (1,6,?)", {events_migration_hash});
                    require(number(one("PRAGMA user_version"), "user_version") == 404,
                        ErrorCode::Corrupt, "event-runtime migration did not produce the expected version");
                    events_schema = true;
                    for (const auto& existing : all("SELECT request_id FROM requests WHERE status NOT IN ('DELIVERED','FAILED','CANCELLED','SUPERSEDED')")) {
                        const auto id = str(existing, "request_id");
                        event(id, "event-runtime-bootstrap", Row{{"type", "event_bootstrap"}}, Row{{"request_id", id}});
                    }
                    exec("COMMIT");
                    version = 404;
                } catch (...) { events_schema = false; rollback(); throw; }
            }
            events_schema = version >= 404;
            if (version == 404 && !workflow_migration_sql.empty() && mode == StoreOpenMode::Takeover) {
                exec("BEGIN IMMEDIATE");
                try {
                    // S1.3 never created questions or submissions. Do not invent
                    // original call bindings for externally populated rows.
                    require(!optional("SELECT question_id FROM questions LIMIT 1"), ErrorCode::Unsupported,
                            "legacy questions have no captured workflow binding; explicit migration required");
                    exec(workflow_migration_sql);
                    execute("INSERT INTO workflow_runtime_metadata VALUES (1,7,?)", {workflow_migration_hash});
                    require(number(one("PRAGMA user_version"), "user_version") == 405,
                            ErrorCode::Corrupt, "workflow migration did not produce the expected version");
                    exec("COMMIT"); version = 405;
                } catch (...) { rollback(); throw; }
            }
            workflow_schema = version >= 405;
            if (version == 405 && !acceptance_migration_sql.empty() && mode == StoreOpenMode::Takeover) {
                exec("BEGIN IMMEDIATE");
                try {
                    require(!optional("SELECT experience_id FROM experience_jobs LIMIT 1") &&
                        !optional("SELECT cleanup_id FROM cleanup_jobs LIMIT 1") && !optional("SELECT request_id FROM receipts LIMIT 1"),
                        ErrorCode::Unsupported, "legacy closeout rows require explicit original-evidence migration");
                    require(!optional("SELECT request_id FROM requests WHERE status='DELIVERED' LIMIT 1"), ErrorCode::Unsupported,
                            "legacy delivered requests have no original package evidence; explicit migration required");
                    exec(acceptance_migration_sql);
                    execute("INSERT INTO acceptance_runtime_metadata VALUES (1,8,?)", {acceptance_migration_hash});
                    require(number(one("PRAGMA user_version"), "user_version") == 406,
                            ErrorCode::Corrupt, "acceptance migration did not produce expected version");
                    exec("COMMIT"); version = 406;
                } catch (...) { rollback(); throw; }
            }
            acceptance_schema = version >= 406;
            if (version == 406 && !cleanup_migration_sql.empty() && mode == StoreOpenMode::Takeover) {
                exec("BEGIN IMMEDIATE");
                try {
                    exec(cleanup_migration_sql);
                    execute("INSERT INTO owned_cleanup_metadata VALUES (1,9,?)", {sha256_hex(cleanup_migration_sql)});
                    require(number(one("PRAGMA user_version"), "user_version") == 407,
                            ErrorCode::Corrupt, "owned cleanup migration did not produce 407");
                    exec("COMMIT"); version = 407;
                } catch (...) { rollback(); throw; }
            }
            cleanup_schema = version >= 407;
            require(str(one("PRAGMA quick_check"), "quick_check") == "ok", ErrorCode::Corrupt, "SQLite quick_check failed");
            require(!optional("PRAGMA foreign_key_check"), ErrorCode::Corrupt, "SQLite foreign key check failed");
            if (mode == StoreOpenMode::Takeover) {
                exec("BEGIN IMMEDIATE");
                try {
                    if (acceptance_schema) {
                        for (const auto& terminal : all("SELECT * FROM requests WHERE status IN ('FAILED','CANCELLED','SUPERSEDED')")) retain_closeout(terminal);
                        // Job CAS fences old post-terminal calls even when a
                        // terminal request does not need a new business epoch.
                        execute("UPDATE experience_jobs SET status='PENDING',claimed_by=NULL,lease_epoch=lease_epoch+1,state_version=state_version+1,updated_at=? WHERE status<>'DONE'", {stamp()});
                        execute("UPDATE cleanup_jobs SET status='PENDING',claimed_by=NULL,lease_epoch=lease_epoch+1,state_version=state_version+1,updated_at=? WHERE status<>'DONE'", {stamp()});
                    }
                    if (events_schema) {
                        execute("UPDATE event_inbox SET status='PENDING',claim_id=NULL WHERE status='CLAIMED'");
                        execute("UPDATE event_actor_claims SET status='RELEASED',updated_at=? WHERE status='ACTIVE'", {stamp()});
                    }
                    for (const auto& request_row : all("SELECT * FROM requests WHERE status NOT IN ('DELIVERED','FAILED','CANCELLED','SUPERSEDED')"))
                        recover(request_row);
                    if (acceptance_schema) for (const auto& terminal : all("SELECT DISTINCT r.request_id FROM requests r JOIN experience_jobs e USING(request_id) WHERE r.status IN ('DELIVERED','FAILED','CANCELLED','SUPERSEDED') AND e.status<>'DONE'")) {
                        const auto id = str(terminal, "request_id");
                        event(id, new_id("recover"), Row{{"type", "recover_experience"}}, Row{{"request_id", id}});
                    }
                    // Accounting survives even if business already entered a
                    // terminal state before a process crash.
                    for (const auto& pending : all("SELECT DISTINCT request_id FROM call_attempts WHERE status='PENDING'"))
                        unknown_attempts(str(pending, "request_id"));
                    exec("COMMIT");
                } catch (...) { rollback(); throw; }
            }
        } catch (...) { close(); throw; }
    }
    ~Impl() { close(); }
    void close() noexcept {
        if (db) { sqlite3_close_v2(db); db = nullptr; }
        if (lease_fd >= 0) { ::flock(lease_fd, LOCK_UN); ::close(lease_fd); lease_fd = -1; }
    }
    void exec(std::string_view sql) {
        const std::string terminated(sql);
        char* message = nullptr;
        const auto result = sqlite3_exec(db, terminated.c_str(), nullptr, nullptr, &message);
        sqlite3_free(message);
        sqlite_failure(db, result, "SQLite statement");
    }
    void rollback() noexcept { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void execute(std::string_view sql, const Array& args = {}) {
        Statement statement(db, sql, args);
        require(!statement.step(), ErrorCode::Storage, "unexpected result from mutation");
    }
    std::optional<Row> optional(std::string_view sql, const Array& args = {}) const {
        Statement statement(db, sql, args);
        if (!statement.step()) return std::nullopt;
        return statement.row();
    }
    Row one(std::string_view sql, const Array& args = {}) const {
        const auto result = optional(sql, args);
        require(result.has_value(), ErrorCode::InvalidInput, "authoritative record not found");
        return *result;
    }
    std::vector<Row> all(std::string_view sql, const Array& args = {}) const {
        Statement statement(db, sql, args);
        std::vector<Row> result;
        while (statement.step()) result.push_back(statement.row());
        return result;
    }
    void cas(std::string_view sql, const Array& args) {
        execute(sql, args);
        require(sqlite3_changes(db) == 1, ErrorCode::Conflict, "state_version compare-and-swap failed");
    }
    class Transaction {
    public:
        explicit Transaction(Impl& owner) : owner_(owner) {
            require(owner_.mode == StoreOpenMode::Takeover, ErrorCode::Unauthorized, "read-only Store cannot mutate");
            owner_.exec("BEGIN IMMEDIATE");
        }
        ~Transaction() { if (!committed_) { owner_.capacity_event_cache.clear(); owner_.capacity_limit_cache.clear(); owner_.rollback(); } }
        void commit() { owner_.exec("COMMIT"); committed_ = true; }
    private:
        Impl& owner_;
        bool committed_{false};
    };

    Row request_row(const std::string& id) const {
        return one("SELECT r.*,x.config_snapshot_sha256,x.tool_allowlist FROM requests r JOIN request_runtime x USING(request_id) WHERE r.request_id=?", {id});
    }
    Row task_row(const std::string& id) const { return one("SELECT * FROM tasks WHERE task_id=?", {id}); }
    Row team_row(const std::string& id) const { return one("SELECT * FROM teams WHERE team_id=?", {id}); }
    Row op_row(const std::string& id) const {
        return one("SELECT o.*,i.arguments,i.evidence_ref FROM ops o JOIN operation_intents i USING(op_id) WHERE o.op_id=?", {id});
    }
    void require_tools_schema() const {
        require(tools_schema, ErrorCode::Unsupported, "worker-tool persistence requires the 403 migration");
    }
    void require_events_schema() const {
        require(events_schema, ErrorCode::Unsupported, "event runtime requires the 404 migration");
    }
    void require_workflow_schema() const {
        require(workflow_schema, ErrorCode::Unsupported, "question/submission workflow requires the 405 migration");
    }
    void require_acceptance_schema() const {
        require(acceptance_schema, ErrorCode::Unsupported, "acceptance/delivery requires the 406 migration");
    }
    Json acceptance_info(const Row& row) const {
        auto result = exposed(row); result.as_object()["actor"] = parse_json_strict(str(row, "actor"));
        const Row fact{{"submission_id", result.at("submission_id")}, {"submission_version", result.at("submission_version")},
            {"task_version", result.at("task_version")}, {"producer_binding", result.at("producer_binding")},
            {"actor", result.at("actor")}, {"captured_binding", result.at("captured_binding")}, {"decision", result.at("decision")}};
        require(sha256_hex(canonical_json(fact)) == str(row, "record_sha256"), ErrorCode::Corrupt, "acceptance evidence fingerprint mismatch");
        return result;
    }
    std::optional<Row> current_acceptance(const std::string& task_id) const {
        if (!acceptance_schema) return std::nullopt;
        const auto row = optional("SELECT * FROM candidate_acceptances WHERE task_id=? AND status='ACCEPTED'", {task_id});
        if (!row || str(task_row(task_id), "status") != "ACCEPTED") return std::nullopt;
        (void)acceptance_info(*row);
        const auto source = execution_binding_from_json(parse_json_strict(str(*row, "producer_binding")));
        if (!semantic_binding_equal(source, binding(task_id))) return std::nullopt;
        const auto r = request_row(str(*row, "request_id"));
        if (source.requirements_revision != number(r, "requirements_revision") || source.plan_revision != number(r, "plan_revision")) return std::nullopt;
        if (str(submission_row(str(*row, "submission_id")), "status") != "CANDIDATE_READY") return std::nullopt;
        return row;
    }
    Json experience_info(const std::string& id) const {
        require_acceptance_schema();
        auto row = one("SELECT * FROM experience_jobs WHERE experience_id=?", {id});
        auto result = exposed(row);
        result.as_object()["source_sha256"] = sha256_hex(canonical_json(result.at("source_snapshot")));
        const auto report = optional("SELECT * FROM experience_reports WHERE experience_id=?", {id});
        if (report) {
            auto value = exposed(*report); value.as_object()["actor"] = parse_json_strict(str(*report, "actor"));
            const Row fact{{"body", value.at("body")}, {"source_sha256", value.at("source_sha256")},
                {"actor", value.at("actor")}, {"captured_binding", value.at("captured_binding")}};
            require(str(*report, "source_sha256") == str(result.as_object(), "source_sha256") &&
                sha256_hex(canonical_json(fact)) == str(*report, "record_sha256"), ErrorCode::Corrupt, "experience report fingerprint mismatch");
            result.as_object()["report"] = value;
        } else result.as_object()["report"] = nullptr;
        return result;
    }
    Json validate_experience_target(const StoreActor& actor, const ExecutionBinding& captured,
                                   const std::string& id, std::int64_t expected, const std::string& source_hash) const {
        (void)authorize(actor, {Role::TeamLead}); actor_binding(actor, captured); matches(captured);
        const auto job = experience_info(id);
        require(str(job.as_object(), "request_id") == actor.request_id && str(job.as_object(), "task_id") == actor.task_id &&
                str(job.as_object(), "team_id") == actor.team_id, ErrorCode::Unauthorized, "experience job belongs to another team");
        require(number(job.as_object(), "state_version") == expected, ErrorCode::Conflict, "experience job CAS changed");
        require(str(job.as_object(), "source_sha256") == source_hash, ErrorCode::Conflict, "experience source changed");
        require(str(job.as_object(), "status") != "DONE", ErrorCode::NotReady, "experience job already completed");
        return job;
    }
    void invalidate_acceptance(const std::string& request_id, const std::string& task_id = {}) {
        if (!acceptance_schema) return;
        const auto filter = task_id.empty() ? std::string{} : " AND task_id=?";
        const auto tasks_sql = "UPDATE tasks SET status='BLOCKED',state_version=state_version+1,updated_at=? WHERE request_id=? AND status='ACCEPTED'" + filter;
        if (task_id.empty()) execute(tasks_sql, {stamp(), request_id}); else execute(tasks_sql, {stamp(), request_id, task_id});
        const auto sql = "UPDATE candidate_acceptances SET status='SUPERSEDED',state_version=state_version+1,updated_at=? WHERE request_id=? AND status='ACCEPTED'" + filter;
        if (task_id.empty()) execute(sql, {stamp(), request_id}); else execute(sql, {stamp(), request_id, task_id});
        execute("UPDATE delivery_runtime SET status='SUPERSEDED',state_version=state_version+1,updated_at=? WHERE request_id=? AND status IN ('PREPARED','STORED')", {stamp(), request_id});
    }
    void require_settled(const std::string& request_id, const std::string& team_id = {}) const {
        const auto filter = team_id.empty() ? std::string{} : " AND team_id=?";
        const Array args = team_id.empty() ? Array{request_id} : Array{request_id, team_id};
        require(!optional("SELECT op_id FROM ops WHERE request_id=? AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN')" + filter + " LIMIT 1", args),
                ErrorCode::UnknownOutcome, "unsettled operation blocks acceptance or delivery");
        require(!optional("SELECT attempt_id FROM call_attempts WHERE request_id=? AND (status IN ('PENDING','UNKNOWN') OR unknown_input=1 OR unknown_output=1 OR unknown_cost=1)" + filter + " LIMIT 1", args),
                ErrorCode::UnknownOutcome, "unsettled model attempt blocks acceptance or delivery");
    }
    Row delivery_row(const std::string& id) const {
        require_acceptance_schema(); return one("SELECT * FROM delivery_runtime WHERE delivery_id=?", {id});
    }
    Json delivery_info(const std::string& id) const {
        auto result = exposed(delivery_row(id));
        const auto& plan = result.at("frozen_plan");
        require(plan.at("delivery_id") == result.at("delivery_id") && plan.at("manifest_sha256") == result.at("manifest_sha256") &&
                sha256_hex(canonical_json(plan.at("manifest"))) == str(result.as_object(), "manifest_sha256"),
                ErrorCode::Corrupt, "delivery plan fingerprint mismatch");
        const auto receipt = optional("SELECT body FROM delivery_receipts WHERE delivery_id=?", {id});
        result.as_object()["receipt"] = receipt ? parse_json_strict(str(*receipt, "body")) : Json(nullptr);
        return result;
    }
    void delivery_current(const Row& delivery) const {
        const auto request = request_row(str(delivery, "request_id"));
        require(!terminal_request(str(request, "status")) &&
            number(request, "requirements_revision") == number(delivery, "requirements_revision") &&
            number(request, "plan_revision") == number(delivery, "plan_revision"), ErrorCode::Stale, "delivery does not match current live requirements and plan");
        require(str(delivery, "status") == "PREPARED" || str(delivery, "status") == "STORED", ErrorCode::NotReady, "delivery is not current");
        std::set<std::string> accepted_tasks;
        const auto acceptance_snapshot = parse_json_strict(str(delivery, "acceptance_snapshot"));
        for (const auto& evidence : acceptance_snapshot.as_array()) {
            const auto& saved = evidence.as_object(); const auto current = current_acceptance(str(saved, "task_id"));
            require(current && str(*current, "acceptance_id") == str(saved, "acceptance_id") &&
                str(*current, "record_sha256") == str(saved, "record_sha256"), ErrorCode::Stale, "delivery acceptance was superseded or revoked");
            accepted_tasks.insert(str(saved, "task_id"));
        }
        for (const auto& task : all("SELECT task_id FROM tasks WHERE request_id=? AND required=1", {str(delivery, "request_id")}))
            require(accepted_tasks.contains(str(task, "task_id")), ErrorCode::NotReady, "delivery lacks a current required task");
        require(!optional("SELECT q.question_id FROM questions q JOIN question_runtime x USING(question_id) WHERE q.request_id=? AND x.status='OPEN' AND x.blocking=1 LIMIT 1", {str(delivery, "request_id")}),
                ErrorCode::NotReady, "unanswered blocking question prevents publication or receipt");
        require_settled(str(delivery, "request_id"));
    }
    void verify_publication(const Row& record, const Json& publication) const {
        require(publication.is_object(), ErrorCode::InvalidInput, "publication must be host evidence");
        const auto plan = delivery_info(str(record, "delivery_id")).at("frozen_plan");
        require(publication.as_object().contains("durable") && publication.at("durable").is_bool() && publication.at("durable").as_bool() &&
                publication.as_object().contains("verified") && publication.at("verified").is_bool() && publication.at("verified").as_bool(),
                ErrorCode::NotReady, "publication requires complete verified fsync evidence");
        for (const auto* key : {"delivery_id", "root_identity", "manifest_sha256"})
            require(publication.as_object().contains(key) && publication.at(key) == plan.at(key), ErrorCode::Conflict, "publication changed frozen package identity");
        require(workflow_text(publication, "path", 4096) == str(plan.at("root_identity").as_object(), "path") + "/" + str(record, "delivery_id") &&
            workflow_text(publication, "files_sha256", 64) == sha256_hex(canonical_json(plan.at("manifest").at("files"))),
            ErrorCode::Conflict, "publication changed frozen path or file inventory");
    }
    void retain_closeout(const Row& request, const std::string& delivery_path = {}, const std::string& manifest_hash = {}) {
        if (!acceptance_schema || optional("SELECT request_id FROM receipts WHERE request_id=?", {str(request, "request_id")})) return;
        const auto id = str(request, "request_id"); const auto created = stamp();
        const auto config = parse_json_strict(str(request, "config_snapshot"), 8 * 1024 * 1024);
        const auto ttl = number(config.at("system").at("cleanup").as_object(), "receipt_ttl_days");
        require(ttl > 0 && ttl <= 36500, ErrorCode::Unsupported, "receipt TTL must be 1..36500 frozen days");
        const auto usage = one("SELECT * FROM request_budgets WHERE request_id=?", {id});
        execute("INSERT INTO receipts(request_id,tenant_id,project_id,root_id,idempotency_key,content_sha256,function_id,final_status,delivery_path,manifest_sha256,usage_summary,cleanup_status,closed_at,expires_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,'PENDING',?,strftime('%Y-%m-%dT%H:%M:%SZ',?,?))",
            {id, request.at("tenant_id"), request.at("project_id"), request.at("root_id"), request.at("idempotency_key"), request.at("content_sha256"), request.at("function_id"), request.at("status"),
             delivery_path.empty() ? Json(nullptr) : Json(delivery_path), manifest_hash.empty() ? Json(nullptr) : Json(manifest_hash), json_text(usage), created, created, "+" + std::to_string(ttl) + " days"});
        Array resources;
        for (const auto& workspace : all("SELECT w.* FROM workspace_bindings w JOIN teams t USING(team_id) WHERE t.request_id=?", {id})) resources.emplace_back(exposed(workspace));
        const Row snapshot{{"request_id", id}, {"final_status", request.at("status")}, {"delivery_path", delivery_path},
            {"manifest_sha256", manifest_hash}, {"workspaces", resources}, {"deletion_authorized", false}, {"receipt_ttl_days", ttl},
            {"checkpoint", "RETAINED"}, {"reason", cleanup_schema ?
                "owned workspace cleanup requires separate authorization; private data remains retained" :
                "controlled resource ownership and process-exit cleanup runner not implemented"}};
        execute("INSERT INTO cleanup_jobs(cleanup_id,request_id,receipt_ref,manifest_snapshot,status,created_at,updated_at) VALUES (?,?,?,?,'PENDING',?,?)",
            {new_id("cleanup"), id, id, json_text(snapshot, 1048576), created, created});
    }
    Row question_row(const std::string& id) const {
        require_workflow_schema();
        return one("SELECT q.*,x.original_binding,x.original_sha256,x.status,x.blocking,x.wait_generation FROM questions q JOIN question_runtime x USING(question_id) WHERE q.question_id=?", {id});
    }
    Json question_info(const std::string& id) const {
        auto result = exposed(question_row(id));
        const auto original = Row{{"body", result.at("body")}, {"original_binding", result.at("original_binding")}};
        require(sha256_hex(canonical_json(original)) == str(result.as_object(), "original_sha256"),
                ErrorCode::Corrupt, "original question fingerprint mismatch");
        if (!result.at("answer").is_null()) result.as_object()["answer"] = parse_json_strict(result.at("answer").as_string(), 32768);
        Array forwarded; Array transitions;
        for (const auto& row : all("SELECT * FROM question_transitions WHERE question_id=? ORDER BY state_version", {id})) {
            auto transition = exposed(row);
            transition.as_object()["actor"] = parse_json_strict(str(row, "actor"));
            if (str(row, "kind") == "FORWARDED") forwarded.emplace_back(transition.at("body"));
            transitions.emplace_back(std::move(transition));
        }
        result.as_object()["forwarded"] = forwarded;
        result.as_object()["transitions"] = transitions;
        return result;
    }
    Row submission_row(const std::string& id) const {
        require_workflow_schema();
        return one("SELECT * FROM workflow_submissions WHERE submission_id=?", {id});
    }
    Json submission_info(const std::string& id) const {
        auto result = exposed(submission_row(id));
        const Row fact{{"body", result.at("body")}, {"original_binding", result.at("original_binding")},
            {"artifact_snapshot", result.at("artifact_snapshot")}};
        require(sha256_hex(canonical_json(fact)) == str(result.as_object(), "record_sha256"),
                ErrorCode::Corrupt, "original submission fingerprint mismatch");
        Array checks;
        for (const auto& row : all("SELECT * FROM team_lead_checks WHERE submission_id=? ORDER BY submission_version", {id})) {
            auto check = exposed(row); check.as_object()["actor"] = parse_json_strict(str(row, "actor"));
            checks.emplace_back(std::move(check));
        }
        result.as_object()["checks"] = checks;
        const auto counter = optional("SELECT rework_count FROM team_workflow_state WHERE team_id=?", {str(result.as_object(), "team_id")});
        result.as_object()["team_rework_count"] = counter ? number(*counter, "rework_count") : 0;
        result.as_object()["business_accepted"] = false;
        result.as_object()["business_delivered"] = false;
        if (acceptance_schema) {
            const auto current = current_acceptance(str(result.as_object(), "task_id"));
            const bool accepted = current && str(*current, "submission_id") == id;
            result.as_object()["business_accepted"] = accepted;
            result.as_object()["acceptance"] = accepted ? acceptance_info(*current) : Json(nullptr);
            result.as_object()["business_delivered"] = accepted && str(request_row(str(result.as_object(), "request_id")), "status") == "DELIVERED";
        }
        return result;
    }
    bool has_blocking_question(const std::string& task_id) const {
        return workflow_schema && optional("SELECT q.question_id FROM questions q JOIN question_runtime x USING(question_id) WHERE q.task_id=? AND x.status='OPEN' AND x.blocking=1 LIMIT 1", {task_id}).has_value();
    }
    void workflow_subject(const StoreActor& actor, const ExecutionBinding& captured,
                          const Row& subject, bool current_policy_control = false) const {
        const auto request = authorize(actor, {Role::Root, Role::Dispatcher, Role::TeamLead});
        actor_binding(actor, captured); matches(captured);
        require(str(subject, "request_id") == actor.request_id,
                ErrorCode::Unauthorized, "workflow subject belongs to another request");
        if (actor.role == Role::TeamLead)
            require(str(subject, "task_id") == actor.task_id && str(subject, "team_id") == actor.team_id,
                    ErrorCode::Unauthorized, "workflow subject belongs to another team");
        const auto original = execution_binding_from_json(parse_json_strict(str(subject, "original_binding")));
        const auto task = task_row(str(subject, "task_id"));
        require(!terminal_request(str(request, "status")) && !terminal_task(str(task, "status")),
                ErrorCode::NotReady, "workflow target has terminated");
        require(semantic_binding_equal(original, binding(str(subject, "task_id")), current_policy_control), ErrorCode::Stale,
                "workflow subject has an obsolete semantic binding");
    }
    Json rejected_info(const std::string& task_id) const {
        require_acceptance_schema();
        const auto task = task_row(task_id);
        const auto saved = optional("SELECT submission_id FROM workflow_submissions WHERE task_id=? AND status='BLOCKED'", {task_id});
        if (str(task, "status") != "BLOCKED" || !saved) return nullptr;
        auto result = submission_info(str(*saved, "submission_id"));
        const auto id = str(result.as_object(), "submission_id");
        const auto request = request_row(str(task, "request_id"));
        const auto config = parse_json_strict(str(request, "config_snapshot"), 8 * 1024 * 1024);
        const auto limit = number(config.at("system").at("limits").as_object(), "team_internal_rework_max");
        const auto used = number(result.as_object(), "team_rework_count");
        require(limit >= 0 && limit <= 10000 && used >= 0, ErrorCode::Corrupt, "invalid frozen shared rework allowance");
        result.as_object()["rework_limit"] = limit;
        result.as_object()["rework_remaining"] = std::max<std::int64_t>(0, limit - used);
        Json cause = nullptr;
        if (const auto rejected = optional("SELECT * FROM candidate_acceptances WHERE submission_id=? AND status='REJECTED' ORDER BY rowid DESC LIMIT 1", {id})) {
            require(next(number(*rejected, "submission_version")) == number(result.as_object(), "state_version"),
                    ErrorCode::Corrupt, "rejection source does not match blocked candidate version");
            cause = Row{{"kind", "business_rejection"}, {"record", acceptance_info(*rejected)}};
        } else {
            const auto& checks = result.at("checks").as_array();
            require(!checks.empty() && checks.back().at("status") == "BLOCKED" &&
                !checks.back().at("check_body").at("passed").as_bool() &&
                checks.back().at("submission_version") == result.at("state_version"),
                ErrorCode::Corrupt, "blocked submission lacks its original failed check");
            cause = Row{{"kind", "team_rework_exhausted"}, {"record", checks.back()}};
        }
        result.as_object()["rejection_cause"] = std::move(cause);
        result.as_object()["escalation"] = nullptr;
        if (const auto prior = optional("SELECT result FROM events WHERE request_id=? AND type='escalate_task' AND json_extract(payload,'$.submission_id')=? ORDER BY seq DESC LIMIT 1", {str(request, "request_id"), id})) {
            const auto fact = parse_json_strict(str(*prior, "result"));
            auto question = question_info(str(fact.as_object(), "question_id"));
            require(fact.at("submission_id") == result.at("submission_id") &&
                fact.at("source_record_sha256") == result.at("record_sha256"), ErrorCode::Corrupt, "escalation source fingerprint changed");
            require(question.at("task_id") == result.at("task_id") && question.at("team_id") == result.at("team_id"),
                    ErrorCode::Corrupt, "escalation source scope changed");
            result.as_object()["escalation"] = std::move(question);
        }
        return result;
    }
    Json rejection_subject(const StoreActor& actor, const ExecutionBinding& captured,
                           std::int64_t task_version, const std::string& submission_id,
                           std::int64_t submission_version) const {
        const auto source = submission_row(submission_id);
        workflow_subject(actor, captured, source, true);
        const auto task_id = str(source, "task_id");
        const auto task = task_row(task_id); const auto request = request_row(actor.request_id);
        require(number(task, "state_version") == task_version && number(source, "state_version") == submission_version,
                ErrorCode::Conflict, "rejected task or submission CAS changed");
        require(str(task, "status") == "BLOCKED" && str(source, "status") == "BLOCKED" && str(task, "kind") == "team_task",
                ErrorCode::NotReady, "only a current blocked team submission has a rejection successor");
        require(number(task, "based_on_requirements_revision") == number(request, "requirements_revision") &&
                number(task, "plan_revision") == number(request, "plan_revision"), ErrorCode::Stale, "rejection successor requires current AMEND disposition");
        const auto team = team_row(str(source, "team_id"));
        require(str(team, "status") == "ACTIVE", ErrorCode::NotReady, "a stopped producer cannot be revived by corrective rework");
        return rejected_info(task_id);
    }
    void supersede_workflow(const std::string& request_id, const std::string& task_id = {}) {
        if (!workflow_schema) return;
        invalidate_acceptance(request_id, task_id);
        const auto filter = std::string(task_id.empty() ? "" : " AND q.task_id=?");
        const Array args = task_id.empty() ? Array{request_id} : Array{request_id, task_id};
        for (const auto& row : all("SELECT q.question_id FROM questions q JOIN question_runtime x USING(question_id) WHERE q.request_id=? AND x.status<>'SUPERSEDED'" + filter, args)) {
            execute("UPDATE questions SET state_version=state_version+1 WHERE question_id=?", {str(row, "question_id")});
            execute("UPDATE question_runtime SET status='SUPERSEDED' WHERE question_id=?", {str(row, "question_id")});
        }
        const auto sql = "UPDATE workflow_submissions SET status='SUPERSEDED',state_version=state_version+1,updated_at=? WHERE request_id=? AND status IN ('SUBMITTED','CANDIDATE_READY','BLOCKED')" +
            std::string(task_id.empty() ? "" : " AND task_id=?");
        if (task_id.empty()) execute(sql, {stamp(), request_id});
        else execute(sql, {stamp(), request_id, task_id});
    }
    Json wait_info(const std::string& task_id) const {
        require_events_schema();
        (void)task_row(task_id);
        const auto row = optional("SELECT * FROM worker_waits WHERE task_id=?", {task_id});
        return row ? exposed(*row) : Json(nullptr);
    }
    Json feedback_info(const std::string& attempt_id) const {
        require_events_schema();
        const auto row = optional("SELECT * FROM model_tool_feedback WHERE attempt_id=?", {attempt_id});
        if (!row) return nullptr;
        const auto value = exposed(*row);
        const Row fact{{"attempt_id", attempt_id}, {"original_binding", value.at("original_binding")},
            {"response", value.at("response")}, {"result", value.at("result")}};
        require(sha256_hex(canonical_json(fact)) == str(*row, "record_sha256"), ErrorCode::Corrupt,
                "model feedback fingerprint differs from its immutable content");
        return value;
    }
    Json workspace_info(const std::string& team_id) const {
        require_tools_schema();
        (void)team_row(team_id);
        const auto row = optional("SELECT * FROM workspace_bindings WHERE team_id=?", {team_id});
        if (!row) return nullptr;
        require(sha256_hex(str(*row, "execution_policy")) == str(*row, "execution_policy_sha256"),
                ErrorCode::Corrupt, "workspace execution policy hash mismatch");
        return exposed(*row);
    }
    Json result_info(const std::string& op_id) const {
        require_tools_schema();
        (void)op_row(op_id);
        const auto row = optional("SELECT r.*,o.status AS operation_status FROM operation_results r JOIN ops o USING(op_id) WHERE r.op_id=?", {op_id});
        return row ? exposed(*row) : Json(nullptr);
    }
    Row authorize(const StoreActor& actor, std::initializer_list<Role> roles, bool current_epochs = true) const {
        require(std::find(roles.begin(), roles.end(), actor.role) != roles.end(), ErrorCode::Unauthorized, "role is not authorized");
        const auto r = request_row(actor.request_id);
        if (actor.role == Role::Root || actor.role == Role::Dispatcher) {
            require(actor.task_id.empty() && actor.team_id.empty() && actor.execution_epoch == 0 &&
                    actor.principal_id == str(r, actor.role == Role::Root ? "root_id" : "dispatcher_id"),
                    ErrorCode::Unauthorized, "Root/Dispatcher owner mismatch");
        } else {
            const auto team = team_row(actor.team_id);
            require(str(team, "request_id") == actor.request_id && str(team, "task_id") == actor.task_id &&
                    str(team, actor.role == Role::TeamLead ? "lead_id" : "worker_id") == actor.principal_id,
                    ErrorCode::Unauthorized, "team owner mismatch");
            if (current_epochs) {
                require(number(team, "execution_epoch") == actor.execution_epoch, ErrorCode::Stale, "stale execution epoch");
                require(str(team, "status") == "STARTING" || str(team, "status") == "ACTIVE" || str(team, "status") == "DRAINING",
                        ErrorCode::NotReady, "team execution has stopped");
            }
        }
        if (current_epochs) require(number(r, "dispatcher_epoch") == actor.dispatcher_epoch, ErrorCode::Stale, "stale dispatcher epoch");
        return r;
    }
    ExecutionBinding request_binding(const std::string& request_id) const {
        const auto r = request_row(request_id);
        ExecutionBinding b;
        b.request_id = request_id;
        b.requirements_revision = number(r, "requirements_revision");
        b.plan_revision = number(r, "plan_revision");
        b.dispatcher_epoch = number(r, "dispatcher_epoch");
        b.execution_epoch = 0;
        b.policy_epoch = number(one("SELECT policy_epoch FROM runtime_policy WHERE singleton=1"), "policy_epoch");
        return b;
    }
    ExecutionBinding binding(const std::string& task_id) const {
        const auto t = task_row(task_id);
        auto b = request_binding(str(t, "request_id"));
        const auto team = one("SELECT * FROM teams WHERE task_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {task_id});
        b.task_id = task_id;
        b.team_id = str(team, "team_id");
        b.task_revision = number(t, "task_revision");
        b.plan_revision = number(t, "plan_revision");
        b.instruction_revision = number(t, "instruction_revision");
        b.execution_epoch = number(team, "execution_epoch");
        return b;
    }
    void matches(const ExecutionBinding& b, bool policy = true) const {
        auto current = b.task_id.empty() ? request_binding(b.request_id) : binding(b.task_id);
        if (!policy) current.policy_epoch = b.policy_epoch;
        require(b == current, ErrorCode::Stale, "frozen execution binding is stale");
    }
    void actor_binding(const StoreActor& actor, const ExecutionBinding& b) const {
        require(actor.request_id == b.request_id && actor.task_id == b.task_id && actor.team_id == b.team_id &&
                actor.dispatcher_epoch == b.dispatcher_epoch && actor.execution_epoch == b.execution_epoch,
                ErrorCode::Unauthorized, "actor and frozen binding disagree");
    }
    void tool_grant(const Row& r, const Row& team, std::string_view tool_id) const {
        const auto frozen_config = FrozenRequestConfig::restore(str(r, "config_snapshot"), str(r, "config_snapshot_sha256"));
        const auto template_tools = frozen_config.worker_tools(str(team, "template_id"));
        const auto denied = parse_json_strict(str(one("SELECT denied_tools FROM runtime_policy WHERE singleton=1"), "denied_tools"));
        require(template_tools.contains(std::string(tool_id)) && contains(parse_json_strict(str(r, "tool_allowlist")), tool_id) && !contains(denied, tool_id),
                ErrorCode::Unauthorized, "tool grant absent or revoked");
    }
    Json control_authorization(const StoreActor& actor, const ExecutionBinding& b,
                               const std::string& tool_id, bool diagnostic) const {
        require_tools_schema();
        require(diagnostic ? diagnostic_tool(tool_id) : tool_id == "op_cancel" || tool_id == "ack_change",
                ErrorCode::Unauthorized, "tool is not registered for this control branch");
        const auto r = authorize(actor, {Role::Worker});
        actor_binding(actor, b); matches(b);
        tool_grant(r, team_row(actor.team_id), tool_id);
        const auto frozen = FrozenRequestConfig::restore(str(r, "config_snapshot"), str(r, "config_snapshot_sha256"));
        const auto& control = frozen.json().at("system").at("control").as_object();
        if (diagnostic) require(contains(control.at("diagnostic_tools"), tool_id), ErrorCode::Unauthorized, "diagnostic tool absent from frozen control policy");
        const auto limit = number(control, "max_queries_per_minute");
        require(limit > 0 && limit <= 10000, ErrorCode::Corrupt, "invalid frozen diagnostic query limit");
        const auto identity = workspace_info(actor.team_id);
        if (tool_id.starts_with("fs_")) require(!identity.is_null(), ErrorCode::NotReady, "team workspace has not been bound");
        return Row{{"tool_id", tool_id}, {"max_queries_per_minute", limit}, {"max_output_bytes", 1048576},
            {"max_page_bytes", 65536}, {"workspace", identity}};
    }
    void ready(const StoreActor& actor, const ExecutionBinding& b, std::string_view tool_id = {}) const {
        const auto r = authorize(actor, {Role::Worker});
        actor_binding(actor, b);
        matches(b);
        const auto t = task_row(actor.task_id);
        const auto team = team_row(actor.team_id);
        require((str(r, "status") == "RUNNING" || str(r, "status") == "VERIFYING") && str(team, "status") == "ACTIVE" &&
            (str(t, "status") == "READY" || str(t, "status") == "RUNNING") && number(team, "paused_for_change") == 0 &&
            number(t, "based_on_requirements_revision") == number(r, "requirements_revision") &&
            number(t, "plan_revision") == number(r, "plan_revision") && number(t, "instruction_revision") > 0 &&
            number(t, "applied_task_revision") == number(t, "task_revision") &&
            number(t, "applied_plan_revision") == number(t, "plan_revision") &&
            number(t, "applied_instruction_revision") == number(t, "instruction_revision"), ErrorCode::NotReady, "business execution is not ready");
        require(!optional("SELECT op_id FROM ops WHERE team_id=? AND status='UNKNOWN' LIMIT 1", {actor.team_id}),
                ErrorCode::UnknownOutcome, "unknown operation requires host reconciliation");
        if (events_schema) require(!optional("SELECT task_id FROM worker_waits WHERE task_id=? AND status='WAITING'", {actor.task_id}),
                ErrorCode::NotReady, "Worker is durably waiting for an explicit wake");
        if (!tool_id.empty()) tool_grant(r, team, tool_id);
    }
    std::optional<Json> replay(const std::string& request_id, const std::string& command_id, const Json& command) const {
        text_field(command_id, "command_id");
        const auto row = optional("SELECT command_sha256,result FROM events WHERE request_id=? AND command_id=?", {request_id, command_id});
        if (!row) return std::nullopt;
        require(str(*row, "command_sha256") == sha256_hex(canonical_json(command)), ErrorCode::Conflict, "command_id reused with different intent");
        return parse_json_strict(str(*row, "result"), 16 * 1024 * 1024);
    }
#include "store_capacity.inc"

    void event(const std::string& request_id, const std::string& command_id,
               const Row& command, const Json& result, std::string_view actor = "runtime") {
        text_field(command_id, "command_id");
        const auto type = str(command, "type");
        guard_capacity_event(request_id, command);
        execute("INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) VALUES (?,?,?,?,?,?,?,?,?)",
            {new_id("evt"), request_id, type, actor, command_id, sha256_hex(canonical_json(command)), json_text(command), json_text(result, 16 * 1024 * 1024), stamp()});
        capacity_event_inserted(request_id, command);
        if (events_schema) route_event(request_id, type, command, result, sqlite3_last_insert_rowid(db));
        observe_capacity(request_id);
    }
    void release_claims(const std::string& request_id) {
        if (!events_schema) return;
        execute("UPDATE event_inbox SET status='PENDING',claim_id=NULL WHERE request_id=? AND status='CLAIMED'", {request_id});
        execute("UPDATE event_actor_claims SET status='RELEASED',updated_at=? WHERE request_id=? AND status='ACTIVE'", {stamp(), request_id});
    }
    void supersede_waits(const std::string& request_id, const std::string& task_id = {}) {
        if (!events_schema) return;
        supersede_workflow(request_id, task_id);
        if (task_id.empty()) execute("UPDATE worker_waits SET status='SUPERSEDED',updated_at=? WHERE request_id=? AND status='WAITING'", {stamp(), request_id});
        else execute("UPDATE worker_waits SET status='SUPERSEDED',updated_at=? WHERE request_id=? AND task_id=? AND status='WAITING'", {stamp(), request_id, task_id});
    }
    void route_event(const std::string& request_id, const std::string& type,
                     const Row& command, const Json& result, std::int64_t seq) {
        const auto request = request_row(request_id);
        const auto revision = number(request, "requirements_revision");
        const auto enqueue = [&](std::string_view role, const std::string& task_id) {
            execute("INSERT OR IGNORE INTO event_inbox(event_seq,request_id,role,actor_task_id,subject_task_id,event_requirements_revision) VALUES (?,?,?,?,?,?)",
                {seq, request_id, role, role == "dispatcher" ? std::string{} : task_id, task_id, revision});
        };
        const auto live = all("SELECT * FROM tasks WHERE request_id=? AND status NOT IN ('CANCELLED','SKIPPED','SUPERSEDED') ORDER BY rowid", {request_id});
        if (type == "amend" || type == "plan" || type == "independent_plan" || type == "cancel_request") supersede_waits(request_id);
        if (type == "disposition" || type == "rebind_task") supersede_waits(request_id, str(command, "task_id"));
        if (type == "instruction") supersede_waits(request_id, str(command.at("binding").as_object(), "task_id"));
        if (type == "accept_candidate") {
            if (result.as_object().contains("experience_id") && !result.at("experience_id").is_null())
                enqueue("team_lead", str(result.as_object(), "task_id"));
            enqueue("dispatcher", str(result.as_object(), "task_id"));
            return;
        }
        if (type == "change_task") { enqueue("team_lead", str(result.as_object(), "task_id")); return; }
        if (type == "escalate_task") return; // User-owned question remains durable; no implicit answer.
        if (type == "report_experience") { if (!terminal_request(str(request, "status"))) enqueue("dispatcher", str(result.as_object(), "task_id")); return; }
        if (type == "recover_experience") {
            for (const auto& job : all("SELECT DISTINCT task_id FROM experience_jobs WHERE request_id=? AND status<>'DONE'", {request_id})) enqueue("team_lead", str(job, "task_id"));
            return;
        }
        if (terminal_request(str(request, "status"))) return;
        if (type == "admit") { enqueue("dispatcher", ""); return; }
        if (type == "amend") {
            if (live.empty()) enqueue("dispatcher", "");
            for (const auto& task : live) enqueue("dispatcher", str(task, "task_id"));
            return;
        }
        if (type == "event_bootstrap" || type == "recover") {
            if (live.empty()) { enqueue("dispatcher", ""); return; }
            for (const auto& task : live) {
                if (number(task, "based_on_requirements_revision") != revision ||
                    number(task, "plan_revision") != number(request, "plan_revision"))
                    enqueue("dispatcher", str(task, "task_id"));
                else if (!terminal_task(str(task, "status"))) {
                    const auto task_id = str(task, "task_id"); bool workflow_pending = false;
                    if (workflow_schema) {
                        for (const auto& question : all("SELECT holder FROM questions q JOIN question_runtime x USING(question_id) WHERE q.task_id=? AND x.status='OPEN'", {task_id})) {
                            workflow_pending = true;
                            if (str(question, "holder") != "user") enqueue(str(question, "holder"), task_id);
                        }
                        if (const auto submission = optional("SELECT status FROM workflow_submissions WHERE task_id=? AND status IN ('SUBMITTED','CANDIDATE_READY','BLOCKED')", {task_id})) {
                            workflow_pending = true;
                            enqueue(str(*submission, "status") == "SUBMITTED" ? "team_lead" : "dispatcher", task_id);
                        }
                    }
                    if (!workflow_pending) enqueue("team_lead", task_id);
                }
                if (acceptance_schema && optional("SELECT experience_id FROM experience_jobs WHERE task_id=? AND status<>'DONE' LIMIT 1", {str(task, "task_id")})) enqueue("team_lead", str(task, "task_id"));
                if (acceptance_schema && str(task, "status") == "ACCEPTED") enqueue("dispatcher", str(task, "task_id"));
            }
            return;
        }
        if (type == "ask_question" || type == "forward_question") {
            const auto holder = str(result.as_object(), "holder");
            if (holder != "user") enqueue(holder, str(result.as_object(), "task_id"));
            return;
        }
        if (type == "answer_question") {
            const auto task_id = str(result.as_object(), "task_id");
            if (result.at("asked_by") == "dispatcher") enqueue("dispatcher", task_id);
            else { enqueue("worker", task_id); enqueue("team_lead", task_id); }
            return;
        }
        if (type == "submit_work") { enqueue("team_lead", str(result.as_object(), "task_id")); return; }
        if (type == "check_submission") {
            enqueue(str(result.as_object(), "status") == "RETURNED_FOR_REWORK" ? "worker" : "dispatcher",
                    str(result.as_object(), "task_id"));
            return;
        }
        if (type == "create_task" || type == "disposition" || type == "rebind_task") {
            const auto task_id = str(result.as_object(), "task_id");
            if (!terminal_task(str(task_row(task_id), "status"))) enqueue("team_lead", task_id);
            return;
        }
        if (type == "independent_plan" || type == "independent_dispositions") {
            for (const auto& changed : result.at("tasks").as_array()) {
                const auto task_id = str(changed.as_object(), "task_id");
                supersede_waits(request_id, task_id);
                if (!terminal_task(str(task_row(task_id), "status"))) enqueue("team_lead", task_id);
            }
            return;
        }
        if (type == "instruction" || type == "ack" || type == "wake_worker" ||
            (type == "execute_tool_control" && str(command, "tool_id") == "ack_change")) {
            enqueue("worker", str(command.at("binding").as_object(), "task_id"));
            return;
        }
        if (type == "tool_feedback") {
            const auto attempt = one("SELECT role,task_id FROM call_attempts WHERE attempt_id=?", {str(command, "attempt_id")});
            if (str(attempt, "role") == "worker") enqueue("worker", str(attempt, "task_id"));
        }
    }
    std::optional<Json> check_reconciliation(const StoreActor& root, const std::string& id,
            std::int64_t expected, const std::string& command_id, const Row& command) const {
        (void)authorize(root, {Role::Root}, false);
        const auto op = op_row(id);
        require(str(op, "request_id") == root.request_id, ErrorCode::Unauthorized, "operation belongs to another request");
        if (auto old = replay(root.request_id, command_id, command)) return old;
        (void)authorize(root, {Role::Root});
        require(str(op, "status") == "UNKNOWN", ErrorCode::NotReady, "only UNKNOWN operation needs reconciliation");
        require(number(op, "state_version") == expected, ErrorCode::Conflict, "operation reconciliation CAS failed");
        return std::nullopt;
    }
    void fence_ops(const std::string& request_id, const std::string& team_id = {}) {
        const auto sql = "UPDATE ops SET status=CASE WHEN status='PREPARED' THEN 'CANCELLED' ELSE 'UNKNOWN' END,state_version=state_version+1 WHERE request_id=?" +
            std::string(team_id.empty() ? "" : " AND team_id=?") + " AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED')";
        if (team_id.empty()) execute(sql, {request_id}); else execute(sql, {request_id, team_id});
    }
    void unknown_attempts(const std::string& request_id) {
        const auto values = one("SELECT COUNT(*) AS n,COALESCE(SUM(reserved_input),0) AS i,COALESCE(SUM(reserved_output),0) AS o,COALESCE(SUM(reserved_cost),0) AS c FROM call_attempts WHERE request_id=? AND status='PENDING'", {request_id});
        if (number(values, "n") == 0) return;
        execute("UPDATE request_budgets SET unknown_input=unknown_input+?,unknown_output=unknown_output+?,unknown_cost=unknown_cost+?,state_version=state_version+1 WHERE request_id=?",
            {number(values, "i"), number(values, "o"), number(values, "c"), request_id});
        execute("UPDATE call_attempts SET status='UNKNOWN',unknown_input=1,unknown_output=1,unknown_cost=1,state_version=state_version+1 WHERE request_id=? AND status='PENDING'", {request_id});
        event(request_id, new_id("recover"), Row{{"type", "attempts_unknown"}}, Row{{"attempts", number(values, "n")}});
    }
    Row current_instruction(const Row& t) const {
        auto order = one("SELECT * FROM instructions WHERE task_id=? AND instruction_revision=?",
            {str(t, "task_id"), number(t, "instruction_revision")});
        if (!optional("SELECT team_id FROM teams WHERE task_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {str(t, "task_id")}))
            return order; // Retired history remains readable without a live actor.
        const auto current = binding(str(t, "task_id"));
        // The original row is append-only. Epoch-only rebinding is an audited
        // host projection, never an UPDATE of the original semantic decision.
        const auto recovery = optional("SELECT result FROM events WHERE request_id=? AND type='recover' ORDER BY seq DESC LIMIT 1",
            {str(t, "request_id")});
        if (!recovery) return order;
        const auto result = parse_json_strict(str(*recovery, "result"), 16 * 1024 * 1024);
        const auto* entries = result.as_object().if_contains("rebound_instructions");
        if (!entries) return order; // Legacy recover events do not grant a rebind.
        for (const auto& entry : entries->as_array()) {
            const auto& e = entry.as_object();
            if (str(e, "task_id") != current.task_id || number(e, "instruction_revision") != current.instruction_revision) continue;
            if (number(e, "dispatcher_epoch") != current.dispatcher_epoch || number(e, "execution_epoch") != current.execution_epoch ||
                str(e, "body_sha256") != sha256_hex(str(order, "body")) ||
                number(e, "previous_dispatcher_epoch") != number(order, "dispatcher_epoch") ||
                number(e, "previous_execution_epoch") != number(order, "execution_epoch")) continue;
            // Recheck semantics even when reading a once-valid recovery event.
            if (str(order, "team_id") != current.team_id || number(order, "task_revision") != current.task_revision ||
                number(order, "plan_revision") != current.plan_revision ||
                number(order, "based_on_requirements_revision") != current.requirements_revision) continue;
            order["dispatcher_epoch"] = current.dispatcher_epoch;
            order["execution_epoch"] = current.execution_epoch;
            break;
        }
        return order;
    }
    void recover(const Row& r) {
        const auto id = str(r, "request_id");
        release_claims(id);
        cas("UPDATE requests SET dispatcher_epoch=dispatcher_epoch+1,state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?",
            {stamp(), id, number(r, "state_version")});
        execute("UPDATE teams SET execution_epoch=execution_epoch+1,paused_for_change=1,state_version=state_version+1,updated_at=? WHERE request_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {stamp(), id});
        fence_ops(id);
        unknown_attempts(id);
        // Instructions are semantic decisions, not leases. The recovery event
        // grants only a current matching epoch projection; original instruction
        // rows, attempts and operation bindings remain immutable audit evidence.
        // The writer transaction encloses epochs, projection, ack invalidation
        // and this event, so a failed append cannot leave a half-recovered team.
        Array rebound;
        for (const auto& order : all(
                "SELECT i.*,g.execution_epoch AS new_execution_epoch FROM instructions i "
                "JOIN tasks t ON t.task_id=i.task_id JOIN teams g ON g.task_id=t.task_id "
                "WHERE t.request_id=? AND t.instruction_revision=i.instruction_revision "
                "AND i.team_id=g.team_id AND i.task_revision=t.task_revision "
                "AND i.plan_revision=t.plan_revision AND t.plan_revision=? "
                "AND i.based_on_requirements_revision=t.based_on_requirements_revision "
                "AND t.based_on_requirements_revision=? "
                "AND g.status IN ('STARTING','ACTIVE','DRAINING') "
                "AND t.status NOT IN ('CANCELLED','FAILED','SKIPPED','SUPERSEDED') ORDER BY t.rowid",
                {id, number(r, "plan_revision"), number(r, "requirements_revision")})) {
            const auto task_id = str(order, "task_id");
            const auto revision = number(order, "instruction_revision");
            const auto epoch = number(order, "new_execution_epoch");
            execute("UPDATE tasks SET applied_instruction_revision=0,state_version=state_version+1,updated_at=? "
                "WHERE task_id=? AND status IN ('READY','RUNNING')", {stamp(), task_id});
            rebound.emplace_back(Row{{"task_id", task_id}, {"instruction_revision", revision},
                {"body_sha256", sha256_hex(str(order, "body"))},
                {"previous_dispatcher_epoch", number(order, "dispatcher_epoch")},
                {"previous_execution_epoch", number(order, "execution_epoch")},
                {"dispatcher_epoch", next(number(r, "dispatcher_epoch"))}, {"execution_epoch", epoch}});
        }
        event(id, new_id("recover"), Row{{"type", "recover"}},
            Row{{"dispatcher_epoch", next(number(r, "dispatcher_epoch"))}, {"rebound_instructions", rebound}});
    }
    void catalog(const std::string& request_id, std::int64_t revision, const Json& value) {
        validate_constraint_catalog_shape(value);
        std::set<std::string> ids;
        for (const auto& item : value.as_array()) {
            require(item.is_object() && item.as_object().size() == 3, ErrorCode::InvalidInput, "invalid constraint record");
            const auto& entry = item.as_object();
            const auto id = str(entry, "constraint_id");
            const auto text = str(entry, "text");
            const auto source = number(entry, "source_revision");
            text_field(id, "constraint_id", 40);
            text_field(text, "constraint text", 1000);
            require(ids.insert(id).second && source > 0 && source <= revision, ErrorCode::InvalidInput, "duplicate constraint or invalid source revision");
            const auto original = str(one("SELECT text FROM request_texts WHERE request_id=? AND requirements_revision=?", {request_id, source}), "text");
            require(original.find(text) != std::string::npos, ErrorCode::InvalidInput, "constraint must preserve original source text");
            execute("INSERT INTO request_constraints VALUES (?,?,?,?,?)", {request_id, revision, id, source, text});
        }
    }
    void brief_constraints(const Row& r, const Json& brief) const {
        require(brief.is_object(), ErrorCode::InvalidInput, "task brief must be an object");
        const auto* refs = brief.as_object().if_contains("constraint_refs");
        require(refs && refs->is_array(), ErrorCode::InvalidInput, "task brief needs constraint_refs");
        const auto* verbatim = brief.as_object().if_contains("constraints_verbatim");
        require(verbatim && verbatim->is_array(), ErrorCode::InvalidInput, "task brief needs faithful constraints_verbatim");
        const auto catalog_rows = all("SELECT constraint_id,text FROM request_constraints WHERE request_id=? AND requirements_revision=?", {str(r, "request_id"), number(r, "requirements_revision")});
        std::set<std::string> known;
        for (const auto& item : catalog_rows) known.insert(str(item, "constraint_id"));
        std::set<std::string> assigned;
        for (const auto& item : refs->as_array()) {
            require(item.is_string() && known.contains(std::string(item.as_string())) && assigned.insert(std::string(item.as_string())).second,
                    ErrorCode::InvalidInput, "unknown or duplicate assigned constraint");
        }
        // This first slice assigns every explicit constraint to each team.
        // A later DAG can narrow that only with a persisted authority mapping.
        require(assigned == known, ErrorCode::InvalidInput, "task omits an authoritative request constraint");
        std::set<std::string> excerpts;
        for (const auto& excerpt : verbatim->as_array()) {
            require(excerpt.is_string(), ErrorCode::InvalidInput, "constraint excerpt must be text");
            const auto text = std::string(excerpt.as_string());
            text_field(text, "constraint excerpt", 1000);
            bool faithful = false;
            for (const auto& source : all("SELECT text FROM request_texts WHERE request_id=?", {str(r, "request_id")}))
                if (str(source, "text").find(text) != std::string::npos) faithful = true;
            require(faithful, ErrorCode::InvalidInput, "constraint excerpt changes original request text");
            excerpts.insert(text);
        }
        for (const auto& item : catalog_rows) require(excerpts.contains(str(item, "text")), ErrorCode::InvalidInput, "task omits assigned original constraint text");
    }
    std::int64_t configured_limit(const Row& r, std::string_view name) const {
        const auto config = parse_json_strict(str(r, "config_snapshot"), 8 * 1024 * 1024);
        require(config.is_object() && config.as_object().contains("system") && config.at("system").is_object() &&
            config.at("system").as_object().contains("limits") && config.at("system").at("limits").is_object(), ErrorCode::InvalidInput, "frozen configuration lacks system limits");
        const auto& limits = config.at("system").at("limits").as_object();
        const auto* value = limits.if_contains(name);
        require(value && (value->is_int64() || value->is_uint64()), ErrorCode::InvalidInput, "missing frozen limit " + std::string(name));
        const auto n = number(limits, name);
        require(n > 0 && n <= 10000, ErrorCode::InvalidInput, "frozen limit out of bounds");
        return n;
    }

    std::map<std::string, Row> independent_nodes(const Row& request, const Json& body) const {
        require(body.is_object() && body.as_object().contains("nodes") && body.at("nodes").is_array(),
                ErrorCode::InvalidInput, "independent plan requires nodes");
        const auto& values = body.at("nodes").as_array();
        require(!values.empty() && values.size() <= 20 && values.size() <= static_cast<std::size_t>(configured_limit(request, "max_teams_per_request")),
                ErrorCode::Quota, "independent plan exceeds its frozen team capacity");
        const auto config = FrozenRequestConfig::restore(str(request, "config_snapshot"), str(request, "config_snapshot_sha256"));
        std::map<std::string, Row> nodes;
        for (const auto& value : values) {
            require(value.is_object(), ErrorCode::InvalidInput, "independent plan node must be an object");
            const auto& node = value.as_object();
            for (const auto* key : {"node_id", "kind"}) require(node.contains(key) && node.at(key).is_string(), ErrorCode::InvalidInput, "plan node identity is missing");
            const auto id = str(node, "node_id"); text_field(id, "node_id", 48);
            require(id.front() != '-' && std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }),
                    ErrorCode::InvalidInput, "invalid independent node_id");
            require(str(node, "kind") == "team_task" && node.contains("depends_on") && node.at("depends_on").is_array() &&
                node.at("depends_on").as_array().empty() && !node.contains("review") && !node.contains("capability") && !node.contains("input"),
                ErrorCode::Unsupported, "independent plan cannot include dependencies, capabilities or automatic reviews");
            require(node.contains("brief"), ErrorCode::InvalidInput, "independent plan node requires a TaskBrief");
            brief_constraints(request, node.at("brief"));
            const auto selected = node.contains("template") ? str(node, "template") : "default";
            (void)config.worker_tools(selected);
            if (node.contains("required")) require(node.at("required").is_bool(), ErrorCode::InvalidInput, "node required must be boolean");
            require(nodes.emplace(id, node).second, ErrorCode::InvalidInput, "duplicate independent node_id");
        }
        (void)json_text(body);
        return nodes;
    }
    void check_dispositions(const Json& dispositions, const std::map<std::string, Row>& nodes) const {
        require(dispositions.is_object() && dispositions.as_object().size() <= nodes.size(), ErrorCode::InvalidInput, "AMEND dispositions must be keyed by existing node_id");
        for (const auto& item : dispositions.as_object()) {
            require(nodes.contains(std::string(item.key())) && item.value().is_object(), ErrorCode::InvalidInput, "AMEND disposition names an unknown node");
            const auto& decision = item.value().as_object();
            require(decision.size() == 2 && decision.contains("disposition") && decision.at("disposition").is_string() &&
                decision.contains("rationale") && decision.at("rationale").is_string(), ErrorCode::InvalidInput, "disposition requires exactly disposition and rationale");
            const auto kind = str(decision, "disposition"); const auto reason = str(decision, "rationale");
            require(kind == "update" || kind == "keep" || kind == "cancel", ErrorCode::InvalidInput, "unknown AMEND disposition");
            text_field(reason, "AMEND disposition rationale", 4096);
            require(reason.find_first_not_of(" \t\r\n") != std::string::npos, ErrorCode::InvalidInput, "AMEND disposition rationale is blank");
        }
    }
    void check_independent_identities(const std::vector<Row>& existing, const std::map<std::string, Row>& nodes) const {
        require(existing.empty() || existing.size() == nodes.size(), ErrorCode::Unsupported, "later independent plan must preserve every allocated node");
        for (const auto& task : existing) {
            const auto found = nodes.find(str(task, "node_id"));
            require(found != nodes.end(), ErrorCode::Unsupported, "later independent plan cannot remove or rename a node");
            const auto& node = found->second;
            const auto selected = node.contains("template") ? str(node, "template") : "default";
            require(str(task, "kind") == "team_task" && str(task, "template_id") == selected, ErrorCode::Unsupported, "later independent plan cannot replace a task template");
            const auto team = one("SELECT * FROM teams WHERE task_id=?", {str(task, "task_id")});
            require(str(team, "template_id") == selected, ErrorCode::Corrupt, "task and allocated team template disagree");
        }
    }
    Json allocate_independent(const Row& request, std::int64_t revision, const std::string& node_id, const Row& node) {
        const auto task_id = new_id("tsk"); const auto team_id = new_id("team"); const auto created = stamp();
        const auto template_id = node.contains("template") ? str(node, "template") : "default";
        const auto id = str(request, "request_id");
        execute("INSERT INTO tasks(task_id,request_id,node_id,plan_revision,based_on_requirements_revision,kind,template_id,status,brief,required,created_at,updated_at) VALUES (?,?,?,?,?,'team_task',?,'READY',?,?,?,?)",
            {task_id, id, node_id, revision, number(request, "requirements_revision"), template_id,
             json_text(node.at("brief")), !node.contains("required") || node.at("required").as_bool(), created, created});
        execute("INSERT INTO teams(team_id,request_id,task_id,lead_id,worker_id,template_id,status,paused_for_change,lead_config,worker_config,workspace_path,created_at,updated_at) VALUES (?,?,?,?,?,?,'ACTIVE',1,?,?,?,?,?)",
            {team_id, id, task_id, new_id("lead"), new_id("worker"), template_id,
             canonical_json(Row{{"request_snapshot_sha256", str(request, "config_snapshot_sha256")}}),
             canonical_json(Row{{"request_snapshot_sha256", str(request, "config_snapshot_sha256")}}),
             "unallocated:" + team_id, created, created});
        return Row{{"node_id", node_id}, {"task_id", task_id}, {"team_id", team_id}, {"state_version", 1},
            {"disposition", "created"}, {"binding", to_json(binding(task_id))}};
    }
    Json apply_independent_task(const Row& request, const Row& task, std::int64_t revision,
                               const Row& node, const Json& decision) {
        const auto request_id = str(request, "request_id"); const auto task_id = str(task, "task_id");
        const auto requirements = number(request, "requirements_revision");
        const auto expected = number(task, "state_version");
        const auto task_revision = next(number(task, "task_revision"));
        const bool amended = number(task, "based_on_requirements_revision") != requirements;
        require(!terminal_request(str(request, "status")) && str(task, "status") != "CANCELLED" &&
            str(task, "status") != "SKIPPED" && str(task, "status") != "SUPERSEDED", ErrorCode::NotReady, "task cannot be reactivated");
        const auto disposition = amended ? str(decision.as_object(), "disposition") : "rebind";
        require(amended || decision.is_null(), ErrorCode::Conflict, "AMEND task already has a disposition for these requirements");
        require(amended || revision > number(task, "plan_revision"), ErrorCode::Stale, "plan rebind requires a newer plan");
        if (amended) require(requirements >= 2 && number(task, "based_on_requirements_revision") < requirements, ErrorCode::Stale, "invalid AMEND target revision");
        const auto team = one("SELECT * FROM teams WHERE task_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {task_id});
        const auto team_id = str(team, "team_id");
        cas("UPDATE tasks SET task_revision=?,based_on_requirements_revision=?,plan_revision=?,status=?,brief=?,required=?,accepted_with_issues=0,review_round=0,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
            {task_revision, requirements, revision, disposition == "cancel" ? "CANCELLED" : "READY",
             disposition == "cancel" ? task.at("brief") : Json(json_text(node.at("brief"))),
             !node.contains("required") || node.at("required").as_bool(), stamp(), task_id, expected});
        if (amended) execute("INSERT INTO amend_dispositions VALUES (?,?,?,?,?,?,?,?)",
            {request_id, requirements, task_id, disposition, task_revision, revision, str(decision.as_object(), "rationale"), stamp()});
        execute("UPDATE teams SET paused_for_change=1,status=CASE WHEN ?='cancel' THEN 'DRAINING' ELSE status END,state_version=state_version+1,updated_at=? WHERE team_id=?",
            {disposition, stamp(), team_id});
        fence_ops(request_id, team_id);
        return Row{{"node_id", str(task, "node_id")}, {"task_id", task_id}, {"team_id", team_id}, {"disposition", disposition},
            {"expected_task_version", expected}, {"state_version", next(expected)}, {"task_revision", task_revision}, {"plan_revision", revision},
            {"binding", to_json(binding(task_id))}};
    }

    Json prepare_operation(const StoreActor& actor, const ExecutionBinding& frozen,
                           std::int64_t expected, const std::string& business_key,
                           const std::string& tool_id, const Json& arguments,
                           const std::string& side_effect, const std::string& command_id,
                           bool registered);

    sqlite3* db{};
    int lease_fd{-1};
    StoreOpenMode mode;
    bool tools_schema{false};
    bool events_schema{false};
    bool workflow_schema{false};
    bool acceptance_schema{false};
    bool cleanup_schema{false};
    Json owned_info(const Row& row) const;
    Array cleanup_blockers(const std::string& id) const;
    Json cleanup_status(const std::string& id) const;
    void cleanup_authorize(const StoreActor& actor, const Json& claim) const;
    void cleanup_publication(const std::string& id, const Json& publication) const;
    Json cleanup_process_facts(const std::string& id) const;
    std::string database_file_path;
    mutable std::mutex mutex;
};

Store::Store(const std::filesystem::path& path, std::string_view base, std::string_view migration, StoreOpenMode mode,
             std::string_view tools_migration, std::string_view events_migration,
             std::string_view workflow_migration, std::string_view acceptance_migration, std::string_view cleanup_migration)
    : impl_(std::make_unique<Impl>(path, base, migration, mode, tools_migration, events_migration, workflow_migration, acceptance_migration, cleanup_migration)) {}
Store::~Store() = default;

Json Store::diagnostics() const {
    const std::lock_guard lock(impl_->mutex);
    return Row{{"sqlite_version", sqlite3_libversion()}, {"user_version", number(impl_->one("PRAGMA user_version"), "user_version")},
        {"journal_mode", str(impl_->one("PRAGMA journal_mode"), "journal_mode")}, {"synchronous", number(impl_->one("PRAGMA synchronous"), "synchronous")},
        {"foreign_keys", number(impl_->one("PRAGMA foreign_keys"), "foreign_keys")}, {"read_only", impl_->mode == StoreOpenMode::ReadOnly}};
}
Json Store::request(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return exposed(impl_->request_row(id));
}
Json Store::plan(const std::string& id, std::int64_t revision) const {
    const std::lock_guard lock(impl_->mutex);
    const auto r = impl_->request_row(id);
    if (revision == 0) revision = number(r, "plan_revision");
    if (revision == 0) return nullptr;
    require(revision > 0, ErrorCode::InvalidInput, "invalid plan revision");
    return exposed(impl_->one("SELECT * FROM plans WHERE request_id=? AND plan_revision=?", {id, revision}));
}
Json Store::task(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return exposed(impl_->task_row(id));
}
Json Store::instruction(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex);
    const auto t = impl_->task_row(id);
    if (number(t, "instruction_revision") == 0) return nullptr;
    return exposed(impl_->current_instruction(t));
}
void Store::validate_brief(const std::string& id, const Json& brief) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->brief_constraints(impl_->request_row(id), brief);
}
Json Store::operation(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return exposed(impl_->op_row(id));
}
Json Store::budget(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->one("SELECT * FROM request_budgets WHERE request_id=?", {id});
}
Json Store::attempt(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return exposed(impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {id}));
}
ExecutionBinding Store::binding(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->binding(id);
}
ExecutionBinding Store::request_binding(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->request_binding(id);
}
Json Store::request_snapshot(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex);
    const auto r = impl_->request_row(id);
    Array texts;
    for (const auto& row : impl_->all("SELECT requirements_revision,kind,text FROM request_texts WHERE request_id=? ORDER BY requirements_revision", {id})) texts.emplace_back(row);
    Array catalog;
    for (const auto& row : impl_->all("SELECT constraint_id,text,source_revision FROM request_constraints WHERE request_id=? AND requirements_revision=? ORDER BY constraint_id", {id, number(r, "requirements_revision")})) catalog.emplace_back(row);
    return Row{{"request_id", id}, {"requirements_revision", number(r, "requirements_revision")}, {"texts", texts}, {"constraint_catalog", catalog}};
}
std::vector<Json> Store::events(const std::string& id, std::int64_t after_seq, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    require(after_seq >= 0 && limit > 0 && limit <= 1000, ErrorCode::InvalidInput, "invalid event page");
    (void)impl_->request_row(id);
    std::vector<Json> result;
    for (const auto& row : impl_->all("SELECT * FROM events WHERE request_id=? AND seq>? ORDER BY seq LIMIT ?", {id, after_seq, limit})) result.push_back(exposed(row));
    return result;
}
std::vector<Json> Store::tasks(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex);
    (void)impl_->request_row(id);
    std::vector<Json> result;
    for (const auto& row : impl_->all("SELECT t.*,g.team_id,g.lead_id,g.worker_id,g.execution_epoch,g.paused_for_change FROM tasks t LEFT JOIN teams g ON g.task_id=t.task_id AND g.status IN ('STARTING','ACTIVE','DRAINING') WHERE t.request_id=? ORDER BY t.node_id", {id})) result.push_back(exposed(row));
    return result;
}
Json Store::pending_recovery() const {
    const std::lock_guard lock(impl_->mutex);
    auto collect = [&](std::string_view sql) { Array result; for (const auto& row : impl_->all(sql)) result.emplace_back(exposed(row)); return result; };
    auto result = Row{{"unknown_operations", collect("SELECT op_id,request_id,task_id,team_id,status FROM ops WHERE status='UNKNOWN'")},
        {"unknown_attempts", collect("SELECT attempt_id,request_id,status FROM call_attempts WHERE status='UNKNOWN' OR unknown_input=1 OR unknown_output=1 OR unknown_cost=1")},
        {"experience_jobs", collect("SELECT * FROM experience_jobs WHERE status<>'DONE'")},
        {"cleanup_jobs", collect("SELECT * FROM cleanup_jobs WHERE status<>'DONE'")},
        {"unconfirmed_delivery", collect("SELECT request_id,delivery_status FROM requests WHERE delivery_status='STORED'")}};
    if (impl_->acceptance_schema) result["delivery_intents"] = collect("SELECT delivery_id,request_id,requirements_revision,plan_revision,manifest_sha256,status,state_version FROM delivery_runtime WHERE status IN ('PREPARED','STORED')");
    Array capacity_holds;
    for (const auto& request : impl_->all("SELECT request_id FROM requests")) {
        const auto state = impl_->capacity_state(str(request, "request_id"));
        if (state.at("status") == "WAITING_INPUT") capacity_holds.emplace_back(state);
    }
    result["capacity_holds"] = std::move(capacity_holds);
    return result;
}
bool Store::event_runtime_enabled() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->events_schema;
}

std::vector<Json> Store::inbox(const StoreActor& actor, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema();
    require(limit > 0 && limit <= 256, ErrorCode::InvalidInput, "inbox limit must be 1..256");
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    std::vector<Json> result;
    for (const auto& row : impl_->all("SELECT i.*,e.event_id,e.type,e.payload,e.result,e.created_at FROM event_inbox i JOIN events e ON e.seq=i.event_seq WHERE i.request_id=? AND i.role=? AND i.actor_task_id=? AND i.status<>'DONE' ORDER BY i.inbox_id LIMIT ?",
            {actor.request_id, role_name(actor.role), actor.task_id, limit})) result.push_back(exposed(row));
    return result;
}

std::vector<Json> Store::request_inbox(const std::string& request_id, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema(); (void)impl_->request_row(request_id);
    require(limit > 0 && limit <= 163840, ErrorCode::InvalidInput, "request inbox limit must be 1..163840");
    require(number(impl_->one("SELECT COUNT(*) AS n FROM event_inbox WHERE request_id=? AND status<>'DONE'", {request_id}), "n") <= limit,
            ErrorCode::Quota, "complete request inbox exceeds requested record capacity");
    std::vector<Json> result;
    // Request-wide status needs each exact identity, not repeated large plan
    // payloads. Actor inbox/events retain the complete command and result.
    for (const auto& row : impl_->all("SELECT i.*,e.event_id,e.type,e.created_at FROM event_inbox i JOIN events e ON e.seq=i.event_seq WHERE i.request_id=? AND i.status<>'DONE' ORDER BY i.inbox_id", {request_id}))
        result.push_back(exposed(row));
    return result;
}

Json Store::claim_events(const StoreActor& actor, std::int64_t max_items) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema();
    require(max_items > 0 && max_items <= 256, ErrorCode::InvalidInput, "event claim limit must be 1..256");
    Impl::Transaction tx(*impl_);
    const auto request = impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    const auto revision = number(request, "requirements_revision");
    const Array scope{actor.request_id, role_name(actor.role), actor.task_id};
    const auto old = impl_->optional("SELECT * FROM event_actor_claims WHERE request_id=? AND role=? AND actor_task_id=?", scope);
    const auto generation = old ? next(number(*old, "generation")) : 1;
    if (old && str(*old, "status") == "ACTIVE") {
        require(number(*old, "requirements_revision") != revision || number(*old, "dispatcher_epoch") != actor.dispatcher_epoch ||
                number(*old, "execution_epoch") != actor.execution_epoch, ErrorCode::Conflict, "actor already has an active event claim");
        impl_->execute("UPDATE event_inbox SET status='PENDING',claim_id=NULL WHERE claim_id=? AND status='CLAIMED'", {str(*old, "claim_id")});
        impl_->execute("UPDATE event_actor_claims SET status='RELEASED',updated_at=? WHERE claim_id=?", {stamp(), str(*old, "claim_id")});
    }
    const auto pending = impl_->all("SELECT i.*,e.event_id,e.type,e.payload,e.result,e.created_at FROM event_inbox i JOIN events e ON e.seq=i.event_seq WHERE i.request_id=? AND i.role=? AND i.actor_task_id=? AND i.status='PENDING' ORDER BY i.inbox_id LIMIT ?",
        {actor.request_id, role_name(actor.role), actor.task_id, max_items});
    if (pending.empty()) { tx.commit(); return Row{{"claim_id", ""}, {"generation", 0}, {"items", Array{}}}; }
    const auto claim_id = new_id("claim");
    Array ids; Array items;
    for (auto row : pending) {
        const auto id = number(row, "inbox_id"); ids.emplace_back(id);
        impl_->cas("UPDATE event_inbox SET status='CLAIMED',claim_id=? WHERE inbox_id=? AND status='PENDING'", {claim_id, id});
        row["status"] = "CLAIMED"; row["claim_id"] = claim_id; items.emplace_back(exposed(std::move(row)));
    }
    impl_->execute("INSERT INTO event_actor_claims(request_id,role,actor_task_id,principal_id,team_id,generation,claim_id,status,requirements_revision,dispatcher_epoch,execution_epoch,captured_ids,created_at,updated_at) VALUES (?,?,?,?,?,?,?,'ACTIVE',?,?,?,?,?,?) ON CONFLICT(request_id,role,actor_task_id) DO UPDATE SET principal_id=excluded.principal_id,team_id=excluded.team_id,generation=excluded.generation,claim_id=excluded.claim_id,status=excluded.status,requirements_revision=excluded.requirements_revision,dispatcher_epoch=excluded.dispatcher_epoch,execution_epoch=excluded.execution_epoch,captured_ids=excluded.captured_ids,created_at=excluded.created_at,updated_at=excluded.updated_at",
        {actor.request_id, role_name(actor.role), actor.task_id, actor.principal_id, actor.team_id, generation, claim_id, revision,
         actor.dispatcher_epoch, actor.execution_epoch, canonical_json(ids), stamp(), stamp()});
    tx.commit();
    return Row{{"claim_id", claim_id}, {"generation", generation}, {"requirements_revision", revision},
        {"dispatcher_epoch", actor.dispatcher_epoch}, {"execution_epoch", actor.execution_epoch}, {"items", items}};
}

Json Store::finish_events(const StoreActor& actor, const std::string& claim_id, std::int64_t generation,
                         const std::vector<std::int64_t>& completed_ids) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema(); text_field(claim_id, "event claim id");
    require(generation > 0 && completed_ids.size() <= 256, ErrorCode::InvalidInput, "invalid event claim completion");
    Impl::Transaction tx(*impl_);
    const auto request = impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    const auto claim = impl_->optional("SELECT * FROM event_actor_claims WHERE request_id=? AND role=? AND actor_task_id=?", {actor.request_id, role_name(actor.role), actor.task_id});
    require(claim && str(*claim, "claim_id") == claim_id && number(*claim, "generation") == generation &&
        str(*claim, "principal_id") == actor.principal_id && str(*claim, "team_id") == actor.team_id &&
        number(*claim, "requirements_revision") == number(request, "requirements_revision") &&
        number(*claim, "dispatcher_epoch") == actor.dispatcher_epoch && number(*claim, "execution_epoch") == actor.execution_epoch,
        ErrorCode::Stale, "event claim belongs to an obsolete owner or requirement revision");
    require(str(*claim, "status") == "ACTIVE" || str(*claim, "status") == "CLOSED", ErrorCode::Stale, "event claim was released during recovery");
    std::set<std::int64_t> completed;
    for (const auto id : completed_ids) require(id > 0 && completed.insert(id).second, ErrorCode::InvalidInput, "duplicate or invalid completed event id");
    const auto captured = parse_json_strict(str(*claim, "captured_ids")).as_array();
    std::set<std::int64_t> captured_set;
    for (const auto& id : captured) captured_set.insert(id.as_int64());
    for (const auto id : completed) require(captured_set.contains(id), ErrorCode::Unauthorized, "cannot complete an event outside the captured claim");
    Array done; Array released;
    const bool replay = str(*claim, "status") == "CLOSED";
    for (const auto& value : captured) {
        const auto id = value.as_int64(); const bool finish = completed.contains(id);
        if (replay) {
            const auto row = impl_->one("SELECT status FROM event_inbox WHERE inbox_id=?", {id});
            require((str(row, "status") == "DONE") == finish, ErrorCode::Conflict, "closed event claim completion changed");
        } else impl_->cas("UPDATE event_inbox SET status=?,claim_id=NULL,completed_at=? WHERE inbox_id=? AND claim_id=? AND status='CLAIMED'",
            {finish ? "DONE" : "PENDING", finish ? Json(stamp()) : Json(nullptr), id, claim_id});
        (finish ? done : released).emplace_back(id);
    }
    if (!replay) impl_->cas("UPDATE event_actor_claims SET status='CLOSED',updated_at=? WHERE claim_id=? AND status='ACTIVE'", {stamp(), claim_id});
    tx.commit();
    return Row{{"claim_id", claim_id}, {"generation", generation}, {"completed_ids", done}, {"released_ids", released}};
}

Json Store::worker_wait(const std::string& task_id) const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->wait_info(task_id);
}

Json Store::wait_worker(const StoreActor& actor, const ExecutionBinding& binding,
                       std::int64_t expected, const std::string& reason, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema(); text_field(reason, "Worker wait reason", 4096);
    const auto command = payload(actor, "wait_worker", Row{{"binding", to_json(binding)}, {"expected_task_version", expected}, {"reason", reason}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Worker}, false); impl_->actor_binding(actor, binding);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto request = impl_->authorize(actor, {Role::Worker}); impl_->matches(binding);
    require(!impl_->has_blocking_question(actor.task_id), ErrorCode::NotReady,
            "an unanswered blocking question owns this Worker wait");
    const auto task = impl_->task_row(actor.task_id);
    require(!terminal_request(str(request, "status")) && !terminal_task(str(task, "status")), ErrorCode::NotReady, "terminal work cannot enter Worker wait");
    if (impl_->workflow_schema) require(str(task, "status") != "SUBMITTED" && str(task, "status") != "REVIEWING" && str(task, "status") != "BLOCKED",
            ErrorCode::NotReady, "a submission workflow already owns this Worker hold");
    require(number(task, "based_on_requirements_revision") == number(request, "requirements_revision") && number(task, "plan_revision") == number(request, "plan_revision"),
            ErrorCode::NotReady, "Worker wait requires a current task disposition and plan");
    const auto old = impl_->optional("SELECT * FROM worker_waits WHERE task_id=?", {actor.task_id});
    require(!old || str(*old, "status") != "WAITING", ErrorCode::NotReady, "Worker already has an active wait");
    const auto generation = old ? next(number(*old, "generation")) : 1;
    impl_->cas("UPDATE tasks SET state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {stamp(), actor.task_id, expected});
    impl_->execute("INSERT INTO worker_waits(task_id,request_id,team_id,generation,status,original_binding,reason,created_at,updated_at) VALUES (?,?,?,?,'WAITING',?,?,?,?) ON CONFLICT(task_id) DO UPDATE SET request_id=excluded.request_id,team_id=excluded.team_id,generation=excluded.generation,status=excluded.status,original_binding=excluded.original_binding,reason=excluded.reason,created_at=excluded.created_at,updated_at=excluded.updated_at",
        {actor.task_id, actor.request_id, actor.team_id, generation, canonical_json(to_json(binding)), reason, stamp(), stamp()});
    const auto result = impl_->wait_info(actor.task_id);
    impl_->event(actor.request_id, command_id, command, result, "worker");
    tx.commit(); return result;
}

Json Store::wake_worker(const StoreActor& actor, const ExecutionBinding& binding,
                       std::int64_t generation, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema(); require(generation > 0, ErrorCode::InvalidInput, "wait generation must be positive");
    const auto command = payload(actor, "wake_worker", Row{{"binding", to_json(binding)}, {"expected_wait_generation", generation}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Worker}, false); impl_->actor_binding(actor, binding);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto request = impl_->authorize(actor, {Role::Worker}); impl_->matches(binding);
    require(!impl_->has_blocking_question(actor.task_id), ErrorCode::NotReady,
            "answer the owning blocking question before waking this Worker");
    const auto task = impl_->task_row(actor.task_id);
    require(!terminal_request(str(request, "status")) && !terminal_task(str(task, "status")), ErrorCode::NotReady, "terminal Worker cannot wake");
    const auto waiting = impl_->optional("SELECT * FROM worker_waits WHERE task_id=?", {actor.task_id});
    require(waiting && number(*waiting, "generation") == generation && str(*waiting, "status") == "WAITING", ErrorCode::Stale, "wait generation is absent or no longer waiting");
    auto original = execution_binding_from_json(parse_json_strict(str(*waiting, "original_binding")));
    original.dispatcher_epoch = binding.dispatcher_epoch; original.execution_epoch = binding.execution_epoch;
    original.policy_epoch = binding.policy_epoch;
    require(original == binding && number(task, "based_on_requirements_revision") == number(request, "requirements_revision") &&
        number(task, "plan_revision") == number(request, "plan_revision"), ErrorCode::Stale, "wait target changed semantically");
    impl_->cas("UPDATE worker_waits SET status='AWAKE',updated_at=? WHERE task_id=? AND generation=? AND status='WAITING'", {stamp(), actor.task_id, generation});
    impl_->cas("UPDATE tasks SET state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {stamp(), actor.task_id, number(task, "state_version")});
    const auto result = impl_->wait_info(actor.task_id);
    impl_->event(actor.request_id, command_id, command, result, "worker");
    tx.commit(); return result;
}

Json Store::record_tool_feedback(const std::string& attempt_id, const ExecutionBinding& original,
                                const Json& response, const Json& result) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema(); text_field(attempt_id, "feedback attempt id");
    const Row fact{{"attempt_id", attempt_id}, {"original_binding", to_json(original)}, {"response", response}, {"result", result}};
    const auto hash = sha256_hex(json_text(fact, 8 * 1024 * 1024));
    const auto response_text = json_text(response, 4 * 1024 * 1024);
    const auto result_text = json_text(result, 4 * 1024 * 1024);
    Impl::Transaction tx(*impl_);
    const auto attempt = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {attempt_id});
    require(execution_binding_from_json(parse_json_strict(str(attempt, "frozen_binding"))) == original, ErrorCode::Conflict, "feedback changed the original attempt binding");
    require(!attempt.at("settlement_sha256").is_null() && !attempt.at("response").is_null(), ErrorCode::NotReady, "feedback requires a durably settled original response");
    require(str(attempt, "response") == response_text, ErrorCode::Conflict, "feedback response differs from the settled original response");
    if (const auto old = impl_->optional("SELECT record_sha256 FROM model_tool_feedback WHERE attempt_id=?", {attempt_id})) {
        require(str(*old, "record_sha256") == hash, ErrorCode::Conflict, "attempt feedback already records different original facts");
        const auto value = impl_->feedback_info(attempt_id); tx.commit(); return value;
    }
    impl_->execute("INSERT INTO model_tool_feedback VALUES (?,?,?,?,?,?)", {attempt_id, canonical_json(to_json(original)), response_text, result_text, hash, stamp()});
    const auto value = impl_->feedback_info(attempt_id);
    // The event contains a reference/hash, not another copy of compiled context.
    impl_->event(str(attempt, "request_id"), "feedback:" + attempt_id,
        Row{{"type", "tool_feedback"}, {"attempt_id", attempt_id}, {"binding", to_json(original)}, {"record_sha256", hash}},
        Row{{"attempt_id", attempt_id}, {"record_sha256", hash}});
    tx.commit(); return value;
}

Json Store::tool_feedback(const std::string& attempt_id) const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->feedback_info(attempt_id);
}

std::vector<Json> Store::attempt_history(const StoreActor& actor, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema();
    require(limit > 0 && limit <= 256, ErrorCode::InvalidInput, "attempt history limit must be 1..256");
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    const Array scope{actor.request_id, role_name(actor.role), actor.principal_id, actor.task_id, actor.team_id};
    require(number(impl_->one("SELECT COUNT(*) AS n FROM call_attempts WHERE request_id=? AND role=? AND principal_id=? AND COALESCE(task_id,'')=? AND COALESCE(team_id,'')=?", scope), "n") <= limit,
            ErrorCode::Quota, "complete attempt history exceeds requested record capacity");
    // Fetch the scalar index, never recursively fetch the former input context.
    Statement query(impl_->db, "SELECT attempt_id,call_id,request_id,task_id,team_id,role,principal_id,config_snapshot_sha256,model_ref,frozen_binding,json_extract(request_envelope,'$.role_call_index') AS role_call_index,status,accepted,input_tokens,output_tokens,cache_read_tokens,cache_write_tokens,cost_micro_usd,unknown_input,unknown_output,unknown_cost,response,usage,created_at,settled_at FROM call_attempts WHERE request_id=? AND role=? AND principal_id=? AND COALESCE(task_id,'')=? AND COALESCE(team_id,'')=? ORDER BY rowid", scope);
    std::vector<Json> result; std::size_t bytes = 0;
    while (query.step()) {
        auto record = exposed(query.row());
        const auto feedback = impl_->feedback_info(str(record.as_object(), "attempt_id"));
        record.as_object()["feedback"] = feedback;
        record.as_object()["feedback_missing"] = feedback.is_null();
        const auto encoded = json_text(record, 8 * 1024 * 1024);
        require(encoded.size() <= 8U * 1024U * 1024U - bytes, ErrorCode::Quota, "complete attempt history exceeds byte capacity");
        bytes += encoded.size(); result.emplace_back(std::move(record));
    }
    return result;
}

#include "store_history.inc"

bool Store::workflow_runtime_enabled() const {
    const std::lock_guard lock(impl_->mutex); return impl_->workflow_schema;
}

Json Store::question(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->question_info(id);
}

std::vector<Json> Store::questions(const std::string& request_id, const std::string& task_id,
                                  std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_workflow_schema(); (void)impl_->request_row(request_id);
    require(limit > 0 && limit <= 128, ErrorCode::InvalidInput, "questions limit must be 1..128");
    if (!task_id.empty()) require(str(impl_->task_row(task_id), "request_id") == request_id,
            ErrorCode::Unauthorized, "question task belongs to another request");
    const auto filter = std::string(task_id.empty() ? "" : " AND task_id=?");
    const Array args = task_id.empty() ? Array{request_id} : Array{request_id, task_id};
    require(number(impl_->one("SELECT COUNT(*) AS n FROM questions WHERE request_id=?" + filter, args), "n") <= limit,
            ErrorCode::Quota, "complete question history exceeds requested capacity");
    std::vector<Json> result; std::size_t bytes = 0;
    for (const auto& row : impl_->all("SELECT question_id FROM questions WHERE request_id=?" + filter + " ORDER BY rowid", args)) {
        auto value = impl_->question_info(str(row, "question_id")); const auto encoded = json_text(value);
        require(encoded.size() <= 8U * 1024U * 1024U - bytes, ErrorCode::Quota, "complete question history exceeds byte capacity");
        bytes += encoded.size(); result.emplace_back(std::move(value));
    }
    return result;
}

Json Store::submission(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->submission_info(id);
}

std::vector<Json> Store::submissions(const std::string& task_id, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_workflow_schema(); (void)impl_->task_row(task_id);
    require(limit > 0 && limit <= 64, ErrorCode::InvalidInput, "submissions limit must be 1..64");
    require(number(impl_->one("SELECT COUNT(*) AS n FROM workflow_submissions WHERE task_id=?", {task_id}), "n") <= limit,
            ErrorCode::Quota, "complete submission history exceeds requested capacity");
    std::vector<Json> result; std::size_t bytes = 0;
    for (const auto& row : impl_->all("SELECT submission_id FROM workflow_submissions WHERE task_id=? ORDER BY rowid", {task_id})) {
        auto value = impl_->submission_info(str(row, "submission_id")); const auto encoded = json_text(value, 2 * 1024 * 1024);
        require(encoded.size() <= 8U * 1024U * 1024U - bytes, ErrorCode::Quota, "complete submission history exceeds byte capacity");
        bytes += encoded.size(); result.emplace_back(std::move(value));
    }
    return result;
}

Json Store::current_submission(const std::string& task_id) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_workflow_schema(); (void)impl_->task_row(task_id);
    const auto row = impl_->optional("SELECT submission_id FROM workflow_submissions WHERE task_id=? AND status IN ('SUBMITTED','CANDIDATE_READY','BLOCKED')", {task_id});
    return row ? impl_->submission_info(str(*row, "submission_id")) : Json(nullptr);
}

Json Store::ask_question(const StoreActor& actor, const ExecutionBinding& binding,
                         std::int64_t expected, const Json& body, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_workflow_schema();
    object_fields(body, {"text", "context", "options", "blocking"}, {"text"});
    (void)workflow_text(body, "text", 16000); (void)workflow_text(body, "context", 16000, true);
    bool blocking = true;
    if (const auto* value = body.as_object().if_contains("blocking")) {
        require(value->is_bool(), ErrorCode::InvalidInput, "question blocking must be a boolean"); blocking = value->as_bool();
    }
    if (const auto* options = body.as_object().if_contains("options")) {
        require(options->is_array() && options->as_array().size() <= 8, ErrorCode::InvalidInput, "question options exceed capacity");
        std::set<std::string> ids;
        for (const auto& option : options->as_array()) {
            object_fields(option, {"id", "label"}, {"id", "label"});
            const auto id = workflow_text(option, "id", 16); (void)workflow_text(option, "label", 800);
            require(std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }) &&
                    ids.insert(id).second, ErrorCode::InvalidInput, "question option identifiers must be unique");
        }
    }
    (void)json_text(body, 65536);
    const auto command = payload(actor, "ask_question", Row{{"binding", to_json(binding)}, {"expected_task_version", expected}, {"body", body}});
    Impl::Transaction tx(*impl_);
    const auto owner = impl_->authorize(actor, {Role::Worker}, false); impl_->actor_binding(actor, binding);
    impl_->tool_grant(owner, impl_->team_row(actor.team_id), "ask");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto request = impl_->authorize(actor, {Role::Worker}); impl_->matches(binding);
    const auto task = impl_->task_row(actor.task_id);
    require(!terminal_request(str(request, "status")) && !terminal_task(str(task, "status")), ErrorCode::NotReady, "terminal Worker cannot ask a new question");
    impl_->capacity_available(actor.request_id, "questions");
    impl_->capacity_available(actor.request_id, "open_questions");
    const auto wait = impl_->optional("SELECT * FROM worker_waits WHERE task_id=?", {actor.task_id});
    if (blocking) require((!wait || str(*wait, "status") != "WAITING") && !impl_->has_blocking_question(actor.task_id),
            ErrorCode::NotReady, "another wait already owns this Worker");
    const auto generation = blocking ? (wait ? next(number(*wait, "generation")) : 1) : 0;
    const auto id = new_id("q"); auto complete = body.as_object();
    complete["question_id"] = id; complete["task_id"] = actor.task_id; complete["asked_by"] = "worker"; complete["blocking"] = blocking;
    const auto original = Row{{"body", complete}, {"original_binding", to_json(binding)}};
    impl_->cas("UPDATE tasks SET state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {stamp(), actor.task_id, expected});
    impl_->execute("INSERT INTO questions(question_id,request_id,task_id,team_id,asked_by,holder,body,created_at) VALUES (?,?,?,?,'worker','team_lead',?,?)",
        {id, actor.request_id, actor.task_id, actor.team_id, json_text(complete, 65536), stamp()});
    impl_->execute("INSERT INTO question_runtime VALUES (?,?,?,'OPEN',?,?)", {id, canonical_json(to_json(binding)), sha256_hex(canonical_json(original)), blocking, blocking ? Json(generation) : Json(nullptr)});
    if (blocking) impl_->execute("INSERT INTO worker_waits(task_id,request_id,team_id,generation,status,original_binding,reason,created_at,updated_at) VALUES (?,?,?,?,'WAITING',?,?,?,?) ON CONFLICT(task_id) DO UPDATE SET request_id=excluded.request_id,team_id=excluded.team_id,generation=excluded.generation,status=excluded.status,original_binding=excluded.original_binding,reason=excluded.reason,created_at=excluded.created_at,updated_at=excluded.updated_at",
        {actor.task_id, actor.request_id, actor.team_id, generation, canonical_json(to_json(binding)), "question:" + id, stamp(), stamp()});
    const auto result = impl_->question_info(id);
    impl_->event(actor.request_id, command_id, command, result, "worker"); tx.commit(); return result;
}

Json Store::forward_question(const StoreActor& actor, const ExecutionBinding& captured,
                             const std::string& id, std::int64_t expected,
                             const std::string& note, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_workflow_schema();
    require(expected > 0, ErrorCode::InvalidInput, "question state_version must be positive");
    if (!note.empty()) nonblank(note, "question forwarding note", 8000);
    const auto command = payload(actor, "forward_question", Row{{"binding", to_json(captured)}, {"question_id", id}, {"expected_question_version", expected}, {"note", note}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::TeamLead, Role::Dispatcher}, false); impl_->actor_binding(actor, captured);
    const auto question = impl_->question_row(id);
    require(str(question, "request_id") == actor.request_id && (actor.role != Role::TeamLead ||
        (str(question, "task_id") == actor.task_id && str(question, "team_id") == actor.team_id)), ErrorCode::Unauthorized, "question belongs to another actor");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    impl_->workflow_subject(actor, captured, question, true);
    require(str(question, "status") == "OPEN" && str(question, "holder") == role_name(actor.role),
            ErrorCode::NotReady, "only the current question holder can forward an open question");
    const auto holder = actor.role == Role::TeamLead ? "dispatcher" : "user";
    Row transition{{"role", role_name(actor.role)}, {"to", holder}};
    if (!note.empty()) transition["note"] = note;
    impl_->cas("UPDATE questions SET holder=?,state_version=state_version+1 WHERE question_id=? AND state_version=?", {holder, id, expected});
    impl_->execute("INSERT INTO question_transitions VALUES (?,?,'FORWARDED',?,?,?,?)",
        {id, next(expected), json_text(actor_json(actor)), canonical_json(to_json(captured)), json_text(transition, 16384), stamp()});
    const auto result = impl_->question_info(id);
    impl_->event(actor.request_id, command_id, command, result, role_name(actor.role)); tx.commit(); return result;
}

Json Store::answer_question(const StoreActor& actor, const ExecutionBinding& captured,
                            const std::string& id, std::int64_t expected,
                            const Json& answer, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_workflow_schema();
    require(expected > 0, ErrorCode::InvalidInput, "question state_version must be positive");
    object_fields(answer, {"text", "selected_option"});
    require(!answer.as_object().empty(), ErrorCode::InvalidInput, "answer needs text or selected_option");
    (void)workflow_text(answer, "text", 16000, true);
    const auto selected = workflow_text(answer, "selected_option", 16, true);
    (void)json_text(answer, 32768);
    const auto command = payload(actor, "answer_question", Row{{"binding", to_json(captured)}, {"question_id", id}, {"expected_question_version", expected}, {"answer", answer}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Root, Role::Dispatcher, Role::TeamLead}, false); impl_->actor_binding(actor, captured);
    const auto question = impl_->question_row(id);
    require(str(question, "request_id") == actor.request_id && (actor.role != Role::TeamLead ||
        (str(question, "task_id") == actor.task_id && str(question, "team_id") == actor.team_id)), ErrorCode::Unauthorized, "question belongs to another actor");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    impl_->workflow_subject(actor, captured, question, true);
    const auto responder = actor.role == Role::Root ? "user" : role_name(actor.role);
    require(str(question, "status") == "OPEN" && str(question, "holder") == responder,
            ErrorCode::NotReady, "only the current question holder can answer an open question");
    const auto body = parse_json_strict(str(question, "body"), 65536);
    if (!selected.empty()) {
        const auto* options = body.as_object().if_contains("options");
        require(options && options->is_array() && std::any_of(options->as_array().begin(), options->as_array().end(), [&](const Json& value) {
            return value.at("id").as_string() == selected;
        }), ErrorCode::InvalidInput, "answer selects an option absent from the original question");
    }
    auto complete = answer.as_object(); complete["answered_by"] = responder; complete["answered_at"] = stamp();
    complete["requirements_revision"] = captured.requirements_revision;
    const auto task_id = str(question, "task_id");
    const auto task = impl_->task_row(task_id);
    if (number(question, "blocking") != 0) {
        const auto wait = impl_->optional("SELECT * FROM worker_waits WHERE task_id=?", {task_id});
        require(wait && str(*wait, "status") == "WAITING" && number(*wait, "generation") == number(question, "wait_generation") &&
                str(*wait, "reason") == "question:" + id &&
                semantic_binding_equal(execution_binding_from_json(parse_json_strict(str(*wait, "original_binding"))), impl_->binding(task_id), true),
                ErrorCode::Stale, "answer no longer owns the original Worker wait");
        impl_->cas("UPDATE worker_waits SET status='AWAKE',updated_at=? WHERE task_id=? AND generation=? AND status='WAITING'",
            {stamp(), task_id, number(question, "wait_generation")});
    }
    impl_->cas("UPDATE questions SET holder='answered',answer=?,answered_at=?,state_version=state_version+1 WHERE question_id=? AND state_version=?",
        {json_text(complete, 32768), stamp(), id, expected});
    impl_->execute("UPDATE question_runtime SET status='ANSWERED' WHERE question_id=?", {id});
    impl_->execute("INSERT INTO question_transitions VALUES (?,?,'ANSWERED',?,?,?,?)",
        {id, next(expected), json_text(actor_json(actor)), canonical_json(to_json(captured)), json_text(complete, 32768), stamp()});
    impl_->cas("UPDATE tasks SET state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
        {stamp(), task_id, number(task, "state_version")});
    const auto result = impl_->question_info(id);
    impl_->event(actor.request_id, command_id, command, result, role_name(actor.role)); tx.commit(); return result;
}

Json Store::submit_work(const StoreActor& actor, const ExecutionBinding& binding,
                        std::int64_t expected, const Json& body,
                        const Json& artifact_snapshot, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_workflow_schema();
    object_fields(body, {"summary", "deliverables", "self_check", "verification", "known_issues", "used_memories"},
                  {"summary", "deliverables", "self_check"});
    (void)workflow_text(body, "summary", 16000); (void)json_text(body, 262144);
    if (const auto* memories = body.as_object().if_contains("used_memories"))
        require(memories->is_array() && memories->as_array().empty(), ErrorCode::Unsupported,
                "submission memory claims require the later versioned memory runner");
    require(body.at("deliverables").is_array() && !body.at("deliverables").as_array().empty() &&
            body.at("deliverables").as_array().size() <= 8, ErrorCode::Quota, "submission needs 1..8 complete file snapshots");
    std::set<std::string> paths;
    for (const auto& deliverable : body.at("deliverables").as_array()) {
        object_fields(deliverable, {"path", "description"}, {"path", "description"});
        const auto path = workflow_text(deliverable, "path", 500); work_path(path);
        (void)workflow_text(deliverable, "description", 2000);
        require(paths.insert(path).second, ErrorCode::InvalidInput, "submission deliverables contain duplicate paths");
    }
    require(artifact_snapshot.is_array() && artifact_snapshot.as_array().size() == paths.size(),
            ErrorCode::InvalidInput, "submission snapshot does not cover every deliverable");
    std::set<std::string> captured_paths; std::size_t total = 0;
    for (const auto& file : artifact_snapshot.as_array()) {
        object_fields(file, {"path", "sha256", "size_bytes", "content_base64"}, {"path", "sha256", "size_bytes", "content_base64"});
        const auto path = workflow_text(file, "path", 500); work_path(path);
        require(paths.contains(path) && captured_paths.insert(path).second, ErrorCode::InvalidInput, "submission snapshot contains a foreign or duplicate path");
        const auto hash = workflow_text(file, "sha256", 64);
        require(hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
                ErrorCode::InvalidInput, "snapshot SHA-256 must be lowercase hexadecimal");
        require(file.at("content_base64").is_string() && (file.at("size_bytes").is_int64() || file.at("size_bytes").is_uint64()),
                ErrorCode::InvalidInput, "snapshot requires raw bytes and an integer byte count");
        require((file.at("size_bytes").is_int64() && file.at("size_bytes").as_int64() >= 0 && file.at("size_bytes").as_int64() <= 65536) ||
                (file.at("size_bytes").is_uint64() && file.at("size_bytes").as_uint64() <= 65536),
                ErrorCode::InvalidInput, "snapshot file size must be 0..65536 bytes");
        const auto bytes = decoded_base64(file.at("content_base64").as_string());
        require(number(file.as_object(), "size_bytes") == static_cast<std::int64_t>(bytes.size()) && sha256_hex(bytes) == hash,
                ErrorCode::InvalidInput, "snapshot bytes, SHA-256 and size disagree");
        require(bytes.size() <= 262144U - total, ErrorCode::Quota, "submission files exceed the 256 KiB total capacity"); total += bytes.size();
    }
    (void)json_text(artifact_snapshot, 524288);
    const auto command = payload(actor, "submit_work", Row{{"binding", to_json(binding)}, {"expected_task_version", expected},
        {"body", body}, {"artifact_snapshot_sha256", sha256_hex(canonical_json(artifact_snapshot))}});
    Impl::Transaction tx(*impl_);
    const auto owner = impl_->authorize(actor, {Role::Worker}, false); impl_->actor_binding(actor, binding);
    const auto team = impl_->team_row(actor.team_id);
    impl_->tool_grant(owner, team, "submit"); impl_->tool_grant(owner, team, "fs_read");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    impl_->ready(actor, binding, "submit");
    (void)impl_->control_authorization(actor, binding, "fs_read", true);
    const auto task = impl_->task_row(actor.task_id);
    require(!impl_->has_blocking_question(actor.task_id), ErrorCode::NotReady, "submission has an unanswered blocking question");
    require(!impl_->optional("SELECT op_id FROM ops WHERE team_id=? AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN') LIMIT 1", {actor.team_id}),
            ErrorCode::NotReady, "submission requires every producer operation to be settled");
    require(!impl_->optional("SELECT submission_id FROM workflow_submissions WHERE task_id=? AND status IN ('SUBMITTED','CANDIDATE_READY','BLOCKED')", {actor.task_id}),
            ErrorCode::Conflict, "producer already has an unresolved submission");
    impl_->capacity_available(actor.request_id, "submissions_per_task", actor.task_id);
    impl_->capacity_available(actor.request_id, "submissions");
    require(number(team, "tool_calls_used") < impl_->configured_limit(owner, "worker_max_tool_calls_per_order"), ErrorCode::Quota, "instruction tool-call budget exhausted");
    const auto criteria = acceptance_ids(parse_json_strict(str(task, "brief")));
    criterion_results(body.at("self_check"), criteria, false, true); submission_verification(body, false);
    const auto id = new_id("sub"); auto complete = body.as_object(); complete["submission_id"] = id;
    const auto binding_document = to_json(binding);
    for (const auto& field : binding_document.as_object()) complete[field.key()] = field.value();
    const Row fact{{"body", complete}, {"original_binding", to_json(binding)}, {"artifact_snapshot", artifact_snapshot}};
    impl_->cas("UPDATE tasks SET status='SUBMITTED',state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {stamp(), actor.task_id, expected});
    impl_->execute("UPDATE teams SET tool_calls_used=tool_calls_used+1,state_version=state_version+1,updated_at=? WHERE team_id=?", {stamp(), actor.team_id});
    impl_->execute("INSERT INTO workflow_submissions(submission_id,request_id,task_id,team_id,original_binding,body,artifact_snapshot,record_sha256,status,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,'SUBMITTED',?,?)",
        {id, actor.request_id, actor.task_id, actor.team_id, canonical_json(to_json(binding)), json_text(complete, 262144),
         json_text(artifact_snapshot, 524288), sha256_hex(canonical_json(fact)), stamp(), stamp()});
    const auto result = impl_->submission_info(id);
    impl_->event(actor.request_id, command_id, command, result, "worker"); tx.commit(); return result;
}

Json Store::check_submission(const StoreActor& actor, const ExecutionBinding& captured,
                             std::int64_t expected_task, const std::string& id,
                             std::int64_t expected_submission, const Json& check_body,
                             const Json& rework_order, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_workflow_schema();
    require(expected_task > 0 && expected_submission > 0, ErrorCode::InvalidInput, "check requires captured task and submission versions");
    object_fields(check_body, {"passed", "notes", "criteria_results"}, {"passed"});
    require(check_body.at("passed").is_bool(), ErrorCode::InvalidInput, "check passed must be a boolean");
    const bool passed = check_body.at("passed").as_bool();
    (void)workflow_text(check_body, "notes", 8000, true); (void)json_text(check_body, 65536);
    if (passed) require(rework_order.is_null(), ErrorCode::InvalidInput, "passing check cannot issue a rework order");
    else work_order_shape(rework_order);
    auto command = payload(actor, "check_submission", Row{{"binding", to_json(captured)}, {"expected_task_version", expected_task},
        {"submission_id", id}, {"expected_submission_version", expected_submission}, {"check_body", check_body}, {"rework_order", rework_order},
        {"adverse_revoke", !passed && expected_submission > 1}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::TeamLead}, false); impl_->actor_binding(actor, captured);
    const auto submission = impl_->submission_row(id);
    require(str(submission, "request_id") == actor.request_id && str(submission, "task_id") == actor.task_id && str(submission, "team_id") == actor.team_id,
            ErrorCode::Unauthorized, "submission belongs to another TeamLead");
    // Replay identity is the original captured command, not the present status
    // that may already have changed as a result of that same command.
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    impl_->workflow_subject(actor, captured, submission, !passed);
    const auto request = impl_->request_row(actor.request_id); const auto task = impl_->task_row(actor.task_id);
    const auto status = str(submission, "status");
    require((status == "SUBMITTED" || (!passed && status == "CANDIDATE_READY")) && str(task, "status") == "SUBMITTED",
            ErrorCode::NotReady, "submission is not awaiting this local check");
    require(number(submission, "state_version") == expected_submission &&
            command.at("adverse_revoke").as_bool() == (status == "CANDIDATE_READY"),
            ErrorCode::Conflict, "check changed the captured submission or adverse-revocation target");
    require(!impl_->has_blocking_question(actor.task_id), ErrorCode::NotReady, "answer the pending question before checking this submission");
    require(number(task, "based_on_requirements_revision") == number(request, "requirements_revision") &&
            number(task, "plan_revision") == number(request, "plan_revision"), ErrorCode::Stale, "check requires the current task disposition and plan");
    require(!impl_->optional("SELECT op_id FROM ops WHERE team_id=? AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN') LIMIT 1", {actor.team_id}),
            ErrorCode::UnknownOutcome, "producer has unsettled operations");
    const auto brief = parse_json_strict(str(task, "brief")); const auto criteria = acceptance_ids(brief);
    if (const auto* checks = check_body.as_object().if_contains("criteria_results")) criterion_results(*checks, criteria, passed, false);
    else require(!passed, ErrorCode::InvalidInput, "passing TeamLead check must cover every current criterion");
    const auto submitted_body = parse_json_strict(str(submission, "body"), 262144);
    if (passed) {
        criterion_results(submitted_body.at("self_check"), criteria, true, true);
        submission_verification(submitted_body, true);
    } else {
        impl_->brief_constraints(request, rework_order);
        for (const auto& excerpt : brief.at("constraints_verbatim").as_array())
            require(contains(rework_order.at("constraints_verbatim"), excerpt.as_string()), ErrorCode::InvalidInput, "rework order omits an existing task constraint");
        const auto previous = impl_->one("SELECT body FROM instructions WHERE task_id=? AND instruction_revision=?", {actor.task_id, number(task, "instruction_revision")});
        const auto old_order = parse_json_strict(str(previous, "body"));
        for (const auto& excerpt : old_order.at("constraints_verbatim").as_array())
            require(contains(rework_order.at("constraints_verbatim"), excerpt.as_string()), ErrorCode::InvalidInput, "rework order drops an existing instruction constraint");
    }
    impl_->execute("INSERT OR IGNORE INTO team_workflow_state(team_id,rework_count) VALUES (?,0)", {actor.team_id});
    const auto used = number(impl_->one("SELECT rework_count FROM team_workflow_state WHERE team_id=?", {actor.team_id}), "rework_count");
    const auto config = parse_json_strict(str(request, "config_snapshot"), 8 * 1024 * 1024);
    const auto maximum = number(config.at("system").at("limits").as_object(), "team_internal_rework_max");
    require(maximum >= 0 && maximum <= 10000, ErrorCode::Corrupt, "invalid frozen internal rework limit");
    const std::string next_status = passed ? "CANDIDATE_READY" : used < maximum ? "RETURNED_FOR_REWORK" : "BLOCKED";
    Json saved_order = rework_order;
    if (next_status == "RETURNED_FOR_REWORK") {
        auto target = captured; target.task_revision = next(number(task, "task_revision"));
        target.instruction_revision = next(number(task, "instruction_revision"));
        auto complete = rework_order.as_object(); const auto target_document = to_json(target);
        for (const auto& field : target_document.as_object()) complete[field.key()] = field.value();
        saved_order = complete;
        impl_->cas("UPDATE tasks SET task_revision=?,instruction_revision=?,status='READY',accepted_with_issues=0,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
            {target.task_revision, target.instruction_revision, stamp(), actor.task_id, expected_task});
        impl_->execute("UPDATE teams SET paused_for_change=1,tool_calls_used=0,work_order=?,state_version=state_version+1,updated_at=? WHERE team_id=?",
            {json_text(complete, 262144), stamp(), actor.team_id});
        impl_->execute("INSERT INTO instructions VALUES (?,?,?,?,?,?,?,?,?,?,?)",
            {actor.request_id, actor.task_id, actor.team_id, target.instruction_revision, target.requirements_revision, target.plan_revision,
             target.task_revision, target.dispatcher_epoch, target.execution_epoch, json_text(complete, 262144), stamp()});
        impl_->execute("UPDATE team_workflow_state SET rework_count=rework_count+1 WHERE team_id=?", {actor.team_id});
    } else impl_->cas("UPDATE tasks SET status=?,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
            {passed ? "SUBMITTED" : "BLOCKED", stamp(), actor.task_id, expected_task});
    impl_->cas("UPDATE workflow_submissions SET status=?,state_version=state_version+1,updated_at=? WHERE submission_id=? AND state_version=?",
        {next_status, stamp(), id, expected_submission});
    impl_->execute("INSERT INTO team_lead_checks VALUES (?,?,?,?,?,?,?,?)",
        {id, next(expected_submission), json_text(actor_json(actor)), canonical_json(to_json(captured)), json_text(check_body, 65536),
         saved_order.is_null() ? Json(nullptr) : Json(json_text(saved_order, 262144)), next_status, stamp()});
    if (next_status == "RETURNED_FOR_REWORK") impl_->supersede_waits(actor.request_id, actor.task_id);
    const auto result = impl_->submission_info(id);
    impl_->event(actor.request_id, command_id, command, result, "team_lead"); tx.commit(); return result;
}

bool Store::acceptance_runtime_enabled() const {
    const std::lock_guard lock(impl_->mutex); return impl_->acceptance_schema;
}
Json Store::accept_candidate(const StoreActor& actor, const ExecutionBinding& captured,
        std::int64_t expected_task, const std::string& id, std::int64_t expected_submission,
        const Json& decision, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    object_fields(decision, {"passed", "criteria_results", "notes"}, {"passed"});
    require(decision.at("passed").is_bool() && expected_task > 0 && expected_submission > 0,
            ErrorCode::InvalidInput, "acceptance needs a decision and captured object versions");
    const bool passed = decision.at("passed").as_bool();
    (void)workflow_text(decision, "notes", 32000, true); (void)json_text(decision, 262144);
    const auto command = payload(actor, "accept_candidate", Row{{"binding", to_json(captured)}, {"expected_task_version", expected_task},
        {"submission_id", id}, {"expected_submission_version", expected_submission}, {"decision", decision}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher}, false); impl_->actor_binding(actor, captured);
    const auto submission = impl_->submission_row(id);
    require(str(submission, "request_id") == actor.request_id, ErrorCode::Unauthorized, "candidate belongs to another Dispatcher");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto request = impl_->authorize(actor, {Role::Dispatcher}); impl_->matches(captured);
    const auto task_id = str(submission, "task_id"); const auto task = impl_->task_row(task_id);
    require(!terminal_request(str(request, "status")) && str(submission, "status") == "CANDIDATE_READY" &&
        (str(task, "status") == "SUBMITTED" || (!passed && str(task, "status") == "ACCEPTED")), ErrorCode::NotReady, "candidate is not awaiting this business decision");
    require(number(task, "state_version") == expected_task && number(submission, "state_version") == expected_submission,
            ErrorCode::Conflict, "captured candidate or task CAS changed");
    const auto original = execution_binding_from_json(parse_json_strict(str(submission, "original_binding")));
    require(semantic_binding_equal(original, impl_->binding(task_id), !passed) &&
        original.requirements_revision == number(request, "requirements_revision") && original.plan_revision == number(request, "plan_revision"),
        ErrorCode::Stale, "candidate has an obsolete semantic binding");
    const auto brief = parse_json_strict(str(task, "brief")); const auto criteria = acceptance_ids(brief);
    if (const auto* results = decision.as_object().if_contains("criteria_results")) criterion_results(*results, criteria, passed, true);
    else require(!passed, ErrorCode::InvalidInput, "positive business acceptance needs complete criterion evidence");
    if (passed) {
        require(task.at("review_config").is_null(), ErrorCode::Unsupported, "required independent review is not implemented by this acceptance path");
        const auto config = parse_json_strict(str(request, "config_snapshot"), 8 * 1024 * 1024);
        bool ordinary_plan = false;
        for (const auto& capability : config.at("function").at("capabilities").as_array())
            if (capability.at("id") == config.at("capability_id")) ordinary_plan = capability.at("dispatch").as_string() == "plan";
        require(ordinary_plan && str(task, "kind") == "team_task", ErrorCode::Unsupported, "template/capability acceptance requires a registered validator");
        const auto plan = parse_json_strict(str(impl_->one("SELECT body FROM plans WHERE request_id=? AND plan_revision=?", {actor.request_id, original.plan_revision}), "body"));
        if (plan.is_object() && plan.as_object().contains("nodes")) for (const auto& node : plan.at("nodes").as_array())
            if (node.at("node_id") == task.at("node_id")) require(!node.as_object().contains("review"), ErrorCode::Unsupported, "plan requires an independent review");
        require(!impl_->has_blocking_question(task_id), ErrorCode::NotReady, "unanswered question blocks acceptance");
        impl_->require_settled(actor.request_id, str(submission, "team_id"));
        const auto original_submission = impl_->submission_info(id);
        criterion_results(original_submission.at("body").at("self_check"), criteria, true, true);
        submission_verification(original_submission.at("body"), true);
        const auto check = impl_->one("SELECT check_body,status FROM team_lead_checks WHERE submission_id=? ORDER BY submission_version DESC LIMIT 1", {id});
        const auto check_body = parse_json_strict(str(check, "check_body"));
        require(str(check, "status") == "CANDIDATE_READY" && check_body.at("passed").as_bool(), ErrorCode::NotReady, "current local TeamLead check did not pass");
        criterion_results(check_body.at("criteria_results"), criteria, true, false);
    }
    impl_->capacity_available(actor.request_id, passed ? "acceptances" : "adverse_acceptances");
    if (!passed) {
        impl_->execute("UPDATE candidate_acceptances SET status='REVOKED',state_version=state_version+1,updated_at=? WHERE task_id=? AND status='ACCEPTED'", {stamp(), task_id});
        impl_->execute("UPDATE delivery_runtime SET status='SUPERSEDED',state_version=state_version+1,updated_at=? WHERE request_id=? AND status IN ('PREPARED','STORED')", {stamp(), actor.request_id});
        impl_->execute("UPDATE requests SET status='RUNNING',delivery_status='NONE',state_version=state_version+1,updated_at=? WHERE request_id=?", {stamp(), actor.request_id});
    }
    const auto acceptance_id = new_id("acc"); const auto created = stamp();
    const Row fact{{"submission_id", id}, {"submission_version", expected_submission}, {"task_version", expected_task},
        {"producer_binding", to_json(original)}, {"actor", actor_json(actor)}, {"captured_binding", to_json(captured)}, {"decision", decision}};
    impl_->execute("INSERT INTO candidate_acceptances(acceptance_id,request_id,task_id,submission_id,submission_version,task_version,producer_binding,actor,captured_binding,decision,record_sha256,status,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {acceptance_id, actor.request_id, task_id, id, expected_submission, expected_task, canonical_json(to_json(original)), canonical_json(actor_json(actor)), canonical_json(to_json(captured)),
         json_text(decision, 262144), sha256_hex(canonical_json(fact)), passed ? "ACCEPTED" : "REJECTED", created, created});
    impl_->cas("UPDATE tasks SET status=?,accepted_with_issues=0,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
        {passed ? "ACCEPTED" : "BLOCKED", created, task_id, expected_task});
    impl_->cas("UPDATE workflow_submissions SET status=?,state_version=state_version+1,updated_at=? WHERE submission_id=? AND state_version=?",
        {passed ? "CANDIDATE_READY" : "BLOCKED", created, id, expected_submission});
    auto result = impl_->submission_info(id);
    result.as_object()["acceptance"] = impl_->acceptance_info(impl_->one("SELECT * FROM candidate_acceptances WHERE acceptance_id=?", {acceptance_id}));
    result.as_object()["experience_id"] = nullptr;
    if (passed) {
        Array requirements;
        for (const auto& row : impl_->all("SELECT * FROM request_texts WHERE request_id=? ORDER BY requirements_revision", {actor.request_id})) requirements.emplace_back(row);
        const Row source{{"submission", result}, {"acceptance", result.at("acceptance")}, {"brief", brief}, {"requirements", requirements},
            {"instruction", exposed(impl_->one("SELECT * FROM instructions WHERE task_id=? AND instruction_revision=?", {task_id, original.instruction_revision}))}};
        const auto experience_id = new_id("expjob");
        impl_->execute("INSERT INTO experience_jobs(experience_id,request_id,task_id,team_id,based_on_requirements_revision,plan_revision,task_revision,instruction_revision,status,source_snapshot,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,'PENDING',?,?,?)",
            {experience_id, actor.request_id, task_id, original.team_id, original.requirements_revision, original.plan_revision, original.task_revision,
             original.instruction_revision, json_text(source, 8 * 1024 * 1024), created, created});
        result.as_object()["experience_id"] = experience_id;
    }
    impl_->event(actor.request_id, command_id, command, result, "dispatcher"); tx.commit(); return result;
}
#include "store_rework.inc"

Json Store::experience_job(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->experience_info(id);
}
std::vector<Json> Store::experience_jobs(const std::string& request_id, const std::string& task_id, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    require(limit > 0 && limit <= 128, ErrorCode::InvalidInput, "experience limit must be 1..128");
    const auto filter = task_id.empty() ? std::string{} : " AND task_id=?";
    const Array args = task_id.empty() ? Array{request_id} : Array{request_id, task_id};
    const auto rows = impl_->all("SELECT experience_id FROM experience_jobs WHERE request_id=?" + filter + " ORDER BY rowid", args);
    require(rows.size() <= static_cast<std::size_t>(limit), ErrorCode::Quota, "complete experience history exceeds requested capacity");
    std::vector<Json> result; for (const auto& row : rows) result.push_back(impl_->experience_info(str(row, "experience_id")));
    return result;
}
Json Store::report_experience(const StoreActor& actor, const ExecutionBinding& captured, const std::string& id,
        std::int64_t expected, const std::string& source_hash, const Json& body, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    object_fields(body, {"report_id", "request_id", "task_id", "team_id", "no_new_experience", "entries", "memory_assessments"},
        {"report_id", "request_id", "task_id", "team_id", "no_new_experience", "entries", "memory_assessments"});
    require(body.at("no_new_experience").is_bool() && body.at("entries").is_array() && body.at("memory_assessments").is_array(),
            ErrorCode::InvalidInput, "invalid experience report structure");
    require(body.at("entries").as_array().size() <= 20 && body.at("memory_assessments").as_array().empty(),
            ErrorCode::Unsupported, "memory feedback is not implemented; experience entries are proposals only");
    require(body.at("no_new_experience").as_bool() == body.at("entries").as_array().empty(), ErrorCode::InvalidInput, "no-new-experience contradicts entries");
    require(is_typed_id(workflow_text(body, "report_id", 80), "rep") && workflow_text(body, "request_id", 80) == actor.request_id &&
            workflow_text(body, "task_id", 80) == actor.task_id && workflow_text(body, "team_id", 80) == actor.team_id,
            ErrorCode::Unauthorized, "experience report host identities disagree");
    for (const auto& entry : body.at("entries").as_array()) {
        require(entry.is_object(), ErrorCode::InvalidInput, "experience entry must be an object");
        for (const auto key : {"kind", "type", "title", "description", "body_markdown", "proposed_scope", "evidence"})
            (void)workflow_text(entry, key, key == std::string_view("body_markdown") ? 80000 : 16000);
        const auto kind = workflow_text(entry, "kind", 16); const auto type = workflow_text(entry, "type", 16);
        require(kind == "new" || kind == "revise" || kind == "conflict", ErrorCode::InvalidInput, "invalid experience proposal kind");
        require(type == "rule" || type == "fact" || type == "experience" || type == "skill", ErrorCode::InvalidInput, "invalid experience proposal type");
        if (kind != "new") (void)workflow_text(entry, "target_memory_id", 80);
        if (kind == "revise") (void)workflow_text(entry, "expected_base_revision", 64);
        if (type == "experience") (void)workflow_text(entry, "outcome", 16);
        if (type == "skill") (void)workflow_text(entry, "skill_name", 64);
    }
    const auto command = payload(actor, "report_experience", Row{{"binding", to_json(captured)}, {"experience_id", id},
        {"expected_job_version", expected}, {"source_sha256", source_hash}, {"body", body}});
    Impl::Transaction tx(*impl_); (void)impl_->authorize(actor, {Role::TeamLead}, false); impl_->actor_binding(actor, captured);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    (void)impl_->validate_experience_target(actor, captured, id, expected, source_hash);
    const Row fact{{"body", body}, {"source_sha256", source_hash}, {"actor", actor_json(actor)}, {"captured_binding", to_json(captured)}};
    impl_->execute("INSERT INTO experience_reports VALUES (?,?,?,?,?,?,?,?)", {id, body.at("report_id"), source_hash,
        canonical_json(actor_json(actor)), canonical_json(to_json(captured)), json_text(body, 2 * 1024 * 1024), sha256_hex(canonical_json(fact)), stamp()});
    impl_->cas("UPDATE experience_jobs SET status='DONE',report_ref=?,state_version=state_version+1,updated_at=? WHERE experience_id=? AND state_version=?",
        {body.at("report_id"), stamp(), id, expected});
    const auto result = impl_->experience_info(id);
    impl_->event(actor.request_id, command_id, command, result, "team_lead"); tx.commit(); return result;
}

Json Store::prepare_delivery(const StoreActor& actor, const ExecutionBinding& captured,
        std::int64_t expected, const Json& root_identity, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    object_fields(root_identity, {"path", "device", "inode"}, {"path", "device", "inode"});
    const auto root_path = workflow_text(root_identity, "path", 4096);
    const std::filesystem::path directory(root_path);
    require(directory.is_absolute() && root_path != "/" && root_path.back() != '/' && directory.lexically_normal().string() == root_path &&
            number(root_identity.as_object(), "device") > 0 && number(root_identity.as_object(), "inode") > 0,
            ErrorCode::InvalidInput, "delivery root requires canonical absolute path and pinned identity");
    require(!overlap(root_path, impl_->database_file_path), ErrorCode::Unauthorized, "delivery root would contain the control database");
    const auto command = payload(actor, "prepare_delivery", Row{{"binding", to_json(captured)}, {"expected_request_version", expected}, {"root_identity", root_identity}});
    Impl::Transaction tx(*impl_); (void)impl_->authorize(actor, {Role::Root}, false); impl_->actor_binding(actor, captured);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto request = impl_->authorize(actor, {Role::Root}); impl_->matches(captured);
    require(number(request, "state_version") == expected, ErrorCode::Conflict, "delivery preparation request CAS changed");
    require(!terminal_request(str(request, "status")) && number(request, "plan_revision") > 0, ErrorCode::NotReady, "delivery needs a live planned request");
    for (const auto& workspace : impl_->all("SELECT canonical_path,device,inode FROM workspace_bindings"))
        require(!overlap(root_path, str(workspace, "canonical_path")) &&
            !(workspace.at("device") == root_identity.at("device") && workspace.at("inode") == root_identity.at("inode")),
            ErrorCode::Unauthorized, "delivery root overlaps or aliases a business workspace");
    if (impl_->cleanup_schema) for (const auto& owned : impl_->all("SELECT intent FROM owned_workspaces")) {
        const auto intent = parse_json_strict(str(owned, "intent"));
        const auto envelope = str(intent.at("parent_identity").as_object(), "path") + "/" + str(intent.as_object(), "leaf");
        require(!overlap(root_path, envelope), ErrorCode::Unauthorized, "delivery root overlaps retained ownership envelope");
    }
    for (const auto& stored : impl_->all("SELECT frozen_plan FROM delivery_runtime")) {
        const auto previous = parse_json_strict(str(stored, "frozen_plan"), 8 * 1024 * 1024).at("root_identity");
        if (previous.at("path") == root_identity.at("path")) require(previous == root_identity, ErrorCode::Conflict, "delivery root identity changed");
    }
    impl_->require_settled(actor.request_id);
    require(!impl_->optional("SELECT q.question_id FROM questions q JOIN question_runtime x USING(question_id) WHERE q.request_id=? AND x.status='OPEN' AND x.blocking=1 LIMIT 1", {actor.request_id}),
            ErrorCode::NotReady, "unanswered question blocks delivery");
    for (const auto& old : impl_->all("SELECT * FROM delivery_runtime WHERE request_id=? AND status IN ('PREPARED','STORED') ORDER BY rowid DESC", {actor.request_id})) {
        const auto plan = parse_json_strict(str(old, "frozen_plan"), 8 * 1024 * 1024);
        if (plan.at("root_identity") == root_identity && number(old, "requirements_revision") == captured.requirements_revision && number(old, "plan_revision") == captured.plan_revision) {
            impl_->delivery_current(old);
            const auto result = impl_->delivery_info(str(old, "delivery_id"));
            impl_->event(actor.request_id, command_id, command, result, "root"); tx.commit(); return result;
        }
    }
    impl_->capacity_available(actor.request_id, "deliveries");
    Array files; Array entries; Array acceptances; Array evidence; Array limitations; Array experience;
    std::size_t total_bytes = 0;
    const auto add_file = [&](const std::string& name, const std::string& bytes) {
        require(bytes.size() <= 2 * 1024 * 1024 && total_bytes <= 4 * 1024 * 1024 - bytes.size() && files.size() < 64,
                ErrorCode::Quota, "complete delivery exceeds bounded package capacity");
        total_bytes += bytes.size();
        files.emplace_back(Row{{"name", name}, {"sha256", sha256_hex(bytes)}, {"size_bytes", static_cast<std::int64_t>(bytes.size())}, {"content_base64", base64_bytes(bytes)}});
    };
    const auto tasks = impl_->all("SELECT * FROM tasks WHERE request_id=? ORDER BY node_id", {actor.request_id});
    require(!tasks.empty(), ErrorCode::NotReady, "delivery has no tasks");
    for (const auto& task : tasks) {
        const auto current = impl_->current_acceptance(str(task, "task_id"));
        if (!current) {
            require(number(task, "required") == 0, ErrorCode::NotReady, "required task has no current business acceptance");
            limitations.emplace_back("Optional task " + str(task, "node_id") + " has no current acceptance and is excluded."); continue;
        }
        acceptances.emplace_back(Row{{"task_id", task.at("task_id")}, {"acceptance_id", current->at("acceptance_id")}, {"record_sha256", current->at("record_sha256")}});
        auto submission = impl_->submission_info(str(*current, "submission_id"));
        Array source_files; std::size_t index = 0;
        for (const auto& artifact : submission.at("artifact_snapshot").as_array()) {
            const auto name = "artifact-" + str(task, "node_id") + "-" + std::to_string(++index) + ".bin";
            const auto bytes = decoded_base64(artifact.at("content_base64").as_string());
            require(sha256_hex(bytes) == artifact.at("sha256").as_string() && bytes.size() == static_cast<std::size_t>(number(artifact.as_object(), "size_bytes")),
                    ErrorCode::Corrupt, "stored original artifact bytes changed");
            add_file(name, bytes);
            const Row entry{{"name", name}, {"source_path", artifact.at("path")}, {"sha256", artifact.at("sha256")},
                {"size_bytes", artifact.at("size_bytes")}, {"task_id", task.at("task_id")}, {"submission_id", submission.at("submission_id")},
                {"acceptance_id", current->at("acceptance_id")}};
            entries.emplace_back(entry); source_files.emplace_back(entry);
        }
        submission.as_object()["artifact_snapshot"] = source_files;
        evidence.emplace_back(Row{{"task", exposed(task)}, {"submission", submission}, {"acceptance", impl_->acceptance_info(*current)}});
    }
    require(!acceptances.empty() && !files.empty(), ErrorCode::NotReady, "delivery requires at least one accepted artifact");
    Array requirements; for (const auto& text : impl_->all("SELECT * FROM request_texts WHERE request_id=? ORDER BY requirements_revision", {actor.request_id})) requirements.emplace_back(text);
    Array constraints; for (const auto& constraint : impl_->all("SELECT * FROM request_constraints WHERE request_id=? ORDER BY requirements_revision,constraint_id", {actor.request_id})) constraints.emplace_back(constraint);
    for (const auto& job : impl_->all("SELECT experience_id FROM experience_jobs WHERE request_id=? ORDER BY rowid", {actor.request_id})) {
        const auto value = impl_->experience_info(str(job, "experience_id"));
        experience.emplace_back(Row{{"experience_id", value.at("experience_id")}, {"task_id", value.at("task_id")}, {"team_id", value.at("team_id")},
            {"status", value.at("status")}, {"source_sha256", value.at("source_sha256")}, {"report", value.at("report")}});
    }
    Array side_effects;
    for (const auto& op : impl_->all("SELECT op_id,task_id,tool_id,side_effect,status FROM ops WHERE request_id=? ORDER BY created_at,op_id", {actor.request_id})) side_effects.emplace_back(op);
    add_file("requirements.json", canonical_json(Row{{"texts", requirements}, {"constraint_catalog", constraints}}));
    add_file("plan.json", canonical_json(exposed(impl_->one("SELECT * FROM plans WHERE request_id=? AND plan_revision=?", {actor.request_id, captured.plan_revision}))));
    Array usage_records;
    for (const auto& attempt : impl_->all("SELECT attempt_id,role,task_id,team_id,status,input_tokens,output_tokens,cache_read_tokens,cache_write_tokens,cost_micro_usd,unknown_input,unknown_output,unknown_cost,usage FROM call_attempts WHERE request_id=? ORDER BY rowid", {actor.request_id})) usage_records.emplace_back(exposed(attempt));
    add_file("usage.json", canonical_json(Row{{"captured_at", stamp()}, {"budget_ledger", impl_->one("SELECT * FROM request_budgets WHERE request_id=?", {actor.request_id})},
        {"attempts", usage_records}, {"scope", "Package usage is frozen at preparation; receipt usage is frozen at the business terminal transition. Later terminal experience usage remains in the retained request budget and attempt ledgers and must be settled and summarized before any future purge."}}));
    Array questions; Array open_questions;
    for (const auto& question : impl_->all("SELECT question_id FROM questions WHERE request_id=? ORDER BY rowid", {actor.request_id})) {
        const auto value = impl_->question_info(str(question, "question_id")); questions.emplace_back(value);
        if (str(value.as_object(), "status") == "OPEN") {
            open_questions.emplace_back(value.at("question_id"));
            limitations.emplace_back("Nonblocking question " + str(value.as_object(), "question_id") + " remains unanswered; original context is retained in questions.json.");
        }
    }
    add_file("questions.json", canonical_json(questions));
    add_file("acceptance.json", canonical_json(evidence));
    add_file("experience.json", canonical_json(experience));
    add_file("side-effects.json", canonical_json(side_effects));
    add_file("delivery.txt", "WLS immutable local delivery package. Artifact names and original paths are mapped in manifest.json.\n"
        "acceptance.json records Dispatcher decisions and original local evidence. experience.json records reports or explicit pending jobs.\n"
        "A durable_local receipt records persistent local storage, not whether a person read the package.\n");
    std::sort(files.begin(), files.end(), [](const Json& a, const Json& b) { return a.at("name").as_string() < b.at("name").as_string(); });
    Array file_metadata;
    for (const auto& file : files) file_metadata.emplace_back(Row{{"name", file.at("name")}, {"sha256", file.at("sha256")}, {"size_bytes", file.at("size_bytes")}});
    const auto id = new_id("dlv"); const auto created = stamp();
    const Row manifest{{"format", "wls.delivery-package.v1"}, {"delivery_id", id}, {"request_id", actor.request_id},
        {"requirements_revision", captured.requirements_revision}, {"plan_revision", captured.plan_revision}, {"created_at", created},
        {"files", file_metadata}, {"artifacts", entries}, {"limitations", limitations},
        {"requirements_file", "requirements.json"}, {"acceptance_file", "acceptance.json"}, {"experience_file", "experience.json"}, {"side_effects_file", "side-effects.json"},
        {"plan_file", "plan.json"}, {"usage_file", "usage.json"}, {"questions_file", "questions.json"}, {"open_questions", open_questions},
        {"independent_review_performed", false}, {"real_model_acceptance_tested", false}};
    const auto manifest_bytes = json_text(manifest, 1024 * 1024);
    require(total_bytes <= 4 * 1024 * 1024 - manifest_bytes.size(), ErrorCode::Quota, "manifest and files exceed complete package capacity");
    const auto hash = sha256_hex(manifest_bytes);
    const Row plan{{"delivery_id", id}, {"root_identity", root_identity}, {"manifest", manifest}, {"manifest_sha256", hash}, {"files", files}};
    impl_->execute("INSERT INTO delivery_runtime(delivery_id,request_id,requirements_revision,plan_revision,captured_binding,frozen_plan,manifest_sha256,acceptance_snapshot,status,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,'PREPARED',?,?)",
        {id, actor.request_id, captured.requirements_revision, captured.plan_revision, canonical_json(to_json(captured)), json_text(plan, 8 * 1024 * 1024), hash, json_text(acceptances), created, created});
    impl_->cas("UPDATE requests SET status='DELIVERING',state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?", {created, actor.request_id, expected});
    const auto result = impl_->delivery_info(id); impl_->event(actor.request_id, command_id, command, result, "root"); tx.commit(); return result;
}
Json Store::delivery(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->delivery_info(id);
}
std::vector<Json> Store::deliveries(const std::string& request_id, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    require(limit > 0 && limit <= 64, ErrorCode::InvalidInput, "delivery limit must be 1..64");
    const auto rows = impl_->all("SELECT delivery_id FROM delivery_runtime WHERE request_id=? ORDER BY rowid", {request_id});
    require(rows.size() <= static_cast<std::size_t>(limit), ErrorCode::Quota, "complete delivery history exceeds requested capacity");
    std::vector<Json> result; for (const auto& row : rows) result.push_back(impl_->delivery_info(str(row, "delivery_id"))); return result;
}
Json Store::record_delivery_publication(const StoreActor& actor, const ExecutionBinding& captured, const std::string& id,
        std::int64_t expected, const Json& publication, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    require(publication.is_object(), ErrorCode::InvalidInput, "publication must be host evidence");
    const auto command = payload(actor, "record_delivery_publication", Row{{"binding", to_json(captured)}, {"delivery_id", id}, {"expected_delivery_version", expected}, {"publication", publication}});
    Impl::Transaction tx(*impl_); (void)impl_->authorize(actor, {Role::Root}, false); impl_->actor_binding(actor, captured);
    const auto record = impl_->delivery_row(id);
    require(str(record, "request_id") == actor.request_id, ErrorCode::Unauthorized, "delivery belongs to another Root request");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    (void)impl_->authorize(actor, {Role::Root}); impl_->matches(captured); impl_->delivery_current(record);
    require(number(record, "state_version") == expected, ErrorCode::Conflict, "publication delivery CAS changed");
    impl_->verify_publication(record, publication);
    impl_->cas("UPDATE delivery_runtime SET status='STORED',publication=?,state_version=state_version+1,updated_at=? WHERE delivery_id=? AND state_version=?",
        {json_text(publication, 262144), stamp(), id, expected});
    impl_->execute("UPDATE requests SET status='DELIVERING',delivery_status='STORED',state_version=state_version+1,updated_at=? WHERE request_id=?", {stamp(), actor.request_id});
    const auto result = impl_->delivery_info(id); impl_->event(actor.request_id, command_id, command, result, "root"); tx.commit(); return result;
}
Json Store::authorize_delivery_publication(const StoreActor& actor, const ExecutionBinding& captured, const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    const auto request = impl_->authorize(actor, {Role::Root}); impl_->actor_binding(actor, captured); impl_->matches(captured);
    const auto record = impl_->delivery_row(id);
    require(str(record, "request_id") == actor.request_id, ErrorCode::Unauthorized, "delivery publication belongs to another request");
    if (str(record, "status") == "CONFIRMED") {
        require(str(request, "status") == "DELIVERED" && number(request, "requirements_revision") == number(record, "requirements_revision") &&
            number(request, "plan_revision") == number(record, "plan_revision") &&
            impl_->optional("SELECT receipt_id FROM delivery_receipts WHERE delivery_id=? AND request_id=? AND root_id=?", {id, actor.request_id, actor.principal_id}).has_value(),
            ErrorCode::NotReady, "only the exact retained confirmed package may be verified again");
    } else impl_->delivery_current(record);
    return impl_->delivery_info(id);
}
Json Store::acknowledge_delivery(const StoreActor& actor, const ExecutionBinding& captured, const std::string& id,
        const std::string& hash, std::int64_t requirements, std::int64_t plan_revision, const std::string& recipient,
        const std::string& method, const std::string& key, const Json& fresh_publication) {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    text_field(key, "receipt idempotency key", 256); nonblank(recipient, "receipt recipient", 256);
    require(method == "durable_local" || method == "explicit_ack", ErrorCode::InvalidInput, "unknown receipt method");
    const Row intent{{"request_id", actor.request_id}, {"root_id", actor.principal_id}, {"delivery_id", id}, {"manifest_sha256", hash},
        {"requirements_revision", requirements}, {"plan_revision", plan_revision}, {"recipient", recipient}, {"method", method}, {"idempotency_key", key}};
    const auto fingerprint = sha256_hex(canonical_json(intent));
    Impl::Transaction tx(*impl_); (void)impl_->authorize(actor, {Role::Root}, false); impl_->actor_binding(actor, captured);
    const auto record = impl_->delivery_row(id);
    require(str(record, "request_id") == actor.request_id, ErrorCode::Unauthorized, "receipt delivery belongs to another request");
    impl_->verify_publication(record, fresh_publication);
    if (const auto old = impl_->optional("SELECT * FROM delivery_receipts WHERE request_id=? AND idempotency_key=?", {actor.request_id, key})) {
        require(str(*old, "root_id") == actor.principal_id && str(*old, "command_sha256") == fingerprint,
                ErrorCode::Conflict, "receipt key reused for a different exact package or recipient");
        const auto result = parse_json_strict(str(*old, "result"), 16 * 1024 * 1024); tx.commit(); return result;
    }
    (void)impl_->authorize(actor, {Role::Root}); impl_->matches(captured);
    require(str(record, "status") == "STORED", ErrorCode::NotReady, "only a durably published package can be acknowledged");
    impl_->delivery_current(record);
    require(hash == str(record, "manifest_sha256") && requirements == number(record, "requirements_revision") && plan_revision == number(record, "plan_revision"),
            ErrorCode::Stale, "receipt must bind the exact package and current versions");
    const auto publication = parse_json_strict(str(record, "publication"));
    require(publication.at("durable").as_bool() && publication.at("verified").as_bool(), ErrorCode::Corrupt, "stored publication lacks durable evidence");
    const auto receipt_id = new_id("receipt"); auto receipt = intent;
    receipt["receipt_id"] = receipt_id; receipt["captured_binding"] = to_json(captured); receipt["created_at"] = stamp(); receipt["publication"] = fresh_publication;
    impl_->cas("UPDATE delivery_runtime SET status='CONFIRMED',state_version=state_version+1,updated_at=? WHERE delivery_id=? AND state_version=?",
        {stamp(), id, number(record, "state_version")});
    // The receipt chooses exactly one package for this request. Other intents
    // retain their immutable plans and any published bytes as history, but
    // cannot remain impossible-to-confirm work after the terminal transition.
    impl_->execute("UPDATE delivery_runtime SET status='SUPERSEDED',state_version=state_version+1,updated_at=? WHERE request_id=? AND delivery_id<>? AND status IN ('PREPARED','STORED')",
        {stamp(), actor.request_id, id});
    impl_->execute("UPDATE requests SET status='DELIVERED',delivery_status='CONFIRMED',state_version=state_version+1,updated_at=? WHERE request_id=?", {stamp(), actor.request_id});
    impl_->execute("UPDATE teams SET paused_for_change=1,state_version=state_version+1,updated_at=? WHERE request_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {stamp(), actor.request_id});
    impl_->retain_closeout(impl_->request_row(actor.request_id), str(publication.as_object(), "path"), hash);
    auto result = impl_->delivery_info(id); result.as_object()["receipt"] = receipt;
    impl_->execute("INSERT INTO delivery_receipts VALUES (?,?,?,?,?,?,?,?,?)", {receipt_id, actor.request_id, id, actor.principal_id, key, fingerprint, json_text(receipt), json_text(result, 16 * 1024 * 1024), stamp()});
    impl_->execute("INSERT INTO closeout_commands VALUES (?,?,?,?,?,?)", {actor.request_id, "ack:" + key, actor.principal_id, fingerprint, json_text(result, 16 * 1024 * 1024), stamp()});
    impl_->event(actor.request_id, "receipt:" + receipt_id, payload(actor, "acknowledge_delivery", receipt), result, "root"); tx.commit(); return result;
}
Json Store::closeout(const std::string& request_id) const {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    (void)impl_->request_row(request_id);
    if (impl_->cleanup_schema) return impl_->cleanup_status(request_id);
    const auto job = impl_->optional("SELECT * FROM cleanup_jobs WHERE request_id=?", {request_id});
    const auto receipt = impl_->optional("SELECT * FROM receipts WHERE request_id=?", {request_id});
    Array blockers;
    if (!job) blockers.emplace_back("business_not_terminal");
    if (impl_->optional("SELECT experience_id FROM experience_jobs WHERE request_id=? AND status<>'DONE' LIMIT 1", {request_id})) blockers.emplace_back("experience_pending");
    if (impl_->optional("SELECT op_id FROM ops WHERE request_id=? AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN') LIMIT 1", {request_id})) blockers.emplace_back("unsettled_operations");
    if (impl_->optional("SELECT attempt_id FROM call_attempts WHERE request_id=? AND (status IN ('PENDING','UNKNOWN') OR unknown_input=1 OR unknown_output=1 OR unknown_cost=1) LIMIT 1", {request_id})) blockers.emplace_back("unsettled_model_accounting");
    if (impl_->optional("SELECT delivery_id FROM delivery_runtime WHERE request_id=? AND status IN ('PREPARED','STORED') LIMIT 1", {request_id})) blockers.emplace_back("delivery_unconfirmed");
    blockers.emplace_back("controlled_resource_ownership_not_registered");
    blockers.emplace_back("process_exit_and_cleanup_runner_not_implemented");
    return Row{{"request_id", request_id}, {"receipt", receipt ? exposed(*receipt) : Json(nullptr)}, {"cleanup_job", job ? exposed(*job) : Json(nullptr)},
        {"checkpoint", job ? "RETAINED" : "NOT_CREATED"}, {"blockers", blockers}, {"cleanup_complete", false}, {"deletion_performed", false}};
}

StoreActor Store::host_actor(Role role, const std::string& id, const std::string& team_id) const {
    const std::lock_guard lock(impl_->mutex);
    const auto r = impl_->request_row(id);
    StoreActor actor;
    actor.role = role; actor.request_id = id; actor.dispatcher_epoch = number(r, "dispatcher_epoch");
    if (role == Role::Root || role == Role::Dispatcher) {
        require(team_id.empty(), ErrorCode::InvalidInput, "manager actor has no team");
        actor.principal_id = str(r, role == Role::Root ? "root_id" : "dispatcher_id");
    } else {
        const auto team = impl_->team_row(team_id);
        require(str(team, "request_id") == id, ErrorCode::Unauthorized, "team belongs to another request");
        actor.team_id = team_id; actor.task_id = str(team, "task_id"); actor.execution_epoch = number(team, "execution_epoch");
        actor.principal_id = str(team, role == Role::TeamLead ? "lead_id" : "worker_id");
    }
    (void)impl_->authorize(actor, {role});
    return actor;
}

Json Store::capacity_status(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->capacity_state(id);
}

void Store::validate_capacity_checkpoint(const StoreActor& root, std::int64_t expected_window,
        const std::string& evidence, const std::string& command_id) const {
    const std::lock_guard lock(impl_->mutex);
    text_field(command_id, "command_id");
    const auto command = impl_->checkpoint_intent(root, expected_window, evidence);
    (void)impl_->authorize(root, {Role::Root}, false);
    if (impl_->replay(root.request_id, command_id, command)) return;
    (void)impl_->checkpoint_state(root.request_id, expected_window);
}

Json Store::checkpoint_capacity(const StoreActor& root, std::int64_t expected_window,
        const std::string& evidence, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    text_field(command_id, "command_id");
    const auto command = impl_->checkpoint_intent(root, expected_window, evidence);
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(root, {Role::Root}, false);
    if (auto old = impl_->replay(root.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto state = impl_->checkpoint_state(root.request_id, expected_window);
    const Row result{{"request_id", root.request_id}, {"window", next(expected_window)},
        {"totals", state.at("totals")}, {"task_submissions", state.at("task_submissions")},
        {"closed_counts", state.at("counts")}, {"limits", state.at("limits")},
        {"audit_rows_retained", true}, {"budgets_reset", false}};
    impl_->capacity_raw_event(root.request_id, command_id, command, result);
    tx.commit(); return result;
}

Json Store::admit(const AdmissionInput& input) {
    const std::lock_guard lock(impl_->mutex);
    for (const auto* field : {&input.tenant_id, &input.project_id, &input.root_id, &input.function_id, &input.idempotency_key}) text_field(*field, "admission scope");
    text_field(input.original_text, "original text", 65536);
    require(input.config_snapshot.is_object(), ErrorCode::InvalidInput, "complete configuration snapshot required");
    require(input.budget.max_attempts > 0 && input.budget.max_attempts <= 10000 && input.budget.input_tokens >= 0 &&
            input.budget.output_tokens >= 0 && input.budget.cost_micro_usd >= 0, ErrorCode::InvalidInput, "invalid frozen budget");
    const auto config = json_text(input.config_snapshot, 8 * 1024 * 1024);
    const auto frozen_config = FrozenRequestConfig::restore(config, sha256_hex(config));
    if (frozen_config.json().at("system").at("cleanup").at("enabled").as_bool())
        require(impl_->cleanup_schema, ErrorCode::Unsupported, "enabled cleanup requires the 407 Store");
    if (impl_->acceptance_schema) {
        const auto ttl = number(frozen_config.json().at("system").at("cleanup").as_object(), "receipt_ttl_days");
        require(ttl > 0 && ttl <= 36500, ErrorCode::Unsupported, "receipt TTL must be 1..36500 frozen days");
    }
    require(frozen_config.function_id() == input.function_id, ErrorCode::InvalidInput, "admission function differs from frozen configuration");
    std::set<std::string> template_tools;
    for (const auto& entry : frozen_config.json().at("function").at("team_templates").as_object()) {
        const auto grant = frozen_config.worker_tools(entry.key());
        template_tools.insert(grant.begin(), grant.end());
    }
    for (const auto& grant : input.tool_allowlist) require(template_tools.contains(grant), ErrorCode::Unauthorized, "request grants tool absent from frozen templates");
    const auto& configured_budget = frozen_config.json().at("system").at("budget_defaults").as_object();
    if (!configured_budget.at("max_input_tokens").is_null()) require(input.budget.input_tokens <= number(configured_budget, "max_input_tokens"), ErrorCode::Quota, "admission exceeds frozen input token limit");
    if (!configured_budget.at("max_output_tokens").is_null()) require(input.budget.output_tokens <= number(configured_budget, "max_output_tokens"), ErrorCode::Quota, "admission exceeds frozen output token limit");
    if (!configured_budget.at("max_cost_usd").is_null()) require(input.budget.cost_micro_usd <= cost_limit_micro(configured_budget.at("max_cost_usd")), ErrorCode::Quota, "admission exceeds frozen money limit");
    const Row snapshot_row{{"config_snapshot", config}};
    (void)impl_->configured_limit(snapshot_row, "max_teams_per_request");
    (void)impl_->configured_limit(snapshot_row, "worker_max_tool_calls_per_order");
    const auto allowlist = strings(input.tool_allowlist);
    const Row budget{{"max_attempts", input.budget.max_attempts}, {"input_tokens", input.budget.input_tokens},
        {"output_tokens", input.budget.output_tokens}, {"cost_micro_usd", input.budget.cost_micro_usd}};
    const Row immutable{{"original_text", input.original_text}, {"function_id", input.function_id},
        {"config_snapshot_sha256", sha256_hex(config)}, {"tool_allowlist", allowlist}, {"constraint_catalog", input.constraint_catalog}, {"budget", budget}};
    const auto fingerprint = sha256_hex(canonical_json(immutable));
    Impl::Transaction tx(*impl_);
    const Array identity{input.tenant_id, input.project_id, input.root_id, input.idempotency_key};
    for (const auto* table : {"requests", "receipts"}) {
        const auto old = impl_->optional(std::string("SELECT request_id,content_sha256 FROM ") + table + " WHERE tenant_id=? AND project_id=? AND root_id=? AND idempotency_key=?", identity);
        if (old) {
            require(str(*old, "content_sha256") == fingerprint, ErrorCode::Conflict, "admission key reused with different immutable input");
            const auto result = std::string_view(table) == "requests" ? exposed(impl_->request_row(str(*old, "request_id"))) : exposed(impl_->one("SELECT * FROM receipts WHERE request_id=?", {str(*old, "request_id")}));
            tx.commit(); return result;
        }
    }
    validate_store_capacity_admission(frozen_config.json().at("system"));
    const auto id = new_id("req");
    const auto created = stamp();
    impl_->execute("INSERT INTO requests(request_id,tenant_id,project_id,root_id,function_id,origin,idempotency_key,content_sha256,dispatcher_id,status,config_snapshot,budget,created_at,updated_at) VALUES (?,?,?,?,?,'cli',?,?,?,'RECEIVED',?,?,?,?)",
        {id, input.tenant_id, input.project_id, input.root_id, input.function_id, input.idempotency_key, fingerprint, new_id("dispatcher"), config, canonical_json(budget), created, created});
    impl_->execute("INSERT INTO request_runtime VALUES (?,?,?)", {id, sha256_hex(config), canonical_json(allowlist)});
    impl_->execute("INSERT INTO request_budgets(request_id,max_attempts,input_limit,output_limit,cost_limit) VALUES (?,?,?,?,?)",
        {id, input.budget.max_attempts, input.budget.input_tokens, input.budget.output_tokens, input.budget.cost_micro_usd});
    impl_->execute("INSERT INTO request_texts VALUES (?,1,'ORIGINAL',?,?)", {id, input.original_text, created});
    impl_->catalog(id, 1, input.constraint_catalog);
    impl_->event(id, "admit", Row{{"type", "admit"}, {"immutable", immutable}}, Row{{"request_id", id}}, "root");
    const auto result = exposed(impl_->request_row(id));
    tx.commit(); return result;
}

Json Store::publish_plan(const StoreActor& actor, std::int64_t expected, std::int64_t requirements,
                         const Json& body, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    const auto command = payload(actor, "plan", Row{{"expected_request_version", expected}, {"based_on_requirements_revision", requirements}, {"body", body}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher}, false);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto r = impl_->authorize(actor, {Role::Dispatcher});
    require(!terminal_request(str(r, "status")) && number(r, "requirements_revision") == requirements, ErrorCode::Stale, "plan does not address current live requirements");
    require(body.is_object(), ErrorCode::InvalidInput, "plan body must be an object");
    if (const auto* nodes = body.as_object().if_contains("nodes")) {
        require(nodes->is_array(), ErrorCode::InvalidInput, "plan nodes must be an array");
        for (const auto& node : nodes->as_array()) {
            require(node.is_object(), ErrorCode::InvalidInput, "plan node must be an object");
            if (const auto* brief = node.as_object().if_contains("brief")) impl_->brief_constraints(r, *brief);
        }
    }
    const auto revision = next(number(r, "plan_revision"));
    impl_->cas("UPDATE requests SET plan_revision=?,status='RUNNING',delivery_status='NONE',state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?",
        {revision, stamp(), actor.request_id, expected});
    impl_->execute("INSERT INTO plans VALUES (?,?,?,?,NULL,?)", {actor.request_id, revision, requirements, json_text(body), stamp()});
    impl_->execute("UPDATE teams SET paused_for_change=1,state_version=state_version+1,updated_at=? WHERE request_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {stamp(), actor.request_id});
    impl_->fence_ops(actor.request_id);
    const Json result = Row{{"plan_revision", revision}, {"based_on_requirements_revision", requirements}, {"state_version", next(expected)}};
    impl_->event(actor.request_id, command_id, command, result, role_name(actor.role));
    tx.commit(); return result;
}

Json Store::create_task(const StoreActor& actor, std::int64_t expected, std::int64_t requirements,
                        std::int64_t plan, const std::string& node_id, const Json& brief,
                        const std::string& command_id, const std::string& template_id) {
    const std::lock_guard lock(impl_->mutex);
    text_field(node_id, "node_id"); text_field(template_id, "template_id");
    const auto command = payload(actor, "create_task", Row{{"expected_request_version", expected}, {"based_on_requirements_revision", requirements},
        {"target_plan_revision", plan}, {"node_id", node_id}, {"brief", brief}, {"template_id", template_id}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher}, false);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto r = impl_->authorize(actor, {Role::Dispatcher});
    require(!terminal_request(str(r, "status")) && requirements == number(r, "requirements_revision") && plan > 0 && plan == number(r, "plan_revision"), ErrorCode::Stale, "task needs current live plan");
    const auto p = impl_->one("SELECT * FROM plans WHERE request_id=? AND plan_revision=?", {actor.request_id, plan});
    require(number(p, "based_on_requirements_revision") == requirements, ErrorCode::Stale, "plan is based on obsolete requirements");
    impl_->brief_constraints(r, brief);
    const auto frozen_config = FrozenRequestConfig::restore(str(r, "config_snapshot"), str(r, "config_snapshot_sha256"));
    (void)frozen_config.worker_tools(template_id);
    require(number(impl_->one("SELECT COUNT(*) AS n FROM teams WHERE request_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {actor.request_id}), "n") < impl_->configured_limit(r, "max_teams_per_request"),
            ErrorCode::Quota, "per-request team capacity exhausted");
    const auto task_id = new_id("tsk"); const auto team_id = new_id("team"); const auto created = stamp();
    impl_->cas("UPDATE requests SET state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?", {created, actor.request_id, expected});
    impl_->execute("INSERT INTO tasks(task_id,request_id,node_id,plan_revision,based_on_requirements_revision,kind,template_id,status,brief,created_at,updated_at) VALUES (?,?,?,?,?,'team_task',?,'READY',?,?,?)",
        {task_id, actor.request_id, node_id, plan, requirements, template_id, json_text(brief), created, created});
    // No initial instruction is fabricated by Dispatcher task allocation.
    impl_->execute("INSERT INTO teams(team_id,request_id,task_id,lead_id,worker_id,template_id,status,paused_for_change,lead_config,worker_config,workspace_path,created_at,updated_at) VALUES (?,?,?,?,?,?,'ACTIVE',1,?,?,?, ?,?)",
        {team_id, actor.request_id, task_id, new_id("lead"), new_id("worker"), template_id,
         canonical_json(Row{{"request_snapshot_sha256", str(r, "config_snapshot_sha256")}}),
         canonical_json(Row{{"request_snapshot_sha256", str(r, "config_snapshot_sha256")}}), std::string("unallocated:") + team_id, created, created});
    const Json result = Row{{"task_id", task_id}, {"team_id", team_id}, {"state_version", next(expected)}, {"binding", to_json(impl_->binding(task_id))}};
    impl_->event(actor.request_id, command_id, command, result, role_name(actor.role));
    tx.commit(); return result;
}

Json Store::apply_independent_plan(const StoreActor& actor, std::int64_t expected,
                                  std::int64_t requirements, const Json& body,
                                  const Json& dispositions, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema();
    const auto command = payload(actor, "independent_plan", Row{{"expected_request_version", expected},
        {"based_on_requirements_revision", requirements}, {"body", body}, {"dispositions", dispositions}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher}, false);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto request = impl_->authorize(actor, {Role::Dispatcher});
    require(!terminal_request(str(request, "status")) && number(request, "requirements_revision") == requirements,
            ErrorCode::Stale, "independent plan does not address current live requirements");
    const auto revision = next(number(request, "plan_revision"));
    const auto nodes = impl_->independent_nodes(request, body);
    for (const auto& target : {std::pair<std::string, std::int64_t>{"plan_revision", revision}, {"based_on_requirements_revision", requirements}})
        if (body.as_object().contains(target.first)) require(number(body.as_object(), target.first) == target.second, ErrorCode::Stale, "plan body changed its authoritative revision");
    impl_->check_dispositions(dispositions, nodes);
    const auto existing = impl_->all("SELECT * FROM tasks WHERE request_id=? ORDER BY node_id", {actor.request_id});
    impl_->check_independent_identities(existing, nodes);
    if (existing.empty()) require(dispositions.as_object().empty(), ErrorCode::InvalidInput, "initial task allocation has no AMEND dispositions");
    // Plan, all initial identities, per-task transitions, audit and inbox routes
    // commit together. A failing later node leaves no partial allocation.
    impl_->cas("UPDATE requests SET plan_revision=?,status='RUNNING',delivery_status='NONE',state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?",
        {revision, stamp(), actor.request_id, expected});
    impl_->execute("INSERT INTO plans VALUES (?,?,?,?,NULL,?)", {actor.request_id, revision, requirements, json_text(body), stamp()});
    impl_->execute("UPDATE teams SET paused_for_change=1,state_version=state_version+1,updated_at=? WHERE request_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {stamp(), actor.request_id});
    impl_->fence_ops(actor.request_id);
    Array changed; Array pending;
    if (existing.empty()) {
        for (const auto& [node_id, node] : nodes) changed.emplace_back(impl_->allocate_independent(request, revision, node_id, node));
    } else for (const auto& task : existing) {
        const auto node_id = str(task, "node_id");
        const auto* decision = dispositions.as_object().if_contains(node_id);
        if (str(task, "status") == "CANCELLED" || str(task, "status") == "SKIPPED" || str(task, "status") == "SUPERSEDED") {
            require(!decision, ErrorCode::NotReady, "disposition cannot reactivate a retired independent task");
            continue;
        }
        if (number(task, "based_on_requirements_revision") != requirements && !decision) {
            pending.emplace_back(Row{{"node_id", node_id}, {"task_id", str(task, "task_id")}, {"reason", "amend_disposition_required"}});
            continue;
        }
        changed.emplace_back(impl_->apply_independent_task(request, task, revision, nodes.at(node_id), decision ? *decision : Json(nullptr)));
    }
    const Json result = Row{{"plan_revision", revision}, {"based_on_requirements_revision", requirements},
        {"state_version", next(expected)}, {"tasks", changed}, {"pending_tasks", pending}};
    impl_->event(actor.request_id, command_id, command, result, "dispatcher");
    tx.commit(); return result;
}

Json Store::apply_independent_dispositions(const StoreActor& actor, std::int64_t expected,
                                          std::int64_t requirements, std::int64_t revision,
                                          const Json& dispositions, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema();
    const auto command = payload(actor, "independent_dispositions", Row{{"expected_request_version", expected},
        {"based_on_requirements_revision", requirements}, {"target_plan_revision", revision}, {"dispositions", dispositions}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher}, false);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto request = impl_->authorize(actor, {Role::Dispatcher});
    require(!terminal_request(str(request, "status")) && requirements >= 2 && number(request, "requirements_revision") == requirements &&
        revision > 0 && number(request, "plan_revision") == revision, ErrorCode::Stale, "dispositions require the current AMEND plan");
    const auto plan = impl_->one("SELECT * FROM plans WHERE request_id=? AND plan_revision=?", {actor.request_id, revision});
    require(number(plan, "based_on_requirements_revision") == requirements, ErrorCode::Stale, "current plan has obsolete requirements");
    const auto nodes = impl_->independent_nodes(request, parse_json_strict(str(plan, "body")));
    impl_->check_dispositions(dispositions, nodes);
    require(!dispositions.as_object().empty(), ErrorCode::InvalidInput, "explicit dispositions are empty");
    const auto existing = impl_->all("SELECT * FROM tasks WHERE request_id=? ORDER BY node_id", {actor.request_id});
    require(!existing.empty(), ErrorCode::NotReady, "there are no independent tasks to amend");
    impl_->check_independent_identities(existing, nodes);
    impl_->cas("UPDATE requests SET state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?", {stamp(), actor.request_id, expected});
    Array changed; Array pending;
    for (const auto& task : existing) {
        const auto node_id = str(task, "node_id");
        const auto* decision = dispositions.as_object().if_contains(node_id);
        if (decision) {
            require(number(task, "based_on_requirements_revision") < requirements, ErrorCode::Conflict, "task already has a current AMEND disposition");
            changed.emplace_back(impl_->apply_independent_task(request, task, revision, nodes.at(node_id), *decision));
        } else if (number(task, "based_on_requirements_revision") < requirements &&
                str(task, "status") != "CANCELLED" && str(task, "status") != "SKIPPED" && str(task, "status") != "SUPERSEDED")
            pending.emplace_back(Row{{"node_id", node_id}, {"task_id", str(task, "task_id")}, {"reason", "amend_disposition_required"}});
    }
    const Json result = Row{{"plan_revision", revision}, {"based_on_requirements_revision", requirements},
        {"state_version", next(expected)}, {"tasks", changed}, {"pending_tasks", pending}};
    impl_->event(actor.request_id, command_id, command, result, "dispatcher");
    tx.commit(); return result;
}

Json Store::amend(const StoreActor& root, std::int64_t expected, const std::string& text,
                  const Json& catalog, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    text_field(text, "amend text", 65536);
    const auto command = payload(root, "amend", Row{{"expected_request_version", expected}, {"text", text}, {"constraint_catalog", catalog}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(root, {Role::Root}, false);
    if (auto old = impl_->replay(root.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto r = impl_->authorize(root, {Role::Root});
    require(!terminal_request(str(r, "status")), ErrorCode::NotReady, "terminal request requires NEW");
    const auto revision = next(number(r, "requirements_revision"));
    impl_->capacity_available(root.request_id, "requirement_revisions");
    impl_->cas("UPDATE requests SET requirements_revision=?,status='RUNNING',delivery_status='NONE',state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?", {revision, stamp(), root.request_id, expected});
    impl_->execute("INSERT INTO request_texts VALUES (?,?,'AMEND',?,?)", {root.request_id, revision, text, stamp()});
    impl_->catalog(root.request_id, revision, catalog);
    impl_->execute("UPDATE teams SET paused_for_change=1,execution_epoch=execution_epoch+1,state_version=state_version+1,updated_at=? WHERE request_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {stamp(), root.request_id});
    impl_->fence_ops(root.request_id);
    const Json result = Row{{"requirements_revision", revision}, {"state_version", next(expected)}};
    impl_->event(root.request_id, command_id, command, result, "root");
    tx.commit(); return result;
}

Json Store::set_amendment_disposition(const StoreActor& actor, const std::string& task_id,
        std::int64_t expected, std::int64_t requirements, std::int64_t plan,
        const std::string& disposition, const std::string& rationale, const Json& brief,
        const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    text_field(rationale, "disposition rationale", 4096);
    require(disposition == "update" || disposition == "keep" || disposition == "cancel", ErrorCode::InvalidInput, "unknown amendment disposition");
    const auto command = payload(actor, "disposition", Row{{"task_id", task_id}, {"expected_task_version", expected},
        {"based_on_requirements_revision", requirements}, {"target_plan_revision", plan}, {"disposition", disposition}, {"rationale", rationale}, {"brief", brief}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher}, false);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto r = impl_->authorize(actor, {Role::Dispatcher}); const auto t = impl_->task_row(task_id);
    require(str(t, "request_id") == actor.request_id, ErrorCode::Unauthorized, "task belongs to another request");
    require(!terminal_request(str(r, "status")) && requirements >= 2 && requirements == number(r, "requirements_revision") && plan == number(r, "plan_revision"), ErrorCode::Stale, "disposition requires current AMEND and plan");
    require(str(t, "status") != "CANCELLED" && str(t, "status") != "SKIPPED" && str(t, "status") != "SUPERSEDED", ErrorCode::NotReady, "task cannot be reactivated");
    require(number(impl_->one("SELECT based_on_requirements_revision FROM plans WHERE request_id=? AND plan_revision=?", {actor.request_id, plan}), "based_on_requirements_revision") == requirements, ErrorCode::Stale, "current amendment plan required");
    if (disposition != "cancel") impl_->brief_constraints(r, brief);
    const auto revision = next(number(t, "task_revision"));
    impl_->cas("UPDATE tasks SET task_revision=?,based_on_requirements_revision=?,plan_revision=?,status=?,brief=?,accepted_with_issues=0,review_round=0,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
        {revision, requirements, plan, disposition == "cancel" ? "CANCELLED" : "READY", disposition == "cancel" ? t.at("brief") : Json(json_text(brief)), stamp(), task_id, expected});
    impl_->execute("INSERT INTO amend_dispositions VALUES (?,?,?,?,?,?,?,?)", {actor.request_id, requirements, task_id, disposition, revision, plan, rationale, stamp()});
    impl_->execute("UPDATE teams SET paused_for_change=1,status=CASE WHEN ?='cancel' THEN 'DRAINING' ELSE status END,state_version=state_version+1,updated_at=? WHERE task_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {disposition, stamp(), task_id});
    const Json result = Row{{"task_id", task_id}, {"task_revision", revision}, {"disposition", disposition}, {"state_version", next(expected)}};
    impl_->event(actor.request_id, command_id, command, result, "dispatcher");
    tx.commit(); return result;
}

Json Store::rebind_task(const StoreActor& actor, const std::string& task_id, std::int64_t expected,
        std::int64_t requirements, std::int64_t plan, const Json& brief, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    const auto command = payload(actor, "rebind_task", Row{{"task_id", task_id}, {"expected_task_version", expected},
        {"based_on_requirements_revision", requirements}, {"target_plan_revision", plan}, {"brief", brief}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher}, false);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto r = impl_->authorize(actor, {Role::Dispatcher}); const auto t = impl_->task_row(task_id);
    require(str(t, "request_id") == actor.request_id, ErrorCode::Unauthorized, "task belongs to another request");
    require(!terminal_request(str(r, "status")) && (!terminal_task(str(t, "status")) || (impl_->acceptance_schema && str(t, "status") == "ACCEPTED")) && requirements == number(r, "requirements_revision") &&
        requirements == number(t, "based_on_requirements_revision") && plan == number(r, "plan_revision") && plan > number(t, "plan_revision"),
        ErrorCode::Stale, "plan-only rebind cannot substitute for AMEND disposition");
    require(number(impl_->one("SELECT based_on_requirements_revision FROM plans WHERE request_id=? AND plan_revision=?", {actor.request_id, plan}), "based_on_requirements_revision") == requirements, ErrorCode::Stale, "current plan required");
    impl_->brief_constraints(r, brief);
    impl_->cas("UPDATE tasks SET task_revision=task_revision+1,plan_revision=?,brief=?,status='READY',accepted_with_issues=0,review_round=0,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {plan, json_text(brief), stamp(), task_id, expected});
    impl_->execute("UPDATE teams SET paused_for_change=1,state_version=state_version+1,updated_at=? WHERE task_id=? AND status='ACTIVE'", {stamp(), task_id});
    const Json result = Row{{"task_id", task_id}, {"task_revision", next(number(t, "task_revision"))}, {"plan_revision", plan}, {"state_version", next(expected)}};
    impl_->event(actor.request_id, command_id, command, result, "dispatcher");
    tx.commit(); return result;
}

Json Store::issue_instruction(const StoreActor& actor, const ExecutionBinding& frozen,
        std::int64_t expected, const Json& body, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    const auto command = payload(actor, "instruction", Row{{"binding", to_json(frozen)}, {"expected_task_version", expected}, {"body", body}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::TeamLead}, false);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto r = impl_->authorize(actor, {Role::TeamLead}); impl_->actor_binding(actor, frozen); impl_->matches(frozen, false);
    const auto t = impl_->task_row(actor.task_id);
    require(!terminal_request(str(r, "status")) && !terminal_task(str(t, "status")) && number(t, "based_on_requirements_revision") == number(r, "requirements_revision") && number(t, "plan_revision") == number(r, "plan_revision"), ErrorCode::NotReady, "current disposition and plan required before instructions");
    if (impl_->workflow_schema) require(!impl_->has_blocking_question(actor.task_id) &&
            str(t, "status") != "SUBMITTED" && str(t, "status") != "REVIEWING" && str(t, "status") != "BLOCKED",
            ErrorCode::NotReady, "pending question/submission must be handled before new instructions");
    impl_->brief_constraints(r, body);
    // R2 corrective revision is not an AMEND. Preserve both TaskBrief and the
    // producer's last instruction constraints before accepting a fresh order.
    // The immutable correction event binds this rule only to its exact semantic
    // target; ordinary AMEND/new-plan behavior is unchanged.
    if (impl_->acceptance_schema && impl_->optional(
            "SELECT 1 FROM events WHERE request_id=? AND type='change_task' AND json_extract(result,'$.task_id')=? AND json_extract(result,'$.target_binding.requirements_revision')=? AND json_extract(result,'$.target_binding.plan_revision')=? AND json_extract(result,'$.target_binding.task_revision')=? LIMIT 1",
            {actor.request_id, actor.task_id, number(r, "requirements_revision"), number(t, "plan_revision"), number(t, "task_revision")})) {
        const auto brief = parse_json_strict(str(t, "brief"));
        const auto prior = impl_->one("SELECT body FROM instructions WHERE task_id=? AND instruction_revision=?", {actor.task_id, number(t, "instruction_revision")});
        const auto old_order = parse_json_strict(str(prior, "body"));
        for (const auto* original : {&brief, &old_order})
            for (const auto& excerpt : original->at("constraints_verbatim").as_array())
                require(contains(body.at("constraints_verbatim"), excerpt.as_string()), ErrorCode::InvalidInput,
                        "corrective instruction drops an existing task or instruction constraint");
    }
    const auto revision = next(number(t, "instruction_revision"));
    impl_->cas("UPDATE tasks SET instruction_revision=?,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {revision, stamp(), actor.task_id, expected});
    impl_->execute("UPDATE teams SET paused_for_change=1,tool_calls_used=0,work_order=?,state_version=state_version+1,updated_at=? WHERE team_id=?", {json_text(body), stamp(), actor.team_id});
    impl_->fence_ops(actor.request_id, actor.team_id);
    impl_->execute("INSERT INTO instructions VALUES (?,?,?,?,?,?,?,?,?,?,?)", {actor.request_id, actor.task_id, actor.team_id, revision, number(r, "requirements_revision"), number(t, "plan_revision"), number(t, "task_revision"), actor.dispatcher_epoch, actor.execution_epoch, json_text(body), stamp()});
    const Json result = Row{{"binding", to_json(impl_->binding(actor.task_id))}, {"state_version", next(expected)}};
    impl_->event(actor.request_id, command_id, command, result, "team_lead");
    tx.commit(); return result;
}

Json Store::acknowledge(const StoreActor& actor, const ExecutionBinding& frozen,
                        std::int64_t expected, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    const auto command = payload(actor, "ack", Row{{"binding", to_json(frozen)}, {"expected_task_version", expected}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Worker}, false);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto r = impl_->authorize(actor, {Role::Worker}); impl_->actor_binding(actor, frozen); impl_->matches(frozen, false);
    const auto t = impl_->task_row(actor.task_id);
    require(!terminal_request(str(r, "status")) && !terminal_task(str(t, "status")) && number(t, "instruction_revision") > 0 &&
        number(t, "based_on_requirements_revision") == number(r, "requirements_revision") && number(t, "plan_revision") == number(r, "plan_revision"), ErrorCode::NotReady, "ack requires current live instruction");
    const auto instruction = impl_->current_instruction(t);
    require(str(instruction, "team_id") == actor.team_id && number(instruction, "task_revision") == number(t, "task_revision") &&
        number(instruction, "plan_revision") == number(t, "plan_revision") && number(instruction, "based_on_requirements_revision") == number(r, "requirements_revision") &&
        number(instruction, "dispatcher_epoch") == actor.dispatcher_epoch && number(instruction, "execution_epoch") == actor.execution_epoch,
        ErrorCode::Stale, "instruction belongs to an obsolete execution context");
    require(number(t, "applied_task_revision") != number(t, "task_revision") || number(t, "applied_plan_revision") != number(t, "plan_revision") ||
        number(t, "applied_instruction_revision") != number(t, "instruction_revision"), ErrorCode::NotReady, "no pending instruction to acknowledge");
    impl_->cas("UPDATE tasks SET applied_task_revision=task_revision,applied_plan_revision=plan_revision,applied_instruction_revision=instruction_revision,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {stamp(), actor.task_id, expected});
    impl_->execute("UPDATE teams SET paused_for_change=0,state_version=state_version+1,updated_at=? WHERE team_id=?", {stamp(), actor.team_id});
    const Json result = Row{{"binding", to_json(frozen)}, {"state_version", next(expected)}};
    impl_->event(actor.request_id, command_id, command, result, "worker");
    tx.commit(); return result;
}

Json Store::Impl::prepare_operation(const StoreActor& actor, const ExecutionBinding& frozen,
        std::int64_t expected, const std::string& business_key, const std::string& tool_id,
        const Json& arguments, const std::string& side_effect, const std::string& command_id, bool registered) {
    text_field(business_key, "business idempotency key"); text_field(tool_id, "tool id");
    require(side_effect == "none" || side_effect == "workspace" || side_effect == "external", ErrorCode::InvalidInput, "invalid side-effect class");
    require(arguments.is_object(), ErrorCode::InvalidInput, "tool arguments must be an object");
    const auto encoded_arguments = json_text(arguments);
    const auto args_hash = sha256_hex(encoded_arguments);
    const auto command = payload(actor, "prepare_op", Row{{"binding", to_json(frozen)}, {"expected_task_version", expected},
        {"business_key", business_key}, {"tool_id", tool_id}, {"arguments_sha256", args_hash}, {"side_effect", side_effect}});
    Transaction tx(*this);
    (void)authorize(actor, {Role::Worker}, false); actor_binding(actor, frozen);
    if (registered) {
        require_tools_schema();
        (void)authorize(actor, {Role::Worker}); matches(frozen);
        tool_grant(request_row(actor.request_id), team_row(actor.team_id), tool_id);
    }
    if (auto old = replay(actor.request_id, command_id, command)) {
        const auto result = registered ? exposed(op_row(str(old->as_object(), "op_id"))) : *old;
        tx.commit(); return result;
    }
    const auto existing = optional("SELECT * FROM ops WHERE request_id=? AND task_id=? AND business_idempotency_key=?", {actor.request_id, actor.task_id, business_key});
    if (existing) {
        require(str(*existing, "team_id") == actor.team_id && str(*existing, "tool_id") == tool_id && str(*existing, "args_sha256") == args_hash &&
            str(*existing, "side_effect") == side_effect, ErrorCode::Conflict, "business key reused with different intent");
        const auto frozen_document = to_json(frozen);
        if (!registered) for (const auto& field : frozen_document.as_object()) require(existing->at(field.key()) == field.value(), ErrorCode::Conflict, "business key reused with different execution binding");
        const auto result = exposed(op_row(str(*existing, "op_id"))); tx.commit(); return result;
    }
    ready(actor, frozen, tool_id);
    if (registered && side_effect == "workspace")
        require(!workspace_info(actor.team_id).is_null(), ErrorCode::NotReady, "team workspace has not been bound");
    const auto r = request_row(actor.request_id);
    const auto team = team_row(actor.team_id);
    require(number(team, "tool_calls_used") < configured_limit(r, "worker_max_tool_calls_per_order"), ErrorCode::Quota, "instruction tool-call budget exhausted");
    capacity_available(actor.request_id, "operations");
    cas("UPDATE tasks SET status='RUNNING',state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {stamp(), actor.task_id, expected});
    const auto id = new_id("op");
    execute("INSERT INTO ops(op_id,request_id,task_id,team_id,tool_id,business_idempotency_key,args_sha256,requirements_revision,plan_revision,task_revision,instruction_revision,dispatcher_epoch,execution_epoch,policy_epoch,side_effect,status,created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'PREPARED',?)",
        {id, frozen.request_id, frozen.task_id, frozen.team_id, tool_id, business_key, args_hash, frozen.requirements_revision, frozen.plan_revision,
         frozen.task_revision, frozen.instruction_revision, frozen.dispatcher_epoch, frozen.execution_epoch, frozen.policy_epoch, side_effect, stamp()});
    execute("INSERT INTO operation_intents(op_id,arguments) VALUES (?,?)", {id, encoded_arguments});
    execute("UPDATE teams SET tool_calls_used=tool_calls_used+1,state_version=state_version+1,updated_at=? WHERE team_id=?", {stamp(), actor.team_id});
    const auto result = exposed(op_row(id));
    event(actor.request_id, command_id, command, result, "worker");
    tx.commit(); return result;
}

Json Store::prepare_operation(const StoreActor& actor, const ExecutionBinding& frozen,
        std::int64_t expected, const std::string& business_key, const std::string& tool_id,
        const Json& arguments, const std::string& side_effect, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    return impl_->prepare_operation(actor, frozen, expected, business_key, tool_id, arguments, side_effect, command_id, false);
}

Json Store::prepare_tool_operation(const StoreActor& actor, const ExecutionBinding& frozen,
        std::int64_t expected, const std::string& business_key, const std::string& tool_id,
        const Json& arguments, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_tools_schema();
    const auto side_effect = tool_side_effect(tool_id);
    return impl_->prepare_operation(actor, frozen, expected, business_key, tool_id, arguments, side_effect, command_id, true);
}

Json Store::start_operation(const StoreActor& actor, const std::string& id,
                            std::int64_t expected, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    const auto command = payload(actor, "start_op", Row{{"op_id", id}, {"expected_op_version", expected}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Worker}, false);
    const auto op = impl_->op_row(id);
    require(str(op, "request_id") == actor.request_id && str(op, "task_id") == actor.task_id && str(op, "team_id") == actor.team_id, ErrorCode::Unauthorized, "operation owner mismatch");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    Row binding_fields;
    const auto current_document = to_json(impl_->binding(actor.task_id));
    for (const auto& field : current_document.as_object()) binding_fields[field.key()] = op.at(field.key());
    impl_->ready(actor, execution_binding_from_json(binding_fields), str(op, "tool_id"));
    require(str(op, "status") == "PREPARED", ErrorCode::NotReady, "only a PREPARED operation may dispatch");
    impl_->cas("UPDATE ops SET status='RUNNING',state_version=state_version+1,started_at=? WHERE op_id=? AND state_version=?", {stamp(), id, expected});
    const auto result = exposed(impl_->op_row(id));
    impl_->event(actor.request_id, command_id, command, result, "worker");
    tx.commit(); return result;
}

Json Store::start_tool_operation(const StoreActor& actor, const std::string& id,
        std::int64_t expected, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_tools_schema();
    const auto command = payload(actor, "start_tool_op", Row{{"op_id", id}, {"expected_op_version", expected}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Worker}, false);
    const auto op = impl_->op_row(id);
    require(str(op, "request_id") == actor.request_id && str(op, "task_id") == actor.task_id && str(op, "team_id") == actor.team_id,
            ErrorCode::Unauthorized, "operation owner mismatch");
    require(tool_side_effect(str(op, "tool_id")) == str(op, "side_effect"), ErrorCode::Unauthorized, "operation differs from static tool classification");
    if (impl_->replay(actor.request_id, command_id, command)) {
        const Json result = Row{{"operation", exposed(op)}, {"dispatch_claimed", false}};
        tx.commit(); return result;
    }
    const auto request = impl_->authorize(actor, {Role::Worker});
    impl_->tool_grant(request, impl_->team_row(actor.team_id), str(op, "tool_id"));
    if (str(op, "status") != "PREPARED") {
        const Json result = Row{{"operation", exposed(op)}, {"dispatch_claimed", false}};
        tx.commit(); return result;
    }
    Row binding_fields;
    const auto current_document = to_json(impl_->binding(actor.task_id));
    for (const auto& field : current_document.as_object()) binding_fields[field.key()] = op.at(field.key());
    impl_->ready(actor, execution_binding_from_json(binding_fields), str(op, "tool_id"));
    if (str(op, "side_effect") == "workspace")
        require(!impl_->workspace_info(actor.team_id).is_null(), ErrorCode::NotReady, "team workspace has not been bound");
    impl_->cas("UPDATE ops SET status='RUNNING',state_version=state_version+1,started_at=? WHERE op_id=? AND state_version=?",
        {stamp(), id, expected});
    const Json result = Row{{"operation", exposed(impl_->op_row(id))}, {"dispatch_claimed", true}};
    impl_->event(actor.request_id, command_id, command, result, "worker");
    tx.commit(); return result;
}

Json Store::finish_operation(const StoreActor& actor, const std::string& id,
        const ExecutionBinding& frozen, std::int64_t expected, const std::string& outcome,
        const std::string& evidence, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    require(outcome == "SUCCEEDED" || outcome == "FAILED" || outcome == "UNKNOWN", ErrorCode::InvalidInput, "invalid execution outcome");
    text_field(evidence, "operation evidence", 512);
    const auto command = payload(actor, "finish_op", Row{{"op_id", id}, {"binding", to_json(frozen)}, {"expected_op_version", expected}, {"outcome", outcome}, {"evidence_ref", evidence}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Worker}, false);
    const auto op = impl_->op_row(id);
    require(str(op, "request_id") == actor.request_id && str(op, "team_id") == actor.team_id && str(op, "task_id") == actor.task_id, ErrorCode::Unauthorized, "operation owner mismatch");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    (void)impl_->authorize(actor, {Role::Worker}); impl_->actor_binding(actor, frozen); impl_->matches(frozen);
    const auto frozen_document = to_json(frozen);
    for (const auto& field : frozen_document.as_object()) require(op.at(field.key()) == field.value(), ErrorCode::Stale, "result does not match original operation context");
    require(str(op, "status") == "RUNNING", ErrorCode::NotReady, "only RUNNING operation can complete directly");
    impl_->cas("UPDATE ops SET status=?,state_version=state_version+1,ended_at=? WHERE op_id=? AND state_version=?", {outcome, stamp(), id, expected});
    impl_->execute("UPDATE operation_intents SET evidence_ref=?,state_version=state_version+1 WHERE op_id=?", {evidence, id});
    const auto result = exposed(impl_->op_row(id));
    impl_->event(actor.request_id, command_id, command, result, "worker");
    tx.commit(); return result;
}

Json Store::cancel_operation(const StoreActor& actor, const std::string& id,
                             std::int64_t expected, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    const auto command = payload(actor, "cancel_op", Row{{"op_id", id}, {"expected_op_version", expected}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Worker}, false);
    const auto op = impl_->op_row(id);
    require(str(op, "request_id") == actor.request_id && str(op, "team_id") == actor.team_id && str(op, "task_id") == actor.task_id, ErrorCode::Unauthorized, "operation owner mismatch");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    (void)impl_->authorize(actor, {Role::Worker});
    require(str(op, "status") == "PREPARED" || str(op, "status") == "RUNNING", ErrorCode::NotReady, "cannot repeatedly cancel a settled or UNKNOWN operation");
    impl_->cas("UPDATE ops SET status=?,state_version=state_version+1 WHERE op_id=? AND state_version=?", {str(op, "status") == "PREPARED" ? "CANCELLED" : "UNKNOWN", id, expected});
    const auto result = exposed(impl_->op_row(id));
    impl_->event(actor.request_id, command_id, command, result, "worker");
    tx.commit(); return result;
}

Json Store::reconcile_operation(const std::string& id, std::int64_t expected,
        const std::string& outcome, const std::string& evidence, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    require(outcome == "SUCCEEDED" || outcome == "FAILED" || outcome == "CANCELLED", ErrorCode::InvalidInput, "reconciliation needs definite outcome");
    text_field(evidence, "host evidence", 512);
    const Row command{{"type", "reconcile_op"}, {"op_id", id}, {"expected_op_version", expected}, {"outcome", outcome}, {"host_evidence", evidence}};
    Impl::Transaction tx(*impl_);
    const auto op = impl_->op_row(id); const auto request_id = str(op, "request_id");
    if (auto old = impl_->replay(request_id, command_id, command)) { tx.commit(); return *old; }
    require(str(op, "status") == "UNKNOWN", ErrorCode::NotReady, "only UNKNOWN operation needs reconciliation");
    impl_->cas("UPDATE ops SET status=?,state_version=state_version+1,ended_at=? WHERE op_id=? AND state_version=?", {outcome, stamp(), id, expected});
    impl_->execute("UPDATE operation_intents SET evidence_ref=?,state_version=state_version+1 WHERE op_id=?", {evidence, id});
    const auto result = exposed(impl_->op_row(id));
    impl_->event(request_id, command_id, command, result);
    tx.commit(); return result;
}

void Store::validate_reconciliation(const StoreActor& root, const std::string& id, std::int64_t expected,
        const std::string& outcome, const std::string& evidence, const std::string& command_id) const {
    const std::lock_guard lock(impl_->mutex);
    const auto command = reconciliation_command(root, id, expected, outcome, evidence, command_id);
    (void)impl_->check_reconciliation(root, id, expected, command_id, command);
}
Json Store::reconcile_operation(const StoreActor& root, const std::string& id, std::int64_t expected,
        const std::string& outcome, const std::string& evidence, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    const auto command = reconciliation_command(root, id, expected, outcome, evidence, command_id);
    Impl::Transaction tx(*impl_);
    if (auto old = impl_->check_reconciliation(root, id, expected, command_id, command)) { tx.commit(); return *old; }
    impl_->cas("UPDATE ops SET status=?,state_version=state_version+1,ended_at=? WHERE op_id=? AND state_version=?",
        {outcome, stamp(), id, expected});
    // Preserve original result/BLOB/exit evidence. The host observation is a
    // separate immutable event, not a rewritten executor fact or cleanup proof.
    impl_->execute("UPDATE operation_intents SET evidence_ref=?,state_version=state_version+1 WHERE op_id=?",
        {"sha256:" + sha256_hex(evidence), id});
    const auto result = exposed(impl_->op_row(id));
    impl_->event(root.request_id, command_id, command, result, "root");
    tx.commit(); return result;
}

Json Store::bind_workspace(const std::string& team_id, const std::string& path,
        std::uint64_t device, std::uint64_t inode, const Json& execution_policy) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_tools_schema();
    text_field(team_id, "workspace team"); text_field(path, "workspace path", 4096);
    const std::filesystem::path directory(path);
    require(directory.is_absolute() && path != "/" && path.back() != '/' && directory.lexically_normal().string() == path,
            ErrorCode::InvalidInput, "workspace path must be an absolute canonical directory");
    // Workers may mutate every permitted path below their directory. Never
    // place this Store's control plane (including its lease and SQLite side
    // files) inside such a directory. Separate Stores remain a deployment
    // ownership boundary; this check does not scan other users' databases.
    for (const auto* suffix : {"", ".writer-lock", "-wal", "-shm"}) {
        const auto control_path = impl_->database_file_path + suffix;
        require(control_path != path && !control_path.starts_with(path + "/"), ErrorCode::Unauthorized,
                "business workspace would expose this Store's database or control files");
    }
    require(device > 0 && inode > 0 && device <= static_cast<std::uint64_t>(INT64_MAX) && inode <= static_cast<std::uint64_t>(INT64_MAX),
            ErrorCode::InvalidInput, "workspace device/inode out of range");
    require(execution_policy.is_object(), ErrorCode::InvalidInput, "execution policy must be an object");
    const auto encoded_policy = json_text(execution_policy, 262144);
    const auto policy_hash = sha256_hex(encoded_policy);
    const auto signed_device = static_cast<std::int64_t>(device);
    const auto signed_inode = static_cast<std::int64_t>(inode);
    Impl::Transaction tx(*impl_);
    const auto team = impl_->team_row(team_id);
    if (impl_->cleanup_schema) {
        if (const auto owned = impl_->optional("SELECT * FROM owned_workspaces WHERE team_id=?", {team_id})) {
            require(str(*owned, "status") == "OWNED", ErrorCode::NotReady, "owned workspace is not ready for binding");
            const auto identity = impl_->owned_info(*owned).at("identity").at("workspace_identity").as_object();
            require(str(identity, "path") == path && number(identity, "device") == signed_device && number(identity, "inode") == signed_inode,
                    ErrorCode::Conflict, "owned team cannot bind a different directory");
        }
    }
    if (const auto old = impl_->optional("SELECT * FROM workspace_bindings WHERE team_id=?", {team_id})) {
        require(str(*old, "canonical_path") == path && number(*old, "device") == signed_device && number(*old, "inode") == signed_inode &&
                str(*old, "execution_policy") == encoded_policy && str(*old, "execution_policy_sha256") == policy_hash,
                ErrorCode::Conflict, "team workspace identity or execution policy changed");
        const auto result = exposed(*old); tx.commit(); return result;
    }
    require(str(team, "status") == "STARTING" || str(team, "status") == "ACTIVE", ErrorCode::NotReady, "stopped/draining team cannot acquire a new workspace");
    for (const auto& other : impl_->all("SELECT canonical_path,device,inode FROM workspace_bindings")) {
        const auto other_path = str(other, "canonical_path");
        require(path != other_path && !path.starts_with(other_path + "/") && !other_path.starts_with(path + "/") &&
                !(number(other, "device") == signed_device && number(other, "inode") == signed_inode),
                ErrorCode::Conflict, "workspace overlaps or aliases another team's workspace");
    }
    if (impl_->cleanup_schema) for (const auto& resource : impl_->all("SELECT team_id,intent FROM owned_workspaces")) {
        if (str(resource, "team_id") == team_id) continue;
        const auto intent = parse_json_strict(str(resource, "intent"));
        const auto envelope = str(intent.at("parent_identity").as_object(), "path") + "/" + str(intent.as_object(), "leaf");
        require(!overlap(path, envelope), ErrorCode::Unauthorized, "business workspace overlaps another ownership envelope");
    }
    if (impl_->acceptance_schema) for (const auto& stored : impl_->all("SELECT frozen_plan FROM delivery_runtime")) {
        const auto root = parse_json_strict(str(stored, "frozen_plan"), 8 * 1024 * 1024).at("root_identity");
        require(!overlap(path, str(root.as_object(), "path")) &&
                !(number(root.as_object(), "device") == signed_device && number(root.as_object(), "inode") == signed_inode),
                ErrorCode::Unauthorized, "workspace overlaps or aliases a retained delivery root");
    }
    impl_->execute("INSERT INTO workspace_bindings VALUES (?,?,?,?,?,?,?)", {team_id, path, signed_device, signed_inode, encoded_policy, policy_hash, stamp()});
    impl_->execute("UPDATE teams SET workspace_path=?,state_version=state_version+1,updated_at=? WHERE team_id=?", {path, stamp(), team_id});
    const auto result = impl_->workspace_info(team_id);
    impl_->event(str(team, "request_id"), new_id("workspace"), Row{{"type", "bind_workspace"}, {"team_id", team_id},
        {"canonical_path", path}, {"device", signed_device}, {"inode", signed_inode}, {"execution_policy_sha256", policy_hash}}, result);
    tx.commit(); return result;
}

Json Store::workspace(const std::string& team_id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->workspace_info(team_id);
}

Json Store::operation_for_key(const StoreActor& actor, const ExecutionBinding& current,
        const std::string& business_key, const std::string& tool_id, const Json& arguments) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_tools_schema();
    text_field(business_key, "business idempotency key");
    const auto side_effect = tool_side_effect(tool_id);
    require(arguments.is_object(), ErrorCode::InvalidInput, "tool arguments must be an object");
    const auto hash = sha256_hex(json_text(arguments));
    const auto request = impl_->authorize(actor, {Role::Worker});
    impl_->actor_binding(actor, current); impl_->matches(current);
    impl_->tool_grant(request, impl_->team_row(actor.team_id), tool_id);
    const auto old = impl_->optional("SELECT * FROM ops WHERE request_id=? AND task_id=? AND business_idempotency_key=?", {actor.request_id, actor.task_id, business_key});
    if (!old) return nullptr;
    require(str(*old, "team_id") == actor.team_id && str(*old, "tool_id") == tool_id && str(*old, "side_effect") == side_effect && str(*old, "args_sha256") == hash,
            ErrorCode::Conflict, "business key reused with different tool, arguments or owner");
    return exposed(impl_->op_row(str(*old, "op_id")));
}

Json Store::authorize_diagnostic(const StoreActor& actor, const ExecutionBinding& current,
        const std::string& tool_id) const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->control_authorization(actor, current, tool_id, true);
}

Json Store::authorize_control(const StoreActor& actor, const ExecutionBinding& current,
        const std::string& tool_id) const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->control_authorization(actor, current, tool_id, false);
}

std::vector<std::string> Store::authorized_worker_tools(const StoreActor& actor,
        const ExecutionBinding& current) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_tools_schema();
    const auto request = impl_->authorize(actor, {Role::Worker});
    impl_->actor_binding(actor, current); impl_->matches(current);
    const auto team = impl_->team_row(actor.team_id);
    const auto frozen = FrozenRequestConfig::restore(str(request, "config_snapshot"), str(request, "config_snapshot_sha256"));
    const auto template_tools = frozen.worker_tools(str(team, "template_id"));
    const auto allowed = parse_json_strict(str(request, "tool_allowlist"));
    const auto denied = parse_json_strict(str(impl_->one("SELECT denied_tools FROM runtime_policy WHERE singleton=1"), "denied_tools"));
    std::vector<std::string> result;
    for (const auto& tool : template_tools) if (contains(allowed, tool) && !contains(denied, tool)) result.push_back(tool);
    return result;
}

Json Store::execute_tool_control(const StoreActor& actor, const ExecutionBinding& original,
        std::int64_t expected, const std::string& tool_id, const Json& arguments,
        const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_tools_schema();
    require(tool_id == "ack_change" || tool_id == "op_cancel", ErrorCode::Unauthorized, "tool is not registered for control execution");
    require(expected > 0 && arguments.is_object(), ErrorCode::InvalidInput, "invalid original control command");
    (void)json_text(arguments, 4096);
    std::string op_id;
    if (tool_id == "ack_change") require(arguments.as_object().empty(), ErrorCode::InvalidInput, "ack_change takes no model arguments");
    else {
        require(arguments.as_object().size() == 1 && arguments.as_object().contains("op_id") && arguments.at("op_id").is_string(),
                ErrorCode::InvalidInput, "op_cancel takes exactly one operation identifier");
        op_id = std::string(arguments.at("op_id").as_string());
        require(is_typed_id(op_id, "op"), ErrorCode::InvalidInput, "invalid control operation id");
    }
    const auto command = payload(actor, "execute_tool_control", Row{{"binding", to_json(original)}, {"expected_task_version", expected},
        {"tool_id", tool_id}, {"arguments", arguments}});
    Impl::Transaction tx(*impl_);
    const auto request = impl_->authorize(actor, {Role::Worker}, false);
    impl_->actor_binding(actor, original);
    impl_->tool_grant(request, impl_->team_row(actor.team_id), tool_id);
    if (!op_id.empty()) {
        const auto op = impl_->op_row(op_id);
        require(str(op, "request_id") == actor.request_id && str(op, "task_id") == actor.task_id && str(op, "team_id") == actor.team_id,
                ErrorCode::Unauthorized, "control operation belongs to another team");
    }
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    (void)impl_->authorize(actor, {Role::Worker}); impl_->matches(original);
    Json result;
    if (tool_id == "ack_change") {
        const auto task = impl_->task_row(actor.task_id);
        require(!terminal_request(str(request, "status")) && !terminal_task(str(task, "status")) && number(task, "instruction_revision") > 0 &&
            number(task, "based_on_requirements_revision") == number(request, "requirements_revision") && number(task, "plan_revision") == number(request, "plan_revision"),
            ErrorCode::NotReady, "ack requires current live instruction");
        const auto instruction = impl_->current_instruction(task);
        require(str(instruction, "team_id") == actor.team_id && number(instruction, "task_revision") == number(task, "task_revision") &&
            number(instruction, "plan_revision") == number(task, "plan_revision") && number(instruction, "based_on_requirements_revision") == number(request, "requirements_revision") &&
            number(instruction, "dispatcher_epoch") == actor.dispatcher_epoch && number(instruction, "execution_epoch") == actor.execution_epoch,
            ErrorCode::Stale, "instruction belongs to an obsolete execution context");
        require(number(task, "applied_task_revision") != number(task, "task_revision") || number(task, "applied_plan_revision") != number(task, "plan_revision") ||
            number(task, "applied_instruction_revision") != number(task, "instruction_revision"), ErrorCode::NotReady, "no pending instruction to acknowledge");
        impl_->cas("UPDATE tasks SET applied_task_revision=task_revision,applied_plan_revision=plan_revision,applied_instruction_revision=instruction_revision,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
            {stamp(), actor.task_id, expected});
        impl_->execute("UPDATE teams SET paused_for_change=0,state_version=state_version+1,updated_at=? WHERE team_id=?", {stamp(), actor.team_id});
        result = Row{{"binding", to_json(original)}, {"state_version", next(expected)}};
    } else {
        const auto op = impl_->op_row(op_id);
        require(str(op, "status") == "PREPARED" || str(op, "status") == "RUNNING", ErrorCode::NotReady, "cannot repeatedly cancel a settled or UNKNOWN operation");
        impl_->cas("UPDATE ops SET status=?,state_version=state_version+1 WHERE op_id=? AND state_version=?",
            {str(op, "status") == "PREPARED" ? "CANCELLED" : "UNKNOWN", op_id, number(op, "state_version")});
        result = exposed(impl_->op_row(op_id));
    }
    impl_->event(actor.request_id, command_id, command, result, "worker");
    tx.commit(); return result;
}

Json Store::record_operation_result(const std::string& id, const ExecutionBinding& original,
        const std::string& outcome, const Json& result, const Json& evidence,
        const std::string& stdout_bytes, const std::string& stderr_bytes,
        bool stdout_truncated, bool stderr_truncated) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_tools_schema();
    require(outcome == "SUCCEEDED" || outcome == "FAILED" || outcome == "TIMED_OUT" || outcome == "CANCELLED" || outcome == "UNKNOWN",
            ErrorCode::InvalidInput, "invalid recorded operation outcome");
    require(evidence.is_object() && !evidence.as_object().empty(), ErrorCode::InvalidInput, "execution evidence must be a nonempty object");
    require(stdout_bytes.size() <= 1048576 && stderr_bytes.size() <= 1048576, ErrorCode::Quota, "operation output exceeds retained byte capacity");
    const auto result_text = json_text(result, 262144);
    const auto evidence_text = json_text(evidence, 262144);
    const auto binding_json = to_json(original);
    const auto binding_text = json_text(binding_json);
    const auto stdout_hash = sha256_hex(stdout_bytes);
    const auto stderr_hash = sha256_hex(stderr_bytes);
    const auto stdout_size = static_cast<std::int64_t>(stdout_bytes.size());
    const auto stderr_size = static_cast<std::int64_t>(stderr_bytes.size());
    const auto record_hash = sha256_hex(canonical_json(Row{{"op_id", id}, {"original_binding", binding_json}, {"outcome", outcome},
        {"result", result}, {"evidence", evidence}, {"stdout_bytes", stdout_size}, {"stderr_bytes", stderr_size},
        {"stdout_sha256", stdout_hash}, {"stderr_sha256", stderr_hash}, {"stdout_truncated", stdout_truncated}, {"stderr_truncated", stderr_truncated}}));
    Impl::Transaction tx(*impl_);
    const auto op = impl_->op_row(id);
    for (const auto& field : binding_json.as_object())
        require(op.at(field.key()) == field.value(), ErrorCode::Stale, "recorded execution fact differs from original operation binding");
    if (const auto old = impl_->optional("SELECT record_sha256 FROM operation_results WHERE op_id=?", {id})) {
        require(str(*old, "record_sha256") == record_hash, ErrorCode::Conflict, "operation execution fact already recorded with different content");
        const auto persisted = impl_->result_info(id); tx.commit(); return persisted;
    }
    require(!op.at("started_at").is_null(), ErrorCode::NotReady, "an undispatched operation cannot have an execution result");
    const auto request = impl_->request_row(original.request_id);
    const auto task = impl_->task_row(original.task_id);
    const auto team = impl_->team_row(original.team_id);
    const auto policy_epoch = number(impl_->one("SELECT policy_epoch FROM runtime_policy WHERE singleton=1"), "policy_epoch");
    const bool eligible = str(op, "status") == "RUNNING" &&
        (str(request, "status") == "RUNNING" || str(request, "status") == "VERIFYING") &&
        (str(task, "status") == "READY" || str(task, "status") == "RUNNING") && str(team, "status") == "ACTIVE" && number(team, "paused_for_change") == 0 &&
        original.requirements_revision == number(request, "requirements_revision") && original.requirements_revision == number(task, "based_on_requirements_revision") &&
        original.plan_revision == number(request, "plan_revision") && original.plan_revision == number(task, "plan_revision") &&
        original.task_revision == number(task, "task_revision") && original.instruction_revision == number(task, "instruction_revision") &&
        original.dispatcher_epoch == number(request, "dispatcher_epoch") && original.execution_epoch == number(team, "execution_epoch") && original.policy_epoch == policy_epoch;
    const bool accepted = eligible && outcome != "UNKNOWN";
    if (str(op, "status") == "RUNNING") {
        impl_->cas("UPDATE ops SET status=?,state_version=state_version+1,ended_at=? WHERE op_id=? AND state_version=?",
            {eligible ? outcome : "UNKNOWN", stamp(), id, number(op, "state_version")});
    }
    impl_->execute("INSERT INTO operation_results VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {id, binding_text, outcome, accepted, result_text, evidence_text, stdout_size, stderr_size,
         stdout_truncated, stderr_truncated, stdout_hash, stderr_hash, record_hash, stamp()});
    impl_->execute("INSERT INTO operation_output_bytes VALUES (?,'stdout',CAST(? AS BLOB))", {id, stdout_bytes});
    impl_->execute("INSERT INTO operation_output_bytes VALUES (?,'stderr',CAST(? AS BLOB))", {id, stderr_bytes});
    impl_->execute("UPDATE operation_intents SET evidence_ref=?,state_version=state_version+1 WHERE op_id=?", {"store:operation_result:" + id + "#" + record_hash, id});
    const auto persisted = impl_->result_info(id);
    const Row command{{"type", "record_op_result"}, {"op_id", id}, {"original_binding", binding_json}, {"record_sha256", record_hash}};
    impl_->event(original.request_id, "result:" + id, command, persisted);
    tx.commit(); return persisted;
}

Json Store::operation_result(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->result_info(id);
}

Json Store::operation_output(const StoreActor& actor, const ExecutionBinding& current,
        const std::string& id, const std::string& stream, std::int64_t offset, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    (void)impl_->control_authorization(actor, current, "op_output", true);
    require((stream == "stdout" || stream == "stderr") && offset >= 0 && limit > 0 && limit <= 65536 && offset <= 1048576,
            ErrorCode::InvalidInput, "invalid operation output page");
    const auto op = impl_->op_row(id);
    require(str(op, "request_id") == actor.request_id && str(op, "task_id") == actor.task_id && str(op, "team_id") == actor.team_id,
            ErrorCode::Unauthorized, "operation output belongs to another team");
    const auto record = impl_->result_info(id);
    require(!record.is_null(), ErrorCode::NotReady, "operation output is not durably recorded yet");
    const auto total = number(record.as_object(), stream + "_bytes");
    require(offset <= total, ErrorCode::InvalidInput, "output offset exceeds retained bytes");
    const auto row = impl_->one("SELECT hex(substr(bytes,?,?)) AS data FROM operation_output_bytes WHERE op_id=? AND stream=?", {offset + 1, limit, id, stream});
    const auto bytes = unhex(str(row, "data"));
    const auto next_offset = offset + static_cast<std::int64_t>(bytes.size());
    return Row{{"op_id", id}, {"stream", stream}, {"encoding", "base64"}, {"data", base64_bytes(bytes)}, {"offset", offset},
        {"next_offset", next_offset}, {"total_bytes", total}, {"truncated", number(record.as_object(), stream + "_truncated") == 1}, {"eof", next_offset == total}};
}

Json Store::recover_request(const std::string& id, std::int64_t expected) {
    const std::lock_guard lock(impl_->mutex);
    Impl::Transaction tx(*impl_);
    const auto r = impl_->request_row(id);
    require(number(r, "state_version") == expected, ErrorCode::Conflict, "request CAS failed");
    require(!terminal_request(str(r, "status")), ErrorCode::NotReady, "terminal work recovers through independent finalization jobs");
    impl_->recover(r);
    const auto result = exposed(impl_->request_row(id)); tx.commit(); return result;
}
Json Store::cancel_request(const StoreActor& root, std::int64_t expected, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    const auto command = payload(root, "cancel_request", Row{{"expected_request_version", expected}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(root, {Role::Root}, false);
    if (auto old = impl_->replay(root.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto r = impl_->authorize(root, {Role::Root});
    require(!terminal_request(str(r, "status")), ErrorCode::NotReady, "request already terminal");
    impl_->cas("UPDATE requests SET status='CANCELLED',dispatcher_epoch=dispatcher_epoch+1,state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?", {stamp(), root.request_id, expected});
    impl_->execute("UPDATE tasks SET status='CANCELLED',state_version=state_version+1,updated_at=? WHERE request_id=? AND status NOT IN ('FAILED','CANCELLED','SKIPPED','SUPERSEDED')", {stamp(), root.request_id});
    impl_->execute("UPDATE teams SET status='DRAINING',execution_epoch=execution_epoch+1,paused_for_change=1,state_version=state_version+1,updated_at=? WHERE request_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {stamp(), root.request_id});
    impl_->fence_ops(root.request_id); impl_->unknown_attempts(root.request_id);
    if (impl_->acceptance_schema) impl_->retain_closeout(impl_->request_row(root.request_id));
    const auto result = exposed(impl_->request_row(root.request_id));
    impl_->event(root.request_id, command_id, command, result, "root");
    tx.commit(); return result;
}

Json Store::reserve_attempt(const StoreActor& actor, const ExecutionBinding& frozen,
                            const AttemptReservation& reservation) {
    const std::lock_guard lock(impl_->mutex);
    text_field(reservation.call_id, "call_id"); text_field(reservation.attempt_id, "attempt_id");
    text_field(reservation.model_ref, "model_ref");
    require(reservation.adapter_kind == "fake", ErrorCode::Unsupported, "this Store increment only admits the fake adapter");
    require(reservation.reserved_input_tokens >= 0 && reservation.reserved_output_tokens >= 0 &&
        reservation.reserved_cost_micro_usd >= 0 && reservation.max_output_tokens > 0 &&
        reservation.max_output_tokens <= reservation.reserved_output_tokens,
        ErrorCode::InvalidInput, "model attempt requires bounded nonnegative reservations");
    auto command = payload(actor, "reserve_attempt", Row{{"binding", to_json(frozen)}, {"call_id", reservation.call_id},
        {"attempt_id", reservation.attempt_id}, {"config_snapshot_sha256", reservation.config_snapshot_sha256},
        {"model_ref", reservation.model_ref}, {"adapter_kind", reservation.adapter_kind},
        {"reserved_input_tokens", reservation.reserved_input_tokens}, {"reserved_output_tokens", reservation.reserved_output_tokens},
        {"reserved_cost_micro_usd", reservation.reserved_cost_micro_usd}, {"max_output_tokens", reservation.max_output_tokens}, {"request_envelope", reservation.request_envelope}});
    const bool experience = !reservation.experience_job_id.empty();
    if (experience) {
        impl_->require_acceptance_schema();
        command["experience_job_id"] = reservation.experience_job_id;
        command["experience_job_version"] = reservation.experience_job_version;
        command["experience_source_sha256"] = reservation.experience_source_sha256;
    } else require(reservation.experience_job_version == 0 && reservation.experience_source_sha256.empty(),
                   ErrorCode::InvalidInput, "experience reservation fields require a job");
    require(reservation.request_envelope.is_object(), ErrorCode::InvalidInput, "complete model request envelope is required");
    const auto original_model_request = model_request_from_json(reservation.request_envelope);
    {
        const auto& envelope = reservation.request_envelope.as_object();
        require(str(envelope, "call_id") == reservation.call_id && str(envelope, "attempt_id") == reservation.attempt_id &&
            str(envelope, "config_snapshot_id") == reservation.config_snapshot_sha256 && str(envelope, "role") == role_name(actor.role) &&
            execution_binding_from_json(envelope.at("binding")) == frozen && number(envelope, "max_output_tokens") == reservation.max_output_tokens &&
            number(envelope, "reserved_input_tokens") == reservation.reserved_input_tokens && number(envelope, "reserved_output_tokens") == reservation.reserved_output_tokens &&
            number(envelope, "reserved_cost_micro_usd") == reservation.reserved_cost_micro_usd,
            ErrorCode::Conflict, "model request envelope differs from its reservation");
        require(envelope.at("context").is_object() && str(envelope.at("context").as_object(), "model_ref") == reservation.model_ref,
            ErrorCode::Conflict, "model request envelope changed the reserved model");
    }
    const auto fingerprint = sha256_hex(canonical_json(command));
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker}, false);
    impl_->actor_binding(actor, frozen);
    const auto existing = impl_->optional("SELECT * FROM call_attempts WHERE attempt_id=? OR call_id=?", {reservation.attempt_id, reservation.call_id});
    if (existing) {
        require(str(*existing, "attempt_id") == reservation.attempt_id && str(*existing, "call_id") == reservation.call_id &&
            str(*existing, "reservation_sha256") == fingerprint, ErrorCode::Conflict, "call/attempt ID reused or unpaid retry is not authorized");
        const auto result = exposed(*existing); tx.commit(); return result;
    }
    const auto r = impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    require(experience || !terminal_request(str(r, "status")), ErrorCode::NotReady, "business model attempt after request termination");
    impl_->matches(frozen);
    require(str(r, "config_snapshot_sha256") == reservation.config_snapshot_sha256, ErrorCode::Stale, "model attempt changed the frozen request configuration");
    const auto frozen_config = FrozenRequestConfig::restore(str(r, "config_snapshot"), str(r, "config_snapshot_sha256"));
    require(original_model_request.context.structured() ==
            (frozen_config.context_request_format() == "structured-v2"), ErrorCode::Unauthorized,
            "model context format differs from frozen request policy");
    const auto selected_model = frozen_config.resolve(actor.role);
    const auto profile = frozen_config.model_profile(actor.role);
    require(selected_model.model == reservation.model_ref, ErrorCode::Unauthorized, "model reference differs from frozen role selection");
    require(original_model_request.model == selected_model && original_model_request.context.role == actor.role &&
        original_model_request.context.adapter_version == std::string(profile.at("adapter_version").as_string()) &&
        original_model_request.context.protocol_version == std::string(profile.at("protocol").as_string()),
        ErrorCode::Unauthorized, "compiled model context differs from frozen role/profile settings");
    if (!original_model_request.context.tool_names.empty()) require(profile.at("capabilities").at("tool_calling").as_string() == "supported",
        ErrorCode::Unsupported, "frozen model profile does not support tool calls");
    require(profile.at("adapter").as_string() == "fake", ErrorCode::Unsupported, "only frozen fake profiles are implemented");
    require(reservation.max_output_tokens <= number(profile.at("capabilities").as_object(), "max_output_tokens"), ErrorCode::Quota, "attempt output exceeds frozen model capability");
    if (actor.role == Role::Worker) {
        impl_->ready(actor, frozen);
        const auto history_policy = frozen_config.worker_history_policy();
        if (!history_policy.is_null()) {
            impl_->require_events_schema();
            const auto contract = parse_json_strict(compiled_task_contract(original_model_request.context));
            const auto* declared = contract.as_object().if_contains("worker_history_policy");
            require(declared && *declared == history_policy, ErrorCode::Unauthorized,
                "Worker window differs from frozen history policy");
        }
        for (const auto& tool : original_model_request.context.tool_names) {
            if (tool == "history_page" || tool == "history_read") {
                const auto denied = parse_json_strict(str(impl_->one("SELECT denied_tools FROM runtime_policy WHERE singleton=1"), "denied_tools"));
                require(!history_policy.is_null() && !contains(denied, tool), ErrorCode::Unauthorized,
                    "history control is absent from frozen/current policy");
            } else impl_->ready(actor, frozen, tool);
        }
    }
    if (experience) {
        const auto job = impl_->validate_experience_target(actor, frozen, reservation.experience_job_id,
            reservation.experience_job_version, reservation.experience_source_sha256);
        require(original_model_request.context.tool_names.size() == 1 && original_model_request.context.tool_names.front() == "report_experience",
                ErrorCode::Unauthorized, "experience attempt may declare only report_experience");
        const auto contract = model_contract(original_model_request);
        require(contract.is_object() && contract.as_object().contains("workflow_action") && contract.at("workflow_action").is_object(),
                ErrorCode::Unauthorized, "experience attempt lacks exact captured workflow target");
        const auto& action = contract.at("workflow_action");
        require(action.at("kind").as_string() == "experience" && action.at("target").at("experience_id") == job.at("experience_id") &&
                action.at("target").at("state_version") == job.at("state_version") &&
                action.at("target").at("source_sha256") == job.at("source_sha256") &&
                action.at("target").at("source_snapshot") == job.at("source_snapshot"),
                ErrorCode::Conflict, "experience model context changed the captured source");
        require(!impl_->optional("SELECT e.attempt_id FROM experience_attempts e JOIN call_attempts c USING(attempt_id) WHERE e.experience_id=? AND c.status='PENDING' LIMIT 1", {reservation.experience_job_id}),
                ErrorCode::Conflict, "experience job already has an in-flight attempt");
    } else if (actor.role == Role::TeamLead) {
        const auto t = impl_->task_row(actor.task_id);
        require(!terminal_task(str(t, "status")) && number(t, "based_on_requirements_revision") == number(r, "requirements_revision") &&
            number(t, "plan_revision") == number(r, "plan_revision"), ErrorCode::NotReady, "TeamLead model attempt requires current task disposition");
    }
    const auto budget_row = impl_->one("SELECT * FROM request_budgets WHERE request_id=?", {actor.request_id});
    require(number(budget_row, "attempts") < number(budget_row, "max_attempts"), ErrorCode::Quota, "model attempt count exhausted");
    for (const auto& dimensions : {std::pair<std::string, std::int64_t>{"input", reservation.reserved_input_tokens},
             {"output", reservation.reserved_output_tokens}, {"cost", reservation.reserved_cost_micro_usd}}) {
        const auto committed = add(number(budget_row, "spent_" + dimensions.first), number(budget_row, "reserved_" + dimensions.first));
        require(add(committed, dimensions.second) <= number(budget_row, dimensions.first + "_limit"), ErrorCode::Quota, "model " + dimensions.first + " budget exhausted");
    }
    impl_->cas("UPDATE request_budgets SET attempts=attempts+1,reserved_input=reserved_input+?,reserved_output=reserved_output+?,reserved_cost=reserved_cost+?,state_version=state_version+1 WHERE request_id=? AND state_version=?",
        {reservation.reserved_input_tokens, reservation.reserved_output_tokens, reservation.reserved_cost_micro_usd, actor.request_id, number(budget_row, "state_version")});
    impl_->execute("INSERT INTO call_attempts(attempt_id,call_id,request_id,task_id,team_id,role,principal_id,config_snapshot_sha256,model_ref,frozen_binding,request_envelope,reservation_sha256,reserved_input,reserved_output,reserved_cost,max_output_tokens,status,created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'PENDING',?)",
        {reservation.attempt_id, reservation.call_id, actor.request_id, actor.task_id.empty() ? Json(nullptr) : Json(actor.task_id),
         actor.team_id.empty() ? Json(nullptr) : Json(actor.team_id), role_name(actor.role), actor.principal_id,
         reservation.config_snapshot_sha256, reservation.model_ref, canonical_json(to_json(frozen)), json_text(reservation.request_envelope, 4 * 1024 * 1024), fingerprint,
         reservation.reserved_input_tokens, reservation.reserved_output_tokens, reservation.reserved_cost_micro_usd, reservation.max_output_tokens, stamp()});
    if (experience) impl_->execute("INSERT INTO experience_attempts VALUES (?,?,?,?)",
        {reservation.attempt_id, reservation.experience_job_id, reservation.experience_job_version, reservation.experience_source_sha256});
    const auto result = exposed(impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {reservation.attempt_id}));
    impl_->event(actor.request_id, "reserve:" + reservation.attempt_id, command, result, role_name(actor.role));
    tx.commit(); return result;
}

Json Store::settle_attempt(const std::string& attempt_id, const ExecutionBinding& frozen,
        const std::string& config_hash, const std::string& outcome, const Json& usage,
        const Json& response) {
    const std::lock_guard lock(impl_->mutex);
    require(outcome == "COMPLETED" || outcome == "ERROR" || outcome == "INTERRUPTED", ErrorCode::InvalidInput, "unknown adapter outcome");
    require(usage.is_object(), ErrorCode::InvalidInput, "usage must be an object");
    const std::set<std::string> allowed_usage{"input_tokens", "output_tokens", "cache_read_tokens", "cache_write_tokens", "cost_micro_usd"};
    for (const auto& item : usage.as_object()) {
        require(allowed_usage.contains(std::string(item.key())) && (item.value().is_null() ||
            (item.value().is_int64() && item.value().as_int64() >= 0) ||
            (item.value().is_uint64() && item.value().as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))),
            ErrorCode::InvalidInput, "usage contains an invalid field or negative amount");
    }
    auto usage_value = [&](std::string_view key) -> Json {
        const auto* value = usage.as_object().if_contains(key);
        if (!value || value->is_null()) return nullptr;
        return value->is_uint64() ? Json(static_cast<std::int64_t>(value->as_uint64())) : *value;
    };
    const Row command{{"type", "settle_attempt"}, {"attempt_id", attempt_id}, {"binding", to_json(frozen)},
        {"config_snapshot_sha256", config_hash}, {"outcome", outcome}, {"usage", usage}, {"response", response}};
    const auto fingerprint = sha256_hex(json_text(command, 4 * 1024 * 1024));
    Impl::Transaction tx(*impl_);
    const auto record = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {attempt_id});
    require(execution_binding_from_json(parse_json_strict(str(record, "frozen_binding"))) == frozen && str(record, "config_snapshot_sha256") == config_hash,
        ErrorCode::Conflict, "response changed the original frozen model context");
    const auto request_id = str(record, "request_id");
    if (!record.at("settlement_sha256").is_null()) {
        require(str(record, "settlement_sha256") == fingerprint, ErrorCode::Conflict, "attempt was already settled with different original response or usage");
        const auto result = exposed(record); tx.commit(); return result;
    }
    require(str(record, "status") == "PENDING" || str(record, "status") == "UNKNOWN", ErrorCode::NotReady, "attempt cannot settle from this state");
    const auto current_request = impl_->request_row(request_id);
    bool experience_current = false;
    if (impl_->acceptance_schema) if (const auto job_attempt = impl_->optional("SELECT * FROM experience_attempts WHERE attempt_id=?", {attempt_id})) {
        const auto job = impl_->experience_info(str(*job_attempt, "experience_id"));
        experience_current = str(job.as_object(), "status") != "DONE" && number(job.as_object(), "state_version") == number(*job_attempt, "job_version") &&
            str(job.as_object(), "source_sha256") == str(*job_attempt, "source_sha256") && str(record, "role") == "team_lead" &&
            job.at("request_id") == record.at("request_id") && job.at("task_id") == record.at("task_id") && job.at("team_id") == record.at("team_id");
    }
    bool current = experience_current || (!terminal_request(str(current_request, "status")) && frozen.plan_revision == number(current_request, "plan_revision") &&
        frozen.requirements_revision == number(current_request, "requirements_revision"));
    if (impl_->acceptance_schema && impl_->optional("SELECT attempt_id FROM experience_attempts WHERE attempt_id=?", {attempt_id}) && !experience_current) current = false;
    try { impl_->matches(frozen); } catch (const Error& error) {
        if (error.code() != ErrorCode::Stale && error.code() != ErrorCode::InvalidInput && error.code() != ErrorCode::NotReady) throw;
        current = false;
    }
    const bool accepted = current && outcome == "COMPLETED";
    auto budget_row = impl_->one("SELECT * FROM request_budgets WHERE request_id=?", {request_id});
    for (const auto& dimension : {std::pair<std::string, std::string>{"input", "input_tokens"}, {"output", "output_tokens"}, {"cost", "cost_micro_usd"}}) {
        const auto value = usage_value(dimension.second);
        const auto reserved = number(record, "reserved_" + dimension.first);
        const auto was_unknown = number(record, "unknown_" + dimension.first) == 1;
        if (!value.is_null()) {
            budget_row["reserved_" + dimension.first] = number(budget_row, "reserved_" + dimension.first) - reserved;
            budget_row["spent_" + dimension.first] = add(number(budget_row, "spent_" + dimension.first), value.as_int64());
            if (was_unknown) budget_row["unknown_" + dimension.first] = number(budget_row, "unknown_" + dimension.first) - reserved;
        } else if (!was_unknown) {
            budget_row["unknown_" + dimension.first] = add(number(budget_row, "unknown_" + dimension.first), reserved);
        }
    }
    impl_->cas("UPDATE request_budgets SET reserved_input=?,reserved_output=?,reserved_cost=?,spent_input=?,spent_output=?,spent_cost=?,unknown_input=?,unknown_output=?,unknown_cost=?,state_version=state_version+1 WHERE request_id=? AND state_version=?",
        {budget_row.at("reserved_input"), budget_row.at("reserved_output"), budget_row.at("reserved_cost"),
         budget_row.at("spent_input"), budget_row.at("spent_output"), budget_row.at("spent_cost"),
         budget_row.at("unknown_input"), budget_row.at("unknown_output"), budget_row.at("unknown_cost"), request_id, number(budget_row, "state_version")});
    impl_->cas("UPDATE call_attempts SET status=?,accepted=?,input_tokens=?,output_tokens=?,cache_read_tokens=?,cache_write_tokens=?,cost_micro_usd=?,unknown_input=?,unknown_output=?,unknown_cost=?,response=?,usage=?,settlement_sha256=?,state_version=state_version+1,settled_at=? WHERE attempt_id=? AND state_version=?",
        {current ? outcome : "STALE", accepted, usage_value("input_tokens"), usage_value("output_tokens"), usage_value("cache_read_tokens"), usage_value("cache_write_tokens"),
         usage_value("cost_micro_usd"), usage_value("input_tokens").is_null(), usage_value("output_tokens").is_null(), usage_value("cost_micro_usd").is_null(),
         json_text(response, 4 * 1024 * 1024), json_text(usage), fingerprint, stamp(), attempt_id, number(record, "state_version")});
    const auto result = exposed(impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {attempt_id}));
    impl_->event(request_id, "settle:" + attempt_id, command, result);
    tx.commit(); return result;
}

#include "store_cleanup.inc"

} // namespace wls::s1
