#pragma once

#include "wls/offline_runtime.hpp"

#include <cstdint>
#include <vector>

namespace wls::s1 {

// Pure matching of a captured management target to one original inbox item.
// Shared with the synchronous EventRuntime; it grants no Store authority.
bool event_matches_workflow_action(const Json& action, const Json& item);
// Preserve the synchronous claim predicate's validation even for an empty C.
bool event_claim_contains_workflow_action(const Json& action, const Json& claim);

// Select S only from the actual T0 snapshot C, after a viable adapter and result
// slot are owned. Never use a pre-claim inbox page: it may contain held IDs that
// T0 deliberately excludes. Store revalidates C/S and proves completion in T4.
std::vector<std::int64_t> select_persistent_event_ids(
    const CapturedModelCall& captured, const Json& original_claim);

} // namespace wls::s1
