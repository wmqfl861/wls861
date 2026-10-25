#pragma once

#include "wls/config.hpp"
#include "wls/runtime_types.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wls::s1 {

// Registrations describe trusted host implementations, never model claims.
// The default fake configuration needs no deterministic acceptance validator.
struct ConfigLoadOptions {
    std::set<std::string> acceptance_validators;
    std::size_t max_file_bytes{1024 * 1024};
    std::size_t max_total_bytes{8 * 1024 * 1024};
    std::size_t max_yaml_nodes{20000};
    std::size_t max_yaml_depth{64};
};

struct ConfigDependency {
    std::string path;
    std::string kind;
    std::string sha256;
    std::string content;
};

class ConfigCatalog;

// An immutable, independently serializable request configuration. Restoration
// reads only these bytes; it cannot consult current global/template files or
// environment variables. This layer never executes environment check commands.
class FrozenRequestConfig {
public:
    static FrozenRequestConfig restore(std::string_view serialized,
                                       std::string_view expected_sha256,
                                       const ConfigLoadOptions& options = {});

    const Json& json() const noexcept { return document_; }
    const std::string& serialize() const noexcept { return serialized_; }
    const std::string& sha256() const noexcept { return sha256_; }
    const std::string& function_id() const noexcept { return function_id_; }
    const std::string& capability_id() const noexcept { return capability_id_; }
    const std::vector<ConfigDependency>& dependencies() const noexcept { return dependencies_; }

    ModelSelection resolve(Role role) const;
    Json explain(Role role) const;
    Json runtime_support() const;
    std::string context_request_format() const;
    Json model_profile(Role role) const;
    ToolSet worker_tools(std::string_view template_id = "default") const;

private:
    friend class ConfigCatalog;
    FrozenRequestConfig(Json document, const ConfigLoadOptions& options);
    Json document_;
    std::string serialized_;
    std::string sha256_;
    std::string function_id_;
    std::string capability_id_;
    std::map<Role, ModelSelection> resolved_;
    std::vector<ConfigDependency> dependencies_;
};

class ConfigCatalog {
public:
    std::vector<std::string> functions() const;
    // An empty capability selects the function's explicitly marked default.
    FrozenRequestConfig freeze(std::string_view function_id,
                               std::string_view capability_id = "") const;

private:
    friend ConfigCatalog load_config_catalog(const std::filesystem::path&,
                                             const std::filesystem::path&,
                                             const std::filesystem::path&,
                                             const ConfigLoadOptions&);
    ConfigLoadOptions options_;
    std::map<std::string, Json> functions_;
};

// Load system YAML, each direct child function/function.yaml, and common role
// prompts. Relative template references are confined to their function root;
// aliases, duplicate/unknown keys, symbolic links and oversized inputs reject.
// In this increment only the fake adapter is executable. Enabling real calls,
// memory or real cleanup rejects explicitly instead of silently doing nothing.
ConfigCatalog load_config_catalog(const std::filesystem::path& system_file,
                                  const std::filesystem::path& functions_directory,
                                  const std::filesystem::path& prompts_directory,
                                  const ConfigLoadOptions& options = {});

} // namespace wls::s1
