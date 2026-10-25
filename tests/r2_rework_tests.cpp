#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"
#include "capacity_test_support.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <atomic>

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

std::string text(const Json& value) { return std::string(value.as_string()); }
std::int64_t number(const Json& value) {
    return value.is_int64() ? value.as_int64() : static_cast<std::int64_t>(value.as_uint64());
}
Json fixture(const std::string& name) { return parse_json_strict(read_runtime_file(source / "tests/fixtures" / name)); }
void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary); output << bytes; CHECK(static_cast<bool>(output));
}
void expect(ErrorCode code, const std::function<void()>& action) {
    bool caught = false;
    try { action(); } catch (const Error& error) {
        if (error.code() != code) throw std::runtime_error(std::string("expected ") + std::string(error_name(code)) +
            ", received " + std::string(error_name(error.code())) + ": " + error.what());
        caught = true;
    }
    CHECK(caught);
}
AdmissionInput input() {
    const auto config = load_config_catalog(source / "config/examples/system.yaml", source / "config/examples/functions",
                                            source / "prompts").freeze("general");
    AdmissionInput value;
    value.tenant_id = "test-tenant"; value.project_id = "test-project"; value.root_id = "test-root";
    value.function_id = config.function_id(); value.idempotency_key = new_id("key");
    value.original_text = read_runtime_file(source / "tests/fixtures/s13_request.txt");
    value.config_snapshot = config.json(); value.constraint_catalog = fixture("s13_constraints.json");
    const auto grants = config.worker_tools(); value.tool_allowlist.assign(grants.begin(), grants.end());
    value.budget.input_tokens = 20000000; value.budget.output_tokens = 1000000;
    return value;
}
Json positive_check() {
    return Object{{"passed", true}, {"notes", "Checked the complete immutable file snapshot."},
        {"criteria_results", Array{Object{{"criterion_id", "ac1"}, {"met", "yes"}}}}};
}
Json positive_acceptance() {
    return Object{{"passed", true}, {"notes", "Explicit Dispatcher judgment on the current original requirement and candidate."},
        {"criteria_results", Array{Object{{"criterion_id", "ac1"}, {"met", "yes"},
            {"evidence", "The preserved result.txt bytes and their original binding satisfy the registered criterion."}}}}};
}
Json no_experience() {
    return Object{{"no_new_experience", true}, {"entries", Array{}}, {"memory_assessments", Array{}}};
}
FakeModelStep step(Role role, const std::string& tool, const Json& arguments, std::int64_t index = 1) {
    FakeModelStep value; value.role = role; value.role_call_index = index;
    value.tool_calls.push_back({"acceptance-action", tool, arguments}); value.usage = {100, 20, 0, 0, 0};
    return value;
}
EventFakeScript full_script() {
    auto script = event_fake_script_from_json(fixture("s14_fake_submit.json"));
    script.default_steps.push_back(step(Role::Dispatcher, "accept_candidate", positive_acceptance(), 2));
    script.by_node.at("alpha").push_back(step(Role::TeamLead, "check_submission", positive_check(), 2));
    script.by_node.at("alpha").push_back(step(Role::TeamLead, "report_experience", no_experience(), 3));
    return script;
}
Json contract(const ModelRequest& request) {
    std::string_view bytes(request.context.dynamic_suffix);
    CHECK(bytes.starts_with("task_contract ")); bytes.remove_prefix(14);
    const auto newline = bytes.find('\n'); CHECK(newline != std::string_view::npos);
    const auto size = static_cast<std::size_t>(std::stoull(std::string(bytes.substr(0, newline))));
    bytes.remove_prefix(newline + 1); CHECK(bytes.size() > size);
    return parse_json_strict(bytes.substr(0, size), 1024U * 1024U);
}
bool blocked(const Json& result, std::string_view code) {
    return std::any_of(result.at("blocked").as_array().begin(), result.at("blocked").as_array().end(),
        [&](const auto& row) { return row.at("code").as_string() == code; });
}
struct Fixture {
    test::TempDir directory;
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> alpha, beta;
    std::string id;
    explicit Fixture(const AdmissionInput& admission = input()) {
        std::filesystem::create_directory(directory.path / "alpha");
        std::filesystem::create_directory(directory.path / "beta");
        reopen(); id = text(store->admit(admission).at("request_id"));
    }
    void reopen(StoreRecoveryScope scope = {}) {
        alpha.reset(); beta.reset(); store.reset();
        store = std::make_unique<Store>(directory.path / "control.db", base_sql, store_sql,
            StoreOpenMode::Takeover, tools_sql, events_sql, workflow_sql, acceptance_sql, std::string_view{}, std::move(scope));
        alpha = std::make_unique<WorkerTools>(*store, directory.path / "alpha");
        beta = std::make_unique<WorkerTools>(*store, directory.path / "beta");
    }
    EventRuntime runtime(bool two = false) {
        if (two) return EventRuntime(*store, *schemas, {{"alpha", alpha.get()}, {"beta", beta.get()}});
        return EventRuntime(*store, *schemas, {{"alpha", alpha.get()}});
    }
    Json task(const std::string& node = "alpha") const {
        for (const auto& row : store->tasks(id)) if (text(row.at("node_id")) == node) return row;
        throw std::runtime_error("test task missing");
    }
    ExecutionBinding binding() const { return store->binding(text(task().at("task_id"))); }
    CapturedModelCall capture(Role role, const FakeModel& fake) {
        return OfflineRuntime(*store, *schemas, alpha.get()).reserve_call(fake, id, role,
            role == Role::Dispatcher ? std::string{} : binding().task_id);
    }
    Json invoke(Role role, const std::string& name, const Json& arguments) {
        const FakeModel fake({step(role, name, arguments)});
        const auto call = capture(role, fake);
        return OfflineRuntime(*store, *schemas, alpha.get()).accept_response(call, fake.complete(call.request));
    }
    Json pump_to(std::int64_t max_calls) {
        EventPumpOptions options; options.max_calls = max_calls; options.max_worker_calls_per_task = 2;
        return runtime().pump(id, full_script(), options);
    }
    Json candidate() {
        const auto result = pump_to(5); CHECK(result.at("calls").as_array().size() == 5);
        const auto submitted = store->current_submission(binding().task_id);
        CHECK(submitted.at("status") == "CANDIDATE_READY"); CHECK(task().at("status") == "SUBMITTED");
        return submitted;
    }
    Json accepted() {
        const auto result = pump_to(6); CHECK(result.at("calls").as_array().size() == 6);
        CHECK(task().at("status") == "ACCEPTED");
        const auto jobs = store->experience_jobs(id); CHECK(jobs.size() == 1);
        CHECK(jobs.front().at("status") != "DONE"); return jobs.front();
    }
    void cancel() {
        (void)store->cancel_request(store->host_actor(Role::Root, id),
            number(store->request(id).at("state_version")), new_id("cancel"));
    }
};

Json correction() { return Object{{"rationale", "The current evidence failed its unchanged criterion."},
    {"correction", "Correct the result without dropping any requirement."}}; }
Json owner_question() { return Object{{"text", "Please decide whether to AMEND or CANCEL this blocked work."},
    {"options", Array{Object{{"id", "amend"}, {"label", "Send a separate AMEND"}}}}}; }
AdmissionInput limited_input(std::int64_t maximum) {
    test::TempDir temp;
    auto yaml = read_runtime_file(source / "config/examples/system.yaml");
    const auto at = yaml.find("team_internal_rework_max: 3"); CHECK(at != std::string::npos);
    yaml.replace(at, std::string("team_internal_rework_max: 3").size(), "team_internal_rework_max: " + std::to_string(maximum));
    write(temp.path / "system.yaml", yaml);
    auto value = input(); value.config_snapshot = load_config_catalog(temp.path / "system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general").json();
    return value;
}
Json reject(Fixture& f) {
    (void)f.candidate();
    (void)f.invoke(Role::Dispatcher, "accept_candidate", Object{{"passed", false}, {"notes", "Incorrect original candidate."}});
    auto rejected = f.store->rejected_submission(f.binding().task_id); CHECK(!rejected.is_null()); return rejected;
}
Json revise(Fixture& f, const std::string& command = "correct") {
    const auto rejected = f.store->rejected_submission(f.binding().task_id);
    return f.store->change_task(f.store->host_actor(Role::Dispatcher, f.id), f.store->request_binding(f.id),
        number(f.task().at("state_version")), text(rejected.at("submission_id")), number(rejected.at("state_version")), correction(), command);
}
Json escalate(Fixture& f, const std::string& command = "escalate") {
    const auto rejected = f.store->rejected_submission(f.binding().task_id);
    return f.store->escalate_task(f.store->host_actor(Role::Dispatcher, f.id), f.store->request_binding(f.id),
        number(f.task().at("state_version")), text(rejected.at("submission_id")), number(rejected.at("state_version")), owner_question(), command);
}
void answer(Fixture& f, const Json& q) {
    (void)f.store->answer_question(f.store->host_actor(Role::Root, f.id), f.store->request_binding(f.id),
        text(q.at("question_id")), number(q.at("state_version")), Object{{"text", "Do not weaken the original criterion."}}, new_id("answer"));
}
void raw(Fixture& f, const std::string& sql) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open_v2((f.directory.path / "control.db").c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    const auto rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    const std::string message = sqlite3_errmsg(db); sqlite3_close(db);
    if (rc != SQLITE_OK) throw std::runtime_error(message);
}
void current_rejection_has_original_evidence_and_two_actions() {
    Fixture f; const auto original = reject(f);
    write(f.directory.path / "alpha/result.txt", "changed current file, not old evidence");
    const FakeModel fake({step(Role::Dispatcher, "change_task", correction())});
    const auto call = f.capture(Role::Dispatcher, fake);
    CHECK(call.workflow_action.at("kind") == "rejection");
    CHECK(call.workflow_action.at("target").at("record_sha256") == original.at("record_sha256"));
    CHECK(call.workflow_action.at("target").at("rejection_cause").at("kind") == "business_rejection");
    CHECK(call.request.context.tool_names == (std::vector<std::string>{"change_task", "escalate_task"}));
    CHECK(contract(call.request).at("submission_file_views").as_array().front().at("content") == "alpha candidate version 1\n");
    const auto response = fake.complete(call.request);
    OfflineRuntime runtime(*f.store, *schemas, f.alpha.get());
    const auto result = runtime.accept_response(call, response);
    CHECK(runtime.accept_response(call, response) == result);
    CHECK(f.task().at("status") == "READY");
    CHECK(f.store->submission(text(original.at("submission_id"))).at("team_rework_count") == 1);
}
void correction_preserves_original_obligations_and_requires_new_order_ack() {
    Fixture f; const auto rejected = reject(f); const auto task = f.task();
    const auto old_order = f.store->instruction(f.binding().task_id); const auto old_binding = f.binding();
    (void)revise(f);
    CHECK(f.task().at("brief") == task.at("brief"));
    CHECK(f.task().at("task_revision") == number(task.at("task_revision")) + 1);
    CHECK(f.task().at("instruction_revision") == task.at("instruction_revision"));
    CHECK(f.task().at("applied_task_revision") == 0 && f.task().at("applied_instruction_revision") == 0);
    CHECK(f.store->instruction(f.binding().task_id) == old_order);
    CHECK(f.store->submission(text(rejected.at("submission_id"))).at("artifact_snapshot") == rejected.at("artifact_snapshot"));
    expect(ErrorCode::Stale, [&] { f.store->acknowledge(f.store->host_actor(Role::Worker,f.id, f.binding().team_id),
        old_binding, number(f.task().at("state_version")), "old-ack"); });
    const auto order = full_script().by_node.at("alpha").front().tool_calls.front().arguments;
    (void)f.invoke(Role::TeamLead, "issue_work_order", order);
    CHECK(f.task().at("instruction_revision") == number(task.at("instruction_revision")) + 1);
    (void)f.store->acknowledge(f.store->host_actor(Role::Worker,f.id, f.binding().team_id), f.binding(),
        number(f.task().at("state_version")), "new-ack");
    CHECK(f.task().at("applied_task_revision") == f.task().at("task_revision"));
}
void exact_command_replay_is_not_a_new_correction() {
    Fixture f; const auto rejected = reject(f); const auto actor = f.store->host_actor(Role::Dispatcher, f.id);
    const auto binding = f.store->request_binding(f.id); const auto version = number(f.task().at("state_version"));
    const auto invoke = [&](const Json& args) { return f.store->change_task(actor,binding,version,
        text(rejected.at("submission_id")),number(rejected.at("state_version")),args,"same-key"); };
    const auto result = invoke(correction()); const auto after = f.task();
    CHECK(invoke(correction()) == result && f.task() == after);
    auto changed = correction(); changed.as_object()["correction"] = "different";
    expect(ErrorCode::Conflict,[&] { (void)invoke(changed); });
    CHECK(f.store->submission(text(rejected.at("submission_id"))).at("team_rework_count") == 1);
}
void roles_scope_and_both_cas_are_required() {
    Fixture f; const auto rejected = reject(f); const auto tv = number(f.task().at("state_version"));
    const auto sv = number(rejected.at("state_version")); const auto id = text(rejected.at("submission_id"));
    const auto actor = f.store->host_actor(Role::Dispatcher, f.id); const auto binding = f.store->request_binding(f.id);
    expect(ErrorCode::Conflict,[&] { f.store->change_task(actor,binding,tv+1,id,sv,correction(),"wrong-task-cas"); });
    expect(ErrorCode::Conflict,[&] { f.store->change_task(actor,binding,tv,id,sv+1,correction(),"wrong-source-cas"); });
    expect(ErrorCode::Unauthorized,[&] { f.store->change_task(f.store->host_actor(Role::Root,f.id),binding,tv,id,sv,correction(),"root-is-not-dispatcher"); });
    const auto other = text(f.store->admit(input()).at("request_id"));
    expect(ErrorCode::Unauthorized,[&] { f.store->change_task(f.store->host_actor(Role::Dispatcher,other),f.store->request_binding(other),tv,id,sv,correction(),"wrong-request"); });
    CHECK(f.store->rejected_submission(f.binding().task_id) == rejected);
}
void captured_rejection_cannot_be_retargeted() {
    Fixture f; (void)reject(f); const FakeModel fake({step(Role::Dispatcher,"change_task",correction())});
    const auto call=f.capture(Role::Dispatcher,fake); const auto response=fake.complete(call.request);
    auto changed=call; changed.workflow_action.at("target").as_object()["submission_id"]=new_id("sub");
    OfflineRuntime runtime(*f.store,*schemas,f.alpha.get());
    expect(ErrorCode::Conflict,[&] { runtime.accept_response(changed,response); });
    changed=call; changed.workflow_action.as_object()["expected_task_version"]=number(f.task().at("state_version"))+1;
    expect(ErrorCode::Conflict,[&] { runtime.accept_response(changed,response); });
    CHECK(f.store->attempt(call.request.attempt_id).at("settlement_sha256").is_null());
    (void)runtime.accept_response(call,response); CHECK(f.task().at("status")=="READY");
}
void amend_fences_old_correction_response_without_losing_usage() {
    Fixture f; const auto rejected=reject(f); const FakeModel fake({step(Role::Dispatcher,"change_task",correction())});
    const auto call=f.capture(Role::Dispatcher,fake);
    f.store->amend(f.store->host_actor(Role::Root,f.id),number(f.store->request(f.id).at("state_version")),
        read_runtime_file(source/"tests/fixtures/s13_amendment.txt"),fixture("s13_amended_constraints.json"),"amend");
    expect(ErrorCode::Stale,[&] { OfflineRuntime(*f.store,*schemas,f.alpha.get()).accept_response(call,fake.complete(call.request)); });
    CHECK(!f.store->attempt(call.request.attempt_id).at("settlement_sha256").is_null());
    const auto after=f.store->submission(text(rejected.at("submission_id")));
    CHECK(after.at("status")=="SUPERSEDED" && after.at("team_rework_count")==0);
    CHECK(after.at("artifact_snapshot")==rejected.at("artifact_snapshot"));
}
void recovery_fences_old_call_and_preserves_original_rejection() {
    Fixture f; const auto rejected=reject(f); const FakeModel fake({step(Role::Dispatcher,"change_task",correction())});
    const auto call=f.capture(Role::Dispatcher,fake); f.reopen();
    expect(ErrorCode::Stale,[&] { OfflineRuntime(*f.store,*schemas,f.alpha.get()).accept_response(call,fake.complete(call.request)); });
    const auto current=f.store->rejected_submission(f.binding().task_id);
    CHECK(current.at("original_binding")==rejected.at("original_binding"));
    CHECK(current.at("record_sha256")==rejected.at("record_sha256"));
    (void)revise(f); CHECK(f.task().at("status")=="READY");
}
void exhausted_team_check_uses_same_counter_and_only_escalation() {
    Fixture f(limited_input(0)); const auto initial=f.pump_to(4); CHECK(initial.at("calls").as_array().size()==4);
    const auto order=full_script().by_node.at("alpha").front().tool_calls.front().arguments;
    (void)f.invoke(Role::TeamLead,"check_submission",Object{{"passed",false},{"notes","Needs rework."},{"work_order",order}});
    const auto rejected=f.store->rejected_submission(f.binding().task_id);
    CHECK(rejected.at("rejection_cause").at("kind")=="team_rework_exhausted");
    CHECK(rejected.at("rework_remaining")==0);
    expect(ErrorCode::NotReady,[&] { (void)revise(f); });
    const FakeModel fake({step(Role::Dispatcher,"escalate_task",owner_question())});
    const auto call=f.capture(Role::Dispatcher,fake);
    CHECK(call.request.context.tool_names==std::vector<std::string>{"escalate_task"});
    (void)OfflineRuntime(*f.store,*schemas,f.alpha.get()).accept_response(call,fake.complete(call.request));
    CHECK(f.store->questions(f.id).size()==1 && f.task().at("status")=="BLOCKED");
}
void escalation_replay_and_answer_cannot_create_more_rework() {
    Fixture f(limited_input(0)); const auto rejected=reject(f);
    const auto actor=f.store->host_actor(Role::Dispatcher,f.id); const auto binding=f.store->request_binding(f.id);
    const auto tv=number(f.task().at("state_version")), sv=number(rejected.at("state_version"));
    const auto id=text(rejected.at("submission_id"));
    const auto q=f.store->escalate_task(actor,binding,tv,id,sv,owner_question(),"one-escalation");
    CHECK(f.store->escalate_task(actor,binding,tv,id,sv,owner_question(),"one-escalation")==q);
    CHECK(f.store->questions(f.id).size()==1);
    answer(f,q); const auto budget=f.store->budget(f.id);
    expect(ErrorCode::NotReady,[&] { (void)revise(f); });
    expect(ErrorCode::NotReady,[&] { (void)escalate(f,"different-key"); });
    f.reopen(); CHECK(next_workflow_action(*f.store,Role::Dispatcher,f.id).is_null());
    const auto result=f.runtime().pump(f.id,EventFakeScript{});
    CHECK(result.at("calls").as_array().empty() && blocked(result,"rework_blocked"));
    CHECK(f.store->budget(f.id)==budget && f.store->questions(f.id).size()==1);
    CHECK(f.task().at("status")=="BLOCKED");
}
void user_answer_releases_only_its_wait_then_new_decision_required() {
    Fixture f; (void)reject(f); const auto q=escalate(f);
    const auto task=f.task();
    expect(ErrorCode::NotReady,[&] { (void)revise(f); });
    CHECK(next_workflow_action(*f.store,Role::Dispatcher,f.id).is_null());
    answer(f,q);
    CHECK(f.task().at("status")=="BLOCKED" && f.task().at("task_revision")==task.at("task_revision"));
    CHECK(f.store->worker_wait(f.binding().task_id).at("status")=="AWAKE");
    CHECK(next_workflow_action(*f.store,Role::Dispatcher,f.id).at("kind")=="rejection");
    (void)revise(f); CHECK(f.task().at("status")=="READY");
}
void unknown_original_effect_blocks_correction_not_user_escalation() {
    Fixture f; const auto rejected=reject(f);
    // Host-failure injection in our own database: simulate an uncertain old
    // operation result appearing after review. No real process/exit proof is invented.
    raw(f,"UPDATE ops SET status='UNKNOWN',state_version=state_version+1 WHERE request_id='"+f.id+"'");
    expect(ErrorCode::UnknownOutcome,[&] { (void)revise(f); });
    const auto q=escalate(f);
    CHECK(q.at("status")=="OPEN" && f.task().at("status")=="BLOCKED");
    CHECK(f.store->rejected_submission(f.binding().task_id).at("record_sha256")==rejected.at("record_sha256"));
    answer(f,q); expect(ErrorCode::UnknownOutcome,[&] { (void)revise(f); });
    CHECK(f.store->pending_recovery().at("unknown_operations").as_array().front().at("status")=="UNKNOWN");
}
void correction_event_failure_rolls_back_versions_and_counter() {
    Fixture f; const auto rejected=reject(f); const auto task=f.task();
    const auto count=f.store->events(f.id).size();
    raw(f,"CREATE TRIGGER test_fail_correction BEFORE INSERT ON events WHEN NEW.type='change_task' BEGIN SELECT RAISE(ABORT,'injected correction commit fault'); END");
    expect(ErrorCode::Conflict,[&] { (void)revise(f); });
    CHECK(f.task()==task && f.store->rejected_submission(f.binding().task_id)==rejected);
    CHECK(f.store->events(f.id).size()==count);
    raw(f,"DROP TRIGGER test_fail_correction"); (void)revise(f); CHECK(f.task().at("status")=="READY");
}
void escalation_event_failure_rolls_back_question_and_wait() {
    Fixture f; const auto rejected=reject(f); const auto task=f.task();
    const auto wait=f.store->worker_wait(f.binding().task_id);
    raw(f,"CREATE TRIGGER test_fail_escalation BEFORE INSERT ON events WHEN NEW.type='escalate_task' BEGIN SELECT RAISE(ABORT,'injected escalation commit fault'); END");
    expect(ErrorCode::Conflict,[&] { (void)escalate(f); });
    CHECK(f.task()==task && f.store->questions(f.id).empty());
    CHECK(f.store->worker_wait(f.binding().task_id)==wait);
    CHECK(f.store->rejected_submission(f.binding().task_id)==rejected);
    raw(f,"DROP TRIGGER test_fail_escalation"); (void)escalate(f); CHECK(f.store->questions(f.id).size()==1);
}
void concurrent_corrections_have_one_cas_winner() {
    Fixture f; const auto rejected=reject(f); const auto tv=number(f.task().at("state_version"));
    const auto actor=f.store->host_actor(Role::Dispatcher,f.id); const auto binding=f.store->request_binding(f.id);
    std::atomic<int> passed{0}, rejected_count{0};
    const auto run=[&](const char* key) {
        try { f.store->change_task(actor,binding,tv,text(rejected.at("submission_id")),number(rejected.at("state_version")),correction(),key); ++passed; }
        catch (const Error&) { ++rejected_count; }
    };
    std::thread one(run,"concurrent-one"),two(run,"concurrent-two"); one.join(); two.join();
    CHECK(passed==1 && rejected_count==1);
    CHECK(f.task().at("task_revision")==2);
    CHECK(f.store->submission(text(rejected.at("submission_id"))).at("team_rework_count")==1);
}
void malformed_model_arguments_are_accounted_not_applied() {
    Fixture f; (void)reject(f); auto forged=correction(); forged.as_object()["submission_id"]=new_id("sub");
    const FakeModel fake({step(Role::Dispatcher,"change_task",forged)}); const auto call=f.capture(Role::Dispatcher,fake);
    expect(ErrorCode::InvalidInput,[&] { OfflineRuntime(*f.store,*schemas,f.alpha.get()).accept_response(call,fake.complete(call.request)); });
    CHECK(!f.store->attempt(call.request.attempt_id).at("settlement_sha256").is_null());
    CHECK(!f.store->tool_feedback(call.request.attempt_id).is_null());
    CHECK(f.task().at("status")=="BLOCKED" && f.store->rejected_submission(f.binding().task_id).at("team_rework_count")==0);
}
void cancellation_never_revives_a_rejected_task() {
    Fixture f; const auto rejected=reject(f); f.cancel();
    CHECK(next_workflow_action(*f.store,Role::Dispatcher,f.id).is_null());
    expect(ErrorCode::NotReady,[&] { f.store->change_task(f.store->host_actor(Role::Dispatcher,f.id),
        f.store->request_binding(f.id),number(f.task().at("state_version")),text(rejected.at("submission_id")),
        number(rejected.at("state_version")),correction(),"cancelled"); });
    CHECK(f.store->request(f.id).at("status")=="CANCELLED");
}
void answered_rejection_keeps_wakeup_after_missing_script() {
    Fixture f; (void)reject(f); const auto q=escalate(f);
    // Consume the original notices while the user owns the question. No restart
    // follows; a new recovery event must not hide a lost answer wakeup.
    (void)f.runtime().pump(f.id,EventFakeScript{}); answer(f,q);
    const auto empty=f.runtime().pump(f.id,EventFakeScript{});
    CHECK(empty.at("calls").as_array().empty());
    EventFakeScript script; script.default_steps={step(Role::Dispatcher,"change_task",correction())};
    const auto result=f.runtime().pump(f.id,script);
    CHECK(result.at("calls").as_array().size()==1 && f.task().at("status")=="READY");
}
void answered_rejection_keeps_wakeup_after_invalid_response() {
    Fixture f; (void)reject(f); const auto q=escalate(f);
    (void)f.runtime().pump(f.id,EventFakeScript{}); answer(f,q);
    EventFakeScript bad; bad.default_steps={step(Role::Dispatcher,"change_task",Object{{"correction","missing rationale"}})};
    const auto failure=f.runtime().pump(f.id,bad); CHECK(blocked(failure,"invalid_input"));
    EventFakeScript valid; valid.default_steps={step(Role::Dispatcher,"change_task",correction())};
    const auto result=f.runtime().pump(f.id,valid);
    CHECK(result.at("calls").as_array().size()==1 && f.task().at("status")=="READY");
}
void team_lead_and_dispatcher_share_one_lifetime_counter() {
    Fixture f(limited_input(1)); (void)f.pump_to(4);
    const auto script=full_script(); const auto order=script.by_node.at("alpha").front().tool_calls.front().arguments;
    (void)f.invoke(Role::TeamLead,"check_submission",Object{{"passed",false},{"notes","Correct it."},{"work_order",order}});
    (void)f.store->acknowledge(f.store->host_actor(Role::Worker,f.id,f.binding().team_id),f.binding(),number(f.task().at("state_version")),"rework-ack");
    Json body=nullptr;
    for (const auto& entry:script.by_node.at("alpha")) if (entry.tool_calls.front().name=="submit") body=entry.tool_calls.front().arguments;
    CHECK(!body.is_null());
    (void)f.invoke(Role::Worker,"submit",body);
    (void)f.invoke(Role::TeamLead,"check_submission",positive_check());
    (void)f.invoke(Role::Dispatcher,"accept_candidate",Object{{"passed",false},{"notes","Still needs correction."}});
    const auto rejected=f.store->rejected_submission(f.binding().task_id);
    CHECK(rejected.at("team_rework_count")==1 && rejected.at("rework_remaining")==0);
    expect(ErrorCode::NotReady,[&] { (void)revise(f); });
    (void)escalate(f); CHECK(f.store->questions(f.id).size()==1);
}
void corrective_order_cannot_drop_previous_extra_constraint() {
    Fixture f; auto script=full_script();
    script.by_node.at("alpha").front().tool_calls.front().arguments.at("constraints_verbatim").as_array().push_back("每个任务保存自己的文件与原始观察");
    EventPumpOptions options; options.max_calls=5; options.max_worker_calls_per_task=2;
    const auto result=f.runtime().pump(f.id,script,options); CHECK(result.at("calls").as_array().size()==5);
    (void)f.invoke(Role::Dispatcher,"accept_candidate",Object{{"passed",false},{"notes","Correction needed."}});
    (void)revise(f);
    const auto dropping=full_script().by_node.at("alpha").front().tool_calls.front().arguments;
    expect(ErrorCode::InvalidInput,[&] { (void)f.invoke(Role::TeamLead,"issue_work_order",dropping); });
    CHECK(f.task().at("instruction_revision")==1);
    (void)f.invoke(Role::TeamLead,"issue_work_order",script.by_node.at("alpha").front().tool_calls.front().arguments);
    CHECK(f.task().at("instruction_revision")==2);
}
void current_policy_can_correct_but_old_captured_policy_is_fenced() {
    Fixture f; const auto rejected=reject(f);
    const FakeModel fake({step(Role::Dispatcher,"change_task",correction())}); const auto call=f.capture(Role::Dispatcher,fake);
    raw(f,"UPDATE runtime_policy SET denied_tools='[\"run\"]',policy_epoch=policy_epoch+1,state_version=state_version+1 WHERE singleton=1");
    expect(ErrorCode::Stale,[&] { OfflineRuntime(*f.store,*schemas,f.alpha.get()).accept_response(call,fake.complete(call.request)); });
    (void)revise(f);
    const auto after=f.store->submission(text(rejected.at("submission_id")));
    CHECK(after.at("original_binding")==rejected.at("original_binding"));
    CHECK(after.at("team_rework_count")==1 && f.task().at("status")=="READY");
}
void escrowed_source_changes_reject_a_second_captured_response() {
    Fixture f; (void)reject(f);
    const FakeModel fake({step(Role::Dispatcher,"change_task",correction())});
    const auto first=f.capture(Role::Dispatcher,fake), second=f.capture(Role::Dispatcher,fake);
    OfflineRuntime runtime(*f.store,*schemas,f.alpha.get());
    (void)runtime.accept_response(first,fake.complete(first.request));
    expect(ErrorCode::Stale,[&] { (void)runtime.accept_response(second,fake.complete(second.request)); });
    CHECK(!f.store->attempt(second.request.attempt_id).at("settlement_sha256").is_null());
    CHECK(f.task().at("task_revision")==2);
}

std::unique_ptr<Store> observer(Fixture& f) {
    return std::make_unique<Store>(f.directory.path / "control.db", base_sql, store_sql,
        StoreOpenMode::ReadOnly, tools_sql, events_sql, workflow_sql, acceptance_sql);
}
void scoped_writer_rejects_foreign_correction_and_escalation() {
    Fixture f; const auto rejected=reject(f);
    const auto actor=f.store->host_actor(Role::Dispatcher,f.id);
    const auto binding=f.store->request_binding(f.id);
    const auto task=f.task(), request=f.store->request(f.id), budget=f.store->budget(f.id);
    const auto other=text(f.store->admit(input()).at("request_id"));
    f.reopen(StoreRecoveryScope::request(other));
    expect(ErrorCode::Unauthorized,[&] { f.store->change_task(actor,binding,number(task.at("state_version")),
        text(rejected.at("submission_id")),number(rejected.at("state_version")),correction(),"foreign-change"); });
    expect(ErrorCode::Unauthorized,[&] { f.store->escalate_task(actor,binding,number(task.at("state_version")),
        text(rejected.at("submission_id")),number(rejected.at("state_version")),owner_question(),"foreign-escalate"); });
    auto read=observer(f);
    CHECK(read->request(f.id)==request && read->budget(f.id)==budget);
    CHECK(read->task(text(task.at("task_id")))==task);
    CHECK(read->rejected_submission(text(task.at("task_id")))==rejected);
}
void scoped_writer_rejects_foreign_exact_correction_replay() {
    Fixture f; const auto rejected=reject(f);
    const auto actor=f.store->host_actor(Role::Dispatcher,f.id);
    const auto binding=f.store->request_binding(f.id); const auto version=number(f.task().at("state_version"));
    (void)revise(f,"replay-scope");
    const auto task=f.task(), budget=f.store->budget(f.id);
    const auto other=text(f.store->admit(input()).at("request_id"));
    f.reopen(StoreRecoveryScope::request(other));
    expect(ErrorCode::Unauthorized,[&] { f.store->change_task(actor,binding,version,
        text(rejected.at("submission_id")),number(rejected.at("state_version")),correction(),"replay-scope"); });
    auto read=observer(f);
    CHECK(read->task(text(task.at("task_id")))==task && read->budget(f.id)==budget);
}
void admission_cannot_decide_rejected_work() {
    Fixture f; const auto rejected=reject(f); const auto task=f.task();
    const auto actor=f.store->host_actor(Role::Dispatcher,f.id); const auto binding=f.store->request_binding(f.id);
    f.reopen(StoreRecoveryScope::admission());
    expect(ErrorCode::Unauthorized,[&] { f.store->change_task(actor,binding,number(task.at("state_version")),
        text(rejected.at("submission_id")),number(rejected.at("state_version")),correction(),"admission-change"); });
    expect(ErrorCode::Unauthorized,[&] { f.store->escalate_task(actor,binding,number(task.at("state_version")),
        text(rejected.at("submission_id")),number(rejected.at("state_version")),owner_question(),"admission-escalate"); });
    auto read=observer(f);
    CHECK(read->rejected_submission(text(task.at("task_id")))==rejected);
    CHECK(read->task(text(task.at("task_id")))==task);
}
void scoped_reopen_fences_old_rejection_decision_without_touching_sibling() {
    Fixture f; const auto rejected=reject(f);
    const auto other=text(f.store->admit(input()).at("request_id"));
    const auto sibling=f.store->request(other), budget=f.store->budget(other);
    const FakeModel fake({step(Role::Dispatcher,"change_task",correction())});
    const auto call=f.capture(Role::Dispatcher,fake);
    f.reopen(StoreRecoveryScope::request(f.id));
    expect(ErrorCode::Stale,[&] { OfflineRuntime(*f.store,*schemas,f.alpha.get()).accept_response(call,fake.complete(call.request)); });
    CHECK(f.store->attempt(call.request.attempt_id).at("status")=="STALE");
    CHECK(f.store->rejected_submission(f.binding().task_id).at("record_sha256")==rejected.at("record_sha256"));
    (void)revise(f);
    CHECK(f.task().at("status")=="READY");
    auto read=observer(f); CHECK(read->request(other)==sibling && read->budget(other)==budget);
}
} // namespace

int main() {
    if (::geteuid()==0) { std::cerr << "ordinary user required\n"; return 1; }
    schemas=std::make_unique<SchemaRegistry>(source/"schemas");
    base_sql=read_runtime_file(source/"db/schema.sql");
    store_sql=read_runtime_file(source/"db/migrations/004_s1_store.sql");
    tools_sql=read_runtime_file(source/"db/migrations/005_worker_tools.sql");
    events_sql=read_runtime_file(source/"db/migrations/006_event_runtime.sql");
    workflow_sql=read_runtime_file(source/"db/migrations/007_workflow_runtime.sql");
    acceptance_sql=read_runtime_file(source/"db/migrations/008_acceptance_runtime.sql");
    return test::run({
        {"scoped writer rejects both foreign corrective actions",scoped_writer_rejects_foreign_correction_and_escalation},
        {"foreign exact correction replay remains out of scope",scoped_writer_rejects_foreign_exact_correction_replay},
        {"admission-only cannot decide rejected work",admission_cannot_decide_rejected_work},
        {"scoped reopen fences old corrective response without sibling changes",scoped_reopen_fences_old_rejection_decision_without_touching_sibling},
        {"rejection captures original bytes and offers only correction or escalation",current_rejection_has_original_evidence_and_two_actions},
        {"correction preserves obligations and requires new TeamLead order and ack",correction_preserves_original_obligations_and_requires_new_order_ack},
        {"exact command replay cannot consume another rework",exact_command_replay_is_not_a_new_correction},
        {"Dispatcher identity scope and both CAS values are required",roles_scope_and_both_cas_are_required},
        {"captured model targets cannot be relabelled",captured_rejection_cannot_be_retargeted},
        {"AMEND fences old correction while retaining usage",amend_fences_old_correction_response_without_losing_usage},
        {"recovery fences old response and preserves original rejected bytes",recovery_fences_old_call_and_preserves_original_rejection},
        {"TeamLead exhausted check offers only user escalation",exhausted_team_check_uses_same_counter_and_only_escalation},
        {"escalation and answer never reset exhausted shared allowance",escalation_replay_and_answer_cannot_create_more_rework},
        {"answer ends exactly its wait and requires a new correction decision",user_answer_releases_only_its_wait_then_new_decision_required},
        {"UNKNOWN blocks corrective work but permits reporting the problem",unknown_original_effect_blocks_correction_not_user_escalation},
        {"correction event failure rolls back all mutation and counter",correction_event_failure_rolls_back_versions_and_counter},
        {"escalation event failure rolls back question and wait",escalation_event_failure_rolls_back_question_and_wait},
        {"concurrent corrections have exactly one CAS winner",concurrent_corrections_have_one_cas_winner},
        {"invalid model target arguments consume usage but never change task",malformed_model_arguments_are_accounted_not_applied},
        {"cancellation cannot become a corrective revival",cancellation_never_revives_a_rejected_task},
        {"answered rejection keeps wakeup when model script is absent",answered_rejection_keeps_wakeup_after_missing_script},
        {"answered rejection keeps wakeup when model response is invalid",answered_rejection_keeps_wakeup_after_invalid_response},
        {"TeamLead and Dispatcher corrections share the frozen counter",team_lead_and_dispatcher_share_one_lifetime_counter},
        {"new corrective instruction retains all previous task constraints",corrective_order_cannot_drop_previous_extra_constraint},
        {"new policy controls preserve old producer evidence but fence old calls",current_policy_can_correct_but_old_captured_policy_is_fenced},
        {"two captured responses cannot correct the same rejected target twice",escrowed_source_changes_reject_a_second_captured_response}
    });
}
