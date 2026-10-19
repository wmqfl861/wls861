#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "wls/model.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <barrier>
#include <fstream>
#include <future>
#include <iterator>

using namespace wls::s1;
using wls::Role;
using Object = boost::json::object;
using Array = boost::json::array;

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
std::filesystem::path source_root;
std::string base_sql, store_sql, tools_sql, events_sql;
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string s(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t n(const Json& value, std::string_view key) { return value.at(key).as_int64(); }
void error(ErrorCode expected, const std::function<void()>& operation) {
    try { operation(); }
    catch (const Error& e) { CHECK(e.code() == expected); return; }
    throw std::runtime_error("expected Store error");
}
void raw_exec(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* db = nullptr; CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    char* message = nullptr; const auto code = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    const std::string reason = message ? message : "SQLite fixture failure";
    sqlite3_free(message); sqlite3_close(db);
    if (code != SQLITE_OK) throw std::runtime_error(reason);
}
std::int64_t scalar(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* db = nullptr; CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr; CHECK(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const auto result = static_cast<std::int64_t>(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement); sqlite3_close(db); return result;
}
AdmissionInput admission() {
    AdmissionInput input;
    input.tenant_id = "tenant"; input.project_id = "project"; input.root_id = "root";
    input.function_id = "general"; input.idempotency_key = new_id("admission");
    input.original_text = "preserve fixture observations";
    input.config_snapshot = load_config_catalog(source_root / "config/examples/system.yaml",
        source_root / "config/examples/functions", source_root / "prompts").freeze("general").json();
    input.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", input.original_text}, {"source_revision", 1}}};
    input.tool_allowlist = {"note", "ack_change", "op_status", "op_cancel"};
    input.budget = {64, 1000000, 100000, 0};
    return input;
}
Json brief(const std::string& title = "fixture") {
    return Object{{"title", title}, {"goal", "preserve original durable observations"},
        {"constraint_refs", Array{"c1"}}, {"constraints_verbatim", Array{"preserve fixture observations"}}};
}
Json plan_body(std::int64_t count = 2) {
    Array nodes;
    for (std::int64_t index = 0; index < count; ++index) {
        const auto id = index == 0 ? "alpha" : index == 1 ? "beta" : "extra-" + std::to_string(index);
        nodes.emplace_back(Object{{"node_id", id}, {"kind", "team_task"}, {"title", id},
            {"depends_on", Array{}}, {"required", true}, {"brief", brief(id)}});
    }
    return Object{{"summary", "independent fixtures"}, {"nodes", nodes}};
}
Json decision(const std::string& node, const std::string& kind = "update") {
    return Object{{node, Object{{"disposition", kind}, {"rationale", "explicit fixture decision"}}}};
}
struct Fixture {
    test::TempDir temp;
    std::filesystem::path path{temp.path / "state.sqlite"};
    std::unique_ptr<Store> store;
    std::string request_id;
    explicit Fixture(bool event_schema = true, bool allocate = true) {
        store = std::make_unique<Store>(path, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, event_schema ? events_sql : "");
        request_id = s(store->admit(admission()), "request_id");
        if (allocate && event_schema) store->apply_independent_plan(actor(Role::Dispatcher), rv(), 1, plan_body(), Object{}, new_id("plan"));
    }
    StoreActor actor(Role role, const std::string& node = "alpha") const {
        if (role == Role::Root || role == Role::Dispatcher) return store->host_actor(role, request_id);
        return store->host_actor(role, request_id, s(task(node), "team_id"));
    }
    Json task(const std::string& node = "alpha") const {
        for (const auto& value : store->tasks(request_id)) if (s(value, "node_id") == node) return value;
        throw std::runtime_error("fixture node not allocated");
    }
    std::string tid(const std::string& node = "alpha") const { return s(task(node), "task_id"); }
    std::int64_t rv() const { return n(store->request(request_id), "state_version"); }
    std::int64_t tv(const std::string& node = "alpha") const { return n(task(node), "state_version"); }
    ExecutionBinding binding(const std::string& node = "alpha") const { return store->binding(tid(node)); }
    void order(const std::string& node = "alpha") { store->issue_instruction(actor(Role::TeamLead, node), binding(node), tv(node), brief(), new_id("order")); }
    void ack(const std::string& node = "alpha") { store->acknowledge(actor(Role::Worker, node), binding(node), tv(node), new_id("ack")); }
    void ready(const std::string& node = "alpha") { order(node); ack(node); }
    void amend() { store->amend(actor(Role::Root), rv(), "review amended fixtures", admission().constraint_catalog, new_id("amend")); }
    void reopen() { store.reset(); store = std::make_unique<Store>(path, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql); }
    Json prepare(const std::string& node = "alpha") { return store->prepare_tool_operation(actor(Role::Worker, node), binding(node), tv(node), new_id("business"), "note", Object{{"text", "fixture"}}, new_id("prepare")); }
    AttemptReservation reservation(const std::string& node = "alpha") const {
        AttemptReservation result; const auto record = store->request(request_id);
        const auto frozen = FrozenRequestConfig::restore(canonical_json(record.at("config_snapshot")), s(record, "config_snapshot_sha256"));
        const auto profile = frozen.model_profile(Role::Worker);
        result.call_id = new_id("call"); result.attempt_id = new_id("attempt"); result.config_snapshot_sha256 = frozen.sha256();
        result.model_ref = frozen.resolve(Role::Worker).model; result.reserved_input_tokens = 100; result.reserved_output_tokens = 50; result.max_output_tokens = 50;
        ContextInput context; context.role = Role::Worker; context.model = frozen.resolve(Role::Worker);
        context.adapter_version = s(profile, "adapter_version"); context.protocol_version = s(profile, "protocol");
        context.system_prompt = "Bounded event Store fixture."; context.role_prompt = "Only Worker executes tools.";
        context.function_prompt = "Preserve exact original observations."; context.task_contract = "fixture context";
        ModelRequest request; request.call_id = result.call_id; request.attempt_id = result.attempt_id; request.role_call_index = 3;
        request.role = Role::Worker; request.binding = binding(node); request.model = context.model; request.config_snapshot_id = frozen.sha256();
        request.context = ContextCompiler().compile(context); request.max_output_tokens = 50; request.reserved_input_tokens = 100; request.reserved_output_tokens = 50;
        result.request_envelope = to_json(request); return result;
    }
};
std::vector<std::int64_t> ids(const Json& claim) {
    std::vector<std::int64_t> result;
    for (const auto& item : claim.at("items").as_array()) result.push_back(n(item, "inbox_id"));
    return result;
}
void finish(Fixture& f, const StoreActor& actor, const Json& claim, const std::vector<std::int64_t>& done) {
    f.store->finish_events(actor, s(claim, "claim_id"), n(claim, "generation"), done);
}

void upgrade_rollback_and_readonly() {
    Fixture f(false, false);
    const auto before = f.store->request(f.request_id);
    CHECK(!f.store->event_runtime_enabled());
    error(ErrorCode::Unsupported, [&] { f.store->claim_events(f.actor(Role::Dispatcher)); });
    { Store observer(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql, events_sql);
      CHECK(!observer.event_runtime_enabled()); CHECK(observer.request(f.request_id) == before); }
    f.store.reset();
    error(ErrorCode::Storage, [&] { Store broken(f.path, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql,
        events_sql + "\nSELECT * FROM missing_upgrade_fixture;\n"); });
    CHECK(scalar(f.path, "PRAGMA user_version") == 403);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM sqlite_master WHERE name='event_inbox'") == 0);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events") == 1);
    f.reopen();
    CHECK(f.store->event_runtime_enabled()); CHECK(n(f.store->diagnostics(), "user_version") == 404);
    CHECK(f.store->request(f.request_id).at("config_snapshot") == before.at("config_snapshot"));
    CHECK(n(f.store->request(f.request_id), "dispatcher_epoch") == n(before, "dispatcher_epoch") + 1);
    CHECK(!f.store->inbox(f.actor(Role::Dispatcher)).empty());
    const auto current = f.store->request(f.request_id);
    { Store observer(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql, events_sql);
      CHECK(observer.request(f.request_id) == current);
      error(ErrorCode::Unauthorized, [&] { observer.claim_events(observer.host_actor(Role::Dispatcher, f.request_id)); }); }
    error(ErrorCode::Corrupt, [&] { Store observer(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql, events_sql + "\n-- drift"); });
    error(ErrorCode::Unsupported, [&] { Store observer(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql); });
}

void claim_partial_and_new_arrivals() {
    Fixture f; f.order();
    const auto worker = f.actor(Role::Worker); const auto first = f.store->claim_events(worker);
    CHECK(ids(first).size() == 1);
    error(ErrorCode::Conflict, [&] { f.store->claim_events(worker); });
    f.ack(); // New ack event is outside this captured claim.
    error(ErrorCode::Unauthorized, [&] { finish(f, worker, first, {ids(first).front() + 1000}); });
    finish(f, worker, first, ids(first));
    CHECK(f.store->inbox(worker).size() == 1);
    finish(f, worker, first, ids(first)); // Same closed receipt can replay.
    f.order();
    const auto second = f.store->claim_events(worker);
    CHECK(ids(second).size() == 2);
    CHECK(n(second, "generation") == n(first, "generation") + 1);
    finish(f, worker, second, {ids(second).front()});
    const auto pending = f.store->inbox(worker); CHECK(pending.size() == 1);
    CHECK(s(pending.front(), "status") == "PENDING");
    error(ErrorCode::Stale, [&] { finish(f, worker, first, ids(first)); });
    CHECK(f.store->inbox(f.actor(Role::Worker, "beta")).empty());
}

void competing_claims_have_one_owner() {
    Fixture f(false, false); f.reopen();
    const auto actor = f.actor(Role::Dispatcher); std::barrier gate(3);
    const auto operation = [&]() {
        gate.arrive_and_wait();
        try { return !s(f.store->claim_events(actor), "claim_id").empty(); }
        catch (const Error& e) { CHECK(e.code() == ErrorCode::Conflict); return false; }
    };
    auto first = std::async(std::launch::async, operation); auto second = std::async(std::launch::async, operation);
    gate.arrive_and_wait(); CHECK(first.get() != second.get());
}

void restart_releases_claims_and_fences_tokens() {
    Fixture f; f.order();
    const auto actor = f.actor(Role::Worker); const auto claim = f.store->claim_events(actor);
    const auto old_ids = ids(claim); f.reopen();
    error(ErrorCode::Stale, [&] { finish(f, actor, claim, old_ids); });
    const auto current = f.actor(Role::Worker);
    error(ErrorCode::Stale, [&] { finish(f, current, claim, old_ids); });
    const auto next_claim = f.store->claim_events(current);
    CHECK(n(next_claim, "generation") == n(claim, "generation") + 1);
    CHECK(ids(next_claim) == old_ids);
    CHECK(!f.store->inbox(f.actor(Role::TeamLead)).empty());
    finish(f, current, next_claim, {});
    CHECK(s(f.store->inbox(current).front(), "status") == "PENDING");
}

void atomic_initial_allocation_rolls_back_all_nodes() {
    Fixture f(true, false); const auto before = f.store->request(f.request_id);
    raw_exec(f.path, "CREATE TRIGGER fail_second_team BEFORE INSERT ON teams WHEN (SELECT COUNT(*) FROM teams)>=1 BEGIN SELECT RAISE(ABORT,'fixture rejects second team'); END;");
    error(ErrorCode::Conflict, [&] { f.store->apply_independent_plan(f.actor(Role::Dispatcher), f.rv(), 1, plan_body(), Object{}, "plan-fixture"); });
    CHECK(f.store->request(f.request_id) == before); CHECK(f.store->tasks(f.request_id).empty());
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM plans") == 0);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM event_inbox") == 1);
    raw_exec(f.path, "DROP TRIGGER fail_second_team;");
    const auto expected = f.rv(); const auto actor = f.actor(Role::Dispatcher);
    const auto result = f.store->apply_independent_plan(actor, expected, 1, plan_body(), Object{}, "plan-fixture");
    CHECK(result.at("tasks").as_array().size() == 2); CHECK(f.store->tasks(f.request_id).size() == 2);
    CHECK(f.store->apply_independent_plan(actor, expected, 1, plan_body(), Object{}, "plan-fixture") == result);
    CHECK(f.store->inbox(f.actor(Role::TeamLead)).size() == 1);
    CHECK(f.store->inbox(f.actor(Role::TeamLead, "beta")).size() == 1);
}

void independent_validation_prevents_hidden_changes() {
    Fixture f; const auto before = f.store->request(f.request_id);
    auto body = plan_body(); body.at("nodes").as_array()[1].at("node_id") = "alpha";
    error(ErrorCode::InvalidInput, [&] { f.store->apply_independent_plan(f.actor(Role::Dispatcher), f.rv(), 1, body, Object{}, new_id("plan")); });
    body = plan_body(); body.at("nodes").as_array()[1].at("depends_on") = Array{"alpha"};
    error(ErrorCode::Unsupported, [&] { f.store->apply_independent_plan(f.actor(Role::Dispatcher), f.rv(), 1, body, Object{}, new_id("plan")); });
    error(ErrorCode::Quota, [&] { f.store->apply_independent_plan(f.actor(Role::Dispatcher), f.rv(), 1, plan_body(5), Object{}, new_id("plan")); });
    error(ErrorCode::Unsupported, [&] { f.store->apply_independent_plan(f.actor(Role::Dispatcher), f.rv(), 1, plan_body(1), Object{}, new_id("plan")); });
    auto dispositions = decision("alpha"); dispositions.at("alpha").as_object()["expected_task_version"] = 1;
    error(ErrorCode::InvalidInput, [&] { f.store->apply_independent_plan(f.actor(Role::Dispatcher), f.rv(), 1, plan_body(), dispositions, new_id("plan")); });
    CHECK(f.store->request(f.request_id) == before);
}

void partial_amend_and_later_keep_share_one_plan() {
    Fixture f; f.ready(); f.ready("beta");
    const auto old_claim = f.store->claim_events(f.actor(Role::Dispatcher));
    f.amend();
    error(ErrorCode::Stale, [&] { finish(f, f.actor(Role::Dispatcher), old_claim, ids(old_claim)); });
    const auto dispatcher = f.actor(Role::Dispatcher); const auto claim = f.store->claim_events(dispatcher);
    const auto beta_before = f.task("beta");
    const auto alpha_before = f.task();
    const auto result = f.store->apply_independent_plan(dispatcher, f.rv(), 2, plan_body(), decision("alpha"), "amend-alpha");
    CHECK(n(result.at("tasks").as_array().front(), "expected_task_version") == n(alpha_before, "state_version"));
    CHECK(result.at("pending_tasks").as_array().size() == 1);
    CHECK(n(f.task(), "based_on_requirements_revision") == 2);
    CHECK(n(f.task("beta"), "based_on_requirements_revision") == 1);
    CHECK(n(f.task("beta"), "paused_for_change") == 1);
    std::vector<std::int64_t> completed;
    for (const auto& item : claim.at("items").as_array()) if (s(item, "subject_task_id") != f.tid("beta")) completed.push_back(n(item, "inbox_id"));
    finish(f, dispatcher, claim, completed);
    CHECK(f.store->inbox(dispatcher).size() == 1);
    CHECK(s(f.store->inbox(dispatcher).front(), "subject_task_id") == f.tid("beta"));
    error(ErrorCode::NotReady, [&] { f.store->issue_instruction(f.actor(Role::TeamLead, "beta"), f.binding("beta"), f.tv("beta"), brief(), new_id("blocked")); });
    const auto plan_revision = n(f.store->request(f.request_id), "plan_revision"); const auto expected = f.rv();
    const auto kept = f.store->apply_independent_dispositions(dispatcher, expected, 2, plan_revision, decision("beta", "keep"), "keep-beta");
    CHECK(n(kept.at("tasks").as_array().front(), "expected_task_version") == n(beta_before, "state_version"));
    CHECK(f.store->apply_independent_dispositions(dispatcher, expected, 2, plan_revision, decision("beta", "keep"), "keep-beta") == kept);
    CHECK(n(f.store->request(f.request_id), "plan_revision") == plan_revision);
    CHECK(n(f.task("beta"), "task_revision") == 2);
    error(ErrorCode::NotReady, [&] { f.prepare("beta"); });
    f.ready("beta"); CHECK(s(f.prepare("beta"), "status") == "PREPARED");
    error(ErrorCode::Conflict, [&] { f.store->apply_independent_dispositions(dispatcher, f.rv(), 2, plan_revision, decision("beta", "keep"), new_id("repeat")); });
}

void wait_blocks_only_new_business_and_reservations() {
    Fixture f; f.order();
    const auto original = f.binding(); const auto worker = f.actor(Role::Worker); const auto expected = f.tv();
    const auto wait = f.store->wait_worker(worker, original, expected, "need host answer", "wait-alpha");
    CHECK(f.store->wait_worker(worker, original, expected, "need host answer", "wait-alpha") == wait);
    f.ack(); // The control action needed for readiness is independent of wait.
    const auto before = f.store->budget(f.request_id);
    error(ErrorCode::NotReady, [&] { f.prepare(); });
    auto reserved = f.reservation();
    error(ErrorCode::NotReady, [&] { f.store->reserve_attempt(f.actor(Role::Worker), f.binding(), reserved); });
    CHECK(f.store->budget(f.request_id) == before);
    CHECK(s(f.store->authorize_diagnostic(f.actor(Role::Worker), f.binding(), "op_status"), "tool_id") == "op_status");
    f.ready("beta"); CHECK(s(f.prepare("beta"), "status") == "PREPARED");
    error(ErrorCode::Stale, [&] { f.store->wake_worker(f.actor(Role::Worker), f.binding(), n(wait, "generation") + 1, new_id("wake")); });
    CHECK(s(f.store->wake_worker(f.actor(Role::Worker), f.binding(), n(wait, "generation"), "wake-alpha"), "status") == "AWAKE");
    const auto op = f.prepare();
    const auto second = f.store->wait_worker(f.actor(Role::Worker), f.binding(), f.tv(), "pause another operation", new_id("wait"));
    CHECK(n(second, "generation") == n(wait, "generation") + 1);
    error(ErrorCode::NotReady, [&] { f.store->start_tool_operation(f.actor(Role::Worker), s(op, "op_id"), n(op, "state_version"), new_id("start")); });
    CHECK(s(f.store->execute_tool_control(f.actor(Role::Worker), f.binding(), f.tv(), "op_cancel", Object{{"op_id", s(op, "op_id")}}, new_id("cancel")), "status") == "CANCELLED");
}

void wait_survives_epoch_recovery_but_not_target_change() {
    Fixture f; f.ready();
    const auto original = f.binding();
    const auto wait = f.store->wait_worker(f.actor(Role::Worker), original, f.tv(), "inspect pending answer", new_id("wait"));
    f.reopen();
    CHECK(f.store->worker_wait(f.tid()).at("original_binding") == to_json(original));
    CHECK(s(f.store->worker_wait(f.tid()), "status") == "WAITING");
    const auto before_task = f.task();
    f.store->wake_worker(f.actor(Role::Worker), f.binding(), n(wait, "generation"), new_id("wake"));
    CHECK(n(f.task(), "paused_for_change") == 1);
    CHECK(f.task().at("applied_instruction_revision") == before_task.at("applied_instruction_revision"));
    error(ErrorCode::NotReady, [&] { f.prepare(); });
    f.ready();
    const auto next_wait = f.store->wait_worker(f.actor(Role::Worker), f.binding(), f.tv(), "new decision", new_id("wait"));
    f.order();
    CHECK(s(f.store->worker_wait(f.tid()), "status") == "SUPERSEDED");
    error(ErrorCode::Stale, [&] { f.store->wake_worker(f.actor(Role::Worker), f.binding(), n(next_wait, "generation"), new_id("wake")); });
    f.ack();
    const auto amended_wait = f.store->wait_worker(f.actor(Role::Worker), f.binding(), f.tv(), "wait before amendment", new_id("wait"));
    f.amend();
    CHECK(s(f.store->worker_wait(f.tid()), "status") == "SUPERSEDED");
    error(ErrorCode::Stale, [&] { f.store->wake_worker(f.actor(Role::Worker), f.binding(), n(amended_wait, "generation"), new_id("wake")); });
}

void wake_does_not_reconcile_unknown_effect() {
    Fixture f; f.ready(); const auto prepared = f.prepare();
    const auto started = f.store->start_tool_operation(f.actor(Role::Worker), s(prepared, "op_id"), n(prepared, "state_version"), new_id("start"));
    CHECK(started.at("dispatch_claimed").as_bool());
    const auto wait = f.store->wait_worker(f.actor(Role::Worker), f.binding(), f.tv(), "wait during uncertain operation", new_id("wait"));
    f.store->execute_tool_control(f.actor(Role::Worker), f.binding(), f.tv(), "op_cancel", Object{{"op_id", s(prepared, "op_id")}}, new_id("cancel"));
    CHECK(s(f.store->operation(s(prepared, "op_id")), "status") == "UNKNOWN");
    f.store->wake_worker(f.actor(Role::Worker), f.binding(), n(wait, "generation"), new_id("wake"));
    error(ErrorCode::UnknownOutcome, [&] { f.prepare(); });
    CHECK(s(f.store->operation(s(prepared, "op_id")), "status") == "UNKNOWN");
}

Json response_for(const AttemptReservation& reservation, const ExecutionBinding& binding) {
    ModelResponse response;
    response.call_id = reservation.call_id; response.attempt_id = reservation.attempt_id;
    response.role = Role::Worker; response.role_call_index = 3; response.binding = binding;
    response.config_snapshot_id = reservation.config_snapshot_sha256; response.status = ModelResultStatus::Completed;
    response.tool_calls.push_back({"original-diagnostic", "op_status", Object{{"op_id", "recorded-fixture"}}});
    response.usage.input_tokens = 10; response.usage.output_tokens = 4; response.usage.cost_micro_usd = 0;
    return to_json(response);
}
void settle(Fixture& f, const AttemptReservation& reservation, const ExecutionBinding& binding, const Json& response) {
    f.store->settle_attempt(reservation.attempt_id, binding, reservation.config_snapshot_sha256, "COMPLETED",
        response.at("usage"), response);
}

void feedback_preserves_original_facts_and_missing_observations() {
    Fixture f; f.ready(); f.ready("beta");
    const auto binding = f.binding(); const auto reservation = f.reservation();
    const auto worker = f.actor(Role::Worker);
    const auto inbox_before = f.store->inbox(worker).size();
    f.store->reserve_attempt(worker, binding, reservation);
    const auto response = response_for(reservation, binding);
    const Json result = Object{{"tool", "op_status"}, {"original_observation", "原始 bytes\n"}, {"arguments", Object{{"offset", 0}, {"limit", 6}}}};
    error(ErrorCode::NotReady, [&] { f.store->record_tool_feedback(reservation.attempt_id, binding, response, result); });
    settle(f, reservation, binding, response);
    CHECK(f.store->inbox(worker).size() == inbox_before); // reserve/settle do not self-wake.
    const auto feedback = f.store->record_tool_feedback(reservation.attempt_id, binding, response, result);
    CHECK(f.store->inbox(worker).size() == inbox_before + 1);
    CHECK(f.store->record_tool_feedback(reservation.attempt_id, binding, response, result) == feedback);
    CHECK(f.store->tool_feedback(reservation.attempt_id) == feedback);
    error(ErrorCode::Conflict, [&] { f.store->record_tool_feedback(reservation.attempt_id, binding, response, Object{{"changed", true}}); });
    auto changed = binding; ++changed.instruction_revision;
    error(ErrorCode::Conflict, [&] { f.store->record_tool_feedback(reservation.attempt_id, changed, response, result); });
    const auto beta = f.reservation("beta"); const auto beta_binding = f.binding("beta");
    f.store->reserve_attempt(f.actor(Role::Worker, "beta"), beta_binding, beta);
    const auto beta_response = response_for(beta, beta_binding); settle(f, beta, beta_binding, beta_response);
    f.store->record_tool_feedback(beta.attempt_id, beta_binding, beta_response, Object{{"private_beta", true}});
    const auto missing = f.reservation(); f.store->reserve_attempt(worker, binding, missing);
    auto history = f.store->attempt_history(worker); CHECK(history.size() == 2);
    CHECK(history.front().at("feedback") == feedback); CHECK(history.front().at("frozen_binding") == to_json(binding));
    CHECK(n(history.front(), "role_call_index") == 3); CHECK(!history.front().as_object().contains("request_envelope"));
    CHECK(history.back().at("feedback_missing").as_bool()); CHECK(history.back().at("response").is_null());
    CHECK(f.store->attempt_history(f.actor(Role::Worker, "beta")).size() == 1);
    error(ErrorCode::Quota, [&] { f.store->attempt_history(worker, 1); });
    f.reopen(); history = f.store->attempt_history(f.actor(Role::Worker));
    CHECK(history.front().at("frozen_binding") == to_json(binding));
    CHECK(history.front().at("feedback") == feedback);
    CHECK(s(history.back(), "status") == "UNKNOWN"); CHECK(history.back().at("feedback_missing").as_bool());
    CHECK(f.store->attempt(reservation.attempt_id).at("request_envelope") == reservation.request_envelope);
}

void late_feedback_and_event_failure_keep_original_binding() {
    Fixture f; f.ready(); const auto original = f.binding(); const auto reservation = f.reservation();
    f.store->reserve_attempt(f.actor(Role::Worker), original, reservation);
    const auto response = response_for(reservation, original); f.amend(); settle(f, reservation, original, response);
    CHECK(n(f.store->attempt(reservation.attempt_id), "accepted") == 0);
    raw_exec(f.path, "CREATE TRIGGER fail_feedback_event BEFORE INSERT ON events WHEN NEW.type='tool_feedback' BEGIN SELECT RAISE(ABORT,'fixture event failure'); END;");
    const Json result = Object{{"runtime_error", true}, {"error_code", "stale"}, {"message", "original attempt is stale"}};
    error(ErrorCode::Conflict, [&] { f.store->record_tool_feedback(reservation.attempt_id, original, response, result); });
    CHECK(f.store->tool_feedback(reservation.attempt_id).is_null());
    raw_exec(f.path, "DROP TRIGGER fail_feedback_event;");
    const auto feedback = f.store->record_tool_feedback(reservation.attempt_id, original, response, result);
    CHECK(feedback.at("original_binding") == to_json(original));
    CHECK(n(f.store->attempt(reservation.attempt_id), "accepted") == 0);
    CHECK(f.store->attempt_history(f.actor(Role::Worker)).front().at("response") == response);
    auto altered = response; altered.at("text") = "invented later response";
    error(ErrorCode::Conflict, [&] { f.store->record_tool_feedback(reservation.attempt_id, original, altered, result); });
    test::throws<std::runtime_error>([&] { raw_exec(f.path, "UPDATE model_tool_feedback SET result='{}';"); });
}

void populated_403_upgrade_retains_operations_and_calls() {
    Fixture f(false, false);
    f.store->publish_plan(f.actor(Role::Dispatcher), f.rv(), 1, plan_body(), new_id("plan"));
    f.store->create_task(f.actor(Role::Dispatcher), f.rv(), 1, 1, "alpha", brief(), new_id("task"));
    f.ready(); const auto original = f.binding(); const auto prepared = f.prepare();
    f.store->start_tool_operation(f.actor(Role::Worker), s(prepared, "op_id"), n(prepared, "state_version"), new_id("start"));
    const auto operation_fact = f.store->record_operation_result(s(prepared, "op_id"), original, "SUCCEEDED", Object{{"original", "effect observed"}}, Object{{"proof", "fixture"}});
    const auto reservation = f.reservation(); const auto response = response_for(reservation, original);
    f.store->reserve_attempt(f.actor(Role::Worker), original, reservation); settle(f, reservation, original, response);
    const auto attempt = f.store->attempt(reservation.attempt_id);
    const auto snapshot = f.store->request_snapshot(f.request_id);
    f.reopen();
    CHECK(f.store->operation_result(s(prepared, "op_id")) == operation_fact);
    CHECK(f.store->attempt(reservation.attempt_id) == attempt);
    CHECK(f.store->request_snapshot(f.request_id).at("texts") == snapshot.at("texts"));
    CHECK(!f.store->inbox(f.actor(Role::TeamLead)).empty());
    const auto history = f.store->attempt_history(f.actor(Role::Worker));
    CHECK(history.front().at("feedback_missing").as_bool());
    CHECK(history.front().at("response") == response);
    CHECK(history.front().at("frozen_binding") == to_json(original));
}

void ordinary_event_capacity_preserves_settlement_controls() {
    Fixture f; f.ready(); const auto original = f.binding(); const auto prepared = f.prepare();
    f.store->start_tool_operation(f.actor(Role::Worker), s(prepared, "op_id"), n(prepared, "state_version"), new_id("start"));
    const auto reservation = f.reservation(); const auto response = response_for(reservation, original);
    f.store->reserve_attempt(f.actor(Role::Worker), original, reservation);
    const auto wait = f.store->wait_worker(f.actor(Role::Worker), original, f.tv(), "pause active fixture", new_id("wait"));
    raw_exec(f.path, "WITH RECURSIVE counter(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM counter WHERE n<4096) INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) SELECT 'fixture-event-'||n,request_id,'fixture','runtime','fixture-command-'||n,'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','{}','{}','fixture' FROM counter,requests WHERE n<=4096-(SELECT COUNT(*) FROM events);");
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events") == 4096);
    const auto request_before = f.store->request(f.request_id);
    error(ErrorCode::Quota, [&] { f.store->publish_plan(f.actor(Role::Dispatcher), f.rv(), 1, plan_body(), new_id("full")); });
    CHECK(f.store->request(f.request_id) == request_before);
    settle(f, reservation, original, response);
    f.store->record_tool_feedback(reservation.attempt_id, original, response, Object{{"complete", true}});
    f.store->execute_tool_control(f.actor(Role::Worker), original, f.tv(), "op_cancel", Object{{"op_id", s(prepared, "op_id")}}, new_id("cancel"));
    const auto fact = f.store->record_operation_result(s(prepared, "op_id"), original, "SUCCEEDED", Object{{"late", true}}, Object{{"proof", "retained"}});
    CHECK(n(fact, "accepted") == 0);
    CHECK(s(f.store->wake_worker(f.actor(Role::Worker), original, n(wait, "generation"), new_id("wake")), "status") == "AWAKE");
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events") == 4101);
    CHECK(s(f.store->operation(s(prepared, "op_id")), "status") == "UNKNOWN");
}

void full_request_inbox_includes_retired_actors() {
    Fixture f; f.order();
    const auto before = f.store->request_inbox(f.request_id);
    CHECK(before.size() >= 4);
    error(ErrorCode::Quota, [&] { f.store->request_inbox(f.request_id, 1); });
    f.store->cancel_request(f.actor(Role::Root), f.rv(), new_id("cancel"));
    CHECK(f.store->request_inbox(f.request_id).size() == before.size());
    raw_exec(f.path, "UPDATE teams SET status='STOPPED',state_version=state_version+1;");
    CHECK(f.store->request_inbox(f.request_id).size() == before.size());
}
}

int main(int argc, char** argv) {
    source_root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(WLS_SOURCE_DIR);
    try {
        base_sql = read(source_root / "db/schema.sql"); store_sql = read(source_root / "db/migrations/004_s1_store.sql");
        tools_sql = read(source_root / "db/migrations/005_worker_tools.sql"); events_sql = read(source_root / "db/migrations/006_event_runtime.sql");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return test::run({
        {"403 upgrade rollback, immutable snapshots and read-only 404 fingerprint guards", upgrade_rollback_and_readonly},
        {"captured IDs complete partially and later arrivals remain pending", claim_partial_and_new_arrivals},
        {"competing role claims acquire one durable owner", competing_claims_have_one_owner},
        {"takeover releases captured items and fences old tokens", restart_releases_claims_and_fences_tokens},
        {"independent initial allocation rolls back every plan/team/event on failure", atomic_initial_allocation_rolls_back_all_nodes},
        {"independent plan guards reject duplicate, dependent and identity-changing nodes", independent_validation_prevents_hidden_changes},
        {"AMEND omission remains pending and later keep uses the same plan", partial_amend_and_later_keep_share_one_plan},
        {"Worker wait gates new effects and reservations while controls remain available", wait_blocks_only_new_business_and_reservations},
        {"wait survives epoch-only recovery and rejects changed semantic targets", wait_survives_epoch_recovery_but_not_target_change},
        {"wake never reconciles an UNKNOWN effect", wake_does_not_reconcile_unknown_effect},
        {"immutable scoped history retains complete observations and explicit missing results", feedback_preserves_original_facts_and_missing_observations},
        {"late feedback retains its binding and failed feedback/event transaction rolls back", late_feedback_and_event_failure_keep_original_binding},
        {"populated 403 upgrade preserves original operation and model accounting facts", populated_403_upgrade_retains_operations_and_calls},
        {"ordinary event exhaustion preserves feedback, settlement, cancel and wake controls", ordinary_event_capacity_preserves_settlement_controls},
        {"request inbox reports unhandled events for retired actors without truncation", full_request_inbox_includes_retired_actors}
    });
}
