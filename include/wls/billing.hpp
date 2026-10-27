#pragma once

#include "wls/live_model.hpp"

#include <optional>
#include <string>

namespace wls::s1 {

// Explicit trusted-host policy. The public keys authenticate one separately
// operated billing issuer/account, not an arbitrary model-produced statement.
// Local TLS fixtures and provider billing have distinct, noninterchangeable
// trust domains. No key, endpoint or account is supplied by default.
class BillingTrustPolicy {
public:
    const Json& json() const noexcept { return definition_; }
    const std::string& sha256() const noexcept { return sha256_; }
    const std::string& request_id() const noexcept { return request_id_; }
    const std::string& config_snapshot_sha256() const noexcept { return config_snapshot_sha256_; }
private:
    friend BillingTrustPolicy billing_trust_policy_from_json(const Json&);
    BillingTrustPolicy() = default;
    Json definition_;
    std::string sha256_;
    std::string request_id_;
    std::string config_snapshot_sha256_;
};

BillingTrustPolicy billing_trust_policy_from_json(const Json& value);

// Read-only checks against all originally frozen role profiles. This neither
// resolves credentials nor grants network, billing or business authority.
void validate_billing_trust_policy(Store& store, const std::string& request_id,
                                  const BillingTrustPolicy& policy);

// Pure bounded signing-message definition for independent issuer adapters.
// There is deliberately no production private-key loading or signing API.
std::string billing_receipt_signing_bytes(const Json& payload);

// Authenticates a signed issuer receipt and maps its unique wire challenge to
// the original attempt and original settlement. It never mutates Store. A late
// receipt can fill accounting after response loss, but cannot invent a missing
// transport observation or restore old business authority.
VerifiedUsageEvidence verify_signed_billing_receipt(Store& store,
        const BillingTrustPolicy& policy, const std::string& attempt_id,
        std::string receipt_bytes);

// Reads the single receipt in original final HTTP headers/trailers. Missing
// receipt returns null; a present malformed or unauthenticated receipt fails.
// The original persisted observation, response and settlement must all agree
// with this invocation. No polling, file lookup or additional network request.
std::optional<VerifiedUsageEvidence> verify_observed_billing_receipt(Store& store,
        const BillingTrustPolicy& policy, const ModelRequest& request,
        const ModelResponse& response, const Json& original_settlement);

} // namespace wls::s1
