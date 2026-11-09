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

// Optional versioned network section (v1) appended to the transport options.
// When enabled, the URL hostname is resolved per call by the pinned host
// resolver helper (host-ipv4-v1); loopback_only additionally requires every
// selected address — target and proxy — to be exactly 127.0.0.1. The frozen
// proxy authority allows one HTTP/HTTPS CONNECT and nothing else.
struct HttpsProxyOptions {
    bool enabled{};
    bool tls{};                    // https:// proxy; http:// proxies stay cleartext
    std::string host;              // strict domain name or IPv4, never userinfo
    std::uint16_t port{};          // explicit 1..65535
    bool basic_auth{};             // only with tls; values are call-time secrets
    std::string basic_username_env;
    std::string basic_password_env;
    // Independent bounded PEM trust for an https proxy; must stay empty for
    // cleartext proxies. The origin CA above is never reused for the proxy.
    std::string ca_bundle_pem;
};

struct HttpsNetworkOptions {
    bool enabled{};
    bool loopback_only{true};
    HttpsProxyOptions proxy;
};

struct HttpsTransportOptions {
    // Explicit bounded trust bytes, never a path or an environment lookup.
    // Test roots belong only in the caller's temporary fixture configuration.
    std::string ca_bundle_pem;
    // Optional host-provided exact IPv4 route for the URL host/port. TLS SNI
    // and hostname verification still use the original URL. It stays more
    // specific than the hostname resolver policy below; numeric endpoints
    // and explicit mappings never spawn the resolver at all.
    std::string resolved_ipv4;
    std::size_t max_header_bytes{64U * 1024U};
    std::size_t max_header_line_bytes{8U * 1024U};
    std::size_t max_total_response_bytes{5U * 1024U * 1024U};
    std::chrono::milliseconds poll_interval{25};
    HttpsNetworkOptions network;
};

// Pure preflight shared by configuration admission and the transport. Rejects
// ambiguous numeric hosts, malformed IPv4 mappings, conflicting numeric
// destinations, and any invalid frozen proxy authority without initializing
// libcurl, resolving DNS or opening sockets.
void validate_responses_https_target(const ProviderProfile& profile,
                                     const HttpsTransportOptions& options);

// One shared descriptor for the frozen options: byte-identical v1 bytes when
// the optional network section is absent, and a v2 descriptor wrapping those
// same v1 bytes plus the frozen network policy (references, never secret
// values) when it is present. Store claim binding hashes this single form.
Json https_transport_options_descriptor(const HttpsTransportOptions& options);
std::string https_transport_options_sha256(const HttpsTransportOptions& options);

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
    // DNS start and HTTP send are distinct facts: a started resolver child
    // sets both dns_started and network_started even when curl never runs.
    bool dns_started{false};
    // An unreaped resolver child keeps its pid/pidfd in the process-local
    // slot and blocks further live transport in this process.
    bool resolver_cleanup_pending{false};
    // Hash over the private route evidence below; empty for legacy calls so
    // summary-v1 bytes stay unchanged.
    std::string network_route_sha256;
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
    // Private route evidence (resolver/helper identity, selected addresses,
    // proxy CONNECT state, DNS/cleanup times). Safe summaries expose only
    // enums, booleans, counts, times and network_route_sha256.
    Json protocol_evidence = boost::json::object{};
    Json network_route = boost::json::object{};
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
// separately supplied in-memory value; it never enters the safe summary. The
// optional proxy Basic values are likewise call-time secrets resolved by the
// trusted host after authorization/budget checks; empty defaults mean no
// authenticated proxy, and an authenticated proxy without them throws before
// any DNS, socket or send-claim work.
HttpsTransportResult perform_responses_https(const ProviderProfile& profile,
        const ModelRequest& request, const EncodedResponsesRequest& encoded,
        const HttpsTransportOptions& options, std::string_view bearer_token,
        std::stop_token stop = {}, std::string_view proxy_basic_username = {},
        std::string_view proxy_basic_password = {});

// Safe by construction: identities, bounded host codes, counts/times/hashes;
// excludes credentials, raw bytes, decoded text/tools and provider diagnostics.
Json https_transport_summary(const HttpsTransportResult& result);
std::string to_string(HttpsTransportStatus status);
std::string to_string(HttpsTransportPhase phase);

} // namespace wls::s1
