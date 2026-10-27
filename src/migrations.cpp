#include "wls/migrations.hpp"

#include "wls/runtime_resources.hpp"

#include <array>
#include <utility>

namespace wls::s1 {
namespace {

constexpr std::string_view base_schema_sha256 = "111dbfe406492521c4c72a9ca49c14ff1cd89db48c115a9ea169f511de34f9e8";
constexpr std::array<RegisteredMigration, 9> registrations{{
    {401, 402, "db/migrations/004_s1_store.sql", "f23563e7776499fd1a592662c2efc50b0d2b2a9c872fa401f233427fb4bfd34a", "store_runtime_metadata", 4},
    {402, 403, "db/migrations/005_worker_tools.sql", "0cfe29dcf094ec65ba0b2e4365889db198fae75107664c26bfc9b476c3903490", "worker_tools_metadata", 5},
    {403, 404, "db/migrations/006_event_runtime.sql", "d143e51f263703b2b01fe26f1645eb619a8814f1aed2077f8f71820a50951055", "event_runtime_metadata", 6},
    {404, 405, "db/migrations/007_workflow_runtime.sql", "f0e3014fa64cbdfff5e96d9203a1583f83b06b6dac7686de904a8a279ad51e21", "workflow_runtime_metadata", 7},
    {405, 406, "db/migrations/008_acceptance_runtime.sql", "615b59344c3c79688108ec903e53641c4f0070cbc3790267f1179b1287a9d149", "acceptance_runtime_metadata", 8},
    {406, 407, "db/migrations/009_owned_cleanup.sql", "25fbcccfe4fa85049350a224bb716a6ef4b6a99cda59c659a7f1d935dda15c15", "owned_cleanup_metadata", 9},
    {407, 408, "db/migrations/010_owned_allocation_recovery.sql", "fe0746733099e53e619e0ddab32172912dbf01f9379d1c21a8eaa914061ca5ce", "owned_allocation_metadata", 10},
    {408, 409, "db/migrations/011_live_model.sql", "1e5f15ce0ce21a2759d2f9a76900e28e5aa0a9b498094f327eddea7386202ef6", "live_model_metadata", 11},
    {409, 410, "db/migrations/012_usage_reconciliation.sql", "6967e936d2466ec62479bb18ea0c8790eee876599cc9384db6da8b8f3293cb37", "usage_reconciliation_metadata", 12}
}};

void require(bool valid, ErrorCode code, const std::string& message) {
    if (!valid) throw Error(code, message);
}

} // namespace

std::span<const RegisteredMigration> registered_store_migrations() noexcept { return registrations; }

MigrationRegistry::MigrationRegistry(const RuntimeResources& resources, std::int64_t target_version)
    : target_version_(target_version), base_schema_(resources.bytes("db/schema.sql")) {
    require(target_version >= 402 && target_version <= latest_version, ErrorCode::Unsupported,
            "unsupported registered Store target version");
    require(sha256_hex(base_schema_) == base_schema_sha256, ErrorCode::Corrupt,
            "base schema differs from the compiled Store registration");
    std::int64_t previous = 401;
    for (const auto& registration : registrations) {
        require(registration.from_version == previous && registration.to_version == previous + 1,
                ErrorCode::Corrupt, "compiled Store migration registrations are not contiguous");
        previous = registration.to_version;
        const auto& sql = resources.bytes(registration.path);
        require(sha256_hex(sql) == registration.sha256, ErrorCode::Corrupt,
                "migration differs from the compiled Store registration");
        if (registration.to_version <= target_version_) migrations_.push_back(sql);
    }
    require(previous == latest_version && migrations_.size() == static_cast<std::size_t>(target_version_ - 401),
            ErrorCode::Corrupt, "registered Store migration prefix is incomplete");
}

std::string_view MigrationRegistry::migration_sql(std::int64_t to_version) const {
    require(to_version >= 402 && to_version <= latest_version, ErrorCode::InvalidInput,
            "unknown registered Store migration version");
    if (to_version > target_version_) return {};
    return migrations_.at(static_cast<std::size_t>(to_version - 402));
}

Json MigrationRegistry::diagnostics() const {
    boost::json::array entries;
    for (const auto& registration : registrations) {
        if (registration.to_version > target_version_) break;
        entries.push_back(boost::json::object{{"from_version", registration.from_version},
            {"to_version", registration.to_version}, {"path", registration.path},
            {"sha256", registration.sha256}, {"metadata_table", registration.metadata_table},
            {"metadata_version", registration.metadata_version}});
    }
    return boost::json::object{{"registry_version", 1}, {"base_version", 401},
        {"base_schema_sha256", base_schema_sha256}, {"target_version", target_version_},
        {"migrations", std::move(entries)}};
}

} // namespace wls::s1
