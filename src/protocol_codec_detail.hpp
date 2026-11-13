#pragma once
#include "wls/protocol_codec.hpp"

namespace wls::s1::protocol_detail {
Json validate_request(const ProviderProfile&, const ModelRequest&, bool,
                      std::string_view expected = "openai-responses-v1");
boost::json::object host_evidence(const ProviderProfile&, const ModelRequest&, const Json&);
ModelResponse response_for(const ModelRequest&);
void failure(ModelResponse&, ModelResultStatus, const std::string&, const std::string&);
ModelUsage usage(const Json&, bool);
std::optional<std::int64_t> amount(const Json&, std::string_view);
std::string compose_instructions(const Json&);
std::string lean_read_output(const std::string&);
Json lean_state(Json);
void seal(EncodedResponsesRequest&);
void seal(DecodedResponsesResponse&);
}
