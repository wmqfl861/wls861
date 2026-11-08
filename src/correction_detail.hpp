#pragma once

// Private G1-3 correction detail shared by the runtime hosts and the POSIX
// workspace. Nothing here joins a public header: the missing-leaf subtype keeps
// its original InvalidInput code for every existing catch, and hosts may use
// the small predicates only after their own trusted eligibility wrapper.

#include "wls/runtime_types.hpp"

namespace wls::s1::correction_detail {

// Workspace::read raises this subtype when the original path, root, lock and
// ancestor checks passed and only the final leaf is absent. It stays an
// InvalidInput Error externally; only the correction hosts catch the subtype.
class MissingWorkspaceLeaf final : public Error {
public:
    explicit MissingWorkspaceLeaf(const std::string& path)
        : Error(ErrorCode::InvalidInput, "workspace final leaf is missing: " + path) {}
};

// The fixed ordinary bounded-failure feedback of FINAL v6 §3. Fixed bounded
// messages only; effect_count stays zero and runtime_error is never set.
inline Json ordinary_failure_feedback(const char* reason, const char* layer, const char* message) {
    return Json(boost::json::object{
        {"format", "wls-correction-feedback-v1"}, {"status", "FAILED"},
        {"correction_eligible", true}, {"failure_layer", layer}, {"reason", reason},
        {"observation_complete", true}, {"effect_status", "NONE"}, {"effect_count", 0},
        {"message", message}});
}

// Host predicate: a settled result carries the fixed ordinary feedback, either
// flat (model/tool observation without an operation) or as the enrichment of a
// preserved failed operation. It never consults model-supplied data.
inline bool correction_eligible_result(const Json& result) {
    if (!result.is_object()) return false;
    const auto& object = result.as_object();
    if (const auto* nested = object.if_contains("correction_feedback")) {
        return nested->is_object() && nested->as_object().contains("format") &&
            nested->at("format") == "wls-correction-feedback-v1" &&
            nested->as_object().contains("correction_eligible") &&
            nested->at("correction_eligible") == true;
    }
    const auto* format = object.if_contains("format");
    return format != nullptr && format->is_string() &&
        *format == "wls-correction-feedback-v1" &&
        object.contains("correction_eligible") && result.at("correction_eligible") == true;
}

// Definite creation/CAS/patch-content precondition refusal: the original
// operation and execution result settled FAILED with the complete no-effect
// evidence set. Any missing member keeps the failure an ordinary STOP.
inline bool definite_precondition_failure(const Json& result) {
    const auto has = [](const Json& value, const char* key) {
        return value.is_object() && value.as_object().contains(key);
    };
    if (!has(result, "operation") || !has(result, "execution_result") ||
            !has(result, "replayed") || !has(result, "executed_now"))
        return false;
    if (result.at("replayed") == true || result.at("executed_now") != true) return false;
    if (result.at("operation").at("status") != "FAILED") return false;
    const auto& execution = result.at("execution_result");
    if (!has(execution, "evidence") || !has(execution, "observed_outcome") ||
            execution.at("observed_outcome") != "FAILED")
        return false;
    const auto& evidence = execution.at("evidence");
    if (!has(evidence, "failure_kind") || evidence.at("failure_kind") != "content_precondition")
        return false;
    for (const auto* flag : {"rename_attempted", "renamed", "temporary_remaining", "temporary_cleanup_complete"})
        if (!has(evidence, flag)) return false;
    return evidence.at("rename_attempted") == false && evidence.at("renamed") == false &&
        evidence.at("temporary_remaining") == false && evidence.at("temporary_cleanup_complete") == true;
}

} // namespace wls::s1::correction_detail
