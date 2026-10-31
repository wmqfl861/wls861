#include "wls/async_coordinator.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}

ModelResponse host_failure(const ModelRequest& original, std::string code) {
    ModelResponse response;
    response.call_id = original.call_id;
    response.attempt_id = original.attempt_id;
    response.role_call_index = original.role_call_index;
    response.role = original.role;
    response.binding = original.binding;
    response.config_snapshot_id = original.config_snapshot_id;
    response.status = ModelResultStatus::Interrupted;
    response.error_code = std::move(code);
    response.error_message = "Offline host did not obtain a complete model response; usage remains unknown.";
    return response;
}

const char* outcome(const ModelResponse& response) {
    switch (response.status) {
    case ModelResultStatus::Completed: return "COMPLETED";
    case ModelResultStatus::Error: return "ERROR";
    case ModelResultStatus::Interrupted: return "INTERRUPTED";
    }
    throw Error(ErrorCode::InvalidInput, "invalid model response status");
}
} // namespace

class AsyncCoordinator::Impl {
public:
    enum class Phase { Queued, AwaitingClaim, Executing, Ready, Retained, Done };
    struct Job {
        CapturedModelCall captured;
        std::shared_ptr<const ModelAdapter> adapter;
        WorkClass work_class;
        std::string resource;
        Phase phase{Phase::Queued};
        bool permitted{};
        bool replay{};
        bool invoked{};
        std::stop_source stop;
        std::optional<ModelResponse> response;
        std::exception_ptr failure;
        std::promise<Json> result;
        std::future<void> execution;
    };
    struct Control {
        StoreActor root;
        std::int64_t version;
        std::string command;
        std::optional<std::string> amendment;
        Json constraints;
        std::promise<Json> result;
    };

    Impl(std::unique_ptr<Store> value, std::shared_ptr<const SchemaRegistry> registry,
         AsyncCoordinatorLimits bounds)
        : store_(std::move(value)), schemas_(std::move(registry)), limits_(bounds),
          owner_(std::this_thread::get_id()), managers_(4, bounds.business_capacity, 1),
          workers_(8, bounds.business_capacity, 1) {
        require(store_ && schemas_, ErrorCode::InvalidInput, "coordinator needs an owned Store and schemas");
        require(bounds.business_capacity >= 2 && bounds.business_capacity <= 1024 &&
                bounds.control_capacity > 0 && bounds.control_capacity <= 256,
                ErrorCode::InvalidInput, "invalid bounded coordinator capacity");
        require(store_->workflow_runtime_enabled(), ErrorCode::Unsupported,
                "async coordinator requires durable S1 feedback and captured workflow CAS");
        jobs_.reserve(bounds.business_capacity);
    }

    ~Impl() {
        // close always joins first, even if evidence persistence subsequently
        // fails. Unpersisted bytes cannot survive a host crash: the original
        // PENDING reservation remains durable and recovery makes it UNKNOWN.
        try {
            if (std::this_thread::get_id() == owner_) close();
            else stop_and_join(); // No foreign-thread Store access, even on destruction.
        } catch (...) {}
    }

    void owner() const {
        require(std::this_thread::get_id() == owner_, ErrorCode::Unauthorized,
                "Store coordinator operations belong to their constructing host thread");
    }

    CapturedModelCall capture(const std::string& id, Role role, const std::string& task,
                              std::int64_t index, bool amendment) {
        owner();
        require(!closed_ && !halted_, ErrorCode::NotReady, "coordinator is closed or settlement-fenced");
        return OfflineRuntime(*store_, *schemas_).capture_call(id, role, task, index, {}, amendment);
    }

    std::future<Json> submit(CapturedModelCall captured,
                            std::shared_ptr<const ModelAdapter> adapter,
                            WorkClass work_class, std::string resource) {
        owner();
        require(!closed_ && !halted_, ErrorCode::NotReady, "coordinator is closed or settlement-fenced");
        require(adapter && !adapter->capabilities().real_calls, ErrorCode::Unsupported,
                "async coordinator only accepts offline adapters");
        require(work_class == WorkClass::Normal || work_class == WorkClass::Review ||
                work_class == WorkClass::Handoff, ErrorCode::InvalidInput, "unknown work class");
        require(jobs_.size() < limits_.business_capacity &&
                (work_class != WorkClass::Normal || normal_ < limits_.business_capacity - 1),
                ErrorCode::Quota, "business queue is full or its remaining admission is reserved");
        for (const auto& job : jobs_)
            require(job->captured.request.attempt_id != captured.request.attempt_id,
                    ErrorCode::Conflict, "attempt is already owned by this coordinator");
        // This vertical slice intentionally has no long-running WorkerTools.
        // A crafted captured context cannot smuggle one into the host result path.
        for (const auto& tool : captured.request.context.tool_names)
            require(tool != "run" && !tool.starts_with("fs_") && tool != "submit",
                    ErrorCode::Unsupported, "async call slice has no file/process executor");
        if (resource.empty()) resource = captured.request.binding.team_id.empty()
            ? captured.request.binding.request_id : captured.request.binding.team_id;
        require(resource.size() <= 256 && resource.find('\0') == std::string::npos,
                ErrorCode::InvalidInput, "invalid scheduler resource key");
        auto job = std::make_shared<Job>();
        job->captured = std::move(captured);
        job->adapter = std::move(adapter);
        job->work_class = work_class;
        job->resource = std::move(resource);
        auto future = job->result.get_future();
        // All memory/bookkeeping required to retain one result is owned before
        // reservation. A later scheduler rejection retains the original attempt.
        jobs_.push_back(job);
        try {
            OfflineRuntime(*store_, *schemas_).reserve_captured_call(*job->adapter, job->captured);
        } catch (...) {
            jobs_.pop_back();
            throw;
        }
        if (work_class == WorkClass::Normal) ++normal_;
        try {
            auto& pool = job->captured.request.role == Role::Worker ? workers_ : managers_;
            job->execution = pool.submit(job->captured.request.attempt_id, job->resource,
                work_class, [this, job] {
                    try { execute(job); }
                    catch (...) {
                        // No allocation is needed to expose an escaping
                        // callback failure to the host. Its pending durable
                        // attempt remains recoverable even without response bytes.
                        std::lock_guard lock(mutex_);
                        job->failure = std::current_exception();
                        job->phase = Phase::Ready;
                        ++generation_;
                        changed_.notify_all();
                    }
                });
        } catch (...) {
            job->failure = std::current_exception();
            job->phase = Phase::Ready;
        }
        return future;
    }

    void execute(const std::shared_ptr<Job>& job) {
        bool run = false;
        {
            std::unique_lock lock(mutex_);
            if (!closing_) {
                job->phase = Phase::AwaitingClaim;
                ++generation_;
                changed_.notify_all();
                changed_.wait(lock, [&] { return closing_ || job->phase != Phase::AwaitingClaim; });
                run = job->permitted && !closing_;
            }
        }
        std::optional<ModelResponse> response;
        std::exception_ptr failure;
        try {
            if (run) {
                job->invoked = true;
                response = job->adapter->complete(job->captured.request, job->stop.get_token());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        {
            std::lock_guard lock(mutex_);
            job->response = std::move(response);
            if (failure) job->failure = failure;
            job->phase = Phase::Ready;
            ++generation_;
        }
        changed_.notify_all();
    }

    std::future<Json> control(StoreActor root, std::int64_t version, std::string command,
                              std::optional<std::string> amendment, Json constraints) {
        require(root.role == Role::Root && !command.empty() && command.size() <= 256,
                ErrorCode::InvalidInput, "control requires a captured Root and bounded command ID");
        require(!amendment || amendment->size() <= 1048576, ErrorCode::Quota,
                "control amendment exceeds its byte limit");
        require(canonical_json(constraints).size() <= 1048576, ErrorCode::Quota,
                "control constraints exceed their byte limit");
        auto item = std::make_unique<Control>(Control{std::move(root), version,
            std::move(command), std::move(amendment), std::move(constraints), {}});
        auto future = item->result.get_future();
        {
            std::lock_guard lock(mutex_);
            require(!closing_, ErrorCode::NotReady, "coordinator is closing");
            require(control_count_ < limits_.control_capacity, ErrorCode::Quota,
                    "reserved control queue is full");
            controls_.push_back(std::move(item));
            ++control_count_;
            ++generation_;
        }
        changed_.notify_all();
        return future;
    }

    void process_control(Control& item) {
        try {
            auto result = item.amendment
                ? store_->amend(item.root, item.version, *item.amendment, item.constraints, item.command)
                : store_->cancel_request(item.root, item.version, item.command);
            for (const auto& job : jobs_)
                if (job->captured.actor.request_id == item.root.request_id) job->stop.request_stop();
            item.result.set_value(std::move(result));
        } catch (...) { item.result.set_exception(std::current_exception()); }
    }

    // Only observation/accounting is retried after persistence failure or close.
    // In particular, a settlement replay is never an application retry.
    void evidence_only(Job& job) {
        require(job.response.has_value(), ErrorCode::UnknownOutcome,
                "no original response was retained; pending attempt requires recovery");
        const auto& call = job.captured;
        const auto& response = *job.response;
        require(response.call_id == call.request.call_id && response.attempt_id == call.request.attempt_id,
                ErrorCode::Conflict, "returned response changed its original attempt identity");
        (void)store_->settle_attempt(call.request.attempt_id, response.binding,
            response.config_snapshot_id, outcome(response), to_json(response.usage), to_json(response));
        if (store_->tool_feedback(call.request.attempt_id).is_null())
            (void)store_->record_tool_feedback(call.request.attempt_id, call.request.binding, to_json(response),
                Object{{"status", "ERROR"}, {"runtime_error", true}, {"error_code", "not_ready"},
                    {"message", "Coordinator retained original evidence without retrying business application."},
                    {"observation_complete", false}});
    }

    void finish(Job& job, bool only_evidence) {
        if (job.replay) {
            job.result.set_exception(job.failure);
            job.phase = Phase::Done;
            return;
        }
        try {
            require(job.response.has_value(), ErrorCode::UnknownOutcome,
                    "original response is unavailable after host failure");
            Json result;
            if (only_evidence) {
                evidence_only(job);
                if (!job.failure) job.failure = std::make_exception_ptr(
                    Error(ErrorCode::NotReady, "response retained without business application"));
            } else {
                result = OfflineRuntime(*store_, *schemas_).accept_independent_response(job.captured, *job.response);
            }
            if (job.failure) job.result.set_exception(job.failure);
            else job.result.set_value(std::move(result));
            job.phase = Phase::Done;
        } catch (const Error& error) {
            if (error.code() == ErrorCode::Storage || error.code() == ErrorCode::Corrupt || only_evidence) {
                retain(job, std::current_exception());
            } else {
                // A rejected envelope may never have reached settlement.
                // Keep the response/slot if there is no durable original yet.
                try {
                    if (store_->attempt(job.captured.request.attempt_id).at("settlement_sha256").is_null() ||
                        store_->tool_feedback(job.captured.request.attempt_id).is_null()) {
                        retain(job, std::current_exception());
                        return;
                    }
                } catch (...) { retain(job, std::current_exception()); return; }
                job.result.set_exception(job.failure ? job.failure : std::current_exception());
                job.phase = Phase::Done;
            }
        } catch (...) { retain(job, std::current_exception()); }
    }

    void retain(Job& job, std::exception_ptr failure) {
        if (!job.failure) job.failure = failure;
        job.phase = Phase::Retained;
        halted_ = true;
        for (const auto& other : jobs_) other->stop.request_stop();
    }

    std::size_t poll() {
        owner();
        std::deque<std::unique_ptr<Control>> batch;
        {
            std::lock_guard lock(mutex_);
            batch.swap(controls_);
        }
        std::size_t progressed = batch.size();
        for (const auto& item : batch) {
            process_control(*item);
            std::lock_guard lock(mutex_);
            --control_count_;
        }
        for (const auto& job : jobs_) {
            Phase phase;
            { std::lock_guard lock(mutex_); phase = job->phase; }
            if (phase != Phase::Ready && phase != Phase::Retained && phase != Phase::Done &&
                job->execution.valid() && job->execution.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                // Also observe the Scheduler's completion channel if a failure
                // escaped before the callback published Ready/notification.
                try {
                    job->execution.get();
                    // Ready may have raced with the first phase snapshot.
                    std::lock_guard lock(mutex_);
                    phase = job->phase;
                    if (phase != Phase::Ready)
                        throw Error(ErrorCode::UnknownOutcome, "callback exited without a retained result");
                } catch (...) {
                    retain(*job, std::current_exception());
                    ++progressed;
                    continue;
                }
            }
            if (phase == Phase::AwaitingClaim) {
                // Both M1 pools share this one host resource order. Preserve
                // FIFO through result commit, including manager/Worker pairs.
                bool earlier_resource = false;
                {
                    std::lock_guard lock(mutex_);
                    for (const auto& earlier : jobs_) {
                        if (earlier == job) break;
                        if (earlier->resource == job->resource && earlier->phase != Phase::Done) {
                            earlier_resource = true; break;
                        }
                    }
                }
                if (earlier_resource && !halted_ && !closed_) continue;
                bool permitted = false;
                if (!closed_ && !halted_ && !job->stop.stop_requested()) {
                    try {
                        const auto claim = store_->claim_offline_call(job->captured.actor,
                            job->captured.request.attempt_id);
                        permitted = claim.at("dispatch_claimed").as_bool();
                        job->replay = !permitted;
                        require(permitted, ErrorCode::Conflict, "original attempt has already been dispatched");
                    } catch (...) { job->failure = std::current_exception(); }
                }
                {
                    std::lock_guard lock(mutex_);
                    job->permitted = permitted;
                    job->phase = Phase::Executing;
                }
                changed_.notify_all();
                ++progressed;
            } else if (phase == Phase::Ready) {
                // Wait for pool bookkeeping to release its execution slot
                // before releasing this admission/result slot.
                try {
                    if (job->execution.valid()) job->execution.get();
                    if (!job->response)
                        job->response = host_failure(job->captured.request,
                            job->invoked ? "host_adapter_exception" : "host_not_dispatched");
                } catch (...) {
                    retain(*job, std::current_exception());
                    ++progressed;
                    continue;
                }
                finish(*job, closed_ || halted_);
                ++progressed;
            }
        }
        {
            std::lock_guard lock(mutex_);
            std::erase_if(jobs_, [&](const auto& job) {
                if (job->phase != Phase::Done) return false;
                if (job->work_class == WorkClass::Normal) --normal_;
                return true;
            });
        }
        return progressed;
    }

    void drain() {
        owner();
        for (;;) {
            std::uint64_t observed;
            { std::lock_guard lock(mutex_); observed = generation_; }
            (void)poll();
            std::unique_lock lock(mutex_);
            if (jobs_.empty() && controls_.empty()) return;
            if (std::all_of(jobs_.begin(), jobs_.end(), [](const auto& job) {
                    return job->phase == Phase::Retained; }) && controls_.empty())
                throw Error(ErrorCode::Storage, "original results retained after persistence failure; new business fenced");
            // A Scheduler future can become exceptional without a callback
            // notification. This bounded host heartbeat inspects that channel.
            (void)changed_.wait_for(lock, std::chrono::milliseconds(25), [&] { return generation_ != observed; });
        }
    }

    AsyncCoordinatorSnapshot snapshot() const {
        owner();
        std::lock_guard lock(mutex_);
        const auto retained = static_cast<std::size_t>(std::count_if(jobs_.begin(), jobs_.end(),
            [](const auto& job) { return job->phase == Phase::Retained; }));
        return {jobs_.size(), retained, control_count_, !closed_ && !halted_, halted_,
            managers_.snapshot(), workers_.snapshot()};
    }

    Json retained_results() const {
        owner();
        std::lock_guard lock(mutex_);
        Array result;
        for (const auto& job : jobs_) if (job->phase == Phase::Retained)
            result.push_back(Object{{"request", to_json(job->captured.request)},
                {"response", job->response ? to_json(*job->response) : Json(nullptr)},
                {"observation_complete", job->response.has_value()}, {"business_retry_allowed", false}});
        return result;
    }

    void flush() {
        owner();
        for (const auto& job : jobs_) {
            Phase phase;
            { std::lock_guard lock(mutex_); phase = job->phase; }
            if (phase == Phase::Retained) finish(*job, true);
        }
        (void)poll();
        std::lock_guard lock(mutex_);
        require(std::none_of(jobs_.begin(), jobs_.end(), [](const auto& job) {
            return job->phase == Phase::Retained;
        }), ErrorCode::Storage, "original result persistence is still blocked");
    }

    void stop_and_join() {
        closed_ = true;
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            ++generation_;
        }
        for (const auto& job : jobs_) job->stop.request_stop();
        changed_.notify_all();
        managers_.close(true);
        workers_.close(true);
    }

    void close() {
        owner();
        stop_and_join();
        (void)poll();
        require(std::none_of(jobs_.begin(), jobs_.end(), [](const auto& job) {
            return job->phase == Phase::Retained;
        }), ErrorCode::Storage, "shutdown retained an original unpersisted result");
    }

    std::unique_ptr<Store> store_;
    std::shared_ptr<const SchemaRegistry> schemas_;
    const AsyncCoordinatorLimits limits_;
    const std::thread::id owner_;
    Scheduler managers_;
    Scheduler workers_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t generation_{};
    std::deque<std::unique_ptr<Control>> controls_;
    std::vector<std::shared_ptr<Job>> jobs_;
    std::size_t normal_{};
    std::size_t control_count_{};
    bool closed_{};
    bool closing_{};
    bool halted_{};
};

AsyncCoordinator::AsyncCoordinator(std::unique_ptr<Store> store,
        std::shared_ptr<const SchemaRegistry> schemas, AsyncCoordinatorLimits limits)
    : impl_(std::make_unique<Impl>(std::move(store), std::move(schemas), limits)) {}
AsyncCoordinator::~AsyncCoordinator() = default;
Store& AsyncCoordinator::store() { impl_->owner(); return *impl_->store_; }
CapturedModelCall AsyncCoordinator::capture(const std::string& id, Role role, const std::string& task,
        std::int64_t index, bool amendment) { return impl_->capture(id, role, task, index, amendment); }
std::future<Json> AsyncCoordinator::submit(CapturedModelCall call,
        std::shared_ptr<const ModelAdapter> adapter, WorkClass work_class, std::string resource) {
    return impl_->submit(std::move(call), std::move(adapter), work_class, std::move(resource));
}
std::future<Json> AsyncCoordinator::cancel(StoreActor root, std::int64_t version, std::string command) {
    return impl_->control(std::move(root), version, std::move(command), std::nullopt, nullptr);
}
std::future<Json> AsyncCoordinator::amend(StoreActor root, std::int64_t version,
        std::string original, Json constraints, std::string command) {
    return impl_->control(std::move(root), version, std::move(command), std::move(original), std::move(constraints));
}
std::size_t AsyncCoordinator::poll() { return impl_->poll(); }
void AsyncCoordinator::drain() { impl_->drain(); }
AsyncCoordinatorSnapshot AsyncCoordinator::snapshot() const { return impl_->snapshot(); }
Json AsyncCoordinator::retained_results() const { return impl_->retained_results(); }
void AsyncCoordinator::flush_retained_evidence() { impl_->flush(); }
void AsyncCoordinator::close() { impl_->close(); }

} // namespace wls::s1
