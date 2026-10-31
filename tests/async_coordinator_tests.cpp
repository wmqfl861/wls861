#include "wls/async_coordinator.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
using namespace wls;
using namespace wls::s1;
using Object = boost::json::object;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::shared_ptr<SchemaRegistry> schemas;
std::string base_sql, store_sql, tools_sql, events_sql, workflow_sql, acceptance_sql;

std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t number(const Json& value, std::string_view key) { return value.at(key).as_int64(); }
Json fixture(const char* name) { return parse_json_strict(read_runtime_file(source / "tests/fixtures" / name)); }
std::vector<FakeModelStep> steps() { return fake_model_steps_from_json(fixture("s1_fake_initial.json")); }

void sql(const std::filesystem::path& path, const char* statement) {
    sqlite3* db{};
    CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    const auto code = sqlite3_exec(db, statement, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    CHECK(code == SQLITE_OK);
}

std::int64_t count(const std::filesystem::path& path, const char* statement) {
    sqlite3* db{};
    CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* query{};
    CHECK(sqlite3_prepare_v2(db, statement, -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW);
    const auto result = sqlite3_column_int64(query, 0);
    sqlite3_finalize(query); sqlite3_close(db);
    return result;
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<int> entered{};
    bool released{};
    void release() { { std::lock_guard lock(mutex); released = true; } changed.notify_all(); }
    void wait(std::stop_token stop, bool respect_stop) {
        std::stop_callback wake(stop, [this] { changed.notify_all(); });
        std::unique_lock lock(mutex);
        ++entered;
        changed.notify_all();
        changed.wait(lock, [&] { return released || (respect_stop && stop.stop_requested()); });
    }
};
struct Release {
    std::shared_ptr<Gate> gate;
    ~Release() { gate->release(); }
};

class GatedModel final : public ModelAdapter {
public:
    GatedModel(std::string ref, std::shared_ptr<Gate> gate = {}, bool respect_stop = false,
               bool throws = false, bool error_response = false)
        : fake_([&] {
            auto script = steps();
            if (error_response) for (auto& step : script) {
                step.status = ModelResultStatus::Error;
                step.error_code = "synthetic_offline_failure";
                step.error_message = "Original offline model failure for scheduling observation";
                step.tool_calls.clear();
            }
            return script;
        }(), std::move(ref)), gate_(std::move(gate)), respect_stop_(respect_stop), throws_(throws) {}
    AdapterCapabilities capabilities() const override { return fake_.capabilities(); }
    void validate(const ModelRequest& request) const override { fake_.validate(request); }
    ModelResponse complete(ModelRequest request, std::stop_token stop) const override {
        CHECK(std::this_thread::get_id() != creator_);
        ++calls;
        if (on_start) on_start();
        if (gate_) gate_->wait(stop, respect_stop_);
        if (throws_) throw std::runtime_error("synthetic offline adapter exception");
        // Deliberately return the original result even after cancellation.
        return fake_.complete(std::move(request));
    }
    mutable std::atomic<int> calls{};
    std::function<void()> on_start;
private:
    FakeModel fake_;
    std::shared_ptr<Gate> gate_;
    bool respect_stop_, throws_;
    const std::thread::id creator_{std::this_thread::get_id()};
};

struct Fixture {
    test::TempDir directory;
    std::filesystem::path path{directory.path / "control.sqlite"};
    FrozenRequestConfig config{load_config_catalog(source / "config/examples/system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general")};
    std::unique_ptr<Store> open(StoreRecoveryScope scope = {}) const {
        return std::make_unique<Store>(path, base_sql, store_sql, StoreOpenMode::Takeover,
            tools_sql, events_sql, workflow_sql, acceptance_sql, "", std::move(scope));
    }
    std::string admit(Store& store) const {
        AdmissionInput in;
        in.tenant_id = "async-tenant"; in.project_id = "async-project"; in.root_id = "async-root";
        in.function_id = config.function_id(); in.idempotency_key = new_id("key");
        in.original_text = read_runtime_file(source / "tests/fixtures/s1_request.txt");
        in.config_snapshot = config.json(); in.constraint_catalog = fixture("s1_constraints.json");
        const auto grants = config.worker_tools(); in.tool_allowlist.assign(grants.begin(), grants.end());
        in.budget.input_tokens = 10000000; in.budget.output_tokens = 1000000;
        return text(store.admit(in), "request_id");
    }
    std::shared_ptr<GatedModel> model(std::shared_ptr<Gate> gate = {}, bool stop = false,
                                    bool throws = false, bool error_response = false) const {
        return std::make_shared<GatedModel>(config.resolve(Role::Dispatcher).model, std::move(gate), stop, throws, error_response);
    }
    std::string ready(Store& store, const std::string& id) const {
        (void)OfflineRuntime(store, *schemas).run(id, steps());
        return text(store.tasks(id).front(), "task_id");
    }
};

template<class Predicate>
void until(AsyncCoordinator& runtime, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        (void)runtime.poll();
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
bool ready(std::future<Json>& future) { return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }

void genuine_overlap_and_sibling_commit() {
    Fixture f;
    AsyncCoordinator runtime(f.open(), schemas);
    const auto a = f.admit(runtime.store()), b = f.admit(runtime.store()), c = f.admit(runtime.store());
    auto gate = std::make_shared<Gate>(); Release release{gate};
    const auto first = runtime.capture(a, Role::Dispatcher), second = runtime.capture(b, Role::Dispatcher);
    auto fa = runtime.submit(first, f.model(gate));
    auto fb = runtime.submit(second, f.model(gate));
    until(runtime, [&] { return gate->entered == 2; });
    CHECK(!ready(fa) && !ready(fb));
    const auto third = runtime.capture(c, Role::Dispatcher);
    auto fc = runtime.submit(third, f.model());
    until(runtime, [&] { return ready(fc); });
    (void)fc.get();
    CHECK(runtime.store().request(c).at("plan_revision") == 1);
    CHECK(runtime.store().request(a).at("plan_revision") == 0);
    CHECK(runtime.store().request(b).at("plan_revision") == 0);
    // A real read connection observes the third commit while both models wait.
    CHECK(count(f.path, "SELECT count(*) FROM call_attempts WHERE status='PENDING'") == 2);
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 3);
    gate->release(); runtime.drain(); (void)fa.get(); (void)fb.get();
    CHECK(runtime.store().attempt(first.request.attempt_id).at("request_envelope") == to_json(first.request));
    CHECK(runtime.store().tool_feedback(second.request.attempt_id).at("original_binding") == to_json(second.request.binding));
    CHECK(runtime.snapshot().outstanding == 0);
}

void complete_s1_chain_and_unique_replay() {
    Fixture f;
    AsyncCoordinator runtime(f.open(), schemas);
    const auto id = f.admit(runtime.store());
    auto model = f.model();
    const auto plan = runtime.capture(id, Role::Dispatcher);
    auto planned = runtime.submit(plan, model); runtime.drain(); (void)planned.get();
    const auto task = text(runtime.store().tasks(id).front(), "task_id");
    auto order = runtime.submit(runtime.capture(id, Role::TeamLead, task), model);
    runtime.drain(); (void)order.get();
    const auto binding = runtime.store().binding(task);
    (void)runtime.store().acknowledge(runtime.store().host_actor(Role::Worker, id, binding.team_id), binding,
        number(runtime.store().task(task), "state_version"), new_id("ack"));
    const auto note = runtime.capture(id, Role::Worker, task);
    auto noted = runtime.submit(note, model); runtime.drain();
    CHECK(noted.get().at("status") == "SUCCEEDED");
    const auto original = runtime.store().attempt(note.request.attempt_id);
    const auto feedback = runtime.store().tool_feedback(note.request.attempt_id);
    CHECK(model->calls == 3);
    auto replay = runtime.submit(note, model); runtime.drain();
    test::throws<Error>([&] { (void)replay.get(); });
    CHECK(model->calls == 3);
    CHECK(runtime.store().attempt(note.request.attempt_id) == original);
    CHECK(runtime.store().tool_feedback(note.request.attempt_id) == feedback);
    CHECK(count(f.path, "SELECT count(*) FROM ops") == 1);
    CHECK(runtime.store().budget(id).at("attempts") == 3);
    CHECK(runtime.store().budget(id).at("spent_input") == 300);
}

void full_queue_keeps_control_and_result_reserves() {
    Fixture f;
    AsyncCoordinator runtime(f.open(), schemas, {4, 1});
    const auto a = f.admit(runtime.store()), b = f.admit(runtime.store()), c = f.admit(runtime.store());
    const auto d = f.admit(runtime.store()), task = f.ready(runtime.store(), d);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    auto fa = runtime.submit(runtime.capture(a, Role::Dispatcher), f.model(gate));
    auto fb = runtime.submit(runtime.capture(b, Role::Dispatcher), f.model(gate));
    until(runtime, [&] { return gate->entered == 2; });
    const auto third = runtime.capture(c, Role::Dispatcher);
    test::throws<Error>([&] { (void)runtime.submit(third, f.model()); });
    CHECK(runtime.store().budget(c).at("attempts") == 0);
    auto review = runtime.submit(third, f.model(gate), WorkClass::Review);
    auto worker_review = runtime.submit(runtime.capture(d, Role::Worker, task), f.model(gate), WorkClass::Review);
    until(runtime, [&] { return gate->entered == 4; });
    const auto root = runtime.store().host_actor(Role::Root, a);
    const auto version = number(runtime.store().request(a), "state_version");
    auto control = std::async(std::launch::async, [&] { return runtime.cancel(root, version, new_id("cancel")); }).get();
    test::throws<Error>([&] { (void)runtime.cancel(root, version, new_id("full")); });
    (void)runtime.poll();
    CHECK(control.get().at("status") == "CANCELLED");
    CHECK(!ready(fa) && runtime.snapshot().outstanding == 4);
    gate->release(); runtime.drain();
    test::throws<Error>([&] { (void)fa.get(); });
    (void)fb.get(); (void)review.get(); (void)worker_review.get();
    CHECK(runtime.snapshot().outstanding == 0);
    CHECK(runtime.store().tasks(a).empty());
    CHECK(runtime.store().budget(a).at("spent_input") == 100);
}

void amend_retains_late_original_without_business() {
    Fixture f;
    AsyncCoordinator runtime(f.open(), schemas);
    const auto id = f.admit(runtime.store()); const auto task = f.ready(runtime.store(), id);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    const auto old = runtime.capture(id, Role::Worker, task);
    auto future = runtime.submit(old, f.model(gate));
    until(runtime, [&] { return gate->entered == 1; });
    auto amended = runtime.amend(runtime.store().host_actor(Role::Root, id),
        number(runtime.store().request(id), "state_version"),
        read_runtime_file(source / "tests/fixtures/s1_amendment.txt"),
        fixture("s1_amended_constraints.json"), new_id("amend"));
    (void)runtime.poll(); (void)amended.get();
    const auto after = runtime.store().task(task);
    gate->release(); runtime.drain(); test::throws<Error>([&] { (void)future.get(); });
    CHECK(runtime.store().task(task) == after);
    CHECK(count(f.path, "SELECT count(*) FROM ops") == 1);
    const auto attempt = runtime.store().attempt(old.request.attempt_id);
    CHECK(attempt.at("status") == "STALE" && attempt.at("accepted") == 0);
    CHECK(attempt.at("request_envelope") == to_json(old.request));
    CHECK(attempt.at("response").at("binding") == to_json(old.request.binding));
    CHECK(runtime.store().budget(id).at("spent_input") == 400);
    CHECK(!runtime.store().tool_feedback(old.request.attempt_id).is_null());
    auto replay_model = f.model();
    auto replay = runtime.submit(old, replay_model); runtime.drain();
    test::throws<Error>([&] { (void)replay.get(); });
    CHECK(replay_model->calls == 0);
}

void queued_cancel_never_enters_model() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas);
    const auto id = f.admit(runtime.store()); auto model = f.model();
    const auto call = runtime.capture(id, Role::Dispatcher);
    auto future = runtime.submit(call, model);
    auto cancelled = runtime.cancel(runtime.store().host_actor(Role::Root, id),
        number(runtime.store().request(id), "state_version"), new_id("cancel"));
    runtime.drain(); (void)cancelled.get();
    test::throws<Error>([&] { (void)future.get(); });
    CHECK(model->calls == 0);
    CHECK(runtime.store().tasks(id).empty());
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 0);
    CHECK(runtime.store().attempt(call.request.attempt_id).at("usage").at("input_tokens").is_null());
}

void store_session_and_first_claim_remain_authoritative() {
    Fixture f; CapturedModelCall old; std::string id;
    {
        auto store = f.open(); id = f.admit(*store);
        old = OfflineRuntime(*store, *schemas).capture_call(id, Role::Dispatcher);
    }
    AsyncCoordinator runtime(f.open(StoreRecoveryScope::request(id)), schemas);
    auto model = f.model();
    test::throws<Error>([&] { (void)runtime.submit(old, model); });
    CHECK(runtime.store().budget(id).at("attempts") == 0 && model->calls == 0);
    auto call = runtime.capture(id, Role::Dispatcher);
    auto future = runtime.submit(call, model);
    // Fault injection targets the actual Store transaction, not a fake claim.
    sql(f.path, "CREATE TRIGGER a3_claim_fault BEFORE INSERT ON events WHEN NEW.type='claim_offline_call' BEGIN SELECT RAISE(ABORT,'a3 original claim fault'); END;");
    runtime.drain(); test::throws<Error>([&] { (void)future.get(); });
    CHECK(model->calls == 0);
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 0);
    CHECK(runtime.store().tasks(id).empty());
    sql(f.path, "DROP TRIGGER a3_claim_fault");
}

void persistence_failure_retains_result_and_only_retries_evidence() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas);
    const auto id = f.admit(runtime.store()); const auto task = f.ready(runtime.store(), id);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    const auto call = runtime.capture(id, Role::Worker, task);
    auto future = runtime.submit(call, f.model(gate));
    until(runtime, [&] { return gate->entered == 1; });
    sql(f.path, "CREATE TRIGGER a3_result_fault BEFORE UPDATE OF settlement_sha256 ON call_attempts BEGIN SELECT RAISE(ABORT,'a3 original settlement fault'); END;");
    gate->release(); test::throws<Error>([&] { runtime.drain(); });
    CHECK(runtime.snapshot().settlement_halted && runtime.snapshot().retained_results == 1);
    CHECK(!ready(future));
    const auto retained = runtime.retained_results();
    CHECK(retained.as_array().size() == 1);
    CHECK(retained.at(0).at("response").at("binding") == to_json(call.request.binding));
    CHECK(runtime.store().attempt(call.request.attempt_id).at("settlement_sha256").is_null());
    CHECK(count(f.path, "SELECT count(*) FROM ops") == 1);
    test::throws<Error>([&] { (void)runtime.submit(call, f.model()); });
    // Even a recovery between fault and flush cannot relabel the old response.
    const auto epoch = number(runtime.store().request(id), "dispatcher_epoch");
    (void)runtime.store().recover_request(id, number(runtime.store().request(id), "state_version"));
    CHECK(runtime.store().attempt(call.request.attempt_id).at("status") == "UNKNOWN");
    sql(f.path, "DROP TRIGGER a3_result_fault");
    runtime.flush_retained_evidence(); test::throws<Error>([&] { (void)future.get(); });
    CHECK(runtime.retained_results().as_array().empty());
    CHECK(runtime.store().attempt(call.request.attempt_id).at("response") == retained.at(0).at("response"));
    CHECK(runtime.store().attempt(call.request.attempt_id).at("accepted") == 0);
    CHECK(number(runtime.store().request(id), "dispatcher_epoch") > epoch);
    CHECK(count(f.path, "SELECT count(*) FROM ops") == 1);
    CHECK(runtime.snapshot().settlement_halted && runtime.snapshot().outstanding == 0);
}

void shutdown_joins_and_restart_preserves_unknown() {
    Fixture f; CapturedModelCall original; std::string id;
    {
        AsyncCoordinator runtime(f.open(), schemas);
        id = f.admit(runtime.store());
        auto gate = std::make_shared<Gate>(); Release release{gate};
        auto model = f.model(gate, true);
        original = runtime.capture(id, Role::Dispatcher);
        auto future = runtime.submit(original, model);
        until(runtime, [&] { return gate->entered == 1; });
        sql(f.path, "CREATE TRIGGER a3_shutdown_fault BEFORE UPDATE OF settlement_sha256 ON call_attempts BEGIN SELECT RAISE(ABORT,'a3 shutdown persistence fault'); END;");
        test::throws<Error>([&] { runtime.close(); });
        CHECK(runtime.snapshot().managers.running == 0 && runtime.snapshot().workers.running == 0);
        CHECK(runtime.retained_results().as_array().size() == 1);
        CHECK(runtime.store().tasks(id).empty());
    }
    sql(f.path, "DROP TRIGGER a3_shutdown_fault");
    auto recovered = f.open(StoreRecoveryScope::request(id));
    const auto attempt = recovered->attempt(original.request.attempt_id);
    CHECK(attempt.at("status") == "UNKNOWN" && attempt.at("settlement_sha256").is_null());
    CHECK(attempt.at("request_envelope") == to_json(original.request));
    CHECK(recovered->budget(id).at("reserved_input").as_int64() > 0);
    CHECK(recovered->tasks(id).empty());
    test::throws<Error>([&] { (void)recovered->claim_offline_call(original.actor, original.request.attempt_id); });
}

void adapter_exception_and_unsupported_long_tool_are_bounded() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas);
    const auto id = f.admit(runtime.store());
    const auto call = runtime.capture(id, Role::Dispatcher);
    auto model = f.model({}, false, true);
    auto future = runtime.submit(call, model); runtime.drain();
    test::throws<std::exception>([&] { (void)future.get(); });
    CHECK(runtime.store().attempt(call.request.attempt_id).at("response").at("error_code") == "host_adapter_exception");
    CHECK(runtime.store().budget(id).at("reserved_input").as_int64() > 0);
    CHECK(runtime.store().tasks(id).empty());
    auto forbidden = runtime.capture(id, Role::Dispatcher);
    forbidden.request.context.tool_names.push_back("run");
    test::throws<Error>([&] { (void)runtime.submit(forbidden, model); });
    CHECK(runtime.store().budget(id).at("attempts") == 1);
    auto wrong_thread = std::async(std::launch::async, [&] {
        test::throws<Error>([&] { (void)runtime.store(); });
    });
    wrong_thread.get();
}

void foreign_thread_destruction_cannot_deadlock_a_claim_waiter() {
    Fixture f; std::string id; CapturedModelCall call; std::future<Json> future;
    auto runtime = std::make_unique<AsyncCoordinator>(f.open(), schemas);
    id = f.admit(runtime->store()); call = runtime->capture(id, Role::Dispatcher);
    auto model = f.model(); future = runtime->submit(call, model);
    // Observe a worker owning the callback, without driving the Store host.
    // It must be waiting for first-dispatch permission, not already executing.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (runtime->snapshot().managers.running == 0) {
        CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield();
    }
    auto destroyed = std::async(std::launch::async, [owned = std::move(runtime)]() mutable { owned.reset(); });
    CHECK(destroyed.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    destroyed.get(); test::throws<std::future_error>([&] { (void)future.get(); });
    CHECK(model->calls == 0);
    auto recovered = f.open(StoreRecoveryScope::request(id));
    CHECK(recovered->attempt(call.request.attempt_id).at("status") == "UNKNOWN");
    CHECK(recovered->attempt(call.request.attempt_id).at("response").is_null());
    CHECK(recovered->tasks(id).empty());
}

void queued_worker_wait_and_unknown_cannot_acquire_permission() {
    for (const bool unknown : {false, true}) {
        Fixture f; AsyncCoordinator runtime(f.open(), schemas);
        const auto id = f.admit(runtime.store()), task = f.ready(runtime.store(), id);
        const auto captured = runtime.capture(id, Role::Worker, task);
        auto model = f.model(); auto future = runtime.submit(captured, model);
        if (unknown) {
            const auto prepared = runtime.store().prepare_operation(captured.actor, captured.request.binding,
                captured.expected_state_version, new_id("business"), "note", Object{{"text", "original unknown"}},
                "none", new_id("prepare"));
            const auto started = runtime.store().start_operation(captured.actor, text(prepared, "op_id"),
                number(prepared, "state_version"), new_id("start"));
            (void)runtime.store().finish_operation(captured.actor, text(prepared, "op_id"), captured.request.binding,
                number(started, "state_version"), "UNKNOWN", "host original unresolved result", new_id("unknown"));
        } else {
            (void)runtime.store().wait_worker(captured.actor, captured.request.binding,
                captured.expected_state_version, "explicit durable wait", new_id("wait"));
        }
        runtime.drain(); test::throws<Error>([&] { (void)future.get(); });
        CHECK(model->calls == 0);
        CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 0);
        CHECK(count(f.path, "SELECT count(*) FROM ops WHERE status='SUCCEEDED'") == 1);
    }
}

void failed_feedback_after_committed_effect_fences_without_reexecution() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas);
    const auto id = f.admit(runtime.store()), task = f.ready(runtime.store(), id);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    const auto captured = runtime.capture(id, Role::Worker, task);
    auto model = f.model(gate); auto future = runtime.submit(captured, model);
    until(runtime, [&] { return gate->entered == 1; });
    sql(f.path, "CREATE TRIGGER a3_feedback_fault BEFORE INSERT ON model_tool_feedback BEGIN SELECT RAISE(ABORT,'a3 original feedback fault'); END;");
    gate->release(); test::throws<Error>([&] { runtime.drain(); });
    CHECK(runtime.snapshot().settlement_halted && runtime.snapshot().retained_results == 1);
    CHECK(count(f.path, "SELECT count(*) FROM ops WHERE status='SUCCEEDED'") == 2);
    CHECK(!runtime.store().attempt(captured.request.attempt_id).at("settlement_sha256").is_null());
    CHECK(runtime.store().tool_feedback(captured.request.attempt_id).is_null());
    sql(f.path, "DROP TRIGGER a3_feedback_fault");
    runtime.flush_retained_evidence(); test::throws<Error>([&] { (void)future.get(); });
    CHECK(count(f.path, "SELECT count(*) FROM ops WHERE status='SUCCEEDED'") == 2);
    CHECK(model->calls == 1);
    CHECK(runtime.store().tool_feedback(captured.request.attempt_id).at("result").at("observation_complete") == false);
}

void both_s1_pools_keep_their_review_execution_reserve() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    std::vector<std::future<Json>> futures;
    for (int i = 0; i < 3; ++i) {
        const auto id = f.admit(runtime.store());
        futures.push_back(runtime.submit(runtime.capture(id, Role::Dispatcher), f.model(gate)));
    }
    for (int i = 0; i < 7; ++i) {
        const auto id = f.admit(runtime.store()), task = f.ready(runtime.store(), id);
        futures.push_back(runtime.submit(runtime.capture(id, Role::Worker, task), f.model(gate)));
    }
    until(runtime, [&] { return gate->entered == 10; });
    auto management_wait = std::make_shared<Gate>(), worker_wait = std::make_shared<Gate>();
    Release release_management{management_wait}, release_worker{worker_wait};
    const auto queued_manager = f.admit(runtime.store()), queued_worker = f.admit(runtime.store());
    const auto queued_task = f.ready(runtime.store(), queued_worker);
    futures.push_back(runtime.submit(runtime.capture(queued_manager, Role::Dispatcher), f.model(management_wait)));
    futures.push_back(runtime.submit(runtime.capture(queued_worker, Role::Worker, queued_task), f.model(worker_wait)));
    const auto review_manager = f.admit(runtime.store()), review_worker = f.admit(runtime.store());
    const auto review_task = f.ready(runtime.store(), review_worker);
    futures.push_back(runtime.submit(runtime.capture(review_manager, Role::Dispatcher), f.model(gate), WorkClass::Review));
    futures.push_back(runtime.submit(runtime.capture(review_worker, Role::Worker, review_task), f.model(gate), WorkClass::Review));
    until(runtime, [&] { return gate->entered == 12; });
    const auto snapshot = runtime.snapshot();
    CHECK(snapshot.managers.workers == 4 && snapshot.workers.workers == 8);
    CHECK(snapshot.managers.normal_running == 3 && snapshot.workers.normal_running == 7);
    CHECK(snapshot.managers.running == 4 && snapshot.workers.running == 8);
    CHECK(snapshot.queued == 2 && snapshot.executing == 12);
    CHECK(snapshot.max_wait_dispatches >= 2 && snapshot.blocked_reason == "execution_capacity");
    CHECK(management_wait->entered == 0 && worker_wait->entered == 0);
    // Durable original reservations/claims are actual S1 state, not M1 counters alone.
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 12);
    gate->release(); management_wait->release(); worker_wait->release();
    runtime.drain(); for (auto& future : futures) (void)future.get();
    CHECK(runtime.store().request(queued_manager).at("plan_revision") == 1);
    CHECK(runtime.store().tool_feedback(text(runtime.store().attempt_history(
        runtime.store().host_actor(Role::Worker, queued_worker,
            runtime.store().binding(queued_task).team_id)).back(), "attempt_id")).at("result").at("status") == "SUCCEEDED");
}

struct Order {
    std::mutex mutex;
    std::vector<std::string> names;
    void add(const std::string& name) { std::lock_guard lock(mutex); names.push_back(name); }
    std::vector<std::string> read() { std::lock_guard lock(mutex); return names; }
};

void queued_s1_requests_and_teams_rotate_without_starvation() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas);
    std::vector<std::shared_ptr<Gate>> blockers;
    struct ReleaseAll {
        std::vector<std::shared_ptr<Gate>>& gates;
        ~ReleaseAll() { for (const auto& gate : gates) gate->release(); }
    } release{blockers};
    std::vector<std::future<Json>> held;
    // Keep all 8 Worker slots occupied, then release exactly one ordinary slot.
    for (int i = 0; i < 8; ++i) {
        const auto id = f.admit(runtime.store()), task = f.ready(runtime.store(), id);
        auto gate = std::make_shared<Gate>(); blockers.push_back(gate);
        held.push_back(runtime.submit(runtime.capture(id, Role::Worker, task), f.model(gate),
            i == 7 ? WorkClass::Review : WorkClass::Normal));
    }
    until(runtime, [&] { return std::all_of(blockers.begin(), blockers.end(), [](const auto& gate) { return gate->entered == 1; }); });
    const auto busy = f.admit(runtime.store()), sibling = f.admit(runtime.store());
    // Build a real two-team independent plan using the existing S1 Store guards.
    auto script = steps();
    auto& nodes = script.front().tool_calls.front().arguments.at("nodes").as_array();
    auto second = nodes.front(); second.as_object()["node_id"] = "second-note"; nodes.push_back(second);
    OfflineRuntime offline(runtime.store(), *schemas);
    const FakeModel initial(script, f.config.resolve(Role::Dispatcher).model);
    const auto plan = offline.reserve_call(initial, busy, Role::Dispatcher);
    (void)offline.accept_independent_response(plan, initial.complete(plan.request));
    std::vector<std::string> tasks;
    for (const auto& item : runtime.store().tasks(busy)) {
        const auto task = text(item, "task_id"); tasks.push_back(task);
        const auto order = offline.reserve_call(initial, busy, Role::TeamLead, task);
        (void)offline.accept_response(order, initial.complete(order.request));
        const auto binding = runtime.store().binding(task);
        (void)runtime.store().acknowledge(runtime.store().host_actor(Role::Worker, busy, binding.team_id),
            binding, number(runtime.store().task(task), "state_version"), new_id("ack"));
    }
    const auto sibling_task = f.ready(runtime.store(), sibling);
    auto sequence = std::make_shared<Order>();
    std::vector<std::shared_ptr<Gate>> selected;
    ReleaseAll release_selected{selected};
    std::vector<std::future<Json>> queued;
    const std::vector<std::string> labels{"a1-first", "a1-second", "a1-third", "a2", "b1"};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        auto gate = std::make_shared<Gate>(); selected.push_back(gate);
        auto model = f.model(gate, false, false, true);
        const auto label = labels[i]; model->on_start = [sequence, label] { sequence->add(label); };
        const auto id = i == 4 ? sibling : busy;
        const auto task = i == 4 ? sibling_task : tasks.at(i == 3 ? 1 : 0);
        queued.push_back(runtime.submit(runtime.capture(id, Role::Worker, task), model));
    }
    blockers.front()->release();
    until(runtime, [&] { return sequence->read().size() >= 1; });
    CHECK(sequence->read() == std::vector<std::string>{"a1-first"});
    selected[0]->release();
    until(runtime, [&] { return sequence->read().size() >= 2; });
    CHECK(sequence->read() == (std::vector<std::string>{"a1-first", "b1"}));
    selected[4]->release();
    until(runtime, [&] { return sequence->read().size() >= 3; });
    CHECK(sequence->read() == (std::vector<std::string>{"a1-first", "b1", "a2"}));
    for (const auto& gate : selected) gate->release();
    for (const auto& gate : blockers) gate->release();
    runtime.drain();
    for (auto& future : held) (void)future.get();
    for (auto& future : queued) test::throws<Error>([&] { (void)future.get(); });
    CHECK(runtime.store().budget(busy).at("attempts") == 7); // Plan + two orders + four observed failed calls.
    CHECK(runtime.store().budget(sibling).at("attempts") == 4);
    CHECK(count(f.path, "SELECT count(*) FROM call_attempts WHERE status='ERROR'") == 5);
    CHECK(runtime.snapshot().outstanding == 0);
}

void same_resource_is_fifo_across_management_and_worker_pools() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas);
    const auto manager = f.admit(runtime.store()), worker = f.admit(runtime.store());
    const auto task = f.ready(runtime.store(), worker);
    auto first_gate = std::make_shared<Gate>(), second_gate = std::make_shared<Gate>();
    Release release_first{first_gate}, release_second{second_gate};
    auto first = runtime.submit(runtime.capture(manager, Role::Dispatcher), f.model(first_gate),
        WorkClass::Normal, "host-shared-resource");
    auto second = runtime.submit(runtime.capture(worker, Role::Worker, task), f.model(second_gate),
        WorkClass::Review, "host-shared-resource");
    until(runtime, [&] { return first_gate->entered == 1 && runtime.snapshot().workers.running == 1; });
    CHECK(second_gate->entered == 0);
    CHECK(runtime.snapshot().resource_blocked == 1);
    CHECK(runtime.snapshot().blocked_reason == "resource_busy");
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 1);
    first_gate->release();
    until(runtime, [&] { return second_gate->entered == 1; });
    CHECK(ready(first)); (void)first.get();
    CHECK(runtime.store().request(manager).at("plan_revision") == 1);
    second_gate->release(); runtime.drain(); CHECK(second.get().at("status") == "SUCCEEDED");
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 2);
}

void host_limits_are_validated_without_rewriting_frozen_requests() {
    Fixture invalid;
    AsyncCoordinatorLimits malformed;
    malformed.management_reserved = malformed.management_workers;
    test::throws<Error>([&] { AsyncCoordinator rejected(invalid.open(), schemas, malformed); });
    Fixture enlarged;
    AsyncCoordinatorLimits too_large;
    too_large.management_workers = 5;
    AsyncCoordinator host(enlarged.open(), schemas, too_large);
    const auto id = enlarged.admit(host.store());
    const auto snapshot = host.store().request(id).at("config_snapshot");
    const auto call = host.capture(id, Role::Dispatcher);
    test::throws<Error>([&] { (void)host.submit(call, enlarged.model()); });
    CHECK(host.store().budget(id).at("attempts") == 0);
    CHECK(host.store().request(id).at("config_snapshot") == snapshot);
    Fixture reduced;
    AsyncCoordinatorLimits smaller;
    smaller.management_workers = 2; smaller.worker_workers = 4;
    AsyncCoordinator bounded(reduced.open(), schemas, smaller);
    const auto other = reduced.admit(bounded.store());
    const auto original = bounded.store().request(other).at("config_snapshot");
    auto future = bounded.submit(bounded.capture(other, Role::Dispatcher), reduced.model());
    bounded.drain(); (void)future.get();
    CHECK(bounded.snapshot().managers.workers == 2 && bounded.snapshot().workers.workers == 4);
    CHECK(bounded.store().request(other).at("config_snapshot") == original);
    CHECK(bounded.store().request(other).at("plan_revision") == 1);
}

void model_callbacks_cannot_submit_same_pool_children() {
    Fixture f;
    AsyncCoordinatorLimits limits;
    limits.management_workers = 2; // One ordinary slot; a child could not run beside its parent.
    AsyncCoordinator runtime(f.open(), schemas, limits);
    const auto parent = f.admit(runtime.store()), child = f.admit(runtime.store());
    const auto child_call = runtime.capture(child, Role::Dispatcher);
    const auto child_request = runtime.store().request(child);
    const auto child_budget = runtime.store().budget(child);
    auto child_model = f.model(), parent_model = f.model();
    std::atomic<int> denied{};
    parent_model->on_start = [&] {
        // These are actual pool callback calls. No child future may be admitted
        // for the parent to synchronously wait on in this same full normal pool.
        try { (void)runtime.capture(child, Role::Dispatcher); }
        catch (const Error& error) { CHECK(error.code() == ErrorCode::Unauthorized); ++denied; }
        try { (void)runtime.submit(child_call, child_model); }
        catch (const Error& error) { CHECK(error.code() == ErrorCode::Unauthorized); ++denied; }
    };
    auto completed = runtime.submit(runtime.capture(parent, Role::Dispatcher), parent_model);
    runtime.drain(); (void)completed.get();
    CHECK(denied == 2 && parent_model->calls == 1 && child_model->calls == 0);
    CHECK(runtime.store().request(parent).at("plan_revision") == 1);
    CHECK(runtime.store().request(child) == child_request);
    CHECK(runtime.store().budget(child) == child_budget);
    CHECK(runtime.store().budget(child).at("attempts") == 0);
    CHECK(runtime.store().tasks(child).empty());
    CHECK(count(f.path, "SELECT count(*) FROM call_attempts") == 1);
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 1);
    CHECK(count(f.path, "SELECT count(*) FROM ops") == 0);
    CHECK(runtime.snapshot().outstanding == 0 && runtime.snapshot().managers.running == 0);
}

void logical_question_wait_releases_slot_without_releasing_unknown_budget() {
    // Record the adapter's original response, then read it only after drain has
    // joined its sole callback. Store, usage and wait conditions stay real.
    class RecordedFake final : public ModelAdapter {
    public:
        RecordedFake(FakeModelStep step, std::string reference)
            : fake_({std::move(step)}, std::move(reference)) {}
        AdapterCapabilities capabilities() const override { return fake_.capabilities(); }
        void validate(const ModelRequest& request) const override { fake_.validate(request); }
        ModelResponse complete(ModelRequest request, std::stop_token stop) const override {
            auto response = fake_.complete(std::move(request), stop);
            original_response = to_json(response);
            return response;
        }
        mutable Json original_response{nullptr};
    private:
        FakeModel fake_;
    };
    Fixture f;
    AsyncCoordinatorLimits limits;
    limits.worker_workers = 2; // Exactly one ordinary Worker execution slot.
    AsyncCoordinator runtime(f.open(), schemas, limits);
    const auto waiting = f.admit(runtime.store()), sibling = f.admit(runtime.store());
    const auto task = f.ready(runtime.store(), waiting), sibling_task = f.ready(runtime.store(), sibling);
    const auto before = runtime.store().budget(waiting);
    FakeModelStep ask;
    ask.role = Role::Worker;
    ask.tool_calls.push_back({"a4-question", "ask", Object{
        {"text", "Which original value should the waiting Worker preserve?"},
        {"context", "No value has been substituted while awaiting the answer."}, {"blocking", true}}});
    ask.usage = {std::nullopt, std::nullopt, 0, 0, 0};
    auto model = std::make_shared<RecordedFake>(ask, f.config.resolve(Role::Worker).model);
    const auto captured = runtime.capture(waiting, Role::Worker, task);
    CHECK(captured.request.reserved_input_tokens > 0 && captured.request.reserved_output_tokens > 0);
    auto asked = runtime.submit(captured, model);
    runtime.drain();
    const auto question = asked.get();
    CHECK(question.at("status") == "OPEN" && question.at("blocking") == 1);
    CHECK(question.at("body").at("text") == ask.tool_calls.front().arguments.at("text"));
    const auto wait = runtime.store().worker_wait(task);
    CHECK(wait.at("status") == "WAITING");
    CHECK(wait.at("original_binding") == to_json(captured.request.binding));
    CHECK(text(wait, "reason") == "question:" + text(question, "question_id"));
    CHECK(runtime.snapshot().outstanding == 0 && runtime.snapshot().workers.running == 0);
    const auto original = runtime.store().attempt(captured.request.attempt_id);
    const auto feedback = runtime.store().tool_feedback(captured.request.attempt_id);
    CHECK(original.at("status") == "COMPLETED" && original.at("accepted") == 1);
    CHECK(original.at("request_envelope") == to_json(captured.request));
    CHECK(original.at("response") == model->original_response);
    CHECK(original.at("usage") == to_json(ask.usage));
    CHECK(!original.at("settlement_sha256").is_null());
    CHECK(feedback.at("response") == model->original_response);
    const auto held = runtime.store().budget(waiting);
    CHECK(number(held, "attempts") == number(before, "attempts") + 1);
    CHECK(number(held, "reserved_input") == number(before, "reserved_input") + captured.request.reserved_input_tokens);
    CHECK(number(held, "reserved_output") == number(before, "reserved_output") + captured.request.reserved_output_tokens);
    CHECK(number(held, "unknown_input") == number(before, "unknown_input") + captured.request.reserved_input_tokens);
    CHECK(number(held, "unknown_output") == number(before, "unknown_output") + captured.request.reserved_output_tokens);
    CHECK(held.at("spent_input") == before.at("spent_input") && held.at("spent_output") == before.at("spent_output"));

    auto gate = std::make_shared<Gate>(); Release release{gate};
    auto sibling_model = f.model(gate);
    const auto sibling_budget = runtime.store().budget(sibling);
    auto advanced = runtime.submit(runtime.capture(sibling, Role::Worker, sibling_task), sibling_model);
    until(runtime, [&] { return gate->entered == 1; });
    CHECK(runtime.snapshot().workers.normal_running == 1 && runtime.snapshot().executing == 1);
    CHECK(runtime.store().question(text(question, "question_id")) == question);
    CHECK(runtime.store().worker_wait(task) == wait);
    CHECK(runtime.store().budget(waiting) == held);
    gate->release(); runtime.drain();
    CHECK(advanced.get().at("status") == "SUCCEEDED" && sibling_model->calls == 1);
    CHECK(number(runtime.store().budget(sibling), "attempts") == number(sibling_budget, "attempts") + 1);
    CHECK(runtime.snapshot().outstanding == 0 && runtime.snapshot().workers.running == 0);
    CHECK(runtime.store().question(text(question, "question_id")) == question);
    CHECK(runtime.store().worker_wait(task) == wait);
    CHECK(runtime.store().budget(waiting) == held);
    CHECK(runtime.store().attempt(captured.request.attempt_id) == original);
    CHECK(runtime.store().tool_feedback(captured.request.attempt_id) == feedback);
}
} // namespace

int main() {
    schemas = std::make_shared<SchemaRegistry>(source / "schemas");
    base_sql = read_runtime_file(source / "db/schema.sql");
    store_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    workflow_sql = read_runtime_file(source / "db/migrations/007_workflow_runtime.sql");
    acceptance_sql = read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql");
    return test::run({
        {"two models overlap while a third real S1 request commits", genuine_overlap_and_sibling_commit},
        {"S1 plan order ack note and original claim forbid duplicate execution", complete_s1_chain_and_unique_replay},
        {"full business queue retains control and result capacity", full_queue_keeps_control_and_result_reserves},
        {"AMEND rejects late business and preserves original response usage", amend_retains_late_original_without_business},
        {"queued cancellation prevents any model execution", queued_cancel_never_enters_model},
        {"Store session and transaction claim guards cannot be bypassed", store_session_and_first_claim_remain_authoritative},
        {"failed settlement retains response and retries evidence only", persistence_failure_retains_result_and_only_retries_evidence},
        {"shutdown joins before Store destruction and restart keeps UNKNOWN", shutdown_joins_and_restart_preserves_unknown},
        {"adapter exceptions thread ownership and long-tool refusal", adapter_exception_and_unsupported_long_tool_are_bounded},
        {"foreign thread destruction releases the claim waiter without Store access", foreign_thread_destruction_cannot_deadlock_a_claim_waiter},
        {"queued Worker wait and UNKNOWN prevent first permission", queued_worker_wait_and_unknown_cannot_acquire_permission},
        {"feedback persistence failure fences an already committed effect", failed_feedback_after_committed_effect_fences_without_reexecution},
        {"S1 management and Worker pools both retain Review execution capacity", both_s1_pools_keep_their_review_execution_reserve},
        {"queued S1 requests and teams rotate under sustained load", queued_s1_requests_and_teams_rotate_without_starvation},
        {"same resource remains FIFO across both S1 execution pools", same_resource_is_fifo_across_management_and_worker_pools},
        {"host capacities respect original frozen maxima and review reserves", host_limits_are_validated_without_rewriting_frozen_requests},
        {"actual model callbacks cannot admit same-pool child calls", model_callbacks_cannot_submit_same_pool_children},
        {"logical question wait releases its slot and preserves original unknown budget", logical_question_wait_releases_slot_without_releasing_unknown_budget}});
}
