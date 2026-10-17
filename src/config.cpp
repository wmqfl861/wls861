#include "wls/config.hpp"

#include <utility>

namespace wls {
namespace {

void validate_reasoning_shape(const ReasoningSetting& reasoning,
                              ConfigurationErrorCode error_code) {
    switch (reasoning.mode) {
    case ReasoningMode::Inherit:
    case ReasoningMode::Disabled:
    case ReasoningMode::ProviderDefault:
        if (!reasoning.effort.empty()) {
            throw ConfigurationError(error_code,
                "reasoning effort is only valid in Explicit mode");
        }
        return;
    case ReasoningMode::Explicit:
        if (reasoning.effort.empty()) {
            throw ConfigurationError(error_code,
                "Explicit reasoning requires a nonempty effort");
        }
        return;
    }
    throw ConfigurationError(error_code, "unknown reasoning mode");
}

void validate_resolved(const ModelSelection& resolved,
                       const CapabilityRegistry& registry) {
    const auto& capability = registry.get(resolved.model);
    bool supported = false;
    switch (resolved.reasoning.mode) {
    case ReasoningMode::Disabled:
        supported = capability.supports_disabled;
        break;
    case ReasoningMode::ProviderDefault:
        supported = capability.supports_provider_default;
        break;
    case ReasoningMode::Explicit:
        supported = capability.reasoning_efforts.contains(resolved.reasoning.effort);
        break;
    case ReasoningMode::Inherit:
        break;
    }
    if (!supported) {
        throw ConfigurationError(ConfigurationErrorCode::UnsupportedReasoning,
            "model does not support the requested reasoning setting: " + resolved.model);
    }
}

} // namespace

ConfigurationError::ConfigurationError(ConfigurationErrorCode code,
                                       const std::string& message)
    : std::runtime_error(message), code_(code) {}

void CapabilityRegistry::add(std::string model, ModelCapabilities capabilities) {
    if (model.empty() || capabilities.reasoning_efforts.contains("")) {
        throw ConfigurationError(ConfigurationErrorCode::InvalidModelCapabilities,
            "model name and registered reasoning efforts must be nonempty");
    }
    if (!models_.emplace(std::move(model), std::move(capabilities)).second) {
        throw ConfigurationError(ConfigurationErrorCode::DuplicateModel,
            "model is already registered");
    }
}

const ModelCapabilities& CapabilityRegistry::get(const std::string& model) const {
    const auto found = models_.find(model);
    if (found == models_.end()) {
        throw ConfigurationError(ConfigurationErrorCode::UnknownModel,
            "unknown model: " + model);
    }
    return found->second;
}

void validate_root_defaults(const ModelSelection& frozen_root,
                            const CapabilityRegistry& registry) {
    if (frozen_root.model.empty() ||
        frozen_root.reasoning.mode == ReasoningMode::Inherit) {
        throw ConfigurationError(ConfigurationErrorCode::InvalidRootDefaults,
            "frozen Root defaults must specify model and non-Inherit reasoning");
    }
    validate_reasoning_shape(frozen_root.reasoning,
                            ConfigurationErrorCode::InvalidRootDefaults);
    validate_resolved(frozen_root, registry);
}

ResolvedModel resolve_model(const ModelSelection& frozen_root,
                            const ModelSelection& role_override,
                            const CapabilityRegistry& registry) {
    validate_root_defaults(frozen_root, registry);
    validate_reasoning_shape(role_override.reasoning,
                            ConfigurationErrorCode::InvalidOverride);
    ResolvedModel result{
        role_override.model.empty() ? frozen_root.model : role_override.model,
        role_override.reasoning.mode == ReasoningMode::Inherit
            ? frozen_root.reasoning : role_override.reasoning
    };
    validate_resolved(result, registry);
    return result;
}

bool tool_allowed(Role role, const std::string& tool,
                  const ToolSet& role_allowlist,
                  const ToolSet& system_allowlist,
                  const ToolSet& request_allowlist) {
    return role == Role::Worker && !tool.empty() &&
        role_allowlist.contains(tool) && system_allowlist.contains(tool) &&
        request_allowlist.contains(tool);
}

ToolSet effective_tool_grants(Role role, const ToolSet& role_allowlist,
                              const ToolSet& system_allowlist,
                              const ToolSet& request_allowlist) {
    ToolSet result;
    if (role != Role::Worker) return result;
    for (const auto& tool : role_allowlist) {
        if (tool_allowed(role, tool, role_allowlist,
                         system_allowlist, request_allowlist)) {
            result.insert(tool);
        }
    }
    return result;
}

} // namespace wls
