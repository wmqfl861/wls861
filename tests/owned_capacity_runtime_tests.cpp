#include "wls/cleanup_runtime.hpp"
#include "wls/delivery.hpp"
#include "wls/event_runtime.hpp"
#include "wls/owned_capacity.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <csignal>
#include <fstream>
#include <memory>
#include <sqlite3.h>
#include <sys/wait.h>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif
namespace {
using namespace wls;
using namespace wls::s1;
using O = boost::json::object;
using A = boost::json::array;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::string base_sql, store_sql, tools_sql, events_sql, workflow_sql, acceptance_sql, cleanup_sql, allocation_sql;
std::unique_ptr<SchemaRegistry> schemas;
std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t num(const Json& value, std::string_view key) { return value.at(key).to_number<std::int64_t>(); }
Json policy(std::size_t entries = 3, std::uint64_t bytes = 1024) {
    return O{{"version", "owned-capacity-v1"}, {"max_entries", entries},
        {"max_total_bytes", bytes}, {"max_inventory_bytes", 8388608}};
}
void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); CHECK(out.good());
}
void sql(const std::filesystem::path& path, const char* statement) {
    sqlite3* db = nullptr; CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    const auto result = sqlite3_exec(db, statement, nullptr, nullptr, nullptr); sqlite3_close(db);
    CHECK(result == SQLITE_OK);
}
std::int64_t scalar(const std::filesystem::path& path, const char* statement) {
    sqlite3* db = nullptr; CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* query = nullptr; CHECK(sqlite3_prepare_v2(db, statement, -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW); const auto result = sqlite3_column_int64(query, 0);
    sqlite3_finalize(query); sqlite3_close(db); return result;
}
void rejects(ErrorCode code, const std::function<void()>& action) {
    try { action(); }
    catch (const Error& error) {
        if (error.code() != code) throw std::runtime_error(std::string("wrong error: ") + error.what());
        return;
    }
    throw std::runtime_error("expected rejection was not thrown");
}
struct Fixture {
    test::TempDir temp;
    std::filesystem::path db{temp.path / "state.sqlite"}, parent{temp.path / "owned"}, work;
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> tools;
    Json capacity, system, frozen, resource, delivery;
    std::string request_id, task_id;
    EventFakeScript script;

    explicit Fixture(Json selected = policy()) : capacity(std::move(selected)) {
        std::filesystem::create_directory(parent); std::filesystem::create_directory(temp.path / "delivery");
        system = load_config_catalog(source / "config/examples/system.yaml", source / "config/examples/functions", source / "prompts").freeze("general").json().at("system");
        system.at("cleanup").as_object()["enabled"] = true;
        frozen = freeze(capacity);
        open(); request_id = admit(frozen, "original");
        script = original_script();
        EventPumpOptions first; first.max_calls = 1; first.max_worker_calls_per_task = 2;
        (void)EventRuntime(*store, *schemas, {}).pump(request_id, script, first);
        task_id = text(store->tasks(request_id).at(0), "task_id");
        std::erase_if(script.default_steps, [](const auto& step) { return step.role == Role::Dispatcher && step.role_call_index == 1; });
        for (auto& step : script.default_steps) if (step.role == Role::Dispatcher) --step.role_call_index;
    }
    static EventFakeScript original_script() {
        return event_fake_script_from_json(parse_json_strict(read_runtime_file(source / "tests/fixtures/s15_fake_complete.json")));
    }
    Json freeze(const Json& selected) {
        if (selected.is_null()) system.at("cleanup").as_object().erase("owned_workspace_capacity");
        else system.at("cleanup").as_object()["owned_workspace_capacity"] = selected;
        write_file(temp.path / "system.json", canonical_json(system));
        return load_config_catalog(temp.path / "system.json", source / "config/examples/functions", source / "prompts").freeze("general").json();
    }
    std::string admit(const Json& config, const std::string& key) {
        AdmissionInput input;
        input.tenant_id = "fixture"; input.project_id = "owned-capacity"; input.root_id = "root";
        input.function_id = "general"; input.idempotency_key = key;
        input.original_text = read_runtime_file(source / "tests/fixtures/s14_request.txt"); input.config_snapshot = config;
        input.constraint_catalog = parse_json_strict(read_runtime_file(source / "tests/fixtures/s14_constraints.json"));
        const auto restored = restore_request_config(O{{"config_snapshot", config}, {"config_snapshot_sha256", sha256_hex(canonical_json(config))}});
        const auto grants = restored.worker_tools(); input.tool_allowlist.assign(grants.begin(), grants.end());
        input.budget = {64, 20000000, 1000000, 0};
        return text(store->admit(input), "request_id");
    }
    void open() {
        tools.reset(); store.reset();
        store = std::make_unique<Store>(db, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql,
            workflow_sql, acceptance_sql, cleanup_sql, StoreRecoveryScope{}, allocation_sql);
    }
    StoreActor root() const { return store->host_actor(Role::Root, request_id); }
    StoreActor worker() const { return store->host_actor(Role::Worker, request_id, store->binding(task_id).team_id); }
    Json current() const { return store->active_owned_workspaces(request_id).at(0); }
    void allocate() {
        resource = allocate_owned_workspace(*store, request_id, task_id, parent, "allocation");
        work = text(resource.at("identity").at("workspace_identity"), "path");
    }
    void ready() {
        allocate(); tools = std::make_unique<WorkerTools>(*store, work);
        EventPumpOptions first; first.max_calls = 1;
        (void)EventRuntime(*store, *schemas, {{"alpha", tools.get()}}).pump(request_id, script, first);
        tools->bind(worker());
    }
    Json execute(const std::string& name, const Json& arguments) {
        return tools->execute(worker(), store->binding(task_id), num(store->task(task_id), "state_version"),
            new_id("key"), name, arguments, new_id("cmd"));
    }
    void delivered() {
        allocate(); tools = std::make_unique<WorkerTools>(*store, work);
        EventPumpOptions options; options.max_calls = 12; options.max_worker_calls_per_task = 2;
        const auto result = EventRuntime(*store, *schemas, {{"alpha", tools.get()}}).pump(request_id, script, options);
        if (store->task(task_id).at("status") != "ACCEPTED") throw std::runtime_error(canonical_json(result));
        CHECK(store->experience_jobs(request_id).at(0).at("status") == "DONE");
        CHECK(result.at("real_model_calls") == 0); tools.reset();
        DeliveryDirectory target(temp.path / "delivery");
        delivery = store->prepare_delivery(root(), store->request_binding(request_id), num(store->request(request_id), "state_version"), target.root_identity(), "prepare-delivery");
        const auto published = target.publish(delivery.at("frozen_plan")); CHECK(published.status == DeliveryMutationStatus::Succeeded);
        delivery = store->record_delivery_publication(root(), store->request_binding(request_id), text(delivery, "delivery_id"),
            num(delivery, "state_version"), published.publication, "publish-delivery");
        delivery = store->acknowledge_delivery(root(), store->request_binding(request_id), text(delivery, "delivery_id"),
            text(delivery, "manifest_sha256"), num(delivery, "requirements_revision"), num(delivery, "plan_revision"),
            "fixture-recipient", "durable_local", "receipt", published.publication);
    }
    Json publication() const { return DeliveryDirectory(temp.path / "delivery").verify_and_sync_existing(delivery.at("frozen_plan")); }
    Json begin() {
        const auto saved = current();
        const auto inventory = saved.at("inventory").is_null() ? inventory_owned_workspace(saved) : saved.at("inventory");
        return store->begin_workspace_cleanup(root(), publication(), A{inventory});
    }
};

void allocation_and_file_tools_share_limits() {
    Fixture f(policy(2, 8)); f.ready();
    CHECK(f.resource.at("intent").at("capacity_policy") == f.capacity);
    const auto binding = f.store->workspace(f.worker().team_id);
    CHECK(binding.at("execution_policy").at("owned_workspace_capacity") == f.capacity);
    CHECK(binding.at("execution_policy").at("workspace_limits").at("max_entries") == 2);
    CHECK(binding.at("execution_policy").at("workspace_limits").at("max_total_bytes") == 8);
    CHECK(f.execute("fs_write", O{{"path", "full"}, {"content", "12345678"}}).at("operation").at("status") == "SUCCEEDED");
    CHECK(f.execute("fs_mkdir", O{{"path", "dir"}}).at("operation").at("status") == "SUCCEEDED");
    for (const auto& name : {"fs_mkdir", "fs_write"}) {
        Json arguments = O{{"path", "extra"}};
        if (std::string_view(name) == "fs_write") arguments.as_object()["content"] = "x";
        const auto failed = f.execute(name, arguments);
        CHECK(failed.at("operation").at("status") == "FAILED");
        CHECK(failed.at("execution_result").at("observed_outcome") == "FAILED");
    }
    CHECK(!std::filesystem::exists(f.work / "extra"));
    CHECK(read_runtime_file(f.work / "full") == "12345678");
    const auto patch = f.execute("fs_patch", O{{"path", "full"}, {"expected_sha256", sha256_hex("12345678")},
        {"offset", 0}, {"expected", "1"}, {"replacement", "99"}});
    CHECK(patch.at("operation").at("status") == "FAILED");
    CHECK(read_runtime_file(f.work / "full") == "12345678");
    CHECK(scalar(f.db, "SELECT count(*) FROM ops WHERE status='UNKNOWN'") == 0);
}

void run_is_refused_before_operation_or_effect() {
    Fixture f; f.ready();
    const auto operations = scalar(f.db, "SELECT count(*) FROM ops");
    const auto events = scalar(f.db, "SELECT count(*) FROM events");
    const auto declarations = f.tools->declarations(f.worker());
    CHECK(std::none_of(declarations.begin(), declarations.end(), [](const auto& tool) { return tool.name == "run"; }));
    CHECK(f.tools->environment_card(f.worker()).at("business_effect_scope") == "file-tools-only");
    rejects(ErrorCode::Unauthorized, [&] { (void)f.execute("run", O{{"argv", A{"/bin/true"}}}); });
    CHECK(scalar(f.db, "SELECT count(*) FROM ops") == operations);
    CHECK(scalar(f.db, "SELECT count(*) FROM events") == events);
    CHECK(std::filesystem::is_empty(f.work));
}

void configuration_updates_only_new_requests() {
    Fixture f(policy(3)); f.ready();
    const auto original_snapshot = canonical_json(f.store->request(f.request_id).at("config_snapshot"));
    const auto original_intent = canonical_json(f.current().at("intent"));
    const auto original_binding = f.store->workspace(f.worker().team_id);
    const auto changed = f.freeze(policy(1));
    WorkerTools restarted(*f.store, f.work); restarted.bind(f.worker());
    CHECK(f.store->workspace(f.worker().team_id) == original_binding);
    CHECK(restarted.environment_card(f.worker()).at("owned_workspace_capacity").at("max_entries") == 3);
    const auto other = f.admit(changed, "new-config");
    EventPumpOptions first; first.max_calls = 1;
    (void)EventRuntime(*f.store, *schemas, {}).pump(other, Fixture::original_script(), first);
    const auto task = text(f.store->tasks(other).at(0), "task_id");
    const auto allocated = allocate_owned_workspace(*f.store, other, task, f.parent, "allocation-new-config");
    CHECK(allocated.at("intent").at("capacity_policy").at("max_entries") == 1);
    CHECK(canonical_json(f.store->request(f.request_id).at("config_snapshot")) == original_snapshot);
    CHECK(canonical_json(f.current().at("intent")) == original_intent);
}

void invalid_new_policy_has_no_store_or_directory_effect() {
    Fixture f;
    const auto requests = scalar(f.db, "SELECT count(*) FROM requests");
    const auto events = scalar(f.db, "SELECT count(*) FROM events");
    auto invalid = policy(); invalid.as_object()["max_entries"] = 4096;
    rejects(ErrorCode::InvalidInput, [&] { (void)f.freeze(invalid); });
    CHECK(scalar(f.db, "SELECT count(*) FROM requests") == requests);
    CHECK(scalar(f.db, "SELECT count(*) FROM events") == events);
    CHECK(std::filesystem::is_empty(f.parent));
}

void prepared_allocation_recovery_keeps_capacity() {
    Fixture f;
    auto resource = f.store->prepare_owned_workspace(f.root(), f.task_id, allocation_parent_identity(f.parent), "allocation");
    const auto intent = resource.at("intent");
    const auto receipt = stage_owned_workspace(intent);
    resource = f.store->record_owned_allocation_receipt(f.root(), text(resource, "resource_id"), num(resource, "state_version"), receipt);
    CHECK(resource.at("status") == "PREPARED");
    (void)f.freeze(policy(1)); f.open(); f.allocate();
    CHECK(f.resource.at("intent") == intent && f.resource.at("allocation_receipt") == receipt);
    CHECK(f.resource.at("identity").at("workspace_identity").at("inode") == receipt.at("workspace_identity").at("inode"));
    CHECK(f.resource.at("intent").at("capacity_policy") == f.capacity);
}

void inventory_reopen_uses_frozen_limits_and_bytes() {
    Fixture f; f.delivered(); write_file(f.work / "a", "a"); write_file(f.work / "z", "z");
    const auto snapshot = canonical_json(f.store->request(f.request_id).at("config_snapshot"));
    CHECK(cleanup_owned_workspaces(*f.store, f.request_id, 1).at("workspace_cleanup_recorded") == false);
    const auto inventory = canonical_json(f.current().at("inventory"));
    CHECK(parse_json_strict(inventory).at("entries").as_array().size() == 3);
    (void)f.freeze(policy(1)); f.open();
    CHECK(cleanup_owned_workspaces(*f.store, f.request_id).at("workspace_absence_verified_now") == true);
    CHECK(canonical_json(f.current().at("inventory")) == inventory);
    CHECK(canonical_json(f.store->request(f.request_id).at("config_snapshot")) == snapshot);
    CHECK(f.current().at("proof").at("inventory_sha256") == Json(sha256_hex(inventory)));
}

void legacy_sqlite_inventory_is_not_retagged() {
    Fixture f(nullptr); f.delivered(); write_file(f.work / "other", "old"); (void)f.begin();
    const auto inventory = canonical_json(f.current().at("inventory"));
    const auto intent = canonical_json(f.current().at("intent"));
    const auto snapshot = canonical_json(f.store->request(f.request_id).at("config_snapshot"));
    CHECK(!f.current().at("inventory").as_object().contains("capacity_policy"));
    (void)f.freeze(policy(1)); f.open();
    CHECK(cleanup_owned_workspaces(*f.store, f.request_id).at("workspace_absence_verified_now") == true);
    CHECK(canonical_json(f.current().at("inventory")) == inventory);
    CHECK(canonical_json(f.current().at("intent")) == intent);
    CHECK(canonical_json(f.store->request(f.request_id).at("config_snapshot")) == snapshot);
}

void legacy_overflow_returns_explicit_retention() {
    Fixture f(nullptr); f.delivered();
    for (std::size_t i = 0; i < 4096; ++i) write_file(f.work / ("extra-" + std::to_string(i)), "");
    const auto saved = f.current(); const auto snapshot = f.store->request(f.request_id).at("config_snapshot");
    const auto result = cleanup_owned_workspaces(*f.store, f.request_id);
    CHECK(result.at("capacity_exit").at("status") == "RETAINED_CAPACITY_EXCEEDED");
    CHECK(result.at("capacity_exit").at("original_capacity_policy").is_null());
    CHECK(result.at("capacity_exit").at("inventory_committed_this_pass") == false);
    CHECK(result.at("capacity_exit").at("deletion_performed_this_pass") == false);
    CHECK(result.at("pass_removed_entries") == 0 && result.at("workspace_absence_verified_now") == false);
    CHECK(f.current() == saved && f.current().at("inventory").is_null());
    CHECK(f.store->request(f.request_id).at("config_snapshot") == snapshot);
    CHECK(scalar(f.db, "SELECT count(*) FROM workspace_cleanup_runs") == 0);
    CHECK(std::filesystem::exists(f.work / "result.txt") && std::filesystem::exists(f.work / "extra-4095"));
}

void host_overflow_cannot_expand_new_cleanup_authority() {
    Fixture f(policy(1)); f.delivered(); write_file(f.work / "a", "a"); write_file(f.work / "z", "z");
    const auto result = cleanup_owned_workspaces(*f.store, f.request_id);
    CHECK(result.at("capacity_exit").at("original_capacity_policy") == f.capacity);
    CHECK(result.at("pass_removed_entries") == 0);
    CHECK(f.current().at("inventory").is_null());
    CHECK(scalar(f.db, "SELECT count(*) FROM workspace_cleanup_runs") == 0);
    CHECK(std::filesystem::exists(f.work / "result.txt"));
}

void store_refuses_policy_changed_inventory() {
    Fixture f; f.delivered(); auto inventory = inventory_owned_workspace(f.current());
    inventory.at("capacity_policy").as_object()["max_entries"] = 4095;
    rejects(ErrorCode::Conflict, [&] { (void)f.store->begin_workspace_cleanup(f.root(), f.publication(), A{inventory}); });
    CHECK(f.current().at("inventory").is_null());
    CHECK(scalar(f.db, "SELECT count(*) FROM workspace_cleanup_runs") == 0);
    CHECK(std::filesystem::exists(f.work / "result.txt"));
}

void replacement_after_frozen_inventory_is_retained() {
    Fixture f; f.delivered(); (void)f.begin();
    const auto inventory = canonical_json(f.current().at("inventory"));
    const auto bytes = read_runtime_file(f.work / "result.txt");
    std::filesystem::rename(f.work / "result.txt", f.temp.path / "original"); write_file(f.work / "result.txt", bytes);
    f.open(); rejects(ErrorCode::Conflict, [&] { (void)cleanup_owned_workspaces(*f.store, f.request_id); });
    CHECK(canonical_json(f.current().at("inventory")) == inventory);
    CHECK(std::filesystem::exists(f.work / "result.txt") && std::filesystem::exists(f.temp.path / "original"));
}

void interrupted_unlink_reuses_original_sqlite_inventory() {
    Fixture f; f.delivered(); write_file(f.work / "z", "z"); f.store.reset();
    const auto child = ::fork(); CHECK(child >= 0);
    if (child == 0) {
        try {
            f.open(); (void)f.begin(); const auto resource = f.current();
            (void)erase_owned_workspace(resource, resource.at("inventory"), 1);
            (void)::kill(::getpid(), SIGKILL);
        } catch (...) { ::_exit(7); }
        ::_exit(8);
    }
    int status = 0; CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    f.open(); const auto inventory = canonical_json(f.current().at("inventory"));
    CHECK(parse_json_strict(inventory).at("entries").as_array().size() == 2);
    CHECK(cleanup_owned_workspaces(*f.store, f.request_id).at("workspace_absence_verified_now") == true);
    CHECK(canonical_json(f.current().at("inventory")) == inventory);
}

void failed_inventory_commit_prevents_deletion() {
    Fixture f; f.delivered();
    sql(f.db, "CREATE TRIGGER fail_capacity_inventory BEFORE UPDATE ON owned_workspaces WHEN NEW.inventory IS NOT NULL BEGIN SELECT RAISE(ABORT,'fixture inventory commit failure'); END;");
    rejects(ErrorCode::Conflict, [&] { (void)cleanup_owned_workspaces(*f.store, f.request_id); });
    CHECK(f.current().at("inventory").is_null());
    CHECK(scalar(f.db, "SELECT count(*) FROM workspace_cleanup_runs") == 0);
    CHECK(std::filesystem::exists(f.work / "result.txt"));
    sql(f.db, "DROP TRIGGER fail_capacity_inventory;"); f.open();
    CHECK(cleanup_owned_workspaces(*f.store, f.request_id).at("workspace_absence_verified_now") == true);
}
} // namespace

int main() {
    if (::geteuid() == 0) { std::cerr << "owned capacity runtime tests require an ordinary uid\n"; return 2; }
    base_sql = read_runtime_file(source / "db/schema.sql"); store_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql"); events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    workflow_sql = read_runtime_file(source / "db/migrations/007_workflow_runtime.sql"); acceptance_sql = read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql");
    cleanup_sql = read_runtime_file(source / "db/migrations/009_owned_cleanup.sql"); allocation_sql = read_runtime_file(source / "db/migrations/010_owned_allocation_recovery.sql");
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    return test::run({
        {"allocation and real Worker file tools share frozen capacity", allocation_and_file_tools_share_limits},
        {"run is refused before operation preparation or filesystem effects", run_is_refused_before_operation_or_effect},
        {"configuration changes only new request capacity", configuration_updates_only_new_requests},
        {"invalid new policy has no Store or directory effects", invalid_new_policy_has_no_store_or_directory_effect},
        {"prepared v2 allocation recovery retains original capacity", prepared_allocation_recovery_keeps_capacity},
        {"partial cleanup and reopen keep original inventory and frozen limits", inventory_reopen_uses_frozen_limits_and_bytes},
        {"legacy SQLite inventory and snapshot never acquire new policy", legacy_sqlite_inventory_is_not_retagged},
        {"legacy over-capacity tree returns explicit retention without cleanup commit", legacy_overflow_returns_explicit_retention},
        {"host overflow cannot expand new cleanup authority", host_overflow_cannot_expand_new_cleanup_authority},
        {"Store rejects capacity-retagged inventory before deletion claim", store_refuses_policy_changed_inventory},
        {"replaced inode after inventory is retained across reopen", replacement_after_frozen_inventory_is_retained},
        {"SIGKILL after unlink reuses original SQLite inventory", interrupted_unlink_reuses_original_sqlite_inventory},
        {"failed inventory transaction leaves the complete tree", failed_inventory_commit_prevents_deletion}});
}
