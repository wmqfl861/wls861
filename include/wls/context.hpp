#pragma once

#include "wls/config.hpp"
#include "wls/runtime_types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace wls::s1 {

struct ContextTool {
    std::string name;
    std::string version;
    Json definition;
};

struct ContextDependency {
    // Logical dependency identity, never a random request ID or host path.
    std::string name;
    std::string version;
    std::string content_sha256;
};

struct ContextMessage {
    std::string role;
    std::string content;
};

struct ContextInput {
    Role role{Role::Worker};
    ResolvedModel model;
    std::string adapter_version;
    std::string protocol_version;
    std::vector<ContextTool> tools;
    std::string system_prompt;
    std::string role_prompt;
    std::string function_prompt;
    std::string environment_card;
    std::string approved_rules;
    std::string project_snapshot;
    std::vector<ContextDependency> dependencies;
    // Dynamic task/AMEND originals and messages remain outside the shareable
    // prefix. The host must pass the complete authoritative contract here.
    std::string task_contract;
    std::vector<ContextMessage> messages;
};

struct ContextLimits {
    std::size_t max_prefix_bytes{256U * 1024U};
    std::size_t max_total_bytes{1024U * 1024U};
    std::size_t max_tools{64};
    std::size_t max_dependencies{128};
    std::size_t max_messages{256};
};

struct CompiledContext {
    Role role{Role::Worker};
    ResolvedModel model;
    std::string adapter_version;
    std::string protocol_version;
    std::string stable_prefix;
    std::string dynamic_suffix;
    std::string prefix_sha256;
    std::string cache_identity;
    std::vector<std::string> tool_names;

    std::string prompt() const;
};

// Pure compilation: no prompt file reads, network, compression or truncation.
// Versioned, length-framed bytes are the actual input handed to the adapter.
// A limit failure is explicit; callers may not omit constraints to make it fit.
class ContextCompiler final {
public:
    explicit ContextCompiler(ContextLimits limits = {});
    CompiledContext compile(const ContextInput& input) const;
private:
    ContextLimits limits_;
};

Json to_json(const CompiledContext& context);
// Restores canonical frames under the standard ContextLimits and checks their
// role/tool contents against metadata; hashes alone do not establish authority.
CompiledContext compiled_context_from_json(const Json& value);

} // namespace wls::s1
