// Private-process boundary coverage, not 4096 real business executions.
// Each selected case gets a fresh process and exits normally for LSan.
#include "wls/worker_tools.hpp"
#include "wls/offline_runtime.hpp"
#include "test_support.hpp"
#include "r24_settlement_allocation_support.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

// Follow worker_tools_tests.cpp: compile the actual implementation in this TU.
// The static linker does not pull a second worker_tools.o. Private state is
// visible only here; no production switch, mock Store or runtime hook is added.
#include "../src/worker_tools.cpp"

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
using namespace wls;
using namespace wls::s1;
using Object = boost::json::object;
using Rows = std::vector<std::string>;
using Snapshot = std::map<std::string, Rows>;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::unique_ptr<SchemaRegistry> schemas;
std::unique_ptr<FrozenRequestConfig> frozen;
std::string base_sql;
std::string migration_sql;
std::string tools_sql;
Json constraints;
std::vector<FakeModelStep> initial_steps;
constexpr std::string_view original_text = "保留原始需求；不要调用外部服务；只在私有目录验证结算故障边界。";

std::string text(const Json& value) { return std::string(value.as_string()); }
std::int64_t number(const Json& value) { return value.to_number<std::int64_t>(); }

template<class Function>
void expect(ErrorCode expected, Function&& function) {
    try { function(); }
    catch (const Error& error) {
        if (error.code() != expected) throw std::runtime_error(std::string("wrong rejection: ") + error.what());
        return;
    }
    throw std::runtime_error("expected rejection was not thrown");
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct Connection {
    sqlite3* db{};
    explicit Connection(const std::filesystem::path& path, bool writable = false) {
        const int rc = sqlite3_open_v2(path.c_str(), &db, writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK) { if (db != nullptr) sqlite3_close(db); db = nullptr; throw std::runtime_error("cannot open fixture database"); }
        CHECK(sqlite3_busy_timeout(db, 1000) == SQLITE_OK);
    }
    ~Connection() { sqlite3_close(db); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    void execute(const std::string& statement) const {
        char* raw = nullptr;
        const int rc = sqlite3_exec(db, statement.c_str(), nullptr, nullptr, &raw);
        const std::string message = raw == nullptr ? "" : raw;
        sqlite3_free(raw);
        if (rc != SQLITE_OK) throw std::runtime_error("fixture SQL: " + message);
    }
};

Rows rows(sqlite3* db, const std::string& query) {
    sqlite3_stmt* raw = nullptr;
    CHECK(sqlite3_prepare_v2(db, query.c_str(), -1, &raw, nullptr) == SQLITE_OK);
    const std::unique_ptr<sqlite3_stmt, int(*)(sqlite3_stmt*)> statement(raw, sqlite3_finalize);
    Rows result;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
        std::string row;
        for (int column = 0; column < sqlite3_column_count(statement.get()); ++column) {
            const int type = sqlite3_column_type(statement.get(), column);
            const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement.get(), column));
            const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement.get(), column));
            row += std::to_string(type) + ":" + std::to_string(size) + ":";
            if (size != 0) { CHECK(bytes != nullptr); row.append(bytes, size); }
            row += "|";
        }
        result.push_back(std::move(row));
    }
    CHECK(rc == SQLITE_DONE);
    std::sort(result.begin(), result.end());
    return result;
}

Snapshot database(const std::filesystem::path& path) {
    Connection connection(path);
    connection.execute("BEGIN");
    sqlite3_stmt* raw = nullptr;
    CHECK(sqlite3_prepare_v2(connection.db, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name", -1, &raw, nullptr) == SQLITE_OK);
    const std::unique_ptr<sqlite3_stmt, int(*)(sqlite3_stmt*)> statement(raw, sqlite3_finalize);
    Snapshot snapshot;
    snapshot.emplace("sqlite_master", rows(connection.db, "SELECT * FROM sqlite_master"));
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto* raw_name = sqlite3_column_text(statement.get(), 0);
        CHECK(raw_name != nullptr);
        const std::string name(reinterpret_cast<const char*>(raw_name));
        std::string quoted = "\"";
        for (const char c : name) { quoted += c; if (c == '"') quoted += c; }
        quoted += '"';
        snapshot.emplace(name, rows(connection.db, "SELECT * FROM " + quoted));
    }
    CHECK(rc == SQLITE_DONE);
    connection.execute("COMMIT");
    return snapshot;
}

void sql(const std::filesystem::path& path, const std::string& statement) {
    Connection(path, true).execute(statement);
}

struct FileIdentity {
    struct stat value {};
    std::string bytes;
    explicit FileIdentity(const std::filesystem::path& path) : bytes(read_bytes(path)) {
        CHECK(::lstat(path.c_str(), &value) == 0);
    }
    void unchanged(const std::filesystem::path& path) const {
        struct stat current {};
        CHECK(::lstat(path.c_str(), &current) == 0);
        CHECK(current.st_dev == value.st_dev && current.st_ino == value.st_ino && current.st_size == value.st_size);
        CHECK(current.st_mtim.tv_sec == value.st_mtim.tv_sec && current.st_mtim.tv_nsec == value.st_mtim.tv_nsec);
        CHECK(current.st_ctim.tv_sec == value.st_ctim.tv_sec && current.st_ctim.tv_nsec == value.st_ctim.tv_nsec);
        CHECK(read_bytes(path) == bytes);
    }
};

struct Session {
    test::TempDir directory;
    std::filesystem::path db{directory.path / "runtime.db"};
    std::filesystem::path work{directory.path / "work"};
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> tools;
    std::string request;
    std::string task;
    Snapshot immutable_initial;
    Json initial_budget;
    std::vector<Json> initial_events;

    Session() {
        std::filesystem::create_directory(work);
        store = open({});
        AdmissionInput input;
        input.tenant_id = "r24-capacity-test"; input.project_id = "private-fixtures"; input.root_id = "fixture-root";
        input.function_id = frozen->function_id(); input.idempotency_key = new_id("key");
        input.original_text = original_text; input.config_snapshot = frozen->json(); input.constraint_catalog = constraints;
        const auto grants = frozen->worker_tools(); input.tool_allowlist.assign(grants.begin(), grants.end());
        request = text(store->admit(input).at("request_id"));
        tools = std::make_unique<WorkerTools>(*store, work);
        const auto result = OfflineRuntime(*store, *schemas, tools.get()).run(request, initial_steps);
        CHECK(result.at("real_model_calls") == 0 && result.at("calls").as_array().size() == 3);
        task = text(store->tasks(request).front().at("task_id"));
        initial_budget = store->budget(request);
        initial_events = store->events(request, 0, 1000);
        const auto snapshot = database(db);
        for (const auto* table : {"request_texts", "request_constraints", "plans", "instructions", "request_runtime", "call_attempts", "llm_calls"})
            immutable_initial.emplace(table, snapshot.at(table));
    }
    std::unique_ptr<Store> open(StoreRecoveryScope scope) const {
        // Use the same supported tools-schema fixture as worker_tools_tests.
        // Full event/cleanup recovery is covered by the separate R2.4 suites;
        // this fixture isolates the real executor's process fault boundary.
        return std::make_unique<Store>(db, base_sql, migration_sql, StoreOpenMode::Takeover, tools_sql,
            "", "", "", "", std::move(scope));
    }
    ExecutionBinding binding() const { return store->binding(task); }
    StoreActor actor(Role role = Role::Worker) const {
        return store->host_actor(role, request, role == Role::Root ? "" : binding().team_id);
    }
    std::int64_t version() const { return number(store->task(task).at("state_version")); }
    Json write(const std::string& name, const std::string& key, const std::string& content) {
        return tools->execute(actor(), binding(), version(), key, "fs_write",
            Object{{"path", name}, {"content", content}}, key + ":command");
    }
    void reopen() {
        tools.reset(); store.reset(); store = open(StoreRecoveryScope::request(request));
        tools = std::make_unique<WorkerTools>(*store, work); tools->bind(actor());
    }
    void originals_unchanged() const {
        const auto snapshot = database(db);
        for (const auto& [table, original] : immutable_initial) CHECK(snapshot.at(table) == original);
        CHECK(store->budget(request) == initial_budget);
        const auto events = store->events(request, 0, 1000);
        CHECK(events.size() < 1000 && events.size() >= initial_events.size());
        CHECK(std::equal(initial_events.begin(), initial_events.end(), events.begin()));
    }
};

Json immutable_result(Store& store, const std::string& id) {
    auto result = store.operation_result(id);
    CHECK(result.is_object());
    result.as_object().erase("operation_status");
    return result;
}

struct Fault {
    StoreActor actor;
    ExecutionBinding binding;
    std::int64_t task_version;
    std::string key;
    Json args;
    std::string id;
    Json operation;
    Json result;
    FileIdentity file;
};

Fault fail_result(Session& session, const std::string& key) {
    const auto actor = session.actor();
    const auto binding = session.binding();
    const auto version = session.version();
    const auto filename = key + ".txt";
    const Json args = Object{{"path", filename}, {"content", "actual file effect before result transaction rollback"}};
    const auto before = database(session.db);
    sql(session.db,
        "CREATE TRIGGER r24_settlement_definite_failure BEFORE INSERT ON events "
        "WHEN NEW.type='record_op_result' AND json_extract(NEW.result,'$.observed_outcome')<>'UNKNOWN' "
        "BEGIN SELECT RAISE(ABORT,'r24-settlement-definite-failure'); END");
    {
        Connection check(session.db);
        CHECK(rows(check.db, "SELECT name FROM sqlite_master WHERE type='trigger' AND name='r24_settlement_definite_failure'").size() == 1);
    }
    expect(ErrorCode::Storage, [&] {
        (void)session.tools->execute(actor, binding, version, key, "fs_write", args, key + ":command");
    });
    sql(session.db, "DROP TRIGGER r24_settlement_definite_failure");
    const auto operation = session.store->operation_for_key(actor, binding, key, "fs_write", args);
    const auto id = text(operation.at("op_id"));
    CHECK(operation.at("status") == "UNKNOWN" && !operation.at("started_at").is_null());
    const auto result = immutable_result(*session.store, id);
    CHECK(result.at("observed_outcome") == "UNKNOWN" && result.at("accepted") == 0);
    CHECK(result.at("result").at("error") == "execution_result_persistence_failed");
    CHECK(result.at("result").at("observed_outcome") == "SUCCEEDED");
    CHECK(result.at("evidence").at("kind") == "settlement_failure");
    CHECK(text(result.at("evidence").at("original_result_sha256")).size() == 64);
    CHECK(result.at("stdout_bytes") == 0 && result.at("stderr_bytes") == 0);
    FileIdentity file(session.work / filename);
    CHECK(file.bytes == text(args.at("content")));
    const auto after = database(session.db);
    CHECK(after.at("ops").size() == before.at("ops").size() + 1);
    CHECK(after.at("operation_results").size() == before.at("operation_results").size() + 1);
    CHECK(after.at("events").size() == before.at("events").size() + 3);
    CHECK(after.at("call_attempts") == before.at("call_attempts"));
    CHECK(session.binding().dispatcher_epoch == binding.dispatcher_epoch);
    CHECK(session.binding().execution_epoch == binding.execution_epoch);
    session.originals_unchanged();
    return {actor, binding, version, key, args, id, operation, result, file};
}

template<class Function>
void rejected_unchanged(Session& session, ErrorCode error, Function&& function) {
    const auto before = database(session.db);
    expect(error, std::forward<Function>(function));
    CHECK(database(session.db) == before);
    session.originals_unchanged();
}

void blocked(Session& session, WorkerTools& tools, const StoreActor& actor,
        const std::string& label, ErrorCode error = ErrorCode::NotReady) {
    const Json args = Object{{"path", label + ".txt"}, {"content", "must not execute"}};
    rejected_unchanged(session, error, [&] {
        (void)tools.execute(actor, session.binding(), session.version(), label, "fs_write", args, label + ":command");
    });
    CHECK(!std::filesystem::exists(session.work / (label + ".txt")));
    CHECK(session.store->operation_for_key(session.actor(), session.binding(), label, "fs_write", args).is_null());
}

void exact_replay(Session& session, const Fault& fault) {
    const auto before = database(session.db);
    const auto replay = session.tools->execute(session.actor(), session.binding(), session.version(), fault.key,
        "fs_write", fault.args, "read-original-fault");
    CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
    CHECK(replay.at("operation") == session.store->operation(fault.id));
    CHECK(replay.at("execution_result") == session.store->operation_result(fault.id));
    CHECK(immutable_result(*session.store, fault.id) == fault.result);
    CHECK(database(session.db) == before);
    fault.file.unchanged(session.work / text(fault.args.at("path")));
}

void global_fence_survives_reconstruction(Session& failed, const Fault& fault, Session& healthy,
        const Json& healthy_result) {
    auto& active = wls::s1::active_runs();
    CHECK(active.settlement_capacity_exhausted);
    const auto memory_records = active.failed_settlements;
    const auto current_actor = failed.actor();
    CHECK(current_actor.writer_session == fault.actor.writer_session);
    blocked(failed, *failed.tools, current_actor, "fresh-actor-blocked");
    failed.tools.reset(); failed.tools = std::make_unique<WorkerTools>(*failed.store, failed.work);
    failed.tools->bind(failed.actor());
    blocked(failed, *failed.tools, failed.actor(), "new-tools-blocked");
    blocked(healthy, *healthy.tools, healthy.actor(), "other-database-blocked");

    // The global fault cannot turn original business-key reads into execution,
    // nor may it waive their existing role, intent or current Store authority.
    exact_replay(failed, fault);
    rejected_unchanged(failed, ErrorCode::Unauthorized, [&] {
        (void)failed.tools->execute(failed.actor(Role::TeamLead), failed.binding(), failed.version(), fault.key,
            "fs_write", fault.args, "leader-cannot-replay");
    });
    auto different = fault.args; different.as_object()["content"] = "altered original intent";
    rejected_unchanged(failed, ErrorCode::Conflict, [&] {
        (void)failed.tools->execute(failed.actor(), failed.binding(), failed.version(), fault.key,
            "fs_write", different, "changed-intent-cannot-replay");
    });
    {
        const auto before = database(healthy.db);
        const auto replay = healthy.write("healthy-before-full.txt", "healthy-before-full", "healthy original effect");
        CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
        CHECK(replay.at("execution_result") == healthy_result.at("execution_result"));
        CHECK(database(healthy.db) == before);
    }
    {
        const auto before = database(failed.db);
        CHECK(failed.tools->diagnostic(failed.actor(), failed.binding(), "fs_read",
            Object{{"path", fault.args.at("path")}}).at("content") == fault.args.at("content"));
        CHECK(database(failed.db) == before);
    }

    // The wrapper's process fault and the Store's durable UNKNOWN are separate
    // boundaries. Exercise UNKNOWN directly rather than letting NotReady mask it.
    rejected_unchanged(failed, ErrorCode::UnknownOutcome, [&] {
        (void)failed.store->prepare_tool_operation(failed.actor(), failed.binding(), failed.version(),
            "store-unknown-blocked", "fs_write", Object{{"path", "store-unknown-blocked.txt"}, {"content", "x"}},
            "store-unknown-blocked:prepare");
    });
    const std::string evidence = "Host observed the original file bytes; sha256=" + sha256_hex(fault.file.bytes);
    rejected_unchanged(failed, ErrorCode::Unauthorized, [&] {
        (void)failed.store->reconcile_operation(failed.actor(), fault.id,
            number(fault.operation.at("state_version")), "SUCCEEDED", evidence, "worker-cannot-reconcile");
    });
    const auto reconciled = failed.store->reconcile_operation(failed.actor(Role::Root), fault.id,
        number(fault.operation.at("state_version")), "SUCCEEDED", evidence, "root-observed-original-file");
    CHECK(reconciled.at("status") == "SUCCEEDED");
    CHECK(immutable_result(*failed.store, fault.id) == fault.result);
    CHECK(active.settlement_capacity_exhausted && active.failed_settlements == memory_records);
    blocked(failed, *failed.tools, failed.actor(), "after-root-reconcile-blocked");

    const auto before_reopen = database(failed.db);
    const auto original_binding = failed.binding();
    const auto old_actor = failed.actor();
    failed.reopen();
    const auto new_actor = failed.actor();
    CHECK(new_actor.writer_session != old_actor.writer_session);
    CHECK(new_actor.execution_epoch == old_actor.execution_epoch && new_actor.dispatcher_epoch == old_actor.dispatcher_epoch);
    CHECK(to_json(failed.binding()) == to_json(original_binding));
    CHECK(database(failed.db) == before_reopen);
    CHECK(active.settlement_capacity_exhausted && active.failed_settlements == memory_records);
    blocked(failed, *failed.tools, new_actor, "new-store-blocked");
    blocked(failed, *failed.tools, old_actor, "old-actor-new-store", ErrorCode::Stale);
    rejected_unchanged(failed, ErrorCode::Stale, [&] {
        (void)failed.tools->execute(old_actor, fault.binding, fault.task_version, fault.key,
            "fs_write", fault.args, fault.key + ":command");
    });
    rejected_unchanged(failed, ErrorCode::Stale, [&] {
        (void)failed.store->prepare_tool_operation(old_actor, fault.binding, fault.task_version,
            fault.key, "fs_write", fault.args, fault.key + ":command:prepare");
    });
    exact_replay(failed, fault);
    {
        Store reader(failed.db, base_sql, migration_sql, StoreOpenMode::ReadOnly, tools_sql);
        WorkerTools tools(reader, failed.work, Object{}, true);
        const auto binding = reader.binding(failed.task);
        const auto actor = reader.host_actor(Role::Worker, failed.request, binding.team_id);
        tools.bind(actor);
        rejected_unchanged(failed, ErrorCode::Unauthorized, [&] {
            (void)tools.execute(actor, binding, failed.version(), fault.key, "fs_write", fault.args, "readonly-business-replay");
        });
        CHECK(tools.diagnostic(actor, binding, "fs_read", Object{{"path", fault.args.at("path")}}).at("content") == fault.args.at("content"));
    }
    CHECK(database(failed.db) == before_reopen);
    CHECK(active.settlement_capacity_exhausted && active.failed_settlements == memory_records);
    failed.originals_unchanged(); healthy.originals_unchanged();
    fault.file.unchanged(failed.work / text(fault.args.at("path")));
    CHECK(active.operations.empty());
}

void capacity_boundary() {
    auto& active = wls::s1::active_runs();
    CHECK(active.operations.empty() && active.failed_settlements.empty() && !active.settlement_capacity_exhausted);
    Session first, next, healthy;
    const auto healthy_result = healthy.write("healthy-before-full.txt", "healthy-before-full", "healthy original effect");
    CHECK(healthy_result.at("executed_now") == true && healthy_result.at("operation").at("status") == "SUCCEEDED");
    const FileIdentity healthy_file(healthy.work / "healthy-before-full.txt");
    {
        const std::lock_guard lock(active.mutex);
        // Synthetic historical memory occupancy only: these are not Store
        // actors and no database rows, file effects or 4095 executions are made.
        for (int i = 0; i < 4095; ++i)
            CHECK(active.failed_settlements.emplace("synthetic-prior-session-" + std::to_string(i),
                "synthetic-prior-team-" + std::to_string(i), 1).second);
    }
    CHECK(active.failed_settlements.size() == 4095 && !active.settlement_capacity_exhausted);
    const auto first_fault = fail_result(first, "real-registration-4096");
    CHECK(active.failed_settlements.size() == 4096 && !active.settlement_capacity_exhausted);
    CHECK(active.failed_settlements.contains({first_fault.actor.writer_session, first_fault.actor.team_id, first_fault.actor.execution_epoch}));
    blocked(first, *first.tools, first.actor(), "local-4096-fence");
    const auto allowed = healthy.write("allowed-at-4096.txt", "allowed-at-4096", "the global fence is still unset");
    CHECK(allowed.at("executed_now") == true && allowed.at("operation").at("status") == "SUCCEEDED");
    CHECK(read_bytes(healthy.work / "allowed-at-4096.txt") == "the global fence is still unset");
    const auto full_records = active.failed_settlements;
    const auto next_fault = fail_result(next, "real-registration-4097");
    CHECK(active.failed_settlements.size() == 4096 && active.failed_settlements == full_records);
    CHECK(active.settlement_capacity_exhausted);
    CHECK(!active.failed_settlements.contains({next_fault.actor.writer_session, next_fault.actor.team_id, next_fault.actor.execution_epoch}));
    std::cout << "boundary synthetic_prefill=4095 real_failed_settlements=2 size_after_4096=4096 "
                 "global_after_4096=false size_after_4097=4096 global_after_4097=true\n";
    global_fence_survives_reconstruction(next, next_fault, healthy, healthy_result);
    first.originals_unchanged();
    CHECK(immutable_result(*first.store, first_fault.id) == first_fault.result);
    first_fault.file.unchanged(first.work / text(first_fault.args.at("path")));
    healthy_file.unchanged(healthy.work / "healthy-before-full.txt");
}

// The existing private prepare observers occur too early to inject a node
// allocation failure safely. SQLite's normal trace interface gives a precise
// external boundary without adding a hook to production source. The failed
// result transaction has fully rolled back when SQLITE_TRACE_PROFILE arrives.
struct RollbackAllocationObserver {
    bool enabled{};
    bool saw_fault_statement{};
    std::size_t completed_rollbacks{};
    static int trace(unsigned event, void* context, void* statement, void* detail) noexcept {
        auto& self = *static_cast<RollbackAllocationObserver*>(context);
        if (!self.enabled) return 0;
        if (event == SQLITE_TRACE_STMT && detail != nullptr) {
            const std::string_view sql_text(static_cast<const char*>(detail));
            if (sql_text.starts_with("-- ") && sql_text.find("SELECT RAISE(ABORT,'r24-settlement-definite-failure')") != std::string_view::npos)
                self.saw_fault_statement = true;
        }
        if (event == SQLITE_TRACE_PROFILE && self.saw_fault_statement) {
            const char* sql_text = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
            if (sql_text != nullptr && std::string_view(sql_text) == "ROLLBACK") {
                self.enabled = false;
                ++self.completed_rollbacks;
                // No allocation, assertion, callback construction or logging
                // occurs here. record_operation_result only destroys locals
                // after rollback. WorkerTools' catch sets a bool/checks size,
                // then its real std::set::emplace makes the next C++ allocation.
                r24_allocation_test::probe.fail_next = true;
            }
        }
        return 0;
    }
    inline static RollbackAllocationObserver* installing{};
    static int install(sqlite3* db, char**, const sqlite3_api_routines*) noexcept {
        return installing == nullptr ? SQLITE_OK :
            sqlite3_trace_v2(db, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE, trace, installing);
    }
    RollbackAllocationObserver() {
        CHECK(installing == nullptr); installing = this;
        if (sqlite3_auto_extension(reinterpret_cast<void(*)()>(install)) != SQLITE_OK) {
            installing = nullptr; throw std::runtime_error("cannot install allocation-boundary trace");
        }
    }
    ~RollbackAllocationObserver() {
        enabled = false; r24_allocation_test::probe.fail_next = false;
        sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(install)); installing = nullptr;
    }
    RollbackAllocationObserver(const RollbackAllocationObserver&) = delete;
    RollbackAllocationObserver& operator=(const RollbackAllocationObserver&) = delete;
};

std::size_t calibrated_node_size() {
    auto& probe = r24_allocation_test::probe;
    CHECK(!probe.fail_next && probe.failures == 0);
    decltype(wls::s1::active_runs().failed_settlements) sample;
    probe.measuring = true;
    try { sample.emplace("s", "t", 1); }
    catch (...) { probe.measuring = false; throw; }
    probe.measuring = false;
    // The short keys require no string allocation on the supported C++
    // implementations. Fail explicitly if this calibration ever changes.
    CHECK(sample.size() == 1 && probe.allocations == 1 && probe.first_size > 0);
    return probe.first_size;
}

void allocation_failure() {
    auto& active = wls::s1::active_runs();
    CHECK(active.operations.empty() && active.failed_settlements.empty() && !active.settlement_capacity_exhausted);
    const auto node_size = calibrated_node_size();
    RollbackAllocationObserver observer;
    Session failed, healthy;
    const auto healthy_result = healthy.write("healthy-before-full.txt", "healthy-before-full", "healthy original effect");
    CHECK(healthy_result.at("executed_now") == true && healthy_result.at("operation").at("status") == "SUCCEEDED");
    const FileIdentity healthy_file(healthy.work / "healthy-before-full.txt");
    observer.enabled = true;
    const auto fault = fail_result(failed, "real-set-node-allocation-failure");
    CHECK(observer.saw_fault_statement && observer.completed_rollbacks == 1 && !observer.enabled);
    const auto& probe = r24_allocation_test::probe;
    CHECK(probe.failures == 1 && !probe.fail_next && probe.failed_size == node_size);
    CHECK(active.failed_settlements.empty() && active.settlement_capacity_exhausted);
    std::cout << "allocation calibrated_node_bytes=" << node_size << " failed_allocation_bytes=" << probe.failed_size
              << " injected_failures=" << probe.failures << " completed_result_rollbacks=" << observer.completed_rollbacks
              << " size_before=0 size_after=0 global_after=true\n";
    global_fence_survives_reconstruction(failed, fault, healthy, healthy_result);
    CHECK(probe.failures == 1 && active.failed_settlements.empty());
    healthy_file.unchanged(healthy.work / "healthy-before-full.txt");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || (std::string_view(argv[1]) != "capacity-boundary" && std::string_view(argv[1]) != "allocation-failure")) {
        std::cerr << "usage: r24_settlement_capacity_tests capacity-boundary|allocation-failure\n";
        return 2;
    }
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    frozen = std::make_unique<FrozenRequestConfig>(load_config_catalog(source / "config/examples/system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general"));
    base_sql = read_runtime_file(source / "db/schema.sql");
    migration_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    constraints = parse_json_strict(read_runtime_file(source / "tests/fixtures/s1_constraints.json"));
    initial_steps = fake_model_steps_from_json(parse_json_strict(read_runtime_file(source / "tests/fixtures/s1_fake_initial.json")));
    if (std::string_view(argv[1]) == "capacity-boundary")
        return test::run({{"4095 synthetic records plus two real failures exercise 4096/4097 and global fencing", capacity_boundary}});
    return test::run({{"actual settlement-set allocation failure globally fences reconstructed executors", allocation_failure}});
}
