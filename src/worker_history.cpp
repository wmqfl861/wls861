#include "wls/worker_history.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
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
    throw Error(ErrorCode::InvalidInput, "history control requires bounded integers");
}
std::string text(const Json& value) {
    require(value.is_string(), "history control requires text");
    return std::string(value.as_string());
}
Json integer_schema(std::int64_t low, std::int64_t high) {
    return Object{{"type", "integer"}, {"minimum", low}, {"maximum", high}};
}
std::size_t message_size(const std::vector<StructuredContextMessage>& messages) {
    Array result;
    for (const auto& message : messages) {
        Array calls;
        for (const auto& call : message.tool_calls)
            calls.push_back(Object{{"call_id", call.call_id}, {"name", call.name}, {"arguments", call.arguments}});
        result.push_back(Object{{"role", message.role}, {"content", message.content},
            {"tool_calls", calls}, {"tool_call_id", message.tool_call_id}, {"source", message.source}});
    }
    return canonical_json(result).size();
}
} // namespace

bool is_history_control(std::string_view name) noexcept {
    return name == "history_page" || name == "history_read";
}

std::vector<ContextTool> history_control_declarations() {
    const Json page = Object{{"type", "object"}, {"additionalProperties", false},
        {"required", Array{"after_sequence", "through_sequence", "limit"}},
        {"properties", Object{{"after_sequence", integer_schema(0, INT64_MAX)},
            {"through_sequence", integer_schema(0, INT64_MAX)}, {"limit", integer_schema(1, 4)}}}};
    const Json read = Object{{"type", "object"}, {"additionalProperties", false},
        {"required", Array{"attempt_id", "record_sha256", "section", "offset", "max_bytes"}},
        {"properties", Object{{"attempt_id", Object{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
            {"record_sha256", Object{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
            {"section", Object{{"type", "string"}, {"enum", Array{"response", "result"}}}},
            {"offset", integer_schema(0, INT64_MAX)}, {"max_bytes", integer_schema(1, 4096)}}}};
    return {{"history_page", "wls-history-control-v1", page}, {"history_read", "wls-history-control-v1", read}};
}

Json execute_history_control(Store& store, const StoreActor& actor,
        const ExecutionBinding& binding, std::string_view name, const Json& arguments) {
    require(is_history_control(name) && arguments.is_object(), "invalid history control");
    const auto& args = arguments.as_object();
    const std::set<std::string> expected = name == "history_page"
        ? std::set<std::string>{"after_sequence", "through_sequence", "limit"}
        : std::set<std::string>{"attempt_id", "record_sha256", "section", "offset", "max_bytes"};
    require(args.size() == expected.size(), "history control has missing or extra fields");
    for (const auto& entry : args)
        require(expected.contains(std::string(entry.key())), "unknown history control field");
    store.authorize_history_control(actor, binding, std::string(name));
    Json result;
    if (name == "history_page") {
        const auto limit = integer(args.at("limit"));
        require(limit >= 1 && limit <= 4, "model history page limit must be 1..4");
        result = store.attempt_history_page(actor, integer(args.at("after_sequence")),
            integer(args.at("through_sequence")), limit);
    } else {
        const auto maximum = integer(args.at("max_bytes"));
        require(maximum >= 1 && maximum <= 4096, "model history read max_bytes must be 1..4096");
        result = store.read_history_feedback(actor, text(args.at("attempt_id")), text(args.at("record_sha256")),
            text(args.at("section")), integer(args.at("offset")), maximum);
    }
    // A policy/version change during a read must not expose the captured result.
    store.authorize_history_control(actor, binding, std::string(name));
    return result;
}

std::vector<StructuredContextMessage> worker_history_messages(const Json& tail, const Json& policy) {
    require(policy.is_object() && policy.at("mode") == "window-v1", "missing frozen history window policy");
    const auto recent = integer(policy.at("recent_attempts"));
    const auto inline_bytes = integer(policy.at("inline_bytes"));
    require(recent >= 1 && recent <= 8 && inline_bytes >= 32768 && inline_bytes <= 65536 &&
        recent * inline_bytes <= 196608, "invalid frozen history window bounds");
    require(tail.is_object() && tail.at("format") == "wls-history-tail-v1" && tail.at("items").is_array() &&
        tail.at("items").as_array().size() <= static_cast<std::size_t>(recent), "invalid scoped history tail");
    const auto total = integer(tail.at("total_count"));
    const auto omitted = integer(tail.at("omitted_count"));
    require(total >= 0 && omitted >= 0 && omitted == total -
        static_cast<std::int64_t>(tail.at("items").as_array().size()), "inconsistent history omission count");
    Object index = tail.as_object(); index.erase("items");
    index["kind"] = "history_window"; index["policy"] = policy;
    index["not_execution_authority"] = true;
    index["retrieval"] = Object{{"tool", "history_page"}, {"arguments", Object{
        {"after_sequence", 0}, {"through_sequence", tail.at("through_sequence")}, {"limit", 4}}}};
    index["notice"] = "Older attempts are omitted from this view, not deleted or summarized. "
        "Large turns have hash-pinned references. Use history_page/history_read for original evidence. "
        "Missing feedback is unknown, never tool success. Current requirements and unresolved state remain in task_contract.";
    std::vector<StructuredContextMessage> result{{"user", canonical_json(index), {}, {},
        Object{{"kind", "history_window"}, {"not_execution_authority", true}}}};
    for (const auto& attempt : tail.at("items").as_array()) {
        auto messages = structured_attempt_history({attempt});
        if (message_size(messages) <= static_cast<std::size_t>(inline_bytes)) {
            result.insert(result.end(), messages.begin(), messages.end());
            continue;
        }
        Object ref{{"kind", "history_reference"}, {"not_execution_authority", true},
            {"attempt_id", attempt.at("attempt_id")}, {"call_id", attempt.at("call_id")},
            {"sequence", attempt.at("sequence")}, {"original_binding", attempt.at("frozen_binding")},
            {"status", attempt.at("status")}, {"accepted", attempt.at("accepted")},
            {"accepted_is_business_success", false}, {"feedback_missing", attempt.at("feedback_missing")},
            {"record_sha256", nullptr}};
        const auto& feedback = attempt.at("feedback");
        if (!feedback.is_null()) {
            ref["record_sha256"] = feedback.at("record_sha256");
            for (const auto* section : {"response", "result"}) {
                const auto bytes = canonical_json(feedback.at(section));
                ref[section] = Object{{"size_bytes", bytes.size()}, {"section_sha256", sha256_hex(bytes)}};
            }
            ref["retrieval"] = Object{{"tool", "history_read"}, {"arguments", Object{
                {"attempt_id", attempt.at("attempt_id")}, {"record_sha256", feedback.at("record_sha256")},
                {"section", "result"}, {"offset", 0}, {"max_bytes", 4096}}}};
        }
        result.push_back({"user", canonical_json(ref), {}, {},
            Object{{"kind", "history_reference"}, {"not_execution_authority", true}}});
    }
    return result;
}
} // namespace wls::s1
