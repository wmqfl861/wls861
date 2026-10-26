#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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
Json fixture(const std::string& name) {
    return parse_json_strict(read_runtime_file(source / "tests/fixtures" / name));
}
EventFakeScript initial_script() { return event_fake_script_from_json(fixture("s13_fake_initial.json")); }
EventFakeScript amended_script() { return event_fake_script_from_json(fixture("s13_fake_amended.json")); }
Json decision(const std::string& kind) {
    return Object{{"disposition", kind}, {"rationale", "Explicitly considered the current original AMEND and task."}};
}
Json plan_arguments(const Json& decisions) {
    return Object{{"plan", amended_script().default_steps.front().tool_calls.front().arguments},
        {"dispositions", decisions}};
}
FakeModelStep step(Role role, const std::string& name, const Json& arguments, std::int64_t index = 1) {
    FakeModelStep result;
    result.role = role; result.role_call_index = index;
    result.tool_calls.push_back({"amend-action", name, arguments});
    result.usage = {100, 20, 0, 0, 0};
    return result;
}
void expect(std::initializer_list<ErrorCode> codes, const std::function<void()>& action) {
    bool caught = false;
    try { action(); } catch (const Error& error) {
        if (std::find(codes.begin(), codes.end(), error.code()) == codes.end())
            throw std::runtime_error(std::string("unexpected ") + std::string(error_name(error.code())) + ": " + error.what());
        caught = true;
    }
    CHECK(caught);
}
AdmissionInput input(bool structured = false) {
    test::TempDir directory;
    auto yaml = read_runtime_file(source / "config/examples/system.yaml");
    if (structured) {
        const std::string old = "request_format: framed-v1";
        const auto at = yaml.find(old); CHECK(at != std::string::npos);
        yaml.replace(at, old.size(), "request_format: structured-v2");
    }
    { std::ofstream output(directory.path / "system.yaml"); output << yaml; CHECK(static_cast<bool>(output)); }
    const auto frozen = load_config_catalog(directory.path / "system.yaml", source / "config/examples/functions",
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
bool blocked(const Json& result, std::string_view code, std::string_view node = {}) {
    return std::any_of(result.at("blocked").as_array().begin(), result.at("blocked").as_array().end(),
        [&](const auto& row) { return row.at("code").as_string() == code &&
            (node.empty() || row.at("node_id").as_string() == node); });
}
struct Fixture {
    test::TempDir directory;
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> alpha, beta;
    std::string id;
    explicit Fixture(bool structured = false) {
        std::filesystem::create_directory(directory.path / "alpha");
        std::filesystem::create_directory(directory.path / "beta");
        reopen(); id = text(store->admit(input(structured)).at("request_id"));
    }
    void reopen(StoreRecoveryScope scope = {}) {
        alpha.reset(); beta.reset(); store.reset();
        store = std::make_unique<Store>(directory.path / "control.db", base_sql, store_sql,
            StoreOpenMode::Takeover, tools_sql, events_sql, workflow_sql, acceptance_sql,
            std::string_view{}, std::move(scope));
        alpha = std::make_unique<WorkerTools>(*store, directory.path / "alpha");
        beta = std::make_unique<WorkerTools>(*store, directory.path / "beta");
    }
    EventRuntime runtime() { return EventRuntime(*store, *schemas, {{"alpha", alpha.get()}, {"beta", beta.get()}}); }
    OfflineRuntime offline() { return OfflineRuntime(*store, *schemas, alpha.get()); }
    Json task(const std::string& node = "alpha") const {
        for (const auto& row : store->tasks(id)) if (text(row.at("node_id")) == node) return row;
        throw std::runtime_error("fixture task missing");
    }
    ExecutionBinding binding(const std::string& node = "alpha") const {
        return store->binding(text(task(node).at("task_id")));
    }
    void initial() {
        EventPumpOptions options; options.max_worker_calls_per_task = 1;
        const auto result = runtime().pump(id, initial_script(), options);
        CHECK(result.at("calls").as_array().size() == 5);
        CHECK(std::filesystem::exists(directory.path / "alpha/r1.txt"));
        CHECK(std::filesystem::exists(directory.path / "beta/r1.txt"));
    }
    void amend() {
        (void)store->amend(store->host_actor(Role::Root, id), number(store->request(id).at("state_version")),
            read_runtime_file(source / "tests/fixtures/s13_amendment.txt"), fixture("s13_amended_constraints.json"), new_id("amend"));
    }
    CapturedModelCall capture(const FakeModel& fake) {
        return offline().reserve_call(fake, id, Role::Dispatcher, {}, 1, {}, true);
    }
    Json decide(const std::string& name, const Json& arguments) {
        const FakeModel fake({step(Role::Dispatcher, name, arguments)});
        const auto call = capture(fake);
        return offline().accept_independent_response(call, fake.complete(call.request));
    }
    void order_and_ack(const std::string& node) {
        const auto order = amended_script().by_node.at(node).front().tool_calls.front().arguments;
        const FakeModel fake({step(Role::TeamLead, "issue_work_order", order)});
        const auto call = offline().reserve_call(fake, id, Role::TeamLead, binding(node).task_id);
        (void)offline().accept_response(call, fake.complete(call.request));
        const auto current = binding(node);
        (void)store->acknowledge(store->host_actor(Role::Worker, id, current.team_id), current,
            number(task(node).at("state_version")), new_id("ack"));
    }
    void raw(const std::string& statement) {
        // Fault/race injection is confined to this fixture's disposable database.
        sqlite3* database = nullptr;
        CHECK(sqlite3_open_v2((directory.path / "control.db").c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
        const auto rc = sqlite3_exec(database, statement.c_str(), nullptr, nullptr, nullptr);
        const std::string error = sqlite3_errmsg(database); sqlite3_close(database);
        if (rc != SQLITE_OK) throw std::runtime_error(error);
    }
};

EventFakeScript model_plan_script(const Json& decisions) {
    auto script = amended_script();
    script.default_steps = {step(Role::Dispatcher, "submit_amendment_plan", plan_arguments(decisions))};
    return script;
}

void capture_uses_original_task_snapshots_and_cannot_be_retargeted() {
    Fixture f; f.initial(); f.amend();
    const FakeModel fake({step(Role::Dispatcher, "submit_amendment_plan",
        plan_arguments(Object{{"alpha", decision("update")}}))});
    const auto call = f.capture(fake);
    CHECK(call.workflow_action.at("kind") == "amendment");
    const auto& target = call.workflow_action.at("target");
    CHECK(target.at("plan_required") == true);
    CHECK(target.at("tasks").as_array().size() == 2 && target.at("pending_node_ids").as_array().size() == 2);
    CHECK(call.request.context.tool_names == std::vector<std::string>{"submit_amendment_plan"});
    const auto durable = f.store->attempt(call.request.attempt_id);
    CHECK(durable.at("request_envelope") == to_json(call.request));
    CHECK(canonical_json(durable.at("request_envelope")).find("amendment") != std::string::npos);
    auto forged = call;
    forged.workflow_action.at("target").at("tasks").as_array().front().as_object()["task_id"] = new_id("tsk");
    expect({ErrorCode::Conflict}, [&] { (void)f.offline().accept_independent_response(forged, fake.complete(call.request)); });
    CHECK(f.store->attempt(call.request.attempt_id).at("settlement_sha256").is_null());
    (void)f.offline().accept_independent_response(call, fake.complete(call.request));
    CHECK(f.task().at("based_on_requirements_revision") == 2);
}

void partial_model_plan_and_later_keep_use_one_plan_and_new_ack() {
    for (const bool structured : {false, true}) {
        Fixture f(structured); f.initial();
        const auto original_beta = f.task("beta");
        const auto old_instruction = f.store->instruction(f.binding("beta").task_id);
        f.amend();
        const auto first = f.runtime().pump(f.id, model_plan_script(Object{{"alpha", decision("update")}}));
        CHECK(first.at("calls").as_array().size() == 3);
        CHECK(blocked(first, "amendment_pending", "beta"));
        CHECK(read_runtime_file(f.directory.path / "alpha/r2.txt") == "alpha revision 2\n");
        CHECK(!std::filesystem::exists(f.directory.path / "beta/r2.txt"));
        CHECK(f.task("beta").at("based_on_requirements_revision") == 1);
        const auto after_alpha = f.task();
        const auto alpha_order = f.store->instruction(f.binding().task_id);
        auto followup = amended_script(); followup.by_node["alpha"].clear();
        followup.default_steps = {step(Role::Dispatcher, "resolve_amendment",
            Object{{"dispositions", Object{{"beta", decision("keep")}}}})};
        const auto second = f.runtime().pump(f.id, followup);
        CHECK(second.at("calls").as_array().size() == 3);
        CHECK(f.store->request(f.id).at("plan_revision") == 2);
        CHECK(f.task() == after_alpha);
        CHECK(f.store->instruction(f.binding().task_id) == alpha_order);
        const auto beta = f.task("beta");
        CHECK(beta.at("task_revision") == number(original_beta.at("task_revision")) + 1);
        CHECK(beta.at("instruction_revision") == number(original_beta.at("instruction_revision")) + 1);
        CHECK(beta.at("applied_task_revision") == beta.at("task_revision"));
        CHECK(beta.at("applied_instruction_revision") == beta.at("instruction_revision"));
        CHECK(f.store->instruction(f.binding("beta").task_id).at("body") != old_instruction.at("body"));
        CHECK(read_runtime_file(f.directory.path / "beta/r2.txt") == "beta revision 2\n");
        CHECK(f.store->inbox(f.store->host_actor(Role::Dispatcher, f.id)).empty());
    }
}

void keep_alone_does_not_authorize_worker_or_rewrite_old_instruction() {
    Fixture f; f.initial();
    const auto old = f.store->instruction(f.binding().task_id);
    f.amend();
    (void)f.decide("submit_amendment_plan", plan_arguments(Object{{"alpha", decision("keep")}}));
    CHECK(f.task().at("applied_task_revision") != f.task().at("task_revision"));
    CHECK(f.store->instruction(f.binding().task_id).at("body") == old.at("body"));
    const FakeModel worker({step(Role::Worker, "fs_read", Object{{"path", "r1.txt"}, {"max_bytes", 16}})});
    expect({ErrorCode::NotReady, ErrorCode::Stale}, [&] {
        (void)f.offline().reserve_call(worker, f.id, Role::Worker, f.binding().task_id);
    });
    f.order_and_ack("alpha");
    CHECK(f.task().at("applied_task_revision") == f.task().at("task_revision"));
    const auto call = f.offline().reserve_call(worker, f.id, Role::Worker, f.binding().task_id);
    (void)f.offline().accept_response(call, worker.complete(call.request));
    CHECK(read_runtime_file(f.directory.path / "alpha/r1.txt") == "alpha original\n");
}

void stale_task_cas_rejects_model_plan_even_when_request_cas_is_unchanged() {
    Fixture f; f.initial(); f.amend();
    const FakeModel fake({step(Role::Dispatcher, "submit_amendment_plan", plan_arguments(Object{{"alpha", decision("update")}}))});
    const auto call = f.capture(fake);
    const auto request = f.store->request(f.id);
    f.raw("UPDATE tasks SET state_version=state_version+1 WHERE task_id='" + f.binding("beta").task_id + "'");
    CHECK(f.store->request(f.id) == request);
    const auto before = f.store->tasks(f.id);
    expect({ErrorCode::Conflict, ErrorCode::Stale}, [&] {
        (void)f.offline().accept_independent_response(call, fake.complete(call.request));
    });
    CHECK(f.store->tasks(f.id) == before);
    CHECK(f.store->request(f.id).at("plan_revision") == 1);
    CHECK(!f.store->attempt(call.request.attempt_id).at("settlement_sha256").is_null());
}

void unresolved_target_cas_is_checked_without_fencing_current_sibling_progress() {
    Fixture f; f.initial(); f.amend();
    (void)f.decide("submit_amendment_plan", plan_arguments(Object{{"alpha", decision("update")}}));
    const FakeModel fake({step(Role::Dispatcher, "resolve_amendment", Object{{"dispositions", Object{{"beta", decision("keep")}}}})});
    const auto call = f.capture(fake);
    CHECK(call.workflow_action.at("target").at("plan_required") == false);
    CHECK(call.workflow_action.at("target").at("tasks").as_array().size() == 1);
    CHECK(call.request.context.tool_names == std::vector<std::string>{"resolve_amendment"});
    const auto request = f.store->request(f.id);
    f.order_and_ack("alpha");
    CHECK(f.store->request(f.id) == request);
    const auto progressed = f.task();
    (void)f.offline().accept_independent_response(call, fake.complete(call.request));
    CHECK(f.task() == progressed);
    CHECK(f.task("beta").at("based_on_requirements_revision") == 2);
    CHECK(f.store->request(f.id).at("plan_revision") == 2);
}

void stale_pending_target_is_not_relabelled_on_followup() {
    Fixture f; f.initial(); f.amend();
    (void)f.decide("submit_amendment_plan", plan_arguments(Object{{"alpha", decision("update")}}));
    const FakeModel fake({step(Role::Dispatcher, "resolve_amendment", Object{{"dispositions", Object{{"beta", decision("keep")}}}})});
    const auto call = f.capture(fake);
    const auto request = f.store->request(f.id);
    f.raw("UPDATE tasks SET state_version=state_version+1 WHERE task_id='" + f.binding("beta").task_id + "'");
    CHECK(f.store->request(f.id) == request);
    const auto before = f.task("beta");
    expect({ErrorCode::Conflict, ErrorCode::Stale}, [&] {
        (void)f.offline().accept_independent_response(call, fake.complete(call.request));
    });
    CHECK(f.task("beta") == before && f.store->request(f.id).at("plan_revision") == 2);
}

void later_amend_and_scoped_reopen_fence_original_decisions_and_retain_usage() {
    for (const bool reopen : {false, true}) {
        Fixture f; f.initial(); f.amend();
        const auto other = text(f.store->admit(input()).at("request_id"));
        const auto sibling = f.store->request(other), sibling_budget = f.store->budget(other);
        const FakeModel fake({step(Role::Dispatcher, "submit_amendment_plan", plan_arguments(Object{{"alpha", decision("update")}}))});
        const auto call = f.capture(fake);
        if (reopen) f.reopen(StoreRecoveryScope::request(f.id)); else f.amend();
        const auto before = f.store->tasks(f.id);
        expect({ErrorCode::Stale}, [&] { (void)f.offline().accept_independent_response(call, fake.complete(call.request)); });
        CHECK(f.store->tasks(f.id) == before);
        const auto attempt = f.store->attempt(call.request.attempt_id);
        CHECK(attempt.at("status") == "STALE" && !attempt.at("settlement_sha256").is_null());
        CHECK(attempt.at("request_envelope") == to_json(call.request));
        Store read(f.directory.path / "control.db", base_sql, store_sql, StoreOpenMode::ReadOnly,
            tools_sql, events_sql, workflow_sql, acceptance_sql);
        CHECK(read.request(other) == sibling && read.budget(other) == sibling_budget);
    }
}

void exact_response_replay_has_one_transition_and_one_charge() {
    Fixture f; f.initial(); f.amend();
    const FakeModel fake({step(Role::Dispatcher, "submit_amendment_plan", plan_arguments(Object{{"alpha", decision("update")}}))});
    const auto call = f.capture(fake); const auto response = fake.complete(call.request);
    const auto result = f.offline().accept_independent_response(call, response);
    const auto tasks = f.store->tasks(f.id), events = f.store->events(f.id);
    const auto budget = f.store->budget(f.id);
    CHECK(f.offline().accept_independent_response(call, response) == result);
    CHECK(f.store->tasks(f.id) == tasks && f.store->events(f.id) == events && f.store->budget(f.id) == budget);
    auto altered = response; altered.tool_calls.front().arguments.at("dispositions").at("alpha").as_object()["disposition"] = "cancel";
    expect({ErrorCode::Conflict}, [&] { (void)f.offline().accept_independent_response(call, altered); });
    CHECK(f.store->tasks(f.id) == tasks && f.store->budget(f.id) == budget);
}

void forbidden_model_fields_and_unknown_nodes_never_change_business_state() {
    for (int variant = 0; variant < 7; ++variant) {
        Fixture f; f.initial(); f.amend();
        auto args = plan_arguments(Object{{"alpha", decision("update")}});
        if (variant == 0) args.as_object()["request_id"] = f.id;
        if (variant == 1) args.at("plan").as_object()["plan_revision"] = 2;
        if (variant == 2) args.at("plan").as_object()["based_on_requirements_revision"] = 2;
        if (variant == 3) args.at("dispositions").at("alpha").as_object()["state_version"] = 1;
        if (variant == 4) args.as_object()["dispositions"] = Object{{"unallocated", decision("keep")}};
        if (variant == 5) args.at("dispositions").at("alpha").as_object()["rationale"] = "   \t";
        if (variant == 6) args.at("plan").at("nodes").as_array().front().as_object()["template"] = "unallocated-template";
        const auto before = f.store->tasks(f.id), old_events = f.store->events(f.id);
        const FakeModel fake({step(Role::Dispatcher, "submit_amendment_plan", args)});
        const auto call = f.capture(fake);
        expect({ErrorCode::InvalidInput, ErrorCode::Unsupported, ErrorCode::Unauthorized}, [&] {
            (void)f.offline().accept_independent_response(call, fake.complete(call.request));
        });
        CHECK(f.store->tasks(f.id) == before);
        CHECK(f.store->request(f.id).at("plan_revision") == 1);
        CHECK(!f.store->attempt(call.request.attempt_id).at("settlement_sha256").is_null());
        CHECK(!f.store->tool_feedback(call.request.attempt_id).is_null());
        const auto events = f.store->events(f.id);
        for (std::size_t i = old_events.size(); i < events.size(); ++i)
            CHECK(events[i].at("type") != "independent_plan" && events[i].at("type") != "independent_dispositions");
    }
}

void empty_decisions_leave_plan_and_tasks_unchanged_without_retry() {
    Fixture f; f.initial(); f.amend();
    auto script = model_plan_script(Object{});
    const auto before = f.store->tasks(f.id);
    const auto result = f.runtime().pump(f.id, script);
    CHECK(result.at("calls").as_array().size() == 1);
    CHECK(blocked(result, "invalid_input"));
    CHECK(f.store->tasks(f.id) == before);
    CHECK(f.store->request(f.id).at("plan_revision") == 1);
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/r2.txt"));
    (void)f.decide("submit_amendment_plan", plan_arguments(Object{{"alpha", decision("update")}}));
    const auto beta = f.task("beta");
    expect({ErrorCode::InvalidInput}, [&] { (void)f.decide("resolve_amendment", Object{{"dispositions", Object{}}}); });
    CHECK(f.task("beta") == beta && f.store->request(f.id).at("plan_revision") == 2);
}

void resolve_cannot_redecide_current_node_or_replace_the_plan() {
    Fixture f; f.initial(); f.amend();
    (void)f.decide("submit_amendment_plan", plan_arguments(Object{{"alpha", decision("update")}}));
    const auto before = f.store->tasks(f.id);
    expect({ErrorCode::InvalidInput, ErrorCode::Conflict, ErrorCode::Unauthorized}, [&] {
        (void)f.decide("resolve_amendment", Object{{"dispositions", Object{{"alpha", decision("cancel")}}}});
    });
    expect({ErrorCode::InvalidInput}, [&] {
        (void)f.decide("submit_amendment_plan", plan_arguments(Object{{"beta", decision("keep")}}));
    });
    CHECK(f.store->tasks(f.id) == before && f.store->request(f.id).at("plan_revision") == 2);
}

void worker_cannot_submit_dispatcher_amendment_decisions() {
    Fixture f; f.initial();
    const auto before = f.store->tasks(f.id);
    const FakeModel fake({step(Role::Worker, "submit_amendment_plan",
        plan_arguments(Object{{"alpha", decision("update")}}))});
    const auto call = f.offline().reserve_call(fake, f.id, Role::Worker, f.binding().task_id);
    CHECK(std::find(call.request.context.tool_names.begin(), call.request.context.tool_names.end(),
        "submit_amendment_plan") == call.request.context.tool_names.end());
    expect({ErrorCode::InvalidInput}, [&] { (void)f.offline().accept_response(call, fake.complete(call.request)); });
    CHECK(f.store->tasks(f.id) == before);
    CHECK(!f.store->attempt(call.request.attempt_id).at("settlement_sha256").is_null());
}

void model_keep_and_cancel_preserve_unknown_original_effects() {
    Fixture f; f.initial();
    const auto original = read_runtime_file(f.directory.path / "alpha/r1.txt");
    // Preserve the saved result, but make its effect uncertain as a crashed
    // executor would. AMEND is not permission to overwrite that evidence.
    f.raw("UPDATE ops SET status='UNKNOWN',state_version=state_version+1 WHERE task_id='" + f.binding().task_id + "'");
    const auto unknown = f.store->pending_recovery().at("unknown_operations");
    CHECK(!unknown.as_array().empty());
    f.amend();
    const auto result = f.runtime().pump(f.id, model_plan_script(Object{
        {"alpha", decision("keep")}, {"beta", decision("cancel")}}));
    CHECK(f.task("beta").at("status") == "CANCELLED");
    CHECK(f.task().at("based_on_requirements_revision") == 2);
    CHECK(f.store->pending_recovery().at("unknown_operations") == unknown);
    CHECK(read_runtime_file(f.directory.path / "alpha/r1.txt") == original);
    CHECK(!std::filesystem::exists(f.directory.path / "alpha/r2.txt"));
    CHECK(!std::filesystem::exists(f.directory.path / "beta/r2.txt"));
    for (const auto& call : result.at("calls").as_array()) CHECK(call.at("role") != "worker");
}

void existing_host_dispositions_remain_available() {
    Fixture f; f.initial(); f.amend();
    EventPumpOptions options; options.amendment_dispositions = fixture("s13_update_alpha.json");
    const auto first = f.runtime().pump(f.id, amended_script(), options);
    CHECK(first.at("calls").as_array().size() == 3);
    CHECK(first.at("calls").as_array().front().at("response").at("tool_calls").as_array().front().at("name") == "submit_plan");
    auto followup = amended_script(); followup.default_steps.clear(); followup.by_node["alpha"].clear();
    options.amendment_dispositions = fixture("s13_keep_beta.json");
    const auto result = f.runtime().pump(f.id, followup, options);
    CHECK(result.at("calls").as_array().size() == 2);
    CHECK(f.store->request(f.id).at("plan_revision") == 2);
    CHECK(read_runtime_file(f.directory.path / "beta/r2.txt") == "beta revision 2\n");
}
} // namespace

int main() {
    if (::geteuid() == 0) { std::cerr << "ordinary user required\n"; return 1; }
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    base_sql = read_runtime_file(source / "db/schema.sql");
    store_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    workflow_sql = read_runtime_file(source / "db/migrations/007_workflow_runtime.sql");
    acceptance_sql = read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql");
    return test::run({
        {"model AMEND captures original host snapshots and rejects retargeting", capture_uses_original_task_snapshots_and_cannot_be_retargeted},
        {"partial model AMEND and later keep share one plan in both context formats", partial_model_plan_and_later_keep_use_one_plan_and_new_ack},
        {"keep requires a new TeamLead instruction and Worker acknowledgement", keep_alone_does_not_authorize_worker_or_rewrite_old_instruction},
        {"amended plan rejects stale task CAS even with unchanged request CAS", stale_task_cas_rejects_model_plan_even_when_request_cas_is_unchanged},
        {"pending-node resolution tolerates progress of already current sibling", unresolved_target_cas_is_checked_without_fencing_current_sibling_progress},
        {"pending-node resolution rejects a stale captured task", stale_pending_target_is_not_relabelled_on_followup},
        {"later AMEND and scoped recovery fence old decisions without losing usage", later_amend_and_scoped_reopen_fence_original_decisions_and_retain_usage},
        {"exact model response replay neither transitions nor charges twice", exact_response_replay_has_one_transition_and_one_charge},
        {"forged model fields nodes and templates never change business state", forbidden_model_fields_and_unknown_nodes_never_change_business_state},
        {"empty model decisions do not publish or retry a plan", empty_decisions_leave_plan_and_tasks_unchanged_without_retry},
        {"followup cannot redecide current nodes or replace the plan", resolve_cannot_redecide_current_node_or_replace_the_plan},
        {"Worker cannot execute Dispatcher model AMEND tools", worker_cannot_submit_dispatcher_amendment_decisions},
        {"model keep and cancel do not clear UNKNOWN or fabricate rollback", model_keep_and_cancel_preserve_unknown_original_effects},
        {"existing host-selected AMEND paths remain compatible", existing_host_dispositions_remain_available}
    });
}
