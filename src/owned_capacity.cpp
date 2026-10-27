#include "wls/owned_capacity.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace wls::s1 {
namespace {
void need(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
std::uint64_t number(const Json& value) {
    if (value.is_uint64()) return value.as_uint64();
    need(value.is_int64() && value.as_int64() >= 0, ErrorCode::InvalidInput,
         "owned capacity requires unsigned integers");
    return static_cast<std::uint64_t>(value.as_int64());
}
constexpr std::uint64_t mib = 1024U * 1024U;
} // namespace

void validate_owned_workspace_capacity_policy(const Json& policy) {
    need(policy.is_object(), ErrorCode::InvalidInput, "owned workspace capacity must be an object");
    constexpr std::array<std::string_view, 4> keys{
        "version", "max_entries", "max_total_bytes", "max_inventory_bytes"};
    need(policy.as_object().size() == keys.size(), ErrorCode::InvalidInput,
         "owned workspace capacity requires its exact versioned fields");
    for (const auto key : keys)
        need(policy.as_object().contains(key), ErrorCode::InvalidInput,
             "owned workspace capacity field is missing");
    need(policy.at("version") == "owned-capacity-v1", ErrorCode::Unsupported,
         "unsupported owned workspace capacity version");
    const auto entries = number(policy.at("max_entries"));
    const auto bytes = number(policy.at("max_total_bytes"));
    const auto inventory = number(policy.at("max_inventory_bytes"));
    need(entries >= 1 && entries <= 4095 && bytes >= 1 && bytes <= 240U * mib &&
         inventory >= 256U * 1024U && inventory <= 8U * mib,
         ErrorCode::InvalidInput, "owned workspace capacity exceeds v1 bounds");
}

Json owned_workspace_capacity_policy(const Json& config) {
    need(config.is_object() && config.as_object().contains("system") && config.at("system").is_object() &&
         config.at("system").as_object().contains("cleanup") && config.at("system").at("cleanup").is_object(),
         ErrorCode::InvalidInput, "owned capacity requires a frozen request configuration");
    const auto* policy = config.at("system").at("cleanup").as_object().if_contains("owned_workspace_capacity");
    if (!policy) return nullptr;
    validate_owned_workspace_capacity_policy(*policy);
    return *policy;
}

Json owned_workspace_intent_capacity(const Json& intent) {
    need(intent.is_object(), ErrorCode::InvalidInput, "owned capacity requires an allocation intent");
    const auto* policy = intent.as_object().if_contains("capacity_policy");
    if (!policy) {
        need(!intent.as_object().contains("marker") || !intent.at("marker").is_object() ||
             !intent.at("marker").as_object().contains("capacity_policy_sha256"), ErrorCode::Conflict,
             "owned capacity policy is missing from its original ownership marker");
        return nullptr;
    }
    validate_owned_workspace_capacity_policy(*policy);
    need(intent.as_object().contains("marker") && intent.at("marker").is_object() &&
         intent.at("marker").as_object().contains("capacity_policy_sha256") &&
         intent.at("marker").at("capacity_policy_sha256") == Json(sha256_hex(canonical_json(*policy))),
         ErrorCode::Conflict, "owned capacity differs from the original ownership marker");
    return *policy;
}

WorkspaceLimits owned_workspace_limits(const Json& policy) {
    WorkspaceLimits limits;
    if (policy.is_null()) return limits;
    validate_owned_workspace_capacity_policy(policy);
    limits.max_entries = static_cast<std::size_t>(number(policy.at("max_entries")));
    limits.max_total_bytes = number(policy.at("max_total_bytes"));
    limits.max_inventory_bytes = static_cast<std::size_t>(number(policy.at("max_inventory_bytes")));
    return limits;
}

CleanupLimits owned_cleanup_limits(const Json& policy) {
    CleanupLimits limits;
    if (policy.is_null()) return limits;
    const auto business = owned_workspace_limits(policy);
    // Atomic replacement can retain one original file and one staged file on
    // interruption. This reserve is inside the unchanged cleanup hard limits.
    limits.max_entries = business.max_entries + 1U;
    limits.max_total_bytes = business.max_total_bytes + business.max_file_bytes;
    return limits;
}

std::size_t owned_inventory_entry_budget(std::string_view path) {
    need(path.size() <= 4096, ErrorCode::Quota, "owned inventory path exceeds its frozen bound");
    return canonical_json(Json(path)).size() + 1024U;
}

void validate_owned_workspace_inventory_capacity(const Json& resource, const Json& inventory) {
    need(resource.is_object() && resource.as_object().contains("intent") && inventory.is_object() &&
         inventory.as_object().contains("entries") && inventory.at("entries").is_array(),
         ErrorCode::InvalidInput, "owned cleanup requires a complete inventory");
    const auto policy = owned_workspace_intent_capacity(resource.at("intent"));
    const auto limits = owned_cleanup_limits(policy);
    const auto& entries = inventory.at("entries").as_array();
    need(entries.size() <= limits.max_entries, ErrorCode::Quota,
         "owned cleanup inventory exceeds the original entry capacity; retain the tree for explicit host handling");
    const auto bytes_limit = policy.is_null() ? 8U * mib : number(policy.at("max_inventory_bytes"));
    need(canonical_json(inventory).size() <= bytes_limit, ErrorCode::Quota,
         "owned cleanup inventory exceeds the original encoded-byte capacity; retain the tree for explicit host handling");
    if (policy.is_null()) return;
    need(inventory.as_object().contains("capacity_policy") && inventory.at("capacity_policy") == policy,
         ErrorCode::Conflict, "cleanup inventory capacity policy differs from the original allocation");
    auto header = inventory;
    header.as_object()["entries"] = boost::json::array{};
    need(canonical_json(header).size() <= owned_inventory_root_budget, ErrorCode::Quota,
         "owned inventory root evidence exceeds the frozen representation bound");
    std::uint64_t bytes = 0;
    for (const auto& record : entries) {
        need(record.is_object() && record.as_object().contains("identity") && record.at("identity").is_object() &&
             record.at("identity").as_object().contains("path") && record.at("identity").at("path").is_string() &&
             record.as_object().contains("type"), ErrorCode::InvalidInput, "invalid owned inventory entry");
        need(canonical_json(record).size() <= owned_inventory_entry_budget(record.at("identity").at("path").as_string()),
             ErrorCode::Quota, "owned inventory entry exceeds the frozen representation bound");
        if (record.at("type") == "file") {
            need(record.as_object().contains("size_bytes"), ErrorCode::InvalidInput, "owned file inventory lacks size");
            const auto size = number(record.at("size_bytes"));
            need(size <= limits.max_file_bytes && size <= limits.max_total_bytes - bytes, ErrorCode::Quota,
                 "owned cleanup inventory exceeds the original byte capacity; retain the tree for explicit host handling");
            bytes += size;
        } else need(record.at("type") == "directory", ErrorCode::InvalidInput, "invalid owned inventory entry type");
    }
}

} // namespace wls::s1
