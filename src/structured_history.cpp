#include "wls/structured_context.hpp"
#include "wls/model.hpp"

#include <algorithm>

namespace wls::s1 {
namespace {
bool valid_record_hash(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}
} // namespace

std::vector<StructuredContextMessage> structured_attempt_history(const std::vector<Json>& history) {
    using Object = boost::json::object;
    std::vector<StructuredContextMessage> messages;
    for (const auto& attempt : history) {
        if (!attempt.is_object()) throw Error(ErrorCode::Corrupt, "history record must be an object");
        const auto& record = attempt.as_object();
        for (const auto* key : {"feedback", "response", "frozen_binding", "accepted", "attempt_id", "call_id"})
            if (!record.contains(key)) throw Error(ErrorCode::Corrupt, "incomplete original history record");
        if (!attempt.at("attempt_id").is_string() || !attempt.at("call_id").is_string() ||
            !attempt.at("frozen_binding").is_object())
            throw Error(ErrorCode::Corrupt, "invalid original history identity");
        const auto& feedback = attempt.at("feedback");
        if (!feedback.is_null()) {
            if (!feedback.is_object()) throw Error(ErrorCode::Corrupt, "invalid original feedback");
            for (const auto* key : {"original_binding", "response", "result", "record_sha256"})
                if (!feedback.as_object().contains(key))
                    throw Error(ErrorCode::Corrupt, "incomplete original feedback");
            if (!feedback.at("record_sha256").is_string() ||
                !valid_record_hash(std::string(feedback.at("record_sha256").as_string())) ||
                feedback.at("original_binding") != attempt.at("frozen_binding") ||
                feedback.at("response") != attempt.at("response"))
                throw Error(ErrorCode::Corrupt, "history feedback differs from the original call");
        }
        // The Store already verified the immutable feedback fingerprint. The
        // pair below additionally binds it to the original attempt/response.
        bool paired = false;
        ModelResponse response;
        try {
            response = model_response_from_json(attempt.at("response"));
            const auto& accepted = attempt.at("accepted");
            paired = !feedback.is_null() && (accepted == 1 || accepted == true) &&
                     response.status == ModelResultStatus::Completed && response.tool_calls.size() == 1 &&
                     response.attempt_id == std::string(attempt.at("attempt_id").as_string()) &&
                     response.call_id == std::string(attempt.at("call_id").as_string()) &&
                     to_json(response.binding) == attempt.at("frozen_binding");
            if (paired) {
                const auto& result = feedback.at("result");
                const auto* error = result.is_object() ? result.as_object().if_contains("runtime_error") : nullptr;
                paired = !(error && *error == true);
            }
        } catch (const Error& error) {
            if (error.code() != ErrorCode::InvalidInput) throw;
            // Historical transport/host records need not have a complete model
            // envelope. Preserve them verbatim as observations, never tool use.
        }
        if (!paired) {
            Object original = attempt.as_object(); original.erase("request_envelope");
            messages.push_back({"user", canonical_json(original), {}, {},
                Object{{"kind", "unapplied_or_missing_observation"}, {"not_execution_authority", true}}});
            continue;
        }
        const auto& call = response.tool_calls.front();
        const auto id = "history_" + sha256_hex(canonical_json(Object{
            {"format", "wls-history-tool-id-v1"}, {"attempt_id", response.attempt_id}, {"call_id", call.call_id}}));
        Object source = attempt.as_object();
        source.erase("request_envelope"); source.erase("response"); source.erase("feedback");
        source["record_sha256"] = feedback.at("record_sha256");
        source["original_tool_call_id"] = call.call_id;
        source["kind"] = "recorded_tool_turn"; source["not_execution_authority"] = true;
        messages.push_back({"assistant", response.text, {{id, call.name, call.arguments}}, {}, source});
        messages.push_back({"tool", canonical_json(feedback.at("result")), {}, id, source});
    }
    return messages;
}
} // namespace wls::s1
