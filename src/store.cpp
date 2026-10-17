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
             "response", "request_envelope", "arguments", "payload", "result", "source_snapshot", "manifest_snapshot"}) {
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
         std::string_view migration_sql, StoreOpenMode selected_mode) : mode(selected_mode) {
        require(database_path != ":memory:", ErrorCode::InvalidInput, "Store requires a durable file path");
        const auto absolute = std::filesystem::absolute(database_path).lexically_normal();
        const auto path = (std::filesystem::canonical(absolute.parent_path()) / absolute.filename()).string();
        text_field(path, "database path", 4096);
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
            require(version == 401 || version == 402, ErrorCode::Unsupported, "unknown database version; explicit migration required");
            require(str(one("SELECT schema_version FROM contract_metadata WHERE singleton=1"), "schema_version") == "0.4.1",
                    ErrorCode::Unsupported, "unsupported base contract metadata");
            const auto migration_hash = sha256_hex(migration_sql);
            if (version == 401) {
                require(mode == StoreOpenMode::Takeover, ErrorCode::Unsupported, "database needs Store migration before read-only inspection");
                // Python reference rows did not capture a complete configuration.
                // Their original data must not acquire invented frozen settings.
                require(!optional("SELECT request_id FROM requests LIMIT 1"), ErrorCode::Unsupported,
                        "v0.4.1 reference database contains requests without full snapshots; explicit data migration required");
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
                    exec("COMMIT");
                } catch (...) { rollback(); throw; }
            }
            const auto metadata = one("SELECT * FROM store_runtime_metadata WHERE singleton=1");
            require(number(metadata, "store_version") == 4 && str(metadata, "migration_sha256") == migration_hash,
                    ErrorCode::Corrupt, "Store migration asset does not match durable metadata");
            require(str(one("PRAGMA quick_check"), "quick_check") == "ok", ErrorCode::Corrupt, "SQLite quick_check failed");
            require(!optional("PRAGMA foreign_key_check"), ErrorCode::Corrupt, "SQLite foreign key check failed");
            if (mode == StoreOpenMode::Takeover) {
                exec("BEGIN IMMEDIATE");
                try {
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
        if (!tool_id.empty()) {
            const auto frozen_config = FrozenRequestConfig::restore(str(r, "config_snapshot"), str(r, "config_snapshot_sha256"));
            const auto template_tools = frozen_config.worker_tools(str(team, "template_id"));
            const auto denied = parse_json_strict(str(one("SELECT denied_tools FROM runtime_policy WHERE singleton=1"), "denied_tools"));
            require(template_tools.contains(std::string(tool_id)) && contains(parse_json_strict(str(r, "tool_allowlist")), tool_id) && !contains(denied, tool_id),
                    ErrorCode::Unauthorized, "tool grant absent or revoked");
        }
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
        const bool control = type == "ack" || type == "cancel_op" || type == "finish_op" || type == "reconcile_op" ||
            type == "recover" || type == "cancel_request" || type == "settle_attempt" || type == "attempts_unknown";
        const auto count = number(one("SELECT COUNT(*) AS n FROM events WHERE request_id=?", {request_id}), "n");
        require(count < (control ? 8192 : 4096), ErrorCode::Quota, "bounded Store event capacity exhausted");
        execute("INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) VALUES (?,?,?,?,?,?,?,?,?)",
            {new_id("evt"), request_id, type, actor, command_id, sha256_hex(canonical_json(command)), json_text(command), json_text(result, 16 * 1024 * 1024), stamp()});
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

    sqlite3* db{};
    int lease_fd{-1};
    StoreOpenMode mode;
    mutable std::mutex mutex;
};

Store::Store(const std::filesystem::path& path, std::string_view base, std::string_view migration, StoreOpenMode mode)
    : impl_(std::make_unique<Impl>(path, base, migration, mode)) {}
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

Json Store::prepare_operation(const StoreActor& actor, const ExecutionBinding& frozen,
        std::int64_t expected, const std::string& business_key, const std::string& tool_id,
        const Json& arguments, const std::string& side_effect, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    text_field(business_key, "business idempotency key"); text_field(tool_id, "tool id");
    require(side_effect == "none" || side_effect == "workspace" || side_effect == "external", ErrorCode::InvalidInput, "invalid side-effect class");
    require(arguments.is_object(), ErrorCode::InvalidInput, "tool arguments must be an object");
    const auto encoded_arguments = json_text(arguments);
    const auto args_hash = sha256_hex(encoded_arguments);
    const auto command = payload(actor, "prepare_op", Row{{"binding", to_json(frozen)}, {"expected_task_version", expected},
        {"business_key", business_key}, {"tool_id", tool_id}, {"arguments_sha256", args_hash}, {"side_effect", side_effect}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Worker}, false); impl_->actor_binding(actor, frozen);
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto existing = impl_->optional("SELECT * FROM ops WHERE request_id=? AND task_id=? AND business_idempotency_key=?", {actor.request_id, actor.task_id, business_key});
    if (existing) {
        require(str(*existing, "team_id") == actor.team_id && str(*existing, "tool_id") == tool_id && str(*existing, "args_sha256") == args_hash &&
            str(*existing, "side_effect") == side_effect, ErrorCode::Conflict, "business key reused with different intent");
        const auto frozen_document = to_json(frozen);
        for (const auto& field : frozen_document.as_object()) require(existing->at(field.key()) == field.value(), ErrorCode::Conflict, "business key reused with different execution binding");
        const auto result = exposed(impl_->op_row(str(*existing, "op_id"))); tx.commit(); return result;
    }
    impl_->ready(actor, frozen, tool_id);
    const auto r = impl_->request_row(actor.request_id);
    const auto team = impl_->team_row(actor.team_id);
    require(number(team, "tool_calls_used") < impl_->configured_limit(r, "worker_max_tool_calls_per_order"), ErrorCode::Quota, "instruction tool-call budget exhausted");
    require(number(impl_->one("SELECT COUNT(*) AS n FROM ops WHERE request_id=?", {actor.request_id}), "n") < 1024, ErrorCode::Quota, "operation capacity exhausted");
    impl_->cas("UPDATE tasks SET status='RUNNING',state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {stamp(), actor.task_id, expected});
    const auto id = new_id("op");
    impl_->execute("INSERT INTO ops(op_id,request_id,task_id,team_id,tool_id,business_idempotency_key,args_sha256,requirements_revision,plan_revision,task_revision,instruction_revision,dispatcher_epoch,execution_epoch,policy_epoch,side_effect,status,created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'PREPARED',?)",
        {id, frozen.request_id, frozen.task_id, frozen.team_id, tool_id, business_key, args_hash, frozen.requirements_revision, frozen.plan_revision,
         frozen.task_revision, frozen.instruction_revision, frozen.dispatcher_epoch, frozen.execution_epoch, frozen.policy_epoch, side_effect, stamp()});
    impl_->execute("INSERT INTO operation_intents(op_id,arguments) VALUES (?,?)", {id, encoded_arguments});
    impl_->execute("UPDATE teams SET tool_calls_used=tool_calls_used+1,state_version=state_version+1,updated_at=? WHERE team_id=?", {stamp(), actor.team_id});
    const auto result = exposed(impl_->op_row(id));
    impl_->event(actor.request_id, command_id, command, result, "worker");
    tx.commit(); return result;
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
