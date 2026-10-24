// Regression conversion of Claude's reopen_repro.cpp. All data are throwaway.
#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "test_support.hpp"
#include <sqlite3.h>
#include <fstream>
#include <iterator>
#include <memory>
using namespace wls::s1;
using wls::Role;
using Object = boost::json::object;
using Array = boost::json::array;
namespace {
const std::filesystem::path source = WLS_SOURCE_DIR;
std::string read(const std::filesystem::path& p) {
    std::ifstream s(p, std::ios::binary);
    if (!s) throw std::runtime_error("fixture missing");
    return {std::istreambuf_iterator<char>(s), {}};
}
std::string text(const Json& v, std::string_view k) { return std::string(v.at(k).as_string()); }
std::int64_t num(const Json& v, std::string_view k) { return v.at(k).as_int64(); }
void fails(ErrorCode code, const std::function<void()>& fn) {
    try { fn(); } catch (const Error& e) { CHECK(e.code() == code); return; }
    throw std::runtime_error("expected failure");
}
void sql(const std::filesystem::path& path, const std::string& statement) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    char* msg = nullptr;
    const auto rc = sqlite3_exec(db, statement.c_str(), nullptr, nullptr, &msg);
    const std::string error = msg ? msg : "SQLite fixture failed";
    sqlite3_free(msg); sqlite3_close(db);
    if (rc != SQLITE_OK) throw std::runtime_error(error);
}
std::int64_t scalar(const std::filesystem::path& path, const std::string& query) {
    sqlite3* db = nullptr; CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr; CHECK(sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW); const auto result = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt); sqlite3_close(db); return result;
}
struct Fixture {
    test::TempDir dir;
    std::filesystem::path db{dir.path / "state.sqlite"};
    std::unique_ptr<Store> store;
    std::string request, task, team;
    Json brief = Object{{"goal", "g"}, {"constraint_refs", Array{"c1"}},
        {"constraints_verbatim", Array{"preserve original input"}}};
    std::unique_ptr<Store> open(StoreOpenMode mode = StoreOpenMode::Takeover) {
        return std::make_unique<Store>(db, read(source / "db/schema.sql"), read(source / "db/migrations/004_s1_store.sql"), mode,
            read(source / "db/migrations/005_worker_tools.sql"), read(source / "db/migrations/006_event_runtime.sql"),
            read(source / "db/migrations/007_workflow_runtime.sql"), read(source / "db/migrations/008_acceptance_runtime.sql"),
            read(source / "db/migrations/009_owned_cleanup.sql"));
    }
    StoreActor actor(Role role) { return store->host_actor(role, request,
        role == Role::TeamLead || role == Role::Worker ? team : ""); }
    std::int64_t tv() { return num(store->task(task), "state_version"); }
    void ack(const std::string& key = "ack-new") {
        (void)store->acknowledge(actor(Role::Worker), store->binding(task), tv(), key);
    }
    void reopen() { store.reset(); store = open(); }
    Json prepare(const std::string& key = "new") {
        return store->prepare_operation(actor(Role::Worker), store->binding(task), tv(), key, "fs_write",
            Object{{"a", key}}, "workspace", "prepare-" + key);
    }
    Fixture() {
        store = open();
        const auto config = load_config_catalog(source / "config/examples/system.yaml", source / "config/examples/functions", source / "prompts");
        AdmissionInput in; in.tenant_id = "t"; in.project_id = "p"; in.root_id = "root"; in.function_id = "general";
        in.idempotency_key = "reopen"; in.original_text = "preserve original input";
        in.config_snapshot = config.freeze("general").json(); in.tool_allowlist = {"fs_write", "ack_change"};
        in.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", "preserve original input"}, {"source_revision", 1}}};
        request = text(store->admit(in), "request_id");
        (void)store->publish_plan(actor(Role::Dispatcher), num(store->request(request), "state_version"), 1, Object{{"goal", "p"}}, "plan");
        const auto t = store->create_task(actor(Role::Dispatcher), num(store->request(request), "state_version"), 1, 1, "node", brief, "task");
        task = text(t, "task_id"); team = text(t, "team_id");
        (void)store->issue_instruction(actor(Role::TeamLead), store->binding(task), tv(), brief, "order");
        ack("ack-original");
    }
};
void same_instruction_reacks_without_rewriting() {
    Fixture f;
    const auto before = f.store->instruction(f.task);
    const auto prepared = f.prepare("old");
    const auto events = f.store->events(f.request);
    f.reopen();
    CHECK(f.store->operation(text(prepared, "op_id")).at("status") == "CANCELLED");
    fails(ErrorCode::NotReady, [&] { (void)f.prepare(); });
    CHECK(f.store->instruction(f.task).at("body") == before.at("body"));
    CHECK(f.store->task(f.task).at("instruction_revision") == 1);
    f.ack();
    CHECK(f.prepare().at("status") == "PREPARED");
    const auto recovered = f.store->instruction(f.task);
    CHECK(num(recovered, "execution_epoch") > num(before, "execution_epoch"));
    const auto after = f.store->events(f.request);
    for (std::size_t i = 0; i < events.size(); ++i) CHECK(events[i] == after[i]);
}
void in_flight_and_old_actor_remain_fenced() {
    Fixture f;
    const auto old_actor = f.actor(Role::Worker); const auto binding = f.store->binding(f.task);
    auto op = f.prepare();
    op = f.store->start_operation(old_actor, text(op, "op_id"), num(op, "state_version"), "start");
    f.reopen();
    fails(ErrorCode::Stale, [&] { (void)f.store->acknowledge(old_actor, binding, f.tv(), "old-actor"); });
    CHECK(f.store->operation(text(op, "op_id")).at("status") == "UNKNOWN");
    f.ack();
    fails(ErrorCode::UnknownOutcome, [&] { (void)f.prepare("another"); });
    const auto current = f.store->operation(text(op, "op_id"));
    (void)f.store->reconcile_operation(text(op, "op_id"), num(current, "state_version"), "FAILED", "host fixture checked effects", "reconcile");
    CHECK(f.prepare("after-reconcile").at("status") == "PREPARED");
}
void waiting_survives_control_ack() {
    Fixture f;
    const auto wait = f.store->wait_worker(f.actor(Role::Worker), f.store->binding(f.task), f.tv(), "wait for input", "wait");
    f.reopen(); f.ack();
    CHECK(f.store->worker_wait(f.task).at("status") == "WAITING");
    fails(ErrorCode::NotReady, [&] { (void)f.prepare(); });
    (void)f.store->wake_worker(f.actor(Role::Worker), f.store->binding(f.task), num(wait, "generation"), "wake");
    CHECK(f.prepare().at("status") == "PREPARED");
    CHECK(f.store->task(f.task).at("instruction_revision") == 1);
}
void used_order_quota_not_reset() {
    Fixture f; (void)f.prepare("one");
    f.reopen(); f.ack();
    CHECK(scalar(f.db, "SELECT tool_calls_used FROM teams") == 1);
    (void)f.prepare("two");
    CHECK(scalar(f.db, "SELECT tool_calls_used FROM teams") == 2);
    f.reopen(); f.ack("third-ack");
    CHECK(scalar(f.db, "SELECT tool_calls_used FROM teams") == 2);
}
void registered_ack_uses_recovery_projection() {
    Fixture f; f.reopen();
    (void)f.store->execute_tool_control(f.actor(Role::Worker), f.store->binding(f.task), f.tv(), "ack_change", Object{}, "registered-ack");
    CHECK(f.store->task(f.task).at("applied_instruction_revision") == 1);
    const auto effective = f.store->instruction(f.task);
    CHECK(num(effective, "execution_epoch") > scalar(f.db, "SELECT execution_epoch FROM instructions"));
    fails(ErrorCode::NotReady, [&] { f.ack("repeat-new-key"); });
    bool refused = false;
    try { sql(f.db, "UPDATE instructions SET execution_epoch=999"); }
    catch (const std::exception&) { refused = true; }
    CHECK(refused); // Do not remove the existing immutable-row trigger.
}
void changed_semantics_never_rebind() {
    Fixture f;
    const auto old_order = f.store->instruction(f.task);
    (void)f.store->amend(f.actor(Role::Root), num(f.store->request(f.request), "state_version"),
        "preserve original input; amended", Array{Object{{"constraint_id", "c1"}, {"text", "preserve original input"}, {"source_revision", 1}}}, "amend");
    f.reopen();
    fails(ErrorCode::NotReady, [&] { f.ack(); });
    CHECK(f.store->instruction(f.task) == old_order);
    fails(ErrorCode::NotReady, [&] { (void)f.prepare(); });
}
void recovery_failure_rolls_back_rebinding() {
    Fixture f;
    const auto order = f.store->instruction(f.task); const auto task = f.store->task(f.task); const auto request = f.store->request(f.request);
    f.store.reset();
    sql(f.db, "CREATE TRIGGER fail_recovery BEFORE INSERT ON events WHEN NEW.type='recover' BEGIN SELECT RAISE(ABORT,'injected'); END;");
    fails(ErrorCode::Conflict, [&] { auto ignored = f.open(); });
    f.store = f.open(StoreOpenMode::ReadOnly);
    CHECK(f.store->instruction(f.task) == order); CHECK(f.store->task(f.task) == task); CHECK(f.store->request(f.request) == request);
    f.store.reset(); sql(f.db, "DROP TRIGGER fail_recovery"); f.store = f.open(); f.ack();
}
}
int main() {
    return test::run({{"same semantic instruction reacks after writer takeover", same_instruction_reacks_without_rewriting},
        {"running effects and old actor remain fenced", in_flight_and_old_actor_remain_fenced},
        {"control ack never clears WAITING", waiting_survives_control_ack},
        {"order tool quota survives repeated takeover", used_order_quota_not_reset},
        {"registered control ack uses host projection, originals remain immutable", registered_ack_uses_recovery_projection},
        {"unhandled AMEND never gets epoch-only rebind", changed_semantics_never_rebind},
        {"failed recover event rolls back all projections", recovery_failure_rolls_back_rebinding}});
}
