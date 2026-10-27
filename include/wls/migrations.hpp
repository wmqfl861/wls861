#pragma once

#include "wls/runtime_types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wls::s1 {

class RuntimeResources;

struct RegisteredMigration {
    std::int64_t from_version;
    std::int64_t to_version;
    std::string_view path;
    std::string_view sha256;
    std::string_view metadata_table;
    std::int64_t metadata_version;
};

// Read-only program registrations, never an arbitrary-directory migration scan.
std::span<const RegisteredMigration> registered_store_migrations() noexcept;

class MigrationRegistry final {
public:
    static constexpr std::int64_t latest_version = 410;
    explicit MigrationRegistry(const RuntimeResources& resources,
                               std::int64_t target_version = latest_version);
    const std::string& base_schema_sql() const noexcept { return base_schema_; }
    // A registered version beyond this selected contiguous prefix returns an
    // empty view, preserving the legacy Store constructor's optional suffix.
    std::string_view migration_sql(std::int64_t to_version) const;
    std::int64_t target_version() const noexcept { return target_version_; }
    Json diagnostics() const;

private:
    std::int64_t target_version_{};
    std::string base_schema_;
    std::vector<std::string> migrations_;
};

} // namespace wls::s1
