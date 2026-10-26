#include "wls/provider_profile.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <string_view>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

void require(bool valid, const char* message) {
    if (!valid) throw Error(ErrorCode::InvalidInput, message);
}
const Object& object(const Json& value) {
    require(value.is_object(), "provider profile field must be an object");
    return value.as_object();
}
const Json& field(const Json& value, std::string_view key) {
    const auto* item = object(value).if_contains(key);
    require(item != nullptr, "missing provider profile field");
    return *item;
}
std::string text(const Json& value, std::string_view key) {
    const auto& item = field(value, key);
    require(item.is_string(), "provider profile field must be text");
    return std::string(item.as_string());
}
bool boolean(const Json& value, std::string_view key) {
    const auto& item = field(value, key);
    require(item.is_bool(), "provider profile capability must be boolean");
    return item.as_bool();
}
std::int64_t integer(const Json& value, std::string_view key) {
    const auto& item = field(value, key);
    if (item.is_int64()) return item.as_int64();
    if (item.is_uint64() && item.as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(item.as_uint64());
    throw Error(ErrorCode::InvalidInput, "provider profile limit must be an integer");
}
std::size_t size(const Json& value, std::string_view key) {
    const auto result = integer(value, key);
    require(result > 0, "provider profile limit must be positive");
    return static_cast<std::size_t>(result);
}
void exact(const Json& value, const std::set<std::string>& keys) {
    require(object(value).size() == keys.size(), "missing or extra provider profile fields");
    for (const auto& entry : object(value))
        require(keys.contains(std::string(entry.key())), "unknown provider profile field");
}
bool ascii_token(const std::string& value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            const auto c = static_cast<unsigned char>(ch);
            return c >= 33 && c <= 126;
        });
}
bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
        });
}
bool environment_name(const std::string& value) {
    const auto letter = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    return !value.empty() && value.size() <= 128 && letter(value.front()) &&
        std::all_of(value.begin(), value.end(), [&](char ch) {
            return letter(ch) || (ch >= '0' && ch <= '9');
        });
}
bool path(const std::string_view value) {
    if (value.empty() || value.front() != '/' || value.size() > 1024 ||
        value.find("//") != std::string_view::npos) return false;
    std::size_t start = 1;
    for (std::size_t i = 1; i <= value.size(); ++i) {
        if (i == value.size() || value[i] == '/') {
            const auto segment = value.substr(start, i - start);
            if (segment == "." || segment == "..") return false;
            start = i + 1;
        } else {
            const auto c = value[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '~'))
                return false;
        }
    }
    return true;
}
bool base_url(const std::string& value) {
    if (value.size() > 2048 || !value.starts_with("https://")) return false;
    const auto rest = std::string_view(value).substr(8);
    const auto slash = rest.find('/');
    auto authority = rest.substr(0, slash);
    if (authority.empty() || authority.size() > 259) return false;
    const auto colon = authority.find(':');
    if (colon != std::string_view::npos) {
        const auto port = authority.substr(colon + 1);
        unsigned parsed = 0;
        const auto converted = std::from_chars(port.data(), port.data() + port.size(), parsed);
        if (port.empty() || converted.ec != std::errc{} ||
            converted.ptr != port.data() + port.size() || parsed == 0 || parsed > 65535) return false;
        authority = authority.substr(0, colon);
    }
    if (authority.empty() || authority.size() > 253 || authority.front() == '.' ||
        authority.back() == '.' || authority.find("..") != std::string_view::npos) return false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= authority.size(); ++i) {
        if (i == authority.size() || authority[i] == '.') {
            const auto label = authority.substr(start, i - start);
            if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
            start = i + 1;
        } else {
            const auto c = authority[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-')) return false;
        }
    }
    return slash == std::string_view::npos ||
        (value.back() != '/' && path(rest.substr(slash)));
}

Json limits_json(const ProtocolLimits& x) {
    return Object{{"max_request_bytes", x.max_request_bytes},
        {"max_response_bytes", x.max_response_bytes}, {"max_text_bytes", x.max_text_bytes},
        {"max_tool_argument_bytes", x.max_tool_argument_bytes}, {"max_output_items", x.max_output_items},
        {"max_tool_calls", x.max_tool_calls}, {"max_json_depth", x.max_json_depth},
        {"max_stream_bytes", x.max_stream_bytes}, {"max_sse_line_bytes", x.max_sse_line_bytes},
        {"max_sse_event_bytes", x.max_sse_event_bytes}, {"max_sse_events", x.max_sse_events},
        {"max_sse_chunks", x.max_sse_chunks}, {"max_timeout_ms", x.max_timeout_ms}};
}
ProtocolLimits parse_limits(const Json& value) {
    exact(value, {"max_request_bytes", "max_response_bytes", "max_text_bytes", "max_tool_argument_bytes",
        "max_output_items", "max_tool_calls", "max_json_depth", "max_stream_bytes", "max_sse_line_bytes",
        "max_sse_event_bytes", "max_sse_events", "max_sse_chunks", "max_timeout_ms"});
    ProtocolLimits x;
    x.max_request_bytes = size(value, "max_request_bytes");
    x.max_response_bytes = size(value, "max_response_bytes");
    x.max_text_bytes = size(value, "max_text_bytes");
    x.max_tool_argument_bytes = size(value, "max_tool_argument_bytes");
    x.max_output_items = size(value, "max_output_items");
    x.max_tool_calls = size(value, "max_tool_calls");
    x.max_json_depth = size(value, "max_json_depth");
    x.max_stream_bytes = size(value, "max_stream_bytes");
    x.max_sse_line_bytes = size(value, "max_sse_line_bytes");
    x.max_sse_event_bytes = size(value, "max_sse_event_bytes");
    x.max_sse_events = size(value, "max_sse_events");
    x.max_sse_chunks = size(value, "max_sse_chunks");
    x.max_timeout_ms = integer(value, "max_timeout_ms");
    return x;
}
} // namespace

void validate_provider_profile(const ProviderProfile& p) {
    require(identifier(p.profile_id) && identifier(p.model_ref), "invalid provider profile or model reference");
    if (p.adapter_version != "1" || p.protocol != "openai-responses-v1")
        throw Error(ErrorCode::Unsupported, "unsupported Responses codec version or protocol");
    require(base_url(p.base_url) && path(p.responses_path) && p.responses_path != "/" &&
            p.responses_path.back() != '/', "provider endpoint must be an explicit HTTPS origin/path without credentials, query or fragment");
    require(ascii_token(p.model, 128), "provider model alias must be a bounded ASCII token");
    require(environment_name(p.api_key_env), "authentication must be a valid environment reference name");
    require(!p.parallel_tool_calls || p.tool_calling, "parallel tool calls require tool calling capability");
    require(p.reasoning_provider_default || p.reasoning_off || !p.reasoning_efforts.empty(),
            "provider must explicitly declare a reasoning mode");
    const std::set<std::string> supported_efforts{"minimal", "low", "medium", "high", "xhigh", "max"};
    for (const auto& effort : p.reasoning_efforts)
        if (!supported_efforts.contains(effort)) throw Error(ErrorCode::Unsupported, "unsupported Responses reasoning effort");
    require(p.context_window > 0 && p.context_window <= 100000000 &&
            p.max_output_tokens > 0 && p.max_output_tokens <= p.context_window,
            "invalid provider token capability bounds");
    const auto& x = p.limits;
    for (const auto bound : {x.max_request_bytes, x.max_response_bytes, x.max_text_bytes,
                            x.max_tool_argument_bytes, x.max_sse_event_bytes})
        require(bound > 0 && bound <= 4U * 1024U * 1024U, "protocol byte limit outside hard bounds");
    require(x.max_output_items > 0 && x.max_output_items <= 512 && x.max_tool_calls > 0 && x.max_tool_calls <= 32 &&
            x.max_json_depth > 0 && x.max_json_depth <= 64, "protocol item or depth limit outside hard bounds");
    require(x.max_stream_bytes > 0 && x.max_stream_bytes <= 16U * 1024U * 1024U &&
            x.max_sse_line_bytes > 0 && x.max_sse_line_bytes <= 1024U * 1024U &&
            x.max_sse_events > 0 && x.max_sse_events <= 16384 &&
            x.max_sse_chunks > 0 && x.max_sse_chunks <= 65536 &&
            x.max_timeout_ms > 0 && x.max_timeout_ms <= 300000,
            "protocol stream or deadline limit outside hard bounds");
}

ProviderProfile provider_profile_from_json(const Json& value) {
    require(canonical_json(value).size() <= 65536, "provider profile byte limit exceeded");
    exact(value, {"format", "profile_id", "adapter", "adapter_version", "protocol", "base_url",
        "responses_path", "model_ref", "model", "api_key_env", "real_calls_enabled", "max_paid_retries",
        "capabilities", "limits"});
    if (text(value, "format") != "wls-provider-profile-v1" || text(value, "adapter") != "openai-responses")
        throw Error(ErrorCode::Unsupported, "unsupported offline provider profile format or adapter");
    if (boolean(value, "real_calls_enabled") || integer(value, "max_paid_retries") != 0)
        throw Error(ErrorCode::Unsupported, "offline codec cannot enable real calls or paid retries");
    ProviderProfile p;
    p.profile_id = text(value, "profile_id"); p.adapter_version = text(value, "adapter_version");
    p.protocol = text(value, "protocol"); p.base_url = text(value, "base_url");
    p.responses_path = text(value, "responses_path"); p.model_ref = text(value, "model_ref");
    p.model = text(value, "model"); p.api_key_env = text(value, "api_key_env");
    const auto& caps = field(value, "capabilities");
    exact(caps, {"streaming", "tool_calling", "parallel_tool_calls", "reasoning_modes", "reasoning_efforts",
                 "context_window", "max_output_tokens"});
    p.streaming = boolean(caps, "streaming"); p.tool_calling = boolean(caps, "tool_calling");
    p.parallel_tool_calls = boolean(caps, "parallel_tool_calls");
    const auto& modes = field(caps, "reasoning_modes");
    const auto& efforts = field(caps, "reasoning_efforts");
    require(modes.is_array() && modes.as_array().size() <= 3 && efforts.is_array() &&
            efforts.as_array().size() <= 6, "invalid provider reasoning capability arrays");
    std::set<std::string> seen;
    for (const auto& mode : modes.as_array()) {
        require(mode.is_string(), "provider reasoning mode must be text");
        const auto name = std::string(mode.as_string());
        require(seen.insert(name).second, "duplicate provider reasoning mode");
        if (name == "provider_default") p.reasoning_provider_default = true;
        else if (name == "off") p.reasoning_off = true;
        else if (name != "effort") throw Error(ErrorCode::Unsupported, "unsupported provider reasoning mode");
    }
    for (const auto& effort : efforts.as_array()) {
        require(effort.is_string(), "provider reasoning effort must be text");
        require(p.reasoning_efforts.insert(std::string(effort.as_string())).second, "duplicate provider reasoning effort");
    }
    require(seen.contains("effort") == !p.reasoning_efforts.empty(), "effort mode requires its explicit nonempty capability list");
    p.context_window = integer(caps, "context_window");
    p.max_output_tokens = integer(caps, "max_output_tokens");
    p.limits = parse_limits(field(value, "limits"));
    validate_provider_profile(p);
    return p;
}

Json to_json(const ProviderProfile& p) {
    validate_provider_profile(p);
    Array modes, efforts;
    if (p.reasoning_provider_default) modes.emplace_back("provider_default");
    if (p.reasoning_off) modes.emplace_back("off");
    if (!p.reasoning_efforts.empty()) modes.emplace_back("effort");
    for (const auto& effort : p.reasoning_efforts) efforts.emplace_back(effort);
    return Object{{"format", "wls-provider-profile-v1"}, {"profile_id", p.profile_id},
        {"adapter", "openai-responses"}, {"adapter_version", p.adapter_version}, {"protocol", p.protocol},
        {"base_url", p.base_url}, {"responses_path", p.responses_path}, {"model_ref", p.model_ref},
        {"model", p.model}, {"api_key_env", p.api_key_env}, {"real_calls_enabled", false}, {"max_paid_retries", 0},
        {"capabilities", Object{{"streaming", p.streaming}, {"tool_calling", p.tool_calling},
            {"parallel_tool_calls", p.parallel_tool_calls}, {"reasoning_modes", modes}, {"reasoning_efforts", efforts},
            {"context_window", p.context_window}, {"max_output_tokens", p.max_output_tokens}}},
        {"limits", limits_json(p.limits)}};
}

std::string provider_profile_sha256(const ProviderProfile& profile) {
    return sha256_hex(canonical_json(to_json(profile)));
}
} // namespace wls::s1
