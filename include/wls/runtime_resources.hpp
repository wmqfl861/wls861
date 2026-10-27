#pragma once

#include "wls/runtime_types.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace wls::s1 {

class FrozenRequestConfig;

// Program resources have a separate identity from each request's external
// configuration. Construction captures a complete, bounded, compiled-approved
// set of original bytes; later consumers must use these bytes, not reopen paths.
class RuntimeResources final {
public:
    static RuntimeResources from_source_tree(const std::filesystem::path& root);
    static RuntimeResources from_bundle(const std::filesystem::path& root);
    // With no explicit executable, Linux /proc/self/exe supplies the actual
    // executable location. Only fixed executable-relative locations are tried;
    // cwd, PATH and environment variables never select program resources.
    static RuntimeResources discover(const std::filesystem::path& executable = {});

    const std::filesystem::path& root() const noexcept { return root_; }
    bool source_tree() const noexcept { return source_tree_; }
    const std::string& bytes(std::string_view relative_path) const;
    // Keys are filenames, e.g. plan.schema.json and common.md respectively.
    std::map<std::string, std::string> schema_documents() const;
    std::map<std::string, std::string> prompt_documents() const;
    // Use only when the host selected the distributed default prompts. An
    // explicit external --prompts-dir remains ordinary frozen configuration.
    void validate_frozen_prompts(const FrozenRequestConfig& config) const;
    Json diagnostics() const;

private:
    RuntimeResources(const std::filesystem::path& root, bool source_tree);
    std::filesystem::path root_;
    bool source_tree_{};
    std::map<std::string, std::string> assets_;
    std::string manifest_file_sha256_;
    std::string bundle_sha256_;
};

} // namespace wls::s1
