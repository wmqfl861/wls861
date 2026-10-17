#include "wls/scheduler.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <utility>

using namespace wls;

namespace {

// Gates establish ordering; the timeout is only a deadlock watchdog, never a
// claim that a callback "probably did not run" because enough time elapsed.
class Gate {
public:
    Gate() : future_(promise_.get_future().share()) {}
    ~Gate() { release(); }
    void wait() const { future_.wait(); }
    void release() noexcept {
        if (!released_) {
            released_ = true;
            try { promise_.set_value(); } catch (...) {}
        }
    }
private:
    std::promise<void> promise_;
    std::shared_future<void> future_;
    bool released_{};
};

template<class Future>
void await_ready(Future& future) {
    CHECK(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
}

template<class Function>
void expect_error(SchedulerErrorCode code, Function&& function) {
    bool caught = false;
    try { function(); }
    catch (const SchedulerError& error) {
        CHECK(error.code() == code);
        caught = true;
    }
    CHECK(caught);
}

void same_resource_serializes_and_other_resource_progresses() {
    Scheduler scheduler(3, 12);
    Gate first_release;
    std::promise<void> first_started;
    auto first_event = first_started.get_future();
    std::atomic<int> active{};
    std::mutex order_mutex;
    std::vector<int> order;
    auto record = [&](int number) {
        std::lock_guard lock(order_mutex);
        order.push_back(number);
    };
    auto first = scheduler.submit("first", "shared", WorkClass::Normal, [&] {
        CHECK(active.fetch_add(1) == 0);
        record(1);
        first_started.set_value();
        first_release.wait();
        CHECK(active.fetch_sub(1) == 1);
    });
    await_ready(first_event);
    first_event.get();
    auto second = scheduler.submit("second", "shared", WorkClass::Normal, [&] {
        CHECK(active.fetch_add(1) == 0);
        record(2);
        CHECK(active.fetch_sub(1) == 1);
    });
    auto third = scheduler.submit("third", "shared", WorkClass::Review, [&] {
        CHECK(active.fetch_add(1) == 0);
        record(3);
        CHECK(active.fetch_sub(1) == 1);
    });
    // A queued blocked resource does not block unrelated later callbacks.
    auto independent = scheduler.submit("independent", "other", WorkClass::Normal, [&] {
        CHECK(active.load() == 1);
    });
    await_ready(independent);
    independent.get();
    first_release.release();
    first.get();
    second.get();
    third.get();
    CHECK(order == std::vector<int>({1, 2, 3}));
    CHECK(scheduler.snapshot().outstanding == 0);
}

void independent_keys_truly_run_concurrently() {
    Scheduler scheduler(3, 6);
    Gate release;
    std::promise<void> started_one;
    std::promise<void> started_two;
    auto event_one = started_one.get_future();
    auto event_two = started_two.get_future();
    auto first = scheduler.submit("one", "key-one", WorkClass::Normal, [&] {
        started_one.set_value();
        release.wait();
    });
    auto second = scheduler.submit("two", "key-two", WorkClass::Normal, [&] {
        started_two.set_value();
        release.wait();
    });
    await_ready(event_one);
    await_ready(event_two);
    CHECK(scheduler.snapshot().normal_running == 2);
    release.release();
    first.get();
    second.get();
}

void admission_and_execution_reserve_protect_control_work() {
    for (const auto control_class : {WorkClass::Review, WorkClass::Handoff}) {
        Scheduler scheduler(3, 5, 1);
        Gate release;
        std::promise<void> normal_one_started;
        std::promise<void> normal_two_started;
        std::promise<void> control_started;
        auto event_one = normal_one_started.get_future();
        auto event_two = normal_two_started.get_future();
        auto control_event = control_started.get_future();
        std::vector<std::future<void>> futures;
        futures.push_back(scheduler.submit("normal-one", "normal-one", WorkClass::Normal, [&] {
            normal_one_started.set_value();
            release.wait();
        }));
        futures.push_back(scheduler.submit("normal-two", "normal-two", WorkClass::Normal, [&] {
            normal_two_started.set_value();
            release.wait();
        }));
        await_ready(event_one);
        await_ready(event_two);
        futures.push_back(scheduler.submit("normal-three", "normal-three", WorkClass::Normal,
            [&] { release.wait(); }));
        futures.push_back(scheduler.submit("normal-four", "normal-four", WorkClass::Normal,
            [&] { release.wait(); }));
        expect_error(SchedulerErrorCode::NormalCapacityReserved, [&] {
            (void)scheduler.submit("normal-five", "normal-five", WorkClass::Normal, [] {});
        });
        futures.push_back(scheduler.submit("control", "control", control_class, [&] {
            control_started.set_value();
            release.wait();
        }));
        await_ready(control_event);
        const auto state = scheduler.snapshot();
        CHECK(state.outstanding == 5);
        CHECK(state.normal_outstanding == 4);
        CHECK(state.normal_running == 2);
        CHECK(state.running == 3);
        CHECK(state.queued == 2);
        expect_error(SchedulerErrorCode::CapacityFull, [&] {
            (void)scheduler.submit("overflow", "overflow", WorkClass::Handoff, [] {});
        });
        release.release();
        for (auto& future : futures) future.get();
    }
}

void runnable_classes_take_turns_without_starvation() {
    Scheduler scheduler(2, 32);
    Gate permanent_release;
    Gate queue_release;
    std::promise<void> permanent_started;
    std::promise<void> queue_started;
    auto permanent_event = permanent_started.get_future();
    auto queue_event = queue_started.get_future();
    auto permanent = scheduler.submit("occupied", "occupied", WorkClass::Handoff, [&] {
        permanent_started.set_value();
        permanent_release.wait();
    });
    await_ready(permanent_event);
    auto initial = scheduler.submit("initial", "initial", WorkClass::Review, [&] {
        queue_started.set_value();
        queue_release.wait();
    });
    await_ready(queue_event);
    std::mutex order_mutex;
    std::vector<WorkClass> order;
    std::vector<std::future<void>> queued;
    // Controls arrive first; late Normal work must still receive its turn.
    for (const auto work_class : {WorkClass::Review, WorkClass::Handoff, WorkClass::Normal}) {
        for (int index = 0; index < 5; ++index) {
            const auto id = std::to_string(static_cast<int>(work_class)) + "-" + std::to_string(index);
            queued.push_back(scheduler.submit(id, id, work_class, [&, work_class] {
                std::lock_guard lock(order_mutex);
                order.push_back(work_class);
            }));
        }
    }
    queue_release.release();
    initial.get();
    for (auto& future : queued) { await_ready(future); future.get(); }
    std::vector<WorkClass> expected;
    for (int index = 0; index < 5; ++index) {
        expected.push_back(WorkClass::Handoff);
        expected.push_back(WorkClass::Normal);
        expected.push_back(WorkClass::Review);
    }
    CHECK(order == expected);
    permanent_release.release();
    permanent.get();
}

void cancellation_is_explicit_and_running_work_finishes() {
    Scheduler scheduler(2, 8);
    // The gate must release on unwind before async close's future is destroyed.
    std::future<void> closing;
    Gate release;
    std::promise<void> started;
    auto started_event = started.get_future();
    std::atomic<bool> queued_ran{false};
    std::atomic<bool> running_finished{false};
    auto running = scheduler.submit("running", "running", WorkClass::Normal, [&] {
        started.set_value();
        release.wait();
        running_finished = true;
    });
    await_ready(started_event);
    auto pending = scheduler.submit("pending", "pending", WorkClass::Normal, [&] {
        queued_ran = true;
    });
    closing = std::async(std::launch::async, [&] { scheduler.close(false); });
    await_ready(pending);
    expect_error(SchedulerErrorCode::Cancelled, [&] { pending.get(); });
    CHECK(!queued_ran.load());
    CHECK(!running_finished.load());
    expect_error(SchedulerErrorCode::Closed, [&] {
        (void)scheduler.submit("late", "late", WorkClass::Review, [] {});
    });
    release.release();
    running.get();
    await_ready(closing);
    closing.get();
    CHECK(running_finished.load());
    CHECK(!scheduler.snapshot().accepting);
    CHECK(scheduler.snapshot().outstanding == 0);
    scheduler.close();
}

void drain_completes_accepted_jobs_and_propagates_exceptions() {
    Scheduler scheduler(2, 16);
    Gate release;
    std::atomic<int> completed{};
    std::vector<std::future<void>> futures;
    for (int index = 0; index < 10; ++index) {
        const auto id = std::to_string(index);
        futures.push_back(scheduler.submit(id, id, WorkClass::Normal, [&] {
            release.wait();
            ++completed;
        }));
    }
    auto failure = scheduler.submit("exception", "exception", WorkClass::Review,
        [] { throw std::logic_error("synthetic failure"); });
    await_ready(failure);
    test::throws<std::logic_error>([&] { failure.get(); });
    auto nonstandard_failure = scheduler.submit("nonstandard", "nonstandard", WorkClass::Review,
        [] { throw 7; });
    await_ready(nonstandard_failure);
    test::throws<int>([&] { nonstandard_failure.get(); });
    auto after_failure = scheduler.submit("after-failure", "after-failure", WorkClass::Review, [] {});
    after_failure.get();
    release.release();
    scheduler.close(true);
    for (auto& future : futures) future.get();
    CHECK(completed.load() == 10);
    CHECK(scheduler.snapshot().outstanding == 0);
    CHECK(scheduler.snapshot().running == 0);
}

void callback_can_use_scheduler_without_queue_lock() {
    Scheduler scheduler(2, 8);
    auto outer = scheduler.submit("outer", "outer", WorkClass::Normal, [&] {
        CHECK(scheduler.snapshot().running >= 1);
        auto inner = scheduler.submit("inner", "inner", WorkClass::Review, [] {});
        inner.get();
        expect_error(SchedulerErrorCode::InvalidShutdown, [&] { scheduler.close(); });
    });
    await_ready(outer);
    outer.get();
}

void invalid_and_duplicate_jobs_do_not_get_accepted() {
    for (const auto workers : {std::size_t{0}, std::size_t{1}}) {
        expect_error(SchedulerErrorCode::InvalidConfiguration, [&] { Scheduler invalid(workers, 8); });
    }
    expect_error(SchedulerErrorCode::InvalidConfiguration, [] { Scheduler invalid(2, 1); });
    expect_error(SchedulerErrorCode::InvalidConfiguration, [] { Scheduler invalid(2, 8, 0); });
    Scheduler scheduler(2, 8);
    Gate release;
    auto accepted = scheduler.submit("same", "same", WorkClass::Normal, [&] { release.wait(); });
    expect_error(SchedulerErrorCode::DuplicateJob, [&] {
        (void)scheduler.submit("same", "different", WorkClass::Review, [] {});
    });
    expect_error(SchedulerErrorCode::InvalidJob, [&] {
        (void)scheduler.submit("", "key", WorkClass::Normal, [] {});
    });
    expect_error(SchedulerErrorCode::InvalidJob, [&] {
        (void)scheduler.submit("id", "", WorkClass::Normal, [] {});
    });
    expect_error(SchedulerErrorCode::InvalidJob, [&] {
        (void)scheduler.submit("id", "key", WorkClass::Normal, {});
    });
    expect_error(SchedulerErrorCode::InvalidJob, [&] {
        (void)scheduler.submit("id", "key", static_cast<WorkClass>(99), [] {});
    });
    CHECK(scheduler.snapshot().outstanding == 1);
    release.release();
    accepted.get();
    // Duplicate protection is bounded to outstanding jobs, not an unbounded log.
    auto reused = scheduler.submit("same", "new-key", WorkClass::Normal, [] {});
    reused.get();
}

} // namespace

int main() {
    return test::run({
        {"same resource is FIFO serial while unrelated work progresses", same_resource_serializes_and_other_resource_progresses},
        {"independent keys execute concurrently", independent_keys_truly_run_concurrently},
        {"normal work preserves admission and execution reserve", admission_and_execution_reserve_protect_control_work},
        {"runnable classes rotate without starvation", runnable_classes_take_turns_without_starvation},
        {"close cancels queued jobs and waits for running callbacks", cancellation_is_explicit_and_running_work_finishes},
        {"drain completes accepted jobs and future preserves exceptions", drain_completes_accepted_jobs_and_propagates_exceptions},
        {"callbacks run outside scheduler mutex", callback_can_use_scheduler_without_queue_lock},
        {"invalid configuration and duplicate jobs reject", invalid_and_duplicate_jobs_do_not_get_accepted}
    });
}
