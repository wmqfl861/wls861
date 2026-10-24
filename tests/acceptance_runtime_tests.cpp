#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"
#include "capacity_test_support.hpp"

#include <sqlite3.h>

#include <algorithm>
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
    void reopen() {
        alpha.reset(); beta.reset(); store.reset();
        store = std::make_unique<Store>(directory.path / "control.db", base_sql, store_sql,
            StoreOpenMode::Takeover, tools_sql, events_sql, workflow_sql, acceptance_sql);
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

void explicit_acceptance_and_experience_complete_without_implied_delivery() {
    Fixture f;
    const auto result = f.pump_to(12);
    CHECK(result.at("calls").as_array().size() == 7);
    CHECK(result.at("acceptance_runtime") == true && result.at("business_delivered") == false);
    CHECK(result.at("real_model_calls") == 0 && f.task().at("status") == "ACCEPTED");
    CHECK(result.at("experience_jobs").as_array().size() == 1);
    CHECK(result.at("experience_jobs").as_array().front().at("status") == "DONE");
    CHECK(f.store->deliveries(f.id).empty());
    const auto submitted = f.store->current_submission(f.binding().task_id);
    CHECK(submitted.at("business_accepted") == true && submitted.at("business_delivered") == false);
    for (const auto& invocation : result.at("calls").as_array()) {
        const auto attempt_id = text(invocation.at("attempt_id"));
        CHECK(!f.store->attempt(attempt_id).at("settlement_sha256").is_null());
        CHECK(!f.store->tool_feedback(attempt_id).is_null());
    }
    const auto budget = f.store->budget(f.id);
    const auto again = f.runtime().pump(f.id, EventFakeScript{});
    CHECK(again.at("calls").as_array().empty() && f.store->budget(f.id) == budget);
}

void dispatcher_observes_original_bytes_and_response_replay_is_exact() {
    Fixture f; const auto original = f.candidate();
    write(f.directory.path / "alpha/result.txt", "mutated live workspace must not replace candidate evidence\n");
    const FakeModel fake({step(Role::Dispatcher, "accept_candidate", positive_acceptance())});
    const auto call = f.capture(Role::Dispatcher, fake);
    CHECK(call.workflow_action.at("kind") == "acceptance");
    CHECK(call.workflow_action.at("target").at("submission_id") == original.at("submission_id"));
    CHECK(call.workflow_action.at("expected_task_version") == f.task().at("state_version"));
    CHECK(call.request.context.tool_names == std::vector<std::string>{"accept_candidate"});
    const auto context = contract(call.request);
    CHECK(context.at("submission_file_views").as_array().front().at("content") == "alpha candidate version 1\n");
    CHECK(context.at("acceptance_task").at("task_id") == f.task().at("task_id"));
    CHECK(!context.at("acceptance_instruction").is_null());
    OfflineRuntime runtime(*f.store, *schemas, f.alpha.get());
    const auto response = fake.complete(call.request);
    const auto accepted = runtime.accept_response(call, response);
    CHECK(runtime.accept_response(call, response) == accepted);
    CHECK(f.store->experience_jobs(f.id).size() == 1);
    CHECK(f.store->experience_jobs(f.id).front().at("source_snapshot").at("submission").at("artifact_snapshot") == original.at("artifact_snapshot"));
}

void raw_submission_cannot_skip_team_lead_check() {
    Fixture f; const auto initial = f.pump_to(4); CHECK(initial.at("calls").as_array().size() == 4);
    const FakeModel fake({step(Role::Dispatcher, "accept_candidate", positive_acceptance())});
    const auto call = f.capture(Role::Dispatcher, fake);
    CHECK(call.workflow_action.is_null());
    CHECK(call.request.context.tool_names == std::vector<std::string>{"submit_plan"});
    expect(ErrorCode::InvalidInput, [&] {
        (void)OfflineRuntime(*f.store, *schemas, f.alpha.get()).accept_response(call, fake.complete(call.request));
    });
    CHECK(f.task().at("status") == "SUBMITTED" && f.store->experience_jobs(f.id).empty());
    CHECK(f.store->current_submission(f.binding().task_id).at("status") == "SUBMITTED");
}

void captured_target_and_task_cas_cannot_be_relabelled() {
    Fixture f; const auto candidate = f.candidate();
    const FakeModel fake({step(Role::Dispatcher, "accept_candidate", positive_acceptance())});
    const auto call = f.capture(Role::Dispatcher, fake);
    const auto response = fake.complete(call.request);
    OfflineRuntime runtime(*f.store, *schemas, f.alpha.get());
    auto forged = call; forged.workflow_action.at("target").as_object()["submission_id"] = new_id("sub");
    expect(ErrorCode::Conflict, [&] { (void)runtime.accept_response(forged, response); });
    forged = call; forged.workflow_action.as_object()["expected_task_version"] =
        number(forged.workflow_action.at("expected_task_version")) + 1;
    expect(ErrorCode::Conflict, [&] { (void)runtime.accept_response(forged, response); });
    CHECK(f.store->attempt(call.request.attempt_id).at("settlement_sha256").is_null());
    CHECK(f.store->submission(text(candidate.at("submission_id"))).at("business_accepted") == false);
    (void)runtime.accept_response(call, response);
    CHECK(f.task().at("status") == "ACCEPTED");
}

void model_acceptance_cannot_supply_identity_or_waiver() {
    for (const auto* forbidden : {"submission_id", "expected_task_version", "accepted_with_issues", "sha256"}) {
        Fixture f; (void)f.candidate();
        auto body = positive_acceptance(); body.as_object()[forbidden] = "forged";
        expect(ErrorCode::InvalidInput, [&] { (void)f.invoke(Role::Dispatcher, "accept_candidate", body); });
        CHECK(f.task().at("status") == "SUBMITTED" && f.store->experience_jobs(f.id).empty());
    }
    Fixture f; (void)f.candidate();
    auto body = positive_acceptance(); body.at("criteria_results").as_array().front().as_object().erase("evidence");
    expect(ErrorCode::InvalidInput, [&] { (void)f.invoke(Role::Dispatcher, "accept_candidate", body); });
    CHECK(f.task().at("status") == "SUBMITTED");
}

void amend_records_old_acceptance_response_without_applying_it() {
    Fixture f; const auto candidate = f.candidate();
    const FakeModel fake({step(Role::Dispatcher, "accept_candidate", positive_acceptance())});
    const auto call = f.capture(Role::Dispatcher, fake);
    (void)f.store->amend(f.store->host_actor(Role::Root, f.id), number(f.store->request(f.id).at("state_version")),
        read_runtime_file(source / "tests/fixtures/s13_amendment.txt"), fixture("s13_amended_constraints.json"), new_id("amend"));
    expect(ErrorCode::Stale, [&] {
        (void)OfflineRuntime(*f.store, *schemas, f.alpha.get()).accept_response(call, fake.complete(call.request));
    });
    CHECK(f.store->attempt(call.request.attempt_id).at("status") == "STALE");
    CHECK(!f.store->tool_feedback(call.request.attempt_id).is_null());
    CHECK(f.store->experience_jobs(f.id).empty());
    const auto historical = f.store->submission(text(candidate.at("submission_id")));
    CHECK(historical.at("status") == "SUPERSEDED");
    CHECK(historical.at("artifact_snapshot") == candidate.at("artifact_snapshot"));
}

void restart_accepts_original_candidate_without_new_business_order() {
    Fixture f; const auto candidate = f.candidate();
    const FakeModel old_fake({step(Role::Dispatcher, "accept_candidate", positive_acceptance())});
    const auto old_call = f.capture(Role::Dispatcher, old_fake);
    f.reopen();
    expect(ErrorCode::Stale, [&] {
        (void)OfflineRuntime(*f.store, *schemas, f.alpha.get()).accept_response(old_call, old_fake.complete(old_call.request));
    });
    EventFakeScript script;
    script.default_steps = {step(Role::Dispatcher, "accept_candidate", positive_acceptance())};
    script.by_node["alpha"] = {step(Role::TeamLead, "report_experience", no_experience())};
    const auto result = f.runtime().pump(f.id, script);
    CHECK(result.at("calls").as_array().size() == 2);
    CHECK(f.task().at("status") == "ACCEPTED" && f.task().at("instruction_revision") == 1);
    const auto current = f.store->submission(text(candidate.at("submission_id")));
    CHECK(current.at("original_binding") == candidate.at("original_binding"));
    CHECK(f.store->experience_jobs(f.id).front().at("status") == "DONE");
}

void rejected_acceptance_is_blocked_without_automatic_experience() {
    Fixture f; const auto candidate = f.candidate();
    (void)f.invoke(Role::Dispatcher, "accept_candidate", Object{{"passed", false}, {"notes", "The requested business result is not demonstrated."}});
    CHECK(f.task().at("status") != "ACCEPTED");
    CHECK(f.store->submission(text(candidate.at("submission_id"))).at("business_accepted") == false);
    CHECK(f.store->experience_jobs(f.id).empty());
    const auto budget = f.store->budget(f.id);
    const auto result = f.runtime().pump(f.id, EventFakeScript{});
    CHECK(result.at("calls").as_array().empty() && f.store->budget(f.id) == budget);
    CHECK(!result.at("pending").as_array().empty());
}

void experience_uses_frozen_source_and_report_ids_are_host_owned() {
    Fixture f; const auto job = f.accepted();
    write(f.directory.path / "alpha/result.txt", "changed after acceptance\n");
    const FakeModel fake({step(Role::TeamLead, "report_experience", no_experience())});
    const auto call = f.capture(Role::TeamLead, fake);
    CHECK(call.workflow_action.at("kind") == "experience");
    CHECK(call.workflow_action.at("target") == job);
    CHECK(call.request.context.tool_names == std::vector<std::string>{"report_experience"});
    CHECK(contract(call.request).at("experience_file_views").as_array().front().at("content") == "alpha candidate version 1\n");
    OfflineRuntime runtime(*f.store, *schemas, f.alpha.get());
    const auto response = fake.complete(call.request);
    const auto reported = runtime.accept_response(call, response);
    CHECK(runtime.accept_response(call, response) == reported);
    const auto current = f.store->experience_job(text(job.at("experience_id")));
    CHECK(current.at("status") == "DONE" && !current.at("report_ref").is_null());
    CHECK(current.at("source_snapshot") == job.at("source_snapshot"));
    CHECK(current.at("source_sha256") == job.at("source_sha256"));
    const auto budget = f.store->budget(f.id);
    const auto drained = f.runtime().pump(f.id, EventFakeScript{});
    CHECK(drained.at("calls").as_array().empty() && f.store->budget(f.id) == budget);
    const auto inbox = f.store->inbox(f.store->host_actor(Role::TeamLead, f.id, f.binding().team_id));
    CHECK(std::none_of(inbox.begin(), inbox.end(), [](const auto& item) { return item.at("type") == "accept_candidate"; }));
}

void reports_cannot_forge_identity_or_invent_memory_use() {
    for (const auto* forbidden : {"report_id", "request_id", "task_id", "team_id", "trace_digest_ref"}) {
        Fixture f; const auto job = f.accepted();
        auto body = no_experience(); body.as_object()[forbidden] = new_id("rep");
        expect(ErrorCode::InvalidInput, [&] { (void)f.invoke(Role::TeamLead, "report_experience", body); });
        CHECK(f.store->experience_job(text(job.at("experience_id"))).at("status") != "DONE");
    }
    Fixture f; const auto job = f.accepted();
    auto body = no_experience();
    body.at("memory_assessments").as_array().push_back(Object{{"memory_id", "exp-20261020-abcd"},
        {"verdict", "worked"}, {"evidence", "A claim without an immutable memory-load receipt."}, {"load_id", new_id("load")}});
    expect(ErrorCode::Unsupported, [&] { (void)f.invoke(Role::TeamLead, "report_experience", body); });
    CHECK(f.store->experience_job(text(job.at("experience_id"))).at("status") != "DONE");
}

void experience_proposals_are_reports_not_automatic_memory_publication() {
    Fixture f; const auto job = f.accepted();
    auto body = no_experience(); body.as_object()["no_new_experience"] = false;
    body.at("entries").as_array().push_back(Object{{"kind", "new"}, {"type", "experience"}, {"outcome", "success"},
        {"title", "Preserve submitted bytes"}, {"description", "Use the immutable snapshot after live workspace changes."},
        {"body_markdown", "Read the saved submission bytes when checking the original candidate."},
        {"proposed_scope", "project"}, {"evidence", "This generated fixture changed result.txt after submission."}});
    const auto report = f.invoke(Role::TeamLead, "report_experience", body);
    CHECK(!report.is_null());
    CHECK(f.store->experience_job(text(job.at("experience_id"))).at("status") == "DONE");
    CHECK(f.store->request(f.id).at("config_snapshot").at("system").at("memory").at("enabled") == false);
    CHECK(f.store->deliveries(f.id).empty());
}

void experience_capture_cannot_change_source_or_become_business_order() {
    Fixture f; const auto job = f.accepted();
    const FakeModel fake({step(Role::TeamLead, "report_experience", no_experience())});
    const auto call = f.capture(Role::TeamLead, fake);
    auto forged = call;
    forged.workflow_action.at("target").as_object()["source_sha256"] = std::string(64, '0');
    OfflineRuntime runtime(*f.store, *schemas, f.alpha.get());
    expect(ErrorCode::Conflict, [&] { (void)runtime.accept_response(forged, fake.complete(call.request)); });
    CHECK(f.store->attempt(call.request.attempt_id).at("settlement_sha256").is_null());
    auto response = fake.complete(call.request);
    response.tool_calls.front().name = "issue_work_order";
    response.tool_calls.front().arguments = full_script().by_node.at("alpha").front().tool_calls.front().arguments;
    expect(ErrorCode::InvalidInput, [&] { (void)runtime.accept_response(call, response); });
    CHECK(f.task().at("instruction_revision") == 1 && f.task().at("status") == "ACCEPTED");
    CHECK(f.store->experience_job(text(job.at("experience_id"))).at("status") != "DONE");
}

void historical_experience_can_finish_while_amended_task_awaits_disposition() {
    Fixture f; const auto job = f.accepted();
    auto next_plan = f.store->plan(f.id).at("body");
    (void)f.store->amend(f.store->host_actor(Role::Root, f.id), number(f.store->request(f.id).at("state_version")),
        "Change the next business result while preserving the original accepted source for experience.",
        fixture("s13_constraints.json"), new_id("amend"));
    next_plan.as_object()["plan_revision"] = 2;
    next_plan.as_object()["based_on_requirements_revision"] = 2;
    (void)f.store->publish_plan(f.store->host_actor(Role::Dispatcher, f.id),
        number(f.store->request(f.id).at("state_version")), 2, next_plan, new_id("plan"));
    CHECK(f.task().at("plan_revision") == 1 && f.store->request(f.id).at("plan_revision") == 2);
    const FakeModel fake({step(Role::TeamLead, "report_experience", no_experience())});
    const auto call = f.capture(Role::TeamLead, fake);
    const auto context = contract(call.request);
    CHECK(context.at("experience_scope").at("control_binding").at("requirements_revision") == 2);
    CHECK(context.at("experience_scope").at("original_source_binding").at("requirements_revision") == 1);
    CHECK(call.workflow_action.at("target").at("source_snapshot") == job.at("source_snapshot"));
    (void)OfflineRuntime(*f.store, *schemas, f.alpha.get()).accept_response(call, fake.complete(call.request));
    CHECK(f.store->experience_job(text(job.at("experience_id"))).at("status") == "DONE");
    CHECK(f.task().at("plan_revision") == 1 && f.task().at("instruction_revision") == 1);
    const auto result = f.runtime().pump(f.id, EventFakeScript{});
    CHECK(result.at("calls").as_array().empty() && blocked(result, "amendment_pending"));
    CHECK(f.task().at("status") != "ACCEPTED");
}

void unanswered_nonblocking_question_does_not_starve_accepted_experience() {
    Fixture f; (void)f.pump_to(2);
    const auto question = f.invoke(Role::Worker, "ask", Object{{"text", "Optional follow-up for a future version?"}, {"blocking", false}});
    write(f.directory.path / "alpha/result.txt", "alpha candidate version 1\n");
    const auto script = full_script();
    const auto submission = f.invoke(Role::Worker, "submit", script.by_node.at("alpha").at(2).tool_calls.front().arguments);
    const auto binding = f.binding();
    (void)f.store->check_submission(f.store->host_actor(Role::TeamLead, f.id, binding.team_id), binding,
        number(f.task().at("state_version")), text(submission.at("submission_id")), number(submission.at("state_version")),
        positive_check(), nullptr, new_id("check"));
    (void)f.invoke(Role::Dispatcher, "accept_candidate", positive_acceptance());
    EventFakeScript resumed; resumed.by_node["alpha"] = {step(Role::TeamLead, "report_experience", no_experience())};
    const auto result = f.runtime().pump(f.id, resumed);
    CHECK(result.at("calls").as_array().size() == 1);
    CHECK(result.at("calls").as_array().front().at("workflow_action").at("kind") == "experience");
    CHECK(f.store->experience_jobs(f.id).front().at("status") == "DONE");
    CHECK(f.store->question(text(question.at("question_id"))).at("status") == "OPEN");
    const auto inbox = f.store->inbox(f.store->host_actor(Role::TeamLead, f.id, binding.team_id));
    CHECK(std::any_of(inbox.begin(), inbox.end(), [](const auto& item) { return item.at("type") == "ask_question"; }));
}

void cancelled_request_recovers_only_pending_experience() {
    Fixture f; const auto job = f.accepted(); f.cancel(); f.reopen();
    EventFakeScript script;
    script.default_steps = full_script().default_steps;
    script.by_node["alpha"] = {step(Role::TeamLead, "report_experience", no_experience()),
        step(Role::Worker, "fs_write", Object{{"path", "forbidden.txt"}, {"content", "must never run"}})};
    const auto result = f.runtime().pump(f.id, script);
    CHECK(result.at("calls").as_array().size() == 1);
    CHECK(result.at("calls").as_array().front().at("role") == "team_lead");
    CHECK(result.at("request_status") == "CANCELLED" && result.at("business_delivered") == false);
    CHECK(f.store->experience_job(text(job.at("experience_id"))).at("status") == "DONE");
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/forbidden.txt"));
    const auto budget = f.store->budget(f.id);
    expect(ErrorCode::NotReady, [&] {
        (void)f.capture(Role::Worker, FakeModel({step(Role::Worker, "note", Object{{"text", "after cancellation"}})}));
    });
    CHECK(f.store->budget(f.id) == budget);
}

void stale_terminal_experience_response_does_not_complete_recovered_job() {
    Fixture f; const auto job = f.accepted(); f.cancel();
    const FakeModel fake({step(Role::TeamLead, "report_experience", no_experience())});
    const auto call = f.capture(Role::TeamLead, fake);
    f.reopen();
    expect(ErrorCode::Stale, [&] {
        (void)OfflineRuntime(*f.store, *schemas, f.alpha.get()).accept_response(call, fake.complete(call.request));
    });
    const auto current = f.store->experience_job(text(job.at("experience_id")));
    CHECK(current.at("status") != "DONE" && current.at("source_snapshot") == job.at("source_snapshot"));
    EventFakeScript script; script.by_node["alpha"] = {step(Role::TeamLead, "report_experience", no_experience())};
    const auto result = f.runtime().pump(f.id, script);
    CHECK(result.at("calls").as_array().size() == 1);
    CHECK(f.store->experience_job(text(job.at("experience_id"))).at("status") == "DONE");
}

void missing_scripts_and_exhausted_budget_leave_experience_pending() {
    auto admission = input(); admission.budget.max_attempts = 6;
    Fixture f(admission);
    const auto result = f.pump_to(12);
    CHECK(result.at("calls").as_array().size() == 6 && blocked(result, "quota"));
    const auto jobs = f.store->experience_jobs(f.id); CHECK(jobs.size() == 1 && jobs.front().at("status") != "DONE");
    const auto budget = f.store->budget(f.id);
    const auto missing = f.runtime().pump(f.id, EventFakeScript{});
    CHECK(missing.at("calls").as_array().empty() && blocked(missing, "missing_script"));
    CHECK(f.store->budget(f.id) == budget);
    const auto inbox = f.store->inbox(f.store->host_actor(Role::TeamLead, f.id, f.binding().team_id));
    CHECK(std::any_of(inbox.begin(), inbox.end(), [](const auto& item) { return item.at("type") == "accept_candidate"; }));
    f.cancel(); f.reopen();
    EventFakeScript script; script.by_node["alpha"] = {step(Role::TeamLead, "report_experience", no_experience())};
    const auto terminal = f.runtime().pump(f.id, script);
    CHECK(terminal.at("calls").as_array().empty() && blocked(terminal, "quota"));
    CHECK(f.store->experience_job(text(jobs.front().at("experience_id"))).at("status") != "DONE");
}

void independent_candidates_and_experience_do_not_share_targets() {
    Fixture f; auto script = full_script();
    auto& nodes = script.default_steps.front().tool_calls.front().arguments.at("nodes").as_array();
    auto beta = nodes.front(); beta.as_object()["node_id"] = "beta"; beta.as_object()["title"] = "Independent beta";
    nodes.push_back(std::move(beta));
    script.default_steps.push_back(step(Role::Dispatcher, "accept_candidate", positive_acceptance(), 3));
    script.by_node["beta"] = script.by_node.at("alpha");
    EventPumpOptions options; options.max_calls = 16; options.max_worker_calls_per_task = 2;
    const auto result = f.runtime(true).pump(f.id, script, options);
    CHECK(result.at("calls").as_array().size() == 13);
    CHECK(f.task("alpha").at("status") == "ACCEPTED" && f.task("beta").at("status") == "ACCEPTED");
    const auto jobs = f.store->experience_jobs(f.id); CHECK(jobs.size() == 2);
    CHECK(jobs[0].at("experience_id") != jobs[1].at("experience_id"));
    CHECK(jobs[0].at("team_id") != jobs[1].at("team_id"));
    for (const auto& job : jobs) {
        CHECK(job.at("status") == "DONE");
        CHECK(job.at("source_snapshot").at("submission").at("task_id") == job.at("task_id"));
    }
}

void ordinary_event_capacity_keeps_only_bounded_experience_reservation() {
    Fixture f; const auto job = f.accepted();
    test::fill_business_capacity(*f.store, f.directory.path / "control.db", f.id);
    const auto budget = f.store->budget(f.id);
    const auto plan = full_script().default_steps.front();
    expect(ErrorCode::NotReady, [&] { (void)f.capture(Role::Dispatcher, FakeModel({plan})); });
    CHECK(f.store->budget(f.id) == budget);
    f.cancel(); f.reopen();
    EventFakeScript script; script.by_node["alpha"] = {step(Role::TeamLead, "report_experience", no_experience())};
    const auto result = f.runtime().pump(f.id, script);
    CHECK(result.at("calls").as_array().size() == 1);
    CHECK(result.at("request_status") == "CANCELLED" && result.at("business_delivered") == false);
    CHECK(f.store->experience_job(text(job.at("experience_id"))).at("status") == "DONE");
    CHECK(number(f.store->budget(f.id).at("attempts")) == number(budget.at("attempts")) + 1);
}
} // namespace

int main() {
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    base_sql = read_runtime_file(source / "db/schema.sql");
    store_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    workflow_sql = read_runtime_file(source / "db/migrations/007_workflow_runtime.sql");
    acceptance_sql = read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql");
    return test::run({
        {"Dispatcher acceptance and explicit experience do not imply delivery", explicit_acceptance_and_experience_complete_without_implied_delivery},
        {"Dispatcher sees immutable candidate bytes and exact response replay", dispatcher_observes_original_bytes_and_response_replay_is_exact},
        {"raw Worker submission cannot bypass TeamLead checking", raw_submission_cannot_skip_team_lead_check},
        {"captured acceptance identity and task CAS cannot be relabelled", captured_target_and_task_cas_cannot_be_relabelled},
        {"model acceptance cannot forge identity hashes waivers or evidence", model_acceptance_cannot_supply_identity_or_waiver},
        {"AMEND records stale acceptance responses without applying them", amend_records_old_acceptance_response_without_applying_it},
        {"recovery accepts original candidate without reissuing business work", restart_accepts_original_candidate_without_new_business_order},
        {"negative business judgment blocks without inventing experience", rejected_acceptance_is_blocked_without_automatic_experience},
        {"experience uses frozen source and host-owned report identity", experience_uses_frozen_source_and_report_ids_are_host_owned},
        {"experience rejects forged identities and unbound memory claims", reports_cannot_forge_identity_or_invent_memory_use},
        {"experience proposals remain reports with memory disabled", experience_proposals_are_reports_not_automatic_memory_publication},
        {"captured experience source cannot be relabelled or become business work", experience_capture_cannot_change_source_or_become_business_order},
        {"old experience can finish while amended task still awaits disposition", historical_experience_can_finish_while_amended_task_awaits_disposition},
        {"accepted experience is not starved by an unanswered optional question", unanswered_nonblocking_question_does_not_starve_accepted_experience},
        {"terminal cancellation resumes only the bounded experience job", cancelled_request_recovers_only_pending_experience},
        {"terminal recovery fences previously captured experience response", stale_terminal_experience_response_does_not_complete_recovered_job},
        {"missing scripts and budget caps preserve unfinished experience", missing_scripts_and_exhausted_budget_leave_experience_pending},
        {"independent acceptance and experience targets remain isolated", independent_candidates_and_experience_do_not_share_targets},
        {"ordinary event exhaustion preserves bounded experience-only reservation", ordinary_event_capacity_keeps_only_bounded_experience_reservation}
    });
}
