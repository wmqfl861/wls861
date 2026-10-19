#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

void require(bool value, ErrorCode code, const std::string& message) {
    if (!value) throw Error(code, message);
}
std::string text(const Json& value, std::string_view key) {
    require(value.is_object() && value.as_object().contains(key) && value.at(key).is_string(),
            ErrorCode::Corrupt, "missing runtime string: " + std::string(key));
    return std::string(value.at(key).as_string());
}
std::int64_t number(const Json& value, std::string_view key) {
    require(value.is_object() && value.as_object().contains(key), ErrorCode::Corrupt,
            "missing runtime integer: " + std::string(key));
    const auto& result = value.at(key);
    if (result.is_int64()) return result.as_int64();
    if (result.is_uint64() && result.as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(result.as_uint64());
    throw Error(ErrorCode::Corrupt, "invalid runtime integer: " + std::string(key));
}
void node_name(std::string_view node) {
    const auto alphanumeric = [](char value) { return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9'); };
    require(!node.empty() && node.size() <= 48 && alphanumeric(node.front()) &&
            std::all_of(node.begin(), node.end(), [&](char value) { return alphanumeric(value) || value == '-'; }),
            ErrorCode::InvalidInput, "invalid event script or workspace node ID");
}
std::vector<FakeModelStep> group(const Json& value) {
    require(value.is_array(), ErrorCode::InvalidInput, "event script group must be an array");
    // An explicitly empty group is a deliberate absent script, not a reason
    // to borrow another actor's steps or reserve an unexecutable attempt.
    return value.as_array().empty() ? std::vector<FakeModelStep>{} : fake_model_steps_from_json(value);
}
bool terminal(const Json& task) {
    const auto status = text(task, "status");
    return status == "ACCEPTED" || status == "CANCELLED" || status == "FAILED" || status == "SKIPPED" || status == "SUPERSEDED";
}
bool instruction_current(const Json& instruction, const ExecutionBinding& binding) {
    return !instruction.is_null() && text(instruction, "team_id") == binding.team_id &&
        number(instruction, "based_on_requirements_revision") == binding.requirements_revision &&
        number(instruction, "task_revision") == binding.task_revision &&
        number(instruction, "plan_revision") == binding.plan_revision &&
        number(instruction, "instruction_revision") == binding.instruction_revision &&
        number(instruction, "dispatcher_epoch") == binding.dispatcher_epoch &&
        number(instruction, "execution_epoch") == binding.execution_epoch;
}
std::vector<std::int64_t> ids(const Json& claim) {
    std::vector<std::int64_t> result;
    for (const auto& item : claim.at("items").as_array()) result.push_back(number(item, "inbox_id"));
    return result;
}
void validate_dispositions(const Json& dispositions) {
    require(dispositions.is_object() && dispositions.as_object().size() <= 64, ErrorCode::InvalidInput,
            "AMEND dispositions must be a bounded object keyed by node_id");
    for (const auto& entry : dispositions.as_object()) {
        node_name(entry.key());
        const auto& value = entry.value();
        require(value.is_object() && value.as_object().size() == 2 &&
                value.as_object().contains("disposition") && value.at("disposition").is_string() &&
                value.as_object().contains("rationale") && value.at("rationale").is_string(),
                ErrorCode::InvalidInput, "each disposition needs exactly disposition and rationale");
        const auto decision = text(value, "disposition");
        const auto rationale = text(value, "rationale");
        require((decision == "update" || decision == "keep" || decision == "cancel") &&
                rationale.find_first_not_of(" \t\r\n") != std::string::npos && rationale.size() <= 4096 &&
                rationale.find('\0') == std::string::npos,
                ErrorCode::InvalidInput, "invalid AMEND decision or rationale");
    }
}
} // namespace

EventFakeScript event_fake_script_from_json(const Json& value) {
    EventFakeScript result;
    if (value.is_array()) { result.default_steps = group(value); return result; }
    require(value.is_object(), ErrorCode::InvalidInput, "event script must be an array or grouped object");
    for (const auto& field : value.as_object())
        require(field.key() == "default" || field.key() == "nodes", ErrorCode::InvalidInput,
                "unknown event script field");
    if (const auto* fallback = value.as_object().if_contains("default")) result.default_steps = group(*fallback);
    if (const auto* nodes = value.as_object().if_contains("nodes")) {
        require(nodes->is_object() && nodes->as_object().size() <= 64, ErrorCode::InvalidInput,
                "event script nodes must be a bounded object");
        for (const auto& node : nodes->as_object()) {
            node_name(node.key());
            require(result.by_node.emplace(std::string(node.key()), group(node.value())).second,
                    ErrorCode::InvalidInput, "duplicate event script node");
        }
    }
    return result;
}

EventRuntime::EventRuntime(Store& store, const SchemaRegistry& schemas,
                          std::map<std::string, WorkerTools*> tools_by_node)
    : store_(store), schemas_(schemas), tools_by_node_(std::move(tools_by_node)) {
    require(tools_by_node_.size() <= 64, ErrorCode::InvalidInput, "too many explicit workspace mappings");
    std::set<WorkerTools*> assigned;
    for (const auto& [node, tools] : tools_by_node_) {
        node_name(node);
        require(tools && assigned.insert(tools).second, ErrorCode::InvalidInput,
                "each mapped task needs a distinct non-null WorkerTools host");
    }
}

Json EventRuntime::pump(const std::string& id, const EventFakeScript& script,
                        const EventPumpOptions& options) {
    require(store_.event_runtime_enabled(), ErrorCode::Unsupported, "event pump requires schema 404");
    require(options.max_calls >= 1 && options.max_calls <= 64 && options.max_worker_calls_per_task >= 1 &&
            options.max_worker_calls_per_task <= 16, ErrorCode::InvalidInput, "invalid finite pump call limits");
    validate_dispositions(options.amendment_dispositions);
    require(script.by_node.size() <= 64, ErrorCode::InvalidInput, "too many event script groups");
    if (!script.default_steps.empty()) (void)FakeModel(script.default_steps);
    for (const auto& [node, steps] : script.by_node) {
        node_name(node);
        if (!steps.empty()) (void)FakeModel(steps);
    }
    const auto initial = store_.request(id);
    const auto config = restore_request_config(initial);
    const auto manager_cap = number(config.json().at("system").at("limits"), "manager_max_tool_calls_per_wake");
    require(manager_cap > 0, ErrorCode::Corrupt, "frozen manager wake limit must be positive");
    Array calls;
    Array blocked;
    std::map<std::pair<Role, std::string>, std::int64_t> indices;
    std::int64_t manager_calls = 0;
    const auto block = [&](Role role, const std::string& node, const std::string& task,
                           std::string_view code, const std::string& reason) {
        blocked.push_back(Object{{"role", role_name(role)}, {"node_id", node}, {"task_id", task},
                                 {"code", code}, {"reason", reason}});
    };
    const auto finish = [&](const StoreActor& actor, const Json& claim,
                            const std::vector<std::int64_t>& completed) {
        if (!text(claim, "claim_id").empty())
            static_cast<void>(store_.finish_events(actor, text(claim, "claim_id"), number(claim, "generation"), completed));
    };
    const auto invoke = [&](Role role, const std::string& node, const std::string& task,
                            WorkerTools* tools) -> std::optional<Json> {
        if (static_cast<std::int64_t>(calls.size()) >= options.max_calls) {
            block(role, node, task, "call_limit", "this explicit pump reached its total call limit");
            return std::nullopt;
        }
        if (role != Role::Worker && manager_calls >= manager_cap) {
            block(role, node, task, "manager_limit", "the frozen management wake limit was reached");
            return std::nullopt;
        }
        const auto found = script.by_node.find(node);
        const auto& steps = node.empty() || found == script.by_node.end() ? script.default_steps : found->second;
        const auto index = indices[{role, node}] + 1;
        if (std::none_of(steps.begin(), steps.end(), [&](const auto& step) {
                return step.role == role && step.role_call_index == index;
            })) {
            block(role, node, task, "missing_script", "no explicit Fake step for this role/node sequence");
            return std::nullopt;
        }
        const FakeModel fake(steps, config.resolve(role).model);
        OfflineRuntime runtime(store_, schemas_, tools);
        const auto captured = runtime.reserve_call(fake, id, role, task, index);
        indices[{role, node}] = index;
        if (role != Role::Worker) ++manager_calls;
        Object invocation{{"attempt_id", captured.request.attempt_id}, {"role", role_name(role)},
            {"node_id", node}, {"task_id", task}, {"role_call_index", index},
            {"config_snapshot_sha256", captured.request.config_snapshot_id},
            {"prefix_sha256", captured.request.context.prefix_sha256}};
        try {
            const auto response = fake.complete(captured.request);
            invocation["response"] = to_json(response);
            auto result = role == Role::Dispatcher
                ? runtime.accept_independent_response(captured, response, options.amendment_dispositions)
                : runtime.accept_response(captured, response);
            invocation["result"] = result;
            calls.push_back(std::move(invocation));
            return result;
        } catch (const Error& error) {
            invocation["error"] = Object{{"code", error_name(error.code())}, {"message", error.what()}};
            calls.push_back(std::move(invocation));
            throw;
        } catch (const std::exception& error) {
            invocation["error"] = Object{{"code", "unexpected"}, {"message", error.what()}};
            calls.push_back(std::move(invocation));
            throw;
        }
    };

    // The Dispatcher claim is request-scoped, but its AMEND items have distinct
    // subjects. A successful alpha decision may never acknowledge beta's item.
    const auto dispatcher = store_.host_actor(Role::Dispatcher, id);
    Json dispatcher_claim = nullptr;
    try {
        dispatcher_claim = store_.claim_events(dispatcher);
        auto current_tasks = store_.tasks(id);
        for (const auto& decision : options.amendment_dispositions.as_object()) {
            const auto found = std::find_if(current_tasks.begin(), current_tasks.end(), [&](const auto& task) {
                return text(task, "node_id") == decision.key();
            });
            require(found != current_tasks.end(), ErrorCode::InvalidInput,
                    "AMEND disposition names an unallocated node");
        }
        if (!dispatcher_claim.at("items").as_array().empty()) {
            const auto request = store_.request(id);
            const bool needs_plan = number(request, "plan_revision") == 0 ||
                number(store_.plan(id), "based_on_requirements_revision") != number(request, "requirements_revision");
            if (needs_plan) {
                if (!current_tasks.empty() && options.amendment_dispositions.as_object().empty())
                    block(Role::Dispatcher, {}, {}, "amendment_pending", "AMEND needs explicit per-node host dispositions");
                else static_cast<void>(invoke(Role::Dispatcher, {}, {}, tools_by_node_.empty() ? nullptr : tools_by_node_.begin()->second));
            } else if (!options.amendment_dispositions.as_object().empty()) {
                static_cast<void>(store_.apply_independent_dispositions(dispatcher, number(request, "state_version"),
                    number(request, "requirements_revision"), number(request, "plan_revision"),
                    options.amendment_dispositions, new_id("dispositions")));
            }
            const auto after = store_.request(id);
            current_tasks = store_.tasks(id);
            const bool plan_current = number(after, "plan_revision") > 0 &&
                number(store_.plan(id), "based_on_requirements_revision") == number(after, "requirements_revision");
            std::vector<std::int64_t> completed;
            for (const auto& item : dispatcher_claim.at("items").as_array()) {
                const auto subject = text(item, "subject_task_id");
                bool done = plan_current && subject.empty();
                if (plan_current && !subject.empty()) {
                    const auto found = std::find_if(current_tasks.begin(), current_tasks.end(), [&](const auto& task) {
                        return text(task, "task_id") == subject;
                    });
                    done = found != current_tasks.end() &&
                        number(*found, "based_on_requirements_revision") == number(after, "requirements_revision") &&
                        number(*found, "plan_revision") == number(after, "plan_revision");
                }
                if (done) completed.push_back(number(item, "inbox_id"));
            }
            finish(dispatcher, dispatcher_claim, completed);
        } else {
            require(options.amendment_dispositions.as_object().empty(), ErrorCode::NotReady,
                    "explicit dispositions have no pending Dispatcher work");
        }
        dispatcher_claim = nullptr;
    } catch (const Error& error) {
        block(Role::Dispatcher, {}, {}, error_name(error.code()), error.what());
        if (!dispatcher_claim.is_null()) try { finish(dispatcher, dispatcher_claim, {}); }
            catch (const Error& release) { block(Role::Dispatcher, {}, {}, error_name(release.code()), release.what()); }
    } catch (const std::exception& error) {
        block(Role::Dispatcher, {}, {}, "unexpected", error.what());
        if (!dispatcher_claim.is_null()) try { finish(dispatcher, dispatcher_claim, {}); }
            catch (const std::exception& release) { block(Role::Dispatcher, {}, {}, "claim_release_failed", release.what()); }
    }

    // Process independent tasks in durable node order. No task waits on a model
    // or child while holding an event transaction or another task's capacity.
    for (const auto& assigned : store_.tasks(id)) {
        const auto node = text(assigned, "node_id");
        const auto task_id = text(assigned, "task_id");
        if (terminal(assigned)) continue;
        Json active_claim = nullptr;
        StoreActor active_actor;
        try {
            const auto waiting = store_.worker_wait(task_id);
            if (!waiting.is_null() && text(waiting, "status") == "WAITING") {
                block(Role::Worker, node, task_id, "waiting", text(waiting, "reason"));
                continue;
            }
            const auto current_request = store_.request(id);
            if (number(assigned, "based_on_requirements_revision") != number(current_request, "requirements_revision") ||
                    number(assigned, "plan_revision") != number(current_request, "plan_revision")) {
                block(Role::TeamLead, node, task_id, "amendment_pending", "task still needs a current explicit disposition and plan binding");
                continue;
            }
            const auto mapped = tools_by_node_.find(node);
            if (!tools_by_node_.empty() && mapped == tools_by_node_.end()) {
                block(Role::Worker, node, task_id, "workspace_missing", "this task has no explicitly mapped WorkerTools host");
                continue;
            }
            WorkerTools* tools = mapped == tools_by_node_.end() ? nullptr : mapped->second;
            auto binding = store_.binding(task_id);
            active_actor = store_.host_actor(Role::TeamLead, id, binding.team_id);
            active_claim = store_.claim_events(active_actor);
            if (!instruction_current(store_.instruction(task_id), binding)) {
                if (active_claim.at("items").as_array().empty()) {
                    block(Role::TeamLead, node, task_id, "event_missing", "no claimed event authorizes a new instruction");
                    active_claim = nullptr;
                    continue;
                }
                if (!invoke(Role::TeamLead, node, task_id, tools)) {
                    finish(active_actor, active_claim, {}); active_claim = nullptr;
                    continue;
                }
            }
            // A replayed startup/recovery event can be completed from the
            // matching persisted instruction without paying for another order.
            finish(active_actor, active_claim, ids(active_claim)); active_claim = nullptr;
            binding = store_.binding(task_id);
            auto current_task = store_.task(task_id);
            active_actor = store_.host_actor(Role::Worker, id, binding.team_id);
            if (number(current_task, "applied_task_revision") != binding.task_revision ||
                    number(current_task, "applied_plan_revision") != binding.plan_revision ||
                    number(current_task, "applied_instruction_revision") != binding.instruction_revision)
                static_cast<void>(store_.acknowledge(active_actor, binding, number(current_task, "state_version"), new_id("ack")));
            for (std::int64_t turn = 0; turn < options.max_worker_calls_per_task; ++turn) {
                active_claim = store_.claim_events(active_actor);
                if (active_claim.at("items").as_array().empty()) { active_claim = nullptr; break; }
                const auto result = invoke(Role::Worker, node, task_id, tools);
                if (!result) { finish(active_actor, active_claim, {}); active_claim = nullptr; break; }
                const auto* operation = result->is_object() ? result->as_object().if_contains("operation") : nullptr;
                const bool failed_operation = operation && text(*operation, "status") != "SUCCEEDED";
                if (failed_operation) {
                    block(Role::Worker, node, task_id, "operation_incomplete", "a failed or unknown operation stops this task's batch");
                    finish(active_actor, active_claim, {}); active_claim = nullptr; break;
                }
                finish(active_actor, active_claim, ids(active_claim)); active_claim = nullptr;
            }
            if (indices[{Role::Worker, node}] == options.max_worker_calls_per_task && !store_.inbox(active_actor).empty())
                block(Role::Worker, node, task_id, "worker_limit", "this task reached the explicit per-pump Worker call limit");
        } catch (const Error& error) {
            block(active_actor.role == Role::Root ? Role::Worker : active_actor.role, node, task_id,
                  error_name(error.code()), error.what());
            if (!active_claim.is_null()) try { finish(active_actor, active_claim, {}); }
                catch (const Error& release) { block(active_actor.role, node, task_id, error_name(release.code()), release.what()); }
        } catch (const std::exception& error) {
            block(active_actor.role == Role::Root ? Role::Worker : active_actor.role, node, task_id, "unexpected", error.what());
            if (!active_claim.is_null()) try { finish(active_actor, active_claim, {}); }
                catch (const std::exception& release) { block(active_actor.role, node, task_id, "claim_release_failed", release.what()); }
        }
    }
    Array pending;
    // Retired tasks can still own unprocessed history. Their pending items are
    // facts, even though this bounded runner will not reactivate their actors.
    for (const auto& item : store_.request_inbox(id)) pending.push_back(item);
    Array tasks;
    for (const auto& task : store_.tasks(id)) tasks.push_back(task);
    return Object{{"mode", "event_fake"}, {"request_id", id}, {"config_snapshot_sha256", config.sha256()},
        {"calls", std::move(calls)}, {"blocked", std::move(blocked)}, {"pending", std::move(pending)},
        {"tasks", std::move(tasks)}, {"request_status", store_.request(id).at("status")}, {"budget", store_.budget(id)},
        {"max_calls", options.max_calls}, {"max_worker_calls_per_task", options.max_worker_calls_per_task},
        {"manager_calls", manager_calls}, {"worker_tools_enabled", !tools_by_node_.empty()},
        {"business_delivered", false}, {"environment_checks_executed", false}, {"real_model_calls", 0}};
}

} // namespace wls::s1
