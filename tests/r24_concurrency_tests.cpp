// Deterministic R2.4 boundaries, using real disposable Store/WorkerTools/files.
// One logical suite; --group only bounds the existing 90-second CI envelope.
#include "wls/worker_tools.hpp"
#include "wls/cleanup.hpp"
#include "wls/cleanup_runtime.hpp"
#include "wls/delivery.hpp"
#include "wls/event_runtime.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sqlite3.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <utility>

namespace {
void concurrency_before_prepare(std::string_view key);
void concurrency_prepared(std::string_view key, const wls::s1::Json& prepared);
}
// Reuse the actual implementation's private TU-only observation points. No
// production observer, runtime switch, or replacement executor is introduced.
#define WLS_WORKER_TOOLS_PRIVATE_TEST_BEFORE_PREPARE(key) ::concurrency_before_prepare(key)
#define WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE(key, prepared) ::concurrency_prepared(key, prepared)
#include "../src/worker_tools.cpp"
#undef WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE
#undef WLS_WORKER_TOOLS_PRIVATE_TEST_BEFORE_PREPARE

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
using namespace wls::s1;
using wls::Role;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::string base_sql, store_sql, tools_sql, events_sql, workflow_sql, acceptance_sql, cleanup_sql;
std::unique_ptr<SchemaRegistry> schemas;
std::unique_ptr<FrozenRequestConfig> config;

std::function<void(std::string_view)> before_prepare;
std::function<void(std::string_view, const Json&)> after_prepare;
void concurrency_before_prepare(std::string_view key) { if (before_prepare) before_prepare(key); }
void concurrency_prepared(std::string_view key, const Json& prepared) { if (after_prepare) after_prepare(key, prepared); }
struct ResetObservers {
    ~ResetObservers() { before_prepare = {}; after_prepare = {}; }
};

std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t number(const Json& value, std::string_view key) { return value.at(key).to_number<std::int64_t>(); }
void expect(ErrorCode code, const std::function<void()>& action) {
    try { action(); }
    catch (const Error& error) {
        if (error.code() != code) throw std::runtime_error(std::string("wrong rejection: ") + error.what());
        return;
    }
    throw std::runtime_error("expected Store rejection");
}
std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary); CHECK(file.good());
    return {std::istreambuf_iterator<char>(file), {}};
}
void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); CHECK(file.good());
}
using Connection = std::unique_ptr<sqlite3, int(*)(sqlite3*)>;
using Statement = std::unique_ptr<sqlite3_stmt, int(*)(sqlite3_stmt*)>;
Connection connect(const std::filesystem::path& path, bool write = false) {
    sqlite3* raw = nullptr;
    const int status = sqlite3_open_v2(path.c_str(), &raw,
        write ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY, nullptr);
    Connection result(raw, sqlite3_close); CHECK(status == SQLITE_OK);
    CHECK(sqlite3_busy_timeout(result.get(), 1000) == SQLITE_OK); return result;
}
Statement statement(sqlite3* db, const std::string& query, const std::vector<std::string>& args = {}) {
    sqlite3_stmt* raw = nullptr; CHECK(sqlite3_prepare_v2(db, query.c_str(), -1, &raw, nullptr) == SQLITE_OK);
    Statement result(raw, sqlite3_finalize);
    for (std::size_t index = 0; index < args.size(); ++index)
        CHECK(sqlite3_bind_text(result.get(), static_cast<int>(index + 1), args[index].c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
    return result;
}
std::vector<std::string> rows(const std::filesystem::path& path, const std::string& query,
        const std::vector<std::string>& args = {}) {
    auto db = connect(path); auto query_statement = statement(db.get(), query, args);
    std::vector<std::string> result; int status{};
    while ((status = sqlite3_step(query_statement.get())) == SQLITE_ROW) {
        std::string row;
        for (int col = 0; col < sqlite3_column_count(query_statement.get()); ++col) {
            const auto size = sqlite3_column_bytes(query_statement.get(), col);
            row += std::to_string(sqlite3_column_type(query_statement.get(), col)) + ":" + std::to_string(size) + ":";
            if (size) row.append(static_cast<const char*>(sqlite3_column_blob(query_statement.get(), col)), static_cast<std::size_t>(size));
            row += '|';
        }
        result.push_back(sha256_hex(row));
    }
    CHECK(status == SQLITE_DONE); std::sort(result.begin(), result.end()); return result;
}
std::int64_t integer(const std::filesystem::path& path, const std::string& query,
        const std::vector<std::string>& args = {}) {
    auto db = connect(path); auto query_statement = statement(db.get(), query, args);
    CHECK(sqlite3_step(query_statement.get()) == SQLITE_ROW);
    CHECK(sqlite3_column_type(query_statement.get(), 0) == SQLITE_INTEGER);
    const auto result = sqlite3_column_int64(query_statement.get(), 0);
    CHECK(sqlite3_step(query_statement.get()) == SQLITE_DONE); return result;
}
std::string value(const std::filesystem::path& path, const std::string& query,
        const std::vector<std::string>& args = {}) {
    auto db = connect(path); auto query_statement = statement(db.get(), query, args);
    CHECK(sqlite3_step(query_statement.get()) == SQLITE_ROW);
    CHECK(sqlite3_column_type(query_statement.get(), 0) == SQLITE_TEXT);
    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(query_statement.get(), 0));
    const std::string result(bytes, static_cast<std::size_t>(sqlite3_column_bytes(query_statement.get(), 0)));
    CHECK(sqlite3_step(query_statement.get()) == SQLITE_DONE); return result;
}
void fixture_update(const std::filesystem::path& path, const std::string& query,
        const std::vector<std::string>& args) {
    auto db = connect(path, true); auto query_statement = statement(db.get(), query, args);
    CHECK(sqlite3_step(query_statement.get()) == SQLITE_DONE); CHECK(sqlite3_changes(db.get()) == 1);
}
Json snapshot(const std::filesystem::path& path) {
    auto db = connect(path); auto query_statement = statement(db.get(),
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
    Object result; int status{};
    while ((status = sqlite3_step(query_statement.get())) == SQLITE_ROW) {
        const std::string name(reinterpret_cast<const char*>(sqlite3_column_text(query_statement.get(), 0)));
        Array table;
        for (const auto& row : rows(path, "SELECT * FROM \"" + name + "\"")) table.emplace_back(row);
        result[name] = std::move(table);
    }
    CHECK(status == SQLITE_DONE); return result;
}
Json immutable_history(const std::filesystem::path& path) {
    Object result;
    for (const auto* table : {"request_texts", "request_constraints", "plans", "instructions", "request_runtime",
             "call_attempts", "model_tool_feedback", "operation_results", "operation_output_bytes", "receipts", "delivery_runtime", "delivery_receipts"}) {
        Array saved; for (const auto& row : rows(path, std::string("SELECT * FROM ") + table)) saved.emplace_back(row);
        result[table] = std::move(saved);
    }
    return result;
}
std::vector<std::string> intent_bytes(const std::filesystem::path& path) {
    return rows(path, "SELECT o.op_id,o.request_id,o.task_id,o.team_id,o.tool_id,o.business_idempotency_key,o.args_sha256,"
        "o.requirements_revision,o.plan_revision,o.task_revision,o.instruction_revision,o.dispatcher_epoch,o.execution_epoch,o.policy_epoch,"
        "o.side_effect,o.created_at,o.started_at,i.arguments FROM ops AS o JOIN operation_intents AS i ON i.op_id=o.op_id");
}
void history_prefix(const std::vector<Json>& prefix, const std::vector<Json>& events, std::size_t added) {
    CHECK(events.size() == prefix.size() + added); CHECK(std::equal(prefix.begin(), prefix.end(), events.begin()));
}
struct FileIdentity {
    struct stat identity{};
    explicit FileIdentity(const std::filesystem::path& path) { CHECK(::lstat(path.c_str(), &identity) == 0); }
    void unchanged(const std::filesystem::path& path) const {
        struct stat now{}; CHECK(::lstat(path.c_str(), &now) == 0);
        CHECK(now.st_dev == identity.st_dev && now.st_ino == identity.st_ino && now.st_size == identity.st_size);
        CHECK(now.st_mtim.tv_sec == identity.st_mtim.tv_sec && now.st_mtim.tv_nsec == identity.st_mtim.tv_nsec);
        CHECK(now.st_ctim.tv_sec == identity.st_ctim.tv_sec && now.st_ctim.tv_nsec == identity.st_ctim.tv_nsec);
    }
};

// Existing worker-tools tests use the same 10-second failure bound. A timeout
// fails the case; it never becomes a scheduling delay or a successful outcome.
struct Gate {
    std::mutex mutex; std::condition_variable changed;
    bool reached{}, released{}; Json fact;
    void arrive(Json observed = nullptr) {
        std::unique_lock lock(mutex); fact = std::move(observed); reached = true; changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(10), [&] { return released; }))
            throw std::runtime_error("concurrency boundary was not released");
    }
    Json await() {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(10), [&] { return reached; }))
            throw std::runtime_error("concurrency boundary was not reached");
        return fact;
    }
    void release() noexcept { const std::lock_guard lock(mutex); released = true; changed.notify_all(); }
};
struct PausedCall {
    Gate& gate; Json result; std::exception_ptr failure; std::jthread thread;
    PausedCall(Gate& observed, std::function<Json()> action) : gate(observed), thread([this, action = std::move(action)] {
        try { result = action(); } catch (...) { failure = std::current_exception(); }
    }) {}
    ~PausedCall() { gate.release(); if (thread.joinable()) thread.join(); }
    void finish() { gate.release(); thread.join(); }
    void rethrow() const { if (failure) std::rethrow_exception(failure); }
};

// Documented SQLite auto-extension/trace hooks attach before the fixture Store
// opens. Only this temporary database's writable connection can fire. Callback
// exceptions are retained and rethrown after the owning call joins; none cross C.
struct SqliteBoundary {
    inline static SqliteBoundary* active{};
    std::filesystem::path target;
    std::function<bool(unsigned, std::string_view)> matches;
    std::function<void()> action;
    std::exception_ptr failure;
    std::atomic_bool fired{false};
    static int trace(unsigned event, void* context, void* raw, void*) noexcept {
        auto& self = *static_cast<SqliteBoundary*>(context);
        if (self.fired.load() || !self.matches) return 0;
        auto* sql_statement = static_cast<sqlite3_stmt*>(raw);
        auto* db = sqlite3_db_handle(sql_statement);
        const auto* path = sqlite3_db_filename(db, "main"); const auto* query = sqlite3_sql(sql_statement);
        if (!path || !query || sqlite3_db_readonly(db, "main") != 0 || self.target != path) return 0;
        try {
            if (self.matches(event, query)) { self.fired.store(true); self.action(); }
        } catch (...) { self.failure = std::current_exception(); }
        return 0;
    }
    static int install(sqlite3* db, char**, const sqlite3_api_routines*) noexcept {
        return active ? sqlite3_trace_v2(db, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE, trace, active) : SQLITE_OK;
    }
    SqliteBoundary() {
        CHECK(active == nullptr); active = this;
        if (sqlite3_auto_extension(reinterpret_cast<void(*)()>(install)) != SQLITE_OK) {
            active = nullptr; throw std::runtime_error("cannot attach SQLite boundary observer");
        }
    }
    ~SqliteBoundary() { sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(install)); active = nullptr; }
    void rethrow() const { if (failure) std::rethrow_exception(failure); CHECK(fired.load()); }
    void after_commit(std::string prefix, std::function<void()> callback) {
        matches = [seen = false, prefix = std::move(prefix)](unsigned event, std::string_view query) mutable {
            if (event != SQLITE_TRACE_PROFILE) return false;
            if (query.starts_with(prefix)) seen = true;
            return seen && query == "COMMIT";
        };
        action = std::move(callback);
    }
};

struct Fixture {
    test::TempDir dir;
    std::filesystem::path db{dir.path / "state.sqlite"}, work{dir.path / "work"};
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> tools;
    std::string id, task;
    Json brief = Object{{"goal", "retain original recovery evidence"}, {"constraint_refs", Array{"c1"}},
        {"constraints_verbatim", Array{"retain original recovery evidence"}}};
    explicit Fixture(bool create_ready = true) {
        std::filesystem::create_directory(work); store = open({});
        if (!create_ready) return;
        AdmissionInput input; input.tenant_id = "concurrency"; input.project_id = "fixtures"; input.root_id = "root";
        input.function_id = "general"; input.idempotency_key = "request"; input.original_text = "retain original recovery evidence";
        input.config_snapshot = config->json();
        input.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", input.original_text}, {"source_revision", 1}}};
        const auto grants = config->worker_tools(); input.tool_allowlist.assign(grants.begin(), grants.end());
        id = text(store->admit(input), "request_id");
        (void)store->publish_plan(actor(Role::Dispatcher), number(store->request(id), "state_version"), 1, brief, "plan");
        const auto created = store->create_task(actor(Role::Dispatcher), number(store->request(id), "state_version"), 1, 1, "node", brief, "task");
        task = text(created, "task_id");
        (void)store->issue_instruction(actor(Role::TeamLead), binding(), version(), brief, "order");
        acknowledge(); bind_tools();
    }
    std::unique_ptr<Store> open(StoreRecoveryScope scope) const {
        return std::make_unique<Store>(db, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql,
            events_sql, workflow_sql, acceptance_sql, cleanup_sql, std::move(scope));
    }
    ExecutionBinding binding() const { return store->binding(task); }
    std::int64_t version() const { return number(store->task(task), "state_version"); }
    StoreActor actor(Role role = Role::Worker) const {
        return store->host_actor(role, id, role == Role::Worker || role == Role::TeamLead ? binding().team_id : "");
    }
    void acknowledge() { (void)store->acknowledge(actor(), binding(), version(), new_id("ack")); }
    void bind_tools() { tools = std::make_unique<WorkerTools>(*store, work); tools->bind(actor()); }
    void close() { tools.reset(); store.reset(); }
    void reopen() { close(); store = open(StoreRecoveryScope::request(id)); }
    Json execute(std::string key, const Json& args) {
        return tools->execute(actor(), binding(), version(), key, "fs_write", args, key + "-command");
    }
    void competing_open_is_rejected() const {
        const auto before = snapshot(db);
        expect(ErrorCode::Conflict, [&] { (void)open(StoreRecoveryScope::request(id)); });
        CHECK(snapshot(db) == before);
    }
};
void succeeded(const Json& result) {
    CHECK(result.at("executed_now") == true && result.at("replayed") == false);
    CHECK(result.at("operation").at("status") == "SUCCEEDED");
    CHECK(result.at("execution_result").at("observed_outcome") == "SUCCEEDED");
    CHECK(result.at("execution_result").at("accepted") == 1);
}

struct StopAfterPrepare {};
void prepared_reopen_exclusion_then_real_recovery() {
    Fixture f; const auto old_actor = f.actor(); const auto original_binding = f.binding(); const auto expected = f.version();
    const auto budget = f.store->budget(f.id); const auto original = immutable_history(f.db);
    const Json args = Object{{"path", "prepared-effect.txt"}, {"content", "must remain absent"}};
    Gate gate; ResetObservers reset;
    after_prepare = [&](std::string_view key, const Json& prepared) {
        if (key == "prepared-boundary") { gate.arrive(prepared); throw StopAfterPrepare{}; }
    };
    PausedCall call(gate, [&] { return f.tools->execute(old_actor, original_binding, expected, "prepared-boundary", "fs_write", args, "prepared-command"); });
    const auto prepared = gate.await(); const auto id = text(prepared, "op_id");
    CHECK(f.store->operation(id).at("status") == "PREPARED"); CHECK(f.store->operation(id).at("started_at").is_null());
    CHECK(f.store->operation_result(id).is_null()); CHECK(!std::filesystem::exists(f.work / "prepared-effect.txt"));
    f.competing_open_is_rejected(); CHECK(f.store->budget(f.id) == budget);
    call.finish(); CHECK(call.failure != nullptr);
    bool stopped = false; try { call.rethrow(); } catch (const StopAfterPrepare&) { stopped = true; } CHECK(stopped);
    const auto events = f.store->events(f.id, 0, 1000); const auto intents = intent_bytes(f.db);
    f.reopen();
    CHECK(number(f.store->request(f.id), "dispatcher_epoch") == original_binding.dispatcher_epoch + 1);
    CHECK(f.binding().execution_epoch == original_binding.execution_epoch + 1);
    CHECK(f.store->operation(id).at("status") == "CANCELLED"); CHECK(f.store->operation_result(id).is_null());
    CHECK(intent_bytes(f.db) == intents && immutable_history(f.db) == original); CHECK(f.store->budget(f.id) == budget);
    history_prefix(events, f.store->events(f.id, 0, 1000), 1); CHECK(f.store->events(f.id, 0, 1000).back().at("type") == "recover");
    const auto recovered = snapshot(f.db);
    expect(ErrorCode::Stale, [&] { (void)f.store->prepare_tool_operation(old_actor, original_binding, expected,
        "prepared-boundary", "fs_write", args, "prepared-command:prepare"); });
    CHECK(snapshot(f.db) == recovered); CHECK(!std::filesystem::exists(f.work / "prepared-effect.txt"));
    f.bind_tools();
    expect(ErrorCode::NotReady, [&] { (void)f.execute("unacknowledged", Object{{"path", "unacknowledged.txt"}, {"content", "denied"}}); });
    CHECK(!std::filesystem::exists(f.work / "unacknowledged.txt"));
    f.acknowledge(); succeeded(f.execute("current", Object{{"path", "current.txt"}, {"content", "one new authorized effect"}}));
    CHECK(read_bytes(f.work / "current.txt") == "one new authorized effect"); CHECK(f.store->budget(f.id) == budget);
}

struct Pipe {
    int read_fd{-1}, write_fd{-1};
    Pipe() { int descriptors[2]; CHECK(::pipe(descriptors) == 0); read_fd = descriptors[0]; write_fd = descriptors[1]; }
    ~Pipe() { if (read_fd >= 0) ::close(read_fd); if (write_fd >= 0) ::close(write_fd); }
};
struct Child {
    pid_t pid{-1};
    explicit Child(pid_t value) : pid(value) { CHECK(pid > 0); }
    ~Child() { if (pid > 0) { ::kill(pid, SIGKILL); int status{}; while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {} } }
    void kill_and_wait() {
        CHECK(::kill(pid, SIGKILL) == 0); int status{}; pid_t waited;
        do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
        CHECK(waited == pid); pid = -1; CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    }
};
void running_crash_recovers_only_after_original_writer_exits() {
    Fixture f; const auto old_actor = f.actor(); const auto original_binding = f.binding();
    const auto budget = f.store->budget(f.id); const auto original = immutable_history(f.db); f.close();
    Pipe ready, hold; const auto pid = ::fork(); CHECK(pid >= 0);
    if (pid == 0) {
        ::close(ready.read_fd); ::close(hold.write_fd);
        try {
            SqliteBoundary boundary; boundary.target = f.db;
            auto writer = f.open(StoreRecoveryScope::request(f.id)); WorkerTools tools(*writer, f.work);
            const auto binding = writer->binding(f.task); const auto actor = writer->host_actor(Role::Worker, f.id, binding.team_id);
            tools.bind(actor);
            boundary.after_commit("UPDATE ops SET status='RUNNING',state_version=state_version+1,started_at=?", [&] {
                const char reached = 'R'; if (::write(ready.write_fd, &reached, 1) != 1) ::_exit(91);
                char release{}; while (::read(hold.read_fd, &release, 1) < 0 && errno == EINTR) {}
                ::_exit(92); // Parent must kill/wait at the persisted RUNNING boundary.
            });
            (void)tools.execute(actor, binding, number(writer->task(f.task), "state_version"), "crash-running", "fs_write",
                Object{{"path", "crash-effect.txt"}, {"content", "must not execute after takeover"}}, "crash-command");
        } catch (...) {
            const char error = 'E';
            if (::write(ready.write_fd, &error, 1) != 1) ::_exit(94);
        }
        ::_exit(93);
    }
    Child child(pid); ::close(ready.write_fd); ready.write_fd = -1; ::close(hold.read_fd); hold.read_fd = -1;
    pollfd descriptor{ready.read_fd, POLLIN, 0}; int polled;
    do { polled = ::poll(&descriptor, 1, 10000); } while (polled < 0 && errno == EINTR);
    CHECK(polled == 1 && (descriptor.revents & POLLIN)); char reached{}; CHECK(::read(ready.read_fd, &reached, 1) == 1 && reached == 'R');
    const auto id = value(f.db, "SELECT op_id FROM ops WHERE business_idempotency_key=?", {"crash-running"});
    CHECK(value(f.db, "SELECT status FROM ops WHERE op_id=?", {id}) == "RUNNING");
    CHECK(integer(f.db, "SELECT count(*) FROM ops WHERE op_id=? AND started_at IS NOT NULL", {id}) == 1);
    CHECK(integer(f.db, "SELECT count(*) FROM operation_results") == 0); CHECK(!std::filesystem::exists(f.work / "crash-effect.txt"));
    f.competing_open_is_rejected(); const auto intents = intent_bytes(f.db);
    const auto events = rows(f.db, "SELECT * FROM events");
    child.kill_and_wait(); f.store = f.open(StoreRecoveryScope::request(f.id));
    CHECK(f.store->operation(id).at("status") == "UNKNOWN"); CHECK(f.store->operation_result(id).is_null());
    CHECK(number(f.store->request(f.id), "dispatcher_epoch") == original_binding.dispatcher_epoch + 1);
    CHECK(f.binding().execution_epoch == original_binding.execution_epoch + 1);
    CHECK(intent_bytes(f.db) == intents && immutable_history(f.db) == original && f.store->budget(f.id) == budget);
    const auto recovered_events = rows(f.db, "SELECT * FROM events");
    CHECK(recovered_events.size() == events.size() + 1); CHECK(std::includes(recovered_events.begin(), recovered_events.end(), events.begin(), events.end()));
    const auto recovered = snapshot(f.db);
    expect(ErrorCode::Stale, [&] { (void)f.store->start_tool_operation(old_actor, id, 1, "obsolete-dispatch"); });
    CHECK(snapshot(f.db) == recovered); CHECK(!std::filesystem::exists(f.work / "crash-effect.txt"));
    // The boundary observation is UNKNOWN, not a manufactured definite result
    // or evidence that a business subprocess has exited successfully.
    const Json observation = Object{{"phase", "dispatch_committed_before_file_effect"}, {"host_child_signal", SIGKILL}};
    const Json evidence = Object{{"kind", "controlled_host_child_boundary"}, {"file_present", false}};
    const auto late = f.store->record_operation_result(id, original_binding, "UNKNOWN", observation, evidence);
    CHECK(late.at("accepted") == 0 && late.at("observed_outcome") == "UNKNOWN");
    CHECK(late.at("result") == observation && late.at("evidence") == evidence);
    CHECK(f.store->operation(id).at("status") == "UNKNOWN" && f.store->budget(f.id) == budget);
    CHECK(f.store->record_operation_result(id, original_binding, "UNKNOWN", observation, evidence) == late);
    f.acknowledge(); f.bind_tools(); const auto before_denial = snapshot(f.db);
    expect(ErrorCode::UnknownOutcome, [&] { (void)f.execute("blocked-by-unknown", Object{{"path", "blocked.txt"}, {"content", "denied"}}); });
    CHECK(snapshot(f.db) == before_denial); CHECK(!std::filesystem::exists(f.work / "blocked.txt"));
    const auto settled = snapshot(f.db); f.reopen(); CHECK(snapshot(f.db) == settled);
}

void legacy_cancel_requested_isolated_recovery_race() {
    SqliteBoundary boundary; Fixture f; boundary.target = f.db;
    const auto old_actor = f.actor(); const auto binding = f.binding(); const auto budget = f.store->budget(f.id);
    const auto original = immutable_history(f.db);
    const auto prepared = f.store->prepare_tool_operation(old_actor, binding, f.version(), "legacy-cancellation", "fs_write",
        Object{{"path", "cancel-requested.txt"}, {"content", "must remain unknown"}}, "legacy-prepare");
    const auto id = text(prepared, "op_id");
    CHECK(f.store->start_tool_operation(old_actor, id, number(prepared, "state_version"), "legacy-start").at("dispatch_claimed") == true);
    f.close();
    // Current cancellation APIs fence RUNNING directly to UNKNOWN. This exact
    // schema-compatible historical state has no current public state producer;
    // change only the temporary fixture, with the real operation's CAS/binding.
    fixture_update(f.db, "UPDATE ops SET status='CANCEL_REQUESTED',state_version=state_version+1 WHERE op_id=? AND status='RUNNING'", {id});
    CHECK(value(f.db, "SELECT status FROM ops WHERE op_id=?", {id}) == "CANCEL_REQUESTED");
    CHECK(integer(f.db, "SELECT count(*) FROM ops WHERE status IN ('PREPARED','RUNNING')") == 0);
    CHECK(integer(f.db, "SELECT count(*) FROM call_attempts WHERE status='PENDING'") == 0);
    CHECK(integer(f.db, "SELECT count(*) FROM event_actor_claims WHERE status='ACTIVE'") == 0);
    const auto initial = snapshot(f.db); const auto intents = intent_bytes(f.db); Gate gate;
    boundary.matches = [](unsigned event, std::string_view query) {
        return event == SQLITE_TRACE_STMT && query.starts_with("UPDATE ops SET status=CASE WHEN status='PREPARED'");
    };
    boundary.action = [&] { gate.arrive(); };
    PausedCall recovering(gate, [&] { f.store = f.open(StoreRecoveryScope::request(f.id)); return f.store->request(f.id); });
    (void)gate.await(); CHECK(snapshot(f.db) == initial); f.competing_open_is_rejected();
    gate.release(); recovering.finish(); recovering.rethrow(); boundary.rethrow();
    CHECK(f.store->operation(id).at("status") == "UNKNOWN"); CHECK(f.store->operation_result(id).is_null());
    CHECK(number(f.store->request(f.id), "dispatcher_epoch") == binding.dispatcher_epoch + 1);
    CHECK(f.binding().execution_epoch == binding.execution_epoch + 1);
    CHECK(f.store->budget(f.id) == budget && immutable_history(f.db) == original && intent_bytes(f.db) == intents);
    CHECK(!std::filesystem::exists(f.work / "cancel-requested.txt"));
    const auto recovered = snapshot(f.db); f.reopen(); CHECK(snapshot(f.db) == recovered);
}

void settlement_window_excludes_reopen_and_commits_one_original_fact() {
    SqliteBoundary boundary; Fixture f; boundary.target = f.db; Gate gate;
    const auto actor = f.actor(); const auto binding = f.binding(); const auto version = f.version();
    const auto budget = f.store->budget(f.id); const auto before_events = f.store->events(f.id, 0, 1000);
    const auto before_calls = integer(f.db, "SELECT tool_calls_used FROM teams WHERE team_id=?", {actor.team_id});
    const auto original = immutable_history(f.db);
    boundary.matches = [](unsigned event, std::string_view query) {
        return event == SQLITE_TRACE_STMT && query.starts_with("INSERT INTO operation_results VALUES");
    };
    boundary.action = [&] { gate.arrive(); };
    const Json args = Object{{"path", "settlement-window.txt"}, {"content", "real file precedes atomic settlement"}};
    PausedCall call(gate, [&] { return f.tools->execute(actor, binding, version, "settlement-window", "fs_write", args, "settlement-command"); });
    (void)gate.await(); CHECK(read_bytes(f.work / "settlement-window.txt") == "real file precedes atomic settlement");
    const FileIdentity identity(f.work / "settlement-window.txt");
    CHECK(value(f.db, "SELECT status FROM ops WHERE business_idempotency_key=?", {"settlement-window"}) == "RUNNING");
    CHECK(integer(f.db, "SELECT count(*) FROM operation_results") == 0);
    CHECK(integer(f.db, "SELECT count(*) FROM operation_output_bytes") == 0);
    CHECK(integer(f.db, "SELECT count(*) FROM events WHERE type='record_op_result'") == 0);
    CHECK(immutable_history(f.db) == original); f.competing_open_is_rejected();
    call.finish(); call.rethrow(); boundary.rethrow(); succeeded(call.result);
    const auto id = text(call.result.at("operation"), "op_id");
    CHECK(f.store->operation_result(id) == call.result.at("execution_result"));
    CHECK(integer(f.db, "SELECT count(*) FROM operation_results") == 1);
    CHECK(integer(f.db, "SELECT count(*) FROM operation_output_bytes") == 2);
    CHECK(integer(f.db, "SELECT tool_calls_used FROM teams WHERE team_id=?", {actor.team_id}) == before_calls + 1);
    CHECK(f.store->budget(f.id) == budget); history_prefix(before_events, f.store->events(f.id, 0, 1000), 3);
    const auto result_bytes = rows(f.db, "SELECT * FROM operation_results"); const auto settled = snapshot(f.db);
    f.reopen(); CHECK(snapshot(f.db) == settled); CHECK(to_json(f.binding()) == to_json(binding)); f.bind_tools();
    const auto before_replay = snapshot(f.db);
    expect(ErrorCode::Stale, [&] { (void)f.tools->execute(actor, binding, version, "settlement-window", "fs_write", args, "settlement-command"); });
    const auto replay = f.execute("settlement-window", args);
    CHECK(replay.at("executed_now") == false && replay.at("replayed") == true);
    CHECK(replay.at("execution_result") == call.result.at("execution_result"));
    CHECK(snapshot(f.db) == before_replay && rows(f.db, "SELECT * FROM operation_results") == result_bytes);
    CHECK(f.store->budget(f.id) == budget); identity.unchanged(f.work / "settlement-window.txt");
    CHECK(read_bytes(f.work / "settlement-window.txt") == "real file precedes atomic settlement");
}

void delayed_prepare_after_epoch_switch_cannot_acquire_a_claim() {
    Fixture f; const auto actor = f.actor(); const auto binding = f.binding(); const auto version = f.version();
    const auto budget = f.store->budget(f.id); const auto original = immutable_history(f.db);
    Gate gate; ResetObservers reset;
    before_prepare = [&](std::string_view key) { if (key == "late-epoch-prepare") gate.arrive(); };
    const Json args = Object{{"path", "late-epoch.txt"}, {"content", "obsolete intent"}};
    PausedCall call(gate, [&] { return f.tools->execute(actor, binding, version, "late-epoch-prepare", "fs_write", args, "late-epoch-command"); });
    (void)gate.await(); CHECK(integer(f.db, "SELECT count(*) FROM ops") == 0);
    (void)f.store->recover_request(f.id, number(f.store->request(f.id), "state_version"));
    CHECK(f.binding().execution_epoch == binding.execution_epoch + 1);
    CHECK(number(f.store->request(f.id), "dispatcher_epoch") == binding.dispatcher_epoch + 1);
    const auto recovered = snapshot(f.db); call.finish(); CHECK(call.failure != nullptr);
    expect(ErrorCode::Stale, [&] { call.rethrow(); });
    CHECK(snapshot(f.db) == recovered && immutable_history(f.db) == original && f.store->budget(f.id) == budget);
    CHECK(integer(f.db, "SELECT count(*) FROM ops") == 0 && integer(f.db, "SELECT tool_calls_used FROM teams WHERE team_id=?", {actor.team_id}) == 0);
    CHECK(!std::filesystem::exists(f.work / "late-epoch.txt"));
    f.acknowledge(); succeeded(f.execute("current-after-switch", Object{{"path", "current-after-switch.txt"}, {"content", "authorized once"}}));
    CHECK(read_bytes(f.work / "current-after-switch.txt") == "authorized once");
}

void late_prepare_exact_replay_has_one_effect() {
    Fixture f; WorkerTools other(*f.store, f.work); other.bind(f.actor());
    const auto actor = f.actor(); const auto binding = f.binding(); const auto version = f.version();
    const auto budget = f.store->budget(f.id); const auto before_events = f.store->events(f.id, 0, 1000);
    Gate gate; ResetObservers reset; std::atomic<unsigned> arrivals{0};
    before_prepare = [&](std::string_view key) {
        if (key == "late-exact" && arrivals.fetch_add(1) == 0) gate.arrive();
    };
    const Json args = Object{{"path", "one-exact-effect.txt"}, {"content", "one original effect"}};
    PausedCall delayed(gate, [&] { return other.execute(actor, binding, version, "late-exact", "fs_write", args, "exact-command"); });
    (void)gate.await(); CHECK(integer(f.db, "SELECT count(*) FROM ops") == 0);
    const auto owner = f.tools->execute(actor, binding, version, "late-exact", "fs_write", args, "exact-command"); succeeded(owner);
    const auto id = text(owner.at("operation"), "op_id"); const FileIdentity identity(f.work / "one-exact-effect.txt");
    const auto settled = snapshot(f.db); delayed.finish(); delayed.rethrow();
    CHECK(arrivals.load() == 2); CHECK(delayed.result.at("executed_now") == false && delayed.result.at("replayed") == true);
    CHECK(delayed.result.at("operation") == owner.at("operation") && delayed.result.at("execution_result") == owner.at("execution_result"));
    CHECK(snapshot(f.db) == settled); identity.unchanged(f.work / "one-exact-effect.txt");
    CHECK(read_bytes(f.work / "one-exact-effect.txt") == "one original effect");
    CHECK(integer(f.db, "SELECT count(*) FROM ops") == 1 && integer(f.db, "SELECT count(*) FROM operation_results") == 1);
    CHECK(integer(f.db, "SELECT tool_calls_used FROM teams WHERE team_id=?", {actor.team_id}) == 1);
    CHECK(f.store->budget(f.id) == budget); history_prefix(before_events, f.store->events(f.id, 0, 1000), 3);
    // This second executor remains borrowed only until the enclosing block
    // ends below; it is never used or destroyed after its Store is destroyed.
    CHECK(f.store->operation(id) == owner.at("operation"));
}

struct CleanupFixture : Fixture {
    Json resource, delivery;
    CleanupFixture() : Fixture(false) {
        std::filesystem::create_directory(dir.path / "owned"); std::filesystem::create_directory(dir.path / "delivery");
        auto yaml = read_runtime_file(source / "config/examples/system.yaml");
        const std::string disabled = "cleanup:\n  enabled: false"; const auto at = yaml.find(disabled); CHECK(at != std::string::npos);
        yaml.replace(at, disabled.size(), "cleanup:\n  enabled: true"); write_bytes(dir.path / "system.yaml", yaml);
        const auto frozen = load_config_catalog(dir.path / "system.yaml", source / "config/examples/functions", source / "prompts").freeze("general");
        AdmissionInput input; input.tenant_id = "cleanup-concurrency"; input.project_id = "fixtures"; input.root_id = "root";
        input.function_id = "general"; input.idempotency_key = "request"; input.config_snapshot = frozen.json();
        input.original_text = read_runtime_file(source / "tests/fixtures/s14_request.txt");
        input.constraint_catalog = parse_json_strict(read_runtime_file(source / "tests/fixtures/s14_constraints.json"));
        const auto grants = frozen.worker_tools(); input.tool_allowlist.assign(grants.begin(), grants.end()); input.budget = {64, 20000000, 1000000, 0};
        id = text(store->admit(input), "request_id");
        auto script = event_fake_script_from_json(parse_json_strict(read_runtime_file(source / "tests/fixtures/s15_fake_complete.json")));
        EventPumpOptions first; first.max_calls = 1; first.max_worker_calls_per_task = 2;
        CHECK(EventRuntime(*store, *schemas, {}).pump(id, script, first).at("real_model_calls") == 0);
        task = text(store->tasks(id).at(0), "task_id");
        std::erase_if(script.default_steps, [](const auto& step) { return step.role == Role::Dispatcher && step.role_call_index == 1; });
        for (auto& step : script.default_steps) if (step.role == Role::Dispatcher) --step.role_call_index;
        resource = allocate_owned_workspace(*store, id, task, dir.path / "owned", "allocation");
        work = text(resource.at("identity").at("workspace_identity"), "path"); tools = std::make_unique<WorkerTools>(*store, work);
        EventPumpOptions options; options.max_calls = 12; options.max_worker_calls_per_task = 2;
        const auto completed = EventRuntime(*store, *schemas, {{"alpha", tools.get()}}).pump(id, script, options);
        CHECK(completed.at("real_model_calls") == 0 && store->task(task).at("status") == "ACCEPTED");
        CHECK(store->experience_jobs(id).at(0).at("status") == "DONE"); tools.reset();
        DeliveryDirectory target(dir.path / "delivery");
        delivery = store->prepare_delivery(actor(Role::Root), store->request_binding(id), number(store->request(id), "state_version"), target.root_identity(), "prepare");
        const auto published = target.publish(delivery.at("frozen_plan")); CHECK(published.status == DeliveryMutationStatus::Succeeded);
        delivery = store->record_delivery_publication(actor(Role::Root), store->request_binding(id), text(delivery, "delivery_id"),
            number(delivery, "state_version"), published.publication, "publish");
        delivery = store->acknowledge_delivery(actor(Role::Root), store->request_binding(id), text(delivery, "delivery_id"),
            text(delivery, "manifest_sha256"), number(delivery, "requirements_revision"), number(delivery, "plan_revision"),
            "fixture-recipient", "durable_local", "receipt", published.publication);
    }
    Json publication() const { return DeliveryDirectory(dir.path / "delivery").verify_and_sync_existing(delivery.at("frozen_plan")); }
};
void cleanup_claim_excludes_online_recovery_then_old_claim_is_fenced() {
    SqliteBoundary boundary; CleanupFixture f; boundary.target = f.db; Gate gate;
    const auto root = f.actor(Role::Root); const auto request = f.store->request(f.id); const auto budget = f.store->budget(f.id);
    const auto original = immutable_history(f.db); const auto events = f.store->events(f.id, 0, 1000);
    const auto resource = f.store->owned_workspaces(f.id).at(0); const auto inventory = inventory_owned_workspace(resource);
    const auto publication = f.publication();
    const auto envelope = std::filesystem::path(text(resource.at("identity").at("envelope_identity"), "path"));
    const auto owner = read_bytes(envelope / "owner.json"); const auto result = read_bytes(f.work / "result.txt");
    boundary.after_commit("UPDATE cleanup_jobs SET status='RUNNING',claimed_by=?", [&] { gate.arrive(); });
    PausedCall claiming(gate, [&] { return f.store->begin_workspace_cleanup(root, publication, Array{inventory}); });
    (void)gate.await(); CHECK(value(f.db, "SELECT status FROM cleanup_jobs WHERE request_id=?", {f.id}) == "RUNNING");
    CHECK(integer(f.db, "SELECT count(*) FROM cleanup_jobs WHERE request_id=? AND claimed_by IS NOT NULL", {f.id}) == 1);
    CHECK(read_bytes(f.work / "result.txt") == result && read_bytes(envelope / "owner.json") == owner);
    f.competing_open_is_rejected(); claiming.finish(); claiming.rethrow(); boundary.rethrow();
    const auto claim = claiming.result; CHECK(integer(f.db, "SELECT lease_epoch FROM cleanup_jobs WHERE request_id=?", {f.id}) == number(claim, "lease_epoch"));
    f.reopen(); CHECK(f.store->request(f.id) == request); const auto new_root = f.actor(Role::Root);
    CHECK(new_root.writer_session != root.writer_session);
    const auto job = f.store->closeout(f.id).at("cleanup_job");
    CHECK(job.at("status") == "PENDING" && job.at("claimed_by").is_null()); CHECK(number(job, "lease_epoch") == number(claim, "lease_epoch") + 1);
    CHECK(f.store->owned_workspaces(f.id).at(0).at("inventory") == inventory);
    const auto recovered = snapshot(f.db);
    expect(ErrorCode::Stale, [&] { f.store->authorize_workspace_cleanup(root, claim); });
    expect(ErrorCode::Stale, [&] { f.store->authorize_workspace_cleanup(new_root, claim); });
    CHECK(snapshot(f.db) == recovered); CHECK(read_bytes(f.work / "result.txt") == result);
    CHECK(immutable_history(f.db) == original && f.store->budget(f.id) == budget && f.store->events(f.id, 0, 1000) == events);
    // Isolate the Store-session guard from the independently stale claim:
    // this exact current claim is valid for the new Root before the old Root
    // attempts it. Terminal recovery left both Roots' persisted scope/epoch
    // unchanged, so a stale claim or epoch cannot mask a missing session guard.
    const auto current_claim = f.store->begin_workspace_cleanup(new_root, f.publication(), Array{inventory});
    CHECK(new_root.principal_id == root.principal_id && new_root.dispatcher_epoch == root.dispatcher_epoch);
    CHECK(number(current_claim, "lease_epoch") == number(job, "lease_epoch") + 1);
    const auto current = snapshot(f.db); const FileIdentity result_identity(f.work / "result.txt");
    const FileIdentity owner_identity(envelope / "owner.json");
    f.store->authorize_workspace_cleanup(new_root, current_claim);
    CHECK(snapshot(f.db) == current);
    expect(ErrorCode::Stale, [&] { f.store->authorize_workspace_cleanup(root, current_claim); });
    CHECK(snapshot(f.db) == current); result_identity.unchanged(f.work / "result.txt");
    owner_identity.unchanged(envelope / "owner.json");
    CHECK(read_bytes(f.work / "result.txt") == result && read_bytes(envelope / "owner.json") == owner);
    const auto cleaned = cleanup_owned_workspaces(*f.store, f.id);
    CHECK(cleaned.at("workspace_absence_verified_now") == true && !std::filesystem::exists(f.work));
    CHECK(read_bytes(envelope / "owner.json") == owner && f.publication().at("verified") == true);
    CHECK(immutable_history(f.db) == original && f.store->budget(f.id) == budget && f.store->events(f.id, 0, 1000) == events);
    CHECK(f.store->request(f.id) == request);
    const auto before_terminal_write = snapshot(f.db);
    expect(ErrorCode::NotReady, [&] { (void)f.store->prepare_owned_workspace(new_root, f.task, publication.at("root_identity"), "terminal-allocation"); });
    CHECK(snapshot(f.db) == before_terminal_write);
}
} // namespace

int main(int argc, char** argv) {
    std::string group = "all";
    if (argc == 3 && std::string_view(argv[1]) == "--group") group = argv[2];
    else if (argc != 1) { std::cerr << "usage: r24_concurrency_tests [--group phases|settlement|replay|cleanup]\n"; return 2; }
    if (group != "all" && group != "phases" && group != "settlement" && group != "replay" && group != "cleanup") {
        std::cerr << "unknown r24 concurrency group\n"; return 2;
    }
    base_sql = read_runtime_file(source / "db/schema.sql"); store_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql"); events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    workflow_sql = read_runtime_file(source / "db/migrations/007_workflow_runtime.sql"); acceptance_sql = read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql");
    cleanup_sql = read_runtime_file(source / "db/migrations/009_owned_cleanup.sql"); schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    config = std::make_unique<FrozenRequestConfig>(load_config_catalog(source / "config/examples/system.yaml", source / "config/examples/functions", source / "prompts").freeze("general"));
    std::vector<test::Case> cases;
    if (group == "all" || group == "phases") {
        cases.emplace_back("PREPARED writer excludes reopen; exited writer recovers once and fences exact old prepare", prepared_reopen_exclusion_then_real_recovery);
        cases.emplace_back("RUNNING child boundary excludes reopen until SIGKILL/wait and preserves UNKNOWN late facts", running_crash_recovers_only_after_original_writer_exits);
        cases.emplace_back("isolated legacy CANCEL_REQUESTED state is fenced atomically against another opener", legacy_cancel_requested_isolated_recovery_race);
    }
    if (group == "all" || group == "settlement")
        cases.emplace_back("real file effect and result INSERT window exclude reopen and preserve one original fact", settlement_window_excludes_reopen_and_commits_one_original_fact);
    if (group == "all" || group == "replay") {
        cases.emplace_back("late prepare cannot cross an explicit execution epoch switch", delayed_prepare_after_epoch_switch_cannot_acquire_a_claim);
        cases.emplace_back("late exact prepare replays one original effect without new writes", late_prepare_exact_replay_has_one_effect);
    }
    if (group == "all" || group == "cleanup")
        cases.emplace_back("cleanup claim excludes online recovery and exited writer loses Root/claim authority", cleanup_claim_excludes_online_recovery_then_old_claim_is_fenced);
    return test::run(cases);
}
