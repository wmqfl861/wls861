#pragma once

#include "wls/model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
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

// Host-declared per-million-token prices in integer micro USD. This is a
// frozen pricing contract for deterministic host-side estimation, never a
// provider quote, signed receipt or authorization to spend.
struct TokenPricing {
    std::string format{"wls-token-pricing-v1"};
    std::string currency{"USD"};
    std::string unit{"micro_usd_per_million_tokens"};
    std::int64_t input_micro_usd_per_million_tokens{};
    std::int64_t output_micro_usd_per_million_tokens{};
    // "reported" dimensions require their rate and an original token count;
    // "not_applicable" dimensions must omit the rate and any provider count.
    bool cache_read_applicable{false};
    std::int64_t cache_read_micro_usd_per_million_tokens{};
    bool cache_write_applicable{false};
    std::int64_t cache_write_micro_usd_per_million_tokens{};
    bool operator==(const TokenPricing&) const = default;
};

enum class UsageDimensionState { Known, Unknown, NotApplicable };
enum class CostBasis { Unknown, Estimated, Reported };

// Deterministic integer result of one price table against original usage.
// unavailable carries a stable reason; policy_mismatch marks a frozen
// not-applicable dimension that the original response nevertheless reported.
struct PricingEstimate {
    bool available{false};
    bool policy_mismatch{false};
    std::int64_t cost_micro_usd{};
    std::string reason;
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
    // Appended optional extension: old aggregate field order is preserved.
    // Empty preserves the original v1 profile and wire encoding byte-for-byte.
    std::string billing_challenge{};
    // Optional frozen price table. When absent every original byte, hash and
    // UNKNOWN cost decision keeps its exact legacy meaning.
    std::optional<TokenPricing> pricing{};
};

// Strict parse/serialize; rates are integers in 0..10^12 micro USD.
TokenPricing token_pricing_from_json(const Json& value);
Json to_json(const TokenPricing& pricing);
std::string token_pricing_sha256(const TokenPricing& pricing);
// Checked integer estimate: one final ceil after summing exact terms. Unknown
// reported dimensions stay unknown (never zero); overflow returns unavailable.
PricingEstimate estimate_token_cost(const TokenPricing& pricing, const ModelUsage& usage);
PricingEstimate minimum_cost_reservation(const TokenPricing& pricing,
    std::int64_t reserved_input_tokens, std::int64_t reserved_output_tokens);

// All profile fields, including limits, must be explicitly supplied. Unknown
// fields and versions, real_calls_enabled:true and nonzero paid retries refuse.
ProviderProfile provider_profile_from_json(const Json& value);
void validate_provider_profile(const ProviderProfile& profile);
Json to_json(const ProviderProfile& profile);
std::string provider_profile_sha256(const ProviderProfile& profile);

} // namespace wls::s1
