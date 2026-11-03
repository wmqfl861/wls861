#include "wls/persistent_event_runtime.hpp"
#include "wls/persistent_event_selection.hpp"
#include "wls/migrations.hpp"
#include "wls/runtime_resources.hpp"
#include "test_support.hpp"
#include "persistent_event_faults.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <source_location>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
using namespace wls;
using namespace wls::s1;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path source = std::filesystem::absolute(WLS_SOURCE_DIR);

const RuntimeResources& resources() {
    static const auto value = RuntimeResources::from_source_tree(source);
    return value;
}
const std::shared_ptr<const SchemaRegistry>& schemas() {
    static const std::shared_ptr<const SchemaRegistry> value =
        std::make_shared<SchemaRegistry>(resources().schema_documents());
    return value;
}
std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t number(const Json& value, std::string_view key) { return value.at(key).as_int64(); }
Json fixture(const char* name) { return parse_json_strict(read_runtime_file(source / "tests/fixtures" / name)); }

template<class F>
void expect(ErrorCode code, F action) {
    bool caught = false;
    try { action(); } catch (const Error& error) {
        if (error.code() != code) throw std::runtime_error(std::string(error_name(error.code())) + ": " + error.what());
        caught = true;
    }
    CHECK(caught);
}

std::int64_t sql_number(const std::filesystem::path& path, const char* statement) {
    sqlite3* database{};
    CHECK(sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* query{};
    CHECK(sqlite3_prepare_v2(database, statement, -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW);
    const auto result = sqlite3_column_int64(query, 0);
    sqlite3_finalize(query); sqlite3_close(database);
    return result;
}
Json original_instruction_rows(const std::filesystem::path& path) {
    sqlite3* database{};
    CHECK(sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* query{};
    CHECK(sqlite3_prepare_v2(database, "SELECT * FROM instructions ORDER BY task_id,instruction_revision",
        -1, &query, nullptr) == SQLITE_OK);
    Array rows;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(query)) == SQLITE_ROW) {
        Array row;
        for (int column = 0; column < sqlite3_column_count(query); ++column) {
            const auto type = sqlite3_column_type(query, column);
            const auto* bytes = sqlite3_column_text(query, column);
            row.push_back(Object{{"type", type}, {"bytes", bytes ?
                std::string(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(sqlite3_column_bytes(query, column))) : std::string{}}});
        }
        rows.push_back(std::move(row));
    }
    CHECK(status == SQLITE_DONE);
    sqlite3_finalize(query); sqlite3_close(database);
    return rows;
}
void sql(const std::filesystem::path& path, const char* statement) {
    sqlite3* database{};
    CHECK(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    const auto result = sqlite3_exec(database, statement, nullptr, nullptr, nullptr);
    sqlite3_close(database);
    CHECK(result == SQLITE_OK);
}

EventFakeScript script(std::int64_t worker_calls = 1) {
    auto result = event_fake_script_from_json(fixture("s13_fake_initial.json"));
    const auto note = fake_model_steps_from_json(fixture("s1_fake_initial.json")).back();
    for (auto& [node, steps] : result.by_node) {
        steps.resize(1); // Keep the original TeamLead work-order fixture.
        for (std::int64_t index = 1; index <= worker_calls; ++index) {
            auto next = note;
            next.role_call_index = index;
            next.tool_calls.front().arguments = Object{{"text", "Original offline " + node + " note " + std::to_string(index)}};
            steps.push_back(std::move(next));
        }
    }
    return result;
}
EventPumpOptions limits(std::int64_t calls, std::int64_t worker_calls = 1) {
    EventPumpOptions result;
    result.max_calls = calls;
    result.max_worker_calls_per_task = worker_calls;
    return result;
}

struct Fixture {
    test::TempDir directory;
    std::filesystem::path path{directory.path / "control.sqlite"};
    FrozenRequestConfig config{load_config_catalog(source / "config/examples/system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general")};
    std::unique_ptr<Store> open(StoreRecoveryScope scope = {}) const {
        return std::make_unique<Store>(path, MigrationRegistry(resources(), 411), StoreOpenMode::Takeover, std::move(scope));
    }
    std::string admit(Store& store, bool enable = true) const {
        AdmissionInput input;
        input.tenant_id = "event-tenant"; input.project_id = "event-project"; input.root_id = "event-root";
        input.function_id = config.function_id(); input.idempotency_key = new_id("key");
        input.original_text = read_runtime_file(source / "tests/fixtures/s13_request.txt");
        input.constraint_catalog = fixture("s13_constraints.json"); input.config_snapshot = config.json();
        const auto grants = config.worker_tools(); input.tool_allowlist.assign(grants.begin(), grants.end());
        input.budget.input_tokens = 10000000; input.budget.output_tokens = 1000000;
        const auto id = text(store.admit(input), "request_id");
        if (enable) (void)store.enable_event_driver(store.host_actor(Role::Root, id), {}, new_id("enable"));
        return id;
    }
};

struct Gate {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::atomic<int> entered{};
    bool released{};
    // Test-only seam for the false-predicate / blocking-wait boundary. Normal
    // model gates never set it; no production interface exposes this callback.
    std::function<void()> before_wait;
    void release() { { std::lock_guard lock(mutex); released = true; } changed.notify_all(); }
    void wait(std::stop_token stop) {
        std::unique_lock lock(mutex);
        ++entered;
        changed.notify_all();
        // The stop-aware overload closes the gap between reading a false
        // predicate and entering the wait. An unrelated notify_all callback
        // can lose a stop in precisely that gap.
        (void)changed.wait(lock, stop, [&] {
            const bool ready = released;
            if (!ready && before_wait) {
                auto hook = std::move(before_wait);
                hook();
            }
            return ready;
        });
    }
};
struct Release {
    std::shared_ptr<Gate> gate;
    ~Release() { gate->release(); }
};
class GatedModel final : public ModelAdapter {
public:
    GatedModel(const CapturedModelCall& call, std::vector<FakeModelStep> steps, std::shared_ptr<Gate> gate = {})
        : fake_(std::move(steps), call.request.model.model), gate_(std::move(gate)) {}
    AdapterCapabilities capabilities() const override { return fake_.capabilities(); }
    void validate(const ModelRequest& request) const override { fake_.validate(request); }
    ModelResponse complete(ModelRequest request, std::stop_token stop) const override {
        CHECK(std::this_thread::get_id() != owner_);
        ++calls;
        if (gate_) gate_->wait(stop);
        // A late original response is deliberately retained after a stop.
        auto response = fake_.complete(std::move(request));
        { std::lock_guard lock(mutex_); original_ = to_json(response); }
        return response;
    }
    Json original() const { std::lock_guard lock(mutex_); return original_; }
    mutable std::atomic<int> calls{};
private:
    FakeModel fake_;
    std::shared_ptr<Gate> gate_;
    const std::thread::id owner_{std::this_thread::get_id()};
    mutable std::mutex mutex_;
    mutable Json original_ = nullptr;
};

template<class Runtime, class Predicate>
void until(Runtime& runtime, Predicate predicate,
           std::source_location where = std::source_location::current()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        (void)runtime.poll();
        const auto checked = std::chrono::steady_clock::now();
        if (checked >= deadline)
            std::cerr << "UNTIL-DEADLINE " << where.file_name() << ':' << where.line()
                      << " in " << where.function_name() << " exceeded its original 5s wait" << std::endl;
        CHECK(checked < deadline);
        std::this_thread::yield();
    }
}
bool ready(std::future<Json>& future) { return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
Json task(Store& store, const std::string& id, std::string_view node) {
    for (const auto& value : store.tasks(id)) if (text(value, "node_id") == node) return value;
    throw std::runtime_error("expected original task is missing");
}
bool contains(const Json& ids, const Json& id) {
    return std::find(ids.as_array().begin(), ids.as_array().end(), id) != ids.as_array().end();
}

void original_target_shape_is_checked_before_empty_subject() {
    const Json event = Object{{"type", "amend"}, {"subject_task_id", ""}};
    const Json empty = Object{{"items", Array{}}};
    test::throws([&] { (void)event_claim_contains_workflow_action(Object{{"kind", "amendment"}}, empty); });
    test::throws([&] { (void)event_claim_contains_workflow_action(Object{{"kind", "question"}, {"target", Object{}}}, empty); });
    test::throws([&] { (void)event_matches_workflow_action(Object{{"kind", "amendment"}}, event); });
    test::throws([&] { (void)event_matches_workflow_action(Object{{"kind", "amendment"}, {"target", nullptr}}, event); });
    test::throws([&] { (void)event_matches_workflow_action(Object{{"kind", "amendment"}, {"target", Object{}}}, event); });
    test::throws([&] { (void)event_matches_workflow_action(Object{{"kind", "amendment"},
        {"target", Object{{"pending_node_ids", "bad"}}}}, event); });
    const Json action = Object{{"kind", "amendment"}, {"target", Object{
        {"pending_node_ids", Array{"alpha"}}, {"tasks", Array{
            Object{{"task_id", "a"}, {"node_id", "alpha"}}, Object{{"task_id", "b"}, {"node_id", "beta"}}}}}}};
    CHECK(event_matches_workflow_action(action, event));
    CHECK(!event_claim_contains_workflow_action(action, empty));
    CHECK(event_matches_workflow_action(action, Object{{"type", "recover"}, {"subject_task_id", "a"}}));
    CHECK(!event_matches_workflow_action(action, Object{{"type", "recover"}, {"subject_task_id", "b"}}));
}

void selector_uses_actual_capture_and_preserves_other_ids() {
    CapturedModelCall call;
    call.request.role = Role::Worker;
    call.request.binding.task_id = "task"; call.request.binding.requirements_revision = 2;
    // Pre-claim context may name an earlier page of held IDs. Selection reads
    // only the Store-returned C; it never scans this unrelated context text.
    call.request.context.dynamic_suffix = "old page: inbox IDs 1..64 are held";
    const auto item = [&](std::int64_t id, std::string type, std::int64_t revision, Json binding) {
        return Object{{"inbox_id", id}, {"type", type}, {"subject_task_id", "task"},
            {"event_requirements_revision", revision}, {"result", Object{{"binding", binding}}}, {"payload", Object{}}};
    };
    auto old = call.request.binding; old.execution_epoch = 9;
    Json claim = Object{{"items", Array{
        item(65, "ack", 2, to_json(old)), item(66, "ack", 2, to_json(call.request.binding)),
        item(67, "answer_question", 2, to_json(call.request.binding)),
        item(68, "instruction", 1, to_json(call.request.binding))}}};
    CHECK(select_persistent_event_ids(call, claim) == std::vector<std::int64_t>{66});
    // A later ID is absent from original C even if a subsequent call sees it.
    auto later = claim;
    later.at("items").as_array().push_back(item(69, "tool_feedback", 2, to_json(call.request.binding)));
    CHECK(select_persistent_event_ids(call, claim) == std::vector<std::int64_t>{66});
    CHECK(select_persistent_event_ids(call, later) == (std::vector<std::int64_t>{66, 69}));
    auto duplicate = claim; duplicate.at("items").as_array().back() = claim.at("items").as_array().front();
    expect(ErrorCode::Corrupt, [&] { (void)select_persistent_event_ids(call, duplicate); });
}

void missing_scripts_and_legacy_entry_have_no_claim_effect() {
    Fixture f; PersistentEventRuntime runtime(f.open(), schemas());
    const auto id = f.admit(runtime.store());
    const auto original = runtime.store().request_snapshot(id);
    const auto budget = runtime.store().budget(id);
    runtime.add_request(id, {});
    (void)runtime.poll(); (void)runtime.poll();
    CHECK(runtime.report(id).at("calls").as_array().empty());
    CHECK(sql_number(f.path, "SELECT count(*) FROM n5_event_claims") == 0);
    CHECK(sql_number(f.path, "SELECT count(*) FROM n5_event_links") == 0);
    CHECK(runtime.store().budget(id) == budget && runtime.store().request_snapshot(id) == original);
    const auto actor = runtime.store().host_actor(Role::Dispatcher, id);
    expect(ErrorCode::Unsupported, [&] { (void)runtime.store().claim_events(actor); });
    const auto legacy = EventRuntime(runtime.store(), *schemas()).pump(id, script());
    CHECK(legacy.at("calls").as_array().empty());
    CHECK(std::any_of(legacy.at("blocked").as_array().begin(), legacy.at("blocked").as_array().end(),
        [](const auto& item) { return item.at("code") == "unsupported"; }));
    CHECK(runtime.store().budget(id) == budget);
    runtime.close();
}

void two_requests_two_nodes_advance_without_callback_wave_barrier() {
    Fixture f; PersistentEventRuntime runtime(f.open(), schemas());
    const auto first = f.admit(runtime.store()), second = f.admit(runtime.store());
    const auto original_first = runtime.store().request_snapshot(first);
    const auto original_second = runtime.store().request_snapshot(second);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    bool assigned_gate = false;
    std::vector<std::shared_ptr<GatedModel>> models;
    const PersistentEventAdapterFactory factory = [&](const auto& call, const auto& steps) {
        std::shared_ptr<Gate> selected;
        if (call.request.binding.request_id == first && call.request.role == Role::TeamLead && !assigned_gate) {
            selected = gate; assigned_gate = true;
        }
        auto model = std::make_shared<GatedModel>(call, steps, selected); models.push_back(model); return model;
    };
    runtime.add_request(first, script(), limits(5), factory);
    runtime.add_request(second, script(), limits(5), factory);
    until(runtime, [&] { return gate->entered == 1 && sql_number(f.path, "SELECT count(*) FROM ops WHERE status='SUCCEEDED'") == 3; });
    CHECK(runtime.snapshot().outstanding == 1);
    CHECK(number(task(runtime.store(), first, "alpha"), "instruction_revision") == 0);
    CHECK(number(task(runtime.store(), first, "beta"), "applied_instruction_revision") == 1);
    CHECK(number(task(runtime.store(), second, "alpha"), "applied_instruction_revision") == 1);
    CHECK(number(task(runtime.store(), second, "beta"), "applied_instruction_revision") == 1);
    gate->release();
    until(runtime, [&] { return sql_number(f.path, "SELECT count(*) FROM ops WHERE status='SUCCEEDED'") == 4 && runtime.snapshot().outstanding == 0; });
    CHECK(models.size() == 10);
    for (const auto& model : models) CHECK(model->calls == 1 && !model->original().is_null());
    for (const auto& id : {first, second}) {
        CHECK(runtime.store().tasks(id).size() == 2);
        const auto links = runtime.store().request_event_links(id);
        CHECK(links.size() == 5);
        for (const auto& link : links) {
            CHECK(link.at("state") == "CLOSED" && !link.at("completed_ids").as_array().empty());
            for (const auto& completed : link.at("completed_ids").as_array()) {
                CHECK(contains(link.at("captured_ids"), completed));
                CHECK(contains(link.at("selected_ids"), completed));
            }
            const auto attempt = runtime.store().attempt(text(link, "attempt_id"));
            CHECK(!attempt.at("settlement_sha256").is_null());
            CHECK(runtime.store().tool_feedback(text(link, "attempt_id")).at("original_binding") == link.at("binding"));
        }
        CHECK(runtime.report(id).at("real_model_calls") == 0);
        CHECK(runtime.report(id).at("business_delivered") == false);
    }
    CHECK(runtime.store().request_snapshot(first) == original_first);
    CHECK(runtime.store().request_snapshot(second) == original_second);
    CHECK(sql_number(f.path, "SELECT count(*) FROM ops WHERE tool_id<>'note'") == 0);
    runtime.close();
}

void feedback_created_after_capture_belongs_only_to_later_call() {
    Fixture f; PersistentEventRuntime runtime(f.open(), schemas());
    const auto id = f.admit(runtime.store());
    auto selected_script = script(2); selected_script.by_node["beta"].clear();
    auto gate = std::make_shared<Gate>(); Release release{gate};
    runtime.add_request(id, std::move(selected_script), limits(4, 2),
        [&](const auto& call, const auto& steps) {
            return std::make_shared<GatedModel>(call, steps,
                call.request.role == Role::Worker && call.request.role_call_index == 2 ? gate : nullptr);
        });
    until(runtime, [&] { return gate->entered == 1; });
    auto links = runtime.store().request_event_links(id);
    CHECK(links.size() == 4 && links[2].at("state") == "CLOSED" && links[3].at("state") == "DISPATCHED");
    const auto original_first = links[2];
    CHECK(!links[3].at("selected_ids").as_array().empty());
    for (const auto& later : links[3].at("selected_ids").as_array()) {
        CHECK(!contains(links[2].at("captured_ids"), later));
        CHECK(!contains(links[2].at("completed_ids"), later));
    }
    CHECK(sql_number(f.path, "SELECT count(*) FROM ops WHERE status='SUCCEEDED'") == 1);
    gate->release();
    until(runtime, [&] { return runtime.snapshot().outstanding == 0; });
    links = runtime.store().request_event_links(id);
    CHECK(links[2].at("dispositions") == original_first.at("dispositions"));
    CHECK(links[3].at("state") == "CLOSED");
    CHECK(sql_number(f.path, "SELECT count(*) FROM ops WHERE status='SUCCEEDED'") == 2);
    const auto worker = runtime.store().host_actor(Role::Worker, id, text(task(runtime.store(), id, "alpha"), "team_id"));
    const auto pending = runtime.store().inbox(worker);
    CHECK(pending.size() == 1 && pending.front().at("type") == "tool_feedback");
    CHECK(!contains(links[3].at("captured_ids"), pending.front().at("inbox_id")));
    runtime.close();
}

void partial_model_amendment_keeps_omitted_id_until_its_own_proof() {
    Fixture f; PersistentEventRuntime runtime(f.open(), schemas());
    const auto id = f.admit(runtime.store());
    auto selected_script = script();
    selected_script.by_node["alpha"].clear(); selected_script.by_node["beta"].clear();
    auto amended = event_fake_script_from_json(fixture("s13_fake_amended.json")).default_steps.front();
    const Json decisions = Object{{"alpha", Object{{"disposition", "update"}, {"rationale", "Current alpha requirements considered."}}}};
    auto& tool = amended.tool_calls.front();
    tool.name = "submit_amendment_plan";
    tool.arguments = Object{{"plan", tool.arguments}, {"dispositions", decisions}};
    amended.role_call_index = 2;
    selected_script.default_steps.push_back(amended);
    amended.role_call_index = 3; amended.tool_calls.front().name = "resolve_amendment";
    amended.tool_calls.front().arguments = Object{{"dispositions", Object{{"beta", Object{
        {"disposition", "keep"}, {"rationale", "Current beta requirements considered separately."}}}}}};
    selected_script.default_steps.push_back(amended);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    runtime.add_request(id, std::move(selected_script), limits(3), [&](const auto& call, const auto& steps) {
        return std::make_shared<GatedModel>(call, steps, call.request.role_call_index == 3 ? gate : nullptr);
    });
    until(runtime, [&] { return runtime.store().tasks(id).size() == 2 && runtime.snapshot().outstanding == 0; });
    const auto beta_before = task(runtime.store(), id, "beta");
    auto changed = runtime.amend(runtime.store().host_actor(Role::Root, id), number(runtime.store().request(id), "state_version"),
        read_runtime_file(source / "tests/fixtures/s13_amendment.txt"), fixture("s13_amended_constraints.json"), new_id("amend"));
    (void)runtime.poll(); (void)changed.get();
    until(runtime, [&] { return gate->entered == 1; });
    auto links = runtime.store().request_event_links(id);
    CHECK(links.size() == 3 && links[1].at("state") == "CLOSED");
    CHECK(links[1].at("selected_ids").as_array().size() == 2);
    CHECK(links[1].at("completed_ids").as_array().size() == 1);
    CHECK(links[2].at("selected_ids").as_array().size() == 1);
    const auto omitted_id = links[2].at("selected_ids").as_array().front();
    CHECK(contains(links[1].at("captured_ids"), omitted_id) && !contains(links[1].at("completed_ids"), omitted_id));
    CHECK(std::any_of(links[1].at("dispositions").as_array().begin(), links[1].at("dispositions").as_array().end(),
        [&](const auto& proof) { return proof.at("inbox_id") == omitted_id && proof.at("disposition") == "DEFERRED_NO_EFFECT"; }));
    CHECK(task(runtime.store(), id, "beta").at("based_on_requirements_revision") == beta_before.at("based_on_requirements_revision"));
    const auto alpha_current = task(runtime.store(), id, "alpha");
    gate->release(); until(runtime, [&] { return runtime.snapshot().outstanding == 0; });
    links = runtime.store().request_event_links(id);
    CHECK(links[2].at("state") == "CLOSED" && contains(links[2].at("completed_ids"), omitted_id));
    CHECK(runtime.store().request(id).at("plan_revision") == 2);
    CHECK(task(runtime.store(), id, "alpha") == alpha_current);
    CHECK(task(runtime.store(), id, "beta").at("based_on_requirements_revision") == 2);
    runtime.close();
}

void unknown_observation_fences_the_batch_and_never_proves_ids() {
    Fixture f; PersistentEventRuntime runtime(f.open(), schemas());
    const auto id = f.admit(runtime.store());
    auto unknown = script(); unknown.default_steps.front().usage.input_tokens.reset();
    runtime.add_request(id, std::move(unknown), limits(5));
    until(runtime, [&] { return runtime.snapshot().settlement_halted; });
    CHECK(runtime.snapshot().retained_results == 1 && !runtime.snapshot().accepting);
    auto link = runtime.store().request_event_links(id).front();
    CHECK(link.at("state") == "HOLD_UNKNOWN" && link.at("completed_ids").as_array().empty());
    const auto attempt = runtime.store().attempt(text(link, "attempt_id"));
    CHECK(attempt.at("unknown_input") == 1 && !attempt.at("settlement_sha256").is_null());
    CHECK(runtime.store().tasks(id).empty() && runtime.store().plan(id).is_null());
    expect(ErrorCode::NotReady, [&] { runtime.add_request(id, script()); });
    const auto observations = runtime.retained_results(); CHECK(observations.as_array().size() == 1);
    CHECK(observations.as_array().front().at("response").at("usage").at("input_tokens").is_null());
    runtime.flush_retained_evidence();
    CHECK(!runtime.snapshot().accepting && runtime.snapshot().outstanding == 0);
    CHECK(runtime.store().attempt(text(link, "attempt_id")) == attempt);
    CHECK(runtime.store().request_event_links(id).front().at("state") == "HOLD_UNKNOWN");
    CHECK(runtime.store().tasks(id).empty());
    runtime.close();
}

void cancel_keeps_late_original_observation_without_business_or_ack() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas());
    const auto id = f.admit(runtime.store());
    auto gate = std::make_shared<Gate>(); Release release{gate};
    const auto captured = runtime.capture(id, Role::Dispatcher);
    auto model = std::make_shared<GatedModel>(captured, script().default_steps, gate);
    auto submitted = runtime.submit_event(captured, model);
    until(runtime, [&] { return gate->entered == 1; });
    auto cancelled = runtime.cancel(runtime.store().host_actor(Role::Root, id),
        number(runtime.store().request(id), "state_version"), new_id("cancel"));
    (void)runtime.poll(); CHECK(cancelled.get().at("status") == "CANCELLED");
    gate->release(); until(runtime, [&] { return ready(submitted.completion); });
    test::throws<Error>([&] { (void)submitted.completion.get(); });
    const auto link = runtime.store().event_link_status(submitted.link_id);
    CHECK(link.at("state") == "HOLD_ERROR" && link.at("completed_ids").as_array().empty());
    const auto attempt = runtime.store().attempt(captured.request.attempt_id);
    CHECK(attempt.at("response") == model->original());
    CHECK(attempt.at("request_envelope") == to_json(captured.request));
    CHECK(runtime.store().tool_feedback(captured.request.attempt_id).at("result").at("runtime_error") == true);
    CHECK(runtime.store().tasks(id).empty());
    CHECK(sql_number(f.path, "SELECT count(*) FROM event_inbox WHERE status='DONE'") == 0);
    CHECK(model->calls == 1);
    runtime.close();
}

void reserved_and_dispatched_restarts_keep_original_actor_hold() {
    for (const bool dispatched : {false, true}) {
        Fixture f;
        std::string id, link_id;
        CapturedModelCall captured;
        Json original_envelope;
        {
            auto store = f.open(); id = f.admit(*store);
            captured = OfflineRuntime(*store, *schemas()).capture_call(id, Role::Dispatcher);
            const FakeModel model(script().default_steps, captured.request.model.model);
            const auto claim = store->claim_event_batch(captured.actor);
            EventCallAssociation association;
            association.claim_id = text(claim, "claim_id"); association.generation = number(claim, "generation");
            association.selected_ids = select_persistent_event_ids(captured, claim);
            association.expected_state_version = captured.expected_state_version;
            association.workflow_action = captured.workflow_action;
            const auto link = store->reserve_event_call(captured.actor, captured.request.binding,
                make_offline_reservation(model, captured), association);
            link_id = text(link, "link_id");
            if (dispatched) CHECK(store->claim_event_dispatch(captured.actor, link_id).at("dispatch_claimed") == true);
            original_envelope = store->attempt(captured.request.attempt_id).at("request_envelope");
        }
        PersistentEventRuntime restarted(f.open(StoreRecoveryScope::request(id)), schemas());
        const auto claims = sql_number(f.path, "SELECT count(*) FROM n5_event_claims");
        const auto original = restarted.store().event_link_status(link_id);
        CHECK(original.at("state") == "HOLD_UNKNOWN" && original.at("completed_ids").as_array().empty());
        CHECK(restarted.store().attempt(captured.request.attempt_id).at("request_envelope") == original_envelope);
        expect(ErrorCode::Stale, [&] { (void)restarted.store().claim_event_dispatch(captured.actor, link_id); });
        restarted.add_request(id, script());
        (void)restarted.poll(); (void)restarted.poll();
        CHECK(restarted.report(id).at("calls").as_array().empty());
        CHECK(sql_number(f.path, "SELECT count(*) FROM n5_event_claims") == claims);
        CHECK(restarted.store().tasks(id).empty());
        CHECK(restarted.store().event_link_status(link_id).at("binding") == original.at("binding"));
        CHECK(restarted.store().request_event_links(id).size() == 1);
        restarted.close();
    }
}

void post_t1_storage_failure_retains_slot_and_flush_never_reapplies_t4() {
    ::test::persistent_events::StorageFault fault;
    Fixture f; AsyncCoordinator runtime(f.open(), schemas());
    const auto id = f.admit(runtime.store());
    const auto captured = runtime.capture(id, Role::Dispatcher);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    auto model = std::make_shared<GatedModel>(captured, script().default_steps, gate);
    auto submitted = runtime.submit_event(captured, model);
    CHECK(runtime.store().event_link_status(submitted.link_id).at("state") == "RESERVED");
    sql(f.path, "CREATE TRIGGER n5_test_close_failure BEFORE INSERT ON n5_event_receipts WHEN NEW.kind='CLOSE' BEGIN SELECT n5_test_storage_fault(); END;");
    until(runtime, [&] { return gate->entered == 1; });
    gate->release(); until(runtime, [&] { return runtime.snapshot().settlement_halted; });
    CHECK(runtime.snapshot().outstanding == 1 && runtime.snapshot().retained_results == 1);
    CHECK(!ready(submitted.completion));
    const auto attempt = runtime.store().attempt(captured.request.attempt_id);
    CHECK(attempt.at("response") == model->original() && !attempt.at("settlement_sha256").is_null());
    CHECK(runtime.store().tasks(id).empty() && runtime.store().tool_feedback(captured.request.attempt_id).is_null());
    CHECK(runtime.store().event_link_status(submitted.link_id).at("completed_ids").as_array().empty());
    CHECK(sql_number(f.path, "SELECT count(*) FROM n5_event_open_items") > 0);
    sql(f.path, "DROP TRIGGER n5_test_close_failure;");
    runtime.flush_retained_evidence();
    CHECK(ready(submitted.completion));
    test::throws<Error>([&] { (void)submitted.completion.get(); });
    CHECK(runtime.snapshot().outstanding == 0 && !runtime.snapshot().accepting);
    CHECK(runtime.store().attempt(captured.request.attempt_id) == attempt);
    CHECK(runtime.store().tasks(id).empty() && runtime.store().tool_feedback(captured.request.attempt_id).is_null());
    CHECK(runtime.store().event_link_status(submitted.link_id).at("state") == "SETTLED");
    CHECK(model->calls == 1 && sql_number(f.path, "SELECT count(*) FROM n5_event_links") == 1);
    runtime.close();
}

void close_joins_callback_and_stores_only_original_observation() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas());
    const auto id = f.admit(runtime.store());
    const auto captured = runtime.capture(id, Role::Dispatcher);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    auto model = std::make_shared<GatedModel>(captured, script().default_steps, gate);
    auto submitted = runtime.submit_event(captured, model);
    until(runtime, [&] { return gate->entered == 1; });
    runtime.close(); // stop wakes the model gate; close joins it before T3.
    CHECK(ready(submitted.completion));
    expect(ErrorCode::NotReady, [&] { (void)submitted.completion.get(); });
    const auto original = runtime.store().attempt(captured.request.attempt_id);
    CHECK(original.at("response") == model->original());
    CHECK(runtime.store().event_link_status(submitted.link_id).at("state") == "SETTLED");
    CHECK(runtime.store().event_link_status(submitted.link_id).at("completed_ids").as_array().empty());
    CHECK(runtime.store().tool_feedback(captured.request.attempt_id).is_null());
    CHECK(runtime.store().tasks(id).empty());
    CHECK(runtime.snapshot().outstanding == 0 && !runtime.snapshot().accepting && model->calls == 1);
    runtime.close();
    CHECK(runtime.store().attempt(captured.request.attempt_id) == original);
    CHECK(model->calls == 1);
}

void stop_between_false_predicate_and_wait_cannot_lose_its_wakeup() {
    Gate gate;
    std::stop_source stop;
    gate.before_wait = [&] { (void)stop.request_stop(); };
    std::promise<void> completed;
    auto completion = completed.get_future();
    std::jthread callback([&] {
        try { gate.wait(stop.get_token()); completed.set_value(); }
        catch (...) { completed.set_exception(std::current_exception()); }
    });
    // The hook requests a real stop after the predicate captured false and
    // before blocking. Measure completion before release, then always reap
    // the callback so the old implementation produces an assertion failure
    // instead of hanging this process until its CTest deadline.
    const bool completed_from_stop = completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    gate.release();
    callback.join();
    completion.get();
    CHECK(stop.stop_requested() && gate.entered == 1);
    CHECK(completed_from_stop);
}

void destructor_joins_callback_before_releasing_the_store_writer_lease() {
    class LeaseCheckedModel final : public ModelAdapter {
    public:
        LeaseCheckedModel(const CapturedModelCall& call, std::shared_ptr<Gate> gate, std::filesystem::path path)
            : fake_(script().default_steps, call.request.model.model), gate_(std::move(gate)), path_(std::move(path)) {}
        AdapterCapabilities capabilities() const override { return fake_.capabilities(); }
        void validate(const ModelRequest& request) const override { fake_.validate(request); }
        ModelResponse complete(ModelRequest request, std::stop_token stop) const override {
            ++calls; gate_->wait(stop);
            CHECK(stop.stop_requested());
            // Check the actual exclusive writer lease while this callback is
            // still live. Do not invoke Store or write SQLite from the model.
            const auto lock_path = path_.string() + ".writer-lock";
            const int descriptor = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
            CHECK(descriptor >= 0);
            const auto locked = ::flock(descriptor, LOCK_EX | LOCK_NB);
            const auto saved_errno = errno;
            CHECK(::close(descriptor) == 0);
            CHECK(locked == -1 && (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN));
            lease_held = true;
            auto response = fake_.complete(std::move(request));
            { std::lock_guard lock(mutex_); original_ = to_json(response); }
            completed = true; return response;
        }
        Json original() const { std::lock_guard lock(mutex_); return original_; }
        mutable std::atomic<int> calls{};
        mutable std::atomic<bool> lease_held{}, completed{};
    private:
        FakeModel fake_;
        std::shared_ptr<Gate> gate_;
        std::filesystem::path path_;
        mutable std::mutex mutex_;
        mutable Json original_ = nullptr;
    };
    Fixture f;
    auto runtime = std::make_unique<AsyncCoordinator>(f.open(), schemas());
    const auto id = f.admit(runtime->store());
    const auto captured = runtime->capture(id, Role::Dispatcher);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    auto model = std::make_shared<LeaseCheckedModel>(captured, gate, f.path);
    auto submission = runtime->submit_event(captured, model);
    until(*runtime, [&] { return gate->entered == 1; });
    runtime.reset(); // Destructor must stop/join, settle original evidence, then release Store.
    CHECK(model->calls == 1 && model->lease_held && model->completed);
    CHECK(ready(submission.completion));
    expect(ErrorCode::NotReady, [&] { (void)submission.completion.get(); });
    auto reopened = f.open(StoreRecoveryScope::request(id));
    const auto original = reopened->attempt(captured.request.attempt_id);
    CHECK(original.at("response") == model->original());
    CHECK(original.at("request_envelope") == to_json(captured.request));
    CHECK(!original.at("settlement_sha256").is_null());
    CHECK(reopened->event_link_status(submission.link_id).at("state") == "SETTLED");
    CHECK(reopened->event_link_status(submission.link_id).at("completed_ids").as_array().empty());
    CHECK(reopened->tool_feedback(captured.request.attempt_id).is_null() && reopened->tasks(id).empty());
    expect(ErrorCode::Stale, [&] { (void)reopened->claim_event_dispatch(captured.actor, submission.link_id); });
    reopened->settle_event_observation(submission.link_id, model_response_from_json(model->original()));
    CHECK(reopened->attempt(captured.request.attempt_id) == original && model->calls == 1);
}

void real_adapter_flag_and_excess_limits_fail_before_t0() {
    class RealFlag final : public ModelAdapter {
    public:
        AdapterCapabilities capabilities() const override { return {"synthetic-refusal", "1", true}; }
        void validate(const ModelRequest&) const override { throw std::runtime_error("must refuse before adapter validation"); }
        ModelResponse complete(ModelRequest, std::stop_token) const override {
            throw std::runtime_error("forbidden adapter must never execute");
        }
    };
    Fixture f; AsyncCoordinator runtime(f.open(), schemas());
    const auto id = f.admit(runtime.store());
    const auto captured = runtime.capture(id, Role::Dispatcher);
    const auto budget = runtime.store().budget(id);
    expect(ErrorCode::Unsupported, [&] { (void)runtime.submit_event(captured, std::make_shared<RealFlag>()); });
    CHECK(runtime.store().budget(id) == budget && runtime.snapshot().outstanding == 0);
    CHECK(sql_number(f.path, "SELECT count(*) FROM n5_event_claims") == 0);
    CHECK(sql_number(f.path, "SELECT count(*) FROM n5_event_links") == 0);
    runtime.close();
    Fixture other;
    AsyncCoordinatorLimits bounds; bounds.business_capacity = 65;
    expect(ErrorCode::InvalidInput, [&] { PersistentEventRuntime refused(other.open(), schemas(), bounds); });
    CHECK(sql_number(other.path, "SELECT count(*) FROM n5_event_claims") == 0);
}

void empty_selection_releases_only_unassociated_original_claim() {
    Fixture f;
    std::string id, task_id;
    Json before_instruction, original_rows;
    {
        PersistentEventRuntime initial(f.open(), schemas());
        id = f.admit(initial.store());
        auto selected_script = script(); selected_script.by_node["beta"].clear();
        initial.add_request(id, std::move(selected_script), limits(3));
        until(initial, [&] { return sql_number(f.path, "SELECT count(*) FROM ops WHERE status='SUCCEEDED'") == 1 && initial.snapshot().outstanding == 0; });
        task_id = text(task(initial.store(), id, "alpha"), "task_id");
        before_instruction = initial.store().instruction(task_id);
        initial.close();
        original_rows = original_instruction_rows(f.path);
    }
    AsyncCoordinator runtime(f.open(StoreRecoveryScope::request(id)), schemas());
    (void)runtime.store().recover_request(id, number(runtime.store().request(id), "state_version"));
    const auto projected = runtime.store().instruction(task_id);
    CHECK(original_instruction_rows(f.path) == original_rows);
    for (const auto* field : {"body", "team_id", "task_revision", "plan_revision", "instruction_revision", "based_on_requirements_revision"})
        CHECK(projected.at(field) == before_instruction.at(field));
    CHECK(number(projected, "dispatcher_epoch") == number(before_instruction, "dispatcher_epoch") + 1);
    CHECK(number(projected, "execution_epoch") == number(before_instruction, "execution_epoch") + 1);
    CHECK(number(projected, "dispatcher_epoch") == runtime.store().binding(task_id).dispatcher_epoch);
    CHECK(number(projected, "execution_epoch") == runtime.store().binding(task_id).execution_epoch);
    // The remaining Worker feedback names the old epoch. It is in C, but
    // cannot enter S for this newly captured binding. submit_event must use
    // typed empty release and leave the original ID pending without an attempt.
    const auto captured = runtime.capture(id, Role::Worker, task_id);
    const auto actor = captured.actor;
    const auto pending = runtime.store().inbox(actor);
    CHECK(pending.size() == 1 && pending.front().at("type") == "tool_feedback");
    const auto budget = runtime.store().budget(id);
    const auto claims = sql_number(f.path, "SELECT count(*) FROM n5_event_claims");
    const auto links = sql_number(f.path, "SELECT count(*) FROM n5_event_links");
    auto model = std::make_shared<GatedModel>(captured, script().by_node.at("alpha"));
    expect(ErrorCode::NotReady, [&] { (void)runtime.submit_event(captured, model); });
    CHECK(runtime.store().budget(id) == budget);
    CHECK(sql_number(f.path, "SELECT count(*) FROM n5_event_claims") == claims + 1);
    CHECK(sql_number(f.path, "SELECT count(*) FROM n5_event_links") == links);
    CHECK(runtime.store().inbox(actor).front().at("inbox_id") == pending.front().at("inbox_id"));
    CHECK(runtime.store().inbox(actor).front().at("status") == "PENDING");
    CHECK(runtime.snapshot().outstanding == 0 && runtime.snapshot().accepting && model->calls == 0);
    CHECK(runtime.store().instruction(task_id) == projected);
    CHECK(original_instruction_rows(f.path) == original_rows);
    runtime.close();
}

} // namespace

int main() { return test::run({
    {"original target shape is validated before empty-subject selection", original_target_shape_is_checked_before_empty_subject},
    {"actual captured IDs exclude held pages unrelated targets and later arrivals", selector_uses_actual_capture_and_preserves_other_ids},
    {"missing scripts and legacy entries cannot create claims or reservations", missing_scripts_and_legacy_entry_have_no_claim_effect},
    {"two requests and two independent nodes advance while one callback waits", two_requests_two_nodes_advance_without_callback_wave_barrier},
    {"feedback arriving after capture requires its own later call proof", feedback_created_after_capture_belongs_only_to_later_call},
    {"partial model AMEND keeps each omitted ID until its independent proof", partial_model_amendment_keeps_omitted_id_until_its_own_proof},
    {"UNKNOWN retains original nullable observation and fences new business", unknown_observation_fences_the_batch_and_never_proves_ids},
    {"cancel preserves a late response while refusing business and ID completion", cancel_keeps_late_original_observation_without_business_or_ack},
    {"reserved and dispatched restarts retain original actor and call holds", reserved_and_dispatched_restarts_keep_original_actor_hold},
    {"post-T1 storage failure retains capacity and evidence retry never applies T4", post_t1_storage_failure_retains_slot_and_flush_never_reapplies_t4},
    {"close joins the callback and persists only its original observation", close_joins_callback_and_stores_only_original_observation},
    {"stop between a false gate predicate and blocking wait cannot be lost", stop_between_false_predicate_and_wait_cannot_lose_its_wakeup},
    {"destructor joins the live callback before releasing the actual Store writer lease", destructor_joins_callback_before_releasing_the_store_writer_lease},
    {"real adapter flags and excessive runtime limits refuse before T0", real_adapter_flag_and_excess_limits_fail_before_t0},
    {"empty S releases only its unassociated original claim and preserves pending IDs", empty_selection_releases_only_unassociated_original_claim}
}); }
