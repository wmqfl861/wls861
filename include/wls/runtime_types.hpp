#pragma once

#include "wls/identity.hpp"

#include <boost/json.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace wls::s1 {

using Json = boost::json::value;

enum class ErrorCode {
    InvalidInput, Unauthorized, Conflict, Stale, NotReady, UnknownOutcome,
    Quota, Storage, Unsupported, Corrupt
};

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    ErrorCode code() const noexcept { return code_; }
private:
    ErrorCode code_;
};

std::string_view error_name(ErrorCode code) noexcept;
std::string_view role_name(Role role) noexcept;
Role role_from_string(std::string_view name);

// Strict JSON rejects invalid UTF-8, duplicate object keys, trailing content,
// non-finite numbers and excess input/depth. No comments or JSON extensions.
Json parse_json_strict(std::string_view text, std::size_t max_bytes = 1048576,
                       std::size_t max_depth = 64);
// Object keys are sorted recursively; array order remains significant.
// This is the versioned internal encoding, not a claim of RFC 8785 compliance.
std::string canonical_json(const Json& value);
std::string sha256_hex(std::string_view bytes);

// Parse the canonical JSON decimal spelling. Fractional micro-dollars round
// down; no binary multiply, hidden extra spending authority or null coercion.
std::int64_t cost_limit_micro(const Json& value);

// Input validation only; Store separately verifies source revisions and
// verbatim coverage inside the admission/AMEND transaction.
void validate_constraint_catalog_shape(const Json& catalog);

// Linux getrandom + a mutex-protected monotonic ULID generator. Typed prefixes
// are host-owned lower-case identifiers, not model-provided authorization.
std::string new_id(std::string_view prefix);
bool is_typed_id(std::string_view id, std::string_view prefix) noexcept;

struct ExecutionBinding {
    std::string request_id;
    std::string task_id;
    std::string team_id;
    std::int64_t requirements_revision{1};
    std::int64_t plan_revision{0};
    std::int64_t task_revision{0};
    std::int64_t instruction_revision{0};
    std::int64_t dispatcher_epoch{1};
    std::int64_t execution_epoch{1};
    std::int64_t policy_epoch{1};
    bool operator==(const ExecutionBinding&) const = default;
};

Json to_json(const ExecutionBinding& binding);
ExecutionBinding execution_binding_from_json(const Json& value);

} // namespace wls::s1
