#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "wls/management_context.hpp"

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
        number(instruction, "instruction_revision") == binding.instruction_revision;
    // Epoch-only recovery is a host rebind + Worker control ack, never a new
    // TeamLead interpretation. Store still fences every old actor and result.
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
    if (store_.workflow_runtime_enabled()) return pump_workflow(id, script, options);
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
        if (store_.capacity_status(id).at("status") == "WAITING_INPUT") {
            block(role, node, task, "capacity_waiting_input", "Root capacity-checkpoint required; no new model reservation");
            return std::nullopt;
        }
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

Json EventRuntime::pump_workflow(const std::string& id, const EventFakeScript& script,
                                 const EventPumpOptions& options) {
    require(options.max_calls >= 1 && options.max_calls <= 64 && options.max_worker_calls_per_task >= 1 &&
            options.max_worker_calls_per_task <= 16, ErrorCode::InvalidInput, "invalid finite pump call limits");
    validate_dispositions(options.amendment_dispositions);
    require(script.by_node.size() <= 64, ErrorCode::InvalidInput, "too many event script groups");
    if (!script.default_steps.empty()) (void)FakeModel(script.default_steps);
    for (const auto& [node, steps] : script.by_node) {
        node_name(node);
        if (!steps.empty()) (void)FakeModel(steps);
    }
    const auto config = restore_request_config(store_.request(id));
    const auto manager_cap = number(config.json().at("system").at("limits"), "manager_max_tool_calls_per_wake");
    require(manager_cap > 0, ErrorCode::Corrupt, "frozen manager wake limit must be positive");
    using ActorKey = std::pair<Role, std::string>;
    std::map<ActorKey, std::int64_t> indices;
    std::set<ActorKey> stopped;
    std::set<std::string> block_keys;
    Array calls, blocked;
    std::int64_t manager_calls = 0;
    // Explicit host decisions retain their compatibility path for this whole
    // pump. Otherwise the Dispatcher captures its own bounded AMEND target;
    // later omitted-node decisions must not publish another plan.
    const bool model_amendment = options.amendment_dispositions.as_object().empty();
    bool dispositions_applied = options.amendment_dispositions.as_object().empty();
    const auto block = [&](Role role, const std::string& node, const std::string& task,
                           std::string_view code, const std::string& reason) {
        const auto key = std::string(role_name(role)) + ":" + node + ":" + std::string(code);
        if (block_keys.insert(key).second)
            blocked.push_back(Object{{"role", role_name(role)}, {"node_id", node}, {"task_id", task},
                {"code", code}, {"reason", reason}});
    };
    const auto finish = [&](const StoreActor& actor, const Json& claim,
                            const std::vector<std::int64_t>& completed) {
        if (!text(claim, "claim_id").empty())
            static_cast<void>(store_.finish_events(actor, text(claim, "claim_id"), number(claim, "generation"), completed));
    };
    const auto request_terminal = [&] {
        const auto status = text(store_.request(id), "status");
        return status == "DELIVERED" || status == "FAILED" || status == "CANCELLED" || status == "SUPERSEDED";
    };
    const auto event_object_id = [](const Json& item, std::string_view field) -> std::string {
        for (const auto* section : {"result", "payload"}) {
            const auto& value = item.at(section);
            if (value.is_object())
                if (const auto* found = value.as_object().if_contains(field); found && found->is_string())
                    return std::string(found->as_string());
        }
        return {};
    };
    const auto recovery_item = [](const Json& item) {
        const auto type = text(item, "type");
        return type == "recover" || type == "event_bootstrap" || type == "workflow_bootstrap" ||
            type == "recover_experience";
    };
    const auto action_in_claim = [&](const Json& action, const Json& claim) {
        const auto kind = text(action, "kind");
        if (kind == "amendment") {
            const auto& target = action.at("target");
            const auto& pending = target.at("pending_node_ids").as_array();
            return std::any_of(claim.at("items").as_array().begin(), claim.at("items").as_array().end(),
                [&](const auto& item) {
                    if (text(item, "type") != "amend" && !recovery_item(item)) return false;
                    const auto subject = text(item, "subject_task_id");
                    if (subject.empty()) return true;
                    return std::any_of(target.at("tasks").as_array().begin(), target.at("tasks").as_array().end(),
                        [&](const auto& task) {
                            return text(task, "task_id") == subject &&
                                std::any_of(pending.begin(), pending.end(), [&](const auto& node) {
                                    return node == task.at("node_id");
                                });
                        });
                });
        }
        const auto field = kind == "question" ? "question_id" : kind == "experience" ? "experience_id" : "submission_id";
        const auto target_id = text(action.at("target"), field);
        const auto target_task = text(action.at("target"), "task_id");
        return std::any_of(claim.at("items").as_array().begin(), claim.at("items").as_array().end(),
            [&](const auto& item) {
                const auto* escalation = action.at("target").as_object().if_contains("escalation");
                const bool answered_rejection = kind == "rejection" && escalation && !escalation->is_null() &&
                    text(item, "type") == "answer_question" &&
                    event_object_id(item, "question_id") == text(*escalation, "question_id");
                return event_object_id(item, field) == target_id || answered_rejection ||
                    (recovery_item(item) && text(item, "subject_task_id") == target_task);
            });
    };
    const auto question_done = [&](const Json& item, Role role) {
        const auto question_id = event_object_id(item, "question_id");
        if (question_id.empty()) return false;
        const auto question = store_.question(question_id);
        return text(question, "status") != "OPEN" || text(question, "holder") != role_name(role);
    };
    // A question answer is a wakeup, not proof that its rejected task was
    // handled. Keep that exact event if a script is absent, malformed, or out
    // of budget. A user-owned question or exhausted answered allowance has no
    // automatic Dispatcher successor; the durable blocked source remains visible.
    const auto rejection_notice_done = [&](const std::string& task_id) {
        if (!store_.acceptance_runtime_enabled()) return true;
        const auto rejected = store_.rejected_submission(task_id);
        if (rejected.is_null()) return true;
        const auto& escalation = rejected.at("escalation");
        return !escalation.is_null() && (escalation.at("status") == "OPEN" ||
            number(rejected, "rework_remaining") == 0);
    };
    const auto management_target = [&](Role role, const std::string& task) -> Json {
        if (role == Role::Dispatcher && model_amendment) {
            const auto amendment = next_amendment_action(store_, id);
            if (!amendment.is_null()) return amendment;
        }
        return next_workflow_action(store_, role, id, task);
    };
    const auto invoke = [&](Role role, const std::string& node, const std::string& task,
                            WorkerTools* tools, const Json& claim) -> std::optional<Json> {
        const ActorKey key{role, node};
        if (stopped.contains(key)) return std::nullopt;
        if (store_.capacity_status(id).at("status") == "WAITING_INPUT") {
            // Resolve a potentially large immutable target only for a hold.
            // The normal path already captures/validates it in reserve_call.
            const auto pending_action = role == Role::Worker ? Json(nullptr) : management_target(role, task);
            if (pending_action.is_null() || text(pending_action, "kind") != "experience") {
                block(role, node, task, "capacity_waiting_input", "Root capacity-checkpoint required; no new model reservation");
                stopped.insert(key); return std::nullopt;
            }
        }
        if (static_cast<std::int64_t>(calls.size()) >= options.max_calls) {
            block(role, node, task, "call_limit", "this explicit pump reached its total call limit");
            stopped.insert(key); return std::nullopt;
        }
        if (role != Role::Worker && manager_calls >= manager_cap) {
            block(role, node, task, "manager_limit", "the frozen management wake limit was reached");
            stopped.insert(key); return std::nullopt;
        }
        if (role == Role::Worker && indices[key] >= options.max_worker_calls_per_task) {
            block(role, node, task, "worker_limit", "this task reached the explicit per-pump Worker call limit");
            stopped.insert(key); return std::nullopt;
        }
        const auto found = script.by_node.find(node);
        const auto& steps = node.empty() || found == script.by_node.end() ? script.default_steps : found->second;
        const auto index = indices[key] + 1;
        if (std::none_of(steps.begin(), steps.end(), [&](const auto& step) {
                return step.role == role && step.role_call_index == index;
            })) {
            block(role, node, task, "missing_script", "no explicit Fake step for this role/node sequence");
            stopped.insert(key); return std::nullopt;
        }
        const FakeModel fake(steps, config.resolve(role).model);
        OfflineRuntime runtime(store_, schemas_, tools);
        const auto captured = runtime.reserve_call(fake, id, role, task, index, {},
            role == Role::Dispatcher && model_amendment);
        indices[key] = index;
        if (role != Role::Worker) ++manager_calls;
        Object invocation{{"attempt_id", captured.request.attempt_id}, {"role", role_name(role)},
            {"node_id", node}, {"task_id", task}, {"role_call_index", index},
            {"config_snapshot_sha256", captured.request.config_snapshot_id},
            {"prefix_sha256", captured.request.context.prefix_sha256},
            {"workflow_action", captured.workflow_action}};
        try {
            // Ordinary events select one exact object. A recovery event may
            // reconstruct a current workflow target only within its captured
            // task subject. Neither case completes newly arrived inbox rows.
            if (!captured.workflow_action.is_null() && !action_in_claim(captured.workflow_action, claim)) {
                ModelResponse aborted;
                aborted.call_id = captured.request.call_id;
                aborted.attempt_id = captured.request.attempt_id;
                aborted.role_call_index = captured.request.role_call_index;
                aborted.role = captured.request.role;
                aborted.binding = captured.request.binding;
                aborted.config_snapshot_id = captured.request.config_snapshot_id;
                aborted.status = ModelResultStatus::Error;
                aborted.usage = {0, 0, 0, 0, 0};
                aborted.error_code = "host_claim_stale_before_model";
                aborted.error_message = "host refused a target outside the captured claim before invoking FakeModel";
                const auto response = to_json(aborted);
                invocation["response"] = response;
                static_cast<void>(store_.settle_attempt(aborted.attempt_id, aborted.binding, aborted.config_snapshot_id,
                    "ERROR", to_json(aborted.usage), response));
                static_cast<void>(store_.record_tool_feedback(aborted.attempt_id, aborted.binding, response,
                    Object{{"status", "ERROR"}, {"runtime_error", true}, {"error_code", "stale"},
                        {"message", aborted.error_message}, {"model_invoked", false}, {"observation_complete", false}}));
                throw Error(ErrorCode::Stale, aborted.error_message);
            }
            const auto response = fake.complete(captured.request);
            invocation["response"] = to_json(response);
            const auto result = role == Role::Dispatcher
                ? runtime.accept_independent_response(captured, response,
                    dispositions_applied ? Json(Object{}) : options.amendment_dispositions)
                : runtime.accept_response(captured, response);
            invocation["result"] = result;
            calls.push_back(std::move(invocation));
            return result;
        } catch (const Error& error) {
            invocation["error"] = Object{{"code", error_name(error.code())}, {"message", error.what()}};
            calls.push_back(std::move(invocation)); stopped.insert(key); throw;
        } catch (const std::exception& error) {
            invocation["error"] = Object{{"code", "unexpected"}, {"message", error.what()}};
            calls.push_back(std::move(invocation)); stopped.insert(key); throw;
        }
    };

    // A successful action may wake an earlier role. Revisit actors in bounded
    // rounds instead of discarding those events or waiting inside a transaction.
    // Ordinary errors and missing script steps stop only that actor for this
    // explicit pump; another round is never an implicit retry of its failure.
    for (std::int64_t round = 0; round < 128 && (!request_terminal() || store_.acceptance_runtime_enabled()); ++round) {
        bool progress = false;
        const auto dispatcher = store_.host_actor(Role::Dispatcher, id);
        Json dispatcher_claim = nullptr;
        if (!request_terminal() && !stopped.contains({Role::Dispatcher, {}})) try {
            dispatcher_claim = store_.claim_events(dispatcher);
            auto current_tasks = store_.tasks(id);
            if (!dispositions_applied) for (const auto& decision : options.amendment_dispositions.as_object())
                require(std::any_of(current_tasks.begin(), current_tasks.end(), [&](const auto& task) {
                    return text(task, "node_id") == decision.key();
                }), ErrorCode::InvalidInput, "AMEND disposition names an unallocated node");
            if (!dispatcher_claim.at("items").as_array().empty()) {
                auto request = store_.request(id);
                const bool needs_plan = number(request, "plan_revision") == 0 ||
                    number(store_.plan(id), "based_on_requirements_revision") != number(request, "requirements_revision");
                if (needs_plan) {
                    if (invoke(Role::Dispatcher, {}, {}, tools_by_node_.empty() ? nullptr : tools_by_node_.begin()->second,
                                    dispatcher_claim)) {
                        progress = true; dispositions_applied = true;
                    }
                } else {
                    if (!dispositions_applied) {
                        static_cast<void>(store_.apply_independent_dispositions(dispatcher, number(request, "state_version"),
                            number(request, "requirements_revision"), number(request, "plan_revision"),
                            options.amendment_dispositions, new_id("dispositions")));
                        dispositions_applied = true; progress = true;
                    }
                    const auto action = management_target(Role::Dispatcher, {});
                    WorkerTools* amendment_tools = !action.is_null() && text(action, "kind") == "amendment" &&
                        !tools_by_node_.empty() ? tools_by_node_.begin()->second : nullptr;
                    if (!action.is_null() && action_in_claim(action, dispatcher_claim) &&
                            invoke(Role::Dispatcher, {}, {}, amendment_tools, dispatcher_claim)) progress = true;
                }
                request = store_.request(id);
                current_tasks = store_.tasks(id);
                const bool plan_current = number(request, "plan_revision") > 0 &&
                    number(store_.plan(id), "based_on_requirements_revision") == number(request, "requirements_revision");
                std::vector<std::int64_t> completed;
                for (const auto& item : dispatcher_claim.at("items").as_array()) {
                    const auto type = text(item, "type");
                    bool done = false;
                    if (type == "ask_question" || type == "forward_question" || type == "answer_question") {
                        done = question_done(item, Role::Dispatcher);
                        if (done && type == "answer_question" && item.at("result").at("asked_by") == "dispatcher")
                            done = rejection_notice_done(text(item.at("result"), "task_id"));
                    }
                    else if (type == "admit" || type == "amend" || recovery_item(item)) {
                        const auto subject = text(item, "subject_task_id");
                        done = plan_current && subject.empty();
                        if (plan_current && !subject.empty()) {
                            const auto found = std::find_if(current_tasks.begin(), current_tasks.end(), [&](const auto& task) {
                                return text(task, "task_id") == subject;
                            });
                            done = found != current_tasks.end() &&
                                number(*found, "based_on_requirements_revision") == number(request, "requirements_revision") &&
                                number(*found, "plan_revision") == number(request, "plan_revision");
                            if (done && recovery_item(item)) done = rejection_notice_done(subject);
                        }
                    }
                    else if (store_.acceptance_runtime_enabled() && (type == "check_submission" ||
                            (type == "accept_candidate" && item.at("result").at("acceptance").at("status") == "REJECTED"))) {
                        const auto submission_id = event_object_id(item, "submission_id");
                        if (!submission_id.empty()) {
                            const auto submission = store_.submission(submission_id);
                            const auto task = store_.task(text(submission, "task_id"));
                            const auto status = text(submission, "status");
                            done = status == "SUPERSEDED" || status == "RETURNED_FOR_REWORK" ||
                                text(task, "status") == "ACCEPTED";
                            if (status == "BLOCKED") done = rejection_notice_done(text(task, "task_id"));
                        }
                    }
                    // Delivery assembly is an explicit host action. Its
                    // acceptance/report notices are retained, never treated
                    // as implicit package publication or receipt.
                    if (done) completed.push_back(number(item, "inbox_id"));
                }
                finish(dispatcher, dispatcher_claim, completed);
                progress = progress || !completed.empty();
            } else require(dispositions_applied, ErrorCode::NotReady, "explicit dispositions have no pending Dispatcher work");
            dispatcher_claim = nullptr;
        } catch (const Error& error) {
            stopped.insert({Role::Dispatcher, {}});
            block(Role::Dispatcher, {}, {}, error_name(error.code()), error.what());
            if (!dispatcher_claim.is_null()) try { finish(dispatcher, dispatcher_claim, {}); }
                catch (const Error& release) { block(Role::Dispatcher, {}, {}, error_name(release.code()), release.what()); }
        } catch (const std::exception& error) {
            stopped.insert({Role::Dispatcher, {}});
            block(Role::Dispatcher, {}, {}, "unexpected", error.what());
            if (!dispatcher_claim.is_null()) try { finish(dispatcher, dispatcher_claim, {}); }
                catch (const std::exception& release) { block(Role::Dispatcher, {}, {}, "claim_release_failed", release.what()); }
        }

        for (const auto& assigned : store_.tasks(id)) {
            const auto node = text(assigned, "node_id"), task_id = text(assigned, "task_id");
            const auto queued_action = management_target(Role::TeamLead, task_id);
            const bool experience_pending = !queued_action.is_null() && text(queued_action, "kind") == "experience";
            const bool finished_business = terminal(assigned) || request_terminal();
            // A report committed through a direct host call may still have an
            // unconsumed acceptance notice. Complete only the proven DONE job
            // items without resurrecting an order or a Worker execution.
            const bool experience_history = finished_business && store_.acceptance_runtime_enabled() &&
                !store_.experience_jobs(id, task_id).empty();
            if (finished_business && !experience_pending && !experience_history) continue;
            Json active_claim = nullptr;
            StoreActor active_actor;
            try {
                const auto current_request = store_.request(id);
                if (!experience_pending && !experience_history &&
                    (number(assigned, "based_on_requirements_revision") != number(current_request, "requirements_revision") ||
                     number(assigned, "plan_revision") != number(current_request, "plan_revision"))) continue;
                const auto mapped = tools_by_node_.find(node);
                WorkerTools* tools = mapped == tools_by_node_.end() ? nullptr : mapped->second;
                auto binding = store_.binding(task_id);
                active_actor = store_.host_actor(Role::TeamLead, id, binding.team_id);
                if (!stopped.contains({Role::TeamLead, node})) {
                    active_claim = store_.claim_events(active_actor);
                    if (!active_claim.at("items").as_array().empty()) {
                        const auto action = management_target(Role::TeamLead, task_id);
                        const auto waiting = store_.worker_wait(task_id);
                        const auto submission = store_.current_submission(task_id);
                        const bool order_needed = !terminal(assigned) && !request_terminal() &&
                            action.is_null() && submission.is_null() &&
                            (waiting.is_null() || text(waiting, "status") != "WAITING") &&
                            !instruction_current(store_.instruction(task_id), binding);
                        if ((!action.is_null() && action_in_claim(action, active_claim)) || order_needed)
                            if (invoke(Role::TeamLead, node, task_id, tools, active_claim)) progress = true;
                        binding = store_.binding(task_id);
                        const bool current_order = instruction_current(store_.instruction(task_id), binding);
                        const auto current_submission = store_.current_submission(task_id);
                        const auto questions = visible_management_questions(store_, Role::TeamLead, id, task_id);
                        const bool question_pending = std::any_of(questions.begin(), questions.end(), [](const auto& question) {
                            return text(question, "status") == "OPEN";
                        });
                        const auto jobs = store_.acceptance_runtime_enabled()
                            ? store_.experience_jobs(id, task_id) : std::vector<Json>{};
                        const bool unfinished_experience = std::any_of(jobs.begin(), jobs.end(), [](const auto& job) {
                            return text(job, "status") != "DONE";
                        });
                        std::vector<std::int64_t> completed;
                        for (const auto& item : active_claim.at("items").as_array()) {
                            const auto type = text(item, "type");
                            bool done = false;
                            if (type == "ask_question" || type == "forward_question" || type == "answer_question")
                                done = question_done(item, Role::TeamLead);
                            else if (type == "submit_work") {
                                const auto submission_id = event_object_id(item, "submission_id");
                                done = !submission_id.empty() && text(store_.submission(submission_id), "status") != "SUBMITTED";
                            } else if (store_.acceptance_runtime_enabled() && type == "accept_candidate") {
                                const auto experience_id = event_object_id(item, "experience_id");
                                done = !experience_id.empty() && text(store_.experience_job(experience_id), "status") == "DONE";
                            } else if (type == "create_task" || type == "disposition" || type == "rebind_task" ||
                                       type == "independent_plan" || type == "independent_dispositions" || type == "change_task" || recovery_item(item))
                                done = current_order || !current_submission.is_null() || question_pending;
                            if (store_.acceptance_runtime_enabled() && recovery_item(item)) {
                                if (unfinished_experience) done = false;
                                else if (experience_pending || type == "recover_experience") done = true;
                            }
                            if (done) completed.push_back(number(item, "inbox_id"));
                        }
                        finish(active_actor, active_claim, completed);
                        progress = progress || !completed.empty();
                    }
                    active_claim = nullptr;
                }

                const auto current_task = store_.task(task_id);
                if (request_terminal()) continue;
                const auto status = text(current_task, "status");
                const auto waiting = store_.worker_wait(task_id);
                if ((status != "READY" && status != "RUNNING") ||
                        (!waiting.is_null() && text(waiting, "status") == "WAITING")) continue;
                if (!tools_by_node_.empty() && !tools) {
                    block(Role::Worker, node, task_id, "workspace_missing", "this task has no explicitly mapped WorkerTools host");
                    stopped.insert({Role::Worker, node}); continue;
                }
                binding = store_.binding(task_id);
                if (!instruction_current(store_.instruction(task_id), binding)) continue;
                active_actor = store_.host_actor(Role::Worker, id, binding.team_id);
                if (number(current_task, "applied_task_revision") != binding.task_revision ||
                        number(current_task, "applied_plan_revision") != binding.plan_revision ||
                        number(current_task, "applied_instruction_revision") != binding.instruction_revision) {
                    static_cast<void>(store_.acknowledge(active_actor, binding, number(current_task, "state_version"), new_id("ack")));
                    progress = true;
                }
                if (stopped.contains({Role::Worker, node})) continue;
                active_claim = store_.claim_events(active_actor);
                if (active_claim.at("items").as_array().empty()) { active_claim = nullptr; continue; }
                const auto result = invoke(Role::Worker, node, task_id, tools, active_claim);
                if (!result) { finish(active_actor, active_claim, {}); active_claim = nullptr; continue; }
                const auto* operation = result->is_object() ? result->as_object().if_contains("operation") : nullptr;
                if (operation && text(*operation, "status") != "SUCCEEDED") {
                    block(Role::Worker, node, task_id, "operation_incomplete", "a failed or unknown operation stops this task's batch");
                    stopped.insert({Role::Worker, node});
                    finish(active_actor, active_claim, {}); active_claim = nullptr; continue;
                }
                finish(active_actor, active_claim, ids(active_claim)); active_claim = nullptr;
                progress = true;
            } catch (const Error& error) {
                const auto role = active_actor.role == Role::Root ? Role::Worker : active_actor.role;
                stopped.insert({role, node}); block(role, node, task_id, error_name(error.code()), error.what());
                if (!active_claim.is_null()) try { finish(active_actor, active_claim, {}); }
                    catch (const Error& release) { block(role, node, task_id, error_name(release.code()), release.what()); }
            } catch (const std::exception& error) {
                const auto role = active_actor.role == Role::Root ? Role::Worker : active_actor.role;
                stopped.insert({role, node}); block(role, node, task_id, "unexpected", error.what());
                if (!active_claim.is_null()) try { finish(active_actor, active_claim, {}); }
                    catch (const std::exception& release) { block(role, node, task_id, "claim_release_failed", release.what()); }
            }
        }
        if (!progress || static_cast<std::int64_t>(calls.size()) >= options.max_calls) break;
    }

    Array pending, tasks, questions, submissions;
    for (const auto& item : store_.request_inbox(id)) pending.push_back(item);
    const auto final_request = store_.request(id);
    for (const auto& task : store_.tasks(id)) {
        tasks.push_back(task);
        const auto node = text(task, "node_id"), task_id = text(task, "task_id");
        const auto submission = store_.current_submission(task_id);
        if (store_.acceptance_runtime_enabled() && !submission.is_null()) submissions.push_back(submission);
        if (terminal(task)) continue;
        if (number(task, "based_on_requirements_revision") != number(final_request, "requirements_revision") ||
                number(task, "plan_revision") != number(final_request, "plan_revision"))
            block(Role::TeamLead, node, task_id, "amendment_pending", "task still needs its current explicit disposition and plan binding");
        const auto waiting = store_.worker_wait(task_id);
        if (!waiting.is_null() && text(waiting, "status") == "WAITING")
            block(Role::Worker, node, task_id, "waiting", text(waiting, "reason"));
        if (!submission.is_null()) {
            if (!store_.acceptance_runtime_enabled()) submissions.push_back(submission);
            const auto status = text(submission, "status");
            if (status == "CANDIDATE_READY") block(Role::Dispatcher, node, task_id, "acceptance_pending",
                store_.acceptance_runtime_enabled()
                    ? "the candidate still needs its explicit current Dispatcher business acceptance"
                    : "TeamLead check produced a candidate; separate independent review and business acceptance remain pending");
            else if (status == "BLOCKED") block(Role::Dispatcher, node, task_id, "rework_blocked",
                "current rejection requires bounded Dispatcher correction or user resolution; answers never reset the frozen rework allowance");
            else block(Role::Worker, node, task_id, "submission_pending", "Worker submission awaits its captured TeamLead check");
        }
    }
    for (const auto& question : visible_management_questions(store_, Role::Dispatcher, id)) questions.push_back(question);
    if (static_cast<std::int64_t>(calls.size()) >= options.max_calls && !pending.empty())
        block(Role::Dispatcher, {}, {}, "call_limit", "this explicit pump reached its total call limit; remaining events stay durable");
    Object result{{"mode", "event_fake"}, {"workflow_runtime", true}, {"request_id", id},
        {"config_snapshot_sha256", config.sha256()}, {"calls", std::move(calls)}, {"blocked", std::move(blocked)},
        {"pending", std::move(pending)}, {"tasks", std::move(tasks)}, {"questions", std::move(questions)},
        {"submissions", std::move(submissions)}, {"request_status", final_request.at("status")}, {"budget", store_.budget(id)},
        {"max_calls", options.max_calls}, {"max_worker_calls_per_task", options.max_worker_calls_per_task},
        {"manager_calls", manager_calls}, {"worker_tools_enabled", !tools_by_node_.empty()},
        {"business_delivered", store_.acceptance_runtime_enabled() && text(final_request, "status") == "DELIVERED" &&
            text(final_request, "delivery_status") == "CONFIRMED"},
        {"environment_checks_executed", false}, {"real_model_calls", 0}};
    if (!config.management_history_policy().is_null())
        result["management_history"] = store_.management_view(store_.host_actor(Role::Dispatcher, id),
            store_.request_binding(id)).at("history_catalogs");
    if (store_.acceptance_runtime_enabled()) {
        Array jobs;
        for (const auto& job : store_.experience_jobs(id)) jobs.push_back(job);
        result["acceptance_runtime"] = true;
        result["experience_jobs"] = std::move(jobs);
    }
    return result;
}

} // namespace wls::s1
