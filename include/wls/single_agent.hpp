#pragma once

#include "wls/event_runtime.hpp"
#include "wls/structured_context.hpp"

namespace wls::s1 {

inline constexpr std::string_view single_agent_contract = "single-agent-role-broker-v1";

struct SingleAgentTranscript {
    // Safe host-generated identity and original-prefix fingerprints. The
    // history itself remains private and is encoded as actual model messages.
    Json execution;
    std::vector<StructuredContextMessage> messages;
};

void validate_single_agent_config(const FrozenRequestConfig& config);
SingleAgentTranscript single_agent_transcript(const std::string& request_id,
    const FrozenRequestConfig& config, const std::vector<Json>& original_history);
Json single_agent_messages_json(const std::vector<StructuredContextMessage>& messages);

// One sequential model decision history with original role/target/permission
// brokers. No new tool authority, automatic paid retries, network defaults or
// independent reviewer are provided by sharing the transcript.
class SingleAgentRuntime final {
public:
    SingleAgentRuntime(Store& store, const SchemaRegistry& schemas,
        std::map<std::string, WorkerTools*> tools_by_node = {});
    Json pump(const std::string& request_id, const EventFakeScript& script,
              const EventPumpOptions& options = {});
    Json pump_live(const std::string& request_id, const LiveWorkflowOptions& options,
                   std::stop_token stop = {});
private:
    Store& store_;
    EventRuntime broker_;
};

} // namespace wls::s1
