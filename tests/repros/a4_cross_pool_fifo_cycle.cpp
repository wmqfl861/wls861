// Reuse the exact registered case, fixtures and original five-second barriers.
// Link only with the selected version's own headers, libraries and Store assets.
#define main a4_original_async_suite_main
#include "../async_coordinator_tests.cpp"
#undef main

int main() {
    schemas = std::make_shared<SchemaRegistry>(source / "schemas");
    base_sql = read_runtime_file(source / "db/schema.sql");
    store_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    workflow_sql = read_runtime_file(source / "db/migrations/007_workflow_runtime.sql");
    acceptance_sql = read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql");
    return test::run({{"cross-pool resource heads progress before slot admission",
        cross_pool_fifo_resource_heads_progress_before_slot_admission}});
}
