#include "wls/persistent_event_selection.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace wls::s1 {
namespace {
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
std::string text(const Json& value, std::string_view field) {
    require(value.is_object() && value.as_object().contains(field) && value.at(field).is_string(),
            ErrorCode::Corrupt, "original event selection string is missing");
    return std::string(value.at(field).as_string());
}
std::int64_t number(const Json& item, std::string_view field) {
    const auto& value = item.at(field);
    if (value.is_int64()) return value.as_int64();
    require(value.is_uint64() && value.as_uint64() <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
            ErrorCode::Corrupt, "original event ID is not a bounded integer");
    return static_cast<std::int64_t>(value.as_uint64());
}
bool recovery(const Json& item) {
    const auto type = text(item, "type");
    return type == "recover" || type == "event_bootstrap" ||
        type == "workflow_bootstrap" || type == "recover_experience";
}
std::string object_id(const Json& item, std::string_view field) {
    for (const auto* section : {"result", "payload"}) {
        const auto& value = item.at(section);
        if (value.is_object())
            if (const auto* found = value.as_object().if_contains(field); found && found->is_string())
                return std::string(found->as_string());
    }
    return {};
}
} // namespace

bool event_matches_workflow_action(const Json& action, const Json& item) {
    const auto kind = text(action, "kind");
    if (kind == "amendment") {
        const auto& target = action.at("target");
        const auto& pending = target.at("pending_node_ids").as_array();
        if (text(item, "type") != "amend" && !recovery(item)) return false;
        const auto subject = text(item, "subject_task_id");
        if (subject.empty()) return true;
        return std::any_of(target.at("tasks").as_array().begin(), target.at("tasks").as_array().end(),
            [&](const auto& task) {
                return text(task, "task_id") == subject &&
                    std::any_of(pending.begin(), pending.end(), [&](const auto& node) {
                        return node == task.at("node_id");
                    });
            });
    }
    const auto field = kind == "question" ? "question_id" :
        kind == "experience" ? "experience_id" : "submission_id";
    const auto target_id = text(action.at("target"), field);
    const auto target_task = text(action.at("target"), "task_id");
    const auto* escalation = action.at("target").as_object().if_contains("escalation");
    const bool answered_rejection = kind == "rejection" && escalation && !escalation->is_null() &&
        text(item, "type") == "answer_question" &&
        object_id(item, "question_id") == text(*escalation, "question_id");
    return object_id(item, field) == target_id || answered_rejection ||
        (recovery(item) && text(item, "subject_task_id") == target_task);
}

bool event_claim_contains_workflow_action(const Json& action, const Json& claim) {
    const auto kind = text(action, "kind");
    if (kind == "amendment") {
        (void)action.at("target").at("pending_node_ids").as_array();
    } else {
        const auto field = kind == "question" ? "question_id" :
            kind == "experience" ? "experience_id" : "submission_id";
        (void)text(action.at("target"), field);
        (void)text(action.at("target"), "task_id");
    }
    return std::any_of(claim.at("items").as_array().begin(), claim.at("items").as_array().end(),
        [&](const auto& item) { return event_matches_workflow_action(action, item); });
}

std::vector<std::int64_t> select_persistent_event_ids(
        const CapturedModelCall& captured, const Json& original_claim) {
    const auto& items = original_claim.at("items").as_array();
    require(items.size() <= 64, ErrorCode::Quota, "persistent claim exceeds 64 original IDs");
    require(captured.request.role == Role::Dispatcher || captured.request.role == Role::TeamLead ||
            captured.request.role == Role::Worker, ErrorCode::Unauthorized,
            "persistent event selection requires an execution actor");
    if (!captured.workflow_action.is_null())
        require(captured.request.role == Role::Dispatcher &&
                text(captured.workflow_action, "kind") == "amendment", ErrorCode::Unsupported,
                "persistent event slice supports only captured model AMEND targets");
    std::vector<std::int64_t> result;
    result.reserve(items.size());
    std::int64_t previous = 0;
    for (const auto& item : items) {
        const auto item_id = number(item, "inbox_id");
        require(item_id > previous, ErrorCode::Corrupt,
                "original claim IDs must be positive and strictly increasing");
        previous = item_id;
        const auto type = text(item, "type");
        const auto subject = text(item, "subject_task_id");
        if (number(item, "event_requirements_revision") != captured.request.binding.requirements_revision) continue;
        bool selected = false;
        if (!captured.workflow_action.is_null()) {
            selected = !subject.empty() &&
                (type == "amend" || type == "recover" || type == "event_bootstrap") &&
                event_matches_workflow_action(captured.workflow_action, item);
        } else if (captured.request.role == Role::Dispatcher) {
            selected = subject.empty() && (type == "admit" || type == "amend" ||
                type == "recover" || type == "event_bootstrap");
        } else if (subject == captured.request.binding.task_id) {
            if (captured.request.role == Role::TeamLead)
                selected = type == "create_task" || type == "disposition" || type == "rebind_task" ||
                    type == "independent_plan" || type == "independent_dispositions" || type == "change_task" ||
                    type == "recover" || type == "event_bootstrap";
            else if (type == "recover" || type == "event_bootstrap" || type == "wake_worker")
                selected = true;
            else if (type == "instruction" || type == "ack" || type == "tool_feedback") {
                const Json* tuple = nullptr;
                for (const auto* section : {"result", "payload"}) {
                    const auto& source = item.at(section);
                    if (source.is_object())
                        if (const auto* value = source.as_object().if_contains("binding")) {
                            tuple = value;
                            break;
                        }
                }
                selected = tuple && *tuple == to_json(captured.request.binding);
            }
        }
        if (selected) result.push_back(item_id);
    }
    return result;
}

} // namespace wls::s1
