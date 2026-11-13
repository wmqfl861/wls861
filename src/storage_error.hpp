#pragma once

#include "wls/runtime_types.hpp"

namespace wls::s1 {
// Preserve the public Conflict code while identifying a host SQLite failure.
class StorageConflict final : public Error {
public:
    explicit StorageConflict(const std::string& message)
        : Error(ErrorCode::Conflict, message) {}
};
} // namespace wls::s1
