#pragma once
#include "wls/runtime_types.hpp"
namespace wls::s1 {
// Immutable request policy. Missing capacity block means the published legacy
// constants, never current disk defaults. Validation does not rewrite snapshots.
Json store_capacity_limits(const Json& system);
void validate_store_capacity_admission(const Json& system);
}
