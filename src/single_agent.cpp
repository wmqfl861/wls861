#include "wls/single_agent.hpp"

#include <set>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

void require(bool valid, ErrorCode code, const char* message) {
    if (!valid) throw Error(code, message);
}
std::int64_t number(const Json& value, std::string_view key) {
    const auto& item = value.at(key);
    if (item.is_int64()) return item.as_int64();
    if (item.is_uint64() && item.as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(item.as_uint64());
    throw Error(ErrorCode::Corrupt, "invalid single-Agent integer");
}
std::string text(const Json& value, std::string_view key) {
    const auto& item = value.at(key);
    require(item.is_string(), ErrorCode::Corrupt, "invalid single-Agent identity");
    return std::string(item.as_string());
}
Json marker(std::string_view contract, const std::string& logical_id, std::size_t count,
            std::string_view history, const std::string& chain) {
    return Object{{"contract", contract}, {"logical_agent_id", logical_id},
        {"global_step", static_cast<std::int64_t>(count) + 1}, {"history_count", count},
        {"history_sha256", sha256_hex(history)}, {"previous_chain_sha256", chain}};
}
Json decorate(Json report, const std::string& request_id, const FrozenRequestConfig& config,
              bool live) {
    report.as_object()["mode"] = live ? "single_agent_live" : "single_agent_fake";
    report.as_object()["execution_contract"] = config.logical_agent_policy().at("mode");
    report.as_object()["logical_agent_id"] = single_agent_transcript(request_id, config, {}).execution.at("logical_agent_id");
    report.as_object()["logical_agent_count"] = 1;
    report.as_object()["authority_roles_are_separate_agents"] = false;
    report.as_object()["provider_quality_acceptance_established"] = false;
    return report;
}
} // namespace

bool plain_single_agent(const FrozenRequestConfig& config) {
    const auto policy = config.logical_agent_policy();
    return policy.is_object() && text(policy, "mode") == plain_single_agent_contract;
}

void validate_single_agent_config(const FrozenRequestConfig& config) {
    const auto policy = config.logical_agent_policy();
    require(policy.is_object() && (text(policy, "mode") == single_agent_contract || plain_single_agent(config)),
        ErrorCode::Unauthorized, "single-Agent runner requires its original frozen logical policy");
    require(config.context_request_format() == "structured-v2" || config.context_request_format() == "structured-v3",
        ErrorCode::Unauthorized,
        "single-Agent execution requires original structured-v2 or structured-v3 input");
    const auto selected = config.resolve(Role::Dispatcher);
    for (const auto role : {Role::TeamLead, Role::Worker})
        require(config.resolve(role) == selected, ErrorCode::Unauthorized,
            "single-Agent authority brokers must select one original model and reasoning");
}

Json single_agent_messages_json(const std::vector<StructuredContextMessage>& messages) {
    Array result;
    for (const auto& message : messages) {
        Array calls;
        for (const auto& call : message.tool_calls)
            calls.emplace_back(Object{{"call_id", call.call_id}, {"name", call.name}, {"arguments", call.arguments}});
        auto encoded = Object{{"role", message.role}, {"content", message.content},
            {"tool_calls", calls}, {"tool_call_id", message.tool_call_id}, {"source", message.source}};
        // Integrity measurement includes the opaque original, never a summary.
        if (!message.responses_output.is_null()) encoded["responses_output"] = message.responses_output;
        result.emplace_back(std::move(encoded));
    }
    return result;
}

SingleAgentTranscript single_agent_transcript(const std::string& request_id,
        const FrozenRequestConfig& config, const std::vector<Json>& history) {
    validate_single_agent_config(config);
    const auto policy = config.logical_agent_policy();
    const auto contract = text(policy, "mode");
    Json worker_identity = nullptr;
    require(history.size() <= static_cast<std::size_t>(number(policy, "max_attempts")), ErrorCode::Quota,
        "complete single-Agent history exceeds its frozen attempt capacity");
    const auto seed = sha256_hex(canonical_json(Object{{"format", "wls-single-agent-chain-v1"},
        {"request_id", request_id}, {"config_snapshot_sha256", config.sha256()}, {"policy", policy}}));
    const std::string logical_id = "logical_" + seed;
    std::string chain = seed;
    std::string prefix = "[]";
    std::size_t count = 0;
    std::set<std::string> attempts, calls;
    for (const auto& record : history) {
        require(record.is_object() && record.at("frozen_binding").is_object(), ErrorCode::Corrupt,
            "invalid original single-Agent history");
        require(text(record, "request_id") == request_id &&
                text(record, "config_snapshot_sha256") == config.sha256() &&
                text(record, "model_ref") == config.resolve(Role::Dispatcher).model,
            ErrorCode::Corrupt, "single-Agent history changed its original request or model");
        if (plain_single_agent(config)) {
            require(text(record, "role") == "worker", ErrorCode::Corrupt, "plain history contains a management call");
            const Json identity = Object{{"task_id", record.at("task_id")}, {"team_id", record.at("team_id")},
                {"principal_id", record.at("principal_id")}};
            require(worker_identity.is_null() || worker_identity == identity, ErrorCode::Corrupt,
                "plain history changed its original Worker identity");
            worker_identity = identity;
        }
        require(attempts.insert(text(record, "attempt_id")).second && calls.insert(text(record, "call_id")).second,
            ErrorCode::Corrupt, "single-Agent history repeats an original call");
        require(!record.at("settlement_sha256").is_null() && !record.at("feedback").is_null(),
            ErrorCode::NotReady, "single-Agent history has an unsettled call or missing immutable feedback");
        require(record.at("logical_agent_execution") == marker(contract, logical_id, count, prefix, chain), ErrorCode::Corrupt,
            "single-Agent original history is missing, reordered or forked");
        // Each original is encoded once for this prefix, including its array
        // level so canonical depth, UTF-8 and finite-number checks still apply.
        // Reuse only these local bytes; every call rechecks the complete chain.
        const auto framed = canonical_json(Array{record});
        const auto encoded = std::string_view(framed).substr(1, framed.size() - 2);
        const auto limit = static_cast<std::size_t>(number(policy, "max_history_bytes"));
        const auto extra = encoded.size() + (count == 0 ? 0U : 1U);
        require(prefix.size() <= limit && extra <= limit - prefix.size(), ErrorCode::Quota,
            "complete single-Agent history exceeds its frozen byte capacity");
        chain = sha256_hex(canonical_json(Object{{"format", "wls-single-agent-link-v1"},
            {"previous_sha256", chain}, {"fact", record}}));
        prefix.pop_back();
        if (count != 0) prefix.push_back(',');
        prefix.append(encoded);
        prefix.push_back(']');
        ++count;
    }
    auto messages = structured_attempt_history(history);
    return {marker(contract, logical_id, count, prefix, chain), std::move(messages)};
}

SingleAgentRuntime::SingleAgentRuntime(Store& store, const SchemaRegistry& schemas,
        std::map<std::string, WorkerTools*> tools_by_node)
    : store_(store), broker_(store, schemas, std::move(tools_by_node)) {}

Json SingleAgentRuntime::pump(const std::string& id, const EventFakeScript& script,
        const EventPumpOptions& options) {
    const auto config = restore_request_config(store_.request(id));
    validate_single_agent_config(config);
    require(!plain_single_agent(config) || store_.workflow_runtime_enabled(), ErrorCode::Unsupported,
        "plain single-Agent requires the complete workflow Store");
    require(options.amendment_dispositions.is_object() && options.amendment_dispositions.as_object().empty(),
        ErrorCode::Unauthorized, "single-Agent AMEND decisions must come from original model calls");
    return decorate(broker_.pump(id, script, options), id, config, false);
}

Json SingleAgentRuntime::pump_live(const std::string& id, const LiveWorkflowOptions& options,
        std::stop_token stop) {
    const auto config = restore_request_config(store_.request(id));
    validate_single_agent_config(config);
    return decorate(broker_.pump_live(id, options, stop), id, config, true);
}

} // namespace wls::s1
