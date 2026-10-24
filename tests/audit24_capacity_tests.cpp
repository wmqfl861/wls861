// Frozen capacity, durable admission holds and retained command replay. All data are throwaway.
#include "wls/capacity.hpp"
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
    explicit Fixture(bool legacy = false, std::int64_t revision_limit = 100) {
        store = open();
        const auto config = load_config_catalog(source / "config/examples/system.yaml", source / "config/examples/functions", source / "prompts");
        AdmissionInput in; in.tenant_id = "t"; in.project_id = "p"; in.root_id = "root"; in.function_id = "general";
        in.idempotency_key = "reopen"; in.original_text = "preserve original input";
        in.config_snapshot = config.freeze("general").json(); in.tool_allowlist = {"fs_write", "ack_change"};
        auto& limits = in.config_snapshot.at("system").at("limits").as_object();
        limits["max_teams_per_request"] = 1; limits["worker_max_tool_calls_per_order"] = 4;
        limits["team_internal_rework_max"] = 0; limits["manager_max_tool_calls_per_wake"] = 1;
        if (legacy) limits.erase("store_capacity");
        else {
            auto& capacity = limits.at("store_capacity").as_object();
            capacity["requirement_revisions"] = revision_limit;
            capacity["business_events"] = 64; capacity["control_events"] = 64; capacity["operations"] = 4;
        }

        // Freeze the changed config from matching source bytes, never forge a
        // parsed snapshot while retaining its original dependency fingerprint.
        const auto config_path = dir.path / "system.json";
        { std::ofstream file(config_path); file << canonical_json(in.config_snapshot.at("system")); }
        in.config_snapshot = load_config_catalog(config_path, source / "config/examples/functions", source / "prompts").freeze("general").json();
        in.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", "preserve original input"}, {"source_revision", 1}}};
        request = text(store->admit(in), "request_id");
        (void)store->publish_plan(actor(Role::Dispatcher), num(store->request(request), "state_version"), 1, Object{{"goal", "p"}}, "plan");
        const auto t = store->create_task(actor(Role::Dispatcher), num(store->request(request), "state_version"), 1, 1, "node", brief, "task");
        task = text(t, "task_id"); team = text(t, "team_id");
        (void)store->issue_instruction(actor(Role::TeamLead), store->binding(task), tv(), brief, "order");
        ack("ack-original");
    }
};
void finish(Fixture& f, const Json& op, const std::string& key) {
    const auto started = f.store->start_operation(f.actor(Role::Worker), text(op, "op_id"), num(op, "state_version"), "start-" + key);
    (void)f.store->finish_operation(f.actor(Role::Worker), text(op, "op_id"), f.store->binding(f.task), num(started, "state_version"), "FAILED", "known synthetic effect", "finish-" + key);
}
void fill_operations(Fixture& f) { for (int i = 0; i < 4; ++i) { const auto key = std::to_string(i); finish(f, f.prepare(key), key); } }
Json checkpoint(Fixture& f, std::int64_t version = 0, const std::string& key = "checkpoint") {
    return f.store->checkpoint_capacity(f.actor(Role::Root), version, "host inspected settled retained audit", key);
}
void hold_is_durable_and_resets_no_business_budget() {
    Fixture f; fill_operations(f);
    CHECK(f.store->capacity_status(f.request).at("status") == "WAITING_INPUT");
    CHECK(scalar(f.db, "SELECT COUNT(*) FROM events WHERE type='capacity_hold'") == 1);
    const auto budget = f.store->budget(f.request); const auto texts = f.store->request_snapshot(f.request);
    const auto first_event = f.store->events(f.request).front();
    const auto reply = checkpoint(f);
    CHECK(reply.at("window") == 1); CHECK(f.store->capacity_status(f.request).at("status") == "OPEN");
    CHECK(f.store->budget(f.request) == budget); CHECK(f.store->request_snapshot(f.request) == texts);
    CHECK(f.store->events(f.request).front() == first_event);
    const auto old_operation = f.store->prepare_operation(f.actor(Role::Worker), f.store->binding(f.task), f.tv(),
        "0", "fs_write", Object{{"a", "0"}}, "workspace", "new-command-for-old-business-key");
    CHECK(old_operation.at("status") == "FAILED");
    CHECK(f.store->capacity_status(f.request).at("counts").at("operations") == 0);
    CHECK(scalar(f.db, "SELECT tool_calls_used FROM teams") == 4);
    fails(ErrorCode::Quota, [&] { (void)f.prepare("over-order-budget"); });
    f.reopen(); CHECK(checkpoint(f) == reply); CHECK(scalar(f.db, "SELECT COUNT(*) FROM events WHERE type='capacity_checkpoint'") == 1);
    f.ack();
    (void)f.store->issue_instruction(f.actor(Role::TeamLead), f.store->binding(f.task), f.tv(), f.brief, "next-real-order");
    f.ack("ack-next-order"); finish(f, f.prepare("resumed"), "resumed");
    CHECK(f.store->capacity_status(f.request).at("counts").at("operations") == 1);
}
void checkpoint_does_not_hide_unknown() {
    Fixture f; for (int i=0;i<3;++i) { const auto key=std::to_string(i); finish(f,f.prepare(key),key); }
    auto op=f.prepare("pending"); op=f.store->start_operation(f.actor(Role::Worker), text(op,"op_id"),num(op,"state_version"),"start-pending");
    f.reopen(); f.ack();
    CHECK(f.store->capacity_status(f.request).at("status") == "WAITING_INPUT");
    fails(ErrorCode::UnknownOutcome, [&] { (void)checkpoint(f); });
    op=f.store->operation(text(op,"op_id"));
    (void)f.store->reconcile_operation(f.actor(Role::Root),text(op,"op_id"),num(op,"state_version"),"FAILED","host checked partial effects","reconcile");
    CHECK(checkpoint(f).at("window") == 1);
    CHECK(f.store->operation(text(op,"op_id")).at("status") == "FAILED");
}
void control_has_separate_warning_and_safe_resume() {
    Fixture f; const auto business = f.store->capacity_status(f.request).at("counts").at("business_events");
    std::int64_t generation=0;
    for (int i=0;i<64;++i) {
        if (f.store->capacity_status(f.request).at("status") == "WAITING_INPUT") break;
        const auto w=f.store->wait_worker(f.actor(Role::Worker), f.store->binding(f.task),f.tv(),"pause","wait-"+std::to_string(i));
        generation=num(w,"generation");
        (void)f.store->wake_worker(f.actor(Role::Worker),f.store->binding(f.task),generation,"wake-"+std::to_string(i));
    }
    CHECK(generation > 0); CHECK(f.store->capacity_status(f.request).at("status") == "WAITING_INPUT");
    CHECK(f.store->capacity_status(f.request).at("counts").at("business_events") == business);
    CHECK(scalar(f.db,"SELECT COUNT(*) FROM events WHERE type='capacity_warning'") == 1);
    fails(ErrorCode::NotReady,[&] { (void)f.prepare(); });
    CHECK(checkpoint(f).at("window") == 1); finish(f,f.prepare(),"after-control-window");
}
void root_cas_replay_and_rollback() {
    Fixture f; fill_operations(f);
    const auto state=f.store->capacity_status(f.request);
    fails(ErrorCode::Unauthorized,[&] { (void)f.store->checkpoint_capacity(f.actor(Role::Worker),0,"evidence","bad-role"); });
    fails(ErrorCode::InvalidInput,[&] { f.store->validate_capacity_checkpoint(f.actor(Role::Root),0,"evidence",""); });
    fails(ErrorCode::Conflict,[&] { (void)checkpoint(f,1,"stale"); });
    fails(ErrorCode::InvalidInput,[&] { f.store->validate_capacity_checkpoint(f.actor(Role::Root),0,"   ","blank"); });
    CHECK(f.store->capacity_status(f.request)==state);
    sql(f.db,"CREATE TRIGGER fail_checkpoint BEFORE INSERT ON events WHEN NEW.type='capacity_checkpoint' BEGIN SELECT RAISE(ABORT,'injected'); END;");
    fails(ErrorCode::Conflict,[&] { (void)checkpoint(f); }); CHECK(f.store->capacity_status(f.request)==state);
    sql(f.db,"DROP TRIGGER fail_checkpoint"); const auto reply=checkpoint(f);
    CHECK(checkpoint(f)==reply);
    fails(ErrorCode::Conflict,[&] { (void)f.store->checkpoint_capacity(f.actor(Role::Root),0,"different evidence","checkpoint"); });
}
void failed_business_event_invalidates_cached_counts() {
    Fixture f; const auto before=f.store->capacity_status(f.request);
    const auto order=f.store->instruction(f.task);
    sql(f.db,"CREATE TRIGGER fail_route BEFORE INSERT ON event_inbox WHEN (SELECT type FROM events WHERE seq=NEW.event_seq)='instruction' BEGIN SELECT RAISE(ABORT,'injected routing failure'); END;");
    fails(ErrorCode::Conflict,[&] { (void)f.store->issue_instruction(f.actor(Role::TeamLead),f.store->binding(f.task),f.tv(),f.brief,"new-order"); });
    CHECK(f.store->capacity_status(f.request)==before); CHECK(f.store->instruction(f.task)==order);
    sql(f.db,"DROP TRIGGER fail_route");
    (void)f.store->issue_instruction(f.actor(Role::TeamLead),f.store->binding(f.task),f.tv(),f.brief,"new-order");
    CHECK(f.store->capacity_status(f.request).at("counts").at("business_events").as_int64() ==
        before.at("counts").at("business_events").as_int64()+1);
}
void checkpoint_cannot_apply_unhandled_amendment() {
    Fixture f(false,2);
    (void)f.store->amend(f.actor(Role::Root),num(f.store->request(f.request),"state_version"),
        "new requirement",Array{},"amendment");
    CHECK(f.store->capacity_status(f.request).at("status")=="WAITING_INPUT");
    const auto prior=f.store->request_snapshot(f.request);
    (void)checkpoint(f); CHECK(f.store->request_snapshot(f.request)==prior);
    fails(ErrorCode::NotReady,[&] { f.ack(); });
    fails(ErrorCode::NotReady,[&] { (void)f.prepare(); });
    CHECK(f.store->request(f.request).at("requirements_revision")==2);
}
void old_snapshot_limits_remain_frozen() {
    Fixture f(true); const auto snapshot=f.store->request_snapshot(f.request);
    const auto state=f.store->capacity_status(f.request);
    CHECK(state.at("limits").at("business_events")==4096); CHECK(state.at("limits").at("operations")==1024);
    f.reopen(); CHECK(f.store->request_snapshot(f.request)==snapshot);
    CHECK(f.store->capacity_status(f.request).at("limits")==state.at("limits"));
    CHECK(!f.store->request(f.request).at("config_snapshot").at("system").at("limits").as_object().contains("store_capacity"));
}
void inconsistent_admission_is_rejected_without_rows() {
    test::TempDir dir;
    const auto config=load_config_catalog(source/"config/examples/system.yaml", source/"config/examples/functions", source/"prompts");
    auto system=config.freeze("general").json().at("system");
    validate_store_capacity_admission(system);
    system.at("limits").at("store_capacity").as_object()["operations"]=1024;
    fails(ErrorCode::InvalidInput,[&] { validate_store_capacity_admission(system); });
    system=config.freeze("general").json().at("system");
    system.at("limits").at("store_capacity").as_object()["control_events"]=64;
    fails(ErrorCode::InvalidInput,[&] { validate_store_capacity_admission(system); });
    system=config.freeze("general").json().at("system");
    system.at("limits").at("store_capacity").as_object().erase("deliveries");
    fails(ErrorCode::InvalidInput,[&] { (void)store_capacity_limits(system); });
    // Legacy restore remains readable; only a new inconsistent admission fails.
    system=config.freeze("general").json().at("system"); system.at("limits").as_object().erase("store_capacity");
    CHECK(store_capacity_limits(system).at("operations")==1024);
    fails(ErrorCode::InvalidInput,[&] { validate_store_capacity_admission(system); });
}
}
int main() { return test::run({
    {"durable window and exact replay preserve all business budgets",hold_is_durable_and_resets_no_business_budget},
    {"checkpoint cannot hide UNKNOWN even after fresh ack",checkpoint_does_not_hide_unknown},
    {"control traffic warns and never consumes business slots",control_has_separate_warning_and_safe_resume},
    {"Root CAS, replay and injected failure are atomic",root_cas_replay_and_rollback},
    {"event rollback invalidates cached counts",failed_business_event_invalidates_cached_counts},
    {"checkpoint never applies unhandled AMEND",checkpoint_cannot_apply_unhandled_amendment},
    {"old snapshots retain exact legacy limits",old_snapshot_limits_remain_frozen},
    {"admission validates full tool lifecycle and frozen rounds",inconsistent_admission_is_rejected_without_rows}}); }
