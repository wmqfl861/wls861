// Standalone old/new capability probe; not another registered product suite.
// Build once against the original 410 library without WLS_EVENT_LINK_PROBE_411,
// then against 411 with it. The old public finish contract intentionally lacks
// an event/call association; its expected red is a capability gap, not a claim
// that a previously documented legacy behavior was a production regression.
#include "wls/offline_runtime.hpp"
#include "wls/migrations.hpp"
#include "wls/runtime_resources.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
using namespace wls;
using namespace wls::s1;
const std::filesystem::path source = std::filesystem::absolute(WLS_SOURCE_DIR);

std::int64_t count(const std::filesystem::path& path, const char* statement) {
    sqlite3* db{};
    CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* query{};
    CHECK(sqlite3_prepare_v2(db, statement, -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW);
    const auto result = sqlite3_column_int64(query, 0);
    sqlite3_finalize(query); sqlite3_close(db);
    return result;
}

void unproven_finish_is_refused_under_explicit_persistent_policy() {
    test::TempDir directory;
    const auto path = directory.path / "probe.sqlite";
    const auto resources = RuntimeResources::from_source_tree(source);
#ifdef WLS_EVENT_LINK_PROBE_411
    Store store(path, MigrationRegistry(resources, 411));
#else
    Store store(path, MigrationRegistry(resources, 410));
#endif
    const auto config = load_config_catalog(source / "config/examples/system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general");
    AdmissionInput input;
    input.tenant_id = "probe-tenant"; input.project_id = "probe-project"; input.root_id = "probe-root";
    input.function_id = config.function_id(); input.idempotency_key = "original-admission";
    input.original_text = read_runtime_file(source / "tests/fixtures/s1_request.txt");
    input.constraint_catalog = parse_json_strict(read_runtime_file(source / "tests/fixtures/s1_constraints.json"));
    input.config_snapshot = config.json();
    const auto grants = config.worker_tools(); input.tool_allowlist.assign(grants.begin(), grants.end());
    const auto admitted = store.admit(input);
    const std::string id(admitted.at("request_id").as_string());
#ifdef WLS_EVENT_LINK_PROBE_411
    (void)store.enable_event_driver(store.host_actor(Role::Root, id), {}, "enable-probe");
#endif
    const auto actor = store.host_actor(Role::Dispatcher, id);
#ifdef WLS_EVENT_LINK_PROBE_411
    const auto claim = store.claim_event_batch(actor);
#else
    const auto claim = store.claim_events(actor);
#endif
    CHECK(claim.at("items").as_array().size() == 1);
    const auto original_request = store.request_snapshot(id);
    const auto original_budget = store.budget(id);
    const auto original_events = store.events(id, 0, 1000);
    std::vector<std::int64_t> ids;
    for (const auto& item : claim.at("items").as_array()) ids.push_back(item.at("inbox_id").as_int64());
    bool refused = false;
    try {
        (void)store.finish_events(actor, std::string(claim.at("claim_id").as_string()),
            claim.at("generation").as_int64(), ids);
    } catch (const Error& error) { CHECK(error.code() == ErrorCode::Unsupported); refused = true; }
    const auto attempts = count(path, "SELECT count(*) FROM call_attempts");
    const auto feedback = count(path, "SELECT count(*) FROM model_tool_feedback");
    const auto done = count(path, "SELECT count(*) FROM event_inbox WHERE status='DONE'");
    std::cout << "PROBE mode="
#ifdef WLS_EVENT_LINK_PROBE_411
              << "411-enabled"
#else
              << "410-legacy"
#endif
              << " refused=" << refused << " attempts=" << attempts << " feedback=" << feedback
              << " done=" << done << std::endl;
    CHECK(attempts == 0 && feedback == 0);
    CHECK(store.request_snapshot(id) == original_request && store.budget(id) == original_budget);
    CHECK(store.events(id, 0, 1000) == original_events);
    CHECK(refused && done == 0);
}
} // namespace

int main() { return test::run({
    {"explicit persistent policy refuses legacy ID completion without a call proof", unproven_finish_is_refused_under_explicit_persistent_policy}
}); }
