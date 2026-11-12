#pragma once

#include "wls/context.hpp"

namespace wls::s1::context_detail {

// Results belong only to the current call stack. The JSON is assembled from
// the rebuilt context, never copied from an untrusted restore document.
struct ContextRestoreResult {
    CompiledContext context;
    Json rebuilt_json;
};

ContextRestoreResult restore_context_with_json(const Json& value);
Json validate_context_to_json(const CompiledContext& context);

// Structured dispatch stays private; each entry performs fresh validation.
ContextRestoreResult restore_structured_with_json(const Json& value);
Json validate_structured_to_json(const CompiledContext& context);

} // namespace wls::s1::context_detail
