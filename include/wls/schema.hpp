#pragma once

#include "wls/runtime_types.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wls::s1 {

// Draft-07 shape validation with a closed, local document registry. The
// directory is a trusted program asset; validation never fetches remote refs.
// Shape success does not grant authority or prove semantic task acceptance.
class SchemaRegistry final {
public:
    explicit SchemaRegistry(const std::filesystem::path& directory);
    // Host-captured immutable program bytes. This avoids reopening files after
    // RuntimeResources has checked their compiled identities.
    explicit SchemaRegistry(const std::map<std::string, std::string>& documents);
    ~SchemaRegistry();
    SchemaRegistry(const SchemaRegistry&) = delete;
    SchemaRegistry& operator=(const SchemaRegistry&) = delete;

    std::vector<std::string> schema_names() const;
    void validate(std::string_view schema_name, const Json& document,
                  bool model_input = false) const;
    // Remove x-runtime-field properties and their required/conditional
    // declarations, also within registered references. Input cannot provide
    // those fields; only the captured host call may fill the full record.
    Json project_model_input(std::string_view schema_name) const;
    Json full_schema(std::string_view schema_name) const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wls::s1
