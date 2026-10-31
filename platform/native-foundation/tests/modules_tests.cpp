#include "support.hpp"
#include "wls/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>

namespace {
using namespace wls;
using native_test::rejects;

void frozen_root_resolution() {
    CapabilityRegistry capability;
    capability.add("root", {{"high", "low"}, true, true});
    capability.add("alternate", {{"high", "max"}, true, true});
    const ModelSelection frozen{"root", {ReasoningMode::Explicit, "high"}};
    const auto manager = resolve_model(frozen, {"alternate", {ReasoningMode::Explicit, "max"}}, capability);
    CHECK(manager.model == "alternate");
    CHECK(resolve_model(frozen, {}, capability) == frozen);
    const auto model_only = resolve_model(frozen, {"alternate", {}}, capability);
    CHECK(model_only.model == "alternate" && model_only.reasoning == frozen.reasoning);
    const auto reasoning_only = resolve_model(frozen, {"", {ReasoningMode::Disabled, ""}}, capability);
    CHECK(reasoning_only.model == "root" && reasoning_only.reasoning.mode == ReasoningMode::Disabled);
    rejects<ConfigurationError>([&] { (void)resolve_model(frozen, {"absent", {}}, capability); });
    rejects<ConfigurationError>([&] { (void)resolve_model(frozen, {"", {ReasoningMode::Explicit, "max"}}, capability); });
    rejects<ConfigurationError>([&] { validate_root_defaults({"root", {}}, capability); });
}

void tools_are_worker_intersection() {
    const ToolSet role{"read", "write", "role-only"};
    const ToolSet system{"read", "write", "system-only"};
    const ToolSet request{"read", "request-only"};
    CHECK(effective_tool_grants(Role::Worker, role, system, request) == ToolSet{"read"});
    for (const auto manager : {Role::Root, Role::Dispatcher, Role::TeamLead})
        CHECK(effective_tool_grants(manager, role, system, request).empty());
    CHECK(!tool_allowed(Role::Worker, "write", role, system, request));
    CHECK(!tool_allowed(Role::Worker, "read", role, system, {}));
}

class Gate {
public:
    Gate() : ready_(promise_.get_future().share()) {}
    ~Gate() { release(); }
    void wait() const { ready_.wait(); }
    void release() { if (!released_) { released_ = true; promise_.set_value(); } }
private:
    std::promise<void> promise_;
    std::shared_future<void> ready_;
    bool released_{};
};

void scheduler_reserve_and_serialization() {
    // Destruction releases the barrier before the scheduler joins on failure.
    std::mutex mutex;
    std::vector<int> order;
    std::atomic<int> active{};
    std::promise<void> started;
    auto started_event = started.get_future();
    Scheduler scheduler(2, 3, 1);
    Gate release;
    auto first = scheduler.submit("first", "shared", WorkClass::Normal, [&] {
        CHECK(active.fetch_add(1) == 0);
        { std::lock_guard lock(mutex); order.push_back(1); }
        started.set_value();
        release.wait();
        CHECK(active.fetch_sub(1) == 1);
    });
    CHECK(started_event.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    started_event.get();
    auto second = scheduler.submit("second", "shared", WorkClass::Normal, [&] {
        CHECK(active.fetch_add(1) == 0);
        { std::lock_guard lock(mutex); order.push_back(2); }
        CHECK(active.fetch_sub(1) == 1);
    });
    rejects<SchedulerError>([&] { (void)scheduler.submit("overflow", "other", WorkClass::Normal, [] {}); });
    auto review = scheduler.submit("review", "review", WorkClass::Review, [] {});
    CHECK(review.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    review.get();
    CHECK(scheduler.snapshot().normal_outstanding == 2);
    release.release();
    first.get();
    second.get();
    scheduler.close();
    CHECK(order == std::vector<int>({1, 2}));
    CHECK(scheduler.snapshot().outstanding == 0);
    rejects<SchedulerError>([&] { (void)scheduler.submit("closed", "other", WorkClass::Review, [] {}); });
}
} // namespace

int main() {
    return native_test::run({
        {"original frozen Root resolution and rejection", frozen_root_resolution},
        {"original Worker-only tool grant intersection", tools_are_worker_intersection},
        {"original scheduler reserve and same-key serialization", scheduler_reserve_and_serialization}
    });
}
