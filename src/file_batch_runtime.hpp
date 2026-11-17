#pragma once

#include "file_batch_detail.hpp"
#include "wls/single_agent.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wls::s1::file_batch_detail {
using Object = boost::json::object;
using Array = boost::json::array;
inline void capacity_require(bool value, ErrorCode code = ErrorCode::Quota, std::string_view message = "file batch capacity exceeded") {
    if (!value) throw Error(code, std::string(message));
}

// This is a size ledger, not a fabricated receipt. It uses the two captured
// calls, their actual arguments, the fixed host binding and producer-defined
// field widths to reject a batch before prepare/claim/effect. The real receipt
// is built only from WorkerTools' persisted operation facts after execution.
inline std::array<std::size_t, 3> preflight_capacity(const ModelResponse& response, const Json& settled, const Json& initial,
                               const Json& history_policy, const Json& correction) {
    capacity_require(response.tool_calls.size() == 2, ErrorCode::InvalidInput, "file batch needs exactly two calls");
    const auto response_size = canonical_json(to_json(response)).size();
    capacity_require(response_size <= 128U * 1024U);
    const auto binding = to_json(response.binding);
    const std::string hash(64, 'f'), id(29, 'x'), timestamp(32, 'x');
    // Workspace's longest fixed diagnostic is 144 bytes. Syscall failures add
    // only a fixed action and the active locale's strerror for Linux errno
    // 1..4095. No pathname/content is interpolated. Allocation/storage faults
    // remain unresolved originals and may stop without feedback.
    std::size_t diagnostic = 144;
    for (int error = 1; error <= 4095; ++error)
        diagnostic = std::max(diagnostic, std::size_t(144 + std::strlen(std::strerror(error))));
    const std::string message(diagnostic, '\1');
    std::array<Json, 2> receipts;
    std::array<Json, 2> items;
    for (std::size_t i = 0; i != 2; ++i) {
        const auto& call = response.tool_calls[i];
        const auto path = std::string(call.arguments.at("path").as_string());
        const auto slash = path.rfind('/');
        const auto temporary = (slash == std::string::npos ? std::string{} : path.substr(0, slash + 1)) +
            ".wls-tmp-" + std::string(30, 'x');
        Object operation = binding.as_object();
        for (const auto& field : Object{{"op_id", id}, {"tool_id", "fs_write"},
                {"business_idempotency_key", "batch:" + hash}, {"args_sha256", hash},
                {"arguments", call.arguments}, {"side_effect", "workspace"}, {"status", "SUCCEEDED"},
                {"exit_code", nullptr}, {"error_signature", nullptr}, {"output_dir", nullptr},
                {"state_version", INT64_MAX}, {"created_at", timestamp}, {"started_at", timestamp},
                {"ended_at", timestamp}, {"evidence_ref", "store:operation_result:" + id + "#" + hash}})
            operation[field.key()] = field.value();
        const Object result{{"path", path}, {"sha256", hash}, {"size_bytes", UINT64_MAX},
            {"created", false}, {"previous_sha256", hash}, {"error", "cancelled_before_effect"}, {"message", message}};
        const Object evidence{{"path", path}, {"before_sha256", hash}, {"after_sha256", hash},
            {"size_bytes", UINT64_MAX}, {"root_device", UINT64_MAX}, {"root_inode", UINT64_MAX},
            {"temporary_path", temporary}, {"phase", "directory_sync"}, {"error", message},
            {"cleanup_error", message}, {"cause_code", "unknown_outcome"}, {"failure_kind", "content_precondition"},
            {"rename_attempted", false}, {"renamed", false}, {"durable", false},
            {"temporary_remaining", false}, {"temporary_cleanup_complete", false},
            {"effect_started", false}, {"kind", "executor_error"}, {"effect_outcome", "not_confirmed"}};
        const Object execution{{"op_id", id}, {"original_binding", binding}, {"observed_outcome", "SUCCEEDED"},
            {"accepted", 1}, {"result", result}, {"evidence", evidence}, {"stdout_bytes", 0},
            {"stderr_bytes", 0}, {"stdout_truncated", 0}, {"stderr_truncated", 0},
            {"stdout_sha256", hash}, {"stderr_sha256", hash}, {"record_sha256", hash},
            {"recorded_at", timestamp}, {"operation_status", "SUCCEEDED"}};
        receipts[i] = Object{{"operation", operation}, {"execution_result", execution},
            {"executed_now", false}, {"replayed", false}};
        items[i] = Object{{"call_id", call.call_id}, {"name", call.name}, {"state", "NOT_EXECUTED"},
            {"result", receipts[i]}, {"error", Object{{"code", "unknown_outcome"}, {"message", message}}}};
    }
    auto batch = initial;
    batch.at("items") = Array{items[0], items[1]};
    batch.at("error_code") = "unknown_outcome"; batch.at("message") = message;
    batch.at("observation_complete") = false; batch.at("runtime_error") = false; // Longest JSON booleans.
    batch.as_object()["correction_feedback"] = correction.at("correction_feedback");
    const auto aggregate_size = canonical_json(batch).size();
    capacity_require(aggregate_size <= 512U * 1024U);
    // settled contains every scalar selected by full history; delta and tail
    // add only the fields below. Remove its recursive original input/response.
    auto source = settled.as_object();
    source.erase("request_envelope"); source.erase("response");
    for (const auto& field : Object{{"role_call_index", response.role_call_index}, {"response_size", INT64_MAX},
            {"feedback_missing", false}, {"record_sha256", hash}, {"kind", "recorded_tool_turn"},
            {"not_execution_authority", true}, {"sequence", INT64_MAX}, {"accounting_attention", 0}})
        source[field.key()] = field.value();
    auto turn = source;
    turn["response"] = to_json(response);
    turn["feedback"] = Object{{"attempt_id", response.attempt_id}, {"original_binding", binding},
        {"response", to_json(response)}, {"result", batch}, {"record_sha256", hash}, {"recorded_at", timestamp}};
    capacity_require(canonical_json(Array{turn}).size() <= 1024U * 1024U);
    std::vector<StructuredContextMessage> messages;
    StructuredContextMessage assistant{"assistant", response.text, {}, {}, source, response.responses_output};
    for (const auto& call : response.tool_calls)
        assistant.tool_calls.push_back({"history_" + hash, call.name, call.arguments});
    messages.push_back(std::move(assistant));
    for (const auto& item : items) {
        auto item_source = source;
        item_source["original_tool_call_id"] = item.at("call_id");
        messages.push_back({"tool", canonical_json(item), {}, "history_" + hash, item_source});
    }
    const auto projected = canonical_json(single_agent_messages_json(messages)).size();
    const auto inline_limit = history_policy.is_object() && history_policy.at("mode") == "window-v1"
        ? history_policy.at("inline_bytes").to_number<std::size_t>() : 1024U * 1024U;
    capacity_require(projected <= 1024U * 1024U && projected <= inline_limit, ErrorCode::Quota,
        "complete file batch receipt/history exceeds frozen context capacity");
    return {canonical_json(receipts[0]).size(), canonical_json(receipts[1]).size(), diagnostic};
}

} // namespace wls::s1::file_batch_detail
