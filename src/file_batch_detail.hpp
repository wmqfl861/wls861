#pragma once

#include "wls/model.hpp"
#include "wls/structured_context.hpp"
#include "correction_detail.hpp"

#include <algorithm>
#include <set>

namespace wls::s1::file_batch_detail {
inline void require(bool valid) {
    if (!valid) throw Error(ErrorCode::Corrupt, "inconsistent original file batch feedback");
}
inline bool enabled(const FileBatchHistoryContext* host) {
    return host && host->role == Role::Worker && host->request_format == "structured-v3" &&
        host->tool_policy == "fixed-role-file-batch-v1";
}
inline void fields(const Json& value, std::initializer_list<std::string_view> required,
                   std::initializer_list<std::string_view> optional = {}) {
    require(value.is_object());
    for (const auto key : required) require(value.as_object().contains(key));
    for (const auto& field : value.as_object())
        require(std::find(required.begin(), required.end(), field.key()) != required.end() ||
            std::find(optional.begin(), optional.end(), field.key()) != optional.end());
}
inline const Json* validate(const Json& attempt, const ModelResponse& response,
                            const FileBatchHistoryContext* host) {
    if (!enabled(host) || attempt.at("feedback").is_null()) return nullptr;
    const auto& feedback = attempt.at("feedback");
    const auto& batch = feedback.at("result");
    const bool marked = batch.is_object() && ((batch.as_object().contains("format") &&
        batch.at("format") == "wls-file-batch-feedback-v1") ||
        (response.tool_calls.size() > 1 && batch.as_object().contains("items")));
    const auto* rejected = correction_detail::correction(batch);
    if (!marked && (response.tool_calls.size() <= 1 || (rejected && rejected->as_object().contains("reason") &&
        rejected->at("reason") == "tool_selection"))) return nullptr;
    try {
        fields(batch, {"format", "status", "runtime_error", "observation_complete", "items"},
            {"error_code", "message", "retry_of_attempt_id", "correction_feedback"});
        require(batch.at("format") == "wls-file-batch-feedback-v1" && batch.at("runtime_error").is_bool() &&
            batch.at("observation_complete").is_bool() && batch.at("items").is_array());
        for (const auto* key : {"error_code", "message", "retry_of_attempt_id"})
            if (const auto* value = batch.as_object().if_contains(key)) require(value->is_string() && !value->as_string().empty());
        require(response.role == Role::Worker && response.status == ModelResultStatus::Completed &&
            response.tool_calls.size() == 2 && batch.at("items").as_array().size() == 2 &&
            response.binding.request_id == host->request_id && !host->request_id.empty() &&
            host->config_snapshot_sha256.size() == 64 && response.config_snapshot_id == host->config_snapshot_sha256 &&
            attempt.at("status") == "COMPLETED" && (attempt.at("accepted") == 1 || attempt.at("accepted") == true));
        require(Json(response.attempt_id) == attempt.at("attempt_id") && Json(response.call_id) == attempt.at("call_id") &&
            to_json(response.binding) == attempt.at("frozen_binding") &&
            std::all_of(host->config_snapshot_sha256.begin(), host->config_snapshot_sha256.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }));
        for (const auto& field : boost::json::object{{"request_id", host->request_id}, {"role", "worker"},
                {"config_snapshot_sha256", host->config_snapshot_sha256}})
            if (const auto* original = attempt.as_object().if_contains(field.key())) require(*original == field.value());
        // Oversized originals remain readable only as an entirely unexecuted
        // rejection. This grants no capacity to execution/partial/UNKNOWN facts.
        const bool unexecuted = std::all_of(batch.at("items").as_array().begin(), batch.at("items").as_array().end(),
            [](const auto& item) { return item.at("state") == "NOT_EXECUTED" && item.at("result").is_null(); });
        require((unexecuted || canonical_json(attempt.at("response")).size() <= 128U * 1024U) &&
            canonical_json(batch).size() <= 512U * 1024U);
        require(feedback.at("record_sha256") == Json(sha256_hex(canonical_json(boost::json::object{
            {"attempt_id", attempt.at("attempt_id")}, {"original_binding", feedback.at("original_binding")},
            {"response", feedback.at("response")}, {"result", batch}}))));
        std::set<std::string> calls, operations;
        bool stopped = false, complete = true, correctable = false;
        Json status = "SUCCEEDED";
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& call = response.tool_calls[i];
            const auto& item = batch.at("items").as_array()[i];
            fields(item, {"call_id", "name", "state", "result", "error"});
            require(!call.call_id.empty() && calls.insert(call.call_id).second && item.at("call_id") == Json(call.call_id) &&
                item.at("name") == Json(call.name) && (call.name == "fs_write" || call.name == "fs_patch" || call.name == "fs_mkdir"));
            const auto& state = item.at("state");
            const auto& result = item.at("result");
            require(state == "SUCCEEDED" || state == "FAILED" || state == "CANCELLED" || state == "UNKNOWN" || state == "NOT_EXECUTED");
            require(!stopped || state == "NOT_EXECUTED");
            if (state != "SUCCEEDED" && !stopped) { status = state; stopped = true; }
            if (state == "UNKNOWN") complete = false;
            if (!item.at("error").is_null()) {
                require(state != "SUCCEEDED");
                fields(item.at("error"), {"code", "message"});
                require(item.at("error").at("code").is_string() && !item.at("error").at("code").as_string().empty() &&
                    item.at("error").at("message").is_string() && !item.at("error").at("message").as_string().empty());
            }
            if (result.is_null()) {
                require((state == "UNKNOWN" || state == "NOT_EXECUTED") && !item.at("error").is_null());
                continue;
            }
            fields(result, {"operation", "execution_result", "executed_now", "replayed"});
            require(state != "NOT_EXECUTED" && result.at("executed_now").is_bool() && result.at("replayed").is_bool() &&
                result.at("executed_now") != result.at("replayed"));
            const auto& op = result.at("operation");
            const auto& execution = result.at("execution_result");
            require(op.at("op_id").is_string() && !op.at("op_id").as_string().empty() &&
                operations.insert(std::string(op.at("op_id").as_string())).second && op.at("tool_id") == Json(call.name) &&
                op.at("side_effect") == "workspace" && op.at("arguments") == call.arguments &&
                op.at("args_sha256") == Json(sha256_hex(canonical_json(call.arguments))) &&
                op.at("business_idempotency_key") == Json("batch:" + sha256_hex(canonical_json(boost::json::object{
                    {"attempt_id", response.attempt_id}, {"tool_call_id", call.call_id}}))));
            require(op.at("status") == "PREPARED" || op.at("status") == "RUNNING" || op.at("status") == "UNKNOWN" ||
                op.at("status") == "SUCCEEDED" || op.at("status") == "FAILED" || op.at("status") == "CANCELLED");
            const auto binding = to_json(response.binding);
            for (const auto& field : binding.as_object()) require(op.at(field.key()) == field.value());
            if (execution.is_null()) { require(state == "UNKNOWN"); continue; }
            require(execution.at("op_id") == op.at("op_id") && execution.at("original_binding") == binding &&
                execution.at("operation_status") == op.at("status") && !op.at("started_at").is_null() &&
                (execution.at("accepted") == 0 || execution.at("accepted") == 1));
            require(execution.at("observed_outcome") == "SUCCEEDED" || execution.at("observed_outcome") == "FAILED" ||
                execution.at("observed_outcome") == "CANCELLED" || execution.at("observed_outcome") == "UNKNOWN");
            boost::json::object fact{{"outcome", execution.at("observed_outcome")}};
            for (const auto* key : {"op_id", "original_binding", "result", "evidence", "stdout_bytes", "stderr_bytes", "stdout_sha256", "stderr_sha256"})
                fact[key] = execution.at(key);
            for (const auto* key : {"stdout_truncated", "stderr_truncated"}) {
                require(execution.at(key) == 0 || execution.at(key) == 1);
                fact[key] = execution.at(key) == 1;
            }
            require(execution.at("record_sha256") == Json(sha256_hex(canonical_json(fact))));
            require(op.at("evidence_ref") == Json("store:operation_result:" + std::string(op.at("op_id").as_string()) +
                "#" + std::string(execution.at("record_sha256").as_string())));
            const bool known = result.at("executed_now") == true && execution.at("accepted") == 1 &&
                (op.at("status") == "SUCCEEDED" || op.at("status") == "FAILED" || op.at("status") == "CANCELLED");
            require(known ? state == op.at("status") && state == execution.at("observed_outcome") : state == "UNKNOWN");
            if (state == "FAILED") correctable = correction_detail::definite_failure(result);
        }
        const auto* correction = correction_detail::correction(batch);
        if (batch.as_object().contains("correction_feedback"))
            require(correction && correctable && status == "FAILED" && correction->at("reason") == "tool_result");
        require(batch.at("status") == status && batch.at("observation_complete") == complete &&
            (status == "SUCCEEDED" ? batch.at("runtime_error") == false : batch.at("runtime_error") == true || correction));
        return &batch;
    } catch (const Error& error) {
        if (error.code() == ErrorCode::Corrupt) throw;
        throw Error(ErrorCode::Corrupt, "invalid original file batch feedback");
    } catch (const std::exception&) { throw Error(ErrorCode::Corrupt, "malformed original file batch feedback"); }
}
} // namespace wls::s1::file_batch_detail
