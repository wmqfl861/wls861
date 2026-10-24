#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"

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
std::string base_sql, store_sql, tools_sql, events_sql, workflow_sql;

std::string text(const Json& value) { return std::string(value.as_string()); }
std::int64_t number(const Json& value) { return value.is_int64() ? value.as_int64() : static_cast<std::int64_t>(value.as_uint64()); }
Json fixture(const std::string& name) { return parse_json_strict(read_runtime_file(source / "tests/fixtures" / name)); }
EventFakeScript script(const std::string& name = "s14_fake_submit.json") { return event_fake_script_from_json(fixture(name)); }
void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary); stream << bytes; CHECK(static_cast<bool>(stream));
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
    const auto frozen = load_config_catalog(source / "config/examples/system.yaml", source / "config/examples/functions",
                                            source / "prompts").freeze("general");
    AdmissionInput result;
    result.tenant_id = "test-tenant"; result.project_id = "test-project"; result.root_id = "test-root";
    result.function_id = frozen.function_id(); result.idempotency_key = new_id("key");
    result.original_text = read_runtime_file(source / "tests/fixtures/s13_request.txt");
    result.config_snapshot = frozen.json(); result.constraint_catalog = fixture("s13_constraints.json");
    const auto grants = frozen.worker_tools(); result.tool_allowlist.assign(grants.begin(), grants.end());
    result.budget.input_tokens = 20000000; result.budget.output_tokens = 1000000;
    return result;
}
Json question_body(bool blocking = true) {
    return Object{{"text", "Which exact value should the original task preserve?"},
        {"context", "The worker has not changed the requested meaning."}, {"blocking", blocking},
        {"options", Array{Object{{"id", "original"}, {"label", "Keep the original value"}},
                         Object{{"id", "revised"}, {"label", "Use the revised value"}}}}};
}
Json submission_body() { return script().by_node.at("alpha").at(2).tool_calls.front().arguments; }
Json positive_check() {
    return Object{{"passed", true}, {"notes", "The captured original bytes meet the current task criteria."},
        {"criteria_results", Array{Object{{"criterion_id", "ac1"}, {"met", "yes"}}}}};
}
FakeModelStep step(Role role, const std::string& name, const Json& arguments, std::int64_t index = 1) {
    FakeModelStep value;
    value.role = role; value.role_call_index = index;
    value.tool_calls.push_back({"workflow-action", name, arguments});
    value.usage = {100, 20, 0, 0, 0};
    return value;
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
        [&](const auto& item) { return item.at("code").as_string() == code; });
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
            StoreOpenMode::Takeover, tools_sql, events_sql, workflow_sql);
        alpha = std::make_unique<WorkerTools>(*store, directory.path / "alpha");
        beta = std::make_unique<WorkerTools>(*store, directory.path / "beta");
    }
    EventRuntime runtime(bool two = false) {
        if (two) return EventRuntime(*store, *schemas, {{"alpha", alpha.get()}, {"beta", beta.get()}});
        return EventRuntime(*store, *schemas, {{"alpha", alpha.get()}});
    }
    Json task(const std::string& node = "alpha") const {
        for (const auto& value : store->tasks(id)) if (text(value.at("node_id")) == node) return value;
        throw std::runtime_error("test task missing");
    }
    ExecutionBinding binding() const { return store->binding(text(task().at("task_id"))); }
    void ready() {
        EventPumpOptions options; options.max_calls = 2;
        const auto result = runtime().pump(id, script(), options);
        CHECK(result.at("calls").as_array().size() == 2);
        CHECK(task().at("applied_instruction_revision") == task().at("instruction_revision"));
    }
    CapturedModelCall capture(Role role, const FakeModel& fake) {
        return OfflineRuntime(*store, *schemas, alpha.get()).reserve_call(fake, id, role,
            role == Role::Dispatcher ? std::string{} : binding().task_id);
    }
    Json invoke(Role role, const std::string& name, const Json& arguments) {
        const FakeModel fake({step(role, name, arguments)});
        const auto call = capture(role, fake);
        return OfflineRuntime(*store, *schemas, alpha.get()).accept_response(call, fake.complete(call.request));
    }
    Json ask(bool blocking = true) { return invoke(Role::Worker, "ask", question_body(blocking)); }
    Json submit() { return invoke(Role::Worker, "submit", submission_body()); }
};

void complete_bounded_rework_preserves_two_original_candidates() {
    Fixture f;
    EventPumpOptions options; options.max_worker_calls_per_task = 4;
    const auto result = f.runtime().pump(f.id, script("s14_fake_rework.json"), options);
    CHECK(result.at("calls").as_array().size() == 8);
    CHECK(result.at("business_delivered") == false && result.at("real_model_calls") == 0);
    CHECK(blocked(result, "acceptance_pending"));
    const auto task = f.task();
    CHECK(task.at("task_revision") == 2 && task.at("instruction_revision") == 2);
    CHECK(task.at("applied_task_revision") == 2 && task.at("applied_instruction_revision") == 2);
    CHECK(task.at("status") != "ACCEPTED");
    const auto submissions = f.store->submissions(f.binding().task_id);
    CHECK(submissions.size() == 2);
    CHECK(submissions.front().at("status") == "RETURNED_FOR_REWORK");
    CHECK(submissions.back().at("status") == "CANDIDATE_READY");
    CHECK(submissions.front().at("original_binding").at("task_revision") == 1);
    CHECK(submissions.back().at("original_binding").at("task_revision") == 2);
    CHECK(submissions.front().at("record_sha256") != submissions.back().at("record_sha256"));
    CHECK(text(submissions.front().at("artifact_snapshot").as_array().front().at("sha256")) ==
          sha256_hex("alpha candidate version 1\n"));
    const auto pending = f.store->inbox(f.store->host_actor(Role::Dispatcher, f.id));
    CHECK(std::any_of(pending.begin(), pending.end(), [](const auto& item) { return item.at("type") == "check_submission"; }));
    for (const auto& invocation : result.at("calls").as_array()) {
        const auto attempt_id = text(invocation.at("attempt_id"));
        CHECK(!f.store->tool_feedback(attempt_id).is_null());
        CHECK(!f.store->attempt(attempt_id).at("settlement_sha256").is_null());
    }
}

void check_reads_original_snapshot_not_mutated_workspace() {
    Fixture f; f.ready(); write(f.directory.path / "alpha/result.txt", "original frozen bytes\n");
    const auto submitted = f.submit();
    const FakeModel fake({step(Role::TeamLead, "check_submission", positive_check())});
    const auto call = f.capture(Role::TeamLead, fake);
    CHECK(call.workflow_action.at("target").at("submission_id") == submitted.at("submission_id"));
    const auto input_contract = contract(call.request);
    CHECK(input_contract.at("submission_file_views").as_array().front().at("content") == "original frozen bytes\n");
    CHECK(input_contract.at("submission_file_views").as_array().front().at("binary") == false);
    write(f.directory.path / "alpha/result.txt", "later host mutation must not replace the submitted bytes\n");
    OfflineRuntime runtime(*f.store, *schemas, f.alpha.get());
    const auto response = fake.complete(call.request);
    const auto checked = runtime.accept_response(call, response);
    CHECK(checked.at("status") == "CANDIDATE_READY");
    CHECK(checked.at("artifact_snapshot") == submitted.at("artifact_snapshot"));
    CHECK(runtime.accept_response(call, response) == checked);
    CHECK(f.store->submission(text(submitted.at("submission_id"))).at("checks").as_array().size() == 1);
    const auto budget = f.store->budget(f.id);
    expect(ErrorCode::NotReady, [&] { (void)f.capture(Role::Worker, FakeModel({step(Role::Worker, "note", Object{{"text", "no more work"}})})); });
    CHECK(f.store->budget(f.id) == budget);
}

void binary_snapshot_remains_lossless_and_explicit() {
    Fixture f; f.ready();
    const std::string bytes("a\0\xff\xfez", 5);
    write(f.directory.path / "alpha/result.txt", bytes);
    const auto submitted = f.submit();
    CHECK(text(submitted.at("artifact_snapshot").as_array().front().at("sha256")) == sha256_hex(bytes));
    const FakeModel fake({step(Role::TeamLead, "check_submission", positive_check())});
    const auto call = f.capture(Role::TeamLead, fake);
    const auto view = contract(call.request).at("submission_file_views").as_array().front();
    CHECK(view.at("content").is_null() && view.at("binary") == true && view.at("size_bytes") == 5);
    CHECK(call.workflow_action.at("target").at("artifact_snapshot") == submitted.at("artifact_snapshot"));
    CHECK(OfflineRuntime(*f.store, *schemas, f.alpha.get()).accept_response(call, fake.complete(call.request)).at("status") == "CANDIDATE_READY");
}

void question_chain_releases_sibling_and_user_answer_wakes_exact_wait() {
    Fixture f;
    auto steps = script("s14_fake_ask.json");
    steps.by_node.at("alpha").push_back(step(Role::TeamLead, "forward_question", Object{{"note", "Need request-level clarification."}}, 2));
    steps.default_steps.push_back(step(Role::Dispatcher, "forward_question", Object{{"note", "The owner must choose the intended value."}}, 2));
    const auto result = f.runtime(true).pump(f.id, steps);
    CHECK(result.at("calls").as_array().size() == 7);
    const auto questions = f.store->questions(f.id);
    CHECK(questions.size() == 1 && questions.front().at("holder") == "user");
    CHECK(questions.front().at("status") == "OPEN" && questions.front().at("forwarded").as_array().size() == 2);
    CHECK(std::filesystem::exists(f.directory.path / "beta/r1.txt"));
    const auto original_body = questions.front().at("body");
    const auto wait = f.store->worker_wait(f.binding().task_id);
    CHECK(wait.at("status") == "WAITING");
    const auto sibling = f.task("beta");
    const auto answered = f.store->answer_question(f.store->host_actor(Role::Root, f.id), f.store->request_binding(f.id),
        text(questions.front().at("question_id")), number(questions.front().at("state_version")),
        Object{{"text", "Keep the original requested value."}}, new_id("answer"));
    CHECK(answered.at("status") == "ANSWERED" && answered.at("body") == original_body);
    CHECK(f.store->worker_wait(f.binding().task_id).at("status") == "AWAKE");
    CHECK(f.task("beta") == sibling);
    EventFakeScript resumed;
    resumed.by_node["alpha"] = {step(Role::Worker, "fs_write", Object{{"path", "answered.txt"}, {"content", "answer resumed only alpha\n"}})};
    resumed.by_node["beta"] = {};
    const auto next = f.runtime(true).pump(f.id, resumed);
    CHECK(next.at("calls").as_array().size() == 1);
    CHECK(read_runtime_file(f.directory.path / "alpha/answered.txt") == "answer resumed only alpha\n");
    const auto attempt = f.store->attempt(text(next.at("calls").as_array().front().at("attempt_id")));
    const auto request = model_request_from_json(attempt.at("request_envelope"));
    CHECK(contract(request).at("questions").as_array().front().at("answer").at("text") == "Keep the original requested value.");
}

void original_target_and_question_cas_reject_retargeting() {
    Fixture f; f.ready(); const auto first = f.ask(false);
    const FakeModel fake({step(Role::TeamLead, "answer_question", Object{{"text", "First captured answer."}})});
    const auto captured = f.capture(Role::TeamLead, fake);
    const auto second = f.ask(false);
    CHECK(first.at("question_id") != second.at("question_id"));
    auto forged = captured;
    forged.workflow_action.as_object()["target"] = second;
    const auto response = fake.complete(captured.request);
    expect(ErrorCode::Conflict, [&] { (void)OfflineRuntime(*f.store, *schemas, f.alpha.get()).accept_response(forged, response); });
    const auto answered = OfflineRuntime(*f.store, *schemas, f.alpha.get()).accept_response(captured, response);
    CHECK(answered.at("question_id") == first.at("question_id"));
    CHECK(f.store->question(text(second.at("question_id"))).at("status") == "OPEN");
    const auto stale = f.capture(Role::TeamLead, fake);
    const auto target = stale.workflow_action.at("target");
    (void)f.store->forward_question(stale.actor, stale.request.binding, text(target.at("question_id")),
        number(target.at("state_version")), "Forwarded after the model began.", new_id("forward"));
    expect(ErrorCode::NotReady, [&] {
        (void)OfflineRuntime(*f.store, *schemas, f.alpha.get()).accept_response(stale, fake.complete(stale.request));
    });
    CHECK(f.store->question(text(second.at("question_id"))).at("holder") == "dispatcher");
    CHECK(f.store->tool_feedback(stale.request.attempt_id).at("result").at("runtime_error") == true);
}

void model_question_arguments_cannot_forge_targets_or_versions() {
    Fixture f; f.ready(); const auto question = f.ask();
    for (const auto& arguments : std::vector<Json>{
            Object{{"text", "answer"}, {"question_id", question.at("question_id")}},
            Object{{"text", "answer"}, {"state_version", 1}},
            Object{{"text", "answer"}, {"execution_epoch", f.binding().execution_epoch}},
            Object{{"text", " \t\n"}}, Object{}}) {
        expect(ErrorCode::InvalidInput, [&] { (void)f.invoke(Role::TeamLead, "answer_question", arguments); });
        CHECK(f.store->question(text(question.at("question_id"))).at("status") == "OPEN");
    }
    expect(ErrorCode::InvalidInput, [&] {
        (void)f.invoke(Role::TeamLead, "forward_question", Object{{"note", "forward"}, {"holder", "user"}});
    });
    const auto order = script().by_node.at("alpha").front().tool_calls.front().arguments;
    expect(ErrorCode::InvalidInput, [&] { (void)f.invoke(Role::TeamLead, "issue_work_order", order); });
    CHECK(f.store->worker_wait(f.binding().task_id).at("status") == "WAITING");
    CHECK(f.task().at("instruction_revision") == 1);
}

void check_schema_refuses_self_approval_and_invalid_rework_shapes() {
    Fixture f; f.ready(); write(f.directory.path / "alpha/result.txt", "bounded candidate\n");
    auto forged_submission = submission_body();
    forged_submission.as_object()["team_lead_check"] = Object{{"passed", true}};
    expect(ErrorCode::InvalidInput, [&] { (void)f.invoke(Role::Worker, "submit", forged_submission); });
    forged_submission = submission_body(); forged_submission.as_object()["submission_id"] = new_id("sub");
    expect(ErrorCode::InvalidInput, [&] { (void)f.invoke(Role::Worker, "submit", forged_submission); });
    const auto submitted = f.submit();
    const auto order = script().by_node.at("alpha").front().tool_calls.front().arguments;
    for (const auto& arguments : std::vector<Json>{Object{{"passed", false}},
            Object{{"passed", true}, {"work_order", order}},
            Object{{"passed", true}, {"submission_id", submitted.at("submission_id")}},
            Object{{"passed", true}, {"state_version", 1}}}) {
        expect(ErrorCode::InvalidInput, [&] { (void)f.invoke(Role::TeamLead, "check_submission", arguments); });
        CHECK(f.store->submission(text(submitted.at("submission_id"))).at("status") == "SUBMITTED");
    }
    Json forged_order = order; forged_order.as_object()["task_revision"] = 999;
    expect(ErrorCode::InvalidInput, [&] {
        (void)f.invoke(Role::TeamLead, "check_submission", Object{{"passed", false}, {"work_order", forged_order}});
    });
    CHECK(f.task().at("instruction_revision") == 1 && f.task().at("task_revision") == 1);
}

void snapshot_requires_complete_authorized_bounded_files() {
    for (const auto& mode : {"missing", "oversized", "traversal", "duplicate", "too_many"}) {
        Fixture f; f.ready(); auto body = submission_body();
        if (std::string(mode) == "oversized") write(f.directory.path / "alpha/result.txt", std::string(65537, 'x'));
        else if (std::string(mode) != "missing") write(f.directory.path / "alpha/result.txt", "small\n");
        if (std::string(mode) == "traversal") body.at("deliverables").as_array().front().as_object()["path"] = "/work/../control.db";
        if (std::string(mode) == "duplicate") body.at("deliverables").as_array().push_back(body.at("deliverables").as_array().front());
        if (std::string(mode) == "too_many") {
            body.at("deliverables") = Array{};
            for (int i = 0; i < 9; ++i) body.at("deliverables").as_array().push_back(Object{{"path", "/work/file" + std::to_string(i)}, {"description", "file"}});
        }
        const auto code = std::string(mode) == "oversized" || std::string(mode) == "too_many" ? ErrorCode::Quota :
            (std::string(mode) == "traversal" ? ErrorCode::Unauthorized : ErrorCode::InvalidInput);
        expect(code,
            [&] { (void)f.invoke(Role::Worker, "submit", body); });
        CHECK(f.store->current_submission(f.binding().task_id).is_null());
        CHECK(f.task().at("status") != "SUBMITTED");
        CHECK(f.store->submissions(f.binding().task_id).empty());
    }
    auto admission = input();
    std::erase(admission.tool_allowlist, "fs_read");
    Fixture f(admission); f.ready(); write(f.directory.path / "alpha/result.txt", "private\n");
    expect(ErrorCode::InvalidInput, [&] { (void)f.submit(); });
    CHECK(f.store->current_submission(f.binding().task_id).is_null());
}

void ask_only_policy_declares_only_authorized_workflow_action() {
    for (const bool with_workspace : {false, true}) {
        auto admission = input(); admission.tool_allowlist = {"ask"};
        Fixture f(admission); f.ready();
        const FakeModel fake({step(Role::Worker, "ask", question_body())});
        OfflineRuntime note_free(*f.store, *schemas, with_workspace ? f.alpha.get() : nullptr);
        const auto call = note_free.reserve_call(fake, f.id, Role::Worker, f.binding().task_id);
        CHECK(call.request.context.tool_names == std::vector<std::string>{"ask"});
        CHECK(note_free.accept_response(call, fake.complete(call.request)).at("status") == "OPEN");
        CHECK(f.store->worker_wait(f.binding().task_id).at("status") == "WAITING");
    }
}

void legacy_entry_cannot_erase_pending_question_or_submission() {
    for (const bool question : {true, false}) {
        Fixture f; f.ready();
        if (question) (void)f.ask();
        else { write(f.directory.path / "alpha/result.txt", "candidate\n"); (void)f.submit(); }
        const auto budget = f.store->budget(f.id), task = f.task();
        auto grouped = script(); auto steps = grouped.default_steps;
        steps.insert(steps.end(), grouped.by_node.at("alpha").begin(), grouped.by_node.at("alpha").end());
        expect(ErrorCode::NotReady, [&] { (void)OfflineRuntime(*f.store, *schemas, f.alpha.get()).run(f.id, steps); });
        CHECK(f.store->budget(f.id) == budget && f.task() == task);
    }
}

void restart_answers_same_question_then_applies_new_epoch_order() {
    Fixture f; f.ready(); const auto question = f.ask();
    const auto original = question.at("original_binding");
    f.reopen();
    CHECK(f.binding().execution_epoch > number(original.at("execution_epoch")));
    EventFakeScript resumed;
    resumed.by_node["alpha"] = {
        step(Role::TeamLead, "answer_question", Object{{"text", "The original requested value remains correct."}}, 1),
        step(Role::Worker, "fs_write", Object{{"path", "after-restart.txt"}, {"content", "new execution, original answer\n"}}, 1)};
    const auto result = f.runtime().pump(f.id, resumed);
    CHECK(result.at("calls").as_array().size() == 2);
    const auto answered = f.store->question(text(question.at("question_id")));
    // Recovery needs no new interpretation: the answered semantic question
    // remains applicable and the original answer/history stay unchanged.
    CHECK(answered.at("status") == "ANSWERED" && answered.at("original_binding") == original);
    CHECK(answered.at("answer").at("text") == "The original requested value remains correct.");
    CHECK(std::any_of(answered.at("transitions").as_array().begin(), answered.at("transitions").as_array().end(),
        [](const auto& transition) { return transition.at("kind") == "ANSWERED"; }));
    CHECK(answered.at("body") == question.at("body"));
    CHECK(f.task().at("instruction_revision") == 1 && f.task().at("applied_instruction_revision") == 1);
    CHECK(read_runtime_file(f.directory.path / "alpha/after-restart.txt") == "new execution, original answer\n");
}

void restart_checks_submitted_bytes_without_reissuing_order() {
    Fixture f; f.ready(); write(f.directory.path / "alpha/result.txt", "before restart\n");
    const auto submission = f.submit();
    f.reopen(); write(f.directory.path / "alpha/result.txt", "after restart live bytes\n");
    EventFakeScript resumed; resumed.by_node["alpha"] = {step(Role::TeamLead, "check_submission", positive_check())};
    const auto result = f.runtime().pump(f.id, resumed);
    CHECK(result.at("calls").as_array().size() == 1 && blocked(result, "acceptance_pending"));
    CHECK(f.task().at("instruction_revision") == 1);
    const auto checked = f.store->submission(text(submission.at("submission_id")));
    CHECK(checked.at("status") == "CANDIDATE_READY");
    CHECK(checked.at("original_binding") == submission.at("original_binding"));
    CHECK(checked.at("artifact_snapshot") == submission.at("artifact_snapshot"));
}

void stale_check_after_amend_cannot_relabel_old_candidate() {
    Fixture f; f.ready(); write(f.directory.path / "alpha/result.txt", "old candidate\n");
    const auto submitted = f.submit();
    const FakeModel fake({step(Role::TeamLead, "check_submission", positive_check())});
    const auto call = f.capture(Role::TeamLead, fake);
    (void)f.store->amend(f.store->host_actor(Role::Root, f.id), number(f.store->request(f.id).at("state_version")),
        read_runtime_file(source / "tests/fixtures/s13_amendment.txt"), fixture("s13_amended_constraints.json"), new_id("amend"));
    expect(ErrorCode::Stale, [&] { (void)OfflineRuntime(*f.store, *schemas, f.alpha.get()).accept_response(call, fake.complete(call.request)); });
    const auto historical = f.store->submission(text(submitted.at("submission_id")));
    CHECK(historical.at("status") == "SUPERSEDED");
    CHECK(historical.at("original_binding") == submitted.at("original_binding"));
    CHECK(historical.at("artifact_snapshot") == submitted.at("artifact_snapshot"));
    CHECK(historical.at("checks").as_array().empty());
}

void missing_script_and_budget_preserve_submission_events_without_retry() {
    auto admission = input(); admission.budget.max_attempts = 4;
    Fixture f(admission);
    EventPumpOptions options; options.max_worker_calls_per_task = 4;
    const auto result = f.runtime().pump(f.id, script("s14_fake_rework.json"), options);
    CHECK(result.at("calls").as_array().size() == 4 && blocked(result, "quota"));
    const auto submission = f.store->current_submission(f.binding().task_id);
    CHECK(!submission.is_null() && submission.at("status") == "SUBMITTED");
    CHECK(submission.at("checks").as_array().empty());
    const auto inbox = f.store->inbox(f.store->host_actor(Role::TeamLead, f.id, f.binding().team_id));
    CHECK(std::any_of(inbox.begin(), inbox.end(), [](const auto& item) { return item.at("type") == "submit_work"; }));
    const auto budget = f.store->budget(f.id);
    const auto empty = f.runtime().pump(f.id, EventFakeScript{});
    CHECK(empty.at("calls").as_array().empty() && blocked(empty, "missing_script"));
    CHECK(f.store->budget(f.id) == budget);
    const auto after = f.store->inbox(f.store->host_actor(Role::TeamLead, f.id, f.binding().team_id));
    CHECK(after.size() == inbox.size());
    for (std::size_t i = 0; i < inbox.size(); ++i) CHECK(after[i].at("inbox_id") == inbox[i].at("inbox_id"));
}
} // namespace

int main() {
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    base_sql = read_runtime_file(source / "db/schema.sql");
    store_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    workflow_sql = read_runtime_file(source / "db/migrations/007_workflow_runtime.sql");
    return test::run({
        {"bounded submit-check-rework-resubmit keeps original candidates", complete_bounded_rework_preserves_two_original_candidates},
        {"TeamLead checks original snapshot despite later workspace mutation", check_reads_original_snapshot_not_mutated_workspace},
        {"binary submission bytes remain lossless and explicitly non-text", binary_snapshot_remains_lossless_and_explicit},
        {"question forwarding and user answer release only the matching wait", question_chain_releases_sibling_and_user_answer_wakes_exact_wait},
        {"captured question identity and CAS reject retargeting and stale decisions", original_target_and_question_cas_reject_retargeting},
        {"question model inputs reject identity CAS and order bypass", model_question_arguments_cannot_forge_targets_or_versions},
        {"submission and check schemas reject self-approval and forged rework", check_schema_refuses_self_approval_and_invalid_rework_shapes},
        {"submission snapshots require complete authorized bounded files", snapshot_requires_complete_authorized_bounded_files},
        {"ask-only Worker policy does not require or declare note", ask_only_policy_declares_only_authorized_workflow_action},
        {"legacy runner cannot erase a pending question or submission", legacy_entry_cannot_erase_pending_question_or_submission},
        {"restart answers original question before applying new epoch order", restart_answers_same_question_then_applies_new_epoch_order},
        {"restart checks submitted original bytes without replacing its order", restart_checks_submitted_bytes_without_reissuing_order},
        {"AMEND rejects stale checks and preserves original candidate bindings", stale_check_after_amend_cannot_relabel_old_candidate},
        {"missing script and exhausted budget retain unprocessed submissions", missing_script_and_budget_preserve_submission_events_without_retry}
    });
}
