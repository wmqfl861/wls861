#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <memory>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif
namespace {
using namespace wls;
using namespace wls::s1;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::unique_ptr<SchemaRegistry> schemas;
std::string base_sql, store_sql, tools_sql, events_sql, workflow_sql, acceptance_sql;
std::string text(const Json& v) { return std::string(v.as_string()); }
std::int64_t number(const Json& v) { return v.is_int64() ? v.as_int64() : static_cast<std::int64_t>(v.as_uint64()); }
Json fixture(const std::string& name) { return parse_json_strict(read_runtime_file(source / "tests/fixtures" / name)); }
void expect(ErrorCode code, const std::function<void()>& fn) {
    try { fn(); } catch (const Error& e) {
        if (e.code() != code) throw std::runtime_error("expected " + std::string(error_name(code)) + ", got " + std::string(error_name(e.code())) + ": " + e.what());
        return;
    }
    throw std::runtime_error("expected guarded rejection");
}
Json decision(std::string_view action = "rework") { return Object{{"action", action}, {"reason", "Repair the original candidate; retain all criteria."}}; }
FakeModelStep step(Role role, const std::string& tool, const Json& args) {
    FakeModelStep s; s.role = role; s.role_call_index = 1;
    s.tool_calls.push_back({"r2-call", tool, args}); s.usage = {100, 20, 0, 0, 0}; return s;
}
AdmissionInput input(std::int64_t limit = 3) {
    test::TempDir configuration;
    auto yaml = read_runtime_file(source / "config/examples/system.yaml");
    const std::string marker = "team_internal_rework_max: 3";
    const auto position = yaml.find(marker); CHECK(position != std::string::npos);
    yaml.replace(position, marker.size(), "team_internal_rework_max: " + std::to_string(limit));
    { std::ofstream file(configuration.path / "system.yaml"); file << yaml; CHECK(static_cast<bool>(file)); }
    const auto config = load_config_catalog(configuration.path / "system.yaml", source / "config/examples/functions", source / "prompts").freeze("general");
    AdmissionInput a; a.tenant_id = "test-tenant"; a.project_id = "test-project"; a.root_id = "test-root";
    a.function_id = config.function_id(); a.idempotency_key = new_id("key");
    a.original_text = read_runtime_file(source / "tests/fixtures/s13_request.txt");
    a.config_snapshot = config.json();
    a.constraint_catalog = fixture("s13_constraints.json");
    const auto grants = config.worker_tools(); a.tool_allowlist.assign(grants.begin(), grants.end());
    a.budget.input_tokens = 20000000; a.budget.output_tokens = 1000000; return a;
}
struct Fixture {
    test::TempDir directory;
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> tools;
    std::string id;
    explicit Fixture(std::int64_t limit = 3) {
        std::filesystem::create_directory(directory.path / "work"); reopen();
        id = text(store->admit(input(limit)).at("request_id"));
    }
    void reopen() {
        tools.reset(); store.reset();
        store = std::make_unique<Store>(directory.path / "control.db", base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql, workflow_sql, acceptance_sql);
        tools = std::make_unique<WorkerTools>(*store, directory.path / "work");
    }
    EventRuntime runtime() { return EventRuntime(*store, *schemas, {{"alpha", tools.get()}}); }
    Json task() const { return store->tasks(id).front(); }
    ExecutionBinding binding() const { return store->binding(text(task().at("task_id"))); }
    Json target() const { return store->current_submission(binding().task_id); }
    StoreActor dispatcher() const { return store->host_actor(Role::Dispatcher, id); }
    Json invoke(Role role, const std::string& tool, const Json& args) {
        const FakeModel fake({step(role, tool, args)});
        OfflineRuntime runtime_(*store, *schemas, tools.get());
        const auto call = runtime_.reserve_call(fake, id, role, role == Role::Dispatcher ? std::string{} : binding().task_id);
        return runtime_.accept_response(call, fake.complete(call.request));
    }
    CapturedModelCall capture(const FakeModel& fake) { return OfflineRuntime(*store, *schemas, tools.get()).reserve_call(fake, id, Role::Dispatcher); }
    void candidate(std::int64_t calls = 5) {
        EventPumpOptions opts; opts.max_calls = calls; opts.max_worker_calls_per_task = 2;
        const auto result = runtime().pump(id, event_fake_script_from_json(fixture("s15_fake_complete.json")), opts);
        CHECK(result.at("calls").as_array().size() == static_cast<std::size_t>(calls));
    }
    void reject() { candidate(); (void)invoke(Role::Dispatcher, "accept_candidate", Object{{"passed", false}, {"notes", "Original candidate rejected."}}); CHECK(task().at("status") == "BLOCKED"); }
    Json resolve(const Json& body = decision(), const std::string& key = new_id("resolve")) {
        return store->resolve_blocked_submission(dispatcher(), store->request_binding(id), number(task().at("state_version")), text(target().at("submission_id")), number(target().at("state_version")), body, key);
    }
    void answer(const Json& q) {
        (void)store->answer_question(store->host_actor(Role::Root, id), store->request_binding(id), text(q.at("question_id")), number(q.at("state_version")), Object{{"text", "Please reconsider, without extra quota."}}, new_id("answer"));
    }
    void sql(const std::string& statement) {
        sqlite3* db = nullptr; CHECK(sqlite3_open((directory.path / "control.db").c_str(), &db) == SQLITE_OK);
        char* error = nullptr; const auto rc = sqlite3_exec(db, statement.c_str(), nullptr, nullptr, &error);
        const std::string message = error ? error : ""; sqlite3_free(error); sqlite3_close(db);
        if (rc != SQLITE_OK) throw std::runtime_error(message);
    }
};
void capture_preserves_original_rejection() {
    Fixture f; f.reject(); const auto action = f.store->blocked_submission_action(f.binding().task_id);
    CHECK(action.at("kind") == "blocked_submission" && action.at("rework_available") == true);
    CHECK(action.at("rejection").at("decision").at("passed") == false);
    CHECK(action.at("target").at("artifact_snapshot") == f.target().at("artifact_snapshot"));
    const FakeModel fake({step(Role::Dispatcher, "resolve_blocked_submission", decision())});
    const auto call = f.capture(fake);
    CHECK(call.workflow_action.at("target").at("record_sha256") == f.target().at("record_sha256"));
    CHECK(call.request.context.dynamic_suffix.find("Original candidate rejected") != std::string::npos);
}
void rework_preserves_brief_and_requires_new_order_ack() {
    Fixture f; f.reject(); const auto task = f.task(), submission = f.target(); const auto binding = f.binding();
    const auto result = f.resolve(); CHECK(result.at("status") == "AWAITING_TEAM_LEAD_ORDER");
    CHECK(f.task().at("brief") == task.at("brief"));
    CHECK(f.binding().task_revision == binding.task_revision + 1 && f.binding().instruction_revision == binding.instruction_revision);
    CHECK(f.binding().plan_revision == binding.plan_revision && f.binding().requirements_revision == binding.requirements_revision);
    CHECK(f.store->submission(text(submission.at("submission_id"))).at("artifact_snapshot") == submission.at("artifact_snapshot"));
    CHECK(f.store->rework_request(binding.task_id).at("reason") == decision().at("reason"));
    expect(ErrorCode::Stale, [&] { (void)f.store->acknowledge(f.store->host_actor(Role::Worker, f.id, binding.team_id), f.binding(), number(f.task().at("state_version")), new_id("ack")); });
    const auto order = fixture("s14_fake_submit.json").at("nodes").at("alpha").as_array().front().at("tool_calls").as_array().front().at("arguments");
    (void)f.invoke(Role::TeamLead, "issue_work_order", order);
    CHECK(f.binding().instruction_revision == binding.instruction_revision + 1);
    (void)f.store->acknowledge(f.store->host_actor(Role::Worker, f.id, binding.team_id), f.binding(), number(f.task().at("state_version")), new_id("ack"));
    CHECK(f.task().at("applied_task_revision") == f.task().at("task_revision"));
}
void replay_is_exact_and_not_new_permission() {
    Fixture f; f.reject(); const auto a = f.dispatcher(); const auto b = f.store->request_binding(f.id);
    const auto t = number(f.task().at("state_version")), v = number(f.target().at("state_version")); const auto id = text(f.target().at("submission_id"));
    const auto key = new_id("resolve"); const auto result = f.store->resolve_blocked_submission(a,b,t,id,v,decision(),key);
    const auto task = f.task(); CHECK(f.store->resolve_blocked_submission(a,b,t,id,v,decision(),key) == result); CHECK(f.task() == task);
    expect(ErrorCode::Conflict,[&] { (void)f.store->resolve_blocked_submission(a,b,t,id,v,decision("ask_user"),key); });
    expect(ErrorCode::Stale,[&] { (void)f.store->resolve_blocked_submission(a,b,t,id,v,decision(),new_id("other")); });
}
void captured_cas_cannot_be_changed() {
    Fixture f; f.reject(); const auto task = f.task(); const auto submission = f.target();
    for (const bool change_task : {true, false}) expect(ErrorCode::Conflict,[&] {
        (void)f.store->resolve_blocked_submission(f.dispatcher(), f.store->request_binding(f.id), number(task.at("state_version")) + (change_task ? 1 : 0), text(submission.at("submission_id")), number(submission.at("state_version")) + (change_task ? 0 : 1), decision(),new_id("cas"));
    });
    CHECK(f.task() == task && f.target() == submission);
}
void quota_has_one_question_without_budget_reset() {
    Fixture f(0); f.reject(); expect(ErrorCode::Quota,[&] { (void)f.resolve(); });
    const auto result = f.resolve(decision("ask_user")); auto q = f.store->question(text(result.at("question_id")));
    CHECK(q.at("asked_by") == "dispatcher" && q.at("holder") == "user");
    CHECK(f.store->blocked_submission_action(f.binding().task_id).is_null());
    f.answer(q); CHECK(f.task().at("status") == "BLOCKED");
    CHECK(f.store->blocked_submission_action(f.binding().task_id).is_null());
    expect(ErrorCode::Quota,[&] { (void)f.resolve(); });
    expect(ErrorCode::NotReady,[&] { (void)f.resolve(decision("ask_user")); });
    CHECK(f.store->questions(f.id).size() == 1);
    f.reopen(); CHECK(f.store->blocked_submission_action(f.binding().task_id).is_null());
    CHECK(f.target().at("team_rework_count") == 0);
}
void answered_question_with_room_exposes_original_target() {
    Fixture f; f.reject(); const auto id = text(f.target().at("submission_id"));
    const auto qid = text(f.resolve(decision("ask_user")).at("question_id")); f.answer(f.store->question(qid));
    const auto action = f.store->blocked_submission_action(f.binding().task_id);
    CHECK(text(action.at("target").at("submission_id")) == id && action.at("followup_question").at("status") == "ANSWERED");
    CHECK(action.at("ask_user_available") == false);
    (void)f.resolve(); CHECK(f.task().at("status") == "READY");
}
void current_user_wait_blocks_rework() {
    Fixture f; f.reject(); (void)f.resolve(decision("ask_user")); const auto task = f.task();
    expect(ErrorCode::NotReady,[&] { (void)f.resolve(); }); CHECK(f.task() == task);
}
void event_failure_rolls_back_entire_followup() {
    for (const auto* action : {"rework", "ask_user"}) {
        Fixture f; f.reject(); const auto task = f.task(), submission = f.target();
        f.sql("CREATE TRIGGER r2_fail BEFORE INSERT ON events WHEN NEW.type='resolve_blocked_submission' BEGIN SELECT RAISE(ABORT,'r2 injected failure'); END;");
        expect(ErrorCode::Conflict,[&] { (void)f.resolve(decision(action)); });
        CHECK(f.task() == task && f.target() == submission && f.store->questions(f.id).empty());
        f.sql("DROP TRIGGER r2_fail;"); (void)f.resolve(decision(action));
    }
}
void model_cannot_supply_identity_or_change_brief() {
    Fixture f; f.reject(); const auto task = f.task();
    for (const auto* key : {"submission_id", "task_id", "state_version", "brief", "rework_limit"}) {
        auto args = decision(); args.as_object()[key] = "forged";
        expect(ErrorCode::InvalidInput,[&] { (void)f.invoke(Role::Dispatcher,"resolve_blocked_submission",args); });
        CHECK(f.task() == task);
    }
    expect(ErrorCode::InvalidInput,[&] { (void)f.invoke(Role::Dispatcher,"resolve_blocked_submission",Object{{"action","rework"},{"reason"," \n\t"}}); });
}
void amend_fences_captured_followup_but_accounts_response() {
    Fixture f; f.reject(); const auto submission = f.target();
    const FakeModel fake({step(Role::Dispatcher,"resolve_blocked_submission",decision())}); const auto call = f.capture(fake);
    (void)f.store->amend(f.store->host_actor(Role::Root,f.id),number(f.store->request(f.id).at("state_version")),read_runtime_file(source/"tests/fixtures/s13_amendment.txt"),fixture("s13_amended_constraints.json"),new_id("amend"));
    expect(ErrorCode::Stale,[&] { (void)OfflineRuntime(*f.store,*schemas,f.tools.get()).accept_response(call,fake.complete(call.request)); });
    CHECK(f.store->attempt(call.request.attempt_id).at("status") == "STALE");
    CHECK(!f.store->tool_feedback(call.request.attempt_id).is_null());
    CHECK(f.store->submission(text(submission.at("submission_id"))).at("artifact_snapshot") == submission.at("artifact_snapshot"));
    CHECK(f.store->blocked_submission_action(f.binding().task_id).is_null());
}
void reopen_fences_old_response_and_preserves_rework_count() {
    Fixture f; f.reject(); const FakeModel fake({step(Role::Dispatcher,"resolve_blocked_submission",decision())}); const auto call = f.capture(fake);
    f.reopen(); expect(ErrorCode::Stale,[&] { (void)OfflineRuntime(*f.store,*schemas,f.tools.get()).accept_response(call,fake.complete(call.request)); });
    (void)f.resolve(); f.reopen(); CHECK(f.store->rework_request(f.binding().task_id).at("rework_count") == 1);
    CHECK(f.task().at("status") == "READY");
}
void wrong_role_and_other_request_have_no_authority() {
    Fixture f; f.reject(); const auto task = f.task(), submission = f.target();
    const auto other = text(f.store->admit(input()).at("request_id"));
    expect(ErrorCode::Unauthorized,[&] { (void)f.store->resolve_blocked_submission(f.store->host_actor(Role::Dispatcher,other),f.store->request_binding(other),number(task.at("state_version")),text(submission.at("submission_id")),number(submission.at("state_version")),decision(),new_id("cross")); });
    expect(ErrorCode::Unauthorized,[&] { (void)f.store->resolve_blocked_submission(f.store->host_actor(Role::TeamLead,f.id,f.binding().team_id),f.binding(),number(task.at("state_version")),text(submission.at("submission_id")),number(submission.at("state_version")),decision(),new_id("role")); });
    CHECK(f.task() == task);
}
void unknown_effect_blocks_rework_not_user_escalation() {
    Fixture f; f.reject();
    // Fault injection only in this private throwaway DB simulates a later
    // adverse settlement; no production trigger or permission is disabled.
    f.sql("UPDATE ops SET status='UNKNOWN',state_version=state_version+1 WHERE status='SUCCEEDED'");
    expect(ErrorCode::UnknownOutcome,[&] { (void)f.resolve(); });
    const auto result = f.resolve(decision("ask_user")); CHECK(!result.at("question_id").is_null());
    CHECK(f.task().at("status") == "BLOCKED");
}
void team_lead_exhaustion_is_a_dispatcher_target() {
    Fixture f(0); f.candidate(4);
    const auto order = fixture("s14_fake_submit.json").at("nodes").at("alpha").as_array().front().at("tool_calls").as_array().front().at("arguments");
    (void)f.invoke(Role::TeamLead,"check_submission",Object{{"passed",false},{"notes","Local check failed at frozen cap."},{"work_order",order}});
    const auto action = f.store->blocked_submission_action(f.binding().task_id);
    CHECK(!action.is_null() && action.at("rework_available") == false && action.at("rejection").is_null());
    CHECK(!f.resolve(decision("ask_user")).at("question_id").is_null());
}
void stale_policy_response_cannot_mutate_but_current_control_can() {
    Fixture f; f.reject(); const FakeModel fake({step(Role::Dispatcher,"resolve_blocked_submission",decision())});
    const auto call = f.capture(fake);
    f.sql("UPDATE runtime_policy SET policy_epoch=policy_epoch+1,denied_tools='[\"note\"]',state_version=state_version+1 WHERE singleton=1;");
    expect(ErrorCode::Stale,[&] { (void)OfflineRuntime(*f.store,*schemas,f.tools.get()).accept_response(call,fake.complete(call.request)); });
    CHECK(f.task().at("status") == "BLOCKED");
    CHECK(!f.resolve(decision("ask_user")).at("question_id").is_null());
}
void cancelled_request_never_restarts_rejected_work() {
    Fixture f; f.reject(); const FakeModel fake({step(Role::Dispatcher,"resolve_blocked_submission",decision())});
    const auto call = f.capture(fake);
    (void)f.store->cancel_request(f.store->host_actor(Role::Root,f.id),number(f.store->request(f.id).at("state_version")),new_id("cancel"));
    expect(ErrorCode::Stale,[&] { (void)OfflineRuntime(*f.store,*schemas,f.tools.get()).accept_response(call,fake.complete(call.request)); });
    CHECK(f.store->request(f.id).at("status") == "CANCELLED");
    CHECK(f.store->blocked_submission_action(f.binding().task_id).is_null());
}

}
int main() {
    schemas = std::make_unique<SchemaRegistry>(source/"schemas");
    base_sql = read_runtime_file(source/"db/schema.sql");
    store_sql = read_runtime_file(source/"db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source/"db/migrations/005_worker_tools.sql");
    events_sql = read_runtime_file(source/"db/migrations/006_event_runtime.sql");
    workflow_sql = read_runtime_file(source/"db/migrations/007_workflow_runtime.sql");
    acceptance_sql = read_runtime_file(source/"db/migrations/008_acceptance_runtime.sql");
    return test::run({
        {"capture retains original rejection and snapshot", capture_preserves_original_rejection},
        {"rework preserves brief and needs new TL order and ack", rework_preserves_brief_and_requires_new_order_ack},
        {"command replay is exact and not new permission", replay_is_exact_and_not_new_permission},
        {"both captured CAS guards reject mutation", captured_cas_cannot_be_changed},
        {"exhaustion escalates once without budget reset", quota_has_one_question_without_budget_reset},
        {"answer with room exposes original target", answered_question_with_room_exposes_original_target},
        {"pending user wait blocks rework", current_user_wait_blocks_rework},
        {"event failure rolls back state count and question", event_failure_rolls_back_entire_followup},
        {"model cannot forge identity brief or quota", model_cannot_supply_identity_or_change_brief},
        {"AMEND fences followup and accounts stale response", amend_fences_captured_followup_but_accounts_response},
        {"restart fences old response and preserves count", reopen_fences_old_response_and_preserves_rework_count},
        {"cross request and wrong role denied", wrong_role_and_other_request_have_no_authority},
        {"unknown effect prevents rework not escalation", unknown_effect_blocks_rework_not_user_escalation},
        {"local rework exhaustion has explicit Dispatcher exit", team_lead_exhaustion_is_a_dispatcher_target},
        {"stale policy response fenced but new control remains available", stale_policy_response_cannot_mutate_but_current_control_can},
        {"cancelled request cannot restart rejected work", cancelled_request_never_restarts_rejected_work}
    });
}
