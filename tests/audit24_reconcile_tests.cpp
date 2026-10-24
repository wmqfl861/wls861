// Host reconciliation and classified settlement contracts; throwaway databases only.
#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "wls/process_evidence.hpp"
#include <fcntl.h>
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
    Json prepare(const std::string& key = "new", const std::string& tool = "fs_write") {
        return store->prepare_operation(actor(Role::Worker), store->binding(task), tv(), key, tool,
            Object{{"a", key}}, "workspace", "prepare-" + key);
    }
    Fixture() {
        store = open();
        const auto config = load_config_catalog(source / "config/examples/system.yaml", source / "config/examples/functions", source / "prompts");
        AdmissionInput in; in.tenant_id = "t"; in.project_id = "p"; in.root_id = "root"; in.function_id = "general";
        in.idempotency_key = "reopen"; in.original_text = "preserve original input";
        in.config_snapshot = config.freeze("general").json(); in.tool_allowlist = {"fs_write", "run", "ack_change"};
        in.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", "preserve original input"}, {"source_revision", 1}}};
        request = text(store->admit(in), "request_id");
        (void)store->publish_plan(actor(Role::Dispatcher), num(store->request(request), "state_version"), 1, Object{{"goal", "p"}}, "plan");
        const auto t = store->create_task(actor(Role::Dispatcher), num(store->request(request), "state_version"), 1, 1, "node", brief, "task");
        task = text(t, "task_id"); team = text(t, "team_id");
        (void)store->issue_instruction(actor(Role::TeamLead), store->binding(task), tv(), brief, "order");
        ack("ack-original");
    }
};
Json unknown(Fixture& f) {
    auto op = f.prepare();
    op = f.store->start_operation(f.actor(Role::Worker), text(op, "op_id"), num(op, "state_version"), "start");
    (void)f.store->cancel_operation(f.actor(Role::Worker), text(op, "op_id"), num(op, "state_version"), "cancel-op");
    (void)f.store->record_operation_result(text(op, "op_id"), f.store->binding(f.task), "UNKNOWN",
        Object{{"error", "fixture_uncertain"}}, Object{{"all_descendants_exited", false}}, "original output", "", false, false);
    return f.store->operation(text(op, "op_id"));
}
void classified_failures_settle_and_allow_new_business() {
    Fixture f; test::TempDir work;
    const int fd = ::open(work.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    const auto before = observe_process_workspace(fd);
    { std::ofstream stream(work.path / "partial-result"); stream << "effect survived a failed command"; CHECK(stream.good()); }
    const auto after = observe_process_workspace(fd);
    // The mount/exit fields below are synthetic classification fixtures. Actual
    // strong sandbox deployment is a separate, still-unexecuted acceptance.
    ProcessResult p;
    p.spawned = p.launcher_exec_confirmed = p.strong_isolation = p.all_descendants_exited = true;
    p.exit_code = 2;
    p.evidence = Object{{"network", "none"}, {"supervision", "cgroup-v2"},
        {"descendants_proof", "cgroup.events populated=0"}, {"workspace_mount_verified", true},
        {"namespaces_verified", true}, {"command_start_released", true},
        {"workspace_observation", process_workspace_changes(before, after)}};
    int index = 0;
    for (const auto status : {ProcessStatus::Exited, ProcessStatus::TimedOut, ProcessStatus::OutputLimit, ProcessStatus::Cancelled}) {
        p.status = status; const auto key = "classified-" + std::to_string(index++);
        auto op = f.prepare(key, "run"); const auto binding = f.store->binding(f.task);
        op = f.store->start_operation(f.actor(Role::Worker), text(op, "op_id"), num(op, "state_version"), "start-" + key);
        const auto outcome = process_operation_outcome(p);
        CHECK(outcome != "UNKNOWN");
        const auto recorded = f.store->record_operation_result(text(op, "op_id"), binding, outcome,
            to_json(p), p.evidence, "original failure output", "", false, false);
        CHECK(f.store->operation(text(op, "op_id")).at("status").as_string() == outcome);
        CHECK(recorded.at("evidence").at("workspace_observation").at("changes").as_array().size() == 1);
    }
    CHECK(f.prepare("new-business").at("status") == "PREPARED");
    CHECK(read(work.path / "partial-result") == "effect survived a failed command");
}
void root_reconciles_and_replays_without_rewriting_execution() {
    Fixture f; const auto op = unknown(f); const auto id = text(op, "op_id");
    const auto original = f.store->operation_result(id); const auto before = f.store->events(f.request).size();
    const auto root = f.actor(Role::Root);
    f.store->validate_reconciliation(root, id, num(op, "state_version"), "FAILED", "checked partial file changes", "settlement");
    CHECK(f.store->events(f.request).size() == before);
    const auto result = f.store->reconcile_operation(root, id, num(op, "state_version"), "FAILED", "checked partial file changes", "settlement");
    CHECK(result.at("status") == "FAILED");
    f.reopen();
    CHECK(f.store->reconcile_operation(f.actor(Role::Root), id, num(op, "state_version"), "FAILED", "checked partial file changes", "settlement") == result);
    auto retained = f.store->operation_result(id);
    // operation_status is a current projection; original result and evidence stay identical.
    retained.as_object()["operation_status"] = original.at("operation_status"); CHECK(retained == original);
    f.ack(); CHECK(f.prepare("after-host-check").at("status") == "PREPARED");
    fails(ErrorCode::Conflict, [&] { (void)f.store->reconcile_operation(f.actor(Role::Root), id, num(op, "state_version"), "FAILED", "different", "settlement"); });
}
void wrong_role_owner_cas_and_blank_evidence_are_rejected() {
    Fixture f; const auto op = unknown(f); const auto id = text(op, "op_id"); const auto before = f.store->events(f.request);
    for (auto role : {Role::Worker, Role::TeamLead, Role::Dispatcher})
        fails(ErrorCode::Unauthorized, [&] { (void)f.store->reconcile_operation(f.actor(role), id, num(op, "state_version"), "FAILED", "proof", "reject"); });
    auto fake = f.actor(Role::Root); fake.principal_id = "not-owner";
    fails(ErrorCode::Unauthorized, [&] { (void)f.store->reconcile_operation(fake, id, num(op, "state_version"), "FAILED", "proof", "reject"); });
    fails(ErrorCode::Conflict, [&] { (void)f.store->reconcile_operation(f.actor(Role::Root), id, num(op, "state_version") + 1, "FAILED", "proof", "reject"); });
    for (const std::string& evidence : {std::string{}, std::string(" \t\r\n"), std::string(4097, 'x')})
        fails(ErrorCode::InvalidInput, [&] { (void)f.store->reconcile_operation(f.actor(Role::Root), id, num(op, "state_version"), "FAILED", evidence, "reject"); });
    fails(ErrorCode::InvalidInput, [&] { (void)f.store->reconcile_operation(f.actor(Role::Root), id, num(op, "state_version"), "UNKNOWN", "proof", "reject"); });
    CHECK(f.store->events(f.request) == before); CHECK(f.store->operation(id) == op);
}
void failed_reconciliation_event_is_atomic() {
    Fixture f; const auto op = unknown(f); const auto id = text(op, "op_id");
    sql(f.db, "CREATE TRIGGER fail_reconcile BEFORE INSERT ON events WHEN NEW.type='reconcile_op' BEGIN SELECT RAISE(ABORT,'injected'); END;");
    fails(ErrorCode::Conflict, [&] { (void)f.store->reconcile_operation(f.actor(Role::Root), id, num(op, "state_version"), "TIMED_OUT", "bounded evidence", "reconcile"); });
    CHECK(f.store->operation(id) == op);
    fails(ErrorCode::UnknownOutcome, [&] { (void)f.prepare("blocked"); });
    sql(f.db, "DROP TRIGGER fail_reconcile");
    CHECK(f.store->reconcile_operation(f.actor(Role::Root), id, num(op, "state_version"), "TIMED_OUT", "bounded evidence", "reconcile").at("status") == "TIMED_OUT");
}
}
int main() { return test::run({
    {"classified failed/timeout/output/cancel results retain effects and admit new work", classified_failures_settle_and_allow_new_business},
    {"Root reconciliation has exact replay and preserves original executor evidence", root_reconciles_and_replays_without_rewriting_execution},
    {"reconciliation rejects other roles wrong owner stale CAS and missing evidence", wrong_role_owner_cas_and_blank_evidence_are_rejected},
    {"reconciliation event failure rolls back and keeps UNKNOWN business fence", failed_reconciliation_event_is_atomic}}); }
