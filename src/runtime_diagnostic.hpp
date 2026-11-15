#pragma once
#include "wls/runtime_types.hpp"
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wls::s1 {
// Public diagnostics only; immutable private observations remain unchanged.
inline std::string runtime_diagnostic(std::string value, std::string_view secret = {}) {
    const std::string owned_secret(secret); // The view may point into the moved-in value.
    const auto original = std::move(value);
    auto lower = original;
    for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    std::vector<std::pair<std::size_t, std::size_t>> redactions;
    if (!owned_secret.empty()) for (std::size_t at = 0; (at = original.find(owned_secret, at)) != std::string::npos; ++at)
        redactions.emplace_back(at, at + owned_secret.size());
    auto display = original;
    const auto token = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    };
    for (std::size_t at = 0; (at = lower.find("authorization", at)) != std::string::npos;) {
        auto begin = at, end = at + 13;
        while (begin && token(lower[begin - 1])) --begin;
        while (end < lower.size() && token(lower[end])) ++end;
        at = end;
        auto colon = end;
        if (colon + 1 < lower.size() && lower[colon] == '\\' && (lower[colon + 1] == '\"' || lower[colon + 1] == '\'')) colon += 2;
        else if (colon < lower.size() && (lower[colon] == '\"' || lower[colon] == '\'')) ++colon;
        while (colon < lower.size() && (lower[colon] == ' ' || lower[colon] == '\t')) ++colon;
        if (colon == lower.size() || lower[colon] != ':') continue;
        auto first = colon + 1;
        while (first < lower.size() && (lower[first] == ' ' || lower[first] == '\t')) ++first;
        auto last = lower.find_first_of("\r\n", first);
        if (last == std::string::npos) last = lower.size();
        while (last < lower.size()) {
            auto next = last + 1;
            if (lower[last] == '\r' && next < lower.size() && lower[next] == '\n') ++next;
            if (next == lower.size() || (lower[next] != ' ' && lower[next] != '\t')) break;
            last = lower.find_first_of("\r\n", next);
            if (last == std::string::npos) last = lower.size();
        }
        redactions.emplace_back(first, last);
        if (lower.compare(begin, end - begin, "authorization") == 0) display.replace(begin, end - begin, "Authorization");
        at = last;
    }
    // All spans refer to original bytes. Merge overlaps, but retain adjacent secrets.
    std::sort(redactions.begin(), redactions.end());
    value.clear();
    std::size_t copied = 0;
    for (const auto& [first, last] : redactions) {
        if (first >= copied) {
            value.append(display, copied, first - copied);
            value += "[redacted]";
        }
        copied = std::max(copied, last);
    }
    value.append(display, copied, std::string::npos);
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
