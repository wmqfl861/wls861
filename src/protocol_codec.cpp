#include "wls/protocol_codec.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <string_view>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

void require(bool valid, ErrorCode code, const char* message) {
    if (!valid) throw Error(code, message);
}
const Object& object(const Json& value) {
    require(value.is_object(), ErrorCode::InvalidInput, "protocol field must be an object");
    return value.as_object();
}
const Json& field(const Json& value, std::string_view key) {
    const auto* result = object(value).if_contains(key);
    require(result != nullptr, ErrorCode::InvalidInput, "missing protocol field");
    return *result;
}
std::string text(const Json& value, std::string_view key) {
    const auto& item = field(value, key);
    require(item.is_string(), ErrorCode::InvalidInput, "protocol field must be text");
    return std::string(item.as_string());
}
const Array& array(const Json& value) {
    require(value.is_array(), ErrorCode::InvalidInput, "protocol field must be an array");
    return value.as_array();
}
void only_fields(const Json& value, const std::set<std::string>& allowed) {
    for (const auto& entry : object(value))
        require(allowed.contains(std::string(entry.key())), ErrorCode::Unsupported,
                "response field is outside the implemented protocol subset");
}
bool token(const std::string& value, std::size_t maximum = 128) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            const auto c = static_cast<unsigned char>(ch);
            return c >= 33 && c <= 126;
        });
}
bool function_name(const std::string& value) {
    return !value.empty() && value.size() <= 64 &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        });
}
void append_bounded(std::string& target, const std::string& bytes, std::size_t maximum) {
    require(bytes.size() <= maximum && target.size() <= maximum - bytes.size(),
            ErrorCode::Quota, "protocol output exceeds its declared byte limit");
    target += bytes;
}

Json validate_request(const ProviderProfile& profile, const ModelRequest& request, bool stream) {
    validate_provider_profile(profile);
    require(token(request.call_id) && token(request.attempt_id), ErrorCode::InvalidInput,
            "protocol host call and attempt IDs must be bounded ASCII tokens");
    require(request.context.structured(), ErrorCode::Unsupported,
            "Responses codec requires explicit structured-v2 input; legacy frames are not converted");
    const auto envelope = to_json(request);
    require(request.model.model == profile.model_ref && request.context.adapter_version == profile.adapter_version &&
            request.context.protocol_version == profile.protocol, ErrorCode::Conflict,
            "request model or frozen adapter/protocol differs from the offline profile");
    require(!stream || profile.streaming, ErrorCode::Unsupported, "profile does not declare streaming support");
    require(request.context.tool_names.empty() || profile.tool_calling, ErrorCode::Unsupported,
            "profile does not declare tool calling support");
    for (const auto& name : request.context.tool_names)
        require(function_name(name), ErrorCode::Unsupported, "tool name is outside the Responses function subset");
    for (const auto& message : request.context.logical_input.at("messages").as_array())
        for (const auto& call : message.at("tool_calls").as_array())
            require(function_name(text(call, "name")), ErrorCode::Unsupported,
                    "historical tool name is outside the Responses function subset");
    require(request.max_output_tokens <= profile.max_output_tokens &&
            request.reserved_input_tokens <= profile.context_window &&
            request.reserved_output_tokens <= profile.context_window - request.reserved_input_tokens,
            ErrorCode::Quota, "request reservations exceed declared provider token capabilities");
    require(request.timeout.count() <= profile.limits.max_timeout_ms, ErrorCode::Quota,
            "request deadline exceeds declared protocol bound");
    require(canonical_json(envelope).size() <= profile.limits.max_request_bytes,
            ErrorCode::Quota, "logical request exceeds declared protocol byte limit");
    switch (request.model.reasoning.mode) {
    case ReasoningMode::ProviderDefault:
        require(profile.reasoning_provider_default, ErrorCode::Unsupported,
                "provider-default reasoning is not declared by this profile");
        break;
    case ReasoningMode::Disabled:
        require(profile.reasoning_off, ErrorCode::Unsupported,
                "explicit reasoning off is not declared by this profile");
        break;
    case ReasoningMode::Explicit:
        require(profile.reasoning_efforts.contains(request.model.reasoning.effort), ErrorCode::Unsupported,
                "requested reasoning effort is not declared by this profile");
        break;
    case ReasoningMode::Inherit:
        throw Error(ErrorCode::InvalidInput, "protocol request reasoning must already be resolved");
    }
    return envelope;
}

Object host_evidence(const ProviderProfile& profile, const ModelRequest& request, const Json& envelope) {
    return Object{{"profile_sha256", provider_profile_sha256(profile)},
        {"logical_request_sha256", envelope.at("logical_request_sha256")},
        {"call_id", request.call_id}, {"attempt_id", request.attempt_id},
        {"role", role_name(request.role)}, {"role_call_index", request.role_call_index},
        {"binding", to_json(request.binding)}, {"config_snapshot_id", request.config_snapshot_id},
        {"network_dispatched", false}, {"credentials_resolved", false}, {"paid_attempts", 0},
        {"business_accepted", false}, {"tool_dispatch_performed", false},
        {"wire_request_sha256", nullptr}};
}

ModelResponse response_for(const ModelRequest& request) {
    ModelResponse response;
    response.call_id = request.call_id; response.attempt_id = request.attempt_id;
    response.role_call_index = request.role_call_index; response.role = request.role;
    response.binding = request.binding; response.config_snapshot_id = request.config_snapshot_id;
    response.status = ModelResultStatus::Error;
    response.error_code = "invalid_response";
    response.error_message = "offline response has not passed protocol validation";
    return response;
}
void failure(ModelResponse& response, ModelResultStatus status, const std::string& code,
             const std::string& message) {
    response.status = status; response.error_code = code; response.error_message = message;
    response.text.clear(); response.tool_calls.clear();
}
std::optional<std::int64_t> amount(const Json& value, std::string_view key) {
    const auto* item = object(value).if_contains(key);
    if (item == nullptr || item->is_null()) return std::nullopt;
    if (item->is_int64() && item->as_int64() >= 0) return item->as_int64();
    if (item->is_uint64() && item->as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(item->as_uint64());
    throw Error(ErrorCode::InvalidInput, "provider usage must be a nonnegative integer or unknown");
}
ModelUsage usage(const Json& value) {
    ModelUsage result;
    if (value.is_null()) return result;
    only_fields(value, {"input_tokens", "output_tokens", "total_tokens", "input_tokens_details", "output_tokens_details"});
    result.input_tokens = amount(value, "input_tokens");
    result.output_tokens = amount(value, "output_tokens");
    const auto total = amount(value, "total_tokens");
    for (const auto component : {result.input_tokens, result.output_tokens})
        require(!total || !component || *component <= *total, ErrorCode::InvalidInput,
                "provider usage component exceeds reported total");
    if (const auto* input = object(value).if_contains("input_tokens_details"); input && !input->is_null()) {
        only_fields(*input, {"cached_tokens", "cache_write_tokens"});
        result.cache_read_tokens = amount(*input, "cached_tokens");
        result.cache_write_tokens = amount(*input, "cache_write_tokens");
    }
    if (const auto* output = object(value).if_contains("output_tokens_details"); output && !output->is_null()) {
        only_fields(*output, {"reasoning_tokens"});
        const auto reasoning = amount(*output, "reasoning_tokens");
        require(!reasoning || !result.output_tokens || *reasoning <= *result.output_tokens,
                ErrorCode::InvalidInput, "provider reasoning usage exceeds reported output");
        require(!reasoning || !total || *reasoning <= *total, ErrorCode::InvalidInput,
                "provider reasoning usage exceeds reported total");
    }
    for (const auto cache : {result.cache_read_tokens, result.cache_write_tokens}) {
        require(!cache || !result.input_tokens || *cache <= *result.input_tokens,
                ErrorCode::InvalidInput, "provider cache usage exceeds reported input");
        require(!cache || !total || *cache <= *total, ErrorCode::InvalidInput,
                "provider cache usage exceeds reported total");
    }
    if (total && result.input_tokens && result.output_tokens) {
        require(*result.input_tokens <= INT64_MAX - *result.output_tokens &&
                *total == *result.input_tokens + *result.output_tokens, ErrorCode::InvalidInput,
                "provider total usage is inconsistent");
    }
    // Responses token accounting does not supply this host's monetary unit.
    // Missing fees are unknown; never infer a price from tokens or a profile.
    result.cost_micro_usd = std::nullopt;
    return result;
}
void empty_optional_array(const Json& value, std::string_view key) {
    if (const auto* item = object(value).if_contains(key))
        require(item->is_array() && item->as_array().empty(), ErrorCode::Unsupported,
                "nonempty annotations or logprobs are outside the implemented text subset");
}

void parse_completed_output(const ProviderProfile& profile, const ModelRequest& request,
                            const Json& value, ModelResponse& response) {
    const auto& items = array(value);
    require(items.size() <= profile.limits.max_output_items, ErrorCode::Quota,
            "response output item count exceeds declared bound");
    std::set<std::string> item_ids, call_ids;
    std::size_t argument_bytes = 0;
    for (const auto& item : items) {
        const auto id = text(item, "id");
        require(token(id) && item_ids.insert(id).second, ErrorCode::InvalidInput,
                "response output IDs must be unique bounded tokens");
        const auto type = text(item, "type");
        if (type == "message") {
            only_fields(item, {"id", "type", "role", "status", "content"});
            require(text(item, "role") == "assistant" && text(item, "status") == "completed",
                    ErrorCode::InvalidInput, "completed response message has wrong role or status");
            const auto& contents = array(field(item, "content"));
            require(contents.size() <= profile.limits.max_output_items, ErrorCode::Quota,
                    "response content item count exceeds declared bound");
            for (const auto& part : contents) {
                only_fields(part, {"type", "text", "annotations", "logprobs"});
                require(text(part, "type") == "output_text", ErrorCode::Unsupported,
                        "response content type is outside the implemented text subset");
                empty_optional_array(part, "annotations"); empty_optional_array(part, "logprobs");
                append_bounded(response.text, text(part, "text"), profile.limits.max_text_bytes);
            }
        } else if (type == "function_call") {
            only_fields(item, {"id", "type", "call_id", "name", "arguments", "status"});
            require(profile.tool_calling, ErrorCode::Unsupported,
                    "response tool call is not supported by the declared profile");
            if (const auto* status = object(item).if_contains("status"))
                require(status->is_string() && status->as_string() == "completed", ErrorCode::InvalidInput,
                        "completed response contains an unfinished function call");
            const auto call_id = text(item, "call_id"), name = text(item, "name");
            require(token(call_id) && token(name) && call_ids.insert(call_id).second,
                    ErrorCode::InvalidInput, "response tool IDs must be unique bounded tokens");
            require(std::find(request.context.tool_names.begin(), request.context.tool_names.end(), name) !=
                    request.context.tool_names.end(), ErrorCode::Unauthorized,
                    "response requested a tool absent from the captured request");
            require(response.tool_calls.size() < profile.limits.max_tool_calls &&
                    (profile.parallel_tool_calls || response.tool_calls.empty()), ErrorCode::Quota,
                    "response tool call count exceeds the declared capability or limit");
            const auto arguments = text(item, "arguments");
            require(arguments.size() <= profile.limits.max_tool_argument_bytes &&
                    argument_bytes <= profile.limits.max_tool_argument_bytes - arguments.size(), ErrorCode::Quota,
                    "response tool arguments exceed declared aggregate bound");
            argument_bytes += arguments.size();
            auto parsed = parse_json_strict(arguments, profile.limits.max_tool_argument_bytes, profile.limits.max_json_depth);
            require(parsed.is_object(), ErrorCode::InvalidInput, "response function arguments must be a complete object");
            response.tool_calls.push_back({call_id, name, std::move(parsed)});
        } else {
            throw Error(ErrorCode::Unsupported, "response item type is outside the implemented text/function subset");
        }
    }
    require(!response.text.empty() || !response.tool_calls.empty(), ErrorCode::InvalidInput,
            "completed response has no supported text or function call");
}

std::string hexadecimal(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        result.push_back(digits[c >> 4]); result.push_back(digits[c & 15]);
    }
    return result;
}

Object encoded_record(const EncodedResponsesRequest& encoded) {
    Object result{{"format", encoded.billing_challenge.empty() ? "wls-encoded-responses-request-v1" :
                      "wls-encoded-responses-request-v2-billing"}, {"method", encoded.method},
        {"url", encoded.url}, {"content_type", "application/json"}, {"body", encoded.body},
        {"body_sha256", encoded.body_sha256}, {"profile_sha256", encoded.profile_sha256},
        {"logical_request_sha256", encoded.logical_request_sha256}, {"stream", encoded.stream},
        {"evidence", encoded.evidence}, {"record_integrity", "local_consistency_only"}};
    if (!encoded.billing_challenge.empty()) result["billing_challenge"] = encoded.billing_challenge;
    return result;
}

Object decoded_record(const DecodedResponsesResponse& decoded) {
    return Object{{"format", "wls-decoded-responses-response-v1"}, {"response", to_json(decoded.response)},
        {"provider_response_id", decoded.provider_response_id.empty() ? Json(nullptr) : Json(decoded.provider_response_id)},
        {"raw_body_encoding", "hex"}, {"raw_body_hex", hexadecimal(decoded.raw_body)},
        {"raw_body_sha256", decoded.raw_body_sha256}, {"evidence", decoded.evidence},
        {"record_integrity", "local_consistency_only"}};
}
} // namespace

EncodedResponsesRequest encode_responses_request(const ProviderProfile& profile,
        const ModelRequest& request, bool stream) {
    const auto envelope = validate_request(profile, request, stream);
    const auto& logical = request.context.logical_input;
    const auto& prefix = logical.at("prefix");
    Array tools, tool_versions, sources;
    for (const auto& definition : array(prefix.at("tools"))) {
        const auto name = text(definition, "name");
        tools.push_back(Object{{"type", "function"}, {"name", name},
            {"parameters", definition.at("parameters")}, {"strict", false}});
        tool_versions.push_back(Object{{"name", name}, {"version", definition.at("version")}});
    }
    std::size_t index = 0;
    for (const auto& message : array(logical.at("messages"))) {
        if (!message.at("source").is_null())
            sources.push_back(Object{{"message_index", index}, {"source", message.at("source")}});
        ++index;
    }
    Array input{Object{{"role", "user"}, {"content", canonical_json(Object{
        {"format", "wls-responses-task-v1"}, {"task_contract", logical.at("task_contract")},
        {"message_sources", sources}})}}};
    for (const auto& message : array(logical.at("messages"))) {
        const auto role = text(message, "role");
        const auto content = text(message, "content");
        if (role == "tool") {
            input.push_back(Object{{"type", "function_call_output"},
                {"call_id", message.at("tool_call_id")}, {"output", content}});
        } else {
            if (!content.empty() || array(message.at("tool_calls")).empty())
                input.push_back(Object{{"role", role}, {"content", content}});
            for (const auto& call : array(message.at("tool_calls")))
                input.push_back(Object{{"type", "function_call"}, {"call_id", call.at("call_id")},
                    {"name", call.at("name")}, {"arguments", canonical_json(call.at("arguments"))}});
        }
    }
    Object body{{"model", profile.model}, {"instructions", canonical_json(Object{
        {"format", "wls-responses-system-v1"}, {"system", prefix.at("system")},
        {"dependencies", prefix.at("dependencies")}, {"tool_versions", tool_versions}})},
        {"input", input}, {"tools", tools}, {"max_output_tokens", request.max_output_tokens},
        {"parallel_tool_calls", profile.parallel_tool_calls}, {"stream", stream},
        {"store", false}, {"background", false}, {"truncation", "disabled"}};
    if (request.model.reasoning.mode == ReasoningMode::Disabled)
        body["reasoning"] = Object{{"effort", "none"}};
    else if (request.model.reasoning.mode == ReasoningMode::Explicit)
        body["reasoning"] = Object{{"effort", request.model.reasoning.effort}};
    EncodedResponsesRequest encoded;
    encoded.url = profile.base_url + profile.responses_path;
    encoded.body = canonical_json(body);
    require(encoded.body.size() <= profile.limits.max_request_bytes, ErrorCode::Quota,
            "encoded protocol body exceeds declared request byte limit");
    encoded.body_sha256 = sha256_hex(encoded.body);
    encoded.profile_sha256 = provider_profile_sha256(profile);
    encoded.logical_request_sha256 = text(envelope, "logical_request_sha256");
    if (!profile.billing_challenge.empty())
        encoded.billing_challenge = sha256_hex("wls-billing-challenge-v1\n" + canonical_json(envelope));
    encoded.stream = stream;
    auto evidence = host_evidence(profile, request, envelope);
    evidence["format"] = "wls-responses-encoding-evidence-v1";
    evidence["encoded_body_sha256"] = encoded.body_sha256;
    evidence["encoded_body_bytes"] = encoded.body.size();
    evidence["evidence_kind"] = "locally_encoded_request";
    evidence["cache_hit_claimed"] = false;
    encoded.evidence = std::move(evidence);
    encoded.record_sha256 = sha256_hex(canonical_json(encoded_record(encoded)));
    return encoded;
}

DecodedResponsesResponse decode_responses_response(const ProviderProfile& profile,
        const ModelRequest& request, int http_status, std::string_view raw_body) {
    const auto envelope = validate_request(profile, request, false);
    require(http_status >= 100 && http_status <= 599, ErrorCode::InvalidInput, "invalid HTTP fixture status");
    require(raw_body.size() <= profile.limits.max_response_bytes, ErrorCode::Quota,
            "HTTP fixture body exceeds declared response byte limit");
    DecodedResponsesResponse decoded;
    decoded.response = response_for(request);
    decoded.raw_body = std::string(raw_body); decoded.raw_body_sha256 = sha256_hex(raw_body);
    auto evidence = host_evidence(profile, request, envelope);
    evidence["format"] = "wls-responses-decoding-evidence-v1";
    evidence["evidence_kind"] = "offline_http_response_fixture";
    evidence["http_status"] = http_status;
    evidence["raw_body_sha256"] = decoded.raw_body_sha256;
    evidence["raw_body_bytes"] = raw_body.size();
    evidence["provider_status"] = nullptr;
    evidence["provider_error"] = nullptr;
    evidence["incomplete_details"] = nullptr;
    try {
        const auto source = parse_json_strict(raw_body, profile.limits.max_response_bytes, profile.limits.max_json_depth);
        (void)object(source);
        if (http_status != 200) {
            if (const auto* error = object(source).if_contains("error")) evidence["provider_error"] = *error;
            failure(decoded.response, ModelResultStatus::Error, "http_" + std::to_string(http_status),
                    "offline HTTP response is not successful; automatic retry is disabled");
        } else {
            only_fields(source, {"id", "object", "status", "model", "output", "error", "incomplete_details", "usage",
                "background", "created_at", "completed_at", "instructions", "max_output_tokens", "parallel_tool_calls",
                "previous_response_id", "reasoning", "store", "temperature", "text", "tool_choice", "tools", "top_p",
                "truncation", "user", "metadata", "service_tier", "max_tool_calls", "top_logprobs", "prompt_cache_key",
                "prompt_cache_options", "prompt_cache_retention", "safety_identifier", "prompt_cache_diagnostics",
                "access_programs", "input", "reasoning_effort"});
            require(text(source, "object") == "response", ErrorCode::InvalidInput, "HTTP object is not a Responses response");
            const auto provider_id = text(source, "id");
            require(token(provider_id), ErrorCode::InvalidInput, "invalid provider response ID");
            decoded.provider_response_id = provider_id;
            require(text(source, "model") == profile.model, ErrorCode::Stale,
                    "provider response model differs from the explicit profile alias");
            if (const auto* reported = object(source).if_contains("usage")) decoded.response.usage = usage(*reported);
            // The documented response envelope includes these empty/null
            // metadata fields. Their presence does not enable program access,
            // an echoed-input format or a second reasoning configuration.
            for (const auto* name : {"access_programs", "reasoning_effort"})
                if (const auto* optional = object(source).if_contains(name))
                    require(optional->is_null(), ErrorCode::Unsupported,
                            "non-null response metadata is outside the implemented protocol subset");
            if (const auto* optional = object(source).if_contains("input"))
                require(optional->is_array() && optional->as_array().empty(), ErrorCode::Unsupported,
                        "nonempty response input echo is outside the implemented protocol subset");
            if (const auto* error = object(source).if_contains("error")) evidence["provider_error"] = *error;
            if (const auto* details = object(source).if_contains("incomplete_details")) evidence["incomplete_details"] = *details;
            const auto status = text(source, "status"); evidence["provider_status"] = status;
            if (status == "failed") {
                require(!evidence.at("provider_error").is_null() && evidence.at("provider_error").is_object(),
                        ErrorCode::InvalidInput, "failed response has no provider error object");
                failure(decoded.response, ModelResultStatus::Error, "provider_failed", "provider reported a failed response");
            } else if (status == "incomplete" || status == "cancelled") {
                failure(decoded.response, ModelResultStatus::Interrupted, "response_" + status,
                        "provider response is incomplete or cancelled; partial output cannot dispatch tools");
            } else if (status == "queued" || status == "in_progress") {
                failure(decoded.response, ModelResultStatus::Interrupted, "response_not_terminal",
                        "offline response has no terminal result");
            } else {
                require(status == "completed", ErrorCode::Unsupported, "unsupported provider response status");
                require(evidence.at("provider_error").is_null() && evidence.at("incomplete_details").is_null(),
                        ErrorCode::InvalidInput, "completed response carries an error or incomplete details");
                parse_completed_output(profile, request, field(source, "output"), decoded.response);
                decoded.response.status = ModelResultStatus::Completed;
                decoded.response.error_code.clear(); decoded.response.error_message.clear();
                validate_response(request, decoded.response);
            }
        }
    } catch (const Error& error) {
        const auto code = error.code() == ErrorCode::Unsupported ? "unsupported_response" :
            error.code() == ErrorCode::Quota ? "response_limit" :
            error.code() == ErrorCode::Unauthorized ? "unavailable_tool" :
            error.code() == ErrorCode::Stale ? "provider_model_mismatch" : "invalid_response";
        failure(decoded.response, ModelResultStatus::Error, code, error.what());
    }
    evidence["provider_response_id"] = decoded.provider_response_id.empty() ? Json(nullptr) : Json(decoded.provider_response_id);
    evidence["protocol_complete"] = decoded.response.status == ModelResultStatus::Completed;
    evidence["reason_code"] = decoded.response.status == ModelResultStatus::Completed ? "completed" : decoded.response.error_code;
    evidence["raw_retained"] = true;
    decoded.evidence = std::move(evidence);
    decoded.record_sha256 = sha256_hex(canonical_json(decoded_record(decoded)));
    return decoded;
}

Json to_json(const EncodedResponsesRequest& encoded) {
    require(encoded.body_sha256 == sha256_hex(encoded.body), ErrorCode::Corrupt,
            "encoded request bytes differ from their fingerprint");
    require(encoded.billing_challenge.empty() ||
            (encoded.billing_challenge.size() == 64 &&
             std::all_of(encoded.billing_challenge.begin(), encoded.billing_challenge.end(), [](char c) {
                 return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
             })), ErrorCode::Corrupt, "invalid encoded billing challenge");
    auto record = encoded_record(encoded);
    require(encoded.record_sha256 == sha256_hex(canonical_json(record)), ErrorCode::Corrupt,
            "encoded request report differs from its local record fingerprint");
    record["record_sha256"] = encoded.record_sha256;
    return record;
}

Json to_json(const DecodedResponsesResponse& decoded) {
    require(decoded.raw_body_sha256 == sha256_hex(decoded.raw_body), ErrorCode::Corrupt,
            "decoded response bytes differ from their fingerprint");
    auto record = decoded_record(decoded);
    require(decoded.record_sha256 == sha256_hex(canonical_json(record)), ErrorCode::Corrupt,
            "decoded response report differs from its local record fingerprint");
    record["record_sha256"] = decoded.record_sha256;
    return record;
}
} // namespace wls::s1
