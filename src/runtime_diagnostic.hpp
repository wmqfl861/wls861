#pragma once
#include "wls/runtime_types.hpp"
#include <string>
#include <string_view>

namespace wls::s1 {
// Public diagnostics only; immutable private observations remain unchanged.
inline std::string runtime_diagnostic(std::string value, std::string_view secret = {}) {
    if (!secret.empty()) for (std::size_t at = 0; (at = value.find(secret, at)) != std::string::npos; at += 10)
        value.replace(at, secret.size(), "[redacted]");
    auto lower = value;
    for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (const auto at = lower.find("authorization"); at != std::string::npos)
        value.replace(at, value.size() - at, "Authorization: [redacted]");
    if (value.size() > 600) {
        std::size_t end = 600;
        while (end && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) --end;
        value.resize(end); value += "...";
    }
    for (auto& c : value) if (static_cast<unsigned char>(c) < 32U) c = ' ';
    try { static_cast<void>(parse_json_strict(canonical_json(Json(value)))); }
    catch (const Error&) { return "Diagnostic contains non-UTF-8 bytes; inspect the private evidence."; }
    return value;
}
}
