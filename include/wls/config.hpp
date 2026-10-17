#pragma once

#include "wls/identity.hpp"

#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace wls {

enum class ReasoningMode { Inherit, Disabled, ProviderDefault, Explicit };

struct ReasoningSetting {
    ReasoningMode mode{ReasoningMode::Inherit};
    std::string effort;
    bool operator==(const ReasoningSetting&) const = default;
};

// Empty model and Inherit are meaningful only in a role override. A frozen
// Root snapshot has a nonempty model and a non-Inherit reasoning setting.
struct ModelSelection {
    std::string model;
    ReasoningSetting reasoning;
    bool operator==(const ModelSelection&) const = default;
};

using ResolvedModel = ModelSelection;

struct ModelCapabilities {
    std::set<std::string> reasoning_efforts;
    bool supports_disabled{true};
    bool supports_provider_default{true};
};

enum class ConfigurationErrorCode {
    UnknownModel,
    UnsupportedReasoning,
    InvalidRootDefaults,
    InvalidOverride,
    DuplicateModel,
    InvalidModelCapabilities
};

class ConfigurationError : public std::runtime_error {
public:
    ConfigurationError(ConfigurationErrorCode code, const std::string& message);
    ConfigurationErrorCode code() const noexcept { return code_; }
private:
    ConfigurationErrorCode code_;
};

// Host-owned symbolic capability data; no URLs, provider calls or credentials.
// Finish populating a registry before sharing it with resolver callers.
class CapabilityRegistry {
public:
    void add(std::string model, ModelCapabilities capabilities);
    const ModelCapabilities& get(const std::string& model) const;
private:
    std::map<std::string, ModelCapabilities> models_;
};

void validate_root_defaults(const ModelSelection& frozen_root,
                            const CapabilityRegistry& registry);

// Each missing field resolves DIRECTLY from this frozen Root snapshot. There
// is deliberately no parent/Dispatcher/TeamLead configuration argument.
ResolvedModel resolve_model(const ModelSelection& frozen_root,
                            const ModelSelection& role_override,
                            const CapabilityRegistry& registry);

using ToolSet = std::set<std::string>;

ToolSet effective_tool_grants(Role role, const ToolSet& role_allowlist,
                              const ToolSet& system_allowlist,
                              const ToolSet& request_allowlist);

bool tool_allowed(Role role, const std::string& tool,
                  const ToolSet& role_allowlist,
                  const ToolSet& system_allowlist,
                  const ToolSet& request_allowlist);

} // namespace wls
