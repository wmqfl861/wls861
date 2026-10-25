#pragma once

#include "wls/context.hpp"

namespace wls::s1 {

struct ContextToolInvocation {
    std::string call_id;
    std::string name;
    Json arguments;
};

// Host-owned logical transcript. Source is provenance/data, never authority.
// Every assistant tool invocation has exactly one immediately following result
// in the same turn; tool IDs are unique across the complete transcript.
struct StructuredContextMessage {
    std::string role;
    std::string content;
    std::vector<ContextToolInvocation> tool_calls;
    std::string tool_call_id;
    Json source = nullptr;
};

// The base contains stable sections and the complete task contract. Its legacy
// messages must be empty, so no caller can silently drop an old transcript.
CompiledContext compile_structured_context(const ContextInput& base,
    const std::vector<StructuredContextMessage>& messages, ContextLimits limits = {});

// Convert complete, scoped Store attempt history without rewriting its source.
// Missing/unapplied observations remain data, not invented native tool results.
std::vector<StructuredContextMessage> structured_attempt_history(const std::vector<Json>& history);

// Used by the common CompiledContext persistence boundary; round-trip rebuilds
// canonical structure and validates pairing as well as all logical hashes.
Json structured_context_to_json(const CompiledContext& context);
CompiledContext structured_context_from_json(const Json& value);

} // namespace wls::s1
