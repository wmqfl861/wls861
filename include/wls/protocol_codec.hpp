#pragma once

#include "wls/provider_profile.hpp"

#include <string>
#include <string_view>

namespace wls::s1 {

struct EncodedResponsesRequest {
    std::string method{"POST"};
    std::string url;
    std::string body;
    std::string body_sha256;
    std::string profile_sha256;
    std::string logical_request_sha256;
    bool stream{false};
    Json evidence;
    // Binds this complete local report against accidental later mutation.
    // This is neither a transport receipt nor authorization against the host.
    std::string record_sha256;
};

struct DecodedResponsesResponse {
    ModelResponse response;
    std::string provider_response_id;
    std::string raw_body;
    std::string raw_body_sha256;
    Json evidence;
    std::string record_sha256;
};

// Pure encoding. Requires a validated structured-v2 request explicitly bound
// to this profile's model_ref, adapter_version and openai-responses-v1 protocol.
// It never rewrites a FakeModel request, resolves credentials or sends bytes.
EncodedResponsesRequest encode_responses_request(const ProviderProfile& profile,
        const ModelRequest& request, bool stream = false);

// Pure decoding of one locally supplied HTTP response body. Only text and
// function_call output are implemented. Malformed/unsupported/partial results
// produce an Error/Interrupted response with no dispatchable tool calls, while
// retaining the bounded original bytes and host tuple in independent evidence.
// Profile/request errors and over-size raw input throw before interpretation.
DecodedResponsesResponse decode_responses_response(const ProviderProfile& profile,
        const ModelRequest& request, int http_status, std::string_view raw_body);

Json to_json(const EncodedResponsesRequest& encoded);
Json to_json(const DecodedResponsesResponse& decoded);

} // namespace wls::s1
