#include "wls/persistent_event_runtime.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;
using ActorKey = std::pair<Role, std::string>;

void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
std::string text(const Json& value, std::string_view field) {
    return std::string(value.at(field).as_string());
}
std::int64_t number(const Json& value, std::string_view field) {
    const auto& item = value.at(field);
    if (item.is_int64()) return item.as_int64();
    require(item.is_uint64() && item.as_uint64() <= static_cast<std::uint64_t>(INT64_MAX),
            ErrorCode::Corrupt, "runtime integer is not bounded");
    return static_cast<std::int64_t>(item.as_uint64());
}
bool terminal_request(const Json& request) {
    const auto status = text(request, "status");
    return status == "DELIVERED" || status == "FAILED" || status == "CANCELLED" || status == "SUPERSEDED";
}
bool terminal_task(const Json& task) {
    const auto status = text(task, "status");
    return status == "ACCEPTED" || status == "CANCELLED" || status == "FAILED" ||
        status == "SKIPPED" || status == "SUPERSEDED";
}
bool instruction_current(const Json& instruction, const ExecutionBinding& binding) {
    return !instruction.is_null() && text(instruction, "team_id") == binding.team_id &&
        number(instruction, "based_on_requirements_revision") == binding.requirements_revision &&
        number(instruction, "task_revision") == binding.task_revision &&
        number(instruction, "plan_revision") == binding.plan_revision &&
        number(instruction, "instruction_revision") == binding.instruction_revision;
}
AsyncCoordinatorLimits checked_limits(AsyncCoordinatorLimits limits) {
    require(limits.business_capacity <= 64 && limits.control_capacity <= 16 &&
            limits.management_workers <= 4 && limits.worker_workers <= 8,
            ErrorCode::InvalidInput, "persistent event limits exceed original protocol ceilings");
    return limits;
}
void validate_script(const EventFakeScript& script) {
    require(script.by_node.size() <= 64, ErrorCode::InvalidInput, "too many event script groups");
    if (!script.default_steps.empty()) (void)FakeModel(script.default_steps);
    const auto alphanumeric = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    for (const auto& [node, steps] : script.by_node) {
        require(!node.empty() && node.size() <= 48 && alphanumeric(node.front()) &&
                std::all_of(node.begin(), node.end(), [&](char c) { return alphanumeric(c) || c == '-'; }),
                ErrorCode::InvalidInput, "invalid persistent event script node");
        if (!steps.empty()) (void)FakeModel(steps);
    }
}
} // namespace

class PersistentEventRuntime::Impl {
public:
    struct Call {
        Role role;
        std::string node;
        std::string task;
        EventCallSubmission submission;
        bool observed{};
        std::string error;
    };
    struct Batch {
        std::string id;
        EventFakeScript script;
        EventPumpOptions options;
        PersistentEventAdapterFactory factory;
        std::int64_t manager_limit{};
        std::int64_t manager_calls{};
        std::map<ActorKey, std::int64_t> indices;
        std::set<ActorKey> stopped;
        std::set<std::string> block_keys;
        Array blocked;
        std::vector<Call> calls;
    };

    Impl(std::unique_ptr<Store> store, std::shared_ptr<const SchemaRegistry> schemas,
         AsyncCoordinatorLimits limits)
        : coordinator(std::move(store), std::move(schemas), checked_limits(limits)),
          normal_capacity(limits.business_capacity - limits.management_reserved - limits.worker_reserved) {
        batches.reserve(64);
    }

    void add(std::string id, EventFakeScript script, EventPumpOptions options,
             PersistentEventAdapterFactory factory) {
        auto& store = coordinator.store(); // Checks the original owner thread.
        require(coordinator.snapshot().accepting, ErrorCode::NotReady, "persistent runtime is not accepting work");
        require(batches.size() < 64, ErrorCode::Quota, "persistent runtime request batch limit reached");
        require(std::none_of(batches.begin(), batches.end(), [&](const auto& batch) { return batch.id == id; }),
                ErrorCode::Conflict, "request already has an explicit batch in this runtime");
        require(options.max_calls >= 1 && options.max_calls <= 64 && options.max_worker_calls_per_task >= 1 &&
                options.max_worker_calls_per_task <= 16, ErrorCode::InvalidInput, "invalid finite event call bounds");
        require(options.amendment_dispositions.is_object() && options.amendment_dispositions.as_object().empty(),
                ErrorCode::Unsupported, "persistent batch uses captured model AMEND decisions");
        require(store.persistent_events_enabled(id), ErrorCode::Unsupported,
                "persistent request mode must be explicitly enabled before its batch");
        validate_script(script);
        const auto config = restore_request_config(store.request(id));
        Batch batch;
        batch.id = std::move(id);
        batch.script = std::move(script);
        batch.options = std::move(options);
        batch.factory = std::move(factory);
        batch.manager_limit = number(config.json().at("system").at("limits"), "manager_max_tool_calls_per_wake");
        require(batch.manager_limit > 0, ErrorCode::Corrupt, "frozen manager call limit is invalid");
        batch.calls.reserve(static_cast<std::size_t>(batch.options.max_calls));
        batches.push_back(std::move(batch));
    }

    void block(Batch& batch, Role role, const std::string& node, const std::string& task,
               std::string_view code, const std::string& message) {
        const auto key = std::string(role_name(role)) + ":" + node + ":" + std::string(code);
        if (batch.block_keys.insert(key).second)
            batch.blocked.push_back(Object{{"role", role_name(role)}, {"node_id", node}, {"task_id", task},
                {"code", code}, {"reason", message}});
    }

    bool active(const Batch& batch, const ActorKey& key) const {
        return std::any_of(batch.calls.begin(), batch.calls.end(), [&](const auto& call) {
            return !call.observed && call.role == key.first && call.node == key.second;
        });
    }

    bool invoke(Batch& batch, Role role, const std::string& node, const std::string& task) {
        const ActorKey key{role, node};
        if (batch.stopped.contains(key) || active(batch, key) ||
                coordinator.snapshot().outstanding >= normal_capacity) return false;
        auto& index = batch.indices[key]; // Own all per-call bookkeeping before T0.
        if (batch.calls.size() >= static_cast<std::size_t>(batch.options.max_calls)) {
            block(batch, role, node, task, "call_limit", "explicit batch call limit reached");
            return false;
        }
        if ((role == Role::Worker && index >= batch.options.max_worker_calls_per_task) ||
                (role != Role::Worker && batch.manager_calls >= batch.manager_limit)) {
            block(batch, role, node, task, role == Role::Worker ? "worker_limit" : "manager_limit",
                "original explicit or frozen role limit reached");
            return false;
        }
        const auto found = batch.script.by_node.find(node);
        const auto& steps = node.empty() || found == batch.script.by_node.end()
            ? batch.script.default_steps : found->second;
        if (std::none_of(steps.begin(), steps.end(), [&](const auto& step) {
                return step.role == role && step.role_call_index == index + 1;
            })) {
            batch.stopped.insert(key);
            block(batch, role, node, task, "missing_script", "no explicit Fake step for this role/node sequence");
            return false;
        }
        // Missing scripts, unsupported targets and adapter validation fail
        // before T0. The coordinator owns the result slot before it claims C.
        auto captured = coordinator.capture(batch.id, role, task, index + 1, role == Role::Dispatcher);
        require(captured.workflow_action.is_null() || (role == Role::Dispatcher &&
                text(captured.workflow_action, "kind") == "amendment"), ErrorCode::Unsupported,
                "persistent first slice does not support this captured workflow target");
        auto adapter = batch.factory ? batch.factory(captured, steps) :
            std::make_shared<FakeModel>(steps, captured.request.model.model);
        Call call{role, node, task, {}, false, {}};
        call.submission.attempt_id = captured.request.attempt_id;
        batch.calls.push_back(std::move(call));
        try {
            batch.calls.back().submission = coordinator.submit_event(std::move(captured), std::move(adapter));
        } catch (...) {
            if (!coordinator.snapshot().settlement_halted) batch.calls.pop_back();
            throw;
        }
        ++index;
        if (role != Role::Worker) ++batch.manager_calls;
        return true;
    }

    std::size_t observe() {
        std::size_t progressed = 0;
        for (auto& batch : batches) for (auto& call : batch.calls) {
            auto& future = call.submission.completion;
            if (call.observed || !future.valid() ||
                    future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            call.observed = true;
            ++progressed;
            try {
                (void)future.get(); // Scheduling completion only, never ID proof.
            } catch (const Error& error) {
                call.error = std::string(error_name(error.code()));
                batch.stopped.insert({call.role, call.node});
                block(batch, call.role, call.node, call.task, call.error, error.what());
            } catch (const std::exception& error) {
                call.error = "host_error";
                batch.stopped.insert({call.role, call.node});
                block(batch, call.role, call.node, call.task, call.error, error.what());
            }
            const auto durable = coordinator.store().event_link_status(call.submission.link_id);
            if (text(durable, "state") != "CLOSED") {
                batch.stopped.insert({call.role, call.node});
                block(batch, call.role, call.node, call.task, "original_call_held",
                    "original durable link remains unresolved; no automatic retry");
            }
        }
        return progressed;
    }

    bool advance(Batch& batch) {
        auto& store = coordinator.store();
        const auto request = store.request(batch.id);
        if (terminal_request(request)) {
            block(batch, Role::Dispatcher, {}, {}, "terminal", "terminal request retains only original evidence");
            return false;
        }
        if (store.capacity_status(batch.id).at("status") == "WAITING_INPUT") {
            block(batch, Role::Dispatcher, {}, {}, "capacity_waiting_input", "original capacity checkpoint is required");
            return false;
        }
        const auto plan = store.plan(batch.id);
        if (plan.is_null() || number(plan, "based_on_requirements_revision") != number(request, "requirements_revision") ||
                !next_amendment_action(store, batch.id).is_null()) {
            try {
                if (invoke(batch, Role::Dispatcher, {}, {})) return true;
            } catch (const Error& error) {
                batch.stopped.insert({Role::Dispatcher, {}});
                block(batch, Role::Dispatcher, {}, {}, error_name(error.code()), error.what());
            }
        }
        for (const auto& task : store.tasks(batch.id)) {
            if (!coordinator.snapshot().accepting) break;
            if (terminal_task(task)) continue;
            const auto task_id = text(task, "task_id"), node = text(task, "node_id");
            if (number(task, "based_on_requirements_revision") != number(request, "requirements_revision") ||
                    number(task, "plan_revision") != number(request, "plan_revision")) continue;
            Role role = Role::TeamLead;
            try {
                const auto waiting = store.worker_wait(task_id);
                if (!waiting.is_null() && text(waiting, "status") == "WAITING") {
                    block(batch, Role::Worker, node, task_id, "waiting", "original Worker wait is still active");
                    continue;
                }
                if (!store.current_submission(task_id).is_null()) {
                    block(batch, Role::TeamLead, node, task_id, "unsupported_workflow", "submission workflow requires its separate original proof");
                    continue;
                }
                const auto binding = store.binding(task_id);
                if (!instruction_current(store.instruction(task_id), binding)) {
                    if (invoke(batch, role, node, task_id)) return true;
                    continue;
                }
                // Epoch-only recovery does not issue a new interpretation of
                // an already current order just to consume a TeamLead notice.
                role = Role::Worker;
                if (active(batch, {role, node}) || batch.stopped.contains({role, node})) continue;
                const auto status = text(task, "status");
                if (status != "READY" && status != "RUNNING") continue;
                const auto actor = store.host_actor(role, batch.id, binding.team_id);
                if (number(task, "applied_task_revision") != binding.task_revision ||
                        number(task, "applied_plan_revision") != binding.plan_revision ||
                        number(task, "applied_instruction_revision") != binding.instruction_revision)
                    (void)store.acknowledge(actor, binding, number(task, "state_version"), new_id("ack"));
                if (invoke(batch, role, node, task_id)) return true;
            } catch (const Error& error) {
                batch.stopped.insert({role, node});
                block(batch, role, node, task_id, error_name(error.code()), error.what());
            }
        }
        return false;
    }

    std::size_t poll() {
        std::size_t progressed = coordinator.poll();
        progressed += observe();
        if (coordinator.snapshot().accepting)
            for (auto& batch : batches) {
                if (!coordinator.snapshot().accepting) break;
                if (advance(batch)) ++progressed;
            }
        return progressed;
    }

    Json report(const std::string& id) {
        auto& store = coordinator.store();
        const auto found = std::find_if(batches.begin(), batches.end(), [&](const auto& batch) { return batch.id == id; });
        require(found != batches.end(), ErrorCode::InvalidInput, "request has no explicit persistent batch");
        const auto& batch = *found;
        Array calls, links, pending;
        for (const auto& call : batch.calls)
            calls.push_back(Object{{"role", role_name(call.role)}, {"node_id", call.node}, {"task_id", call.task},
                {"attempt_id", call.submission.attempt_id}, {"link_id", call.submission.link_id},
                {"completion_observed", call.observed}, {"error", call.error}});
        // Read actual Store proof for all links, including original recovered
        // HOLDs that this host never dispatched. No guessed finished-ID set.
        for (const auto& link : store.request_event_links(id)) links.push_back(link);
        for (const auto& item : store.request_inbox(id)) pending.push_back(item);
        const auto state = coordinator.snapshot();
        return Object{{"mode", "persistent_event_fake"}, {"request_id", id}, {"calls", std::move(calls)},
            {"links", std::move(links)}, {"pending", std::move(pending)}, {"blocked", batch.blocked},
            {"budget", store.budget(id)}, {"request_status", store.request(id).at("status")},
            {"manager_calls", batch.manager_calls}, {"max_calls", batch.options.max_calls},
            {"max_worker_calls_per_task", batch.options.max_worker_calls_per_task},
            {"accepting", state.accepting}, {"settlement_halted", state.settlement_halted},
            {"real_model_calls", 0}, {"worker_tools_enabled", false}, {"business_delivered", false}};
    }

    AsyncCoordinator coordinator;
    const std::size_t normal_capacity;
    std::vector<Batch> batches;
};

PersistentEventRuntime::PersistentEventRuntime(std::unique_ptr<Store> store,
        std::shared_ptr<const SchemaRegistry> schemas, AsyncCoordinatorLimits limits)
    : impl_(std::make_unique<Impl>(std::move(store), std::move(schemas), limits)) {}
PersistentEventRuntime::~PersistentEventRuntime() = default;
Store& PersistentEventRuntime::store() { return impl_->coordinator.store(); }
void PersistentEventRuntime::add_request(std::string id, EventFakeScript script, EventPumpOptions options,
        PersistentEventAdapterFactory factory) {
    impl_->add(std::move(id), std::move(script), std::move(options), std::move(factory));
}
std::size_t PersistentEventRuntime::poll() { return impl_->poll(); }
Json PersistentEventRuntime::report(const std::string& id) { return impl_->report(id); }
AsyncCoordinatorSnapshot PersistentEventRuntime::snapshot() const { return impl_->coordinator.snapshot(); }
std::future<Json> PersistentEventRuntime::cancel(StoreActor root, std::int64_t version, std::string command) {
    return impl_->coordinator.cancel(std::move(root), version, std::move(command));
}
std::future<Json> PersistentEventRuntime::amend(StoreActor root, std::int64_t version,
        std::string original, Json constraints, std::string command) {
    return impl_->coordinator.amend(std::move(root), version, std::move(original), std::move(constraints), std::move(command));
}
Json PersistentEventRuntime::retained_results() const { return impl_->coordinator.retained_results(); }
void PersistentEventRuntime::flush_retained_evidence() { impl_->coordinator.flush_retained_evidence(); }
void PersistentEventRuntime::close() { impl_->coordinator.close(); }

} // namespace wls::s1
