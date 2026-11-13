#include "protocol_codec_detail.hpp"
#include "response_metadata.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;
namespace detail = protocol_detail;
void check(bool valid, const char* message, ErrorCode code = ErrorCode::InvalidInput) {
    if (!valid) throw Error(code, message);
}
const Json& field(const Json& value, std::string_view key) {
    check(value.is_object(), "Chat field must be an object");
    const auto* item = value.as_object().if_contains(key);
    check(item, "missing required Chat field"); return *item;
}
std::string text(const Json& value, std::string_view key) {
    const auto& item = field(value, key);
    check(item.is_string(), "Chat field must be text"); return std::string(item.as_string());
}
bool token(std::string_view value, std::size_t bound = 128) {
    return !value.empty() && value.size() <= bound && std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return c >= 33 && c <= 126; });
}
void inspect(const Json& value, const std::set<std::string>& fields, std::string_view path,
             response_metadata::Collector& metadata) {
    check(value.is_object(), "Chat field must be an object");
    metadata.inspect(value.as_object(), fields, path);
}
void schema_self_contained(const Json& value) {
    if (value.is_object()) for (const auto& entry : value.as_object()) {
        check(entry.key() != "$ref" && entry.key() != "$dynamicRef", "Chat tool Schema must be self-contained", ErrorCode::Unsupported);
        schema_self_contained(entry.value());
    }
    if (value.is_array()) for (const auto& item : value.as_array()) schema_self_contained(item);
}
ModelUsage chat_usage(const Json& source, response_metadata::Collector& metadata) {
    const auto* value = source.as_object().if_contains("usage");
    if (!value || value->is_null()) return {};
    inspect(*value, {"prompt_tokens", "completion_tokens", "total_tokens", "prompt_tokens_details", "completion_tokens_details"}, "/usage", metadata);
    Object normalized;
    for (const auto& [from, to] : {std::pair{"prompt_tokens", "input_tokens"}, {"completion_tokens", "output_tokens"}, {"total_tokens", "total_tokens"}})
        if (const auto* item = value->as_object().if_contains(from)) normalized[to] = *item;
    for (const auto& [from, to] : {std::pair{"prompt_tokens_details", "input_tokens_details"}, {"completion_tokens_details", "output_tokens_details"}}) {
        const auto* item = value->as_object().if_contains(from);
        if (!item || item->is_null()) continue;
        const bool input = std::string_view(from) == "prompt_tokens_details";
        const std::set<std::string> fields = input ? std::set<std::string>{"cached_tokens", "cache_write_tokens", "audio_tokens", "image_tokens", "text_tokens"}
            : std::set<std::string>{"reasoning_tokens", "audio_tokens", "accepted_prediction_tokens", "rejected_prediction_tokens", "text_tokens"};
        inspect(*item, fields, std::string("/usage/") + from, metadata);
        for (const auto& field : fields) (void)detail::amount(*item, field);
        normalized[to] = *item;
    }
    return detail::usage(normalized, true); // Same integer/overflow/UNKNOWN rules; no inferred fee.
}

EncodedProtocolRequest encode_chat(const ProviderProfile& profile, const ModelRequest& request, bool stream) {
    const auto envelope = detail::validate_request(profile, request, stream, "openai-chat-v1");
    check(!stream, "Chat streaming is not implemented", ErrorCode::Unsupported);
    const auto& logical = request.context.logical_input;
    const auto& prefix = logical.at("prefix");
    const bool v3 = logical.at("format") == "wls-logical-input-v3";
    const bool lean = logical.as_object().contains("model_projection") && logical.at("model_projection") == "lean-v1";
    const auto allowed = model_allowed_tools(request.context);
    Array tools;
    for (const auto& definition : prefix.at("tools").as_array()) {
        const auto name = text(definition, "name");
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) continue;
        schema_self_contained(definition.at("parameters"));
        Object function{{"name", name}, {"parameters", definition.at("parameters")}, {"strict", false}};
        if (const auto* description = definition.as_object().if_contains("description")) function["description"] = *description;
        tools.push_back(Object{{"type", "function"}, {"function", std::move(function)}});
    }
    Array messages{Object{{"role", "system"}, {"content", detail::compose_instructions(prefix.at("system"))}},
        Object{{"role", "user"}, {"content", canonical_json(v3 ? logical.at("contract_anchor") : logical.at("task_contract"))}}};
    std::size_t contract_bytes = canonical_json(messages.back()).size(), history_bytes = 0;
    std::map<std::size_t, Array> updates;
    std::map<std::int64_t, Array> folded_updates;
    if (v3) for (const auto& update : logical.at("contract_updates").as_array()) {
        std::size_t boundary = 0, position = 0;
        for (const auto& message : logical.at("messages").as_array()) {
            ++position;
            Json identity = message.at("source");
            if ((!identity.is_object() || !identity.as_object().contains("attempt_id")) &&
                    text(message, "role") == "user" && text(message, "content").starts_with("{")) identity = parse_json_strict(text(message, "content"));
            if (identity.is_object()) for (const auto* key : {"attempt_id", "record_id"})
                if (const auto* id = identity.as_object().if_contains(key); id && *id == update.at("after_attempt_id")) boundary = position;
        }
        auto contract = update.at("contract"); contract.as_object()["semantic_versions"] = update.at("semantic_versions");
        Json wire = Object{{"role", "user"}, {"content", canonical_json(contract)}};
        contract_bytes += canonical_json(wire).size();
        bool folded = false;
        if (const auto* history = logical.as_object().if_contains("worker_history"))
            for (const auto& entry : history->at("folded_entries").as_array()) if (entry.at("attempt_id") == update.at("after_attempt_id")) {
                folded_updates[entry.at("sequence").to_number<std::int64_t>()].push_back(wire); folded = true; break;
            }
        if (!folded) updates[boundary].push_back(std::move(wire));
    }
    const auto append_updates = [&](std::size_t position) {
        if (const auto found = updates.find(position); found != updates.end())
            for (const auto& update : found->second) messages.push_back(update);
    };
    std::vector<std::string> pending;
    std::set<std::string> seen;
    std::map<std::string, std::string> names;
    std::size_t index = 0, consumed = 0;
    for (const auto& message : logical.at("messages").as_array()) {
        const auto* opaque = message.as_object().if_contains("responses_output");
        check(!opaque || opaque->is_null(), "Chat cannot continue opaque Responses/reasoning history", ErrorCode::Unsupported);
        append_updates(index++);
        const auto& source = message.at("source");
        const auto* kind = source.is_object() ? source.as_object().if_contains("kind") : nullptr;
        if (v3 && kind && *kind == "history_fold") {
            Array entries;
            const auto emit = [&] {
                if (entries.empty()) return;
                Json wire = Object{{"role", "user"}, {"content", canonical_json(Object{{"kind", "history_fold"},
                    {"from_sequence", entries.front().at("sequence")}, {"through_sequence", entries.back().at("sequence")}, {"entries", entries}})}};
                history_bytes += canonical_json(wire).size(); messages.push_back(std::move(wire)); entries.clear();
            };
            for (const auto& entry : logical.at("worker_history").at("folded_entries").as_array()) {
                entries.push_back(entry);
                if (const auto found = folded_updates.find(entry.at("sequence").to_number<std::int64_t>()); found != folded_updates.end()) {
                    emit(); for (const auto& update : found->second) messages.push_back(update);
                }
            }
            emit(); continue;
        }
        if (v3 && kind && (*kind == "history_window" || *kind == "management_history_window")) continue;
        const auto role = text(message, "role"), content = text(message, "content");
        Object wire{{"role", role}, {"content", content}};
        if (role == "tool") {
            const auto id = text(message, "tool_call_id");
            check(consumed < pending.size() && pending[consumed++] == id, "Chat tool result does not pair with its original round");
            wire["tool_call_id"] = id;
            if (lean && names.at(id) == "fs_read") wire["content"] = detail::lean_read_output(content);
        } else {
            check(consumed == pending.size(), "Chat history has an unfinished tool round");
            pending.clear(); consumed = 0;
            check(role == "user" || role == "assistant", "unsupported Chat history role", ErrorCode::Unsupported);
            Array calls;
            for (const auto& call : message.at("tool_calls").as_array()) {
                const auto id = text(call, "call_id"), name = text(call, "name");
                check(role == "assistant" && token(id) && seen.insert(id).second, "Chat history call IDs must be unique");
                pending.push_back(id); names[id] = name;
                calls.push_back(Object{{"id", id}, {"type", "function"}, {"function", Object{{"name", name}, {"arguments", canonical_json(call.at("arguments"))}}}});
            }
            if (!calls.empty()) { wire["tool_calls"] = std::move(calls); if (content.empty()) wire["content"] = nullptr; }
        }
        history_bytes += canonical_json(wire).size(); messages.push_back(std::move(wire));
    }
    check(consumed == pending.size(), "Chat history ends before all tool results");
    append_updates(index);
    std::size_t state_bytes = 0;
    if (v3) {
        messages.push_back(Object{{"role", "user"}, {"content", canonical_json(lean ? detail::lean_state(logical.at("current_state")) : logical.at("current_state"))}});
        state_bytes = canonical_json(messages.back()).size();
    }
    Object body{{"model", profile.model}, {"messages", messages}, {"tools", tools}, {"max_completion_tokens", request.max_output_tokens},
        {"n", 1}, {"parallel_tool_calls", profile.parallel_tool_calls}, {"stream", false}, {"store", false}};
    if (!profile.tool_choice_policy.empty() && !tools.empty()) body["tool_choice"] = "required";
    if (request.model.reasoning.mode == ReasoningMode::Disabled) body["reasoning_effort"] = "none";
    else if (request.model.reasoning.mode == ReasoningMode::Explicit) body["reasoning_effort"] = request.model.reasoning.effort;
    EncodedProtocolRequest encoded;
    encoded.codec = profile.protocol; encoded.url = profile.base_url + profile.chat_completions_path;
    encoded.body = canonical_json(body);
    check(encoded.body.size() <= profile.limits.max_request_bytes, "encoded Chat body exceeds its byte limit", ErrorCode::Quota);
    encoded.body_sha256 = sha256_hex(encoded.body); encoded.profile_sha256 = provider_profile_sha256(profile);
    encoded.logical_request_sha256 = text(envelope, "logical_request_sha256");
    const auto system_bytes = canonical_json(messages.front()).size(), tool_bytes = canonical_json(tools).size();
    encoded.body_segments = Object{{"system", system_bytes}, {"tools", tool_bytes}, {"contract", contract_bytes},
        {"history", history_bytes}, {"current_state", state_bytes},
        {"other", encoded.body.size() - system_bytes - tool_bytes - contract_bytes - history_bytes - state_bytes}, {"total", encoded.body.size()}};
    auto evidence = detail::host_evidence(profile, request, envelope);
    evidence["format"] = "wls-chat-encoding-evidence-v1"; evidence["evidence_kind"] = "locally_encoded_request";
    evidence["encoded_body_sha256"] = encoded.body_sha256; evidence["encoded_body_bytes"] = encoded.body.size();
    evidence["cache_hit_claimed"] = false; encoded.evidence = std::move(evidence); detail::seal(encoded);
    return encoded;
}

DecodedProtocolResponse decode_chat(const ProviderProfile& profile, const ModelRequest& request, int status, std::string_view raw) {
    const auto envelope = detail::validate_request(profile, request, false, "openai-chat-v1");
    check(status >= 100 && status <= 599, "invalid Chat HTTP status");
    check(raw.size() <= profile.limits.max_response_bytes, "Chat response exceeds its byte limit", ErrorCode::Quota);
    DecodedProtocolResponse decoded;
    decoded.codec = profile.protocol; decoded.response = detail::response_for(request);
    decoded.raw_body = std::string(raw); decoded.raw_body_sha256 = sha256_hex(raw);
    auto evidence = detail::host_evidence(profile, request, envelope);
    evidence["format"] = "wls-chat-decoding-evidence-v1"; evidence["evidence_kind"] = "offline_http_response_fixture";
    evidence["http_status"] = status; evidence["raw_body_sha256"] = decoded.raw_body_sha256; evidence["raw_body_bytes"] = raw.size();
    response_metadata::Collector metadata(profile);
    auto& response = decoded.response;
    try {
        const auto value = parse_json_strict(raw, profile.limits.max_response_bytes, profile.limits.max_json_depth);
        inspect(value, {"id", "object", "created", "model", "choices", "usage", "system_fingerprint", "service_tier", "metadata", "error"}, "", metadata);
        response.usage = chat_usage(value, metadata);
        if (status != 200) detail::failure(response, ModelResultStatus::Error, "http_" + std::to_string(status), "Chat HTTP response is not successful");
        else {
            check(text(value, "object") == "chat.completion", "not a Chat completion");
            decoded.provider_response_id = text(value, "id"); check(token(decoded.provider_response_id), "invalid Chat response ID");
            const auto model = text(value, "model");
            check(model == profile.model || std::find(profile.chat_model_aliases.begin(), profile.chat_model_aliases.end(), model) != profile.chat_model_aliases.end(),
                "Chat response model differs from frozen declarations", ErrorCode::Stale);
            check(detail::amount(value, "created").has_value(), "Chat response lacks creation time");
            if (const auto* error = value.as_object().if_contains("error")) check(error->is_null(), "successful Chat response carries an error");
            for (const auto* key : {"system_fingerprint", "service_tier"})
                if (const auto* item = value.as_object().if_contains(key)) check(item->is_null() || item->is_string(), "invalid Chat metadata type");
            if (const auto* item = value.as_object().if_contains("metadata")) check(item->is_null() || item->is_object(), "invalid Chat metadata object");
            const auto* choices = value.as_object().if_contains("choices");
            check(choices && choices->is_array() && choices->as_array().size() == 1, "Chat requires exactly one choice");
            const auto& choice = choices->as_array().front();
            inspect(choice, {"index", "finish_reason", "message", "logprobs"}, "/choices/0", metadata);
            check(detail::amount(choice, "index") == std::optional<std::int64_t>(0), "Chat choice index must be zero");
            if (const auto* logs = choice.as_object().if_contains("logprobs")) check(logs->is_null(), "Chat logprobs are unsupported", ErrorCode::Unsupported);
            const auto& message = field(choice, "message");
            inspect(message, {"role", "content", "tool_calls", "refusal", "annotations", "audio", "function_call", "reasoning", "reasoning_content", "thinking", "encrypted_content"}, "/choices/0/message", metadata);
            check(text(message, "role") == "assistant", "Chat output role must be assistant");
            for (const auto* key : {"audio", "function_call", "reasoning", "reasoning_content", "thinking", "encrypted_content"})
                if (const auto* item = message.as_object().if_contains(key)) check(item->is_null(), "unsupported Chat output semantics", ErrorCode::Unsupported);
            if (const auto* item = message.as_object().if_contains("annotations")) check(item->is_array() && item->as_array().empty(), "Chat annotations are unsupported", ErrorCode::Unsupported);
            const auto* content = message.as_object().if_contains("content");
            check(!content || content->is_null() || content->is_string(), "Chat content must be text or null");
            if (content && content->is_string()) response.text = std::string(content->as_string());
            check(response.text.size() <= profile.limits.max_text_bytes, "Chat text limit exceeded", ErrorCode::Quota);
            const auto* refusal = message.as_object().if_contains("refusal");
            check(!refusal || refusal->is_null() || refusal->is_string(), "invalid Chat refusal");
            check(!refusal || !refusal->is_string() || refusal->as_string().size() <= profile.limits.max_text_bytes,
                "Chat refusal limit exceeded", ErrorCode::Quota);
            const auto* calls = message.as_object().if_contains("tool_calls");
            check(!calls || calls->is_null() || calls->is_array(), "Chat tool_calls must be an array or null");
            const auto finish = text(choice, "finish_reason"); evidence["finish_reason"] = finish;
            check(finish == "stop" || finish == "tool_calls" || finish == "length" || finish == "content_filter", "unsupported Chat finish reason", ErrorCode::Unsupported);
            if ((refusal && refusal->is_string() && !refusal->as_string().empty()) || finish == "content_filter")
                detail::failure(response, ModelResultStatus::Error, "model_refusal", "Chat provider refused this request");
            else if (finish == "length") detail::failure(response, ModelResultStatus::Interrupted, "response_max_output_tokens", "Chat output limit reached; partial tools cannot execute");
            else {
                std::set<std::string> ids; std::size_t argument_bytes = 0, index = 0;
                if (calls && calls->is_array()) for (const auto& call : calls->as_array()) {
                    check(profile.tool_calling && response.tool_calls.size() < profile.limits.max_tool_calls &&
                        response.tool_calls.size() < profile.limits.max_output_items && (profile.parallel_tool_calls || response.tool_calls.empty()), "Chat call limit exceeded", ErrorCode::Quota);
                    const auto path = "/choices/0/message/tool_calls/" + std::to_string(index++);
                    inspect(call, {"id", "type", "function"}, path, metadata);
                    check(text(call, "type") == "function", "unsupported Chat tool type", ErrorCode::Unsupported);
                    const auto id = text(call, "id"); check(token(id) && ids.insert(id).second, "Chat call IDs must be unique tokens");
                    const auto& function = field(call, "function"); inspect(function, {"name", "arguments"}, path + "/function", metadata);
                    const auto name = text(function, "name"), arguments = text(function, "arguments");
                    check(token(name, 64), "invalid Chat function name");
                    check(arguments.size() <= profile.limits.max_tool_argument_bytes - argument_bytes, "Chat argument limit exceeded", ErrorCode::Quota);
                    argument_bytes += arguments.size();
                    auto parsed = parse_json_strict(arguments, profile.limits.max_tool_argument_bytes, profile.limits.max_json_depth);
                    check(parsed.is_object(), "Chat function arguments must be an object");
                    response.tool_calls.push_back({id, name, std::move(parsed)});
                }
                check((finish == "tool_calls") == !response.tool_calls.empty(), "Chat finish reason contradicts tool calls");
                check(!response.text.empty() || !response.tool_calls.empty(), "Chat completion has no supported output");
                response.status = ModelResultStatus::Completed; response.error_code.clear(); response.error_message.clear();
                validate_response_envelope(request, response);
            }
        }
    } catch (const Error& error) {
        detail::failure(response, ModelResultStatus::Error, error.code() == ErrorCode::Unsupported ? "unsupported_response" :
            error.code() == ErrorCode::Quota ? "response_limit" : error.code() == ErrorCode::Stale ? "provider_model_mismatch" : "invalid_response", error.what());
    }
    evidence["provider_response_id"] = decoded.provider_response_id.empty() ? Json(nullptr) : Json(decoded.provider_response_id);
    evidence["protocol_complete"] = response.status == ModelResultStatus::Completed;
    evidence["reason_code"] = response.status == ModelResultStatus::Completed ? "completed" : response.error_code;
    evidence["raw_retained"] = true; metadata.write(evidence); decoded.evidence = std::move(evidence); detail::seal(decoded);
    return decoded;
}
} // namespace

EncodedProtocolRequest encode_protocol_request(const ProviderProfile& profile, const ModelRequest& request, bool stream) {
    return profile.protocol == "openai-chat-v1" ? encode_chat(profile, request, stream) : encode_responses_request(profile, request, stream);
}
DecodedProtocolResponse decode_protocol_response(const ProviderProfile& profile, const ModelRequest& request, int status, std::string_view raw) {
    return profile.protocol == "openai-chat-v1" ? decode_chat(profile, request, status, raw) : decode_responses_response(profile, request, status, raw);
}
} // namespace wls::s1
