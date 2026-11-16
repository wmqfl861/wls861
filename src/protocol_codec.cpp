#include "wls/protocol_codec.hpp"
#include "protocol_codec_detail.hpp"

#include "responses_compat.hpp"
#include "response_metadata.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <openssl/evp.h>

namespace wls::s1 {
namespace protocol_detail {
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
void only_fields(const Json& value, const std::set<std::string>& allowed,
                 bool ignore_metadata = false) {
    const auto& source = object(value);
    if (ignore_metadata) return;
    for (const auto& entry : source)
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

bool model_accepted(const ProviderProfile& profile, std::string_view model) {
    if (!profile.responses_compatibility) return model == profile.model;
    if (model == profile.model) return true;
    return std::any_of(profile.responses_compatibility->model_aliases.begin(),
                       profile.responses_compatibility->model_aliases.end(),
                       [&](const std::string& alias) { return model == alias; });
}

// Six titled plain-text sections; the title, order and separators are the
// frozen local compatibility contract. Section bytes are never rewritten.
std::string compose_instructions(const Json& system) {
    static constexpr std::string_view sections[]{"system_prompt", "role_prompt", "function_prompt",
        "environment_card", "approved_rules", "project_snapshot"};
    std::string result;
    for (const auto section : sections) {
        if (!result.empty()) result += "\n\n";
        result += "# ";
        result += section;
        result += "\n";
        const auto* value = system.as_object().if_contains(section);
        if (value && value->is_string()) result += std::string(value->as_string());
    }
    return result;
}

Json payload_for(const std::string& model, const Json& output) {
    return Json(Object{{"format", responses_compat::output_format},
        {"reference", responses_compat::reference}, {"model", model}, {"output", output}});
}

Json validate_request(const ProviderProfile& profile, const ModelRequest& request, bool stream,
                      std::string_view expected) {
    validate_provider_profile(profile);
    require(profile.protocol == expected, ErrorCode::Unsupported, "codec does not support this protocol");
    require(token(request.call_id) && token(request.attempt_id), ErrorCode::InvalidInput,
            "protocol host call and attempt IDs must be bounded ASCII tokens");
    require(request.context.structured(), ErrorCode::Unsupported,
            "Responses codec requires explicit structured-v2 input; legacy frames are not converted");
    const auto envelope = to_json(request);
    if (!profile.responses_tool_result_policy.empty())
        require(request.role == Role::Worker && request.context.logical_input.at("format") == "wls-logical-input-v3" &&
            request.context.logical_input.as_object().contains("model_projection") &&
            request.context.logical_input.at("model_projection") == "lean-v1", ErrorCode::Unsupported,
            "write-summary-v1 requires Worker structured-v3 lean-v1");
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
            require(function_name(text(call, "name")) || (message.at("source").is_object() &&
                message.at("source").as_object().contains("rejected_tool_selection") &&
                message.at("source").at("rejected_tool_selection") == true && token(text(call, "name"), 64)), ErrorCode::Unsupported,
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
    response.responses_output = nullptr;  // never keep a replayable original on failure
}
std::optional<std::int64_t> amount(const Json& value, std::string_view key) {
    const auto* item = object(value).if_contains(key);
    if (item == nullptr || item->is_null()) return std::nullopt;
    if (item->is_int64() && item->as_int64() >= 0) return item->as_int64();
    if (item->is_uint64() && item->as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(item->as_uint64());
    throw Error(ErrorCode::InvalidInput, "provider usage must be a nonnegative integer or unknown");
}
ModelUsage usage(const Json& value, bool ignore_metadata) {
    ModelUsage result;
    if (value.is_null()) return result;
    only_fields(value, {"input_tokens", "output_tokens", "total_tokens", "input_tokens_details", "output_tokens_details"}, ignore_metadata);
    result.input_tokens = amount(value, "input_tokens");
    result.output_tokens = amount(value, "output_tokens");
    const auto total = amount(value, "total_tokens");
    for (const auto component : {result.input_tokens, result.output_tokens})
        require(!total || !component || *component <= *total, ErrorCode::InvalidInput,
                "provider usage component exceeds reported total");
    if (const auto* input = object(value).if_contains("input_tokens_details"); input && !input->is_null()) {
        only_fields(*input, {"cached_tokens", "cache_write_tokens"}, ignore_metadata);
        result.cache_read_tokens = amount(*input, "cached_tokens");
        result.cache_write_tokens = amount(*input, "cache_write_tokens");
    }
    if (const auto* output = object(value).if_contains("output_tokens_details"); output && !output->is_null()) {
        only_fields(*output, {"reasoning_tokens"}, ignore_metadata);
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
                            const Json& value, ModelResponse& response, response_metadata::Collector& metadata) {
    (void)request; // Decode complete structure first; the host classifies tool selection after settlement.
    const auto& items = array(value);
    require(items.size() <= profile.limits.max_output_items, ErrorCode::Quota,
            "response output item count exceeds declared bound");
    if (profile.responses_compatibility) {
        // One shared strict validator produces the public projection and the
        // refusal / replay classification in a single pass over the original.
        responses_compat::ValidationBounds codec_bounds{profile.limits.max_output_items,
            profile.limits.max_tool_calls, profile.limits.max_text_bytes, profile.limits.max_tool_argument_bytes};
        auto projection = responses_compat::validate_output(value, codec_bounds,
            nullptr, profile.parallel_tool_calls, metadata.enabled, &metadata);
        response.text = std::move(projection.text);
        response.tool_calls = std::move(projection.tool_calls);
        if (projection.refusal) {
            response.status = ModelResultStatus::Error;
            response.error_code = "model_refusal";
            response.error_message = "provider refused this request; original payload retained without executable output";
            response.text.clear(); response.tool_calls.clear();
            return;
        }
        if (!projection.reasoning_replay_ready) {
            response.status = ModelResultStatus::Error;
            response.error_code = "reasoning_replay_unavailable";
            response.error_message = "completed reasoning lacks replayable encrypted content; the round cannot continue";
            response.text.clear(); response.tool_calls.clear();
            return;
        }
        require(!response.text.empty() || !response.tool_calls.empty(), ErrorCode::InvalidInput,
                "completed response has no supported text or function call");
        response.status = ModelResultStatus::Completed;
        return;
    }
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
    response.status = ModelResultStatus::Completed;
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
    require(encoded.codec.empty() || encoded.codec == "openai-chat-v1", ErrorCode::Corrupt, "unknown encoded codec");
    Object result{{"format", !encoded.codec.empty() ? "wls-encoded-chat-request-v1" : encoded.billing_challenge.empty() ? "wls-encoded-responses-request-v1" :
                      "wls-encoded-responses-request-v2-billing"}, {"method", encoded.method},
        {"url", encoded.url}, {"content_type", "application/json"}, {"body", encoded.body},
        {"body_sha256", encoded.body_sha256}, {"profile_sha256", encoded.profile_sha256},
        {"logical_request_sha256", encoded.logical_request_sha256}, {"stream", encoded.stream},
        {"evidence", encoded.evidence}, {"record_integrity", "local_consistency_only"}};
    if (!encoded.billing_challenge.empty()) result["billing_challenge"] = encoded.billing_challenge;
    return result;
}

Object decoded_record(const DecodedResponsesResponse& decoded) {
    require(decoded.codec.empty() || decoded.codec == "openai-chat-v1", ErrorCode::Corrupt, "unknown decoded codec");
    return Object{{"format", decoded.codec.empty() ? "wls-decoded-responses-response-v1" : "wls-decoded-chat-response-v1"}, {"response", to_json(decoded.response)},
        {"provider_response_id", decoded.provider_response_id.empty() ? Json(nullptr) : Json(decoded.provider_response_id)},
        {"raw_body_encoding", "hex"}, {"raw_body_hex", hexadecimal(decoded.raw_body)},
        {"raw_body_sha256", decoded.raw_body_sha256}, {"evidence", decoded.evidence},
        {"record_integrity", "local_consistency_only"}};
}

std::string lean_read_output(const std::string& content) {
    // Only the complete normal Workspace::read page shape is projected. Errors,
    // arbitrary business JSON and legacy/non-JSON results retain their bytes.
    Json page;
    try { page = parse_json_strict(content); }
    catch (const Error&) { return content; }
    constexpr std::string_view keys[]{"path", "offset", "bytes_returned", "size_bytes", "sha256",
        "content", "content_base64", "eof", "next_offset"};
    if (!page.is_object() || page.as_object().size() != std::size(keys)) return content;
    for (const auto key : keys) if (!page.as_object().contains(key)) return content;
    const auto nonnegative = [](const Json& value) {
        return value.is_uint64() || (value.is_int64() && value.as_int64() >= 0);
    };
    if (!page.at("path").is_string() || !page.at("sha256").is_string() ||
        !page.at("content_base64").is_string() || !page.at("eof").is_bool() ||
        !(page.at("content").is_string() || page.at("content").is_null())) return content;
    for (const auto key : {"offset", "bytes_returned", "size_bytes"})
        if (!nonnegative(page.at(key))) return content;
    const auto offset = page.at("offset").to_number<std::uint64_t>();
    const auto count = page.at("bytes_returned").to_number<std::uint64_t>();
    const auto size = page.at("size_bytes").to_number<std::uint64_t>();
    if (offset > size || count > size - offset || count > 65536 ||
        page.at("eof").as_bool() != (offset + count == size)) return content;
    if (page.at("eof").as_bool() ? !page.at("next_offset").is_null() :
        (!nonnegative(page.at("next_offset")) || page.at("next_offset").to_number<std::uint64_t>() != offset + count)) return content;
    const auto& hash = page.at("sha256").as_string();
    if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    })) return content;
    if (page.at("content").is_string() && page.at("content").as_string().size() != count) return content;
    page.as_object().erase(page.at("content").is_string() ? "content_base64" : "content");
    return canonical_json(page);
}

namespace {
const Json& compact_member(const Json& value, std::string_view key) {
    static const Json absent;
    const auto* item = value.is_object() ? value.as_object().if_contains(key) : nullptr;
    return item ? *item : absent;
}
bool compact_equal(const Json& left, const Json& right) {
    return left == right && canonical_json(left) == canonical_json(right);
}
bool compact_same(const Json& left, const Json& right, std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) {
        const auto& value = compact_member(left, key);
        if (value.is_null() || !compact_equal(value, compact_member(right, key))) return false;
    }
    return true;
}
std::string compact_reference(const Json& retained, std::string_view key, const std::string& path, const Json& original) {
    const auto& reference = compact_member(compact_member(retained, "duplicate_fields"), key);
    return original.is_object() && original.as_object().contains(key) && !original.as_object().contains("duplicate_fields") &&
        reference.is_string() && !retained.as_object().contains(key) ? std::string(reference.as_string()) : path + "." + std::string(key);
}
// Called only for the explicitly typed slots below. Unknown outer fields stay
// in place; each removed original has a path to an equal, surviving value.
void compact_duplicates(Json& value, const Json& retained,
        std::initializer_list<std::string_view> keys, const std::string& path, const Json* projected = nullptr) {
    if (!value.is_object() || value.as_object().contains("duplicate_fields")) return;
    Object references;
    for (const auto key : keys) {
        const auto& original = compact_member(value, key);
        if (original.is_null() || !compact_equal(original, compact_member(retained, key))) continue;
        const auto reference = projected ? compact_reference(*projected, key, path, retained) : path + "." + std::string(key);
        if (canonical_json(original).size() <= reference.size() + key.size() + 48) continue;
        references[key] = reference;
        value.as_object().erase(key);
    }
    if (!references.empty()) value.as_object()["duplicate_fields"] = std::move(references);
}
bool same_submission(const Json& left, const Json& right) {
    return compact_member(left, "original_binding").is_object() &&
        compact_same(left, right, {"submission_id", "request_id", "task_id", "team_id",
            "state_version", "original_binding", "record_sha256"});
}
void compact_submission(Json& value, const Json& retained, const std::string& path, const Json* projected) {
    if (same_submission(value, retained))
        compact_duplicates(value, retained, {"body", "artifact_snapshot", "checks", "acceptance"}, path, projected);
}
void compact_metadata(Json& value, const Json& binding, const std::string& path,
        std::initializer_list<std::string_view> keys, std::initializer_list<std::string_view> display_columns) {
    if (!value.is_object() || value.as_object().contains("binding_ref") ||
        !compact_same(value, binding, {"request_id"})) return;
    for (const auto key : keys) if (const auto* item = value.as_object().if_contains(key))
        if (!compact_equal(*item, compact_member(binding, key))) return;
    if (const auto* revision = value.as_object().if_contains("based_on_requirements_revision"))
        if (!compact_equal(*revision, compact_member(binding, "requirements_revision"))) return;
    auto projected = value;
    for (const auto key : keys) projected.as_object().erase(key);
    for (const auto key : display_columns) projected.as_object().erase(key);
    projected.as_object()["binding_ref"] = path;
    if (canonical_json(projected).size() < canonical_json(value).size()) value = std::move(projected);
}
void compact_plan_tasks(Json& state, const Json& original) {
    const auto& plan = compact_member(original, "plan");
    const auto& nodes = compact_member(compact_member(plan, "body"), "nodes");
    const auto& tasks = compact_member(original, "tasks");
    if (!tasks.is_array()) return;
    for (std::size_t i = 0; i < tasks.as_array().size(); ++i) {
        const auto& task = tasks.as_array()[i];
        const auto path = "current_state.tasks[" + std::to_string(i) + "]";
        if (nodes.is_array() && compact_same(task, plan, {"request_id", "plan_revision", "based_on_requirements_revision"}))
          for (std::size_t j = 0; j < nodes.as_array().size(); ++j)
            if (compact_same(task, nodes.as_array()[j], {"node_id"}))
                compact_duplicates(state.at("tasks").as_array()[i], nodes.as_array()[j], {"brief"},
                    "current_state.plan.body.nodes[" + std::to_string(j) + "]");
        if (compact_same(compact_member(original, "acceptance_task"), task,
                {"request_id", "task_id", "state_version", "based_on_requirements_revision", "plan_revision", "task_revision", "instruction_revision"}))
            compact_duplicates(state.at("acceptance_task"), task, {"brief"}, path, &state.at("tasks").as_array()[i]);
    }
}
void compact_files(Json& submission, const Json& views, const std::string& path) {
    const auto& files = compact_member(submission, "artifact_snapshot");
    if (!files.is_array() || !views.is_array() || files.as_array().size() != views.as_array().size()) return;
    for (std::size_t i = 0; i < views.as_array().size(); ++i) {
        auto& file = submission.at("artifact_snapshot").as_array()[i];
        const auto& view = views.as_array()[i];
        const auto& content = compact_member(view, "content");
        const auto& encoded = compact_member(file, "content_base64");
        const auto& size = compact_member(file, "size_bytes");
        if (!file.is_object() || file.as_object().contains("content_ref") || !content.is_string() ||
            !encoded.is_string() || compact_member(view, "binary") != false ||
            compact_member(view, "source") != "immutable_submission_snapshot" ||
            !compact_same(file, view, {"path", "sha256", "size_bytes"})) continue;
        const auto& bytes = content.as_string();
        if (bytes.size() > 65536 || !(size.is_int64() ? size.as_int64() == static_cast<std::int64_t>(bytes.size()) :
                size.is_uint64() && size.as_uint64() == bytes.size()) ||
            compact_member(file, "sha256") != Json(sha256_hex(std::string_view(bytes.data(), bytes.size())))) continue;
        try { (void)canonical_json(content); }
        catch (const Error& error) { if (error.code() == ErrorCode::InvalidInput) continue; throw; }
        std::string base64(4 * ((bytes.size() + 2) / 3) + 1, '\0');
        const auto count = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(base64.data()),
            reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
        if (count < 0) continue;
        base64.resize(static_cast<std::size_t>(count));
        if (encoded != Json(base64)) continue;
        file.as_object().erase("content_base64");
        file.as_object()["content_ref"] = path + "[" + std::to_string(i) + "].content";
    }
}
bool actionable_keys(const Json& value, std::initializer_list<std::string_view> keys) {
    return value.is_object() && std::all_of(value.as_object().begin(), value.as_object().end(), [&](const auto& item) {
        return std::find(keys.begin(), keys.end(), std::string_view(item.key().data(), item.key().size())) != keys.end();
    });
}
std::optional<std::int64_t> actionable_integer(const Json& value) {
    if (value.is_int64() && value.as_int64() >= 0) return value.as_int64();
    if (value.is_uint64() && value.as_uint64() <= INT64_MAX) return static_cast<std::int64_t>(value.as_uint64());
    return std::nullopt;
}
std::string write_summary_output(const std::string& content, const Json& call, const Json& source,
        const Json& original_source, const std::string& provider_call_id) {
    // A send-only copy of one settled native fs_write receipt. Unknown shapes
    // and inconsistent facts retain the complete original, including errors.
    if (call.at("name") != "fs_write" || !compact_equal(source, original_source) ||
            compact_member(source, "kind") != "recorded_tool_turn" || compact_member(source, "status") != "COMPLETED" ||
            actionable_integer(compact_member(source, "accepted")) != 1 ||
            actionable_integer(compact_member(source, "accounting_attention")) != 0 ||
            compact_member(source, "not_execution_authority") != true ||
            compact_member(source, "original_tool_call_id") != Json(provider_call_id)) return content;
    Json receipt;
    try { receipt = parse_json_strict(content); }
    catch (const Error&) { return content; }
    const auto shape = [](const Json& value, std::initializer_list<std::string_view> strings,
            std::initializer_list<std::string_view> integers, std::initializer_list<std::string_view> other) {
        if (!value.is_object() || value.as_object().size() != strings.size() + integers.size() + other.size()) return false;
        for (const auto key : strings) {
            const auto& item = compact_member(value, key);
            if (!item.is_string() || item.as_string().empty() || item.as_string().find('\0') != boost::json::string::npos) return false;
        }
        for (const auto key : integers) if (!actionable_integer(compact_member(value, key))) return false;
        for (const auto key : other) if (!value.as_object().contains(key)) return false;
        return true;
    };
    const auto hash = [](const Json& value) {
        return value.is_string() && value.as_string().size() == 64 &&
            std::all_of(value.as_string().begin(), value.as_string().end(), [](char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            });
    };
    const auto& op = compact_member(receipt, "operation");
    const auto& execution = compact_member(receipt, "execution_result");
    const auto& result = compact_member(execution, "result");
    const auto& evidence = compact_member(execution, "evidence");
    const auto& binding = compact_member(execution, "original_binding");
    const auto& args = compact_member(op, "arguments");
    if (!shape(receipt, {}, {}, {"operation", "execution_result", "executed_now", "replayed"}) ||
        !shape(op, {"args_sha256", "business_idempotency_key", "created_at", "ended_at", "evidence_ref", "op_id", "request_id",
            "side_effect", "started_at", "status", "task_id", "team_id", "tool_id"}, {"dispatcher_epoch", "execution_epoch",
            "instruction_revision", "plan_revision", "policy_epoch", "requirements_revision", "state_version", "task_revision"},
            {"arguments", "error_signature", "exit_code", "output_dir"}) ||
        !shape(execution, {"observed_outcome", "op_id", "operation_status", "record_sha256", "recorded_at", "stderr_sha256", "stdout_sha256"},
            {"accepted", "stderr_bytes", "stderr_truncated", "stdout_bytes", "stdout_truncated"}, {"evidence", "original_binding", "result"}) ||
        !shape(result, {"path", "sha256"}, {"size_bytes"}, {"created", "previous_sha256"}) ||
        !shape(evidence, {"after_sha256", "path", "temporary_path"}, {"root_device", "root_inode", "size_bytes"},
            {"before_sha256", "durable", "renamed", "temporary_cleanup_complete", "temporary_remaining"}) ||
        !shape(binding, {"request_id", "task_id", "team_id"}, {"dispatcher_epoch", "execution_epoch", "instruction_revision",
            "plan_revision", "policy_epoch", "requirements_revision", "task_revision"}, {})) return content;
    if (op.at("tool_id") != "fs_write" || op.at("side_effect") != "workspace" || op.at("status") != "SUCCEEDED" ||
        execution.at("operation_status") != "SUCCEEDED" || execution.at("observed_outcome") != "SUCCEEDED" ||
        actionable_integer(execution.at("accepted")) != 1 || !compact_same(op, execution, {"op_id"}) ||
        !compact_equal(binding, compact_member(source, "frozen_binding")) ||
        !hash(compact_member(source, "record_sha256")) || !hash(execution.at("record_sha256"))) return content;
    for (const auto& item : binding.as_object()) if (!compact_equal(item.value(), op.at(item.key()))) return content;
    for (const auto key : {"error_signature", "exit_code", "output_dir"}) if (!op.at(key).is_null()) return content;
    for (const auto key : {"stdout_bytes", "stderr_bytes", "stdout_truncated", "stderr_truncated"})
        if (actionable_integer(execution.at(key)) != 0) return content;
    for (const auto key : {"stdout_sha256", "stderr_sha256"}) if (execution.at(key) != Json(sha256_hex(""))) return content;
    // Store fingerprints boolean truncation flags before its SQL integer view.
    const auto record_hash = sha256_hex(canonical_json(Object{{"op_id", execution.at("op_id")},
        {"original_binding", binding}, {"outcome", execution.at("observed_outcome")}, {"result", result}, {"evidence", evidence},
        {"stdout_bytes", execution.at("stdout_bytes")}, {"stderr_bytes", execution.at("stderr_bytes")},
        {"stdout_sha256", execution.at("stdout_sha256")}, {"stderr_sha256", execution.at("stderr_sha256")},
        {"stdout_truncated", false}, {"stderr_truncated", false}}));
    if (execution.at("record_sha256") != Json(record_hash) ||
        op.at("evidence_ref") != Json("store:operation_result:" + text(op, "op_id") + "#" + record_hash)) return content;
    if (!receipt.at("replayed").is_bool() || !receipt.at("executed_now").is_bool() ||
        receipt.at("replayed") == receipt.at("executed_now") || !result.at("created").is_bool() ||
        !hash(result.at("sha256")) || !compact_same(result, evidence, {"path", "size_bytes"}) ||
        result.at("sha256") != evidence.at("after_sha256") || !compact_equal(result.at("previous_sha256"), evidence.at("before_sha256")) ||
        evidence.at("durable") != true || evidence.at("renamed") != true ||
        evidence.at("temporary_cleanup_complete") != true || evidence.at("temporary_remaining") != false) return content;
    const bool created = result.at("created").as_bool();
    if (created ? !result.at("previous_sha256").is_null() : !hash(result.at("previous_sha256"))) return content;
    if (!(created ? shape(args, {"path"}, {}, {"content"}) : shape(args, {"path", "expected_sha256"}, {}, {"content"})) ||
        !compact_equal(args, call.at("arguments")) || !args.at("content").is_string() || args.at("path") != result.at("path") ||
        (!created && args.at("expected_sha256") != result.at("previous_sha256")) ||
        op.at("args_sha256") != Json(sha256_hex(canonical_json(args)))) return content;
    const auto& bytes = args.at("content").as_string();
    if (actionable_integer(result.at("size_bytes")) != static_cast<std::int64_t>(bytes.size()) ||
        result.at("sha256") != Json(sha256_hex(std::string_view(bytes.data(), bytes.size())))) return content;
    auto summary = result.as_object();
    summary["status"] = op.at("status"); summary["executed_now"] = receipt.at("executed_now"); summary["replayed"] = receipt.at("replayed");
    return canonical_json(summary);
}
void actionable_budget(Json& value, const Json& binding) {
    if (!actionable_keys(value, {"request_id", "state_version", "max_attempts", "attempts", "input_limit", "output_limit", "cost_limit",
            "spent_input", "spent_output", "spent_cost", "reserved_input", "reserved_output", "reserved_cost", "unknown_input", "unknown_output", "unknown_cost",
            "cost_policy", "money_limit_micro_usd", "total_cost_micro_usd", "unknown_cost_attempts", "accounting_format", "cost_breakdown"}) ||
        !compact_same(value, binding, {"request_id"})) return;
    const auto integer = [&](std::string_view key) { return actionable_integer(compact_member(value, key)); };
    const auto maximum = integer("max_attempts"), attempts = integer("attempts");
    if (!maximum || !attempts || !integer("state_version") || *attempts > *maximum) return;
    Object remaining{{"calls", *maximum - *attempts}};
    for (const auto dimension : {"input", "output", "cost"}) {
        const std::string name(dimension);
        const auto limit = integer(name + "_limit"), spent = integer("spent_" + name), reserved = integer("reserved_" + name), unknown = integer("unknown_" + name);
        if (!limit || !spent || !reserved || !unknown || *unknown > *reserved || *spent > *limit || *reserved > *limit - *spent) return;
        if (name != "cost") remaining[name + "_tokens"] = *limit - *spent - *reserved;
    }
    const bool observe = compact_member(value, "cost_policy") == "observe-only-v1";
    if (value.as_object().contains("cost_policy") && !observe) return;
    if (observe && (!value.as_object().contains("money_limit_micro_usd") || !value.at("money_limit_micro_usd").is_null() ||
            !value.as_object().contains("total_cost_micro_usd") || !integer("unknown_cost_attempts") || *integer("unknown_cost_attempts") > *attempts)) return;
    if (value.as_object().contains("unknown_cost_attempts") && !integer("unknown_cost_attempts")) return;
    if (const auto* format = value.as_object().if_contains("accounting_format"); format && *format != "wls-estimated-budget-v1") return;
    for (const auto key : {"money_limit_micro_usd", "total_cost_micro_usd"})
        if (const auto* item = value.as_object().if_contains(key); item && !item->is_null() && !actionable_integer(*item)) return;
    const auto& breakdown = compact_member(value, "cost_breakdown");
    if (value.as_object().contains("cost_breakdown") && !breakdown.is_object()) return;
    if (!breakdown.is_null()) {
        if (!actionable_keys(breakdown, {"estimated_micro_usd", "reported_micro_usd", "reservation_charged_micro_usd", "unknown_reserved_micro_usd",
                "pricing_sha256", "cost_policy", "unknown_cost_attempts", "total_cost_micro_usd", "known_cost_micro_usd"})) return;
        for (const auto& item : breakdown.as_object()) {
            if (item.key() == "pricing_sha256") {
                if (!item.value().is_array() || !std::all_of(item.value().as_array().begin(), item.value().as_array().end(),
                        [](const Json& hash) { return hash.is_string() && hash.as_string().size() == 64; })) return;
            } else if (item.key() == "cost_policy" || item.key() == "unknown_cost_attempts" || item.key() == "total_cost_micro_usd") {
                if (!value.as_object().contains(item.key()) || !compact_equal(item.value(), value.at(item.key()))) return;
            } else if (!(observe && item.key() == "estimated_micro_usd" && item.value().is_null()) && !actionable_integer(item.value())) return;
        }
        if (const auto* unknown = breakdown.as_object().if_contains("unknown_reserved_micro_usd"); unknown &&
                (!actionable_integer(*unknown) || *actionable_integer(*unknown) != 0)) return;
    }
    auto projected = value;
    for (const auto key : {"request_id", "state_version", "max_attempts", "attempts", "input_limit", "spent_input", "reserved_input", "output_limit", "spent_output", "reserved_output"})
        projected.as_object().erase(key);
    projected.as_object()["remaining_at_capture"] = std::move(remaining);
    if (observe && *integer("cost_limit") == 0 && *integer("spent_cost") == 0 && *integer("reserved_cost") == 0) {
        for (const auto key : {"cost_limit", "spent_cost", "reserved_cost"}) projected.as_object().erase(key);
        if (breakdown.is_object()) {
            auto& costs = projected.at("cost_breakdown").as_object();
            for (const auto key : {"estimated_micro_usd", "reported_micro_usd", "reservation_charged_micro_usd", "pricing_sha256", "cost_policy", "total_cost_micro_usd", "unknown_cost_attempts"}) costs.erase(key);
            if (*integer("unknown_cost") == 0) costs.erase("unknown_reserved_micro_usd");
            if (compact_member(breakdown, "known_cost_micro_usd") == Json(0)) costs.erase("known_cost_micro_usd");
            if (costs.empty()) projected.as_object().erase("cost_breakdown");
        }
    }
    value = std::move(projected);
}
void actionable_actor(Json& actor, const Json& binding, std::string_view role) {
    if (actor.is_object() && actor.as_object().size() == 7 && actionable_keys(actor,
            {"role", "principal_id", "request_id", "task_id", "team_id", "dispatcher_epoch", "execution_epoch"}) &&
        compact_member(actor, "role") == Json(role) && compact_member(actor, "principal_id").is_string() &&
        compact_same(actor, binding, {"request_id", "task_id", "team_id", "dispatcher_epoch", "execution_epoch"}))
        actor = Object{{"role", role}};
}
void actionable_state(Json& state, const Json& original, Role role, const Json& semantic) {
    const auto& binding = compact_member(original, "binding");
    if (auto* budget = state.as_object().if_contains("budget")) actionable_budget(*budget, binding);
    if (role == Role::Worker && compact_member(compact_member(original, "task"), "kind") == "team_task") {
        auto& task = state.at("task").as_object();
        const auto captured_null = [&](std::string_view key) { const auto* item = original.at("task").as_object().if_contains(key); return item && item->is_null(); };
        if (captured_null("capability") && captured_null("capability_input") && captured_null("callee_request_id"))
            for (const auto key : {"capability", "capability_input", "callee_request_id", "template_id"}) task.erase(key);
        const auto review_round = actionable_integer(compact_member(original.at("task"), "review_round"));
        if (captured_null("review_config") && review_round && *review_round == 0) task.erase("review_round");
    }
    if (auto* events = state.as_object().if_contains("pending_events"); events && events->is_array())
      for (std::size_t i = 0; i < events->as_array().size(); ++i) {
        auto& event = events->as_array()[i]; const auto& source = original.at("pending_events").as_array()[i];
        const auto& type = compact_member(source, "type"); const auto& payload = compact_member(source, "payload");
        const auto& target = compact_member(compact_member(original, "workflow_action"), "target");
        if ((type != "instruction" && type != "ack" && type != "tool_feedback" && type != "admit" && type != "independent_plan" &&
                type != "submit_work" && type != "check_submission" && type != "accept_candidate") ||
            compact_member(source, "role") != Json(role_name(role)) || !compact_same(source, binding, {"request_id"}) ||
            !compact_equal(compact_member(source, "event_requirements_revision"), compact_member(binding, "requirements_revision")) ||
            !(compact_equal(compact_member(source, "subject_task_id"), compact_member(binding, "task_id")) ||
                (role == Role::Dispatcher && compact_same(source, target, {"request_id"}) && compact_equal(compact_member(source, "subject_task_id"), compact_member(target, "task_id"))))) continue;
        for (const auto key : {"inbox_id", "event_seq", "event_id", "request_id", "role", "actor_task_id", "claim_id", "created_at", "completed_at"}) event.as_object().erase(key);
        if (payload.is_object() && compact_member(payload, "type") == type) {
            if (auto* actor = event.at("payload").as_object().if_contains("actor"))
                actionable_actor(*actor, compact_member(payload, "binding"), type == "instruction" || type == "check_submission" ? "team_lead" : type == "accept_candidate" ? "dispatcher" : "worker");
            if (type == "tool_feedback" && actionable_keys(payload, {"type", "attempt_id", "binding", "record_sha256"}) &&
                actionable_keys(compact_member(source, "result"), {"attempt_id", "record_sha256"}) && compact_same(payload, compact_member(source, "result"), {"attempt_id", "record_sha256"}) &&
                compact_equal(compact_member(payload, "binding"), binding)) {
                auto& notice = event.at("payload").as_object(); notice.erase("type"); notice.erase("binding"); notice.erase("duplicate_fields");
                notice["current_binding"] = true; event.as_object().erase("result");
            }
            if (type == "admit" && compact_member(payload, "immutable").is_object()) {
                auto& immutable = event.at("payload").at("immutable"); const auto& request = compact_member(semantic, "request");
                if (const auto& texts = compact_member(request, "texts"); texts.is_array()) for (std::size_t j = 0; j < texts.as_array().size(); ++j) {
                    const auto& text = texts.as_array()[j];
                    if (compact_member(text, "kind") == "ORIGINAL" && compact_member(text, "requirements_revision") == Json(1) &&
                        compact_equal(compact_member(immutable, "original_text"), compact_member(text, "text")))
                        immutable.as_object()["original_text"] = Object{{"semantic_contract_ref", "request.texts[" + std::to_string(j) + "].text"}};
                }
                if (!compact_member(immutable, "constraint_catalog").is_null() && compact_equal(immutable.at("constraint_catalog"), compact_member(request, "constraint_catalog")))
                    immutable.as_object()["constraint_catalog"] = Object{{"semantic_contract_ref", "request.constraint_catalog"}};
                for (const auto key : {"function_id", "config_snapshot_sha256", "budget"}) immutable.as_object().erase(key);
            }
        }
    }
    const auto& action = compact_member(original, "workflow_action");
    if (role != Role::Dispatcher || (compact_member(action, "kind") != "acceptance" && compact_member(action, "kind") != "rejection")) return;
    const auto& producer = compact_member(compact_member(action, "target"), "original_binding");
    if (!compact_member(action, "target").is_object() || !producer.is_object()) return;
    const std::string path = "current_state.workflow_action.target.original_binding";
    for (const auto key : {"acceptance_task", "acceptance_instruction"}) if (auto* value = state.as_object().if_contains(key)) {
        if (!compact_same(*value, producer, {"request_id", "task_id", "plan_revision", "task_revision", "instruction_revision"}) ||
                !compact_equal(compact_member(*value, "based_on_requirements_revision"), compact_member(producer, "requirements_revision"))) continue;
        compact_metadata(*value, producer, path, std::string_view(key) == "acceptance_task"
                ? std::initializer_list<std::string_view>{"request_id", "task_id", "plan_revision", "task_revision", "instruction_revision"}
                : std::initializer_list<std::string_view>{"request_id", "task_id", "team_id", "plan_revision", "task_revision", "instruction_revision", "dispatcher_epoch", "execution_epoch"},
            std::string_view(key) == "acceptance_task" ? std::initializer_list<std::string_view>{"created_at", "updated_at"} : std::initializer_list<std::string_view>{"created_at"});
        if (std::string_view(key) == "acceptance_instruction" && compact_member(*value, "body").is_object())
            compact_metadata(value->at("body"), producer, path, {"request_id", "task_id", "team_id", "requirements_revision", "plan_revision", "task_revision", "instruction_revision", "dispatcher_epoch", "execution_epoch", "policy_epoch"}, {});
    }
    auto& target = state.at("workflow_action").at("target");
    for (const auto key : {"created_at", "updated_at"}) target.as_object().erase(key);
    if (auto* checks = target.as_object().if_contains("checks"); checks && checks->is_array()) for (auto& check : checks->as_array()) if (check.is_object()) {
        if (auto* actor = check.as_object().if_contains("actor")) actionable_actor(*actor, compact_member(check, "captured_binding"), "team_lead");
        if (compact_equal(compact_member(check, "captured_binding"), producer) && producer.is_object()) check.as_object()["captured_binding"] = Object{{"binding_ref", path}};
        check.as_object().erase("created_at");
    }
}
} // namespace

Json lean_state(Json state, Role role, const Json& latest_contract) {
    state.as_object().erase("message_sources");
    for (auto& window : state.at("history_windows").as_array()) window.as_object().erase("source");
    const auto* marker = state.as_object().if_contains("compact_model_state");
    const auto* actionable = state.as_object().if_contains("actionable_model_state");
    require(!actionable || (actionable->is_bool() && actionable->as_bool() && marker && marker->is_bool() && marker->as_bool()),
        ErrorCode::InvalidInput, "invalid actionable model state marker");
    const bool project_action = actionable != nullptr;
    if (!marker) return state; // Old captured attempts retain their exact wire and admission hashes.
    require(marker->is_bool() && marker->as_bool(), ErrorCode::InvalidInput, "invalid compact model state marker");
    state.as_object().erase("compact_model_state");
    state.as_object().erase("actionable_model_state");
    const auto original = state;
    const auto& binding = compact_member(original, "binding");
    const auto& action = compact_member(original, "workflow_action");
    const auto& kind = compact_member(action, "kind");
    const Json* retained = nullptr;
    Json* projected = nullptr;
    std::string target_path, views_key;
    if ((role == Role::TeamLead && kind == "submission") ||
            (role == Role::Dispatcher && (kind == "acceptance" || kind == "rejection"))) {
        retained = &compact_member(action, "target");
        if (retained->is_object()) projected = &state.at("workflow_action").at("target");
        target_path = "current_state.workflow_action.target";
        views_key = "submission_file_views";
    } else if (role == Role::TeamLead && kind == "experience") {
        const auto& source = compact_member(compact_member(action, "target"), "source_snapshot");
        retained = &compact_member(source, "submission");
        if (retained->is_object()) projected = &state.at("workflow_action").at("target").at("source_snapshot").at("submission");
        target_path = "current_state.workflow_action.target.source_snapshot.submission";
        views_key = "experience_file_views";
        if (projected) compact_duplicates(*projected, source, {"acceptance"},
            "current_state.workflow_action.target.source_snapshot");
        const auto& scope = compact_member(original, "experience_scope");
        if (projected && binding.is_object() && compact_equal(compact_member(*retained, "original_binding"), binding) &&
                compact_equal(compact_member(scope, "original_source_binding"), binding) && compact_equal(compact_member(scope, "control_binding"), binding) &&
                compact_same(scope, compact_member(action, "target"), {"source_sha256"})) {
            auto& wire_source = state.at("workflow_action").at("target").at("source_snapshot");
            for (const auto section : {"task", "instruction"}) {
                const auto& semantic = compact_member(latest_contract, section);
                const auto& original_binding = compact_member(compact_member(original, "original_contract_bindings"), section);
                if (!semantic.is_object() || !original_binding.is_object()) continue;
                const bool instruction = std::string_view(section) == "instruction";
                const auto& metadata = instruction ? compact_member(source, "instruction") : compact_member(original, "task");
                if (!compact_same(metadata, binding, {"request_id", "task_id", "plan_revision", "task_revision", "instruction_revision"}) ||
                        !compact_equal(compact_member(metadata, "based_on_requirements_revision"), compact_member(binding, "requirements_revision")) ||
                        (instruction && (!compact_equal(original_binding, binding) ||
                            !compact_same(metadata, binding, {"team_id", "dispatcher_epoch", "execution_epoch"})))) continue;
                auto complete = semantic;
                for (const auto& entry : original_binding.as_object()) complete.as_object()[entry.key()] = entry.value();
                const auto& original_body = instruction ? compact_member(metadata, "body") : compact_member(source, "brief");
                if (!compact_equal(complete, original_body)) continue;
                Object reference{{"semantic_contract_ref", section}};
                if (!original_binding.as_object().empty()) reference["original_binding_ref"] = compact_equal(original_binding, binding)
                    ? "current_state.binding" : "current_state.original_contract_bindings." + std::string(section);
                if (canonical_json(reference).size() >= canonical_json(original_body).size()) continue;
                if (instruction) wire_source.at("instruction").as_object()["body"] = std::move(reference);
                else wire_source.as_object()["brief"] = std::move(reference);
            }
        }
    }
    if (projected && compact_member(state, "submission").is_object())
        compact_submission(state.at("submission"), *retained, target_path, projected);
    auto* events = state.as_object().if_contains("pending_events");
    if (events && events->is_array()) for (std::size_t i = 0; i < events->as_array().size(); ++i) {
        auto& event = events->as_array()[i];
        const auto& source = original.at("pending_events").as_array()[i];
        const auto& type = compact_member(source, "type");
        const auto& payload = compact_member(source, "payload");
        const auto& result = compact_member(source, "result");
        if ((type == "instruction" || type == "ack" || type == "tool_feedback") &&
                compact_member(payload, "type") == type && binding.is_object() &&
                compact_member(source, "role") == Json(role_name(role)) && compact_same(source, binding, {"request_id"}) &&
                compact_equal(compact_member(source, "subject_task_id"), compact_member(binding, "task_id")) &&
                compact_equal(compact_member(source, "event_requirements_revision"), compact_member(binding, "requirements_revision"))) {
            const auto& order_binding = compact_member(compact_member(original, "original_contract_bindings"), "instruction");
            const auto& order = compact_member(latest_contract, "instruction");
            if (type == "instruction" && order.is_object() && compact_equal(order_binding, binding) &&
                    compact_equal(compact_member(result, "binding"), order_binding) &&
                    compact_same(compact_member(original, "task"), binding,
                        {"request_id", "task_id", "plan_revision", "task_revision", "instruction_revision"}) &&
                    compact_equal(compact_member(compact_member(original, "task"), "based_on_requirements_revision"), compact_member(binding, "requirements_revision")) &&
                    compact_equal(compact_member(compact_member(original, "instruction"), "based_on_requirements_revision"), compact_member(binding, "requirements_revision")) &&
                    compact_same(compact_member(original, "semantic_versions"), binding,
                        {"requirements_revision", "task_revision", "instruction_revision"}) &&
                    compact_same(compact_member(original, "instruction"), order_binding,
                        {"request_id", "task_id", "team_id", "plan_revision", "task_revision", "instruction_revision", "dispatcher_epoch", "execution_epoch"})) {
                auto complete = order;
                for (const auto& entry : order_binding.as_object()) complete.as_object()[entry.key()] = entry.value();
                if (compact_equal(complete, compact_member(payload, "body")))
                    event.at("payload").as_object()["body"] = Object{{"semantic_contract_ref", "instruction"},
                        {"original_binding_ref", "current_state.binding"}};
            }
            compact_duplicates(event.at("payload"), original, {"binding"}, "current_state");
            if (result.is_object()) compact_duplicates(event.at("result"), original, {"binding"}, "current_state");
            if (type == "tool_feedback" && result.is_object() && result.as_object().size() == 2 &&
                    compact_same(payload, result, {"attempt_id", "record_sha256"}))
                event.as_object()["result"] = Object{{"payload_fields", Array{"attempt_id", "record_sha256"}}};
            event.as_object().erase("created_at");
            continue;
        }
        if ((type != "submit_work" && type != "check_submission" && type != "accept_candidate") ||
            compact_member(payload, "type") != type || !result.is_object() ||
            compact_member(source, "role") != Json(role_name(role)) ||
            !compact_same(source, result, {"request_id"}) ||
            !compact_equal(compact_member(source, "subject_task_id"), compact_member(result, "task_id"))) continue;
        const auto result_path = "current_state.pending_events[" + std::to_string(i) + "].result";
        if (projected) compact_submission(event.at("result"), *retained, target_path, projected);
        if (type == "submit_work" && compact_equal(compact_member(payload, "binding"), compact_member(result, "original_binding")) &&
                compact_member(result, "original_binding").is_object() && compact_member(result, "body").is_object()) {
            compact_duplicates(event.at("payload"), result, {"body"}, result_path, &event.at("result"));
        } else if (type == "check_submission" && compact_member(result, "checks").is_array()) {
            const auto& version = compact_member(payload, "expected_submission_version");
            const auto& checks = result.at("checks").as_array();
            for (std::size_t j = 0; j < checks.size(); ++j) {
                const auto& check = checks[j];
                if (version.is_int64() && version.as_int64() > 0 && version.as_int64() < INT64_MAX &&
                    compact_same(payload, check, {"submission_id"}) && compact_same(result, check, {"submission_id", "status"}) &&
                    compact_member(payload, "binding").is_object() && compact_equal(compact_member(payload, "binding"), compact_member(check, "captured_binding")) &&
                    compact_equal(compact_member(check, "submission_version"), Json(version.as_int64() + 1)) &&
                    compact_equal(compact_member(result, "state_version"), compact_member(check, "submission_version")))
                    compact_duplicates(event.at("payload"), check, {"check_body", "rework_order"},
                        compact_reference(event.at("result"), "checks", result_path, result) + "[" + std::to_string(j) + "]");
            }
        } else if (type == "accept_candidate") {
            const auto& acceptance = compact_member(result, "acceptance");
            if (compact_same(payload, acceptance, {"submission_id"}) &&
                compact_member(payload, "binding").is_object() &&
                !compact_member(payload, "expected_submission_version").is_null() && !compact_member(payload, "expected_task_version").is_null() &&
                compact_equal(compact_member(payload, "binding"), compact_member(acceptance, "captured_binding")) &&
                compact_equal(compact_member(payload, "expected_submission_version"), compact_member(acceptance, "submission_version")) &&
                compact_equal(compact_member(payload, "expected_task_version"), compact_member(acceptance, "task_version")))
                compact_duplicates(event.at("payload"), acceptance, {"decision"}, compact_reference(event.at("result"), "acceptance", result_path, result));
        }
    }
    if (projected && (kind != "submission" || same_submission(compact_member(original, "submission"), *retained)))
        compact_files(*projected, compact_member(original, views_key), "current_state." + views_key);
    if (role == Role::Dispatcher) compact_plan_tasks(state, original);
    if (projected && role == Role::Dispatcher && (kind == "acceptance" || kind == "rejection") &&
            compact_same(compact_member(original, "acceptance_task"), compact_member(original, "acceptance_instruction"),
                {"request_id", "task_id", "based_on_requirements_revision", "plan_revision", "task_revision", "instruction_revision"})) {
        const auto& instruction = original.at("acceptance_instruction");
        const auto& producer = compact_member(*retained, "original_binding");
        if (compact_same(compact_member(instruction, "body"), producer, {"request_id", "task_id", "team_id", "requirements_revision",
                "plan_revision", "task_revision", "instruction_revision", "dispatcher_epoch", "execution_epoch", "policy_epoch"}))
            compact_duplicates(state.at("acceptance_instruction").at("body"), compact_member(original.at("acceptance_task"), "brief"),
                {"constraints_verbatim", "constraint_refs"}, compact_reference(state.at("acceptance_task"), "brief", "current_state.acceptance_task", original.at("acceptance_task")));
    }
    if (auto* request = state.as_object().if_contains("request"))
        compact_metadata(*request, binding, "current_state.binding", {"request_id", "requirements_revision"}, {});
    if (auto* task = state.as_object().if_contains("task")) compact_metadata(*task, binding, "current_state.binding",
        {"request_id", "task_id", "plan_revision", "task_revision", "instruction_revision"}, {"created_at", "updated_at"});
    const auto& originals = compact_member(original, "original_contract_bindings");
    if (auto* instruction = state.as_object().if_contains("instruction"))
        compact_metadata(*instruction, compact_member(originals, "instruction"),
            compact_equal(compact_member(originals, "instruction"), binding) ? "current_state.binding" : "current_state.original_contract_bindings.instruction",
            {"request_id", "task_id", "team_id", "plan_revision", "task_revision", "instruction_revision", "dispatcher_epoch", "execution_epoch"}, {"created_at"});
    if (originals.is_object() && compact_member(binding, "request_id").is_string()) for (const auto section : {"task", "instruction"})
        if (compact_equal(compact_member(originals, section), binding))
            state.at("original_contract_bindings").as_object()[section] = Object{{"binding_ref", "current_state.binding"}};
    if (project_action) actionable_state(state, original, role, latest_contract);
    return state;
}
void seal(EncodedResponsesRequest& value) { value.record_sha256 = sha256_hex(canonical_json(encoded_record(value))); }
void seal(DecodedResponsesResponse& value) { value.record_sha256 = sha256_hex(canonical_json(decoded_record(value))); }
} // namespace
using namespace protocol_detail;

EncodedResponsesRequest encode_responses_request(const ProviderProfile& profile,
        const ModelRequest& request, bool stream) {
    const auto envelope = validate_request(profile, request, stream);
    const auto& logical = request.context.logical_input;
    const auto& prefix = logical.at("prefix");
    const bool v3 = logical.at("format") == "wls-logical-input-v3";
    const bool lean = logical.as_object().contains("model_projection") && logical.at("model_projection") == "lean-v1";
    std::map<std::string, std::string> historical_tools;
    Array tools, tool_versions, sources;
    for (const auto& definition : array(prefix.at("tools"))) {
        const auto name = text(definition, "name");
        tools.push_back(Object{{"type", "function"}, {"name", name},
            {"parameters", definition.at("parameters")}, {"strict", false}});
        if (logical.at("format") == "wls-logical-input-v3")
            tools.back().as_object()["description"] = definition.at("description");
        tool_versions.push_back(Object{{"name", name}, {"version", definition.at("version")}});
    }
    std::size_t index = 0;
    for (const auto& message : array(logical.at("messages"))) {
        if (lean) for (const auto& call : array(message.at("tool_calls")))
            historical_tools.emplace(text(call, "call_id"), text(call, "name"));
        if (!lean && !message.at("source").is_null())
            sources.push_back(Object{{"message_index", index}, {"source", message.at("source")}});
        ++index;
    }
    Array input;
    if (lean) input.push_back(Object{{"role", "user"}, {"content", canonical_json(logical.at("contract_anchor"))}});
    else input.push_back(Object{{"role", "user"}, {"content", canonical_json(Object{
        {"format", "wls-responses-task-v1"}, {"task_contract", logical.at("task_contract")},
        {"message_sources", sources}})}});
    if (v3 && !lean) input.front().at("content") = canonical_json(Object{{"format", "wls-responses-contract-v3"},
        {"task_contract", logical.at("contract_anchor")}});
    std::size_t contract_bytes = canonical_json(input.front()).size();
    std::map<std::size_t, Array> updates;
    std::map<std::int64_t, Array> folded_updates;
    if (v3) for (const auto& update : array(logical.at("contract_updates"))) {
        std::size_t boundary = 0, position = 0;
        for (const auto& message : array(logical.at("messages"))) {
            ++position;
            Json identity = message.at("source");
            if ((!identity.is_object() || !identity.as_object().contains("attempt_id")) &&
                    text(message, "role") == "user" && text(message, "content").starts_with("{"))
                identity = parse_json_strict(text(message, "content"));
            if (identity.is_object()) for (const auto* key : {"attempt_id", "record_id"})
                if (const auto* id = identity.as_object().if_contains(key); id && *id == update.at("after_attempt_id"))
                    boundary = position;
        }
        auto contract = update.at("contract");
        if (lean) contract.as_object()["semantic_versions"] = update.at("semantic_versions");
        const Json encoded_update = Object{{"role", "user"}, {"content", lean ? canonical_json(contract) : canonical_json(Object{
            {"format", "wls-responses-contract-update-v3"}, {"task_contract", update.at("contract")},
            {"semantic_versions", update.at("semantic_versions")}})}};
        contract_bytes += canonical_json(encoded_update).size();
        bool folded = false;
        if (const auto* history = logical.as_object().if_contains("worker_history"))
            for (const auto& entry : history->at("folded_entries").as_array()) if (entry.at("attempt_id") == update.at("after_attempt_id")) {
                folded_updates[entry.at("sequence").to_number<std::int64_t>()].push_back(encoded_update); folded = true; break;
            }
        if (!folded) updates[boundary].push_back(encoded_update);
    }
    const auto append_updates = [&](std::size_t position) {
        if (const auto found = updates.find(position); found != updates.end())
            for (const auto& update : found->second) input.push_back(update);
    };
    index = 0;
    const bool compat = profile.responses_compatibility.has_value();
    std::set<std::string> provider_item_ids, provider_call_ids;
    const Array* native_calls = nullptr;    // logical tool_calls of the native round
    const Json* native_source = nullptr;
    std::vector<std::string> native_provider_call_ids;
    std::size_t native_consumed = 0;
    for (const auto& message : array(logical.at("messages"))) {
        append_updates(index++);
        const auto& history_source = message.at("source");
        const auto* kind = history_source.is_object() ? history_source.as_object().if_contains("kind") : nullptr;
        if (v3 && kind && *kind == "history_fold") {
            Array entries;
            const auto emit = [&] {
                if (entries.empty()) return;
                input.push_back(Object{{"role", "user"}, {"content", canonical_json(Object{{"kind", "history_fold"},
                    {"from_sequence", entries.front().at("sequence")}, {"through_sequence", entries.back().at("sequence")}, {"entries", entries}})}});
                entries.clear();
            };
            // One persisted flat record; wire slices retain immutable update boundaries.
            for (const auto& entry : logical.at("worker_history").at("folded_entries").as_array()) {
                entries.push_back(entry);
                if (const auto found = folded_updates.find(entry.at("sequence").to_number<std::int64_t>()); found != folded_updates.end()) {
                    emit(); for (const auto& update : found->second) input.push_back(update);
                }
            }
            emit(); continue;
        }
        if (v3 && kind && (*kind == "history_window" || *kind == "management_history_window")) continue;
        const auto role = text(message, "role");
        const auto content = text(message, "content");
        const auto* original_output = message.as_object().if_contains("responses_output");
        if (original_output && original_output->is_object())
            if (const auto* policy = original_output->as_object().if_contains("response_metadata_policy"))
                require(compat && *policy == Json(profile.response_metadata_policy), ErrorCode::Unsupported,
                        "native output metadata policy differs from the frozen profile");
        const Json* payload = compat ? original_output : nullptr;
        if (payload && !payload->is_null()) {
            require(role == "assistant", ErrorCode::InvalidInput,
                    "only an assistant message may carry opaque responses output");
            const auto& calls = array(message.at("tool_calls"));
            native_provider_call_ids.clear();  // each native round pairs only its own provider IDs
            const auto projection = responses_compat::validate_payload(*payload);
            std::vector<ContextToolInvocation> logical_calls;
            for (const auto& call : calls)
                logical_calls.push_back({std::string(call.at("call_id").as_string()),
                    std::string(call.at("name").as_string()), call.at("arguments")});
            responses_compat::require_projection_match(projection, content, logical_calls);
            for (const auto& item : payload->at("output").as_array()) {
                const auto id = std::string(item.at("id").as_string());
                require(provider_item_ids.insert(id).second, ErrorCode::Conflict,
                        "provider output item ID repeats across the transcript");
                if (std::string(item.at("type").as_string()) == "function_call") {
                    const auto call_id = std::string(item.at("call_id").as_string());
                    require(provider_call_ids.insert(call_id).second, ErrorCode::Conflict,
                            "provider call ID repeats across the transcript");
                    const auto& source = message.at("source");
                    const bool rejected = source.is_object() && source.as_object().contains("rejected_tool_selection") &&
                        source.at("rejected_tool_selection") == true;
                    const bool shared = v3 && logical.at("current_state").as_object().contains("logical_agent_execution");
                    require(rejected || shared || std::find(request.context.tool_names.begin(), request.context.tool_names.end(),
                            std::string(item.at("name").as_string())) != request.context.tool_names.end(),
                            ErrorCode::Unauthorized, "native round replay names an unavailable tool");
                    native_provider_call_ids.push_back(call_id);
                }
                input.push_back(item);  // Original JSON value, unchanged order.
            }
            native_calls = &calls;
            native_source = &history_source;
            native_consumed = 0;
            continue;
        }
        if (compat && role == "assistant" && !array(message.at("tool_calls")).empty())
            throw Error(ErrorCode::Unsupported,
                        "native reasoning continuation requires the original responses output");
        if (role == "tool") {
            const auto output = [&] {
                return lean && historical_tools.at(text(message, "tool_call_id")) == "fs_read"
                    ? lean_read_output(content) : content;
            };
            if (native_calls) {
                // Tool results follow their native round in order; each one
                // pairs positionally with its original provider call ID.
                require(native_consumed < native_calls->size() &&
                        message.at("tool_call_id") == native_calls->at(native_consumed).at("call_id"),
                        ErrorCode::InvalidInput, "native tool result does not pair with its original round");
                input.push_back(Object{{"type", "function_call_output"},
                    {"call_id", native_provider_call_ids.at(native_consumed)}, {"output", profile.responses_tool_result_policy.empty()
                        ? output() : write_summary_output(output(), native_calls->at(native_consumed), history_source,
                            *native_source, native_provider_call_ids.at(native_consumed))}});
                ++native_consumed;
                continue;
            }
            input.push_back(Object{{"type", "function_call_output"},
                {"call_id", message.at("tool_call_id")}, {"output", output()}});
        } else {
            if (!content.empty() || array(message.at("tool_calls")).empty())
                input.push_back(Object{{"role", role}, {"content", content}});
            for (const auto& call : array(message.at("tool_calls")))
                input.push_back(Object{{"type", "function_call"}, {"call_id", call.at("call_id")},
                    {"name", call.at("name")}, {"arguments", canonical_json(call.at("arguments"))}});
            native_calls = nullptr;
        }
    }
    append_updates(index);
    if (v3) input.push_back(Object{{"role", "user"}, {"content", canonical_json(Object{
        {"format", "wls-responses-current-state-v3"}, {"current_state", lean ? lean_state(logical.at("current_state"), request.role,
            logical.at("contract_updates").as_array().empty() ? logical.at("contract_anchor") :
                logical.at("contract_updates").as_array().back().at("contract")) : logical.at("current_state")}})}});
    std::string instructions = canonical_json(Object{{"format", "wls-responses-system-v1"},
        {"system", prefix.at("system")},
        {"dependencies", prefix.at("dependencies")}, {"tool_versions", tool_versions}});
    if (compat) instructions = compose_instructions(prefix.at("system"));
    Object body{{"model", profile.model}, {"instructions", instructions},
        {"input", input}, {"tools", tools}, {"max_output_tokens", request.max_output_tokens},
        {"parallel_tool_calls", profile.parallel_tool_calls}, {"stream", stream},
        {"store", false}, {"background", false}, {"truncation", "disabled"}};
    if (profile.responses_background_policy == "omit-false-v1") body.erase("background");
    if (compat) body["include"] = Array{"reasoning.encrypted_content"};
    if (logical.as_object().contains("tool_policy") && profile.tool_choice_policy == "allowed-tools-v1") {
        Array allowed;
        for (const auto& name : model_allowed_tools(request.context))
            allowed.push_back(Object{{"type", "function"}, {"name", name}});
        body["tool_choice"] = Object{{"type", "allowed_tools"}, {"mode", "required"}, {"tools", allowed}};
    }
    if (profile.tool_choice_policy == "required-v1") body["tool_choice"] = "required";
    if (request.model.reasoning.mode == ReasoningMode::Disabled)
        body["reasoning"] = Object{{"effort", "none"}};
    else if (request.model.reasoning.mode == ReasoningMode::Explicit)
        body["reasoning"] = Object{{"effort", request.model.reasoning.effort}};
    EncodedResponsesRequest encoded;
    encoded.url = profile.base_url + profile.responses_path;
    encoded.body = canonical_json(body);
    std::size_t input_bytes = 0;
    for (const auto& item : input) input_bytes += canonical_json(item).size();
    const auto state_bytes = v3 ? canonical_json(input.back()).size() : 0;
    const auto tools_bytes = canonical_json(tools).size(), system_bytes = canonical_json(Json(instructions)).size();
    encoded.body_segments = Object{{"tools", tools_bytes}, {"system", system_bytes}, {"contract", contract_bytes},
        {"history", input_bytes - contract_bytes - state_bytes}, {"current_state", state_bytes},
        {"other", encoded.body.size() - tools_bytes - system_bytes - input_bytes}, {"total", encoded.body.size()}};
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
    if (compat) {
        evidence["instructions"] = Object{{"format", "wls-responses-instructions-v1"},
            {"sections", Array{"system_prompt", "role_prompt", "function_prompt", "environment_card",
                "approved_rules", "project_snapshot"}},
            {"instructions_sha256", sha256_hex(instructions)}, {"instructions_bytes", instructions.size()},
            {"prefix_sha256", sha256_hex(canonical_json(prefix))},
            {"dependencies_sha256", sha256_hex(canonical_json(prefix.at("dependencies")))},
            {"tool_versions_sha256", sha256_hex(canonical_json(Object{{"tool_versions", tool_versions}}))}};
    }
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
    response_metadata::Collector metadata(profile);
    try {
        const auto source = parse_json_strict(raw_body, profile.limits.max_response_bytes, profile.limits.max_json_depth);
        (void)object(source);
        if (profile.pricing && profile.billing_challenge.empty())
            if (const auto* reported = object(source).if_contains("usage")) decoded.response.usage = usage(*reported, metadata.enabled);
        if (http_status != 200) {
            if (const auto* error = object(source).if_contains("error")) evidence["provider_error"] = *error;
            failure(decoded.response, ModelResultStatus::Error, "http_" + std::to_string(http_status),
                    "offline HTTP response is not successful; automatic retry is disabled");
        } else {
            metadata.response(source, "");
            only_fields(source, response_metadata::response_fields, metadata.enabled);
            require(text(source, "object") == "response", ErrorCode::InvalidInput, "HTTP object is not a Responses response");
            const auto provider_id = text(source, "id");
            require(token(provider_id, profile.max_response_id_bytes.value_or(128)), ErrorCode::InvalidInput,
                    "invalid provider response ID");
            decoded.provider_response_id = provider_id;
            require(model_accepted(profile, text(source, "model")), ErrorCode::Stale,
                    "provider response model differs from the explicit profile alias");
            if (const auto* reported = object(source).if_contains("usage")) decoded.response.usage = usage(*reported, metadata.enabled);
            // Program metadata is opaque under the explicit ignore policy;
            // retaining it never grants program access. Legacy profiles still
            // accept only the documented null value.
            for (const auto* name : {"access_programs", "reasoning_effort"})
                if (const auto* optional = object(source).if_contains(name)) {
                    if (metadata.enabled && std::string_view(name) == "access_programs" && !optional->is_null()) {
                        metadata.record("", name);
                        continue;
                    }
                    require(optional->is_null(), ErrorCode::Unsupported,
                            "non-null response metadata is outside the implemented protocol subset");
                }
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
                const auto& details = evidence.at("incomplete_details");
                require(details.is_null() || details.is_object(), ErrorCode::InvalidInput, "invalid incomplete details");
                std::string reason;
                if (details.is_object()) {
                    metadata.inspect(object(details), {"reason"}, "/incomplete_details");
                    only_fields(details, {"reason"}, metadata.enabled);
                    if (object(details).contains("reason")) reason = text(details, "reason");
                    require(!object(details).contains("reason") || reason == "max_output_tokens" || reason == "max_messages" ||
                        reason == "content_filter" || reason == "steered", ErrorCode::Unsupported, "unsupported incomplete reason");
                }
                const bool output_limit = status == "incomplete" && reason == "max_output_tokens" &&
                    evidence.at("provider_error").is_null();
                failure(decoded.response, ModelResultStatus::Interrupted, output_limit ? "response_max_output_tokens" : "response_" + status,
                        "provider response is incomplete or cancelled; partial output cannot dispatch tools");
            } else if (status == "queued" || status == "in_progress") {
                failure(decoded.response, ModelResultStatus::Interrupted, "response_not_terminal",
                        "offline response has no terminal result");
            } else {
                require(status == "completed", ErrorCode::Unsupported, "unsupported provider response status");
                require(evidence.at("provider_error").is_null() && evidence.at("incomplete_details").is_null(),
                        ErrorCode::InvalidInput, "completed response carries an error or incomplete details");
                const auto metadata_before_output = metadata.observed();
                parse_completed_output(profile, request, field(source, "output"), decoded.response, metadata);
                if (decoded.response.status == ModelResultStatus::Completed) {
                    decoded.response.error_code.clear(); decoded.response.error_message.clear();
                }
                if (profile.responses_compatibility && (decoded.response.status == ModelResultStatus::Completed ||
                        decoded.response.error_code == "model_refusal")) {
                    decoded.response.responses_output = payload_for(text(source, "model"), field(source, "output"));
                    if (metadata.observed() != metadata_before_output)
                        decoded.response.responses_output.as_object()["response_metadata_policy"] = profile.response_metadata_policy;
                    // Reject before interpretation: a payload this large could
                    // never persist, so it must not become a sendable original.
                    require(canonical_json(decoded.response.responses_output).size() <=
                            profile.limits.max_response_bytes, ErrorCode::Quota,
                            "responses output copy exceeds the declared response bound");
                }
                validate_response_envelope(request, decoded.response);
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
    metadata.write(evidence);
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
