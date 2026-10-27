#include "wls/management_context.hpp"

#include "wls/config_loader.hpp"
#include "runtime_config_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

void require(bool valid, const std::string& message) {
    if (!valid) throw Error(ErrorCode::InvalidInput, message);
}
std::int64_t integer(const Json& value) {
    if (value.is_int64()) return value.as_int64();
    if (value.is_uint64() && value.as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(value.as_uint64());
    throw Error(ErrorCode::InvalidInput, "management history requires bounded integers");
}
std::string text(const Json& value) {
    require(value.is_string(), "management history requires text");
    return std::string(value.as_string());
}
Json integer_schema(std::int64_t low, std::int64_t high) {
    return Object{{"type", "integer"}, {"minimum", low}, {"maximum", high}};
}
std::size_t message_bytes(const std::vector<StructuredContextMessage>& messages) {
    Array encoded;
    for (const auto& message : messages) {
        Array calls;
        for (const auto& call : message.tool_calls)
            calls.push_back(Object{{"call_id", call.call_id}, {"name", call.name}, {"arguments", call.arguments}});
        encoded.push_back(Object{{"role", message.role}, {"content", message.content},
            {"tool_calls", calls}, {"tool_call_id", message.tool_call_id}, {"source", message.source}});
    }
    return canonical_json(encoded).size();
}
void validate_policy(const Json& policy) {
    require(policy.is_object() && policy.at("mode") == "window-v1", "missing frozen management window");
    const auto recent = integer(policy.at("recent_attempts"));
    const auto inline_bytes = integer(policy.at("inline_bytes"));
    const auto total = integer(policy.at("max_context_bytes"));
    require(recent >= 1 && recent <= 4 && inline_bytes >= 32768 && inline_bytes <= 65536 &&
        recent * inline_bytes <= 131072 && total >= 131072 && total <= 1048576,
        "invalid frozen management window bounds");
}
Json read_arguments(std::string_view kind, const Json& item) {
    return Object{{"kind", kind}, {"record_id", item.at("record_id")},
        {"record_sha256", item.at("record_sha256")}, {"record_version", item.at("record_version")},
        {"offset", 0}, {"max_bytes", 4096}};
}
} // namespace

bool is_management_history_control(std::string_view name) noexcept {
    return name == "management_history_page" || name == "management_history_read";
}

std::vector<ContextTool> management_history_declarations() {
    const Json kind = Object{{"type", "string"}, {"enum", Array{"question", "submission", "attempt"}}};
    const Json page = Object{{"type", "object"}, {"additionalProperties", false},
        {"required", Array{"kind", "after_sequence", "through_sequence", "limit"}},
        {"properties", Object{{"kind", kind}, {"after_sequence", integer_schema(0, INT64_MAX)},
            {"through_sequence", integer_schema(0, INT64_MAX)}, {"limit", integer_schema(1, 4)}}}};
    const Json read = Object{{"type", "object"}, {"additionalProperties", false},
        {"required", Array{"kind", "record_id", "record_sha256", "record_version", "offset", "max_bytes"}},
        {"properties", Object{{"kind", kind},
            {"record_id", Object{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
            {"record_sha256", Object{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
            {"record_version", integer_schema(1, INT64_MAX)}, {"offset", integer_schema(0, INT64_MAX)},
            {"max_bytes", integer_schema(1, 4096)}}}};
    return {{"management_history_page", "wls-management-history-v1", page},
            {"management_history_read", "wls-management-history-v1", read}};
}

Json execute_management_history_control(Store& store, const StoreActor& actor,
        const ExecutionBinding& current, std::string_view name, const Json& arguments) {
    require(is_management_history_control(name) && arguments.is_object(), "invalid management history control");
    // Authorization precedes parameter inspection; rejected settled attempts
    // still consume the persistent query allowance.
    store.authorize_management_control(actor, current, std::string(name));
    const auto& args = arguments.as_object();
    const std::set<std::string> expected = name == "management_history_page"
        ? std::set<std::string>{"kind", "after_sequence", "through_sequence", "limit"}
        : std::set<std::string>{"kind", "record_id", "record_sha256", "record_version", "offset", "max_bytes"};
    require(args.size() == expected.size(), "management history has missing or extra fields");
    for (const auto& field : args)
        require(expected.contains(std::string(field.key())), "unknown management history field");
    Json result;
    if (name == "management_history_page") {
        result = store.management_history_page(actor, current, text(args.at("kind")),
            integer(args.at("after_sequence")), integer(args.at("through_sequence")), integer(args.at("limit")));
    } else {
        result = store.read_management_history(actor, current, text(args.at("kind")), text(args.at("record_id")),
            text(args.at("record_sha256")), integer(args.at("record_version")),
            integer(args.at("offset")), integer(args.at("max_bytes")));
    }
    store.authorize_management_control(actor, current, std::string(name));
    return result;
}

std::vector<Json> visible_management_questions(Store& store, Role role,
        const std::string& request_id, const std::string& task_id) {
    return runtime_detail::visible_management_questions(store, role, request_id, task_id, nullptr);
}

std::vector<Json> runtime_detail::visible_management_questions(Store& store, Role role,
        const std::string& request_id, const std::string& task_id,
        const FrozenRequestConfig* config) {
    const auto record = store.request(request_id);
    std::optional<FrozenRequestConfig> restored;
    if (config) require_matching_config(record, *config);
    else {
        restored.emplace(FrozenRequestConfig::restore(canonical_json(record.at("config_snapshot")),
            text(record.at("config_snapshot_sha256"))));
        config = &*restored;
    }
    if (config->management_history_policy().is_null()) return store.questions(request_id, task_id);
    require((role == Role::Dispatcher && task_id.empty()) || (role == Role::TeamLead && !task_id.empty()),
        "management question scope disagrees with role");
    const auto current = task_id.empty() ? store.request_binding(request_id) : store.binding(task_id);
    const auto actor = store.host_actor(role, request_id, current.team_id);
    const auto view = store.management_view(actor, current);
    std::vector<Json> result;
    for (const auto& question : view.at("questions").as_array())
        result.push_back(question);
    return result;
}

std::vector<StructuredContextMessage> management_history_messages(const Json& view, const Json& policy) {
    validate_policy(policy);
    require(view.is_object() && view.at("format") == "wls-management-view-v1" &&
        view.at("recent_attempts").is_array() && view.at("recent_attempts").as_array().size() <=
            static_cast<std::size_t>(integer(policy.at("recent_attempts"))), "invalid management view");
    const Json index = Object{{"kind", "management_history_window"}, {"scope", view.at("scope")},
        {"policy", policy}, {"history_catalogs", view.at("history_catalogs")},
        {"not_execution_authority", true},
        {"notice", "Completed originals outside this view remain in Store. Catalogs pin insertion membership, not later state. "
                   "Page then read hash/version-pinned records; join base64 bytes and verify the complete section SHA. "
                   "Current requirements, captured targets, OPEN questions and unresolved facts remain complete in task_contract."}};
    std::vector<StructuredContextMessage> result{{"user", canonical_json(index), {}, {},
        Object{{"kind", "management_history_window"}, {"not_execution_authority", true}}}};
    for (const auto& attempt : view.at("recent_attempts").as_array()) {
        auto messages = structured_attempt_history({attempt});
        if (message_bytes(messages) <= static_cast<std::size_t>(integer(policy.at("inline_bytes")))) {
            result.insert(result.end(), messages.begin(), messages.end());
            continue;
        }
        // Unsettled or missing-feedback originals are pinned, in full, in the
        // current contract even if their duplicate recent turn is referenced.
        Object reference{{"kind", "management_history_reference"}, {"record_kind", "attempt"},
            {"record_id", attempt.at("attempt_id")}, {"sequence", attempt.at("sequence")},
            {"record_sha256", attempt.at("management_record_sha256")}, {"record_version", 1},
            {"original_binding", attempt.at("frozen_binding")}, {"status", attempt.at("status")},
            {"feedback_missing", attempt.at("feedback_missing")}, {"not_execution_authority", true}};
        if (!reference.at("record_sha256").is_null())
            reference["retrieval"] = Object{{"tool", "management_history_read"},
                {"arguments", read_arguments("attempt", reference)}};
        result.push_back({"user", canonical_json(reference), {}, {},
            Object{{"kind", "management_history_reference"}, {"not_execution_authority", true}}});
    }
    return result;
}

CompiledContext compile_management_context(const ContextInput& input,
        const std::vector<StructuredContextMessage>& messages, const Json& policy) {
    validate_policy(policy);
    const auto limit = integer(policy.at("max_context_bytes"));
    Object sections;
    const auto contract = parse_json_strict(input.task_contract, 32U * 1024U * 1024U);
    require(contract.is_object(), "management contract must be an object");
    for (const auto& section : contract.as_object())
        sections[section.key()] = canonical_json(section.value()).size();
    Object diagnostic{{"code", "management_context_capacity"}, {"limit_bytes", limit},
        {"task_contract_bytes", input.task_contract.size()}, {"history_message_bytes", message_bytes(messages)},
        {"section_bytes", sections}, {"reservation_created", false},
        {"required_action", "Resolve named outstanding facts or explicitly split/replan the current task. "
                            "Original requirements, effective constraints and captured targets cannot be clipped."}};
    const auto* catalogs = contract.as_object().if_contains("management_history_catalogs");
    if (catalogs) diagnostic["history_catalogs"] = *catalogs;
    // This larger temporary measurement ceiling never authorizes a larger
    // persisted input. The returned context still fits both the frozen bound
    // and the established 1 MiB restoration boundary.
    ContextLimits measurement;
    measurement.max_total_bytes = 32U * 1024U * 1024U;
    CompiledContext compiled;
    try { compiled = compile_structured_context(input, messages, measurement); }
    catch (const Error& error) {
        if (error.code() != ErrorCode::Quota) throw;
        diagnostic["measurement_complete"] = false;
        throw Error(ErrorCode::Quota, "management context blocked before reservation: " + canonical_json(diagnostic));
    }
    diagnostic["measurement_complete"] = true;
    diagnostic["required_context_bytes"] = compiled.prompt().size();
    diagnostic["stable_prefix_bytes"] = compiled.stable_prefix.size();
    if (compiled.prompt().size() > static_cast<std::size_t>(limit))
        throw Error(ErrorCode::Quota, "management context blocked before reservation: " + canonical_json(diagnostic));
    return compiled;
}

} // namespace wls::s1
