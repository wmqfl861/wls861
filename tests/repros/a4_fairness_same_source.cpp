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

} // namespace

int main() {
    schemas = std::make_shared<SchemaRegistry>(source / "schemas");
    base_sql = read_runtime_file(source / "db/schema.sql");
    store_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    events_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    workflow_sql = read_runtime_file(source / "db/migrations/007_workflow_runtime.sql");
    acceptance_sql = read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql");
    return test::run({{"S1 request then team fairness under one-slot release", queued_s1_requests_and_teams_rotate_without_starvation}});
}
