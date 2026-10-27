#pragma once

#include "wls/cleanup.hpp"
#include "wls/workspace.hpp"

#include <cstddef>
#include <string_view>

namespace wls::s1 {

// Explicit new-request policy. Absence preserves the historical Workspace and
// cleanup limits and the original snapshot bytes. v1 permits only file tools;
// arbitrary subprocess writes cannot satisfy this capacity contract.
void validate_owned_workspace_capacity_policy(const Json& policy);
Json owned_workspace_capacity_policy(const Json& frozen_request_config);
Json owned_workspace_intent_capacity(const Json& intent);
WorkspaceLimits owned_workspace_limits(const Json& policy);
CleanupLimits owned_cleanup_limits(const Json& policy);

// A conservative upper bound for canonical cleanup JSON: a full escaped path
// plus 1024 bytes covers one entry, including all bounded generation numbers,
// mode, size, SHA-256 and punctuation. The root allowance covers all absolute
// identities and the frozen policy; one worst-case staging entry is reserved.
inline constexpr std::size_t owned_inventory_root_budget = 128U * 1024U;
inline constexpr std::size_t owned_inventory_stage_budget = 6U * 4096U + 1026U;
std::size_t owned_inventory_entry_budget(std::string_view path);

// Pure validation before a Store inventory commit or any unlink. New-policy
// inventories bind the exact original intent policy; legacy inventories retain
// their original shape and byte identity. This never grants deletion authority.
void validate_owned_workspace_inventory_capacity(const Json& resource,
                                                const Json& inventory);

} // namespace wls::s1
