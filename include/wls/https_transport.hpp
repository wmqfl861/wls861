#pragma once

#include "wls/protocol_codec.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace wls::s1 {

enum class HttpsTransportStatus {
    Completed, HttpError, Cancelled, DeadlineExceeded, CertificateError,
    ConnectionError, TransferError, LimitExceeded, ProtocolError
};

enum class HttpsTransportPhase {
    NotStarted, Connecting, TlsHandshake, RequestWrite, ResponseHeaders, ResponseBody
};

struct HttpsTransportOptions {
    // Explicit bounded trust bytes, never a path or an environment lookup.
    // Test roots belong only in the caller's temporary fixture configuration.
    std::string ca_bundle_pem;
    // Optional host-provided exact IPv4 route for the URL host/port. TLS SNI
    // and hostname verification still use the original URL. No DNS lookup is
    // performed for this route; it is not inferred from environment settings.
    // Without it, hostname resolution requires libcurl's c-ares backend;
    // threaded getaddrinfo cleanup cannot promise bounded cancellation.
    std::string resolved_ipv4;
    std::size_t max_header_bytes{64U * 1024U};
    std::size_t max_header_line_bytes{8U * 1024U};
    std::size_t max_total_response_bytes{5U * 1024U * 1024U};
    std::chrono::milliseconds poll_interval{25};
};

struct HttpsTransportResult {
    ModelResponse response;
    HttpsTransportStatus status{HttpsTransportStatus::TransferError};
    // Last observed stage, independent of the returned terminal status.
    HttpsTransportPhase phase{HttpsTransportPhase::NotStarted};
    // Stable host reason only; never a provider diagnostic or credential value.
    std::string reason;
    std::string encoded_record_sha256;
    std::string request_body_sha256;
    std::string profile_sha256;
    std::string logical_request_sha256;
    std::string ca_bundle_sha256;
    std::string transport_options_sha256;
    int http_status{};
    bool network_started{false};
    bool transfer_complete{false};
    bool raw_headers_truncated{false};
    bool raw_body_truncated{false};
    bool upload_may_have_occurred{false};
    std::size_t header_bytes_observed{};
    std::size_t body_bytes_observed{};
    // Bytes supplied to libcurl and libcurl's reported payload upload are
    // distinct local observations. Neither proves provider acceptance/billing.
    std::size_t request_body_bytes_supplied{};
    std::uint64_t uploaded_body_bytes_reported{};
    std::chrono::microseconds elapsed{};
    std::optional<std::chrono::microseconds> connected_at;
    std::optional<std::chrono::microseconds> tls_verified_at;
    std::optional<std::chrono::microseconds> first_header_at;
    std::optional<std::chrono::microseconds> first_body_at;
    // Private exact HTTP-layer callback bytes, not TLS packets. A server may
    // echo secrets in these fields or response; do not put them in shared logs.
    std::string raw_headers;
    std::string raw_body;
    Json protocol_evidence = boost::json::object{};
    // Consistency of the bounded report, not a signed receipt or Store grant.
    std::string record_sha256;
};

// Trusted-host component only. It validates the exact encoded request against
// the profile and original request, performs one HTTPS POST with no redirects,
// proxies or retries, then consumes the existing strict HTTP/SSE codec. It does
// not resolve credential references, open Store, admit attempts, settle usage,
// dispatch tools or enable any live CLI/ModelAdapter. R3.3b must wrap this call
// in durable admission and one-time send authority before production use.
//
// Invalid inputs throw before network work. Transport/protocol failures return
// the original response tuple with no usable text/tools. The bearer is a
// separately supplied in-memory value; it never enters the safe summary.
HttpsTransportResult perform_responses_https(const ProviderProfile& profile,
        const ModelRequest& request, const EncodedResponsesRequest& encoded,
        const HttpsTransportOptions& options, std::string_view bearer_token,
        std::stop_token stop = {});

// Safe by construction: identities, bounded host codes, counts/times/hashes;
// excludes credentials, raw bytes, decoded text/tools and provider diagnostics.
Json https_transport_summary(const HttpsTransportResult& result);
std::string to_string(HttpsTransportStatus status);
std::string to_string(HttpsTransportPhase phase);

} // namespace wls::s1
