#include "wls/worker_history.hpp"
#include "wls/config_loader.hpp"
#include "wls/single_agent.hpp"

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
        auto encoded = Object{{"role", message.role}, {"content", message.content},
            {"tool_calls", calls}, {"tool_call_id", message.tool_call_id}, {"source", message.source}};
        // Opaque originals count toward window capacity; they are never free.
        if (!message.responses_output.is_null()) encoded["responses_output"] = message.responses_output;
        result.push_back(std::move(encoded));
    }
    return canonical_json(result).size();
}
} // namespace

bool is_history_control(std::string_view name) noexcept {
    return name == "history_page" || name == "history_read";
}

bool append_fold_policy(const Json& policy) {
    return policy.is_object() && policy.at("mode") == "append-fold-v1";
}

std::vector<ContextTool> history_control_declarations(const Json& policy) {
    const Json page = Object{{"type", "object"}, {"additionalProperties", false},
        {"required", Array{"after_sequence", "through_sequence", "limit"}},
        {"properties", Object{{"after_sequence", integer_schema(0, INT64_MAX)},
            {"through_sequence", integer_schema(0, INT64_MAX)}, {"limit", integer_schema(1, 4)}}}};
    Json read = Object{{"type", "object"}, {"additionalProperties", false},
        {"required", Array{"attempt_id", "record_sha256", "section", "offset", "max_bytes"}},
        {"properties", Object{{"attempt_id", Object{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
            {"record_sha256", Object{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
            {"section", Object{{"type", "string"}, {"enum", Array{"response", "result"}}}},
            {"offset", integer_schema(0, INT64_MAX)}, {"max_bytes", integer_schema(1, 4096)}}}};
    if (append_fold_policy(policy)) read = Object{{"type", "object"}, {"additionalProperties", false},
        {"required", Array{"sequence", "section", "offset"}}, {"properties", Object{
            {"sequence", integer_schema(1, INT64_MAX)},
            {"section", Object{{"type", "string"}, {"enum", Array{"response", "result", "result_data"}}}},
            {"offset", integer_schema(0, INT64_MAX)}, {"max_bytes", integer_schema(1, integer(policy.at("read_max_bytes")))}}}};
    return {{"history_page", "wls-history-control-v1", page, builtin_tool_description("history_page")},
            {"history_read", append_fold_policy(policy) ? "wls-history-text-v1" : "wls-history-control-v1",
                read, builtin_tool_description("history_read")}};
}

Json execute_history_control(Store& store, const StoreActor& actor,
        const ExecutionBinding& binding, std::string_view name, const Json& arguments) {
    require(is_history_control(name) && arguments.is_object(), "invalid history control");
    const auto& args = arguments.as_object();
    const auto policy = restore_request_config(store.request(actor.request_id)).worker_history_policy();
    if (name == "history_read" && append_fold_policy(policy)) {
        require((args.size() == 3 || (args.size() == 4 && args.contains("max_bytes"))) &&
            args.contains("sequence") && args.contains("section") && args.contains("offset"), "invalid text history read fields");
        store.authorize_history_control(actor, binding, std::string(name));
        const auto result = store.read_history_sequence(actor, integer(args.at("sequence")), text(args.at("section")),
            integer(args.at("offset")), args.contains("max_bytes") ? integer(args.at("max_bytes")) : integer(policy.at("read_max_bytes")));
        store.authorize_history_control(actor, binding, std::string(name));
        return result;
    }
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

Json append_fold_history(const Json& previous, const Json& delta, bool fold) {
    Json result = previous.is_null() ? Json(Object{{"scope", delta.at("scope")}, {"last_sequence", 0},
        {"fold_generation", 0}, {"visible_turns", Array{}}, {"folded_entries", Array{}}}) : previous.at("worker_history");
    require(result.is_object() && result.as_object().size() == 5 && result.at("scope") == delta.at("scope"),
        "append-fold history scope or shape changed");
    const auto before = integer(result.at("last_sequence"));
    auto& turns = result.at("visible_turns").as_array();
    auto& entries = result.at("folded_entries").as_array();
    Array messages = previous.is_null() ? Array{} : previous.at("messages").as_array();
    if (!entries.empty()) { require(!messages.empty(), "missing persisted fold record"); messages.erase(messages.begin()); }
    require(turns.size() <= 128 && entries.size() <= 200 && integer(result.at("fold_generation")) >= 0 && integer(result.at("fold_generation")) <= 200,
        "invalid append-fold projection bounds");
    std::int64_t sequence = 0;
    for (const auto& entry : entries) {
        require(integer(entry.at("sequence")) > sequence && integer(entry.at("sequence")) <= before,
            "folded sequence is not chronological");
        sequence = integer(entry.at("sequence"));
    }
    std::size_t consumed = 0;
    for (const auto& turn : turns) {
        require(integer(turn.at("sequence")) > sequence && integer(turn.at("sequence")) <= before &&
            integer(turn.at("message_count")) > 0 && integer(turn.at("message_count")) <= 256,
            "visible sequence or message bound is invalid");
        sequence = integer(turn.at("sequence"));
        consumed += static_cast<std::size_t>(integer(turn.at("message_count")));
    }
    require(consumed == messages.size(), "visible original turn boundaries changed");
    require(delta.at("items").as_array().size() <= 64, "append-fold delta exceeds bounded membership");
    for (const auto& turn : delta.at("items").as_array()) {
        require(integer(turn.at("sequence")) > std::max(sequence, before), "append-fold delta overlaps consumed originals");
        sequence = integer(turn.at("sequence"));
        const auto& feedback = turn.at("feedback");
        if (!feedback.is_null()) require(std::string(feedback.at("record_sha256").as_string()) == sha256_hex(canonical_json(Object{
            {"attempt_id", turn.at("attempt_id")}, {"original_binding", turn.at("frozen_binding")},
            {"response", turn.at("response")}, {"result", feedback.at("result")}})), "append-fold original feedback hash changed");
        const auto native = structured_attempt_history({turn});
        Array tools;
        try { for (const auto& call : model_response_from_json(turn.at("response")).tool_calls) tools.emplace_back(call.name); }
        catch (const Error& error) { if (error.code() != ErrorCode::InvalidInput) throw; }
        Object outcome{{"attempt_status", turn.at("status")}, {"accepted", turn.at("accepted")}, {"accepted_is_business_success", false}};
        if (!feedback.is_null() && feedback.at("result").is_object())
            for (const auto* key : {"status", "runtime_error", "error_code", "kind"})
                if (const auto* value = feedback.at("result").as_object().if_contains(key))
                    if (canonical_json(*value).size() <= 128) outcome[key] = *value;
        const bool reasoning = std::any_of(native.begin(), native.end(), [](const auto& m) { return !m.responses_output.is_null(); });
        const bool completed = turn.at("status") != "PENDING" && turn.at("status") != "UNKNOWN" &&
            !feedback.is_null() && turn.at("accounting_attention") == 0;
        turns.push_back(Object{{"sequence", sequence}, {"attempt_id", turn.at("attempt_id")}, {"tools", tools},
            {"outcome", canonical_json(outcome)},
            {"record_sha256", feedback.is_null() ? Json(nullptr) : feedback.at("record_sha256")},
            {"completed", completed}, {"native_reasoning", reasoning}, {"message_count", native.size()}});
        const auto encoded = single_agent_messages_json(native);
        for (const auto& message : encoded.as_array()) messages.push_back(message);
    }
    require(sequence == integer(delta.at("through_sequence")) && (before == 0 || sequence >= before),
        "append-fold high watermark differs from consumed originals");
    result.at("last_sequence") = sequence;
    std::size_t latest_reasoning = turns.size();
    for (std::size_t i = 0; i < turns.size(); ++i) if (turns[i].at("native_reasoning") == true) latest_reasoning = i;
    if (fold) {
        require(sequence > before, "a persisted fold boundary cannot fold again without growth");
        std::size_t eligible = 0;
        // Only a completed prefix may fold; unresolved facts and its order remain native.
        while (eligible < turns.size() && eligible != latest_reasoning && turns[eligible].at("completed") == true) ++eligible;
        const auto count = eligible / 2;
        if (!count) throw Error(ErrorCode::Quota, "no completed history prefix can restore context capacity");
        std::size_t removed = 0;
        for (std::size_t i = 0; i < count; ++i) {
            auto entry = turns[i].as_object();
            removed += static_cast<std::size_t>(integer(entry.at("message_count")));
            for (const auto* key : {"completed", "native_reasoning", "message_count"}) entry.erase(key);
            entry["readback"] = Object{{"sequence", entry.at("sequence")}, {"section", "result"}, {"offset", 0}};
            entries.push_back(std::move(entry));
        }
        turns.erase(turns.begin(), turns.begin() + static_cast<std::ptrdiff_t>(count));
        messages.erase(messages.begin(), messages.begin() + static_cast<std::ptrdiff_t>(removed));
        result.at("fold_generation") = integer(result.at("fold_generation")) + 1;
    }
    if (!entries.empty()) {
        const StructuredContextMessage summary{"user", canonical_json(Object{{"kind", "history_fold"},
            {"from_sequence", entries.front().at("sequence")}, {"through_sequence", entries.back().at("sequence")},
            {"entries", entries}}), {}, {}, Object{{"kind", "history_fold"}, {"sequence", entries.back().at("sequence")}}};
        messages.insert(messages.begin(), single_agent_messages_json({summary}).as_array().front());
    }
    result.as_object()["messages"] = std::move(messages); // Split at compilation; originals are persisted once.
    return result;
}

std::vector<StructuredContextMessage> append_fold_messages(const Json& projection) {
    std::vector<StructuredContextMessage> result;
    for (const auto& item : projection.at("messages").as_array()) {
        StructuredContextMessage message{text(item.at("role")), text(item.at("content")), {},
            text(item.at("tool_call_id")), item.at("source")};
        for (const auto& call : item.at("tool_calls").as_array())
            message.tool_calls.push_back({text(call.at("call_id")), text(call.at("name")), call.at("arguments")});
        if (const auto* payload = item.as_object().if_contains("responses_output")) message.responses_output = *payload;
        result.push_back(std::move(message));
    }
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
    const auto& tail_items = tail.at("items").as_array();
    for (std::size_t tail_index = 0; tail_index < tail_items.size(); ++tail_index) {
        const auto& attempt = tail_items[tail_index];
        auto messages = structured_attempt_history({attempt});
        const bool reasoning_bearing = std::any_of(messages.begin(), messages.end(),
            [](const StructuredContextMessage& message) { return !message.responses_output.is_null(); });
        if (message_size(messages) <= static_cast<std::size_t>(inline_bytes)) {
            result.insert(result.end(), messages.begin(), messages.end());
            continue;
        }
        // The most recent reasoning-bearing round must not degrade into a
        // reasoning-less reference: fail before any reservation instead.
        // Older completed rounds keep the original reference policy.
        if (reasoning_bearing && tail_index + 1 == tail_items.size())
            throw Error(ErrorCode::Quota, "recent reasoning round exceeds the frozen inline window");
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
