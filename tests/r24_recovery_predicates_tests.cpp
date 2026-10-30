// R2.4 predicate coverage. Every fact originates in a disposable real Store.
// Four legacy states have no current public producer: CANCEL_REQUESTED,
// orphan CLAIMED inbox, FAILED experience and FAILED cleanup. The narrowly
// identified SQL injections below change only those state/CAS fields, with all
// original schema checks/triggers enabled. They do not manufacture acceptance,
// process exit, model settlement, ownership or a successful cleanup.
#include "wls/cleanup.hpp"
#include "wls/cleanup_runtime.hpp"
#include "wls/delivery.hpp"
#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <memory>
#include <sqlite3.h>
#include <string_view>

namespace {
using namespace wls;
using namespace wls::s1;
using Object = boost::json::object;
using Array = boost::json::array;
using Snapshot = std::map<std::string, std::vector<std::string>>;
const std::filesystem::path source = WLS_SOURCE_DIR;

std::string text(const Json& value, std::string_view key) {
    return std::string(value.at(key).as_string());
}
std::int64_t num(const Json& value, std::string_view key) {
    return value.at(key).to_number<std::int64_t>();
}
void rejected(ErrorCode expected, const std::function<void()>& action) {
    try { action(); }
    catch (const Error& error) {
        if (error.code() != expected)
            throw std::runtime_error(std::string("wrong rejection: ") + error.what());
        return;
    }
    throw std::runtime_error("expected rejection");
}

// Preserve SQLite type, length and every byte, including non-UTF-8 BLOBs.
class Database {
public:
    explicit Database(const std::filesystem::path& path, bool writable = false) {
        sqlite3* raw = nullptr;
        const auto code = sqlite3_open_v2(path.c_str(), &raw,
            writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY, nullptr);
        db_.reset(raw);
        CHECK(code == SQLITE_OK);
        if (writable) CHECK(sqlite3_exec(db_.get(), "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
    Statement statement(const std::string& sql, const std::vector<std::string>& args = {}) const {
        sqlite3_stmt* raw = nullptr;
        const auto code = sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw, nullptr);
        Statement result(raw, sqlite3_finalize);
        if (code != SQLITE_OK) throw std::runtime_error(sql + ": " + sqlite3_errmsg(db_.get()));
        for (std::size_t i = 0; i < args.size(); ++i)
            CHECK(sqlite3_bind_text(raw, static_cast<int>(i + 1), args[i].c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
        return result;
    }
    std::vector<std::string> rows(const std::string& sql, const std::vector<std::string>& args = {}) const {
        auto query = statement(sql, args);
        std::vector<std::string> result;
        int code{};
        while ((code = sqlite3_step(query.get())) == SQLITE_ROW) {
            std::string row;
            for (int i = 0; i < sqlite3_column_count(query.get()); ++i) {
                const auto type = sqlite3_column_type(query.get(), i);
                const auto* bytes = static_cast<const unsigned char*>(sqlite3_column_blob(query.get(), i));
                const auto size = sqlite3_column_bytes(query.get(), i);
                row += std::to_string(type) + ':' + std::to_string(size) + ':';
                constexpr std::string_view hex = "0123456789abcdef";
                for (int j = 0; j < size; ++j) {
                    row += hex[static_cast<std::size_t>(bytes[j] >> 4U)];
                    row += hex[static_cast<std::size_t>(bytes[j] & 15U)];
                }
                row += '|';
            }
            result.push_back(std::move(row));
        }
        CHECK(code == SQLITE_DONE);
        return result;
    }
    std::int64_t count(const std::string& sql, const std::vector<std::string>& args = {}) const {
        auto query = statement(sql, args);
        CHECK(sqlite3_step(query.get()) == SQLITE_ROW);
        const auto value = sqlite3_column_int64(query.get(), 0);
        CHECK(sqlite3_step(query.get()) == SQLITE_DONE);
        return value;
    }
    void inject_one(const std::string& sql, const std::vector<std::string>& args) const {
        auto update = statement(sql, args);
        const auto code = sqlite3_step(update.get());
        if (code != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_.get()));
        CHECK(sqlite3_changes(db_.get()) == 1);
    }
    Snapshot snapshot(const std::string& request = {}) const {
        auto tables = statement("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
        Snapshot result;
        int code{};
        while ((code = sqlite3_step(tables.get())) == SQLITE_ROW) {
            const std::string table(reinterpret_cast<const char*>(sqlite3_column_text(tables.get(), 0)));
            if (!request.empty()) {
                auto columns = statement("PRAGMA table_info(\"" + table + "\")");
                bool scoped = false;
                int step{};
                while ((step = sqlite3_step(columns.get())) == SQLITE_ROW)
                    scoped = scoped || std::string_view(reinterpret_cast<const char*>(sqlite3_column_text(columns.get(), 1))) == "request_id";
                CHECK(step == SQLITE_DONE);
                if (!scoped) continue;
            }
            auto values = rows("SELECT * FROM \"" + table + "\"" + (request.empty() ? "" : " WHERE request_id=?"),
                               request.empty() ? std::vector<std::string>{} : std::vector<std::string>{request});
            std::sort(values.begin(), values.end());
            result.emplace(table, std::move(values));
        }
        CHECK(code == SQLITE_DONE);
        return result;
    }
    std::vector<std::string> preserved_columns(const std::string& table,
            const std::vector<std::string>& mutable_fields, const std::string& request) const {
        auto columns = statement("PRAGMA table_info(\"" + table + "\")");
        std::string fields;
        int code{};
        while ((code = sqlite3_step(columns.get())) == SQLITE_ROW) {
            const std::string name(reinterpret_cast<const char*>(sqlite3_column_text(columns.get(), 1)));
            if (std::find(mutable_fields.begin(), mutable_fields.end(), name) != mutable_fields.end()) continue;
            if (!fields.empty()) fields += ',';
            fields += '"' + name + '"';
        }
        CHECK(code == SQLITE_DONE && !fields.empty());
        auto values = rows("SELECT " + fields + " FROM \"" + table + "\" WHERE request_id=?", {request});
        std::sort(values.begin(), values.end());
        return values;
    }
private:
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, sqlite3_close};
};

struct Assets {
    test::TempDir config_dir;
    SchemaRegistry schemas{source / "schemas"};
    std::array<std::string, 7> sql{
        read_runtime_file(source / "db/schema.sql"),
        read_runtime_file(source / "db/migrations/004_s1_store.sql"),
        read_runtime_file(source / "db/migrations/005_worker_tools.sql"),
        read_runtime_file(source / "db/migrations/006_event_runtime.sql"),
        read_runtime_file(source / "db/migrations/007_workflow_runtime.sql"),
        read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql"),
        read_runtime_file(source / "db/migrations/009_owned_cleanup.sql")};
    FrozenRequestConfig config{make_config()};
    Json constraints = parse_json_strict(read_runtime_file(source / "tests/fixtures/s14_constraints.json"));
    EventFakeScript script{event_fake_script_from_json(parse_json_strict(read_runtime_file(source / "tests/fixtures/s15_fake_complete.json")))};
    FrozenRequestConfig make_config() {
        auto bytes = read_runtime_file(source / "config/examples/system.yaml");
        const std::string original = "cleanup:\n  enabled: false";
        const auto at = bytes.find(original); CHECK(at != std::string::npos);
        bytes.replace(at, original.size(), "cleanup:\n  enabled: true");
        const auto path = config_dir.path / "system.yaml";
        { std::ofstream output(path, std::ios::binary); output << bytes; CHECK(output.good()); }
        return load_config_catalog(path, source / "config/examples/functions", source / "prompts").freeze("general");
    }
};
Assets& assets() { static Assets value; return value; }
struct Node { std::string request, task, team; };

struct Fixture {
    test::TempDir directory;
    std::filesystem::path path{directory.path / "state.sqlite"};
    std::unique_ptr<Store> store;
    Node selected, sibling;
    Json delivery, resource;
    std::filesystem::path workspace;

    explicit Fixture(bool basic_selected = true) {
        open();
        selected.request = text(store->admit(input("selected")), "request_id");
        sibling.request = text(store->admit(input("sibling")), "request_id");
        ready(sibling);
        // A sibling really needs recovery, so an accidental all-request scan
        // cannot pass merely because every other request happens to be idle.
        (void)running(sibling);
        CHECK(!store->claim_events(actor(sibling, Role::Dispatcher), 1).at("items").as_array().empty());
        if (basic_selected) ready(selected);
    }
    void open(StoreRecoveryScope scope = {}) {
        store.reset();
        const auto& sql = assets().sql;
        store = std::make_unique<Store>(path, sql[0], sql[1], StoreOpenMode::Takeover,
            sql[2], sql[3], sql[4], sql[5], sql[6], std::move(scope));
    }
    void reopen() { open(StoreRecoveryScope::request(selected.request)); }
    AdmissionInput input(const std::string& key) const {
        AdmissionInput result;
        result.tenant_id = "predicate-fixture"; result.project_id = "project";
        result.root_id = "root"; result.function_id = "general"; result.idempotency_key = key;
        result.original_text = read_runtime_file(source / "tests/fixtures/s14_request.txt");
        result.constraint_catalog = assets().constraints; result.config_snapshot = assets().config.json();
        const auto grants = assets().config.worker_tools();
        result.tool_allowlist.assign(grants.begin(), grants.end());
        result.budget = {64, 20000000, 1000000, 0};
        return result;
    }
    StoreActor actor(const Node& node, Role role = Role::Worker) const {
        return store->host_actor(role, node.request,
            role == Role::Worker || role == Role::TeamLead ? node.team : "");
    }
    void ready(Node& node) {
        Array refs, verbatim;
        for (const auto& constraint : assets().constraints.as_array()) {
            refs.emplace_back(constraint.at("constraint_id")); verbatim.emplace_back(constraint.at("text"));
        }
        const Json brief = Object{{"goal", "preserve original recovery evidence"},
                                  {"constraint_refs", refs}, {"constraints_verbatim", verbatim}};
        store->publish_plan(actor(node, Role::Dispatcher), num(store->request(node.request), "state_version"), 1, brief, new_id("plan"));
        const auto task = store->create_task(actor(node, Role::Dispatcher), num(store->request(node.request), "state_version"),
                                              1, 1, "alpha", brief, new_id("task"));
        node.task = text(task, "task_id"); node.team = text(task, "team_id");
        store->issue_instruction(actor(node, Role::TeamLead), store->binding(node.task),
                                 num(store->task(node.task), "state_version"), brief, new_id("order"));
        acknowledge(node);
    }
    void acknowledge(const Node& node) {
        store->acknowledge(actor(node), store->binding(node.task), num(store->task(node.task), "state_version"), new_id("ack"));
    }
    Json running(const Node& node) {
        const auto prepared = store->prepare_operation(actor(node), store->binding(node.task),
            num(store->task(node.task), "state_version"), new_id("key"), "fs_write", Object{{"fixture", true}}, "workspace", new_id("prepare"));
        return store->start_operation(actor(node), text(prepared, "op_id"), num(prepared, "state_version"), new_id("start"));
    }
    void allocate() {
        CHECK(std::filesystem::create_directory(directory.path / "owned"));
        resource = allocate_owned_workspace(*store, selected.request, selected.task, directory.path / "owned", "owned-predicate-fixture");
        workspace = text(resource.at("identity").at("workspace_identity"), "path");
    }
    void accepted(bool report, bool owned) {
        auto script = assets().script;
        EventPumpOptions first; first.max_calls = 1;
        const auto planned = EventRuntime(*store, assets().schemas).pump(selected.request, script, first);
        CHECK(planned.at("real_model_calls") == 0);
        const auto task = store->tasks(selected.request).at(0);
        selected.task = text(task, "task_id"); selected.team = text(task, "team_id");
        if (owned) allocate();
        else { workspace = directory.path / "legacy"; CHECK(std::filesystem::create_directory(workspace)); }
        std::erase_if(script.default_steps, [](const auto& step) { return step.role == Role::Dispatcher && step.role_call_index == 1; });
        for (auto& step : script.default_steps) if (step.role == Role::Dispatcher) --step.role_call_index;
        if (!report) std::erase_if(script.by_node.at("alpha"), [](const auto& step) {
            return step.role == Role::TeamLead && step.role_call_index == 3;
        });
        WorkerTools tools(*store, workspace);
        EventPumpOptions options; options.max_calls = 12; options.max_worker_calls_per_task = 2;
        const auto result = EventRuntime(*store, assets().schemas, {{"alpha", &tools}}).pump(selected.request, script, options);
        if (store->task(selected.task).at("status") != "ACCEPTED") throw std::runtime_error(canonical_json(result));
        CHECK(result.at("real_model_calls") == 0);
        CHECK(store->experience_jobs(selected.request).size() == 1);
        CHECK(store->experience_jobs(selected.request).at(0).at("status") == (report ? "DONE" : "PENDING"));
    }
    void deliver() {
        CHECK(std::filesystem::create_directory(directory.path / "delivery"));
        DeliveryDirectory target(directory.path / "delivery");
        delivery = store->prepare_delivery(actor(selected, Role::Root), store->request_binding(selected.request),
            num(store->request(selected.request), "state_version"), target.root_identity(), "predicate-prepare-delivery");
        const auto publication = target.publish(delivery.at("frozen_plan"));
        CHECK(publication.status == DeliveryMutationStatus::Succeeded);
        delivery = store->record_delivery_publication(actor(selected, Role::Root), store->request_binding(selected.request),
            text(delivery, "delivery_id"), num(delivery, "state_version"), publication.publication, "predicate-publish");
        delivery = store->acknowledge_delivery(actor(selected, Role::Root), store->request_binding(selected.request),
            text(delivery, "delivery_id"), text(delivery, "manifest_sha256"), num(delivery, "requirements_revision"),
            num(delivery, "plan_revision"), "fixture-recipient", "durable_local", "predicate-receipt", publication.publication);
        CHECK(store->request(selected.request).at("status") == "DELIVERED");
    }
    Json publication() const {
        return DeliveryDirectory(directory.path / "delivery").verify_and_sync_existing(delivery.at("frozen_plan"));
    }
};

enum class Trigger { None, CancelRequested, OrphanInbox, FailedExperience, FailedCleanup };
void only_trigger(const Fixture& fixture, Trigger wanted) {
    const Database db(fixture.path);
    const std::vector<std::string> id{fixture.selected.request};
    const auto count = [&](const std::string& table, const std::string& condition) {
        return db.count("SELECT COUNT(*) FROM " + table + " WHERE request_id=? AND " + condition, id);
    };
    CHECK(count("ops", "status='PREPARED'") == 0);
    CHECK(count("ops", "status='RUNNING'") == 0);
    CHECK(count("ops", "status='CANCEL_REQUESTED'") == (wanted == Trigger::CancelRequested ? 1 : 0));
    CHECK(count("call_attempts", "status='PENDING'") == 0);
    CHECK(count("event_actor_claims", "status='ACTIVE'") == 0);
    CHECK(count("event_inbox", "status='CLAIMED'") == (wanted == Trigger::OrphanInbox ? 1 : 0));
    CHECK(count("experience_jobs", "status='RUNNING'") == 0);
    CHECK(count("experience_jobs", "status='FAILED'") == (wanted == Trigger::FailedExperience ? 1 : 0));
    CHECK(count("experience_jobs", "status<>'DONE' AND claimed_by IS NOT NULL") == 0);
    CHECK(count("cleanup_jobs", "status='RUNNING'") == 0);
    CHECK(count("cleanup_jobs", "status='FAILED'") == (wanted == Trigger::FailedCleanup ? 1 : 0));
    CHECK(count("cleanup_jobs", "status<>'DONE' AND claimed_by IS NOT NULL") == 0);
    CHECK(db.count("SELECT COUNT(*) FROM requests r WHERE request_id=? AND status IN ('FAILED','CANCELLED','SUPERSEDED') "
                   "AND NOT EXISTS(SELECT 1 FROM receipts p WHERE p.request_id=r.request_id)", id) == 0);
}

Snapshot persistent_business_facts(const Fixture& fixture) {
    const Database db(fixture.path);
    Snapshot result;
    result["requests"] = db.preserved_columns("requests", {"dispatcher_epoch", "state_version", "updated_at"}, fixture.selected.request);
    result["teams"] = db.preserved_columns("teams", {"execution_epoch", "paused_for_change", "state_version", "updated_at"}, fixture.selected.request);
    result["tasks"] = db.preserved_columns("tasks", {"applied_instruction_revision", "state_version", "updated_at"}, fixture.selected.request);
    for (const auto* table : {"experience_jobs", "cleanup_jobs"}) {
        result[table] = db.preserved_columns(table, {"status", "claimed_by", "lease_epoch", "state_version", "updated_at"}, fixture.selected.request);
        result[std::string(table) + "_done"] = db.rows(std::string("SELECT * FROM ") + table + " WHERE status='DONE' ORDER BY request_id");
    }
    return result;
}
struct Before {
    Snapshot all, sibling, business;
    std::vector<std::string> events;
    Json request, task;
    ExecutionBinding binding;
    explicit Before(const Fixture& fixture)
        : all(Database(fixture.path).snapshot()), sibling(Database(fixture.path).snapshot(fixture.sibling.request)),
          business(persistent_business_facts(fixture)),
          events(Database(fixture.path).rows("SELECT * FROM events ORDER BY seq")),
          request(fixture.store->request(fixture.selected.request)), task(fixture.store->task(fixture.selected.task)),
          binding(fixture.store->binding(fixture.selected.task)) {}
    void preserved(const Fixture& fixture, bool operation_changes = false) const {
        const Database db(fixture.path);
        CHECK(db.snapshot(fixture.sibling.request) == sibling);
        CHECK(persistent_business_facts(fixture) == business);
        const auto after = db.snapshot();
        for (const auto& [table, values] : all) {
            if (table == "requests" || table == "teams" || table == "tasks" || table == "events" ||
                table == "event_inbox" || table == "event_actor_claims" || table == "experience_jobs" ||
                table == "cleanup_jobs" || (operation_changes && table == "ops")) continue;
            CHECK(after.at(table) == values);
        }
        // This also checks original request text/configuration, attempts,
        // responses, feedback, receipts, operation results/output and budgets.
        const auto current_events = db.rows("SELECT * FROM events ORDER BY seq");
        CHECK(current_events.size() >= events.size());
        CHECK(std::equal(events.begin(), events.end(), current_events.begin()));
        auto old_request = request, current_request = fixture.store->request(fixture.selected.request);
        for (const auto* field : {"dispatcher_epoch", "state_version", "updated_at"}) {
            old_request.as_object().erase(field); current_request.as_object().erase(field);
        }
        CHECK(current_request == old_request);
    }
    void recovered(const Fixture& fixture, bool terminal, const std::string& event_type) const {
        const auto current = fixture.store->request(fixture.selected.request);
        if (terminal) {
            CHECK(current == request);
            CHECK(fixture.store->task(fixture.selected.task) == task);
            CHECK(fixture.store->binding(fixture.selected.task) == binding);
            const auto after = Database(fixture.path).snapshot();
            CHECK(after.at("teams") == all.at("teams"));
            CHECK(after.at("tasks") == all.at("tasks"));
        } else {
            CHECK(num(current, "dispatcher_epoch") == num(request, "dispatcher_epoch") + 1);
            CHECK(num(current, "state_version") == num(request, "state_version") + 1);
            const auto rebound = fixture.store->binding(fixture.selected.task);
            CHECK(rebound.dispatcher_epoch == binding.dispatcher_epoch + 1);
            CHECK(rebound.execution_epoch == binding.execution_epoch + 1);
            CHECK(rebound.requirements_revision == binding.requirements_revision);
            CHECK(rebound.plan_revision == binding.plan_revision);
            CHECK(rebound.task_revision == binding.task_revision);
            CHECK(rebound.instruction_revision == binding.instruction_revision);
        }
        const auto after_events = Database(fixture.path).rows("SELECT * FROM events ORDER BY seq");
        CHECK(after_events.size() == events.size() + (event_type.empty() ? 0U : 1U));
        if (!event_type.empty())
            CHECK(Database(fixture.path).count("SELECT COUNT(*) FROM events WHERE seq=(SELECT MAX(seq) FROM events) AND request_id=? AND type=?",
                {fixture.selected.request, event_type}) == 1);
    }
};

void unchanged_after_idle_reopen(Fixture& fixture) {
    only_trigger(fixture, Trigger::None);
    const auto stable = Database(fixture.path).snapshot();
    // On a6b8559 the first recovery is valid but this assertion can fail:
    // unconditional opening repeats it. That is not a new predicate defect.
    std::cout << "CHECKPOINT isolated recovery passed; checking idle reopen\n";
    fixture.reopen();
    CHECK(Database(fixture.path).snapshot() == stable);
}

void cancel_requested_is_the_only_trigger() {
    Fixture fixture;
    const auto running = fixture.running(fixture.selected);
    const auto id = text(running, "op_id");
    fixture.store.reset();
    // cancel_operation currently maps RUNNING straight to UNKNOWN. Model a
    // retained legacy cancellation request, without inventing an exit result.
    Database(fixture.path, true).inject_one(
        "UPDATE ops SET status='CANCEL_REQUESTED',state_version=state_version+1 WHERE op_id=? AND status='RUNNING' AND state_version=?",
        {id, std::to_string(num(running, "state_version"))});
    only_trigger(fixture, Trigger::CancelRequested);
    // Read-only Store does not trigger recovery or issue write authority.
    const auto& sql = assets().sql;
    fixture.store = std::make_unique<Store>(fixture.path, sql[0], sql[1], StoreOpenMode::ReadOnly,
        sql[2], sql[3], sql[4], sql[5], sql[6]);
    const auto pending = fixture.store->operation(id);
    CHECK(pending.at("status") == "CANCEL_REQUESTED");
    CHECK(num(pending, "state_version") == num(running, "state_version") + 1);
    auto running_fact = running, pending_fact = pending;
    for (const auto* field : {"status", "state_version"}) {
        running_fact.as_object().erase(field); pending_fact.as_object().erase(field);
    }
    CHECK(running_fact == pending_fact);
    const Before before(fixture);
    fixture.reopen();
    before.preserved(fixture, true); before.recovered(fixture, false, "recover");
    const auto recovered = fixture.store->operation(id);
    CHECK(recovered.at("status") == "UNKNOWN");
    CHECK(num(recovered, "state_version") == num(pending, "state_version") + 1);
    auto original = pending, current = recovered;
    for (const auto* field : {"status", "state_version"}) { original.as_object().erase(field); current.as_object().erase(field); }
    CHECK(current == original);
    const auto stable = Database(fixture.path).snapshot();
    rejected(ErrorCode::Conflict, [&] {
        fixture.store->reconcile_operation(id, num(pending, "state_version"), "FAILED", "stale CAS must not settle", "stale-reconcile");
    });
    CHECK(Database(fixture.path).snapshot() == stable);
    fixture.acknowledge(fixture.selected);
    const auto acknowledged = Database(fixture.path).snapshot();
    rejected(ErrorCode::UnknownOutcome, [&] { (void)fixture.running(fixture.selected); });
    CHECK(Database(fixture.path).snapshot() == acknowledged);
    CHECK(fixture.store->operation(id).at("status") == "UNKNOWN");
    unchanged_after_idle_reopen(fixture);
}

void orphan_claimed_inbox_is_the_only_trigger() {
    Fixture fixture;
    auto op = fixture.running(fixture.selected);
    op = fixture.store->finish_operation(fixture.actor(fixture.selected), text(op, "op_id"),
        fixture.store->binding(fixture.selected.task), num(op, "state_version"), "UNKNOWN", "original unknown observation", new_id("finish"));
    const auto unknown = op;
    const auto claim = fixture.store->claim_events(fixture.actor(fixture.selected, Role::Dispatcher), 1);
    CHECK(claim.at("items").as_array().size() == 1);
    const auto claim_id = text(claim, "claim_id");
    // Public finish_events releases both tables in one transaction. Retain a
    // damaged legacy inbox paired with a released claim, never deleting it.
    Database(fixture.path, true).inject_one(
        "UPDATE event_actor_claims SET status='RELEASED' WHERE claim_id=? AND status='ACTIVE'", {claim_id});
    only_trigger(fixture, Trigger::OrphanInbox);
    const Before before(fixture);
    const auto claim_row = Database(fixture.path).rows("SELECT * FROM event_actor_claims WHERE claim_id=?", {claim_id});
    fixture.reopen();
    before.preserved(fixture); before.recovered(fixture, false, "recover");
    CHECK(fixture.store->operation(text(unknown, "op_id")) == unknown);
    CHECK(Database(fixture.path).rows("SELECT * FROM event_actor_claims WHERE claim_id=?", {claim_id}) == claim_row);
    const auto inbox_id = num(claim.at("items").at(0), "inbox_id");
    CHECK(Database(fixture.path).count("SELECT COUNT(*) FROM event_inbox WHERE inbox_id=? AND status='PENDING' AND claim_id IS NULL AND completed_at IS NULL",
                                     {std::to_string(inbox_id)}) == 1);
    const auto stable = Database(fixture.path).snapshot();
    rejected(ErrorCode::Stale, [&] {
        fixture.store->finish_events(fixture.actor(fixture.selected, Role::Dispatcher), claim_id, num(claim, "generation"), {inbox_id});
    });
    CHECK(Database(fixture.path).snapshot() == stable);
    unchanged_after_idle_reopen(fixture);
    const auto fresh = fixture.store->claim_events(fixture.actor(fixture.selected, Role::Dispatcher), 1);
    CHECK(num(fresh, "generation") == num(claim, "generation") + 1);
    CHECK(text(fresh, "claim_id") != claim_id);
    CHECK(num(fresh.at("items").at(0), "inbox_id") == inbox_id);
    fixture.store->finish_events(fixture.actor(fixture.selected, Role::Dispatcher), text(fresh, "claim_id"), num(fresh, "generation"), {});
    CHECK(fixture.store->operation(text(unknown, "op_id")) == unknown);
}

Json fail_job(Fixture& fixture, bool experience) {
    const auto job = experience ? fixture.store->experience_jobs(fixture.selected.request).at(0)
                                : fixture.store->closeout(fixture.selected.request).at("cleanup_job");
    CHECK(job.at("status") == "PENDING" || job.at("status") == "RUNNING");
    const std::string table = experience ? "experience_jobs" : "cleanup_jobs";
    const std::string key = experience ? "experience_id" : "cleanup_id";
    // The current job APIs create PENDING, claim RUNNING (cleanup), and finish;
    // none writes FAILED. Preserve an existing real job/source/receipt and
    // model only a legacy failure release. A null claim makes FAILED itself
    // the only matching recovery branch, independent of RUNNING or ownership.
    Database(fixture.path, true).inject_one("UPDATE " + table +
        " SET status='FAILED',claimed_by=NULL,state_version=state_version+1 WHERE " + key + "=? AND state_version=? AND status IN ('PENDING','RUNNING')",
        {text(job, key), std::to_string(num(job, "state_version"))});
    const auto failed = experience ? fixture.store->experience_job(text(job, key))
                                  : fixture.store->closeout(fixture.selected.request).at("cleanup_job");
    CHECK(num(failed, "state_version") == num(job, "state_version") + 1);
    auto original = job, injected = failed;
    for (const auto* field : {"status", "claimed_by", "state_version"}) {
        original.as_object().erase(field); injected.as_object().erase(field);
    }
    CHECK(original == injected);
    return failed;
}
void job_recovered(const Json& before, const Json& after) {
    CHECK(before.at("status") == "FAILED" && before.at("claimed_by").is_null());
    CHECK(after.at("status") == "PENDING" && after.at("claimed_by").is_null());
    CHECK(num(after, "lease_epoch") == num(before, "lease_epoch") + 1);
    CHECK(num(after, "state_version") == num(before, "state_version") + 1);
    auto original = before, recovered = after;
    for (const auto* field : {"status", "claimed_by", "lease_epoch", "state_version", "updated_at"}) {
        original.as_object().erase(field); recovered.as_object().erase(field);
    }
    CHECK(original == recovered);
}
void failed_experience(bool terminal) {
    Fixture fixture(false); fixture.accepted(false, false);
    CHECK(Database(fixture.path).count("SELECT COUNT(*) FROM cleanup_jobs WHERE request_id=?", {fixture.selected.request}) == 0);
    if (terminal) fixture.deliver();
    const auto failed = fail_job(fixture, true);
    only_trigger(fixture, Trigger::FailedExperience);
    const Before before(fixture);
    const auto cleanup = fixture.store->closeout(fixture.selected.request).at("cleanup_job");
    fixture.reopen();
    before.preserved(fixture); before.recovered(fixture, terminal, terminal ? "recover_experience" : "recover");
    const auto recovered = fixture.store->experience_job(text(failed, "experience_id"));
    job_recovered(failed, recovered);
    if (terminal) {
        const auto current_cleanup = fixture.store->closeout(fixture.selected.request).at("cleanup_job");
        CHECK(current_cleanup.at("status") == "PENDING" && current_cleanup.at("claimed_by").is_null());
        CHECK(num(current_cleanup, "lease_epoch") == num(cleanup, "lease_epoch") + 1);
        CHECK(num(current_cleanup, "state_version") == num(cleanup, "state_version") + 1);
        CHECK(current_cleanup.at("manifest_snapshot") == cleanup.at("manifest_snapshot"));
        CHECK(fixture.publication().at("verified") == true);
    }
    const Json report = Object{{"report_id", new_id("rep")}, {"request_id", fixture.selected.request},
        {"task_id", fixture.selected.task}, {"team_id", fixture.selected.team},
        {"no_new_experience", true}, {"entries", Array{}}, {"memory_assessments", Array{}}};
    const auto stable = Database(fixture.path).snapshot();
    rejected(ErrorCode::Conflict, [&] {
        fixture.store->report_experience(fixture.actor(fixture.selected, Role::TeamLead), fixture.store->binding(fixture.selected.task),
            text(failed, "experience_id"), num(failed, "state_version"), text(failed, "source_sha256"), report, "stale-experience-CAS");
    });
    rejected(ErrorCode::NotReady, [&] {
        fixture.store->begin_workspace_cleanup(fixture.actor(fixture.selected, Role::Root), Object{}, Array{});
    });
    CHECK(Database(fixture.path).snapshot() == stable);
    CHECK(read_runtime_file(fixture.workspace / "result.txt") == "alpha candidate version 1\n");
    unchanged_after_idle_reopen(fixture);
}

void failed_delivered_cleanup_is_the_only_trigger() {
    Fixture fixture(false); fixture.accepted(true, true); fixture.deliver();
    const auto inventory = inventory_owned_workspace(fixture.resource);
    const auto claim = fixture.store->begin_workspace_cleanup(fixture.actor(fixture.selected, Role::Root), fixture.publication(), Array{inventory});
    const auto failed = fail_job(fixture, false);
    only_trigger(fixture, Trigger::FailedCleanup);
    const Before before(fixture);
    fixture.reopen();
    before.preserved(fixture); before.recovered(fixture, true, "");
    job_recovered(failed, fixture.store->closeout(fixture.selected.request).at("cleanup_job"));
    CHECK(fixture.store->owned_workspaces(fixture.selected.request).at(0).at("inventory") == inventory);
    CHECK(read_runtime_file(fixture.workspace / "result.txt") == "alpha candidate version 1\n");
    CHECK(fixture.publication().at("verified") == true);
    const auto stable = Database(fixture.path).snapshot();
    rejected(ErrorCode::Stale, [&] { fixture.store->authorize_workspace_cleanup(fixture.actor(fixture.selected, Role::Root), claim); });
    rejected(ErrorCode::NotReady, [&] { (void)fixture.running(fixture.selected); });
    CHECK(Database(fixture.path).snapshot() == stable);
    unchanged_after_idle_reopen(fixture);
    const auto fresh = fixture.store->begin_workspace_cleanup(fixture.actor(fixture.selected, Role::Root), fixture.publication(), Array{inventory});
    CHECK(num(fresh, "lease_epoch") == num(claim, "lease_epoch") + 2);
    CHECK(num(fresh, "job_version") == num(failed, "state_version") + 2);
    fixture.store->authorize_workspace_cleanup(fixture.actor(fixture.selected, Role::Root), fresh);
    CHECK(std::filesystem::exists(fixture.workspace / "result.txt"));
}

void failed_cancelled_cleanup_retains_work_without_business_authority() {
    Fixture fixture; fixture.allocate();
    {
        WorkerTools tools(*fixture.store, fixture.workspace);
        tools.bind(fixture.actor(fixture.selected));
        (void)tools.execute(fixture.actor(fixture.selected), fixture.store->binding(fixture.selected.task),
            num(fixture.store->task(fixture.selected.task), "state_version"), "retained-file", "fs_write",
            Object{{"path", "retained.txt"}, {"content", "original cancelled work\n"}}, "retained-write");
    }
    // There is no active-request cleanup job: it comes from a real terminal
    // receipt. Do not manufacture a FAILED cleanup under an active request.
    CHECK(Database(fixture.path).count("SELECT COUNT(*) FROM cleanup_jobs WHERE request_id=?", {fixture.selected.request}) == 0);
    rejected(ErrorCode::NotReady, [&] {
        fixture.store->begin_workspace_cleanup(fixture.actor(fixture.selected, Role::Root), Object{}, Array{});
    });
    fixture.store->cancel_request(fixture.actor(fixture.selected, Role::Root), num(fixture.store->request(fixture.selected.request), "state_version"), "cancel-retained");
    CHECK(fixture.store->request(fixture.selected.request).at("status") == "CANCELLED");
    const auto failed = fail_job(fixture, false);
    only_trigger(fixture, Trigger::FailedCleanup);
    const Before before(fixture);
    fixture.reopen();
    before.preserved(fixture); before.recovered(fixture, true, "");
    job_recovered(failed, fixture.store->closeout(fixture.selected.request).at("cleanup_job"));
    const auto stable = Database(fixture.path).snapshot();
    rejected(ErrorCode::NotReady, [&] {
        fixture.store->begin_workspace_cleanup(fixture.actor(fixture.selected, Role::Root), Object{}, Array{});
    });
    rejected(ErrorCode::NotReady, [&] { (void)fixture.running(fixture.selected); });
    CHECK(Database(fixture.path).snapshot() == stable);
    CHECK(read_runtime_file(fixture.workspace / "retained.txt") == "original cancelled work\n");
    CHECK(fixture.store->owned_workspaces(fixture.selected.request).at(0).at("status") == "OWNED");
    CHECK(fixture.store->closeout(fixture.selected.request).at("cleanup_complete") == false);
    unchanged_after_idle_reopen(fixture);
}
} // namespace

int main(int argc, char** argv) {
    // Groups are fixed before running the suite: each is independently subject
    // to the existing 90-second S1 registration deadline. Default runs all six.
    const std::vector<test::Case> active{
        {"CANCEL_REQUESTED alone fences an original operation without inventing exit", cancel_requested_is_the_only_trigger},
        {"orphan CLAIMED inbox alone releases original work and preserves UNKNOWN", orphan_claimed_inbox_is_the_only_trigger},
        {"FAILED experience alone recovers an active request with source and budget intact", [] { failed_experience(false); }}};
    const std::vector<test::Case> terminal{
        {"FAILED experience alone recovers delivered management without business epochs", [] { failed_experience(true); }},
        {"FAILED cleanup alone renews delivered lease without deleting or accepting old CAS", failed_delivered_cleanup_is_the_only_trigger},
        {"FAILED retained cleanup after cancellation cannot gain deletion or business authority", failed_cancelled_cleanup_retains_work_without_business_authority}};
    if (argc == 3 && std::string_view(argv[1]) == "--group") {
        if (std::string_view(argv[2]) == "active") return test::run(active);
        if (std::string_view(argv[2]) == "terminal") return test::run(terminal);
    }
    if (argc != 1) { std::cerr << "usage: r24_recovery_predicates_tests [--group active|terminal]\n"; return 2; }
    auto all = active; all.insert(all.end(), terminal.begin(), terminal.end());
    return test::run(all);
}
