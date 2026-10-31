#pragma once

#include "wls/offline_runtime.hpp"
#include "wls/scheduler.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace wls::s1 {

struct AsyncCoordinatorLimits {
    // All accepted calls, including results awaiting persistence, occupy one
    // slot. Ordinary admission preserves both pools' Review/Handoff reserves.
    std::size_t business_capacity{64};
    std::size_t control_capacity{16};
    std::size_t management_workers{4};
    std::size_t worker_workers{8};
    std::size_t management_reserved{1};
    std::size_t worker_reserved{1};
};

struct AsyncCoordinatorSnapshot {
    std::size_t outstanding{};
    std::size_t retained_results{};
    std::size_t controls{};
    bool accepting{};
    bool settlement_halted{};
    SchedulerSnapshot managers;
    SchedulerSnapshot workers;
    std::size_t queued{};
    std::size_t awaiting_claim{};
    std::size_t executing{};
    std::size_t ready_results{};
    std::size_t resource_blocked{};
    std::uint64_t dispatches{};
    std::uint64_t max_wait_dispatches{};
    std::string blocked_reason;
};

// Opt-in, offline S1 call coordinator. The constructing host thread owns all
// Store access and drives poll/drain. Scheduler callbacks only wait on/execute
// offline models; they never access the Store. The owned Store and schemas
// outlive every callback, including cancellation and exception unwinding.
//
// This is a bounded call-level API, not a daemon or an automatic event pump.
// It never consumes inbox IDs. Existing EventRuntime claim/ack behavior remains
// authoritative. No WorkerTools are attached: long run and filesystem actions
// are unavailable. Review is only a scheduling class, not a review verdict.
class AsyncCoordinator final {
public:
    AsyncCoordinator(std::unique_ptr<Store> store,
                     std::shared_ptr<const SchemaRegistry> schemas,
                     AsyncCoordinatorLimits limits = {});
    ~AsyncCoordinator();
    AsyncCoordinator(const AsyncCoordinator&) = delete;
    AsyncCoordinator& operator=(const AsyncCoordinator&) = delete;

    // Host-thread only. Borrowed access must not escape the coordinator lifetime
    // or be used concurrently. Actors/calls can be captured here before queueing;
    // submit preserves them exactly, including their original writer session.
    Store& store();
    CapturedModelCall capture(const std::string& request_id, Role role,
                              const std::string& task_id = {},
                              std::int64_t call_index = 1,
                              bool model_amendment = false);
    std::future<Json> submit(CapturedModelCall captured,
                            std::shared_ptr<const ModelAdapter> adapter,
                            WorkClass work_class = WorkClass::Normal,
                            std::string resource_key = {});

    // Thread-safe bounded controls. Caller supplies the previously captured
    // Root and CAS. The host never refreshes them to make a stale command work.
    std::future<Json> cancel(StoreActor root, std::int64_t expected_version,
                             std::string command_id);
    std::future<Json> amend(StoreActor root, std::int64_t expected_version,
                            std::string original_text, Json constraint_catalog,
                            std::string command_id);

    // Each pass services bounded controls, first-execution claims and ready
    // results in host order, without waiting for model completion.
    std::size_t poll();
    void drain();
    AsyncCoordinatorSnapshot snapshot() const;

    // A failed persistence attempt permanently fences this coordinator's new
    // business. Original responses retain their admission/result slots. This
    // explicit retry only stores original evidence; it never reapplies tools.
    Json retained_results() const;
    void flush_retained_evidence();
    // Stop admission/callbacks and join before releasing Store ownership.
    // Late results are recorded without business application during close.
    // Storage failure throws and leaves evidence available for export/retry.
    void close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wls::s1
