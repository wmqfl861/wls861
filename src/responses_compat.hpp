#pragma once

// Shared pure validation for the opt-in wls-responses-compat-v1 output subset:
// HTTP decode, SSE terminal decode, restore and next-round re-encoding all use
// this one validator so no weak duplicate can accept what another rejects.

#include "wls/model.hpp"
#include "wls/provider_profile.hpp"
#include "wls/structured_context.hpp"

#include <algorithm>
#include <set>
#include <string_view>

namespace wls::s1::responses_compat {

inline constexpr std::string_view output_format = "wls-responses-output-v1";
inline constexpr std::string_view reference = "openai-responses-2026-11-08";

struct ValidationBounds {
    std::size_t max_output_items{512};
    std::size_t max_tool_calls{32};
    std::size_t max_text_bytes{4U * 1024U * 1024U};
    std::size_t max_tool_argument_bytes{4U * 1024U * 1024U};
};

struct Projection {
    std::string text;                       // concatenated output_text parts
    std::vector<ModelToolCall> tool_calls;  // provider call IDs, original order
    bool refusal{false};
    bool reasoning_replay_ready{true};      // every reasoning item has ciphertext
};

inline bool token(const std::string& value, std::size_t maximum = 128) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            const auto c = static_cast<unsigned char>(ch);
            return c >= 33 && c <= 126;
        });
}

inline void only(const boost::json::object& source, std::string_view kind,
                 std::initializer_list<std::string_view> names, ErrorCode code) {
    for (const auto& entry : source) {
        const bool known = std::any_of(names.begin(), names.end(),
            [&](std::string_view name) { return entry.key() == name; });
        if (!known) throw Error(code, std::string(kind) + " field is outside the compatibility subset");
    }
}
inline const Json* member(const boost::json::object& source, std::string_view key) {
    return source.if_contains(key);
}
inline std::string text_of(const Json& value) { return std::string(value.as_string()); }

// Validates one complete ordered output array; tool_allowlist may be null for
// restore-side shape checks. Throws Error.
inline Projection validate_output(const Json& output, const ValidationBounds& bounds,
                                  const std::vector<std::string>* tool_allowlist,
                                  bool parallel_tool_calls) {
    Projection result;
    if (!output.is_array()) throw Error(ErrorCode::InvalidInput, "compatibility output must be an array");
    const auto& items = output.as_array();
    if (items.size() > bounds.max_output_items)
        throw Error(ErrorCode::Quota, "compatibility output item count exceeds its bound");
    std::set<std::string> item_ids, call_ids;
    std::size_t text_bytes = 0, argument_bytes = 0;
    const auto charge = [&](std::size_t size, const char* what) {
        if (size > bounds.max_text_bytes - text_bytes)
            throw Error(ErrorCode::Quota, std::string(what) + " exceeds the aggregate text bound");
        text_bytes += size;
    };
    for (const auto& item : items) {
        if (!item.is_object()) throw Error(ErrorCode::InvalidInput, "compatibility output item must be an object");
        const auto& source = item.as_object();
        const auto type = source.contains("type") && source.at("type").is_string() ?
            std::string(source.at("type").as_string()) : std::string{};
        const auto* id = member(source, "id");
        if (!id || !id->is_string() || !token(text_of(*id)) || !item_ids.insert(text_of(*id)).second)
            throw Error(ErrorCode::InvalidInput, "compatibility output IDs must be unique bounded tokens");
        if (type == "reasoning") {
            only(source, "reasoning", {"id", "type", "summary", "encrypted_content", "status", "content"},
                 ErrorCode::Unsupported);
            const auto* summary = member(source, "summary");
            if (!summary || !summary->is_array())
                throw Error(ErrorCode::InvalidInput, "reasoning item requires its summary array");
            if (summary->as_array().size() > bounds.max_output_items)
                throw Error(ErrorCode::Quota, "reasoning summary part count exceeds its bound");
            for (const auto& entry : summary->as_array()) {
                if (!entry.is_object()) throw Error(ErrorCode::InvalidInput, "summary part must be an object");
                const auto& fields = entry.as_object();
                only(fields, "summary", {"type", "text"}, ErrorCode::Unsupported);
                const auto* part_type = member(fields, "type");
                const auto* part_text = member(fields, "text");
                if (!part_type || !part_type->is_string() || std::string_view(part_type->as_string()) != "summary_text")
                    throw Error(ErrorCode::Unsupported, "summary part type is outside the compatibility subset");
                if (!part_text || !part_text->is_string())
                    throw Error(ErrorCode::InvalidInput, "summary part must be summary_text with text");
                charge(part_text->as_string().size(), "summary text");
            }
            const auto* encrypted = member(source, "encrypted_content");
            if (encrypted && !encrypted->is_null() && !encrypted->is_string())
                throw Error(ErrorCode::InvalidInput, "encrypted reasoning content must be a string or null");
            if (!encrypted || encrypted->is_null() || encrypted->as_string().empty())
                result.reasoning_replay_ready = false;
            if (encrypted && encrypted->is_string() && encrypted->as_string().size() > bounds.max_text_bytes)
                throw Error(ErrorCode::Quota, "encrypted reasoning content exceeds its byte bound");
            if (const auto* status = member(source, "status")) {
                if (!status->is_string() || status->as_string() != "completed")
                    throw Error(ErrorCode::InvalidInput, "terminal reasoning item must be completed");
            }
            if (const auto* content = member(source, "content");
                content && (!content->is_array() || !content->as_array().empty()))
                throw Error(ErrorCode::Unsupported, "reasoning text content is outside the compatibility subset");
        } else if (type == "message") {
            only(source, "message", {"id", "type", "role", "status", "content"}, ErrorCode::Unsupported);
            const auto* role = member(source, "role");
            const auto* status = member(source, "status");
            const auto* content = member(source, "content");
            if (!role || !role->is_string() || std::string_view(role->as_string()) != "assistant" ||
                !status || !status->is_string() || std::string_view(status->as_string()) != "completed" ||
                !content || !content->is_array())
                throw Error(ErrorCode::InvalidInput, "compatibility message needs assistant/completed content");
            if (content->as_array().size() > bounds.max_output_items)
                throw Error(ErrorCode::Quota, "message content part count exceeds its bound");
            for (const auto& entry : content->as_array()) {
                if (!entry.is_object()) throw Error(ErrorCode::InvalidInput, "message part must be an object");
                const auto& fields = entry.as_object();
                const auto part_type = member(fields, "type");
                const bool is_refusal = part_type && *part_type == Json("refusal");
                only(fields, is_refusal ? "refusal" : "message",
                     is_refusal ? std::initializer_list<std::string_view>{"type", "refusal"}
                                : std::initializer_list<std::string_view>{"type", "text", "annotations", "logprobs"},
                     ErrorCode::Unsupported);
                if (is_refusal) {
                    const auto* refusal = member(fields, "refusal");
                    if (!refusal || !refusal->is_string())
                        throw Error(ErrorCode::InvalidInput, "refusal part requires its refusal text");
                    charge(refusal->as_string().size(), "refusal text");
                    result.refusal = true;
                    continue;
                }
                const auto* part_text = member(fields, "text");
                if (!part_type || !part_type->is_string() ||
                    std::string_view(part_type->as_string()) != "output_text")
                    throw Error(ErrorCode::Unsupported, "message part type is outside the compatibility subset");
                if (!part_text || !part_text->is_string())
                    throw Error(ErrorCode::InvalidInput, "message part must be output_text with text");
                for (const auto* optional : {"annotations", "logprobs"})
                    if (const auto* found = fields.if_contains(optional);
                        found && (!found->is_array() || !found->as_array().empty()))
                        throw Error(ErrorCode::Unsupported,
                                    "message annotations or logprobs are outside the compatibility subset");
                charge(part_text->as_string().size(), "message text");
                result.text += text_of(*part_text);
            }
        } else if (type == "function_call") {
            only(source, "function", {"id", "type", "call_id", "name", "arguments", "status"}, ErrorCode::Unsupported);
            if (const auto* status = member(source, "status");
                status && (!status->is_string() || status->as_string() != "completed"))
                throw Error(ErrorCode::InvalidInput, "terminal function call must be completed");
            const auto* call_id = member(source, "call_id");
            const auto* name = member(source, "name");
            const auto* arguments = member(source, "arguments");
            if (!call_id || !call_id->is_string() || !token(text_of(*call_id)) ||
                !call_ids.insert(text_of(*call_id)).second)
                throw Error(ErrorCode::InvalidInput, "compatibility tool IDs must be unique bounded tokens");
            if (!name || !name->is_string() || !token(text_of(*name), 64))
                throw Error(ErrorCode::InvalidInput, "compatibility tool name must be a bounded token");
            if (tool_allowlist && std::find(tool_allowlist->begin(), tool_allowlist->end(), text_of(*name)) ==
                    tool_allowlist->end())
                throw Error(ErrorCode::Unauthorized, "compatibility output requested an unavailable tool");
            if (result.tool_calls.size() >= bounds.max_tool_calls ||
                (!parallel_tool_calls && !result.tool_calls.empty()))
                throw Error(ErrorCode::Quota, "compatibility tool call count exceeds its capability bound");
            if (!arguments || !arguments->is_string())
                throw Error(ErrorCode::InvalidInput, "compatibility tool arguments must be text");
            const auto size = arguments->as_string().size();
            if (size > bounds.max_tool_argument_bytes ||
                argument_bytes > bounds.max_tool_argument_bytes - size)
                throw Error(ErrorCode::Quota, "compatibility tool arguments exceed their aggregate bound");
            argument_bytes += size;
            auto parsed = parse_json_strict(arguments->as_string(), bounds.max_tool_argument_bytes, 64);
            if (!parsed.is_object())
                throw Error(ErrorCode::InvalidInput, "compatibility tool arguments must be a complete object");
            result.tool_calls.push_back({text_of(*call_id), text_of(*name), std::move(parsed)});
        } else {
            throw Error(ErrorCode::Unsupported, "compatibility output type is outside the implemented subset");
        }
    }
    return result;
}

// Strict shape validation of a stored payload object (restore/encode side).
inline Projection validate_payload(const Json& payload) {
    if (!payload.is_object()) throw Error(ErrorCode::InvalidInput, "responses output must be an object");
    const auto& source = payload.as_object();
    only(source, "responses output payload", {"format", "reference", "model", "output"}, ErrorCode::InvalidInput);
    const bool envelope = source.size() == 4 && source.contains("format") && source.at("format").is_string() &&
        std::string_view(source.at("format").as_string()) == output_format &&
        source.contains("reference") && source.at("reference").is_string() &&
        std::string_view(source.at("reference").as_string()) == reference &&
        source.contains("model") && source.at("model").is_string() && token(text_of(source.at("model")));
    if (!envelope)
        throw Error(ErrorCode::InvalidInput, "unsupported responses output payload envelope");
    return validate_output(source.at("output"), ValidationBounds{}, nullptr, true);
}

// The public projection of a payload-carrying assistant message must equal the
// opaque original (text plus name/argument sequence; logical IDs differ).
template <typename Calls>
inline void require_projection_match(const Projection& projection, const std::string& text,
                                     const Calls& calls) {
    if (projection.text != text || projection.tool_calls.size() != calls.size())
        throw Error(ErrorCode::InvalidInput, "responses output projection differs from its message");
    std::size_t index = 0;
    for (const auto& call : calls) {
        if (projection.tool_calls[index].name != call.name ||
            projection.tool_calls[index].arguments != call.arguments)
            throw Error(ErrorCode::InvalidInput, "responses output projection differs from its message");
        ++index;
    }
}

} // namespace wls::s1::responses_compat
