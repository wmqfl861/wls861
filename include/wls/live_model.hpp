#pragma once

#include "wls/https_transport.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace wls::s1 {

class Store;
struct StoreActor;

// This data is part of the original request configuration, not a mutable
// environment lookup. Credentials remain a name in provider.api_key_env.
struct LiveModelSettings {
    ProviderProfile provider;
    HttpsTransportOptions transport;
    bool stream{false};
    std::string execution_kind;
    std::string capability_evidence_source;
    std::string capability_evidence_sha256;
};

// Parses the full system.models[model_ref] entry, including its existing
// declared capabilities. No endpoint, CA, credential or model defaults.
LiveModelSettings live_model_settings_from_json(const Json& profile,
                                                std::string_view model_ref);
Json live_transport_options_json(const HttpsTransportOptions& options);
std::string live_transport_options_sha256(const HttpsTransportOptions& options);
Json live_model_settings_json(const LiveModelSettings& settings);

struct LiveExecutionAuthorization {
    // Host execution authorization is independent from frozen configuration.
    // No CLI enables this flag implicitly. Tests use only local_tls profiles.
    bool network_enabled{false};
    bool provider_enabled{false};
};

struct LiveAttemptResult {
    Json reservation = nullptr;
    Json send_claim = nullptr;
    Json settlement = nullptr;
    Json transport_summary = nullptr;
    bool sent{false};
    bool response_accepted{false};
};

// The complete original Store path: reserve -> unique send claim -> HTTPS
// outside any transaction -> private transport record -> original settlement.
// An exception after the claim retains unresolved accounting and never retries.
LiveAttemptResult execute_live_attempt(Store& store, const StoreActor& actor,
        const ModelRequest& request, std::string_view bearer_token,
        const LiveExecutionAuthorization& authorization = {}, std::stop_token stop = {});

struct UsageEvidenceStatement {
    std::string attempt_id;
    std::string settlement_sha256;
    ModelUsage usage;
};

// Only a trusted host registers the verifier. Its job includes authenticating
// the source and parsing its units; a model-produced "verified" field is not
// a verifier. Store never registers this API as an agent tool.
using UsageEvidenceVerifier = std::function<UsageEvidenceStatement(std::string_view)>;

class VerifiedUsageEvidence {
public:
    const UsageEvidenceStatement& statement() const noexcept { return statement_; }
    const std::string& source_kind() const noexcept { return source_kind_; }
    const std::string& source_reference() const noexcept { return source_reference_; }
    const std::string& bytes() const noexcept { return bytes_; }
    const std::string& sha256() const noexcept { return sha256_; }
private:
    friend VerifiedUsageEvidence verify_usage_evidence(std::string, std::string,
        std::string, const UsageEvidenceVerifier&);
    UsageEvidenceStatement statement_;
    std::string source_kind_;
    std::string source_reference_;
    std::string bytes_;
    std::string sha256_;
};

// source_kind is loopback_fixture, host_verified_provider or host_no_dispatch.
// A loopback fixture can only reconcile an attempt frozen as local_tls.
// host_no_dispatch requires the immutable recovered no-claim observation and
// all five dimensions explicitly zero; it cannot clear an actual send claim.
VerifiedUsageEvidence verify_usage_evidence(std::string source_kind,
        std::string source_reference, std::string bytes,
        const UsageEvidenceVerifier& verifier);

struct UsageReconciliation {
    std::string reconciliation_id;
    std::string idempotency_key;
    std::int64_t expected_projection_version{};
};

} // namespace wls::s1
