#include "wls/store.hpp"
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
             "original_binding", "evidence", "execution_policy", "captured_ids"}) {
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
// Parse the JSON decimal spelling without a binary floating-point multiply.
// A fractional micro-dollar rounds down as a limit, never grants extra budget.
std::int64_t cost_limit_micro(const Json& value) {
    const auto spelling = canonical_json(value);
    const auto exponent_at = spelling.find_first_of("eE");
    const auto coefficient = spelling.substr(0, exponent_at);
    int exponent = 0;
    if (exponent_at != std::string::npos) {
        auto exponent_text = std::string_view(spelling).substr(exponent_at + 1);
        if (exponent_text.starts_with('+')) exponent_text.remove_prefix(1);
        const auto parsed = std::from_chars(exponent_text.data(), exponent_text.data() + exponent_text.size(), exponent);
        require(parsed.ec == std::errc{} && parsed.ptr == exponent_text.data() + exponent_text.size() && exponent >= -1000 && exponent <= 1000,
            ErrorCode::InvalidInput, "invalid decimal cost limit");
    }
    std::int64_t digits = 0;
    int fractional_digits = 0;
    bool fractional = false;
    for (const auto c : coefficient) {
        if (c == '.' && !fractional) { fractional = true; continue; }
        require(c >= '0' && c <= '9', ErrorCode::InvalidInput, "cost limit must be a nonnegative number");
        require(digits <= (INT64_MAX - (c - '0')) / 10, ErrorCode::InvalidInput, "cost limit is out of range");
        digits = digits * 10 + (c - '0');
        if (fractional) ++fractional_digits;
    }
    const auto scale = exponent - fractional_digits + 6;
    if (digits == 0) return 0;
    if (scale >= 0) {
        require(scale <= 18, ErrorCode::InvalidInput, "cost limit exceeds integer capacity");
        for (int i = 0; i < scale; ++i) { require(digits <= INT64_MAX / 10, ErrorCode::InvalidInput, "cost limit exceeds integer capacity"); digits *= 10; }
    } else {
        for (int i = 0; i > scale && digits > 0; --i) digits /= 10;
    }
    return digits;
}
void sqlite_failure(sqlite3* db, int result, std::string_view operation) {
    if (result == SQLITE_OK || result == SQLITE_ROW || result == SQLITE_DONE) return;
    const auto primary = result & 255;
    const auto code = primary == SQLITE_CONSTRAINT ? ErrorCode::Conflict :
        primary == SQLITE_BUSY || primary == SQLITE_LOCKED ? ErrorCode::Conflict :
        primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB ? ErrorCode::Corrupt : ErrorCode::Storage;
    fail(code, std::string(operation) + ": " + (db ? sqlite3_errmsg(db) : sqlite3_errstr(result)));
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
         std::string_view tools_migration_sql, std::string_view events_migration_sql) : mode(selected_mode) {
        require(events_migration_sql.empty() || !tools_migration_sql.empty(), ErrorCode::InvalidInput,
                "event-runtime migration requires the worker-tools asset");
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
            require(version == 401 || version == 402 || version == 403 || version == 404, ErrorCode::Unsupported, "unknown database version; explicit migration required");
            require(str(one("SELECT schema_version FROM contract_metadata WHERE singleton=1"), "schema_version") == "0.4.1",
                    ErrorCode::Unsupported, "unsupported base contract metadata");
            const auto migration_hash = sha256_hex(migration_sql);
            const auto tools_migration_hash = sha256_hex(tools_migration_sql);
            const auto events_migration_hash = sha256_hex(events_migration_sql);
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
            if (version == 404) {
                require(!events_migration_sql.empty(), ErrorCode::Unsupported, "404 database requires the distributed event-runtime migration asset");
                const auto event_metadata = one("SELECT * FROM event_runtime_metadata WHERE singleton=1");
                require(number(event_metadata, "store_version") == 6 && str(event_metadata, "migration_sha256") == events_migration_hash,
                        ErrorCode::Corrupt, "event-runtime migration asset does not match durable metadata");
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
            events_schema = version == 404;
            require(str(one("PRAGMA quick_check"), "quick_check") == "ok", ErrorCode::Corrupt, "SQLite quick_check failed");
            require(!optional("PRAGMA foreign_key_check"), ErrorCode::Corrupt, "SQLite foreign key check failed");
            if (mode == StoreOpenMode::Takeover) {
                exec("BEGIN IMMEDIATE");
                try {
                    if (events_schema) {
                        execute("UPDATE event_inbox SET status='PENDING',claim_id=NULL WHERE status='CLAIMED'");
                        execute("UPDATE event_actor_claims SET status='RELEASED',updated_at=? WHERE status='ACTIVE'", {stamp()});
                    }
                    for (const auto& request_row : all("SELECT * FROM requests WHERE status NOT IN ('DELIVERED','FAILED','CANCELLED','SUPERSEDED')"))
                        recover(request_row);
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
        ~Transaction() { if (!committed_) owner_.rollback(); }
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
    void event(const std::string& request_id, const std::string& command_id,
               const Row& command, const Json& result, std::string_view actor = "runtime") {
        text_field(command_id, "command_id");
        const auto type = str(command, "type");
        const bool control = type == "ack" || type == "cancel_op" || type == "finish_op" || type == "reconcile_op" || type == "record_op_result" || type == "execute_tool_control" ||
            type == "wait_worker" || type == "wake_worker" || type == "tool_feedback" || type == "event_bootstrap" ||
            type == "recover" || type == "cancel_request" || type == "settle_attempt" || type == "attempts_unknown";
        const auto count = number(one("SELECT COUNT(*) AS n FROM events WHERE request_id=?", {request_id}), "n");
        require(count < (control ? 8192 : 4096), ErrorCode::Quota, "bounded Store event capacity exhausted");
        execute("INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) VALUES (?,?,?,?,?,?,?,?,?)",
            {new_id("evt"), request_id, type, actor, command_id, sha256_hex(canonical_json(command)), json_text(command), json_text(result, 16 * 1024 * 1024), stamp()});
        if (events_schema) route_event(request_id, type, command, result, sqlite3_last_insert_rowid(db));
    }
    void release_claims(const std::string& request_id) {
        if (!events_schema) return;
        execute("UPDATE event_inbox SET status='PENDING',claim_id=NULL WHERE request_id=? AND status='CLAIMED'", {request_id});
        execute("UPDATE event_actor_claims SET status='RELEASED',updated_at=? WHERE request_id=? AND status='ACTIVE'", {stamp(), request_id});
    }
    void supersede_waits(const std::string& request_id, const std::string& task_id = {}) {
        if (!events_schema) return;
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
                else if (!terminal_task(str(task, "status"))) enqueue("team_lead", str(task, "task_id"));
            }
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
    void recover(const Row& r) {
        const auto id = str(r, "request_id");
        release_claims(id);
        cas("UPDATE requests SET dispatcher_epoch=dispatcher_epoch+1,state_version=state_version+1,updated_at=? WHERE request_id=? AND state_version=?",
            {stamp(), id, number(r, "state_version")});
        execute("UPDATE teams SET execution_epoch=execution_epoch+1,paused_for_change=1,state_version=state_version+1,updated_at=? WHERE request_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {stamp(), id});
        fence_ops(id);
        unknown_attempts(id);
        event(id, new_id("recover"), Row{{"type", "recover"}}, Row{{"dispatcher_epoch", next(number(r, "dispatcher_epoch"))}});
    }
    void catalog(const std::string& request_id, std::int64_t revision, const Json& value) {
        require(value.is_array() && value.as_array().size() <= 200, ErrorCode::InvalidInput, "constraint catalog must be a bounded array");
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
    std::string database_file_path;
    mutable std::mutex mutex;
};

Store::Store(const std::filesystem::path& path, std::string_view base, std::string_view migration, StoreOpenMode mode,
             std::string_view tools_migration, std::string_view events_migration)
    : impl_(std::make_unique<Impl>(path, base, migration, mode, tools_migration, events_migration)) {}
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
    return exposed(impl_->one("SELECT * FROM instructions WHERE task_id=? AND instruction_revision=?", {id, number(t, "instruction_revision")}));
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
    return Row{{"unknown_operations", collect("SELECT op_id,request_id,task_id,team_id,status FROM ops WHERE status='UNKNOWN'")},
        {"unknown_attempts", collect("SELECT attempt_id,request_id,status FROM call_attempts WHERE status='UNKNOWN' OR unknown_input=1 OR unknown_output=1 OR unknown_cost=1")},
        {"experience_jobs", collect("SELECT * FROM experience_jobs WHERE status<>'DONE'")},
        {"cleanup_jobs", collect("SELECT * FROM cleanup_jobs WHERE status<>'DONE'")},
        {"unconfirmed_delivery", collect("SELECT request_id,delivery_status FROM requests WHERE delivery_status='STORED'")}};
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
    const auto task = impl_->task_row(actor.task_id);
    require(!terminal_request(str(request, "status")) && !terminal_task(str(task, "status")), ErrorCode::NotReady, "terminal work cannot enter Worker wait");
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

Json Store::admit(const AdmissionInput& input) {
    const std::lock_guard lock(impl_->mutex);
    for (const auto* field : {&input.tenant_id, &input.project_id, &input.root_id, &input.function_id, &input.idempotency_key}) text_field(*field, "admission scope");
    text_field(input.original_text, "original text", 65536);
    require(input.config_snapshot.is_object(), ErrorCode::InvalidInput, "complete configuration snapshot required");
    require(input.budget.max_attempts > 0 && input.budget.max_attempts <= 10000 && input.budget.input_tokens >= 0 &&
            input.budget.output_tokens >= 0 && input.budget.cost_micro_usd >= 0, ErrorCode::InvalidInput, "invalid frozen budget");
    const auto config = json_text(input.config_snapshot, 8 * 1024 * 1024);
    const auto frozen_config = FrozenRequestConfig::restore(config, sha256_hex(config));
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
    require(revision <= 100, ErrorCode::Quota, "request text history capacity exhausted");
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
    require(!terminal_request(str(r, "status")) && !terminal_task(str(t, "status")) && requirements == number(r, "requirements_revision") &&
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
    impl_->brief_constraints(r, body);
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
    const auto instruction = impl_->one("SELECT * FROM instructions WHERE task_id=? AND instruction_revision=?", {actor.task_id, number(t, "instruction_revision")});
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
    require(number(one("SELECT COUNT(*) AS n FROM ops WHERE request_id=?", {actor.request_id}), "n") < 1024, ErrorCode::Quota, "operation capacity exhausted");
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
        const auto instruction = impl_->one("SELECT * FROM instructions WHERE task_id=? AND instruction_revision=?", {actor.task_id, number(task, "instruction_revision")});
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
    const auto command = payload(actor, "reserve_attempt", Row{{"binding", to_json(frozen)}, {"call_id", reservation.call_id},
        {"attempt_id", reservation.attempt_id}, {"config_snapshot_sha256", reservation.config_snapshot_sha256},
        {"model_ref", reservation.model_ref}, {"adapter_kind", reservation.adapter_kind},
        {"reserved_input_tokens", reservation.reserved_input_tokens}, {"reserved_output_tokens", reservation.reserved_output_tokens},
        {"reserved_cost_micro_usd", reservation.reserved_cost_micro_usd}, {"max_output_tokens", reservation.max_output_tokens}, {"request_envelope", reservation.request_envelope}});
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
    require(!terminal_request(str(r, "status")), ErrorCode::NotReady, "business model attempt after request termination");
    impl_->matches(frozen);
    require(str(r, "config_snapshot_sha256") == reservation.config_snapshot_sha256, ErrorCode::Stale, "model attempt changed the frozen request configuration");
    const auto frozen_config = FrozenRequestConfig::restore(str(r, "config_snapshot"), str(r, "config_snapshot_sha256"));
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
        for (const auto& tool : original_model_request.context.tool_names) impl_->ready(actor, frozen, tool);
    }
    if (actor.role == Role::TeamLead) {
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
    bool current = !terminal_request(str(current_request, "status")) && frozen.plan_revision == number(current_request, "plan_revision") &&
        frozen.requirements_revision == number(current_request, "requirements_revision");
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

} // namespace wls::s1
