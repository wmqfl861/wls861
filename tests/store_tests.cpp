#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "wls/model.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <csignal>
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
void raw_exec(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    char* message = nullptr;
    const auto result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    const std::string reason = message ? message : "SQLite fixture failure";
    sqlite3_free(message); sqlite3_close(db);
    if (result != SQLITE_OK) throw std::runtime_error(reason);
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
}

int main(int argc, char** argv) {
    const auto root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(WLS_SOURCE_DIR);
    source_root = root;
    try {
        base_sql = read(root / "db/schema.sql");
        migration_sql = read(root / "db/migrations/004_s1_store.sql");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return test::run({
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
        {"process crash retains committed request and rolls back partial SQL", process_crash_keeps_commits_and_rolls_back_uncommitted_change}
    });
}
