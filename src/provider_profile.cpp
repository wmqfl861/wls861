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

// Checked nonnegative int64 arithmetic: no extended integers, no UB, no float.
constexpr std::int64_t pricing_rate_bound = 1000000000000LL;
bool pricing_add(std::int64_t a, std::int64_t b, std::int64_t* out) {
    if (a < 0 || b < 0 || a > INT64_MAX - b) return false;
    *out = a + b;
    return true;
}
bool pricing_mul(std::int64_t a, std::int64_t b, std::int64_t* out) {
    if (a < 0 || b < 0 || (a != 0 && b > INT64_MAX / a)) return false;
    *out = a * b;
    return true;
}
// Splits tokens*rate into whole micro USD plus a sub-million remainder part.
bool pricing_term(std::int64_t tokens, std::int64_t rate, std::int64_t* whole, std::int64_t* remainder) {
    std::int64_t head = 0, tail = 0, high = 0;
    if (!pricing_mul(tokens / 1000000, rate, &head) || !pricing_mul(tokens % 1000000, rate, &tail) ||
        !pricing_add(head, tail / 1000000, &high) || !pricing_add(*whole, high, whole) ||
        !pricing_add(*remainder, tail % 1000000, remainder)) return false;
    return true;
}
bool pricing_total(std::int64_t whole, std::int64_t remainder, std::int64_t* out) {
    return pricing_add(whole, (remainder + 999999) / 1000000, out);  // single final ceil
}
std::int64_t pricing_rate(const Json& value, std::string_view key) {
    const auto rate = integer(value, key);
    require(rate >= 0 && rate <= pricing_rate_bound, "token pricing rate outside hard bounds");
    return rate;
}
bool pricing_cache(const Json& value, std::string_view key, std::int64_t* rate) {
    const auto& item = field(value, key);
    require(item.is_object() && !item.as_object().empty(), "token pricing cache dimension must be an object");
    const auto applicability = text(item, "applicability");
    if (applicability == "not_applicable") {
        require(item.as_object().size() == 1, "not-applicable token pricing must omit its rate");
        return false;
    }
    require(applicability == "reported" && item.as_object().size() == 2,
            "token pricing applicability must be reported with its exact rate");
    *rate = pricing_rate(item, "micro_usd_per_million_tokens");
    return true;
}
} // namespace

TokenPricing token_pricing_from_json(const Json& value) {
    exact(value, {"format", "currency", "unit", "input_micro_usd_per_million_tokens",
        "output_micro_usd_per_million_tokens", "cache_read", "cache_write"});
    if (text(value, "format") != "wls-token-pricing-v1" || text(value, "currency") != "USD" ||
        text(value, "unit") != "micro_usd_per_million_tokens")
        throw Error(ErrorCode::Unsupported, "unsupported token pricing format, currency or unit");
    TokenPricing pricing;
    pricing.input_micro_usd_per_million_tokens = pricing_rate(value, "input_micro_usd_per_million_tokens");
    pricing.output_micro_usd_per_million_tokens = pricing_rate(value, "output_micro_usd_per_million_tokens");
    pricing.cache_read_applicable = pricing_cache(value, "cache_read", &pricing.cache_read_micro_usd_per_million_tokens);
    pricing.cache_write_applicable = pricing_cache(value, "cache_write", &pricing.cache_write_micro_usd_per_million_tokens);
    return pricing;
}

Json to_json(const TokenPricing& pricing) {
    const auto cache = [](bool applicable, std::int64_t rate) {
        return applicable ? Json(Object{{"applicability", "reported"},
            {"micro_usd_per_million_tokens", rate}}) : Json(Object{{"applicability", "not_applicable"}});
    };
    return Object{{"format", pricing.format}, {"currency", pricing.currency}, {"unit", pricing.unit},
        {"input_micro_usd_per_million_tokens", pricing.input_micro_usd_per_million_tokens},
        {"output_micro_usd_per_million_tokens", pricing.output_micro_usd_per_million_tokens},
        {"cache_read", cache(pricing.cache_read_applicable, pricing.cache_read_micro_usd_per_million_tokens)},
        {"cache_write", cache(pricing.cache_write_applicable, pricing.cache_write_micro_usd_per_million_tokens)}};
}

std::string token_pricing_sha256(const TokenPricing& pricing) {
    return sha256_hex(canonical_json(to_json(pricing)));
}

PricingEstimate estimate_token_cost(const TokenPricing& pricing, const ModelUsage& usage, bool conservative_cache) {
    PricingEstimate result;
    const auto unavailable = [&result](std::string_view reason) {
        result.reason = std::string(reason);
        return result;
    };
    if ((!pricing.cache_read_applicable && usage.cache_read_tokens.has_value()) ||
        (!pricing.cache_write_applicable && usage.cache_write_tokens.has_value())) {
        result.policy_mismatch = true;
        if (!conservative_cache) return unavailable("policy_mismatch_not_applicable_cache_reported");
    }
    if (!usage.input_tokens.has_value() || !usage.output_tokens.has_value())
        return unavailable("unknown_input_or_output_tokens");
    const auto valid = [](const std::optional<std::int64_t>& value) {
        return value.has_value() && *value >= 0;
    };
    std::int64_t cache_read = 0, cache_write = 0;
    if (pricing.cache_read_applicable) {
        if (!valid(usage.cache_read_tokens)) {
            if (!conservative_cache) return unavailable("unknown_cache_read_tokens");
            result.policy_mismatch = true;
        } else cache_read = *usage.cache_read_tokens;
    }
    if (pricing.cache_write_applicable) {
        if (!valid(usage.cache_write_tokens)) {
            if (!conservative_cache) return unavailable("unknown_cache_write_tokens");
            result.policy_mismatch = true;
        } else cache_write = *usage.cache_write_tokens;
    }
    if (!valid(usage.input_tokens) || !valid(usage.output_tokens) ||
        cache_read > *usage.input_tokens || cache_write > *usage.input_tokens - cache_read)
        return unavailable("invalid_original_usage");
    auto write_rate = pricing.cache_write_micro_usd_per_million_tokens;
    if (conservative_cache && pricing.cache_write_applicable && !usage.cache_write_tokens) {
        cache_write = *usage.input_tokens - cache_read;
        write_rate = std::max(write_rate, pricing.input_micro_usd_per_million_tokens);
    }
    std::int64_t whole = 0, remainder = 0;
    if (!pricing_term(*usage.input_tokens - cache_read - cache_write, pricing.input_micro_usd_per_million_tokens,
                &whole, &remainder) ||
        !pricing_term(cache_read, pricing.cache_read_micro_usd_per_million_tokens, &whole, &remainder) ||
        !pricing_term(cache_write, write_rate, &whole, &remainder) ||
        !pricing_term(*usage.output_tokens, pricing.output_micro_usd_per_million_tokens, &whole, &remainder) ||
        !pricing_total(whole, remainder, &result.cost_micro_usd))
        return unavailable("estimate_unavailable_overflow");
    result.available = true;
    return result;
}

PricingEstimate minimum_cost_reservation(const TokenPricing& pricing, std::int64_t reserved_input,
                                         std::int64_t reserved_output) {
    PricingEstimate result;
    if (reserved_input < 0 || reserved_output < 0) {
        result.reason = "invalid_reservation";
        return result;
    }
    auto input_rate = pricing.input_micro_usd_per_million_tokens;
    if (pricing.cache_read_applicable)
        input_rate = std::max(input_rate, pricing.cache_read_micro_usd_per_million_tokens);
    if (pricing.cache_write_applicable)
        input_rate = std::max(input_rate, pricing.cache_write_micro_usd_per_million_tokens);
    std::int64_t whole = 0, remainder = 0;
    if (!pricing_term(reserved_input, input_rate, &whole, &remainder) ||
        !pricing_term(reserved_output, pricing.output_micro_usd_per_million_tokens, &whole, &remainder) ||
        !pricing_total(whole, remainder, &result.cost_micro_usd)) {
        result.reason = "estimate_unavailable_overflow";
        return result;
    }
    result.available = true;
    return result;
}

void validate_provider_profile(const ProviderProfile& p) {
    require(identifier(p.profile_id) && identifier(p.model_ref), "invalid provider profile or model reference");
    const bool chat = p.protocol == "openai-chat-v1";
    if (p.adapter_version != "1" || (!chat && p.protocol != "openai-responses-v1"))
        throw Error(ErrorCode::Unsupported, "unsupported Responses codec version or protocol");
    const auto& endpoint = chat ? p.chat_completions_path : p.responses_path;
    require(base_url(p.base_url) && path(endpoint) && endpoint != "/" &&
            endpoint.back() != '/', "provider endpoint must be an explicit HTTPS origin/path without credentials, query or fragment");
    if (chat) {
        if (p.streaming || !p.responses_path.empty() || p.responses_compatibility || !p.billing_challenge.empty() ||
                p.response_metadata_policy != "ignore-record-v1")
            throw Error(ErrorCode::Unsupported, "Chat requires nonstream metadata policy without Responses extensions");
        if ((!p.chat_reasoning_policy.empty() && p.chat_reasoning_policy != "reasoning-effort-v1") ||
                (p.chat_reasoning_policy.empty() && (p.reasoning_off || !p.reasoning_efforts.empty())))
            throw Error(ErrorCode::Unsupported, "Chat off/effort requires an explicit reasoning-effort-v1 declaration");
        require(p.chat_model_aliases.size() <= 8 && std::is_sorted(p.chat_model_aliases.begin(), p.chat_model_aliases.end()),
                "Chat model aliases must be bounded and normalized");
        std::set<std::string> seen;
        for (const auto& alias : p.chat_model_aliases)
            require(ascii_token(alias, 128) && seen.insert(alias).second, "Chat model aliases must be unique bounded tokens");
    } else if (!p.chat_completions_path.empty() || !p.chat_reasoning_policy.empty() || !p.chat_model_aliases.empty()) {
        throw Error(ErrorCode::Unsupported, "Chat-only declarations cannot alter a Responses profile");
    }
    require(ascii_token(p.model, 128), "provider model alias must be a bounded ASCII token");
    require(environment_name(p.api_key_env), "authentication must be a valid environment reference name");
    require(p.billing_challenge.empty() || p.billing_challenge == "wls-billing-challenge-v1",
            "unsupported frozen billing challenge extension");
    if (p.pricing) {
        require(p.pricing->format == "wls-token-pricing-v1" && p.pricing->currency == "USD" &&
                p.pricing->unit == "micro_usd_per_million_tokens",
                "unsupported token pricing contract");
        for (const auto rate : {p.pricing->input_micro_usd_per_million_tokens,
                p.pricing->output_micro_usd_per_million_tokens,
                p.pricing->cache_read_applicable ? p.pricing->cache_read_micro_usd_per_million_tokens : 0,
                p.pricing->cache_write_applicable ? p.pricing->cache_write_micro_usd_per_million_tokens : 0})
            require(rate >= 0 && rate <= pricing_rate_bound, "token pricing rate outside hard bounds");
    }
    if (p.responses_compatibility) {
        const auto& compat = *p.responses_compatibility;
        require(compat.format == "wls-responses-compat-v1" && compat.reference == "openai-responses-2026-11-08",
                "unsupported Responses compatibility format or reference");
        require(compat.model_aliases.size() >= 1 && compat.model_aliases.size() <= 8,
                "Responses compatibility needs 1..8 explicit model snapshots");
        std::set<std::string> seen;
        for (const auto& alias : compat.model_aliases) {
            require(ascii_token(alias, 128) && seen.insert(alias).second,
                    "Responses compatibility model snapshots must be unique bounded ASCII tokens");
        }
        require(std::is_sorted(compat.model_aliases.begin(), compat.model_aliases.end()),
                "Responses compatibility model snapshots must be stored normalized");
    }
    require(p.response_metadata_policy.empty() || p.response_metadata_policy == "ignore-record-v1",
            "unsupported response metadata policy");
    require(p.tool_choice_policy.empty() || p.tool_choice_policy == "allowed-tools-v1" || p.tool_choice_policy == "required-v1", "unsupported tool choice policy");
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
    const bool chat = text(value, "adapter") == "openai-chat";
    auto keys = std::set<std::string>{"format", "profile_id", "adapter", "adapter_version", "protocol", "base_url",
        chat ? "chat_completions_path" : "responses_path", "model_ref", "model", "api_key_env", "real_calls_enabled", "max_paid_retries",
        "capabilities", "limits"};
    if (chat) for (const auto* key : {"chat_reasoning_policy", "chat_model_aliases"})
        if (object(value).contains(key)) keys.insert(key);
    if (object(value).contains("billing_challenge")) keys.insert("billing_challenge");
    if (object(value).contains("pricing")) keys.insert("pricing");
    if (object(value).contains("responses_compatibility")) keys.insert("responses_compatibility");
    if (object(value).contains("response_metadata_policy")) keys.insert("response_metadata_policy");
    if (object(value).contains("tool_choice_policy")) keys.insert("tool_choice_policy");
    exact(value, keys);
    if (text(value, "format") != "wls-provider-profile-v1" || (!chat && text(value, "adapter") != "openai-responses") ||
            (chat != (text(value, "protocol") == "openai-chat-v1")))
        throw Error(ErrorCode::Unsupported, "unsupported offline provider profile format or adapter");
    if (boolean(value, "real_calls_enabled") || integer(value, "max_paid_retries") != 0)
        throw Error(ErrorCode::Unsupported, "offline codec cannot enable real calls or paid retries");
    ProviderProfile p;
    p.profile_id = text(value, "profile_id"); p.adapter_version = text(value, "adapter_version");
    p.protocol = text(value, "protocol"); p.base_url = text(value, "base_url");
    (chat ? p.chat_completions_path : p.responses_path) = text(value, chat ? "chat_completions_path" : "responses_path");
    p.model_ref = text(value, "model_ref");
    p.model = text(value, "model"); p.api_key_env = text(value, "api_key_env");
    if (chat && object(value).contains("chat_reasoning_policy")) {
        p.chat_reasoning_policy = text(value, "chat_reasoning_policy");
        require(!p.chat_reasoning_policy.empty(), "Chat reasoning policy cannot be empty");
    }
    if (chat && object(value).contains("chat_model_aliases")) {
        const auto& aliases = field(value, "chat_model_aliases");
        require(aliases.is_array() && !aliases.as_array().empty(), "Chat aliases must be a nonempty array");
        for (const auto& alias : aliases.as_array()) {
            require(alias.is_string(), "Chat alias must be text");
            p.chat_model_aliases.emplace_back(alias.as_string());
        }
        std::sort(p.chat_model_aliases.begin(), p.chat_model_aliases.end());
    }
    if (object(value).contains("tool_choice_policy")) {
        p.tool_choice_policy = text(value, "tool_choice_policy");
        require(p.tool_choice_policy == "allowed-tools-v1" || p.tool_choice_policy == "required-v1", "unsupported tool choice policy");
    }
    if (object(value).contains("response_metadata_policy")) {
        p.response_metadata_policy = text(value, "response_metadata_policy");
        require(p.response_metadata_policy == "ignore-record-v1", "unsupported response metadata policy");
    }
    if (object(value).contains("billing_challenge")) {
        p.billing_challenge = text(value, "billing_challenge");
        require(p.billing_challenge == "wls-billing-challenge-v1", "unsupported frozen billing challenge extension");
    }
    if (object(value).contains("pricing")) p.pricing = token_pricing_from_json(field(value, "pricing"));
    if (const auto* compat = object(value).if_contains("responses_compatibility")) {
        if (!compat->is_object())
            throw Error(ErrorCode::Unsupported, "Responses compatibility extension must be an object");
        const auto& source = compat->as_object();
        for (const auto& entry : source)
            if (entry.key() != "format" && entry.key() != "reference" && entry.key() != "model_aliases")
                throw Error(ErrorCode::Unsupported, "unknown Responses compatibility field");
        if (source.size() != 3)
            throw Error(ErrorCode::Unsupported, "Responses compatibility needs its exact three fields");
        if (text(*compat, "format") != "wls-responses-compat-v1" ||
            text(*compat, "reference") != "openai-responses-2026-11-08")
            throw Error(ErrorCode::Unsupported, "unsupported Responses compatibility format or reference");
        const auto& aliases = field(*compat, "model_aliases");
        require(aliases.is_object() && aliases.as_object().size() == 1 &&
                aliases.as_object().begin()->value().is_array(),
                "Responses compatibility declares exactly one model alias array");
        const auto& mapping = *aliases.as_object().begin();
        require(std::string(mapping.key()) == p.model,
                "Responses compatibility alias key must be the exact profile model");
        p.responses_compatibility.emplace();
        for (const auto& alias : mapping.value().as_array()) {
            require(alias.is_string(), "Responses compatibility model snapshot must be text");
            p.responses_compatibility->model_aliases.emplace_back(alias.as_string());
        }
        std::sort(p.responses_compatibility->model_aliases.begin(), p.responses_compatibility->model_aliases.end());
    }
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
    const bool chat = p.protocol == "openai-chat-v1";
    Array modes, efforts;
    if (p.reasoning_provider_default) modes.emplace_back("provider_default");
    if (p.reasoning_off) modes.emplace_back("off");
    if (!p.reasoning_efforts.empty()) modes.emplace_back("effort");
    for (const auto& effort : p.reasoning_efforts) efforts.emplace_back(effort);
    Object result{{"format", "wls-provider-profile-v1"}, {"profile_id", p.profile_id},
        {"adapter", chat ? "openai-chat" : "openai-responses"}, {"adapter_version", p.adapter_version}, {"protocol", p.protocol},
        {"base_url", p.base_url}, {chat ? "chat_completions_path" : "responses_path", chat ? p.chat_completions_path : p.responses_path}, {"model_ref", p.model_ref},
        {"model", p.model}, {"api_key_env", p.api_key_env}, {"real_calls_enabled", false}, {"max_paid_retries", 0},
        {"capabilities", Object{{"streaming", p.streaming}, {"tool_calling", p.tool_calling},
            {"parallel_tool_calls", p.parallel_tool_calls}, {"reasoning_modes", modes}, {"reasoning_efforts", efforts},
            {"context_window", p.context_window}, {"max_output_tokens", p.max_output_tokens}}},
        {"limits", limits_json(p.limits)}};
    if (!p.billing_challenge.empty()) result["billing_challenge"] = p.billing_challenge;
    if (p.pricing) result["pricing"] = to_json(*p.pricing);
    if (!p.response_metadata_policy.empty()) result["response_metadata_policy"] = p.response_metadata_policy;
    if (!p.tool_choice_policy.empty()) result["tool_choice_policy"] = p.tool_choice_policy;
    if (!p.chat_reasoning_policy.empty()) result["chat_reasoning_policy"] = p.chat_reasoning_policy;
    if (!p.chat_model_aliases.empty()) {
        Array aliases; for (const auto& alias : p.chat_model_aliases) aliases.emplace_back(alias);
        result["chat_model_aliases"] = std::move(aliases);
    }
    if (p.responses_compatibility) {
        Array snapshots;
        for (const auto& alias : p.responses_compatibility->model_aliases) snapshots.emplace_back(alias);
        result["responses_compatibility"] = Object{{"format", p.responses_compatibility->format},
            {"reference", p.responses_compatibility->reference},
            {"model_aliases", Object{{p.model, std::move(snapshots)}}}};
    }
    return result;
}

std::string provider_profile_sha256(const ProviderProfile& profile) {
    return sha256_hex(canonical_json(to_json(profile)));
}
} // namespace wls::s1
