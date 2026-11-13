#pragma once

#include "wls/store.hpp"
#include "wls/model.hpp"
#include "wls/process_evidence.hpp"

namespace wls::s1::correction_detail {
inline constexpr const char* limit_reason = "correction_limit: frozen consecutive failure bound reached";
class RejectedArguments final : public Error {
public:
    explicit RejectedArguments(const std::string& message) : Error(ErrorCode::InvalidInput, message) {}
};
class RejectedRead final : public Error {
public:
    explicit RejectedRead(const Error& error) : Error(error.code(), error.what()) {}
};
class MissingWorkspaceLeaf final : public Error {
public:
    explicit MissingWorkspaceLeaf(const std::string& path)
        : Error(ErrorCode::InvalidInput, "workspace final leaf is missing: " + path) {}
};

// Metering changes CAS versions, never the semantic correction target.
inline Json semantic_target(Json value) {
    if (value.is_object()) {
        for (const auto* key : {"state_version", "expected_task_version", "execution_epoch", "dispatcher_epoch"}) value.as_object().erase(key);
        for (auto& field : value.as_object()) field.value() = semantic_target(field.value());
    } else if (value.is_array())
        for (auto& item : value.as_array()) item = semantic_target(item);
    return value;
}
inline std::string target_key(const ModelRequest& request, const Json& action) {
    return sha256_hex(canonical_json(boost::json::object{{"role", role_name(request.role)},
        {"binding", semantic_target(to_json(request.binding))}, {"config", request.config_snapshot_id},
        {"action", semantic_target(action)}}));
}
inline const Json* correction(const Json& result) {
    if (!result.is_object()) return nullptr;
    const auto* value = result.as_object().if_contains("correction_feedback");
    return value && value->is_object() && value->as_object().if_contains("format") &&
        value->at("format") == "wls-correction-feedback-v1" ? value : nullptr;
}
inline std::int64_t failures(const Json& tail, const std::string& key,
                             const std::string& excluding = {}) {
    const auto& items = tail.at("items").as_array();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (it->at("attempt_id") == Json(excluding)) continue;
        const auto& feedback = it->at("feedback");
        const auto* value = feedback.is_object() ? correction(feedback.at("result")) : nullptr;
        if (!value || value->at("target_sha256") != Json(key)) return 0;
        const auto count = value->at("consecutive_failures");
        if (!count.is_int64() || count.as_int64() < 1 || count.as_int64() > 1000000000000LL)
            throw Error(ErrorCode::Corrupt, "invalid persisted correction count");
        return count.as_int64();
    }
    return 0;
}
inline Json failure(const char* reason, const std::string& key, std::int64_t count) {
    return boost::json::object{{"status", "FAILED"}, {"correction_feedback", boost::json::object{
        {"format", "wls-correction-feedback-v1"}, {"reason", reason},
        {"target_sha256", key}, {"consecutive_failures", count},
        {"message", "Correct the rejected output using its original response and feedback; do not replay the old operation."}}}};
}
inline bool output_limit_repeated(const Json& tail) {
    const auto& items = tail.at("items").as_array();
    if (items.size() < 2) return false;
    for (auto it = items.rbegin(); it != items.rbegin() + 2; ++it) {
        const auto& feedback = it->at("feedback");
        const auto* value = feedback.is_object() ? correction(feedback.at("result")) : nullptr;
        if (!value || value->at("reason") != "max_output_tokens") return false;
    }
    return true;
}
inline std::int64_t same_errors(const Json& tail, const std::string& key,
                               const std::string& signature = {}, const std::string& excluding = {}) {
    for (auto it = tail.at("items").as_array().rbegin(); it != tail.at("items").as_array().rend(); ++it) {
        if (it->at("attempt_id") == Json(excluding)) continue;
        const auto& feedback = it->at("feedback");
        const auto* value = feedback.is_object() ? correction(feedback.at("result")) : nullptr;
        if (!value || value->at("target_sha256") != Json(key)) return 0;
        const auto* extension = value->as_object().if_contains("bounded_runtime_v1");
        if (!extension || (!signature.empty() && extension->at("error_sha256") != Json(signature))) return 0;
        const auto count = extension->at("same_error_repeats");
        if (!count.is_int64() || count.as_int64() < 1 || count.as_int64() > 1000000000000LL)
            throw Error(ErrorCode::Corrupt, "invalid persisted same-error count");
        return count.as_int64();
    }
    return 0;
}
inline bool definite_failure(const Json& value) {
    if (!value.is_object() || !value.as_object().contains("operation") ||
        !value.as_object().contains("execution_result") || value.at("replayed") != false ||
        value.at("executed_now") != true || value.at("operation").at("status") != "FAILED") return false;
    const auto& execution = value.at("execution_result");
    if (!execution.is_object() || execution.at("observed_outcome") != "FAILED") return false;
    const auto& evidence = execution.at("evidence");
    const auto& tool = value.at("operation").at("tool_id");
    const auto* cause = evidence.as_object().if_contains("cause_code");
    const bool safe_cause = cause && (*cause == "invalid_input" || *cause == "quota");
    if ((tool == "fs_write" || tool == "fs_patch") && (safe_cause ||
        (evidence.as_object().if_contains("failure_kind") && evidence.at("failure_kind") == "content_precondition")))
        return evidence.at("rename_attempted") == false && evidence.at("renamed") == false &&
            evidence.at("temporary_remaining") == false && evidence.at("temporary_cleanup_complete") == true;
    if (tool == "fs_mkdir" && safe_cause)
        return evidence.at("mkdir_attempted") == false && evidence.at("created") == false;
    const auto& result = execution.at("result");
    if (tool != "run" || !result.is_object()) return false;
    if (result.at("status") == "REFUSED" && result.at("spawned") == false) {
        const auto* code = evidence.as_object().if_contains("error_code");
        return code && (*code == "invalid_input" || *code == "unsupported");
    }
    if (result.at("status") != "EXITED" || !result.at("exit_code").is_int64() ||
        result.at("exit_code").as_int64() <= 0 || result.at("exit_code").as_int64() > 255) return false;
    ProcessResult process;
    process.status = ProcessStatus::Exited; process.exit_code = static_cast<int>(result.at("exit_code").as_int64());
    process.spawned = result.at("spawned").as_bool();
    process.launcher_exec_confirmed = result.at("launcher_exec_confirmed").as_bool();
    process.strong_isolation = result.at("strong_isolation").as_bool();
    process.all_descendants_exited = result.at("all_descendants_exited").as_bool();
    process.evidence = evidence;
    return process.spawned && process_operation_outcome(process) == "FAILED";
}
} // namespace wls::s1::correction_detail
