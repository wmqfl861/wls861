#include "wls/scheduler.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wls {
namespace {

thread_local const void* current_scheduler = nullptr;

std::size_t class_index(WorkClass work_class) {
    switch (work_class) {
    case WorkClass::Normal: return 0;
    case WorkClass::Review: return 1;
    case WorkClass::Handoff: return 2;
    }
    throw SchedulerError(SchedulerErrorCode::InvalidJob, "unknown work class");
}

} // namespace

SchedulerError::SchedulerError(SchedulerErrorCode code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

class Scheduler::Impl {
public:
    struct Job {
        std::string id;
        std::string resource;
        std::size_t work_class;
        std::function<void()> callback;
        std::promise<void> result;
    };
    using Queue = std::deque<std::unique_ptr<Job>>;

    Impl(std::size_t workers, std::size_t capacity, std::size_t reserved)
        : workers_(workers), capacity_(capacity), reserved_(reserved) {
        if (reserved == 0 || reserved >= workers || reserved >= capacity) {
            throw SchedulerError(SchedulerErrorCode::InvalidConfiguration,
                "scheduler requires 0 < reserved < workers and reserved < capacity");
        }
        running_jobs_.resize(workers, nullptr);
        threads_.reserve(workers);
        try {
            for (std::size_t index = 0; index < workers; ++index) {
                threads_.emplace_back([this, index] { run(index); });
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                accepting_ = false;
            }
            changed_.notify_all();
            for (auto& thread : threads_) thread.join();
            throw;
        }
    }

    std::future<void> submit(std::string id, std::string resource,
                             WorkClass work_class, std::function<void()> callback) {
        const auto index = class_index(work_class);
        if (id.empty() || resource.empty() || !callback) {
            throw SchedulerError(SchedulerErrorCode::InvalidJob,
                "job ID, resource key and callback must be nonempty");
        }
        auto job = std::make_unique<Job>(Job{
            std::move(id), std::move(resource), index, std::move(callback), {}});
        auto future = job->result.get_future();
        {
            std::lock_guard lock(mutex_);
            if (!accepting_) {
                throw SchedulerError(SchedulerErrorCode::Closed, "scheduler is closed");
            }
            if (ids_.contains(job->id)) {
                throw SchedulerError(SchedulerErrorCode::DuplicateJob,
                    "job ID is already outstanding");
            }
            if (ids_.size() >= capacity_) {
                throw SchedulerError(SchedulerErrorCode::CapacityFull,
                    "scheduler outstanding capacity is full");
            }
            if (index == 0 && normal_outstanding_ >= capacity_ - reserved_) {
                throw SchedulerError(SchedulerErrorCode::NormalCapacityReserved,
                    "remaining capacity is reserved for review/handoff");
            }
            const auto inserted = ids_.insert(job->id).first;
            try {
                pending_.push_back(std::move(job));
            } catch (...) {
                ids_.erase(inserted);
                throw;
            }
            if (index == 0) ++normal_outstanding_;
        }
        changed_.notify_all();
        return future;
    }

    void close(bool drain) {
        if (current_scheduler == this) {
            throw SchedulerError(SchedulerErrorCode::InvalidShutdown,
                "a scheduler worker cannot join its own scheduler");
        }
        // Serializes join, without holding the queue mutex across callbacks.
        std::lock_guard shutdown_lock(shutdown_mutex_);
        Queue cancelled;
        {
            std::lock_guard lock(mutex_);
            accepting_ = false;
            if (!drain) {
                cancelled.swap(pending_);
                for (const auto& job : cancelled) {
                    ids_.erase(job->id);
                    if (job->work_class == 0) --normal_outstanding_;
                }
            }
        }
        for (auto& job : cancelled) {
            job->result.set_exception(std::make_exception_ptr(
                SchedulerError(SchedulerErrorCode::Cancelled,
                               "queued job cancelled during scheduler close")));
        }
        changed_.notify_all();
        for (auto& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
    }

    SchedulerSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return {workers_, capacity_, reserved_, ids_.size(), pending_.size(),
            running_count_, normal_outstanding_, running_by_class_[0], accepting_};
    }

private:
    bool resource_available(Queue::const_iterator candidate) const {
        const auto& key = (*candidate)->resource;
        for (const Job* running : running_jobs_) {
            if (running && running->resource == key) return false;
        }
        // Preserve FIFO for each resource across all classes. This prevents a
        // stream of newly admitted jobs from overtaking an older blocked key.
        return std::none_of(pending_.cbegin(), candidate,
            [&key](const auto& earlier) { return earlier->resource == key; });
    }

    Queue::iterator find_runnable() {
        // Round robin between runnable classes; priority work cannot starve
        // Normal, nor can Review continuously bypass Handoff (or vice versa).
        for (std::size_t offset = 0; offset < 3; ++offset) {
            const auto wanted = (next_class_ + offset) % 3;
            if (wanted == 0 && running_by_class_[0] >= workers_ - reserved_) continue;
            for (auto candidate = pending_.begin(); candidate != pending_.end(); ++candidate) {
                if ((*candidate)->work_class == wanted && resource_available(candidate)) {
                    return candidate;
                }
            }
        }
        return pending_.end();
    }

    void run(std::size_t worker_index) {
        current_scheduler = this;
        while (true) {
            std::unique_ptr<Job> job;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [this] {
                    return (!accepting_ && pending_.empty()) ||
                        find_runnable() != pending_.end();
                });
                if (!accepting_ && pending_.empty()) break;
                const auto candidate = find_runnable();
                job = std::move(*candidate);
                pending_.erase(candidate);
                next_class_ = (job->work_class + 1) % 3;
                running_jobs_[worker_index] = job.get();
                ++running_by_class_[job->work_class];
                ++running_count_;
            }

            std::exception_ptr failure;
            try { job->callback(); }
            catch (...) { failure = std::current_exception(); }

            {
                std::lock_guard lock(mutex_);
                running_jobs_[worker_index] = nullptr;
                --running_by_class_[job->work_class];
                --running_count_;
                ids_.erase(job->id);
                if (job->work_class == 0) --normal_outstanding_;
            }
            // Bookkeeping commits before the future is ready; callback and
            // promise completion both occur outside the queue mutex.
            if (failure) job->result.set_exception(failure);
            else job->result.set_value();
            changed_.notify_all();
        }
        current_scheduler = nullptr;
    }

    const std::size_t workers_;
    const std::size_t capacity_;
    const std::size_t reserved_;
    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable changed_;
    bool accepting_{true};
    Queue pending_;
    std::unordered_set<std::string> ids_;
    std::vector<Job*> running_jobs_;
    std::array<std::size_t, 3> running_by_class_{};
    std::size_t running_count_{};
    std::size_t normal_outstanding_{};
    std::size_t next_class_{};
    std::vector<std::thread> threads_;
};

Scheduler::Scheduler(std::size_t workers, std::size_t capacity, std::size_t reserved)
    : impl_(std::make_unique<Impl>(workers, capacity, reserved)) {}

Scheduler::~Scheduler() { impl_->close(true); }

std::future<void> Scheduler::submit(std::string job_id, std::string resource_key,
                                    WorkClass work_class, std::function<void()> work) {
    return impl_->submit(std::move(job_id), std::move(resource_key),
                          work_class, std::move(work));
}

void Scheduler::close(bool drain) { impl_->close(drain); }

Scheduler::Snapshot Scheduler::snapshot() const { return impl_->snapshot(); }

} // namespace wls
