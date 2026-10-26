#pragma once

#include "wls/model.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace wls::s1 {

// Host-declared bounds for a pure offline codec. These are never a claim about
// actual provider tokenization, availability, billing or transport limits.
struct ProtocolLimits {
    std::size_t max_request_bytes{1024U * 1024U};
    std::size_t max_response_bytes{1024U * 1024U};
    std::size_t max_text_bytes{256U * 1024U};
    std::size_t max_tool_argument_bytes{256U * 1024U};
    std::size_t max_output_items{128};
    std::size_t max_tool_calls{32};
    std::size_t max_json_depth{64};
    std::size_t max_stream_bytes{4U * 1024U * 1024U};
    std::size_t max_sse_line_bytes{256U * 1024U};
    std::size_t max_sse_event_bytes{1024U * 1024U};
    std::size_t max_sse_events{4096};
    std::size_t max_sse_chunks{16384};
    std::int64_t max_timeout_ms{60000};
};

struct ProviderProfile {
    std::string profile_id;
    std::string adapter_version{"1"};
    std::string protocol{"openai-responses-v1"};
    std::string base_url;
    std::string responses_path;
    std::string model_ref;
    std::string model;
    // A name only. The codec neither resolves the reference nor emits an
    // Authorization header, and no API accepts a credential value.
    std::string api_key_env;
    bool streaming{false};
    bool tool_calling{false};
    bool parallel_tool_calls{false};
    bool reasoning_provider_default{false};
    bool reasoning_off{false};
    std::set<std::string> reasoning_efforts;
    std::int64_t context_window{};
    std::int64_t max_output_tokens{};
    ProtocolLimits limits;
};

// All profile fields, including limits, must be explicitly supplied. Unknown
// fields and versions, real_calls_enabled:true and nonzero paid retries refuse.
ProviderProfile provider_profile_from_json(const Json& value);
void validate_provider_profile(const ProviderProfile& profile);
Json to_json(const ProviderProfile& profile);
std::string provider_profile_sha256(const ProviderProfile& profile);

} // namespace wls::s1
