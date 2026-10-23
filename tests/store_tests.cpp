#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "wls/model.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <csignal>
#include <barrier>
#include <fstream>
#include <future>
#include <iterator>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace wls::s1;
using wls::Role;
using Object = boost::json::object;
using Array = boost::json::array;

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
std::string base_sql;
std::string migration_sql;
std::string tools_migration_sql;
std::filesystem::path source_root;
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string s(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t n(const Json& value, std::string_view key) { return value.at(key).as_int64(); }
void error(ErrorCode code, const std::function<void()>& operation) {
    try { operation(); }
    catch (const Error& e) { CHECK(e.code() == code); return; }
    throw std::runtime_error("expected Store error was not thrown");
}
void catalog_shape_rejected_at_store_boundary();
void raw_exec(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    char* message = nullptr;
    const auto result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    const std::string reason = message ? message : "SQLite fixture failure";
    sqlite3_free(message); sqlite3_close(db);
    if (result != SQLITE_OK) throw std::runtime_error(reason);
}
std::int64_t raw_number(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    CHECK(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const auto result = static_cast<std::int64_t>(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement); sqlite3_close(db);
    return result;
}
AdmissionInput input(std::string key = "first", std::int64_t teams = 4, std::int64_t tools = 200) {
    AdmissionInput result;
    result.tenant_id = "tenant"; result.project_id = "project"; result.root_id = "root";
    result.function_id = "general"; result.idempotency_key = std::move(key);
    result.original_text = "preserve original input; use cents";
    test::TempDir config;
    std::filesystem::copy(source_root / "config/examples", config.path / "config", std::filesystem::copy_options::recursive);
    std::filesystem::copy(source_root / "prompts", config.path / "prompts", std::filesystem::copy_options::recursive);
    const auto system_path = config.path / "config/system.yaml";
    auto catalog = load_config_catalog(system_path, config.path / "config/functions", config.path / "prompts");
    if (teams != 4 || tools != 200) {
        auto system = catalog.freeze("general").json().at("system");
        system.at("limits").at("max_teams_per_request") = teams;
        system.at("limits").at("worker_max_tool_calls_per_order") = tools;
        { std::ofstream file(system_path); file << canonical_json(system); }
        catalog = load_config_catalog(system_path, config.path / "config/functions", config.path / "prompts");
    }
    result.config_snapshot = catalog.freeze("general").json();
    result.tool_allowlist = {"fs_read", "fs_write"};
    result.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", "preserve original input"}, {"source_revision", 1}},
        Object{{"constraint_id", "c2"}, {"text", "use cents"}, {"source_revision", 1}}};
    result.budget = BudgetLimits{64, 10000, 10000, 1000};
    return result;
}
Json brief() {
    return Object{{"goal", "perform the bounded fixture"}, {"constraint_refs", Array{"c1", "c2"}},
        {"constraints_verbatim", Array{"preserve original input", "use cents"}}};
}
struct Fixture {
    test::TempDir temp;
    std::filesystem::path path{temp.path / "state.sqlite"};
    std::unique_ptr<Store> store{std::make_unique<Store>(path, base_sql, migration_sql)};
    std::string request_id;
    std::string task_id;
    std::string team_id;
    explicit Fixture(AdmissionInput admission = input()) {
        request_id = s(store->admit(admission), "request_id");
        plan(); create("node-one");
    }
    StoreActor actor(Role role, const std::string& team = {}) const {
        return store->host_actor(role, request_id, role == Role::TeamLead || role == Role::Worker ? (team.empty() ? team_id : team) : "");
    }
    std::int64_t rv() const { return n(store->request(request_id), "state_version"); }
    std::int64_t tv(const std::string& task = {}) const { return n(store->task(task.empty() ? task_id : task), "state_version"); }
    void plan() {
        store->publish_plan(actor(Role::Dispatcher), rv(), n(store->request(request_id), "requirements_revision"), Object{{"goal", "fixture plan"}}, new_id("cmd"));
    }
    Json create(const std::string& node) {
        const auto request = store->request(request_id);
        const auto result = store->create_task(actor(Role::Dispatcher), rv(), n(request, "requirements_revision"), n(request, "plan_revision"), node, brief(), new_id("cmd"));
        if (task_id.empty()) { task_id = s(result, "task_id"); team_id = s(result, "team_id"); }
        return result;
    }
    void instruction(const std::string& task = {}, const std::string& team = {}) {
        const auto t = task.empty() ? task_id : task;
        store->issue_instruction(actor(Role::TeamLead, team), store->binding(t), tv(t), brief(), new_id("cmd"));
    }
    void ack(const std::string& task = {}, const std::string& team = {}) {
        const auto t = task.empty() ? task_id : task;
        store->acknowledge(actor(Role::Worker, team), store->binding(t), tv(t), new_id("cmd"));
    }
    void ready() { instruction(); ack(); }
    Json prepare(const std::string& key = "business-one") {
        return store->prepare_operation(actor(Role::Worker), store->binding(task_id), tv(), key, "fs_write", Object{{"append", "one"}}, "workspace", new_id("cmd"));
    }
    AttemptReservation reservation(std::string id = "attempt-one",
            const std::function<void(ContextInput&)>& customize = {}) const {
        AttemptReservation result;
        result.call_id = "call-" + id; result.attempt_id = std::move(id); result.config_snapshot_sha256 = s(store->request(request_id), "config_snapshot_sha256");
        result.model_ref = "fake"; result.reserved_input_tokens = 100; result.reserved_output_tokens = 50;
        result.reserved_cost_micro_usd = 100; result.max_output_tokens = 50;
        const auto record = store->request(request_id);
        const auto config = FrozenRequestConfig::restore(canonical_json(record.at("config_snapshot")), s(record, "config_snapshot_sha256"));
        const auto profile = config.model_profile(Role::Worker);
        ContextInput context;
        context.role = Role::Worker; context.model = config.resolve(Role::Worker);
        context.adapter_version = s(profile, "adapter_version"); context.protocol_version = s(profile, "protocol");
        context.system_prompt = "Bounded Store regression fixture.";
        context.role_prompt = "Only worker records business intents.";
        context.function_prompt = "Record a deterministic fixture response.";
        context.task_contract = canonical_json(store->request_snapshot(request_id));
        if (customize) customize(context);
        result.model_ref = context.model.model;
        ModelRequest request;
        request.call_id = result.call_id; request.attempt_id = result.attempt_id;
        request.role = Role::Worker; request.binding = store->binding(task_id); request.config_snapshot_id = result.config_snapshot_sha256;
        request.model = context.model; request.context = ContextCompiler().compile(context);
        request.max_output_tokens = result.max_output_tokens; request.reserved_input_tokens = result.reserved_input_tokens;
        request.reserved_output_tokens = result.reserved_output_tokens; request.reserved_cost_micro_usd = result.reserved_cost_micro_usd;
        result.request_envelope = to_json(request);
        return result;
    }
};

AdmissionInput tools_input(std::int64_t tools = 200) {
    auto result = input("worker-tools", 4, tools);
    result.tool_allowlist = {"fs_list", "fs_read", "fs_search", "fs_write", "fs_patch", "run", "note", "op_status", "op_output", "op_cancel", "ack_change"};
    return result;
}
void upgrade_tools(Fixture& fixture) {
    fixture.store.reset();
    fixture.store = std::make_unique<Store>(fixture.path, base_sql, migration_sql, StoreOpenMode::Takeover, tools_migration_sql);
}
Json bind_tools(Fixture& fixture, const Json& policy = Object{{"mode", "fixture-no-process"}}) {
    const auto path = fixture.temp.path / "workspace";
    std::filesystem::create_directory(path);
    struct stat info{}; CHECK(::stat(path.c_str(), &info) == 0);
    return fixture.store->bind_workspace(fixture.team_id, std::filesystem::canonical(path).string(),
        static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino), policy);
}
Json prepare_tool(Fixture& fixture, const std::string& key = "write-once", const Json& arguments = Object{{"path", "/work/a.txt"}, {"text", "bounded"}}) {
    return fixture.store->prepare_tool_operation(fixture.actor(Role::Worker), fixture.store->binding(fixture.task_id), fixture.tv(),
        key, "fs_write", arguments, new_id("cmd"));
}
Json start_tool(Fixture& fixture, const Json& op) {
    return fixture.store->start_operation(fixture.actor(Role::Worker), s(op, "op_id"), n(op, "state_version"), new_id("cmd"));
}

void durability_migration_and_readonly() {
    Fixture f;
    CHECK(n(f.store->diagnostics(), "user_version") == 402);
    CHECK(n(f.store->diagnostics(), "synchronous") == 2);
    CHECK(n(f.store->diagnostics(), "foreign_keys") == 1);
    CHECK(s(f.store->diagnostics(), "journal_mode") == "wal");
    struct stat info{};
    CHECK(::stat(f.path.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
    const auto old = f.store->request(f.request_id);
    {
        Store observer(f.path, base_sql, migration_sql, StoreOpenMode::ReadOnly);
        CHECK(observer.request(f.request_id) == old);
        error(ErrorCode::Unauthorized, [&] { observer.admit(input("observer")); });
    }
    CHECK(f.store->request(f.request_id) == old);
    error(ErrorCode::Conflict, [&] { Store other(f.path, base_sql, migration_sql); });
    f.store.reset();
    f.store = std::make_unique<Store>(f.path, base_sql, migration_sql);
    CHECK(n(f.store->request(f.request_id), "dispatcher_epoch") == n(old, "dispatcher_epoch") + 1);
    CHECK(f.store->request(f.request_id).at("config_snapshot") == old.at("config_snapshot"));
    f.store.reset();
    f.store = std::make_unique<Store>(f.path, base_sql, migration_sql);
    CHECK(n(f.store->diagnostics(), "user_version") == 402);
    error(ErrorCode::Corrupt, [&] { Store observer(f.path, base_sql, migration_sql + "\n-- altered", StoreOpenMode::ReadOnly); });
}
void unknown_database_is_preserved() {
    test::TempDir temp;
    const auto path = temp.path / "old.sqlite";
    raw_exec(path, "CREATE TABLE owner_data(text TEXT); INSERT INTO owner_data VALUES ('keep'); PRAGMA user_version=999;");
    const auto original_bytes = read(path);
    error(ErrorCode::Unsupported, [&] { Store store(path, base_sql, migration_sql); });
    CHECK(read(path) == original_bytes);
    raw_exec(path, "CREATE TABLE still_here AS SELECT text FROM owner_data WHERE text='keep';");
    const auto reference = temp.path / "reference.sqlite";
    raw_exec(reference, base_sql);
    raw_exec(reference, "INSERT INTO requests(request_id,tenant_id,project_id,root_id,function_id,origin,idempotency_key,content_sha256,dispatcher_id,status,config_snapshot,budget,created_at,updated_at) VALUES ('old','t','p','r','f','cli','k','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','d','RUNNING','{}','{}','t','t');");
    error(ErrorCode::Unsupported, [&] { Store store(reference, base_sql, migration_sql); });
    const auto symlink = temp.path / "link.sqlite";
    std::filesystem::create_symlink(path, symlink);
    error(ErrorCode::Storage, [&] { Store store(symlink, base_sql, migration_sql); });
}
void scoped_admission_and_original_retry() {
    Fixture f;
    CHECK(s(f.store->admit(input()), "request_id") == f.request_id);
    auto changed = input(); changed.budget.max_attempts = 63;
    error(ErrorCode::Conflict, [&] { f.store->admit(changed); });
    changed = input(); changed.project_id = "second-project";
    CHECK(s(f.store->admit(changed), "request_id") != f.request_id);
    f.store->amend(f.actor(Role::Root), f.rv(), "now output euros", input().constraint_catalog, "amend-one");
    CHECK(s(f.store->admit(input()), "request_id") == f.request_id);
    CHECK(f.store->request_snapshot(f.request_id).at("texts").as_array().size() == 2);
}
void event_failure_rolls_back_whole_transaction() {
    Fixture f;
    const auto before = f.store->request(f.request_id);
    raw_exec(f.path, "CREATE TRIGGER injected_event_failure BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'injected event failure'); END;");
    error(ErrorCode::Conflict, [&] { f.plan(); });
    CHECK(f.store->request(f.request_id) == before);
    raw_exec(f.path, "DROP TRIGGER injected_event_failure;");
    f.plan();
    CHECK(n(f.store->request(f.request_id), "plan_revision") == n(before, "plan_revision") + 1);
}
void instruction_ack_and_durable_exact_commands() {
    Fixture f;
    error(ErrorCode::NotReady, [&] { f.prepare(); });
    const auto lead = f.actor(Role::TeamLead); const auto binding = f.store->binding(f.task_id); const auto version = f.tv();
    const auto result = f.store->issue_instruction(lead, binding, version, brief(), "instruction-one");
    CHECK(f.store->issue_instruction(lead, binding, version, brief(), "instruction-one") == result);
    auto other = brief(); other.as_object()["goal"] = "changed";
    error(ErrorCode::Conflict, [&] { f.store->issue_instruction(lead, binding, version, other, "instruction-one"); });
    f.ack();
    error(ErrorCode::NotReady, [&] { f.ack(); });
    CHECK(s(f.prepare(), "status") == "PREPARED");
    f.store.reset(); f.store = std::make_unique<Store>(f.path, base_sql, migration_sql);
    CHECK(f.store->issue_instruction(lead, binding, version, brief(), "instruction-one") == result);
    error(ErrorCode::Stale, [&] { f.store->issue_instruction(lead, binding, version, brief(), "late-new-command"); });
}
void full_constraint_text_and_frozen_team_limit() {
    Fixture f(input("first", 1));
    error(ErrorCode::Quota, [&] { f.create("forbidden-second-team"); });
    auto missing = brief(); missing.at("constraints_verbatim").as_array().pop_back();
    error(ErrorCode::InvalidInput, [&] { f.store->issue_instruction(f.actor(Role::TeamLead), f.store->binding(f.task_id), f.tv(), missing, "missing-text"); });
    auto reworded = brief(); reworded.at("constraints_verbatim").as_array()[0] = "overwrite original input";
    error(ErrorCode::InvalidInput, [&] { f.store->issue_instruction(f.actor(Role::TeamLead), f.store->binding(f.task_id), f.tv(), reworded, "rewritten-text"); });
}
void amend_omission_and_stale_disposition() {
    Fixture f; f.ready();
    const auto second = f.create("node-two"); const auto t2 = s(second, "task_id"); const auto g2 = s(second, "team_id");
    f.instruction(t2, g2); f.ack(t2, g2);
    const auto op = f.prepare(); f.store->start_operation(f.actor(Role::Worker), s(op, "op_id"), 1, "start");
    error(ErrorCode::Unauthorized, [&] { f.store->amend(f.actor(Role::Dispatcher), f.rv(), "changed", input().constraint_catalog, "not-root"); });
    f.store->amend(f.actor(Role::Root), f.rv(), "now output euros", input().constraint_catalog, "amend"); f.plan();
    CHECK(s(f.store->operation(s(op, "op_id")), "status") == "UNKNOWN");
    const auto request = f.store->request(f.request_id);
    f.store->set_amendment_disposition(f.actor(Role::Dispatcher), f.task_id, f.tv(), 2, n(request, "plan_revision"), "keep", "constraints unchanged", brief(), "keep-first");
    f.instruction(); f.ack();
    error(ErrorCode::UnknownOutcome, [&] { f.prepare("new-business"); });
    error(ErrorCode::NotReady, [&] { f.instruction(t2, g2); });
    error(ErrorCode::NotReady, [&] { f.ack(t2, g2); });
    f.store->amend(f.actor(Role::Root), f.rv(), "now also keep precision", input().constraint_catalog, "amend-again"); f.plan();
    error(ErrorCode::Stale, [&] { f.store->set_amendment_disposition(f.actor(Role::Dispatcher), t2, f.tv(t2), 2, n(request, "plan_revision"), "keep", "old decision", brief(), "old-disposition"); });
    CHECK(n(f.store->task(t2), "based_on_requirements_revision") == 1);
}
void plan_only_rebind_fences_running_result() {
    Fixture f; f.ready();
    const auto worker = f.actor(Role::Worker); const auto old_binding = f.store->binding(f.task_id);
    const auto op = f.prepare(); f.store->start_operation(worker, s(op, "op_id"), 1, "start");
    f.plan();
    CHECK(s(f.store->operation(s(op, "op_id")), "status") == "UNKNOWN");
    error(ErrorCode::NotReady, [&] { f.instruction(); });
    f.store->rebind_task(f.actor(Role::Dispatcher), f.task_id, f.tv(), 1, 2, brief(), "rebind"); f.instruction(); f.ack();
    error(ErrorCode::Stale, [&] { f.store->finish_operation(worker, s(op, "op_id"), old_binding, 2, "SUCCEEDED", "fixture:evidence", "old-finish"); });
}
void operation_business_key_owner_and_cancellation() {
    Fixture f; f.ready();
    const auto actor = f.actor(Role::Worker); const auto binding = f.store->binding(f.task_id); const auto version = f.tv();
    auto op = f.store->prepare_operation(actor, binding, version, "intent", "fs_write", Object{{"append", "one"}}, "workspace", "prepare");
    const auto id = s(op, "op_id");
    CHECK(f.store->prepare_operation(actor, binding, version, "intent", "fs_write", Object{{"append", "one"}}, "workspace", "prepare") == op);
    error(ErrorCode::Conflict, [&] { f.store->prepare_operation(actor, binding, f.tv(), "intent", "fs_write", Object{{"append", "two"}}, "workspace", "other"); });
    auto forged = actor; forged.principal_id = "someone-else";
    error(ErrorCode::Unauthorized, [&] { f.store->cancel_operation(forged, id, 1, "forged"); });
    f.store->start_operation(actor, id, 1, "start");
    op = f.store->cancel_operation(actor, id, 2, "cancel");
    CHECK(s(op, "status") == "UNKNOWN");
    CHECK(f.store->cancel_operation(actor, id, 2, "cancel") == op);
    error(ErrorCode::NotReady, [&] { f.store->cancel_operation(actor, id, 3, "repeat-cancel"); });
    f.instruction(); f.ack();
    error(ErrorCode::UnknownOutcome, [&] { f.prepare("second"); });
    op = f.store->reconcile_operation(id, 3, "SUCCEEDED", "fixture:verified exactly one append", "reconcile");
    CHECK(s(op, "status") == "SUCCEEDED");
    CHECK(s(f.prepare("second"), "status") == "PREPARED");
}
void ordinary_tool_quota_does_not_block_controls() {
    Fixture f(input("first", 4, 1)); f.ready();
    auto op = f.prepare();
    error(ErrorCode::Quota, [&] { f.prepare("over-quota"); });
    op = f.store->cancel_operation(f.actor(Role::Worker), s(op, "op_id"), 1, "cancel-under-quota");
    CHECK(s(op, "status") == "CANCELLED");
    f.instruction(); f.ack();
    CHECK(s(f.prepare("new-order-operation"), "status") == "PREPARED");
}
void attempt_budget_exact_retry_nullable_usage() {
    auto admission = input(); admission.budget = BudgetLimits{2, 1000, 1000, 150};
    Fixture f(admission); f.ready();
    const auto actor = f.actor(Role::Worker); const auto binding = f.store->binding(f.task_id); auto reservation = f.reservation();
    const auto attempt = f.store->reserve_attempt(actor, binding, reservation);
    CHECK(f.store->reserve_attempt(actor, binding, reservation) == attempt);
    CHECK(n(f.store->budget(f.request_id), "attempts") == 1);
    auto changed = reservation; changed.attempt_id = "retry-same-call";
    error(ErrorCode::Conflict, [&] { f.store->reserve_attempt(actor, binding, changed); });
    const Json usage = Object{{"input_tokens", 40}, {"output_tokens", 20}, {"cache_read_tokens", nullptr}, {"cost_micro_usd", nullptr}};
    const auto settled = f.store->settle_attempt(reservation.attempt_id, binding, reservation.config_snapshot_sha256, "COMPLETED", usage, Object{{"text", "result"}});
    CHECK(n(settled, "accepted") == 1);
    CHECK(settled.at("cost_micro_usd").is_null());
    const auto budget = f.store->budget(f.request_id);
    CHECK(n(budget, "spent_input") == 40 && n(budget, "reserved_input") == 0);
    CHECK(n(budget, "spent_cost") == 0 && n(budget, "reserved_cost") == 100 && n(budget, "unknown_cost") == 100);
    CHECK(f.store->settle_attempt(reservation.attempt_id, binding, reservation.config_snapshot_sha256, "COMPLETED", usage, Object{{"text", "result"}}) == settled);
    error(ErrorCode::Conflict, [&] { f.store->settle_attempt(reservation.attempt_id, binding, reservation.config_snapshot_sha256, "COMPLETED", Object{{"cost_micro_usd", 0}}, Object{{"text", "result"}}); });
    error(ErrorCode::Quota, [&] { f.store->reserve_attempt(actor, binding, f.reservation("second")); });
    CHECK(n(f.store->budget(f.request_id), "attempts") == 1);
}
void model_envelope_and_frozen_permissions_precede_reservation() {
    Fixture f; f.ready();
    const auto actor = f.actor(Role::Worker); const auto binding = f.store->binding(f.task_id);
    const auto before = f.store->budget(f.request_id);
    const auto event_count = f.store->events(f.request_id).size();
    auto missing = f.reservation(); missing.request_envelope = nullptr;
    error(ErrorCode::InvalidInput, [&] { f.store->reserve_attempt(actor, binding, missing); });
    auto wrong_identity = f.reservation(); wrong_identity.request_envelope.at("attempt_id") = "another-attempt";
    error(ErrorCode::Conflict, [&] { f.store->reserve_attempt(actor, binding, wrong_identity); });
    const auto wrong_model = f.reservation("wrong-model", [](ContextInput& context) { context.model.model = "unselected-model"; });
    error(ErrorCode::Unauthorized, [&] { f.store->reserve_attempt(actor, binding, wrong_model); });
    const auto wrong_reasoning = f.reservation("wrong-reasoning", [](ContextInput& context) {
        context.model.reasoning = {wls::ReasoningMode::Disabled, {}};
    });
    error(ErrorCode::Unauthorized, [&] { f.store->reserve_attempt(actor, binding, wrong_reasoning); });
    const auto wrong_adapter = f.reservation("wrong-adapter", [](ContextInput& context) { context.adapter_version = "unfrozen-v2"; });
    error(ErrorCode::Unauthorized, [&] { f.store->reserve_attempt(actor, binding, wrong_adapter); });
    const auto wrong_protocol = f.reservation("wrong-protocol", [](ContextInput& context) { context.protocol_version = "unfrozen-v2"; });
    error(ErrorCode::Unauthorized, [&] { f.store->reserve_attempt(actor, binding, wrong_protocol); });
    const auto ungranted_tool = f.reservation("ungranted-tool", [](ContextInput& context) {
        context.tools.push_back(ContextTool{"run", "1", Object{{"type", "object"}}});
    });
    error(ErrorCode::Unauthorized, [&] { f.store->reserve_attempt(actor, binding, ungranted_tool); });
    auto excessive_output = f.reservation("excessive-output");
    excessive_output.max_output_tokens = 8193; excessive_output.reserved_output_tokens = 8193;
    excessive_output.request_envelope.at("max_output_tokens") = 8193;
    excessive_output.request_envelope.at("reserved_output_tokens") = 8193;
    error(ErrorCode::Quota, [&] { f.store->reserve_attempt(actor, binding, excessive_output); });
    CHECK(f.store->budget(f.request_id) == before);
    CHECK(f.store->events(f.request_id).size() == event_count);
    const auto valid = f.reservation("valid-complete-envelope");
    const auto accepted = f.store->reserve_attempt(actor, binding, valid);
    CHECK(accepted.at("request_envelope") == valid.request_envelope);
    f.store.reset(); f.store = std::make_unique<Store>(f.path, base_sql, migration_sql);
    CHECK(f.store->attempt(valid.attempt_id).at("request_envelope") == valid.request_envelope);
    CHECK(s(f.store->attempt(valid.attempt_id), "status") == "UNKNOWN");
}
void model_late_result_is_accounted_without_retagging() {
    Fixture f; f.ready();
    const auto old_binding = f.store->binding(f.task_id); const auto reservation = f.reservation();
    f.store->reserve_attempt(f.actor(Role::Worker), old_binding, reservation);
    f.store->amend(f.actor(Role::Root), f.rv(), "later requirements", input().constraint_catalog, "amend");
    const auto current = f.store->binding(f.task_id);
    error(ErrorCode::Conflict, [&] { f.store->settle_attempt(reservation.attempt_id, current, reservation.config_snapshot_sha256, "COMPLETED", Object{{"cost_micro_usd", 20}}, Object{{"text", "late"}}); });
    const auto result = f.store->settle_attempt(reservation.attempt_id, old_binding, reservation.config_snapshot_sha256, "COMPLETED", Object{{"cost_micro_usd", 20}}, Object{{"text", "late"}});
    CHECK(n(result, "accepted") == 0 && s(result, "status") == "STALE");
    CHECK(n(f.store->budget(f.request_id), "spent_cost") == 20);
    CHECK(n(f.store->budget(f.request_id), "reserved_cost") == 0);
    CHECK(n(f.store->budget(f.request_id), "unknown_input") == 100);
}
void recovery_preserves_unknown_attempt_reservations() {
    Fixture f; f.ready();
    const auto old = f.store->binding(f.task_id); const auto reservation = f.reservation();
    f.store->reserve_attempt(f.actor(Role::Worker), old, reservation);
    auto op = f.prepare(); f.store->start_operation(f.actor(Role::Worker), s(op, "op_id"), 1, "start");
    f.store.reset(); f.store = std::make_unique<Store>(f.path, base_sql, migration_sql);
    CHECK(s(f.store->attempt(reservation.attempt_id), "status") == "UNKNOWN");
    CHECK(n(f.store->budget(f.request_id), "reserved_cost") == 100 && n(f.store->budget(f.request_id), "unknown_cost") == 100);
    CHECK(s(f.store->operation(s(op, "op_id")), "status") == "UNKNOWN");
    CHECK(f.store->pending_recovery().at("unknown_operations").as_array().size() == 1);
    const auto result = f.store->settle_attempt(reservation.attempt_id, old, reservation.config_snapshot_sha256, "ERROR", Object{{"input_tokens", 2}, {"output_tokens", 0}, {"cost_micro_usd", 7}}, Object{{"error", "provider evidence"}});
    CHECK(s(result, "status") == "STALE" && n(result, "accepted") == 0);
    CHECK(n(f.store->budget(f.request_id), "unknown_cost") == 0 && n(f.store->budget(f.request_id), "spent_cost") == 7);
}
void concurrent_cas_has_one_winner() {
    Fixture f;
    const auto actor = f.actor(Role::Dispatcher); const auto version = f.rv();
    auto update = [&](const std::string& command) {
        try { f.store->publish_plan(actor, version, 1, Object{{"goal", command}}, command); return true; }
        catch (const Error& e) { CHECK(e.code() == ErrorCode::Conflict); return false; }
    };
    auto first = std::async(std::launch::async, update, "plan-one");
    auto second = std::async(std::launch::async, update, "plan-two");
    CHECK(first.get() != second.get());
    CHECK(f.rv() == version + 1);
}
void cancelled_request_retains_accounting_and_control_retry() {
    Fixture f; f.ready();
    const auto reservation = f.reservation(); const auto binding = f.store->binding(f.task_id);
    f.store->reserve_attempt(f.actor(Role::Worker), binding, reservation);
    const auto root = f.actor(Role::Root); const auto version = f.rv();
    const auto cancelled = f.store->cancel_request(root, version, "cancel-request");
    CHECK(f.store->cancel_request(root, version, "cancel-request") == cancelled);
    CHECK(s(f.store->attempt(reservation.attempt_id), "status") == "UNKNOWN");
    f.store.reset(); f.store = std::make_unique<Store>(f.path, base_sql, migration_sql);
    CHECK(n(f.store->budget(f.request_id), "unknown_cost") == 100);
    CHECK(f.store->cancel_request(root, version, "cancel-request") == cancelled);
}
void populated_402_migrates_without_replacing_request_history() {
    Fixture f(tools_input()); f.ready();
    const auto original_snapshot = f.store->request_snapshot(f.request_id);
    const auto original_config = f.store->request(f.request_id).at("config_snapshot");
    auto op = f.prepare();
    op = f.store->start_operation(f.actor(Role::Worker), s(op, "op_id"), n(op, "state_version"), "start-before-upgrade");
    const auto reservation = f.reservation("pending-before-upgrade");
    f.store->reserve_attempt(f.actor(Role::Worker), f.store->binding(f.task_id), reservation);
    upgrade_tools(f);
    CHECK(n(f.store->diagnostics(), "user_version") == 403);
    CHECK(f.store->request_snapshot(f.request_id) == original_snapshot);
    CHECK(f.store->request(f.request_id).at("config_snapshot") == original_config);
    CHECK(s(f.store->operation(s(op, "op_id")), "status") == "UNKNOWN");
    CHECK(f.store->operation(s(op, "op_id")).at("arguments") == op.at("arguments"));
    CHECK(s(f.store->attempt(reservation.attempt_id), "status") == "UNKNOWN");
    CHECK(n(f.store->budget(f.request_id), "reserved_cost") == 100);
    CHECK(f.store->workspace(f.team_id).is_null());
    CHECK(raw_number(f.path, "SELECT COUNT(*) FROM schema_migrations WHERE version=5") == 1);
    const auto before = f.store->binding(f.task_id);
    const auto count = f.store->events(f.request_id).size();
    {
        Store observer(f.path, base_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql);
        CHECK(observer.binding(f.task_id) == before);
        CHECK(observer.request_snapshot(f.request_id) == original_snapshot);
        error(ErrorCode::Unauthorized, [&] { observer.bind_workspace(f.team_id, "/tmp/observer", 1, 2); });
    }
    CHECK(f.store->events(f.request_id).size() == count);
    error(ErrorCode::Unsupported, [&] { Store observer(f.path, base_sql, migration_sql, StoreOpenMode::ReadOnly); });
    error(ErrorCode::Corrupt, [&] { Store observer(f.path, base_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql + "\n-- altered"); });
    upgrade_tools(f);
    CHECK(n(f.store->diagnostics(), "user_version") == 403);
    CHECK(raw_number(f.path, "SELECT COUNT(*) FROM schema_migrations WHERE version=5") == 1);
    CHECK(f.store->request_snapshot(f.request_id) == original_snapshot);
}
void tools_migration_failure_rolls_back_all_schema_changes() {
    Fixture f(tools_input()); f.ready();
    const auto before = f.store->request(f.request_id);
    const auto before_events = f.store->events(f.request_id);
    f.store.reset();
    error(ErrorCode::Storage, [&] {
        Store failed(f.path, base_sql, migration_sql, StoreOpenMode::Takeover,
            tools_migration_sql + "\nINSERT INTO deliberately_absent_table VALUES(1);\n");
    });
    {
        Store observer(f.path, base_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql);
        CHECK(n(observer.diagnostics(), "user_version") == 402);
        CHECK(observer.request(f.request_id) == before);
        CHECK(observer.events(f.request_id) == before_events);
        error(ErrorCode::Unsupported, [&] { observer.workspace(f.team_id); });
    }
    CHECK(raw_number(f.path, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('worker_tools_metadata','workspace_bindings','operation_results','operation_output_bytes')") == 0);
    CHECK(raw_number(f.path, "SELECT COUNT(*) FROM schema_migrations WHERE version=5") == 0);
    upgrade_tools(f);
    CHECK(n(f.store->diagnostics(), "user_version") == 403);
}
void workspace_identity_policy_and_team_aliases_are_immutable() {
    Fixture f(tools_input()); upgrade_tools(f);
    const Json policy = Object{{"mode", "strong"}, {"runtime_manifest", Array{"sha256:fixture"}}, {"network", "none"}};
    const auto identity = bind_tools(f, policy);
    const auto path = s(identity, "canonical_path");
    const auto device = static_cast<std::uint64_t>(n(identity, "device"));
    const auto inode = static_cast<std::uint64_t>(n(identity, "inode"));
    CHECK(identity.at("execution_policy") == policy);
    CHECK(s(identity, "execution_policy_sha256") == sha256_hex(canonical_json(policy)));
    const auto event_count = f.store->events(f.request_id).size();
    CHECK(f.store->bind_workspace(f.team_id, path, device, inode, policy) == identity);
    CHECK(f.store->events(f.request_id).size() == event_count);
    error(ErrorCode::Conflict, [&] { f.store->bind_workspace(f.team_id, path, device, inode, Object{{"mode", "weak"}}); });
    error(ErrorCode::Conflict, [&] { f.store->bind_workspace(f.team_id, path, device, inode + 1, policy); });
    error(ErrorCode::Conflict, [&] { f.store->bind_workspace(f.team_id, path + "-other", device, inode, policy); });
    error(ErrorCode::InvalidInput, [&] { f.store->bind_workspace(f.team_id, "", device, inode, policy); });
    error(ErrorCode::InvalidInput, [&] { f.store->bind_workspace(f.team_id, "/tmp/../workspace", device, inode, policy); });
    error(ErrorCode::InvalidInput, [&] { f.store->bind_workspace(f.team_id, path, 0, inode, policy); });
    error(ErrorCode::InvalidInput, [&] { f.store->bind_workspace(f.team_id, path, device, 0, policy); });
    const auto other = f.create("separate-team");
    const auto other_id = s(other, "team_id");
    error(ErrorCode::Conflict, [&] { f.store->bind_workspace(other_id, path, device, inode, policy); });
    error(ErrorCode::Conflict, [&] { f.store->bind_workspace(other_id, path + "-alias", device, inode, policy); });
    error(ErrorCode::Conflict, [&] { f.store->bind_workspace(other_id, path + "/nested", device, inode + 1, policy); });
    CHECK(f.store->workspace(other_id).is_null());
    upgrade_tools(f);
    CHECK(f.store->workspace(f.team_id) == identity);
    CHECK(f.store->bind_workspace(f.team_id, path, device, inode, policy) == identity);
}
void workspace_cannot_enclose_its_store_control_files() {
    Fixture f(tools_input()); upgrade_tools(f);
    const auto request = f.store->request(f.request_id);
    const auto events = f.store->events(f.request_id);
    const auto parent = std::filesystem::canonical(f.temp.path);
    struct stat identity{}; CHECK(::stat(parent.c_str(), &identity) == 0 && S_ISDIR(identity.st_mode));
    CHECK(std::filesystem::is_regular_file(f.path));
    CHECK(std::filesystem::is_regular_file(f.path.string() + ".writer-lock"));
    CHECK(std::filesystem::is_regular_file(f.path.string() + "-wal"));
    error(ErrorCode::Unauthorized, [&] {
        f.store->bind_workspace(f.team_id, parent.string(), static_cast<std::uint64_t>(identity.st_dev), static_cast<std::uint64_t>(identity.st_ino));
    });
    CHECK(f.store->workspace(f.team_id).is_null());
    CHECK(f.store->request(f.request_id) == request && f.store->events(f.request_id) == events);
    const auto safe = bind_tools(f);
    CHECK(s(safe, "canonical_path") == (parent / "workspace").string());
    CHECK(f.store->request(f.request_id) == request);
}
void registered_business_tools_preserve_quota_and_exact_intents() {
    Fixture f(tools_input(1)); upgrade_tools(f); bind_tools(f); f.ready();
    const auto actor = f.actor(Role::Worker); const auto binding = f.store->binding(f.task_id);
    const Json args = Object{{"path", "/work/a"}, {"text", "bounded"}};
    const auto expected = f.tv();
    auto op = f.store->prepare_tool_operation(actor, binding, expected, "intent", "fs_write", args, "prepare-registered");
    CHECK(s(op, "side_effect") == "workspace");
    error(ErrorCode::Unsupported, [&] { f.store->prepare_tool_operation(actor, binding, f.tv(), "diagnostic-spoof", "fs_read", args, "bad-class"); });
    error(ErrorCode::Unsupported, [&] { f.store->prepare_tool_operation(actor, binding, f.tv(), "control-spoof", "op_cancel", args, "bad-control"); });
    op = start_tool(f, op);
    const auto recorded = f.store->record_operation_result(s(op, "op_id"), binding, "SUCCEEDED", Object{{"written", 7}}, Object{{"fixture", "verified once"}});
    CHECK(n(recorded, "accepted") == 1 && s(recorded, "operation_status") == "SUCCEEDED");
    const auto replay = f.store->prepare_tool_operation(actor, binding, expected, "intent", "fs_write", args, "prepare-registered");
    CHECK(s(replay, "status") == "SUCCEEDED");
    CHECK(f.store->operation_for_key(actor, binding, "intent", "fs_write", args) == replay);
    error(ErrorCode::Conflict, [&] { f.store->operation_for_key(actor, binding, "intent", "fs_write", Object{{"text", "different"}}); });
    error(ErrorCode::Conflict, [&] { f.store->operation_for_key(actor, binding, "intent", "run", args); });
    error(ErrorCode::Quota, [&] { prepare_tool(f, "second"); });
    CHECK(n(f.store->authorize_diagnostic(actor, binding, "fs_read"), "max_queries_per_minute") == 60);
    CHECK(f.store->operation_for_key(actor, binding, "missing", "fs_write", args).is_null());
}
void concurrent_registered_intent_has_one_dispatch_claim() {
    Fixture f(tools_input()); upgrade_tools(f); bind_tools(f); f.ready();
    const auto actor = f.actor(Role::Worker);
    const auto binding = f.store->binding(f.task_id);
    const auto expected = f.tv();
    const Json args = Object{{"path", "/work/once"}, {"content", "one effect"}};
    std::barrier gate(2);
    const auto execute = [&] {
        const auto old = f.store->operation_for_key(actor, binding, "race-key", "fs_write", args);
        CHECK(old.is_null());
        gate.arrive_and_wait();
        const auto prepared = f.store->prepare_tool_operation(actor, binding, expected, "race-key", "fs_write", args, "same-prepare");
        gate.arrive_and_wait();
        return f.store->start_tool_operation(actor, s(prepared, "op_id"), n(prepared, "state_version"), "same-start");
    };
    auto first = std::async(std::launch::async, execute);
    auto second = std::async(std::launch::async, execute);
    const auto a = first.get(); const auto b = second.get();
    CHECK(a.at("dispatch_claimed").as_bool() != b.at("dispatch_claimed").as_bool());
    const auto id = s(a.at("operation"), "op_id");
    CHECK(id == s(b.at("operation"), "op_id"));
    CHECK(s(a.at("operation"), "status") == "RUNNING" && s(b.at("operation"), "status") == "RUNNING");
    CHECK(raw_number(f.path, "SELECT COUNT(*) FROM ops") == 1);
    CHECK(raw_number(f.path, "SELECT COUNT(*) FROM events WHERE type='start_tool_op'") == 1);
    CHECK(!f.store->start_tool_operation(actor, id, 1, "same-start").at("dispatch_claimed").as_bool());
    CHECK(!f.store->start_tool_operation(actor, id, 2, "different-start").at("dispatch_claimed").as_bool());
    f.store->record_operation_result(id, binding, "SUCCEEDED", Object{{"once", true}}, Object{{"fixture", "only claim winner executes"}});
    const auto replay = f.store->start_tool_operation(actor, id, 1, "same-start");
    CHECK(!replay.at("dispatch_claimed").as_bool() && s(replay.at("operation"), "status") == "SUCCEEDED");
    CHECK(raw_number(f.path, "SELECT COUNT(*) FROM events WHERE type='start_tool_op'") == 1);
}
void execution_results_and_binary_output_commit_atomically() {
    Fixture f(tools_input()); upgrade_tools(f); bind_tools(f); f.ready();
    const auto binding = f.store->binding(f.task_id);
    auto op = start_tool(f, prepare_tool(f)); const auto id = s(op, "op_id");
    const Json result = Object{{"bytes_written", 7}};
    const Json evidence = Object{{"path", "/work/a.txt"}, {"observed_sha256", sha256_hex("bounded")}};
    const std::string output = std::string("A\0\xFF", 3) + "B";
    const auto original_fields = to_json(binding);
    for (const auto& field : original_fields.as_object()) {
        auto forged = to_json(binding);
        if (field.value().is_string()) forged.at(field.key()) = new_id(field.key() == "request_id" ? "req" : field.key() == "task_id" ? "tsk" : "team");
        else forged.at(field.key()) = field.value().as_int64() + 1;
        error(ErrorCode::Stale, [&] { f.store->record_operation_result(id, execution_binding_from_json(forged), "SUCCEEDED", result, evidence, output); });
    }
    error(ErrorCode::Quota, [&] { f.store->record_operation_result(id, binding, "SUCCEEDED", result, evidence, std::string(1048577, 'x')); });
    error(ErrorCode::InvalidInput, [&] { f.store->record_operation_result(id, binding, "SUCCEEDED", result, Object{}); });
    raw_exec(f.path, "CREATE TRIGGER reject_execution_record BEFORE INSERT ON events WHEN NEW.type='record_op_result' BEGIN SELECT RAISE(ABORT,'fixture event failure'); END;");
    error(ErrorCode::Conflict, [&] { f.store->record_operation_result(id, binding, "SUCCEEDED", result, evidence, output, "stderr\n", true); });
    CHECK(s(f.store->operation(id), "status") == "RUNNING");
    CHECK(f.store->operation_result(id).is_null());
    CHECK(raw_number(f.path, "SELECT COUNT(*) FROM operation_output_bytes") == 0);
    raw_exec(f.path, "DROP TRIGGER reject_execution_record;");
    const auto recorded = f.store->record_operation_result(id, binding, "SUCCEEDED", result, evidence, output, "stderr\n", true);
    CHECK(recorded.at("original_binding") == to_json(binding));
    CHECK(recorded.at("result") == result && recorded.at("evidence") == evidence);
    CHECK(n(recorded, "stdout_bytes") == 4 && s(recorded, "stdout_sha256") == sha256_hex(output));
    CHECK(f.store->record_operation_result(id, binding, "SUCCEEDED", result, evidence, output, "stderr\n", true) == recorded);
    error(ErrorCode::Conflict, [&] { f.store->record_operation_result(id, binding, "SUCCEEDED", result, evidence, output + "!"); });
    const auto actor = f.actor(Role::Worker);
    const auto page = f.store->operation_output(actor, binding, id, "stdout", 0, 3);
    CHECK(s(page, "data") == "QQD/" && n(page, "next_offset") == 3 && page.at("truncated").as_bool() && !page.at("eof").as_bool());
    const auto last = f.store->operation_output(actor, binding, id, "stdout", 3, 3);
    CHECK(s(last, "data") == "Qg==" && last.at("eof").as_bool());
    CHECK(s(f.store->operation_output(actor, binding, id, "stdout", 1, 2), "data") == "AP8=");
    error(ErrorCode::InvalidInput, [&] { f.store->operation_output(actor, binding, id, "stdout", 5); });
    error(ErrorCode::InvalidInput, [&] { f.store->operation_output(actor, binding, id, "stdout", 0, 65537); });
    const auto extra = f.create("foreign-reader");
    error(ErrorCode::Unauthorized, [&] { f.store->operation_output(f.actor(Role::Worker, s(extra, "team_id")), f.store->binding(s(extra, "task_id")), id, "stdout"); });
    Store reader(f.path, base_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql);
    CHECK(reader.operation_result(id) == recorded);
    CHECK(reader.operation_output(actor, binding, id, "stdout", 0, 3) == page);
}
void stale_execution_facts_survive_amend_cancel_and_reopen() {
    for (const std::string action : {"amend", "cancel", "reopen"}) {
        Fixture f(tools_input()); upgrade_tools(f); bind_tools(f); f.ready();
        const auto original = f.store->binding(f.task_id);
        auto op = start_tool(f, prepare_tool(f)); const auto id = s(op, "op_id");
        if (action == "amend") f.store->amend(f.actor(Role::Root), f.rv(), "preserve original input; use cents; changed direction", input().constraint_catalog, "amend-late");
        else if (action == "cancel") f.store->cancel_request(f.actor(Role::Root), f.rv(), "cancel-late");
        else upgrade_tools(f);
        CHECK(s(f.store->operation(id), "status") == "UNKNOWN");
        const Json evidence = Object{{"fixture", "original write occurred"}, {"action", action}};
        const auto recorded = f.store->record_operation_result(id, original, "SUCCEEDED", Object{{"observed", true}}, evidence, "old-output");
        CHECK(n(recorded, "accepted") == 0 && s(recorded, "operation_status") == "UNKNOWN");
        CHECK(recorded.at("original_binding") == to_json(original));
        CHECK(f.store->record_operation_result(id, original, "SUCCEEDED", Object{{"observed", true}}, evidence, "old-output") == recorded);
        error(ErrorCode::Stale, [&] { f.store->record_operation_result(id, f.store->binding(f.task_id), "SUCCEEDED", Object{{"observed", true}}, evidence, "old-output"); });
        CHECK(n(f.store->authorize_diagnostic(f.actor(Role::Worker), f.store->binding(f.task_id), "fs_read"), "max_page_bytes") == 65536);
        const auto reconciled = f.store->reconcile_operation(id, n(f.store->operation(id), "state_version"), "SUCCEEDED", "host checked original write evidence", new_id("cmd"));
        CHECK(s(reconciled, "status") == "SUCCEEDED");
        CHECK(n(f.store->operation_result(id), "accepted") == 0);
        CHECK(f.store->operation_result(id).at("evidence") == evidence);
    }
}
void completed_business_key_does_not_reexecute_after_new_instruction() {
    Fixture f(tools_input()); upgrade_tools(f); bind_tools(f); f.ready();
    const auto original_actor = f.actor(Role::Worker);
    const auto original = f.store->binding(f.task_id);
    auto op = start_tool(f, prepare_tool(f)); const auto id = s(op, "op_id");
    const auto args = op.at("arguments");
    f.store->record_operation_result(id, original, "SUCCEEDED", Object{{"once", true}}, Object{{"fixture", "one write"}});
    f.instruction();
    const auto current_actor = f.actor(Role::Worker);
    const auto current = f.store->binding(f.task_id);
    CHECK(current.instruction_revision > original.instruction_revision);
    const auto before = f.store->events(f.request_id).size();
    const auto historical = f.store->operation_for_key(current_actor, current, "write-once", "fs_write", args);
    CHECK(s(historical, "op_id") == id && n(historical, "instruction_revision") == original.instruction_revision);
    CHECK(f.store->prepare_tool_operation(current_actor, current, f.tv(), "write-once", "fs_write", args, new_id("cmd")) == historical);
    CHECK(f.store->events(f.request_id).size() == before);
    error(ErrorCode::Stale, [&] { f.store->operation_for_key(original_actor, original, "write-once", "fs_write", args); });
    error(ErrorCode::Conflict, [&] { f.store->prepare_tool_operation(current_actor, current, f.tv(), "write-once", "fs_write", Object{{"text", "different"}}, new_id("cmd")); });
    upgrade_tools(f);
    const auto restored = f.store->operation_for_key(f.actor(Role::Worker), f.store->binding(f.task_id), "write-once", "fs_write", args);
    CHECK(restored == historical && n(f.store->operation_result(id), "accepted") == 1);
    CHECK(raw_number(f.path, "SELECT COUNT(*) FROM ops") == 1);
    CHECK(f.store->workspace(f.team_id).at("execution_policy").is_object());
}
void diagnostic_and_control_guards_are_current_bounded_and_readonly() {
    Fixture f(tools_input(1)); upgrade_tools(f); bind_tools(f); f.ready();
    const auto original = f.store->binding(f.task_id);
    const auto original_actor = f.actor(Role::Worker);
    auto op = start_tool(f, prepare_tool(f)); const auto id = s(op, "op_id");
    f.store->cancel_operation(original_actor, id, n(op, "state_version"), "controlled-cancel");
    f.instruction();
    const auto current = f.store->binding(f.task_id);
    const auto actor = f.actor(Role::Worker);
    CHECK(s(f.store->operation(id), "status") == "UNKNOWN");
    const auto before_request = f.store->request(f.request_id);
    const auto before_events = f.store->events(f.request_id);
    Store reader(f.path, base_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql);
    CHECK(n(reader.authorize_diagnostic(actor, current, "fs_read"), "max_queries_per_minute") == 60);
    CHECK(n(reader.authorize_control(actor, current, "ack_change"), "max_queries_per_minute") == 60);
    CHECK(n(reader.authorize_control(actor, current, "op_cancel"), "max_output_bytes") == 1048576);
    error(ErrorCode::Unauthorized, [&] { reader.authorize_diagnostic(actor, current, "run"); });
    error(ErrorCode::Unauthorized, [&] { reader.authorize_control(actor, current, "run"); });
    error(ErrorCode::Unauthorized, [&] { reader.authorize_diagnostic(f.actor(Role::TeamLead), current, "fs_read"); });
    error(ErrorCode::Stale, [&] { reader.authorize_diagnostic(original_actor, original, "fs_read"); });
    auto forged = actor; forged.principal_id = "not-the-worker";
    error(ErrorCode::Unauthorized, [&] { reader.authorize_diagnostic(forged, current, "fs_read"); });
    CHECK(f.store->request(f.request_id) == before_request && f.store->events(f.request_id) == before_events);
    raw_exec(f.path, "UPDATE runtime_policy SET policy_epoch=policy_epoch+1,denied_tools='[\"fs_read\"]',state_version=state_version+1 WHERE singleton=1;");
    error(ErrorCode::Stale, [&] { reader.authorize_diagnostic(actor, current, "fs_read"); });
    const auto policy_current = f.store->binding(f.task_id);
    error(ErrorCode::Unauthorized, [&] { reader.authorize_diagnostic(actor, policy_current, "fs_read"); });
    CHECK(reader.authorize_diagnostic(actor, policy_current, "op_status").is_object());
}
void revoked_tool_preserves_other_model_declarations_and_business_grants() {
    Fixture f(tools_input()); upgrade_tools(f); bind_tools(f); f.ready();
    const auto actor = f.actor(Role::Worker);
    const auto old_binding = f.store->binding(f.task_id);
    raw_exec(f.path, "UPDATE runtime_policy SET policy_epoch=policy_epoch+1,denied_tools='[\"fs_read\"]',state_version=state_version+1 WHERE singleton=1;");
    error(ErrorCode::Stale, [&] { f.store->authorized_worker_tools(actor, old_binding); });
    const auto current = f.store->binding(f.task_id);
    const auto grants = f.store->authorized_worker_tools(actor, current);
    CHECK(std::find(grants.begin(), grants.end(), "fs_read") == grants.end());
    CHECK(std::find(grants.begin(), grants.end(), "fs_write") != grants.end());
    auto reservation = f.reservation("after-revocation", [&](ContextInput& context) {
        for (const auto& name : grants) context.tools.push_back({name, "fixture-v1", Object{{"type", "object"}}});
    });
    const auto attempt = f.store->reserve_attempt(actor, current, reservation);
    CHECK(s(attempt, "status") == "PENDING");
    CHECK(n(f.store->budget(f.request_id), "attempts") == 1);
    auto still_denied = f.reservation("old-declaration", [](ContextInput& context) {
        context.tools.push_back({"fs_read", "fixture-v1", Object{{"type", "object"}}});
    });
    error(ErrorCode::Unauthorized, [&] { f.store->reserve_attempt(actor, current, still_denied); });
    CHECK(n(f.store->budget(f.request_id), "attempts") == 1);
    const auto op = prepare_tool(f);
    CHECK(s(op, "tool_id") == "fs_write");
    error(ErrorCode::Unauthorized, [&] { f.store->authorize_diagnostic(actor, current, "fs_read"); });
}
void tool_ack_exact_replay_cannot_apply_a_new_instruction() {
    Fixture f(tools_input()); upgrade_tools(f); bind_tools(f); f.instruction();
    const auto actor = f.actor(Role::Worker);
    const auto original = f.store->binding(f.task_id);
    const auto expected = f.tv();
    const auto before = f.store->task(f.task_id);
    raw_exec(f.path, "CREATE TRIGGER reject_tool_control BEFORE INSERT ON events WHEN NEW.type='execute_tool_control' BEGIN SELECT RAISE(ABORT,'control event failure'); END;");
    error(ErrorCode::Conflict, [&] { f.store->execute_tool_control(actor, original, expected, "ack_change", Object{}, "tool-ack"); });
    CHECK(f.store->task(f.task_id) == before);
    raw_exec(f.path, "DROP TRIGGER reject_tool_control;");
    const auto acknowledged = f.store->execute_tool_control(actor, original, expected, "ack_change", Object{}, "tool-ack");
    CHECK(n(f.store->task(f.task_id), "applied_instruction_revision") == original.instruction_revision);
    f.instruction();
    const auto next_instruction = n(f.store->task(f.task_id), "instruction_revision");
    const auto count = f.store->events(f.request_id).size();
    CHECK(next_instruction > original.instruction_revision);
    CHECK(f.store->execute_tool_control(actor, original, expected, "ack_change", Object{}, "tool-ack") == acknowledged);
    CHECK(f.store->events(f.request_id).size() == count);
    CHECK(n(f.store->task(f.task_id), "applied_instruction_revision") == original.instruction_revision);
    error(ErrorCode::Stale, [&] { f.store->execute_tool_control(actor, original, expected, "ack_change", Object{}, "fresh-old-ack"); });
    error(ErrorCode::Conflict, [&] { f.store->execute_tool_control(actor, original, expected + 1, "ack_change", Object{}, "tool-ack"); });
    upgrade_tools(f);
    CHECK(f.store->execute_tool_control(actor, original, expected, "ack_change", Object{}, "tool-ack") == acknowledged);
    CHECK(n(f.store->task(f.task_id), "instruction_revision") == next_instruction);
    CHECK(n(f.store->task(f.task_id), "applied_instruction_revision") == original.instruction_revision);
    error(ErrorCode::Stale, [&] { f.store->execute_tool_control(actor, original, expected, "ack_change", Object{}, "new-command-old-epoch"); });
    f.instruction();
    const auto current = f.store->binding(f.task_id);
    const auto current_actor = f.actor(Role::Worker);
    f.store->execute_tool_control(current_actor, current, f.tv(), "ack_change", Object{}, "ack-current");
    CHECK(n(f.store->task(f.task_id), "applied_instruction_revision") == current.instruction_revision);
    raw_exec(f.path, "UPDATE runtime_policy SET policy_epoch=policy_epoch+1,denied_tools='[\"ack_change\"]',state_version=state_version+1 WHERE singleton=1;");
    error(ErrorCode::Unauthorized, [&] { f.store->execute_tool_control(actor, original, expected, "ack_change", Object{}, "tool-ack"); });
}
void tool_cancel_retries_use_original_command_not_changed_op_version() {
    Fixture f(tools_input(1)); upgrade_tools(f); bind_tools(f); f.ready();
    const auto actor = f.actor(Role::Worker);
    const auto original = f.store->binding(f.task_id);
    const auto op = start_tool(f, prepare_tool(f));
    const auto id = s(op, "op_id");
    const auto expected = f.tv();
    const Json args = Object{{"op_id", id}};
    const auto cancelled = f.store->execute_tool_control(actor, original, expected, "op_cancel", args, "stable-cancel");
    CHECK(s(cancelled, "status") == "UNKNOWN");
    const auto count = f.store->events(f.request_id).size();
    CHECK(f.store->execute_tool_control(actor, original, expected, "op_cancel", args, "stable-cancel") == cancelled);
    CHECK(f.store->events(f.request_id).size() == count);
    error(ErrorCode::NotReady, [&] { f.store->execute_tool_control(actor, original, expected, "op_cancel", args, "new-cancel"); });
    error(ErrorCode::Conflict, [&] { f.store->execute_tool_control(actor, original, expected + 1, "op_cancel", args, "stable-cancel"); });
    error(ErrorCode::InvalidInput, [&] { f.store->execute_tool_control(actor, original, expected, "op_cancel", Object{{"op_id", id}, {"class", "control"}}, "bad-shape"); });
    error(ErrorCode::Unauthorized, [&] { f.store->execute_tool_control(actor, original, expected, "run", Object{}, "not-control"); });
    f.instruction();
    CHECK(f.store->execute_tool_control(actor, original, expected, "op_cancel", args, "stable-cancel") == cancelled);
    error(ErrorCode::Stale, [&] { f.store->execute_tool_control(actor, original, expected, "op_cancel", args, "stale-fresh-cancel"); });
    f.store->reconcile_operation(id, n(f.store->operation(id), "state_version"), "SUCCEEDED", "host observed completed original write", "checked-old-write");
    CHECK(f.store->execute_tool_control(actor, original, expected, "op_cancel", args, "stable-cancel") == cancelled);
    CHECK(s(f.store->operation(id), "status") == "SUCCEEDED");
    upgrade_tools(f);
    CHECK(f.store->execute_tool_control(actor, original, expected, "op_cancel", args, "stable-cancel") == cancelled);
    CHECK(s(f.store->operation(id), "status") == "SUCCEEDED");
}
void process_crash_keeps_commits_and_rolls_back_uncommitted_change() {
    test::TempDir temp;
    const auto path = temp.path / "crash.sqlite";
    int descriptors[2]{}; CHECK(::pipe(descriptors) == 0);
    const auto child = ::fork(); CHECK(child >= 0);
    if (child == 0) {
        ::close(descriptors[0]);
        try {
            Store store(path, base_sql, migration_sql);
            const auto request = store.admit(input());
            const auto id = s(request, "request_id");
            if (::write(descriptors[1], id.data(), id.size()) != static_cast<ssize_t>(id.size())) ::_exit(3);
            sqlite3* raw = nullptr;
            if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) ::_exit(4);
            if (sqlite3_exec(raw, "BEGIN IMMEDIATE; UPDATE requests SET title='uncommitted',state_version=state_version+1;", nullptr, nullptr, nullptr) != SQLITE_OK) ::_exit(5);
            ::kill(::getpid(), SIGKILL);
        } catch (...) { ::_exit(6); }
        ::_exit(7);
    }
    ::close(descriptors[1]);
    char bytes[128]{};
    const auto count = ::read(descriptors[0], bytes, sizeof(bytes));
    ::close(descriptors[0]);
    int status{}; CHECK(::waitpid(child, &status, 0) == child);
    CHECK(count > 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    Store recovered(path, base_sql, migration_sql);
    const auto request = recovered.request(std::string(bytes, static_cast<std::size_t>(count)));
    CHECK(request.at("title").is_null());
    CHECK(n(request, "dispatcher_epoch") == 2 && n(request, "state_version") == 2);
    CHECK(recovered.request_snapshot(s(request, "request_id")).at("texts").as_array().size() == 1);
}
void catalog_shape_rejected_at_store_boundary() {
    test::TempDir temp;
    const auto path = temp.path / "catalog.sqlite";
    Store store(path, base_sql, migration_sql);
    auto admission = input();
    admission.constraint_catalog = Array{Object{{"a", 1}, {"b", 2}, {"c", 3}}};
    error(ErrorCode::InvalidInput, [&] { (void)store.admit(admission); });
    CHECK(raw_number(path, "SELECT count(*) FROM requests") == 0);
    admission.constraint_catalog = Array{Object{{"constraint_id", 5}, {"text", "x"}, {"source_revision", 1}}};
    error(ErrorCode::InvalidInput, [&] { (void)store.admit(admission); });
    CHECK(raw_number(path, "SELECT count(*) FROM events") == 0);
}
}

int main(int argc, char** argv) {
    const auto root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(WLS_SOURCE_DIR);
    source_root = root;
    try {
        base_sql = read(root / "db/schema.sql");
        migration_sql = read(root / "db/migrations/004_s1_store.sql");
        tools_migration_sql = read(root / "db/migrations/005_worker_tools.sql");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return test::run({
        {"malformed catalog is invalid input and creates no rows", catalog_shape_rejected_at_store_boundary},
        {"SQLite FULL/WAL/FK, writer lease, read-only and repeated migration", durability_migration_and_readonly},
        {"unknown and incomplete reference databases remain intact", unknown_database_is_preserved},
        {"scoped immutable admission and original retry after AMEND", scoped_admission_and_original_retry},
        {"state, plan and event commit atomically", event_failure_rolls_back_whole_transaction},
        {"initial instruction, bounded acknowledgment and durable command retry", instruction_ack_and_durable_exact_commands},
        {"full assigned constraint text and frozen team capacity", full_constraint_text_and_frozen_team_limit},
        {"AMEND omission stays paused and old dispositions cannot be retagged", amend_omission_and_stale_disposition},
        {"plan-only rebind fences running effects and old results", plan_only_rebind_fences_running_result},
        {"operation idempotency, owner, cancellation and reconciliation", operation_business_key_owner_and_cancellation},
        {"ordinary tool quota preserves instruction/cancel controls", ordinary_tool_quota_does_not_block_controls},
        {"durable model reservations, exact retry and nullable accounting", attempt_budget_exact_retry_nullable_usage},
        {"complete model envelope and frozen permissions precede reservation", model_envelope_and_frozen_permissions_precede_reservation},
        {"late model response is accounted without current-version retagging", model_late_result_is_accounted_without_retagging},
        {"restart keeps UNKNOWN operation and model reservations", recovery_preserves_unknown_attempt_reservations},
        {"concurrent request CAS has exactly one successful commit", concurrent_cas_has_one_winner},
        {"cancelled request retains accounting and exact control retry", cancelled_request_retains_accounting_and_control_retry},
        {"process crash retains committed request and rolls back partial SQL", process_crash_keeps_commits_and_rolls_back_uncommitted_change},
        {"populated 402 migration preserves snapshots, attempts and intents", populated_402_migrates_without_replacing_request_history},
        {"failed 403 migration leaves data, schema version and events intact", tools_migration_failure_rolls_back_all_schema_changes},
        {"workspace identities and execution policies are immutable across teams and recovery", workspace_identity_policy_and_team_aliases_are_immutable},
        {"workspace cannot enclose this Store database, writer lease or SQLite side files", workspace_cannot_enclose_its_store_control_files},
        {"static business registry preserves quota and exact durable intents", registered_business_tools_preserve_quota_and_exact_intents},
        {"concurrent same-key intent and command have one durable dispatch claim", concurrent_registered_intent_has_one_dispatch_claim},
        {"original execution facts and bounded binary output commit atomically", execution_results_and_binary_output_commit_atomically},
        {"AMEND, cancellation and restart retain stale facts without resolving UNKNOWN", stale_execution_facts_survive_amend_cancel_and_reopen},
        {"old business keys return original results across instruction changes and restart", completed_business_key_does_not_reexecute_after_new_instruction},
        {"diagnostics and controls keep current identity, grants and read-only guards", diagnostic_and_control_guards_are_current_bounded_and_readonly},
        {"revoking one tool preserves other model declarations and business grants", revoked_tool_preserves_other_model_declarations_and_business_grants},
        {"exact tool ack replay preserves new pending instructions across epochs", tool_ack_exact_replay_cannot_apply_a_new_instruction},
        {"tool cancellation retries retain the original command despite changed op state", tool_cancel_retries_use_original_command_not_changed_op_version}
    });
}
