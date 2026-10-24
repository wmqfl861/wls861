// Reproduction: any writer reopen fences every live team; the existing
// instruction becomes stale, so business cannot resume without a NEW
// TeamLead instruction (i.e. a new management model call), not just an ack.
#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "test_support.hpp"
#include <fstream>
#include <iterator>
#include <iostream>
#include <memory>
using namespace wls::s1; using wls::Role;
using Object = boost::json::object; using Array = boost::json::array;
static std::string read(const std::filesystem::path& p) { std::ifstream s(p, std::ios::binary); return {std::istreambuf_iterator<char>(s), {}}; }
static std::string s(const Json& v, std::string_view k) { return std::string(v.at(k).as_string()); }
static std::int64_t n(const Json& v, std::string_view k) { return v.at(k).as_int64(); }
int main() {
    const std::filesystem::path src = WLS_SOURCE_DIR;
    test::TempDir dir; const auto db = dir.path / "state.sqlite";
    auto open = [&] { return std::make_unique<Store>(db, read(src / "db/schema.sql"), read(src / "db/migrations/004_s1_store.sql"), StoreOpenMode::Takeover,
        read(src / "db/migrations/005_worker_tools.sql"), read(src / "db/migrations/006_event_runtime.sql"), read(src / "db/migrations/007_workflow_runtime.sql"),
        read(src / "db/migrations/008_acceptance_runtime.sql"), read(src / "db/migrations/009_owned_cleanup.sql")); };
    auto store = open();
    auto catalog = load_config_catalog(src / "config/examples/system.yaml", src / "config/examples/functions", src / "prompts");
    AdmissionInput in; in.tenant_id = "t"; in.project_id = "p"; in.root_id = "root"; in.function_id = "general";
    in.idempotency_key = "reopen"; in.original_text = "preserve original input";
    in.config_snapshot = catalog.freeze("general").json(); in.tool_allowlist = {"fs_write"};
    in.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", "preserve original input"}, {"source_revision", 1}}};
    const auto rid = s(store->admit(in), "request_id");
    auto actor = [&](Role r, const std::string& team = {}) { return store->host_actor(r, rid, team); };
    const Json brief = Object{{"goal", "g"}, {"constraint_refs", Array{"c1"}}, {"constraints_verbatim", Array{"preserve original input"}}};
    store->publish_plan(actor(Role::Dispatcher), n(store->request(rid), "state_version"), 1, Object{{"goal", "p"}}, "plan-1");
    const auto task = store->create_task(actor(Role::Dispatcher), n(store->request(rid), "state_version"), 1, 1, "node", brief, "task-1");
    const auto tid = s(task, "task_id"), team = s(task, "team_id");
    auto tv = [&] { return n(store->task(tid), "state_version"); };
    store->issue_instruction(actor(Role::TeamLead, team), store->binding(tid), tv(), brief, "order-1");
    store->acknowledge(actor(Role::Worker, team), store->binding(tid), tv(), "ack-1");
    auto op = store->prepare_operation(actor(Role::Worker, team), store->binding(tid), tv(), "k1", "fs_write", Object{{"a", 1}}, "workspace", "p1");
    std::cout << "before reopen: business prepare OK (op " << s(op, "status") << ")\n";
    store.reset(); store = open();   // e.g. the next `wls` CLI command (any writer)
    const auto t = store->tasks(rid).front();
    std::cout << "after reopen: execution_epoch=" << n(t, "execution_epoch") << " paused_for_change=" << n(t, "paused_for_change")
              << " instruction_revision=" << n(t, "instruction_revision") << " applied=" << n(t, "applied_instruction_revision") << "\n";
    try { store->acknowledge(actor(Role::Worker, team), store->binding(tid), tv(), "ack-2"); std::cout << "re-ack OK\n"; }
    catch (const Error& e) { std::cout << "re-ack of existing instruction -> " << error_name(e.code()) << ": " << e.what() << "\n"; }
    try { store->prepare_operation(actor(Role::Worker, team), store->binding(tid), tv(), "k2", "fs_write", Object{{"a", 2}}, "workspace", "p2"); std::cout << "business OK\n"; }
    catch (const Error& e) { std::cout << "business prepare -> " << error_name(e.code()) << ": " << e.what() << "\n"; }
    store->issue_instruction(actor(Role::TeamLead, team), store->binding(tid), tv(), brief, "order-2");
    store->acknowledge(actor(Role::Worker, team), store->binding(tid), tv(), "ack-3");
    store->prepare_operation(actor(Role::Worker, team), store->binding(tid), tv(), "k3", "fs_write", Object{{"a", 3}}, "workspace", "p3");
    std::cout << "only after a NEW TeamLead instruction (revision " << n(store->task(tid), "instruction_revision") << ") + ack does business resume\n";
    return 0;
}
