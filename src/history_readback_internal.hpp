#pragma once

#include "wls/config_loader.hpp"
#include "wls/live_model.hpp"
#include "amendment_disposition.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace wls::s1::readback_detail {

// Callers restore and match the admitted snapshot before selecting this policy.
inline Json policy(const FrozenRequestConfig& config) {
    const auto* value = config.json().at("system").at("context").as_object().if_contains("readback");
    return value ? *value : Json(nullptr);
}

inline std::int64_t page_limit(const Json& policy) {
    if (!policy.is_object() || policy.as_object().size() != 3 ||
        !policy.as_object().contains("mode") || !policy.as_object().contains("token_estimator") ||
        !policy.as_object().contains("max_page_tokens") ||
        policy.at("mode") != "utf8-token-v1" || policy.at("token_estimator") != "utf8-byte-v1" ||
        integer_at(policy, "max_page_tokens") < 1024 || integer_at(policy, "max_page_tokens") > 65536)
        throw Error(ErrorCode::InvalidInput, "invalid frozen UTF-8 readback policy");
    return integer_at(policy, "max_page_tokens");
}

inline std::int64_t request_units(const FrozenRequestConfig& config, const ModelRequest& request) {
    auto bytes = request.context.prompt().size();
    const auto profile = config.model_profile(request.role);
    if (profile.at("protocol") == "openai-responses-v1") {
        const auto settings = live_model_settings_from_json(profile, request.model.model);
        bytes = std::max(bytes, encode_responses_request(settings.provider, request, settings.stream).body.size());
    }
    const auto window = integer_at(profile.at("capabilities"), "context_window");
    if (bytes > static_cast<std::uint64_t>(window) ||
        request.reserved_output_tokens > window - static_cast<std::int64_t>(bytes))
        throw Error(ErrorCode::Quota, "complete readback logical/wire input exceeds the frozen context window");
    return static_cast<std::int64_t>(bytes);
}

// Validate the whole original, including bytes beyond the selected page.
inline std::vector<std::size_t> utf8_endpoints(std::string_view bytes) {
    std::vector<std::size_t> result{0};
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto lead = static_cast<unsigned char>(bytes[offset]);
        const unsigned count = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
            lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        if (!count || count > bytes.size() - offset)
            throw Error(ErrorCode::Corrupt, "management original contains invalid UTF-8");
        std::uint32_t point = lead & (count == 1 ? 0x7f : count == 2 ? 0x1f : count == 3 ? 0xf : 7);
        for (unsigned i = 1; i < count; ++i) {
            const auto next = static_cast<unsigned char>(bytes[offset + i]);
            if ((next & 0xc0) != 0x80)
                throw Error(ErrorCode::Corrupt, "management original contains invalid UTF-8");
            point = (point << 6) | (next & 0x3f);
        }
        if ((count == 2 && point < 0x80) || (count == 3 && point < 0x800) ||
            (count == 4 && point < 0x10000) || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
            throw Error(ErrorCode::Corrupt, "management original contains invalid UTF-8");
        offset += count;
        result.push_back(offset);
    }
    return result;
}

inline Json measured_page(Json page) {
    auto& value = page.as_object();
    value["host_reserved_units"] = 0;
    value["canonical_json_bytes"] = 0;
    // The measured integers themselves belong to the canonical result.
    for (unsigned iteration = 0; iteration < std::numeric_limits<std::size_t>::digits10 + 2; ++iteration) {
        const auto bytes = canonical_json(page).size();
        if (value.at("host_reserved_units") == bytes) return page;
        value["host_reserved_units"] = bytes;
        value["canonical_json_bytes"] = bytes;
    }
    throw Error(ErrorCode::Corrupt, "canonical readback unit measurement did not converge");
}

} // namespace wls::s1::readback_detail
