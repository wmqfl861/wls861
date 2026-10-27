#include "wls/billing.hpp"

#include "wls/config_loader.hpp"
#include "wls/store.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
constexpr std::size_t receipt_limit = 16384;
constexpr std::string_view signing_domain = "wls-signed-billing-receipt-v1\n";

void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
const Object& object(const Json& value) {
    require(value.is_object(), ErrorCode::InvalidInput, "billing field must be an object");
    return value.as_object();
}
const Json& field(const Json& value, std::string_view name) {
    const auto* found = object(value).if_contains(name);
    require(found != nullptr, ErrorCode::InvalidInput, "missing billing field");
    return *found;
}
std::string text(const Json& value, std::string_view name) {
    const auto& item = field(value, name);
    require(item.is_string(), ErrorCode::InvalidInput, "billing field must be text");
    return std::string(item.as_string());
}
std::int64_t integer(const Json& value, std::string_view name) {
    const auto& item = field(value, name);
    if (item.is_int64()) return item.as_int64();
    if (item.is_uint64() && item.as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(item.as_uint64());
    throw Error(ErrorCode::InvalidInput, "billing field must be a bounded integer");
}
bool boolean(const Json& value, std::string_view name) {
    const auto& item = field(value, name);
    require(item.is_bool(), ErrorCode::Corrupt, "original billing observation flag is invalid");
    return item.as_bool();
}
void exact(const Json& value, const std::set<std::string>& names) {
    require(object(value).size() == names.size(), ErrorCode::InvalidInput, "missing or extra billing fields");
    for (const auto& entry : object(value))
        require(names.contains(std::string(entry.key())), ErrorCode::InvalidInput, "unsupported billing field");
}
bool token(std::string_view value, std::size_t maximum = 128) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        });
}
bool digest(std::string_view value, std::size_t size = 64) {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string unhex(std::string_view bytes, std::size_t maximum) {
    require(bytes.size() % 2 == 0 && bytes.size() / 2 <= maximum && digest(bytes, bytes.size()),
            ErrorCode::InvalidInput, "invalid bounded billing hexadecimal bytes");
    const auto nibble = [](char c) { return static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10); };
    std::string result(bytes.size() / 2, '\0');
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>(nibble(bytes[2 * i]) * 16U + nibble(bytes[2 * i + 1]));
    return result;
}
void validate_payload(const Json& payload) {
    exact(payload, {"trust_domain", "issuer", "account_id", "key_id", "receipt_id", "revision",
        "billing_challenge", "request_body_sha256", "response_body_sha256", "provider_response_id",
        "provider_model", "token_unit", "cost_unit", "usage"});
    require(canonical_json(payload).size() <= receipt_limit - 512, ErrorCode::Quota, "billing payload exceeds its bound");
    const auto domain = text(payload, "trust_domain");
    require(domain == "local_tls_fixture" || domain == "provider_billing", ErrorCode::Unsupported,
            "unsupported billing trust domain");
    for (const auto* name : {"issuer", "account_id", "key_id", "receipt_id"})
        require(token(text(payload, name), 256), ErrorCode::InvalidInput, "invalid billing issuer identity");
    const auto& response_id = field(payload, "provider_response_id");
    require(response_id.is_null() || (response_id.is_string() && token(response_id.as_string(), 512)),
            ErrorCode::InvalidInput, "invalid billing provider response identity");
    const auto model = text(payload, "provider_model");
    require(!model.empty() && model.size() <= 128 && std::all_of(model.begin(), model.end(), [](char c) {
                const auto byte = static_cast<unsigned char>(c); return byte >= 33U && byte <= 126U;
            }), ErrorCode::InvalidInput, "invalid billing provider model identity");
    require(integer(payload, "revision") > 0 && text(payload, "token_unit") == "tokens" &&
            text(payload, "cost_unit") == "micro_usd", ErrorCode::InvalidInput, "invalid billing revision or units");
    for (const auto* name : {"billing_challenge", "request_body_sha256", "response_body_sha256"})
        require(digest(text(payload, name)), ErrorCode::InvalidInput, "invalid billing fingerprint");
    exact(field(payload, "usage"), {"input_tokens", "output_tokens", "cache_read_tokens", "cache_write_tokens", "cost_micro_usd"});
    (void)model_usage_from_json(field(payload, "usage"));
}
void verify_signature(std::string_view key_hex, std::string_view signature_hex, std::string_view message) {
    require(digest(key_hex) && digest(signature_hex, 128), ErrorCode::InvalidInput,
            "billing signature or public key has an invalid encoding");
    const auto key_bytes = unhex(key_hex, 32);
    const auto signature = unhex(signature_hex, 64);
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Key key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(key_bytes.data()), key_bytes.size()), EVP_PKEY_free);
    Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(key && context && EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1,
            ErrorCode::InvalidInput, "billing signature verifier could not initialize");
    require(EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1,
            ErrorCode::Unauthorized, "billing receipt signature was not authenticated");
}
std::optional<std::string> receipt_header(std::string_view headers) {
    require(headers.size() <= 65536, ErrorCode::Quota, "original billing headers exceed their bound");
    if (headers.empty()) return std::nullopt;
    std::optional<std::string> receipt;
    int status = 0;
    bool complete = false;
    bool final = false;
    while (!headers.empty()) {
        const auto end = headers.find("\r\n");
        require(end != std::string_view::npos, ErrorCode::Corrupt, "original billing headers are incomplete");
        auto line = headers.substr(0, end);
        headers.remove_prefix(end + 2);
        require(line.size() <= 8192, ErrorCode::Quota, "billing header line exceeds its bound");
        if (line.starts_with("HTTP/")) {
            require(!final && (status == 0 || complete) &&
                    (line.starts_with("HTTP/1.0 ") || line.starts_with("HTTP/1.1 ")) && line.size() >= 12,
                    ErrorCode::Corrupt, "invalid original billing header block");
            const auto digits = line.substr(9, 3);
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), status);
            require(parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() && status >= 100 &&
                    status <= 599 && status != 101, ErrorCode::Corrupt, "invalid original billing HTTP status");
            complete = false;
            continue;
        }
        require(status != 0, ErrorCode::Corrupt, "billing headers have no original HTTP status");
        if (line.empty()) {
            complete = true;
            final = status >= 200;
            continue;
        }
        const auto colon = line.find(':');
        require(colon != std::string_view::npos && colon != 0 && line.front() != ' ' && line.front() != '\t',
                ErrorCode::Corrupt, "invalid original billing header framing");
        std::string name(line.substr(0, colon));
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (name != "x-wls-usage-receipt") continue;
        require(status >= 200 && !receipt.has_value(), ErrorCode::InvalidInput,
                "billing receipt must appear once in the final response");
        line.remove_prefix(colon + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        require(!line.empty() && line.size() <= receipt_limit, ErrorCode::Quota,
                "billing receipt header is empty or oversized");
        receipt = std::string(line);
    }
    require(final && complete, ErrorCode::Corrupt, "original billing final headers are incomplete");
    return receipt;
}

struct OriginalBillingContext {
    Json original;
    ModelRequest request;
    LiveModelSettings settings;
    EncodedResponsesRequest encoded;
    std::string full_profile_sha256;
};
OriginalBillingContext original_context(Store& store, const BillingTrustPolicy& policy, const std::string& attempt_id) {
    validate_billing_trust_policy(store, policy.request_id(), policy);
    auto original = store.live_attempt(attempt_id, true);
    const auto& attempt = field(original, "original_attempt");
    require(text(attempt, "request_id") == policy.request_id() &&
            text(attempt, "config_snapshot_sha256") == policy.config_snapshot_sha256(), ErrorCode::Unauthorized,
            "billing attempt belongs to another authorized request");
    require(field(original, "send_claim").is_object() && !field(attempt, "settlement_sha256").is_null(),
            ErrorCode::NotReady, "billing requires an original send claim and original settlement");
    const auto request = model_request_from_json(field(attempt, "request_envelope"));
    require(request.attempt_id == attempt_id, ErrorCode::Corrupt, "billing original attempt envelope changed identity");
    const auto row = store.request(policy.request_id());
    const auto frozen = FrozenRequestConfig::restore(canonical_json(field(row, "config_snapshot")), policy.config_snapshot_sha256());
    const auto profile = frozen.model_profile(request.role);
    auto settings = live_model_settings_from_json(profile, request.model.model);
    auto encoded = encode_responses_request(settings.provider, request, settings.stream);
    require(!encoded.billing_challenge.empty() &&
            field(original, "frozen_live_settings") == live_model_settings_json(settings),
            ErrorCode::Corrupt, "billing source differs from the original frozen live settings");
    const auto& execution = field(original, "execution");
    require(text(execution, "encoded_record_sha256") == encoded.record_sha256 &&
            text(execution, "request_body_sha256") == encoded.body_sha256 &&
            text(execution, "profile_sha256") == encoded.profile_sha256 &&
            text(execution, "logical_request_sha256") == encoded.logical_request_sha256,
            ErrorCode::Corrupt, "billing source differs from the original encoded send identity");
    return {std::move(original), request, std::move(settings), std::move(encoded), sha256_hex(canonical_json(profile))};
}

Json validate_observation(const OriginalBillingContext& context, const Json& payload) {
    const auto& transport = field(context.original, "transport");
    if (transport.is_null()) {
        require(field(context.original, "host_unobserved_settlement").is_object() &&
                text(field(context.original, "host_unobserved_settlement"), "reason") == "unobserved_after_recovery",
                ErrorCode::NotReady, "missing response has no original recovery settlement");
        return Object{{"status", "unobserved"}, {"original_response_id", nullptr}, {"original_body_complete", false}};
    }
    require(boolean(transport, "network_started"), ErrorCode::Conflict,
            "billing receipt contradicts an original observation that network never started");
    require(text(transport, "encoded_record_sha256") == context.encoded.record_sha256 &&
            text(transport, "request_body_sha256") == context.encoded.body_sha256,
            ErrorCode::Corrupt, "billing transport differs from the original send");
    const auto& observation = field(context.original, "private_observation");
    const auto body = unhex(text(observation, "raw_body_hex"), 4U * 1024U * 1024U);
    const auto headers = unhex(text(observation, "raw_headers_hex"), 65536);
    require(sha256_hex(body) == text(transport, "raw_body_sha256") &&
            sha256_hex(headers) == text(transport, "raw_headers_sha256") &&
            sha256_hex(canonical_json(field(observation, "response"))) == text(transport, "decoded_response_sha256") &&
            sha256_hex(canonical_json(field(observation, "protocol_evidence"))) == text(transport, "protocol_evidence_sha256"),
            ErrorCode::Corrupt, "billing original private observation failed its fingerprints");
    const bool complete = boolean(transport, "transfer_complete") && !boolean(transport, "raw_body_truncated");
    std::optional<std::string> observed_id;
    const auto& protocol = field(observation, "protocol_evidence");
    if (const auto* report = object(protocol).if_contains("offline_decoder_report"); report && report->is_object()) {
        if (const auto* id = report->as_object().if_contains("provider_response_id"); id && id->is_string() && !id->as_string().empty())
            observed_id = std::string(id->as_string());
    }
    if (observed_id)
        require(field(payload, "provider_response_id").is_string() &&
                *observed_id == text(payload, "provider_response_id"), ErrorCode::Conflict,
                "billing receipt names a different observed provider response");
    if (complete) {
        require(sha256_hex(body) == text(payload, "response_body_sha256"),
                ErrorCode::Conflict, "billing receipt differs from the complete original response");
    }
    return Object{{"status", complete ? "complete" : "partial"},
        {"original_response_id", observed_id ? Json(*observed_id) : Json(nullptr)}, {"original_body_complete", complete}};
}
} // namespace

BillingTrustPolicy billing_trust_policy_from_json(const Json& value) {
    require(canonical_json(value).size() <= 65536, ErrorCode::Quota, "billing policy exceeds its byte bound");
    exact(value, {"schema_version", "kind", "trust_domain", "request_id", "config_snapshot_sha256",
        "issuer", "account_id", "public_keys", "models"});
    require(integer(value, "schema_version") == 1 && text(value, "kind") == "wls-billing-trust-policy-v1",
            ErrorCode::Unsupported, "unsupported billing trust policy");
    const auto domain = text(value, "trust_domain");
    require(domain == "local_tls_fixture" || domain == "provider_billing", ErrorCode::Unsupported,
            "unsupported billing trust domain");
    require(is_typed_id(text(value, "request_id"), "req") && digest(text(value, "config_snapshot_sha256")) &&
            token(text(value, "issuer"), 256) && token(text(value, "account_id"), 256),
            ErrorCode::InvalidInput, "invalid billing policy identity");
    const auto& keys = object(field(value, "public_keys"));
    require(!keys.empty() && keys.size() <= 8, ErrorCode::Quota, "billing policy needs a bounded public-key set");
    for (const auto& entry : keys)
        require(token(entry.key(), 256) && entry.value().is_string() && digest(entry.value().as_string()),
                ErrorCode::InvalidInput, "invalid billing public-key identity");
    const auto& models = object(field(value, "models"));
    require(!models.empty() && models.size() <= 32, ErrorCode::Quota, "billing policy needs a bounded model set");
    for (const auto& entry : models)
        require(token(entry.key()) && entry.value().is_string() && digest(entry.value().as_string()),
                ErrorCode::InvalidInput, "invalid billing model profile identity");
    BillingTrustPolicy result;
    result.definition_ = value;
    result.sha256_ = sha256_hex(canonical_json(value));
    result.request_id_ = text(value, "request_id");
    result.config_snapshot_sha256_ = text(value, "config_snapshot_sha256");
    return result;
}

void validate_billing_trust_policy(Store& store, const std::string& request_id, const BillingTrustPolicy& policy) {
    require(request_id == policy.request_id(), ErrorCode::Unauthorized, "billing policy belongs to another request");
    const auto row = store.request(request_id);
    require(text(row, "config_snapshot_sha256") == policy.config_snapshot_sha256(), ErrorCode::Conflict,
            "billing policy differs from the original frozen configuration");
    const auto frozen = FrozenRequestConfig::restore(canonical_json(field(row, "config_snapshot")), policy.config_snapshot_sha256());
    std::set<std::string> selected;
    const auto& models = object(field(policy.json(), "models"));
    for (const auto role : {Role::Dispatcher, Role::TeamLead, Role::Worker}) {
        const auto model = frozen.resolve(role).model;
        const auto profile = frozen.model_profile(role);
        const auto* approved = models.if_contains(model);
        require(approved && approved->is_string() && approved->as_string() == sha256_hex(canonical_json(profile)),
                ErrorCode::Unauthorized, "billing policy does not authorize an original role profile");
        const auto settings = live_model_settings_from_json(profile, model);
        require(settings.provider.billing_challenge == "wls-billing-challenge-v1", ErrorCode::Unsupported,
                "original profile did not freeze the billing challenge contract");
        const auto expected_domain = settings.execution_kind == "local_tls" ? "local_tls_fixture" : "provider_billing";
        require(text(policy.json(), "trust_domain") == expected_domain, ErrorCode::Unauthorized,
                "billing trust domain differs from the original execution kind");
        selected.insert(model);
    }
    require(selected.size() == models.size(), ErrorCode::InvalidInput,
            "billing policy must authorize exactly the original selected model profiles");
}

std::string billing_receipt_signing_bytes(const Json& payload) {
    validate_payload(payload);
    return std::string(signing_domain) + canonical_json(payload);
}

VerifiedUsageEvidence verify_signed_billing_receipt(Store& store, const BillingTrustPolicy& policy,
                                                   const std::string& attempt_id, std::string receipt_bytes) {
    const auto receipt = parse_json_strict(receipt_bytes, receipt_limit);
    exact(receipt, {"schema_version", "kind", "payload", "signature_hex"});
    require(integer(receipt, "schema_version") == 1 && text(receipt, "kind") == "wls-signed-billing-receipt-v1",
            ErrorCode::Unsupported, "unsupported signed billing receipt");
    const auto& signed_payload = field(receipt, "payload");
    const auto message = billing_receipt_signing_bytes(signed_payload);
    for (const auto* name : {"trust_domain", "issuer", "account_id"})
        require(text(signed_payload, name) == text(policy.json(), name), ErrorCode::Unauthorized,
                "billing receipt is outside the authorized issuer/account trust domain");
    const auto* key = object(field(policy.json(), "public_keys")).if_contains(text(signed_payload, "key_id"));
    require(key && key->is_string(), ErrorCode::Unauthorized, "billing key is outside the explicit trust policy");
    verify_signature(key->as_string(), text(receipt, "signature_hex"), message);
    const auto context = original_context(store, policy, attempt_id);
    require(text(signed_payload, "billing_challenge") == context.encoded.billing_challenge &&
            text(signed_payload, "request_body_sha256") == context.encoded.body_sha256 &&
            text(signed_payload, "provider_model") == context.settings.provider.model,
            ErrorCode::Conflict, "billing receipt differs from the original unique wire request");
    const auto observation = validate_observation(context, signed_payload);
    const auto& attempt = field(context.original, "original_attempt");
    const auto settlement_sha256 = text(attempt, "settlement_sha256");
    require(digest(settlement_sha256), ErrorCode::Corrupt, "billing original settlement identity is invalid");
    const Object identity{{"trust_domain", signed_payload.at("trust_domain")}, {"issuer", signed_payload.at("issuer")},
        {"account_id", signed_payload.at("account_id")}, {"receipt_id", signed_payload.at("receipt_id")}};
    const auto identity_sha256 = sha256_hex(canonical_json(identity));
    const auto source = "wls-signed-billing-v1:" + identity_sha256;
    const auto bytes = canonical_json(Object{{"kind", "wls-verified-signed-billing-source-v1"},
        {"policy", policy.json()}, {"policy_sha256", policy.sha256()}, {"receipt", receipt},
        {"receipt_original_bytes_hex", [&] {
            constexpr char digits[] = "0123456789abcdef";
            std::string encoded; encoded.reserve(receipt_bytes.size() * 2);
            for (const auto c : receipt_bytes) { const auto byte = static_cast<unsigned char>(c);
                encoded.push_back(digits[byte >> 4U]); encoded.push_back(digits[byte & 15U]); }
            return encoded;
        }()},
        {"original_mapping", Object{{"request_id", policy.request_id()}, {"attempt_id", attempt_id},
            {"settlement_sha256", settlement_sha256}, {"config_snapshot_sha256", policy.config_snapshot_sha256()},
            {"model_profile_sha256", context.full_profile_sha256}, {"encoded_record_sha256", context.encoded.record_sha256},
            {"observation", observation}}}});
    const auto source_kind = context.settings.execution_kind == "local_tls" ? "loopback_fixture" : "host_verified_provider";
    auto result = verify_usage_evidence(source_kind, source, bytes, [&](std::string_view) {
        return UsageEvidenceStatement{attempt_id, settlement_sha256, model_usage_from_json(signed_payload.at("usage"))};
    });
    result.required_reconciliation_id_ = "billing:" + identity_sha256 + ":" + result.sha256();
    return result;
}

std::optional<VerifiedUsageEvidence> verify_observed_billing_receipt(Store& store, const BillingTrustPolicy& policy,
        const ModelRequest& request, const ModelResponse& response, const Json& original_settlement) {
    const auto context = original_context(store, policy, request.attempt_id);
    const auto& original = field(context.original, "original_attempt");
    require(field(original, "request_envelope") == to_json(request) && field(original, "response") == to_json(response) &&
            text(original, "settlement_sha256") == text(original_settlement, "settlement_sha256") &&
            text(original_settlement, "attempt_id") == request.attempt_id,
            ErrorCode::Conflict, "billing hook differs from this original invocation and settlement");
    const auto& transport = field(context.original, "transport");
    require(transport.is_object() && !boolean(transport, "raw_headers_truncated"), ErrorCode::NotReady,
            "billing hook requires complete original response headers");
    const auto& observation = field(context.original, "private_observation");
    require(field(observation, "response") == to_json(response), ErrorCode::Conflict,
            "billing hook differs from the original observed response");
    const auto headers = unhex(text(observation, "raw_headers_hex"), 65536);
    require(sha256_hex(headers) == text(transport, "raw_headers_sha256"), ErrorCode::Corrupt,
            "billing original headers failed their fingerprint");
    const auto receipt = receipt_header(headers);
    if (!receipt) return std::nullopt;
    require(boolean(transport, "transfer_complete") && !boolean(transport, "raw_body_truncated"), ErrorCode::NotReady,
            "synchronous billing needs the complete original response");
    return verify_signed_billing_receipt(store, policy, request.attempt_id, *receipt);
}

} // namespace wls::s1
