#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace wls {

enum class WorkClass { Normal, Review, Handoff };

enum class SchedulerErrorCode {
    InvalidConfiguration,
    InvalidJob,
    Closed,
    DuplicateJob,
    CapacityFull,
    NormalCapacityReserved,
    Cancelled,
    InvalidShutdown
};

class SchedulerError : public std::runtime_error {
public:
    SchedulerError(SchedulerErrorCode code, const std::string& message);
    SchedulerErrorCode code() const noexcept { return code_; }
private:
    SchedulerErrorCode code_;
};

struct SchedulerSnapshot {
    std::size_t workers{};
    std::size_t capacity{};
    std::size_t reserved{};
    std::size_t outstanding{};
    std::size_t queued{};
    std::size_t running{};
    std::size_t normal_outstanding{};
    std::size_t normal_running{};
    bool accepting{};
};

class Scheduler {
public:
    using Snapshot = SchedulerSnapshot;

    // capacity bounds queued + running jobs. reserved protects BOTH admission
    // capacity and execution threads for Review/Handoff (one shared reserve).
    // Requires 0 < reserved < workers and reserved < capacity.
    Scheduler(std::size_t workers, std::size_t capacity,
              std::size_t reserved = 1);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // IDs/keys/callback must be nonempty. IDs are unique among outstanding
    // jobs. A rejection throws SchedulerError without accepting the callback.
    // Callbacks sharing a key execute serially, in submission order.
    // Callbacks must return before waiting for dependent jobs on this pool;
    // the host chains futures. Reserved slots do not break dependency cycles.
    std::future<void> submit(std::string job_id, std::string resource_key,
                             WorkClass work_class, std::function<void()> work);

    // Opt-in round robin by request, then team, within each work class.
    // Resource FIFO and permanent Review/Handoff reserves remain unchanged.
    // Newly admitted groups join the tail; blocked groups consume no turn.
    std::future<void> submit_fair(std::string job_id, std::string resource_key,
                                std::string request_key, std::string team_key,
                                WorkClass work_class, std::function<void()> work);

    // Stop admission and join all threads. drain=false cancels queued jobs
    // through their futures; already running callbacks still finish. drain=true
    // runs all accepted jobs. The first concurrent close call chooses the mode.
    // Blocking close/destruction must be owned by a non-worker host thread.
    void close(bool drain = true);
    Snapshot snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wls
