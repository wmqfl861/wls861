#pragma once

#include "wls/runtime_types.hpp"

#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace wls::s1 {

// The bounded AMEND disposition vocabulary shared by the CLI preflight, the
// offline runtime and the Store guards. Empty is never a decision: callers
// that allow "not supplied" keep their explicit empty() branch, and callers
// that require a decision keep rejecting empty values.
constexpr bool is_amendment_disposition(std::string_view value) noexcept {
    return value == "update" || value == "keep" || value == "cancel";
}

// Shared strict integer field accessor. A non-object value or a missing field
// is InvalidInput "integer field missing: <key>"; a non-integer, fractional
// or above-INT64_MAX uint64 spelling is InvalidInput "invalid integer field:
// <key>".
inline std::int64_t integer_at(const Json& value, std::string_view key) {
    if (!(value.is_object() && value.as_object().contains(key)))
        throw Error(ErrorCode::InvalidInput, "integer field missing: " + std::string(key));
    const auto& number = value.at(key);
    if (number.is_int64()) return number.as_int64();
    if (number.is_uint64() &&
        number.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return static_cast<std::int64_t>(number.as_uint64());
    throw Error(ErrorCode::InvalidInput, "invalid integer field: " + std::string(key));
}

// Shared pending-AMEND judgment over already-captured Store rows. Callers
// still reject multiple tasks first; empty tasks stay a short circuit that
// never reads the request row. This helper opens no Store and caches nothing.
inline bool has_pending_amendment(const std::vector<Json>& tasks, const Json& request) {
    if (tasks.empty()) return false;
    return integer_at(tasks.front(), "based_on_requirements_revision") !=
           integer_at(request, "requirements_revision");
}

} // namespace wls::s1
