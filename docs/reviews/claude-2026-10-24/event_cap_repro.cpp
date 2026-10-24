// Reproduction: hard-coded per-request event capacity (4096) blocks
// non-control progress long before configured limits are exhausted.
#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "test_support.hpp"
#include <fstream>
#include <iterator>
#include <iostream>
using namespace wls::s1; using wls::Role;
using Object = boost::json::object; using Array = boost::json::array;
static std::string read(const std::filesystem::path& p) { std::ifstream s(p, std::ios::binary); return {std::istreambuf_iterator<char>(s), {}}; }
static std::string s(const Json& v, std::string_view k) { return std::string(v.at(k).as_string()); }
static std::int64_t n(const Json& v, std::string_view k) { return v.at(k).as_int64(); }
int main() {
    const std::filesystem::path src = WLS_SOURCE_DIR;
    test::TempDir dir;
    Store store(dir.path / "state.sqlite", read(src / "db/schema.sql"), read(src / "db/migrations/004_s1_store.sql"), StoreOpenMode::Takeover, read(src / "db/migrations/005_worker_tools.sql"), read(src / "db/migrations/006_event_runtime.sql"), read(src / "db/migrations/007_workflow_runtime.sql"), read(src / "db/migrations/008_acceptance_runtime.sql"), read(src / "db/migrations/009_owned_cleanup.sql"));
    auto catalog = load_config_catalog(src / "config/examples/system.yaml", src / "config/examples/functions", src / "prompts");
    AdmissionInput in; in.tenant_id = "t"; in.project_id = "p"; in.root_id = "root"; in.function_id = "general";
    in.idempotency_key = "cap"; in.original_text = "preserve original input";
    in.config_snapshot = catalog.freeze("general").json(); in.tool_allowlist = {"fs_write"};
    in.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", "preserve original input"}, {"source_revision", 1}}};
    const auto rid = s(store.admit(in), "request_id");
    auto actor = [&](Role r, const std::string& team = {}) { return store.host_actor(r, rid, team); };
    const Json brief = Object{{"goal", "g"}, {"constraint_refs", Array{"c1"}}, {"constraints_verbatim", Array{"preserve original input"}}};
    store.publish_plan(actor(Role::Dispatcher), n(store.request(rid), "state_version"), 1, Object{{"goal", "p"}}, "plan-1");
    const auto task = store.create_task(actor(Role::Dispatcher), n(store.request(rid), "state_version"), 1, 1, "node", brief, "task-1");
    const auto tid = s(task, "task_id"), team = s(task, "team_id");
    auto tv = [&] { return n(store.task(tid), "state_version"); };
    store.issue_instruction(actor(Role::TeamLead, team), store.binding(tid), tv(), brief, "order-1");
    store.acknowledge(actor(Role::Worker, team), store.binding(tid), tv(), "ack-1");
    // Realistic mix: each Worker business tool call writes 3 non-control
    // (reserve_attempt, prepare_op, start_tool_op) and 3 control events.
    // Here: prepare/start (non-control) + finish (control) per op; new order every 150 ops.
    std::int64_t ops = 0; std::string failure;
    for (int i = 0; i < 5000; ++i) {
        try {
            if (ops > 0 && ops % 150 == 0) {
                store.issue_instruction(actor(Role::TeamLead, team), store.binding(tid), tv(), brief, "order-" + std::to_string(i));
                store.acknowledge(actor(Role::Worker, team), store.binding(tid), tv(), "ack-" + std::to_string(i));
            }
            const auto op = store.prepare_operation(actor(Role::Worker, team), store.binding(tid), tv(), "key-" + std::to_string(i),
                                                    "fs_write", Object{{"i", i}}, "workspace", "prep-" + std::to_string(i));
            const auto started = store.start_operation(actor(Role::Worker, team), s(op, "op_id"), n(op, "state_version"), "start-" + std::to_string(i));
            store.finish_operation(actor(Role::Worker, team), s(op, "op_id"), store.binding(tid), n(started, "state_version"), "SUCCEEDED", "ok", "fin-" + std::to_string(i));
            ++ops;
        } catch (const Error& e) { failure = std::string(error_name(e.code())) + ": " + e.what(); break; }
    }
    const auto events = store.events(rid, 0, 1000).size();
    std::cout << "ops_completed=" << ops << " first_failure=\"" << failure << "\"\n";
    std::cout << "configured worker_max_tool_calls_per_order=200, max_teams_per_request=4, team_internal_rework_max=3\n";
    // Pad with control events (as settle/feedback/result would in a real run) until the shared counter reaches 4096.
    std::int64_t wait_gen = 0; int pads = 0;
    for (;;) {
        try {
            const auto w = store.wait_worker(actor(Role::Worker, team), store.binding(tid), tv(), "pad", "w-" + std::to_string(pads));
            wait_gen = n(w, "generation");
            store.wake_worker(actor(Role::Worker, team), store.binding(tid), wait_gen, "k-" + std::to_string(pads));
            ++pads;
            // Stop when the next non-control event would be refused.
            try { store.issue_instruction(actor(Role::TeamLead, team), store.binding(tid), tv(), brief, "probe-" + std::to_string(pads));
                  store.acknowledge(actor(Role::Worker, team), store.binding(tid), tv(), "probe-ack-" + std::to_string(pads)); }
            catch (const Error& e) { std::cout << "after " << pads << " control pads, non-control instruction -> " << error_name(e.code()) << ": " << e.what() << "\n"; break; }
        } catch (const Error& e) { std::cout << "pad failed: " << e.what() << "\n"; break; }
    }
    try { store.publish_plan(actor(Role::Dispatcher), n(store.request(rid), "state_version"), 1, Object{{"goal", "replan"}}, "plan-2");
          std::cout << "publish_plan OK\n"; }
    catch (const Error& e) { std::cout << "publish_plan (non-control) -> " << error_name(e.code()) << ": " << e.what() << "\n"; }
    try { store.cancel_request(actor(Role::Root), n(store.request(rid), "state_version"), "cancel");
          std::cout << "cancel_request (control) -> OK, status=" << s(store.request(rid), "status") << "\n"; }
    catch (const Error& e) { std::cout << "cancel_request -> " << e.what() << "\n"; }
    (void)events; return 0;
}
