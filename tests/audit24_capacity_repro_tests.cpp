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
void sustained_calls_and_control_traffic() {
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
    for (int i = 0; i < 1100; ++i) {
        if (i > 0 && i % 150 == 0) {
            store.issue_instruction(actor(Role::TeamLead, team), store.binding(tid), tv(), brief, "order-" + std::to_string(i));
            store.acknowledge(actor(Role::Worker, team), store.binding(tid), tv(), "ack-" + std::to_string(i));
        }
        const auto op = store.prepare_operation(actor(Role::Worker, team), store.binding(tid), tv(), "key-" + std::to_string(i),
            "fs_write", Object{{"i", i}}, "workspace", "prep-" + std::to_string(i));
        const auto started = store.start_operation(actor(Role::Worker, team), s(op, "op_id"), n(op, "state_version"), "start-" + std::to_string(i));
        store.finish_operation(actor(Role::Worker, team), s(op, "op_id"), store.binding(tid), n(started, "state_version"), "SUCCEEDED", "synthetic Store result", "fin-" + std::to_string(i));
    }
    // Controls must not consume the business pool, even above the old shared
    // 4096 rows. Each pair is legitimate, current wait/wake control traffic.
    for (int i = 0; i < 1800; ++i) {
        const auto w = store.wait_worker(actor(Role::Worker, team), store.binding(tid), tv(), "explicit fixture wait", "w-" + std::to_string(i));
        store.wake_worker(actor(Role::Worker, team), store.binding(tid), n(w, "generation"), "wake-" + std::to_string(i));
    }
    const auto plan = store.publish_plan(actor(Role::Dispatcher), n(store.request(rid), "state_version"), 1, Object{{"goal", "still permitted"}}, "plan-after-controls");
    (void)plan;
    CHECK(store.request(rid).at("plan_revision") == 2);
}
int main() { return test::run({{"old event-cap reproduction: 1100 operations plus 3600 independent controls", sustained_calls_and_control_traffic}}); }
