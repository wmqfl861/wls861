#include "wls/https_transport.hpp"

#include "https_resolver_internal.hpp"
#include "wls/sse_replay.hpp"

#include <curl/curl.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;

constexpr std::size_t max_trust_bytes = 1024U * 1024U;
constexpr std::size_t max_header_bytes = 1024U * 1024U;
constexpr std::size_t max_total_bytes = 32U * 1024U * 1024U;
constexpr int socket_send_buffer = 64 * 1024;

void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}

struct CurlGlobal {
    CurlGlobal() {
        require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, ErrorCode::Unsupported,
                "HTTPS transport could not initialize libcurl");
    }
    ~CurlGlobal() { curl_global_cleanup(); }
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
};

const curl_version_info_data* initialize_curl() {
    static const CurlGlobal global;
    (void)global;
    const auto* version = curl_version_info(CURLVERSION_NOW);
    require(version != nullptr && version->version_num >= 0x075800U &&
            (version->features & CURL_VERSION_SSL) != 0 &&
            (version->features & CURL_VERSION_ASYNCHDNS) != 0,
            ErrorCode::Unsupported,
            "HTTPS transport requires libcurl 7.88 or later with TLS and asynchronous DNS");
    return version;
}

struct Handles {
    CURL* easy{curl_easy_init()};
    CURLM* multi{curl_multi_init()};
    curl_slist* headers{};
    curl_slist* resolution{};
    curl_slist* connect_to{};
    bool attached{false};
    Handles() = default;
    Handles(const Handles&) = delete;
    Handles& operator=(const Handles&) = delete;
    ~Handles() { close(); }
    void close() noexcept {
        if (attached) (void)curl_multi_remove_handle(multi, easy);
        attached = false;
        if (easy != nullptr) curl_easy_cleanup(easy);
        easy = nullptr;
        if (multi != nullptr) curl_multi_cleanup(multi);
        multi = nullptr;
        if (headers != nullptr) curl_slist_free_all(headers);
        headers = nullptr;
        if (resolution != nullptr) curl_slist_free_all(resolution);
        resolution = nullptr;
        if (connect_to != nullptr) curl_slist_free_all(connect_to);
        connect_to = nullptr;
    }
    void header(const std::string& line) {
        auto* appended = curl_slist_append(headers, line.c_str());
        require(appended != nullptr, ErrorCode::Storage, "HTTPS header allocation failed");
        headers = appended;
    }
    void resolve(const std::string& line) {
        auto* appended = curl_slist_append(resolution != nullptr ? resolution : nullptr,
                                           line.c_str());
        require(appended != nullptr, ErrorCode::Storage, "HTTPS address pin allocation failed");
        resolution = appended;
    }
};

template <typename T>
void option(CURL* handle, CURLoption key, T value) {
    require(curl_easy_setopt(handle, key, value) == CURLE_OK, ErrorCode::Unsupported,
            "required HTTPS transport option is unavailable");
}

ModelResponse original_response(const ModelRequest& request) {
    ModelResponse result;
    result.call_id = request.call_id;
    result.attempt_id = request.attempt_id;
    result.role_call_index = request.role_call_index;
    result.role = request.role;
    result.binding = request.binding;
    result.config_snapshot_id = request.config_snapshot_id;
    result.status = ModelResultStatus::Error;
    return result;
}

void fail(HttpsTransportResult& result, HttpsTransportStatus status,
          const char* reason) {
    result.status = status;
    result.reason = reason;
    result.response.status = status == HttpsTransportStatus::Cancelled ||
            status == HttpsTransportStatus::DeadlineExceeded ?
            ModelResultStatus::Interrupted : ModelResultStatus::Error;
    result.response.error_code = reason;
    result.response.error_message = "HTTPS result is not usable; no automatic retry or tool dispatch";
    result.response.text.clear();
    result.response.tool_calls.clear();
}

Json time_json(const std::optional<Microseconds>& time) {
    return time ? Json(time->count()) : Json(nullptr);
}

Object summary_record(const HttpsTransportResult& result) {
    return Object{
        {"format", "wls-https-transport-summary-v1"},
        {"record_kind", "local_transport_observation"},
        {"timing_source", "executor_steady_clock"},
        {"byte_source", "libcurl_http_callbacks_not_tls_packets"},
        {"call_id", result.response.call_id}, {"attempt_id", result.response.attempt_id},
        {"role", role_name(result.response.role)}, {"role_call_index", result.response.role_call_index},
        {"binding", to_json(result.response.binding)},
        {"config_snapshot_id", result.response.config_snapshot_id},
        {"status", to_string(result.status)}, {"phase", to_string(result.phase)},
        {"reason", result.reason}, {"http_status", result.http_status},
        {"network_started", result.network_started}, {"transfer_complete", result.transfer_complete},
        {"upload_may_have_occurred", result.upload_may_have_occurred},
        {"request_body_bytes_supplied", result.request_body_bytes_supplied},
        {"uploaded_body_bytes_reported", result.uploaded_body_bytes_reported},
        {"header_bytes_observed", result.header_bytes_observed},
        {"header_bytes_retained", result.raw_headers.size()},
        {"body_bytes_observed", result.body_bytes_observed},
        {"body_bytes_retained", result.raw_body.size()},
        {"raw_headers_truncated", result.raw_headers_truncated},
        {"raw_body_truncated", result.raw_body_truncated},
        {"raw_headers_sha256", sha256_hex(result.raw_headers)},
        {"raw_body_sha256", sha256_hex(result.raw_body)},
        {"encoded_record_sha256", result.encoded_record_sha256},
        {"request_body_sha256", result.request_body_sha256},
        {"profile_sha256", result.profile_sha256},
        {"logical_request_sha256", result.logical_request_sha256},
        {"ca_bundle_sha256", result.ca_bundle_sha256},
        {"transport_options_sha256", result.transport_options_sha256},
        {"elapsed_us", result.elapsed.count()}, {"connected_at_us", time_json(result.connected_at)},
        {"tls_verified_at_us", time_json(result.tls_verified_at)},
        {"first_header_at_us", time_json(result.first_header_at)},
        {"first_body_at_us", time_json(result.first_body_at)},
        {"decoded_response_sha256", sha256_hex(canonical_json(to_json(result.response)))},
        {"protocol_evidence_sha256", sha256_hex(canonical_json(result.protocol_evidence))},
        {"response_usable", result.status == HttpsTransportStatus::Completed &&
                            result.response.status == ModelResultStatus::Completed},
        {"provider_receipt_verified", false}, {"billing_verified", false},
        {"retry_attempts", 0}, {"tools_dispatched", 0}, {"business_accepted", false}
    };
    // Legacy summaries stay byte-identical; network-policy calls additionally
    // expose the bounded DNS facts and the private route evidence hash.
    if (result.dns_started || result.resolver_cleanup_pending ||
        !result.network_route_sha256.empty()) {
        record["dns_started"] = result.dns_started;
        record["resolver_cleanup_pending"] = result.resolver_cleanup_pending;
        record["network_route_sha256"] = result.network_route_sha256;
    }
    return record;
}

enum class Abort {
    None, Cancelled, Deadline, HeaderLimit, BodyLimit, TotalLimit, ChunkLimit,
    InvalidHeader, CallbackFailure, RepeatRequest, SocketOption, ProxyConnect
};

struct HeaderBlock {
    int status{};
    bool completed{false};
    bool final{false};
    bool content_type_seen{false};
    bool content_encoding_seen{false};
    bool transfer_encoding_seen{false};
    bool content_length_seen{false};
    std::string content_type;
};

struct Transfer {
    const ProviderProfile& profile;
    const EncodedResponsesRequest& encoded;
    const HttpsTransportOptions& options;
    HttpsTransportResult& result;
    std::stop_token stop;
    Clock::time_point started;
    Clock::time_point deadline;
    Abort aborted{Abort::None};
    HeaderBlock header;
    // Bounded proxy-CONNECT sub-state: through an explicit proxy the first
    // response block belongs to the CONNECT, and only exactly one 200 block
    // switches this parser to the origin response. Byte budgets are shared.
    bool proxy_tunnel{false};
    int proxy_status{0};
    bool proxy_established{false};
    std::vector<SseReplayChunk> chunks;
    std::size_t request_offset{};
    unsigned prerequests{};

    Microseconds elapsed() const noexcept {
        return std::chrono::duration_cast<Microseconds>(Clock::now() - started);
    }
    bool interrupted() noexcept {
        if (stop.stop_requested()) {
            aborted = Abort::Cancelled;
            return true;
        }
        if (Clock::now() >= deadline) {
            aborted = Abort::Deadline;
            return true;
        }
        return aborted != Abort::None;
    }
    void advance(HttpsTransportPhase phase) noexcept {
        if (static_cast<int>(result.phase) < static_cast<int>(phase)) result.phase = phase;
    }
};

bool multiply_size(std::size_t size, std::size_t count, std::size_t& answer) noexcept {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return false;
    answer = size * count;
    return true;
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& character : result)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    return result;
}

bool token_character(char character) noexcept {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           std::string_view("!#$%&'*+-.^_`|~").find(character) != std::string_view::npos;
}

bool parse_header(Transfer& transfer, std::string_view bytes) {
    auto& header = transfer.header;
    // No obs-fold or replacement of malformed bytes. The retained callback
    // bytes remain private evidence even when the framing is refused.
    if (!bytes.ends_with("\r\n")) return false;
    bytes.remove_suffix(2);
    for (const char character : bytes) {
        const auto value = static_cast<unsigned char>(character);
        if ((value < 32U && character != '\t') || value == 127U) return false;
    }
    // While the proxy tunnel is not established every line belongs to the
    // single CONNECT response block, which is never the origin HTTP status.
    const bool connect_block = transfer.proxy_tunnel && !transfer.proxy_established;
    if (bytes.starts_with("HTTP/")) {
        if (!connect_block && (header.final || (header.status != 0 && !header.completed)))
            return false;
        if (!(bytes.starts_with("HTTP/1.1 ") || bytes.starts_with("HTTP/1.0 ")) || bytes.size() < 12)
            return false;
        if (bytes.size() > 12 && bytes[12] != ' ') return false;
        int status = 0;
        const auto number = bytes.substr(9, 3);
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), status);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() ||
            status < 100 || status > 599 || status == 101) return false;
        if (connect_block) {
            if (transfer.proxy_status != 0) return false; // one block only
            transfer.proxy_status = status;
            return true;
        }
        header = HeaderBlock{};
        header.status = status;
        transfer.result.http_status = status;
        return true;
    }
    if (!connect_block && header.status == 0) return false;
    if (bytes.empty()) {
        if (connect_block) {
            if (transfer.proxy_status == 0) return false;
            if (transfer.proxy_status < 200) { // interim block; one final block follows
                transfer.proxy_status = 0;
                return true;
            }
            if (transfer.proxy_status != 200) return false; // 407/3xx/others fail now
            transfer.proxy_established = true;
            return true;
        }
        if (header.completed) return header.final; // End of a trailer block.
        if (header.transfer_encoding_seen && header.content_length_seen) return false;
        header.completed = true;
        header.final = header.status >= 200;
        if (!header.final) return !header.content_length_seen && !header.transfer_encoding_seen;
        if (header.status == 200) {
            const auto expected = transfer.encoded.stream ? "text/event-stream" : "application/json";
            if (!header.content_type_seen || header.content_type != expected) return false;
        }
        return true;
    }
    if (bytes.front() == ' ' || bytes.front() == '\t') return false;
    const auto colon = bytes.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    const auto name = bytes.substr(0, colon);
    if (!std::all_of(name.begin(), name.end(), token_character)) return false;
    const auto key = lower(name);
    const auto value = trim(bytes.substr(colon + 1));
    if (connect_block) return true; // bounded syntax only; no origin semantics
    if (header.completed) {
        // HTTP trailers cannot rewrite the already selected response meaning
        // or its message framing. Other valid trailer fields stay raw only.
        return header.final && key != "content-length" && key != "transfer-encoding" &&
               key != "content-type" && key != "content-encoding" && key != "trailer";
    }
    if (key == "content-type") {
        if (header.content_type_seen) return false;
        header.content_type_seen = true;
        header.content_type = lower(trim(value.substr(0, value.find(';'))));
        if (header.content_type.empty()) return false;
    } else if (key == "content-encoding") {
        if (header.content_encoding_seen || lower(value) != "identity") return false;
        header.content_encoding_seen = true;
    } else if (key == "transfer-encoding") {
        if (header.transfer_encoding_seen || lower(value) != "chunked") return false;
        header.transfer_encoding_seen = true;
    } else if (key == "content-length") {
        if (header.content_length_seen || value.empty()) return false;
        header.content_length_seen = true;
        std::uint64_t length{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
    }
    return true;
}

std::size_t receive_header(char* bytes, std::size_t size, std::size_t count, void* pointer) noexcept {
    auto& transfer = *static_cast<Transfer*>(pointer);
    std::size_t length{};
    if (!multiply_size(size, count, length)) {
        transfer.aborted = Abort::HeaderLimit;
        return 0;
    }
    try {
        auto& result = transfer.result;
        transfer.advance(HttpsTransportPhase::ResponseHeaders);
        if (!result.first_header_at) result.first_header_at = transfer.elapsed();
        const auto total_retained = result.raw_headers.size() + result.raw_body.size();
        const auto remaining_total = transfer.options.max_total_response_bytes - total_retained;
        const auto remaining_headers = transfer.options.max_header_bytes - result.raw_headers.size();
        const auto retained = std::min({length, remaining_total, remaining_headers,
                                       transfer.options.max_header_line_bytes});
        result.header_bytes_observed += length;
        result.raw_headers.append(bytes, retained);
        if (retained < length) {
            result.raw_headers_truncated = true;
            transfer.aborted = length > remaining_total ? Abort::TotalLimit : Abort::HeaderLimit;
        }
        if (transfer.interrupted()) return 0;
        if (!parse_header(transfer, std::string_view(bytes, length))) {
            transfer.aborted = transfer.proxy_tunnel && !transfer.proxy_established ?
                Abort::ProxyConnect : Abort::InvalidHeader;
            return 0;
        }
        return length;
    } catch (...) {
        transfer.aborted = Abort::CallbackFailure;
        transfer.result.raw_headers_truncated = true;
        return 0;
    }
}

std::size_t receive_body(char* bytes, std::size_t size, std::size_t count, void* pointer) noexcept {
    auto& transfer = *static_cast<Transfer*>(pointer);
    std::size_t length{};
    if (!multiply_size(size, count, length)) {
        transfer.aborted = Abort::BodyLimit;
        return 0;
    }
    try {
        auto& result = transfer.result;
        transfer.advance(HttpsTransportPhase::ResponseBody);
        const auto arrived = transfer.elapsed();
        if (!result.first_body_at) result.first_body_at = arrived;
        const SseReplayLimits replay_hard;
        const auto body_limit = transfer.encoded.stream ?
            std::min(transfer.profile.limits.max_stream_bytes, replay_hard.max_total_bytes) :
            transfer.profile.limits.max_response_bytes;
        const auto remaining_total = transfer.options.max_total_response_bytes -
                                    result.raw_headers.size() - result.raw_body.size();
        const auto remaining_body = body_limit - result.raw_body.size();
        const auto retained = std::min({length, remaining_total, remaining_body});
        result.body_bytes_observed += length;
        result.raw_body.append(bytes, retained);
        if (transfer.encoded.stream && retained != 0) {
            if (transfer.chunks.size() >= std::min(transfer.profile.limits.max_sse_chunks,
                                                 replay_hard.max_chunks)) {
                transfer.aborted = Abort::ChunkLimit;
            } else {
                transfer.chunks.push_back({std::string(bytes, retained),
                    std::chrono::duration_cast<Milliseconds>(arrived), false});
            }
        }
        if (retained < length) {
            result.raw_body_truncated = true;
            transfer.aborted = length > remaining_total ? Abort::TotalLimit : Abort::BodyLimit;
        }
        if (transfer.interrupted()) return 0;
        if (!transfer.header.final || !transfer.header.completed) {
            transfer.aborted = Abort::InvalidHeader;
            return 0;
        }
        return length;
    } catch (...) {
        transfer.aborted = Abort::CallbackFailure;
        transfer.result.raw_body_truncated = true;
        return 0;
    }
}

std::size_t supply_body(char* buffer, std::size_t size, std::size_t count, void* pointer) noexcept {
    auto& transfer = *static_cast<Transfer*>(pointer);
    if (transfer.interrupted()) return CURL_READFUNC_ABORT;
    std::size_t capacity{};
    if (!multiply_size(size, count, capacity)) {
        transfer.aborted = Abort::CallbackFailure;
        return CURL_READFUNC_ABORT;
    }
    transfer.advance(HttpsTransportPhase::RequestWrite);
    const auto length = std::min(capacity, transfer.encoded.body.size() - transfer.request_offset);
    if (length != 0) std::memcpy(buffer, transfer.encoded.body.data() + transfer.request_offset, length);
    transfer.request_offset += length;
    transfer.result.request_body_bytes_supplied += length;
    return length;
}

int prerequisite(void* pointer, char*, char*, int, int) noexcept {
    auto& transfer = *static_cast<Transfer*>(pointer);
    // This callback is after successful certificate/host verification and
    // before request headers. A second request is never authorized here.
    if (transfer.prerequests++ != 0) {
        transfer.aborted = Abort::RepeatRequest;
        return CURL_PREREQFUNC_ABORT;
    }
    const auto observed = transfer.elapsed();
    if (!transfer.result.connected_at) transfer.result.connected_at = observed;
    transfer.result.tls_verified_at = observed;
    transfer.advance(HttpsTransportPhase::TlsHandshake);
    if (transfer.interrupted()) return CURL_PREREQFUNC_ABORT;
    transfer.result.upload_may_have_occurred = true;
    transfer.advance(HttpsTransportPhase::RequestWrite);
    return CURL_PREREQFUNC_OK;
}

int socket_options(void* pointer, curl_socket_t socket, curlsocktype purpose) noexcept {
    auto& transfer = *static_cast<Transfer*>(pointer);
    if (transfer.interrupted()) return CURL_SOCKOPT_ERROR;
    if (purpose != CURLSOCKTYPE_IPCXN ||
        ::setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &socket_send_buffer,
                     static_cast<socklen_t>(sizeof(socket_send_buffer))) != 0) {
        transfer.aborted = Abort::SocketOption;
        return CURL_SOCKOPT_ERROR;
    }
    return CURL_SOCKOPT_OK;
}

int progress(void* pointer, curl_off_t, curl_off_t, curl_off_t, curl_off_t uploaded) noexcept {
    auto& transfer = *static_cast<Transfer*>(pointer);
    if (uploaded >= 0)
        transfer.result.uploaded_body_bytes_reported = std::max(
            transfer.result.uploaded_body_bytes_reported, static_cast<std::uint64_t>(uploaded));
    return transfer.interrupted() ? 1 : 0;
}

int refuse_seek(void*, curl_off_t, int) noexcept {
    // Even a byte-identical body replay would be another unadmitted attempt.
    return CURL_SEEKFUNC_FAIL;
}

struct Endpoint {
    std::string host;
    std::string port;
    bool numeric{false};
};

Endpoint endpoint_of(std::string_view validated_url, const HttpsTransportOptions& options) {
    // Both callers first validate the provider's HTTPS authority and port.
    // That validation accepts no userinfo, percent escapes or IPv6.
    const auto url = validated_url.substr(8);
    const auto authority = url.substr(0, url.find('/'));
    const auto colon = authority.find(':');
    Endpoint endpoint{std::string(authority.substr(0, colon)),
        colon == std::string_view::npos ? "443" : std::string(authority.substr(colon + 1)), false};
    in_addr parsed{};
    endpoint.numeric = ::inet_pton(AF_INET, endpoint.host.c_str(), &parsed) == 1;
    if (!endpoint.numeric) {
        // curl accepts several legacy numeric spellings. Do not allow those
        // spellings to bypass the explicit resolver policy or address mapping.
        bool legacy_numeric = !endpoint.host.empty();
        std::size_t start = 0;
        for (std::size_t i = 0; i <= endpoint.host.size(); ++i) {
            if (i != endpoint.host.size() && endpoint.host[i] != '.') continue;
            auto part = std::string_view(endpoint.host).substr(start, i - start);
            const bool hex = part.starts_with("0x") || part.starts_with("0X");
            if (hex) part.remove_prefix(2);
            legacy_numeric = legacy_numeric && !part.empty() &&
                std::all_of(part.begin(), part.end(), [hex](char character) {
                    return (character >= '0' && character <= '9') ||
                        (hex && ((character >= 'a' && character <= 'f') ||
                                 (character >= 'A' && character <= 'F')));
                });
            start = i + 1;
        }
        require(!legacy_numeric, ErrorCode::InvalidInput, "HTTPS endpoint uses an ambiguous numeric address");
    }
    if (!options.resolved_ipv4.empty()) {
        require(::inet_pton(AF_INET, options.resolved_ipv4.c_str(), &parsed) == 1 &&
                options.resolved_ipv4.find('\0') == std::string::npos,
                ErrorCode::InvalidInput, "HTTPS address mapping must be one strict IPv4 address");
        require(!endpoint.numeric || endpoint.host == options.resolved_ipv4,
                ErrorCode::InvalidInput, "HTTPS numeric endpoint conflicts with its explicit address mapping");
    }
    return endpoint;
}

bool strict_ipv4_spelling(std::string_view host) {
    if (host.empty() || host.size() > 15) return false;
    std::size_t start = 0;
    int parts = 0;
    for (std::size_t index = 0; index <= host.size(); ++index) {
        if (index != host.size() && host[index] != '.') continue;
        const auto part = host.substr(start, index - start);
        int value = 0;
        if (++parts > 4 || part.empty() || part.size() > 3 ||
            !std::all_of(part.begin(), part.end(), [](char c) {
                return c >= '0' && c <= '9';
            }) || (part.size() > 1 && part.front() == '0') ||
            (value = std::stoi(std::string(part))) > 255)
            return false;
        start = index + 1;
    }
    return parts == 4;
}

bool strict_domain_name(std::string_view host) {
    // RFC-style letter-first labels; numeric-looking spellings must instead be
    // strict IPv4, so legacy numeric hosts never ride the resolver policy.
    const auto label = [](std::string_view part) {
        const auto letter = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        };
        const auto inner = [&](char c) { return letter(c) || c == '-' || (c >= '0' && c <= '9'); };
        return !part.empty() && part.size() <= 63 && letter(part.front()) &&
            letter(part.back()) && std::all_of(part.begin(), part.end(), inner);
    };
    if (host.empty() || host.size() > 253) return false;
    std::size_t start = 0;
    int labels = 0;
    for (std::size_t index = 0; index <= host.size(); ++index) {
        if (index != host.size() && host[index] != '.') continue;
        if (!label(host.substr(start, index - start))) return false;
        ++labels;
        start = index + 1;
    }
    return labels >= 1;
}

bool environment_reference(const std::string& name) {
    const auto letter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    return !name.empty() && name.size() <= 128 && letter(name.front()) &&
        std::all_of(name.begin(), name.end(), [&](char c) {
            return letter(c) || (c >= '0' && c <= '9');
        });
}

// Pure preflight for the frozen network section: strict proxy authority
// grammar, scheme-specific trust and reference rules, no DNS or sockets.
void validate_network_policy(const HttpsNetworkOptions& network) {
    if (!network.enabled) {
        require(!network.proxy.enabled, ErrorCode::InvalidInput,
                "an explicit proxy requires the frozen host resolver policy");
        return;
    }
    const auto& proxy = network.proxy;
    if (!proxy.enabled) return;
    require(proxy.port != 0, ErrorCode::InvalidInput,
            "the frozen proxy authority requires an explicit port");
    require(strict_domain_name(proxy.host) || strict_ipv4_spelling(proxy.host),
            ErrorCode::InvalidInput,
            "the frozen proxy host must be a strict domain name or IPv4");
    if (proxy.tls) {
        require(!proxy.ca_bundle_pem.empty() && proxy.ca_bundle_pem.size() <= max_trust_bytes &&
                proxy.ca_bundle_pem.find('\0') == std::string::npos, ErrorCode::InvalidInput,
                "an HTTPS proxy requires its own bounded PEM trust");
        require(!proxy.basic_auth ||
                (environment_reference(proxy.basic_username_env) &&
                 environment_reference(proxy.basic_password_env)), ErrorCode::InvalidInput,
                "proxy authentication must use valid environment reference names");
    } else {
        require(proxy.ca_bundle_pem.empty() && !proxy.basic_auth, ErrorCode::InvalidInput,
                "cleartext HTTP proxies cannot carry authentication or trust material");
    }
}

Object network_policy_json(const HttpsNetworkOptions& network) {
    Json proxy = nullptr;
    if (network.proxy.enabled) {
        proxy = Object{{"scheme", network.proxy.tls ? "https" : "http"},
            {"host", network.proxy.host}, {"port", network.proxy.port},
            {"auth", network.proxy.basic_auth ? "basic" : "none"},
            {"ca_bundle_sha256", sha256_hex(network.proxy.ca_bundle_pem)},
            {"basic_username_env", network.proxy.basic_username_env},
            {"basic_password_env", network.proxy.basic_password_env}};
    }
    return Object{{"version", 1}, {"resolver", "host-ipv4-v1"},
        {"address_scope", network.loopback_only ? "loopback-only" : "configured-endpoint"},
        {"proxy", proxy}};
}

// Resolves one frozen hostname through the pinned helper under the shared
// deadline, records the bounded observations, and applies the address scope.
// Returns false only after the result is classified and ready to finalize.
void collect_observations(CURL* easy, Transfer& transfer) noexcept {
    curl_off_t connected{};
    if (curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME_T, &connected) == CURLE_OK && connected > 0) {
        // libcurl offsets start at transfer start, after the bounded local
        // preparation. Record the first executor observation on this clock.
        if (!transfer.result.connected_at) transfer.result.connected_at = transfer.elapsed();
        transfer.advance(HttpsTransportPhase::TlsHandshake);
    }
    curl_off_t uploaded{};
    if (curl_easy_getinfo(easy, CURLINFO_SIZE_UPLOAD_T, &uploaded) == CURLE_OK && uploaded >= 0)
        transfer.result.uploaded_body_bytes_reported = std::max(
            transfer.result.uploaded_body_bytes_reported, static_cast<std::uint64_t>(uploaded));
    long status{};
    if (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status >= 100 && status <= 599)
        transfer.result.http_status = static_cast<int>(status);
}

void classify_abort(HttpsTransportResult& result, Abort aborted, CURLcode code) {
    switch (aborted) {
    case Abort::Cancelled: return fail(result, HttpsTransportStatus::Cancelled, "cancelled");
    case Abort::Deadline: return fail(result, HttpsTransportStatus::DeadlineExceeded, "deadline");
    case Abort::HeaderLimit: return fail(result, HttpsTransportStatus::LimitExceeded, "header_limit");
    case Abort::BodyLimit: return fail(result, HttpsTransportStatus::LimitExceeded, "body_limit");
    case Abort::TotalLimit: return fail(result, HttpsTransportStatus::LimitExceeded, "total_limit");
    case Abort::ChunkLimit: return fail(result, HttpsTransportStatus::LimitExceeded, "chunk_limit");
    case Abort::InvalidHeader: return fail(result, HttpsTransportStatus::ProtocolError, "invalid_http_header");
    case Abort::CallbackFailure: return fail(result, HttpsTransportStatus::TransferError, "callback_failure");
    case Abort::RepeatRequest: return fail(result, HttpsTransportStatus::TransferError, "repeat_request_refused");
    case Abort::SocketOption: return fail(result, HttpsTransportStatus::ConnectionError, "socket_option_failed");
    case Abort::ProxyConnect: return fail(result, HttpsTransportStatus::ConnectionError, "proxy_connect_refused");
    case Abort::None: break;
    }
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
        return fail(result, HttpsTransportStatus::DeadlineExceeded, "deadline");
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_ISSUER_ERROR:
        return fail(result, HttpsTransportStatus::CertificateError, "certificate_error");
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_PROXY:
        return fail(result, HttpsTransportStatus::ConnectionError, "connection_error");
    case CURLE_WEIRD_SERVER_REPLY:
    case CURLE_BAD_CONTENT_ENCODING:
        return fail(result, HttpsTransportStatus::ProtocolError, "invalid_http_framing");
    default:
        return fail(result, HttpsTransportStatus::TransferError, "transfer_error");
    }
}

// Resolves one frozen hostname through the pinned helper under the shared
// deadline, records the bounded observations, and applies the address scope.
// Returns false only after the result is classified and ready to finalize.
bool route_address(Transfer& transfer, const std::string& host, const std::string& port,
                   std::string& selected, Object& route) {
    auto& result = transfer.result;
    auto dns = resolver::resolve_ipv4(host, port, transfer.deadline, transfer.stop);
    if (dns.started) {
        result.dns_started = true;
        result.network_started = true;
    }
    result.resolver_cleanup_pending = result.resolver_cleanup_pending || dns.cleanup_pending;
    route = Object{{"hostname", host}, {"helper_identity", dns.helper_identity},
        {"addresses_returned", dns.addresses.size()},
        {"resolver_elapsed_us", dns.elapsed.count()},
        {"cleanup_pending", dns.cleanup_pending}};
    if (!dns.ok) {
        if (transfer.stop.stop_requested())
            classify_abort(result, Abort::Cancelled, CURLE_ABORTED_BY_CALLBACK);
        else if (Clock::now() >= transfer.deadline)
            classify_abort(result, Abort::Deadline, CURLE_OPERATION_TIMEDOUT);
        else
            fail(result, HttpsTransportStatus::ConnectionError, "dns_resolution_failed");
        return false;
    }
    if (transfer.options.network.loopback_only) {
        const auto found = std::find(dns.addresses.begin(), dns.addresses.end(), "127.0.0.1");
        if (found == dns.addresses.end()) {
            fail(result, HttpsTransportStatus::ConnectionError, "address_scope_refused");
            return false;
        }
        selected = *found; // A resolved address never widens loopback authority.
    } else {
        selected = dns.addresses.front();
    }
    route["selected"] = selected;
    return true;
}

void decode_retained(Transfer& transfer, const ModelRequest& request) {
    auto& result = transfer.result;
    // Decode retained bytes independently of business usability. For example,
    // a valid terminal's usage survives an illegal tail or later cancellation.
    // Its tools are cleared by the outer transport outcome before return.
    if (!transfer.header.final || !transfer.header.completed) return;
    try {
        if (transfer.encoded.stream && result.http_status == 200 &&
            transfer.header.content_type == "text/event-stream") {
            // The decoder already intersects its hard limits with the frozen
            // profile. A wider valid profile cannot enlarge those hard limits
            // or make an otherwise small stream fail its caller validation.
            const auto replay = replay_responses_sse(transfer.profile, request, transfer.chunks);
            result.response = replay.response;
            result.protocol_evidence = Object{{"decoder", "bounded_responses_sse_replay"},
                {"input_origin", "captured_libcurl_body_callbacks"},
                {"arrival_time_origin", "executor_steady_clock"},
                {"offline_decoder_report", replay.evidence}};
        } else if (result.raw_body.size() <= transfer.profile.limits.max_response_bytes &&
                   result.http_status >= 100 && result.http_status <= 599) {
            const auto decoded = decode_responses_response(transfer.profile, request,
                                                          result.http_status, result.raw_body);
            result.response = decoded.response;
            result.protocol_evidence = Object{{"decoder", "bounded_responses_http_decoder"},
                {"input_origin", "captured_libcurl_body_callbacks"},
                {"offline_decoder_report", decoded.evidence}};
        }
    } catch (const Error&) {
        // Local protocol errors are represented by a bounded host code. Do
        // not export arbitrary decoder/provider diagnostic strings as logs.
        result.protocol_evidence = Object{{"decoder_failed", true}};
    }
}

void finalize(HttpsTransportResult& result, const Transfer& transfer) {
    result.elapsed = transfer.elapsed();
    result.record_sha256 = sha256_hex(canonical_json(summary_record(result)));
    // Final cancellation/deadline check includes protocol validation and the
    // first report hash. The final check is the completion linearization point.
    if (transfer.stop.stop_requested()) {
        fail(result, HttpsTransportStatus::Cancelled, "cancelled");
    } else if (Clock::now() >= transfer.deadline) {
        fail(result, HttpsTransportStatus::DeadlineExceeded, "deadline");
    } else {
        return;
    }
    result.elapsed = transfer.elapsed();
    result.record_sha256 = sha256_hex(canonical_json(summary_record(result)));
}

} // namespace

void validate_responses_https_target(const ProviderProfile& profile,
                                     const HttpsTransportOptions& options) {
    validate_provider_profile(profile);
    (void)endpoint_of(profile.base_url, options);
    validate_network_policy(options.network);
}

Json https_transport_options_descriptor(const HttpsTransportOptions& options) {
    validate_network_policy(options.network);
    // Byte-identical to the legacy v1 descriptor whenever the optional
    // network section is absent; the v2 form wraps the same v1 bytes.
    Json descriptor = Object{{"format", "wls-https-transport-options-v1"},
        {"ca_bundle_sha256", sha256_hex(options.ca_bundle_pem)},
        {"resolved_ipv4", options.resolved_ipv4}, {"max_header_bytes", options.max_header_bytes},
        {"max_header_line_bytes", options.max_header_line_bytes},
        {"max_total_response_bytes", options.max_total_response_bytes},
        {"poll_interval_ms", options.poll_interval.count()},
        {"socket_send_buffer_bytes", socket_send_buffer}, {"upload_buffer_bytes", 16U * 1024U},
        {"dns_policy", "strict_ipv4_or_explicit_mapping_or_cares"}};
    if (options.network.enabled) {
        descriptor = Object{{"format", "wls-https-transport-options-v2"},
            {"base", descriptor}, {"network", network_policy_json(options.network)}};
    }
    return descriptor;
}

std::string https_transport_options_sha256(const HttpsTransportOptions& options) {
    return sha256_hex(canonical_json(https_transport_options_descriptor(options)));
}

std::string to_string(HttpsTransportStatus status) {
    switch (status) {
    case HttpsTransportStatus::Completed: return "completed";
    case HttpsTransportStatus::HttpError: return "http_error";
    case HttpsTransportStatus::Cancelled: return "cancelled";
    case HttpsTransportStatus::DeadlineExceeded: return "deadline_exceeded";
    case HttpsTransportStatus::CertificateError: return "certificate_error";
    case HttpsTransportStatus::ConnectionError: return "connection_error";
    case HttpsTransportStatus::TransferError: return "transfer_error";
    case HttpsTransportStatus::LimitExceeded: return "limit_exceeded";
    case HttpsTransportStatus::ProtocolError: return "protocol_error";
    }
    throw Error(ErrorCode::InvalidInput, "unknown HTTPS transport status");
}

std::string to_string(HttpsTransportPhase phase) {
    switch (phase) {
    case HttpsTransportPhase::NotStarted: return "not_started";
    case HttpsTransportPhase::Connecting: return "connecting";
    case HttpsTransportPhase::TlsHandshake: return "tls_handshake";
    case HttpsTransportPhase::RequestWrite: return "request_write";
    case HttpsTransportPhase::ResponseHeaders: return "response_headers";
    case HttpsTransportPhase::ResponseBody: return "response_body";
    }
    throw Error(ErrorCode::InvalidInput, "unknown HTTPS transport phase");
}

HttpsTransportResult perform_responses_https(const ProviderProfile& profile,
        const ModelRequest& request, const EncodedResponsesRequest& encoded,
        const HttpsTransportOptions& options, std::string_view bearer_token,
        std::stop_token stop, std::string_view proxy_basic_username,
        std::string_view proxy_basic_password) {
    const auto started = Clock::now();
    const auto expected = encode_responses_request(profile, request, encoded.stream);
    require(to_json(encoded) == to_json(expected), ErrorCode::Corrupt,
            "HTTPS encoded request differs from its original request and profile");
    require(!options.ca_bundle_pem.empty() && options.ca_bundle_pem.size() <= max_trust_bytes,
            ErrorCode::InvalidInput, "HTTPS transport requires an explicit bounded PEM trust bundle");
    require(options.max_header_bytes > 0 && options.max_header_bytes <= max_header_bytes &&
            options.max_header_line_bytes > 0 && options.max_header_line_bytes <= 64U * 1024U &&
            options.max_total_response_bytes > 0 && options.max_total_response_bytes <= max_total_bytes &&
            options.poll_interval.count() > 0 && options.poll_interval <= Milliseconds(25),
            ErrorCode::InvalidInput, "HTTPS transport limits are outside hard bounds");
    require(!bearer_token.empty() && bearer_token.size() <= 8192 &&
            std::all_of(bearer_token.begin(), bearer_token.end(), [](char character) {
                const auto byte = static_cast<unsigned char>(character);
                return byte >= 33U && byte <= 126U;
            }), ErrorCode::InvalidInput, "HTTPS bearer must be bounded nonempty ASCII without whitespace");
    const auto endpoint = endpoint_of(encoded.url, options);
    const auto* key_log = std::getenv("SSLKEYLOGFILE");
    require(key_log == nullptr || *key_log == '\0', ErrorCode::Unsupported,
            "HTTPS transport refuses TLS key logging enabled by the environment");
    validate_network_policy(options.network);
    const auto printable_secret = [](std::string_view value, std::string_view::size_type maximum) {
        return !value.empty() && value.size() <= maximum &&
            value.find(':') == std::string_view::npos &&
            std::all_of(value.begin(), value.end(), [](char character) {
                const auto byte = static_cast<unsigned char>(character);
                return byte >= 33U && byte <= 126U;
            });
    };
    require(!options.network.proxy.enabled || !options.network.proxy.basic_auth ||
            (printable_secret(proxy_basic_username, 1024) &&
             !proxy_basic_password.empty() && proxy_basic_password.size() <= 8192 &&
             std::all_of(proxy_basic_password.begin(), proxy_basic_password.end(),
                 [](char character) {
                     const auto byte = static_cast<unsigned char>(character);
                     return byte >= 33U && byte <= 126U;
                 })),
            ErrorCode::InvalidInput,
            "an authenticated proxy needs both bounded call-time secret values before DNS");

    HttpsTransportResult result;
    result.response = original_response(request);
    result.encoded_record_sha256 = encoded.record_sha256;
    result.request_body_sha256 = encoded.body_sha256;
    result.profile_sha256 = encoded.profile_sha256;
    result.logical_request_sha256 = encoded.logical_request_sha256;
    result.ca_bundle_sha256 = sha256_hex(options.ca_bundle_pem);
    result.transport_options_sha256 = https_transport_options_sha256(options);
    Transfer transfer{profile, encoded, options, result, stop, started, started + request.timeout,
                      Abort::None, {}, false, 0, false, {}, 0, 0};
    if (transfer.interrupted()) {
        classify_abort(result, transfer.aborted, CURLE_ABORTED_BY_CALLBACK);
        finalize(result, transfer);
        return result;
    }

    // Frozen resolver route: numeric endpoints and explicit mappings keep the
    // no-resolver paths; everything else goes through the pinned helper under
    // this call's single deadline, then pins libcurl to the selected address.
    std::string target_address;
    std::string proxy_address;
    Object target_route;
    Object proxy_route;
    if (options.network.enabled) {
        bool routed = true;
        if (endpoint.numeric) {
            target_address = endpoint.host;
            target_route = Object{{"mode", "numeric_url"}, {"selected", target_address}};
        } else if (!options.resolved_ipv4.empty()) {
            target_address = options.resolved_ipv4;
            target_route = Object{{"mode", "explicit_mapping"}, {"selected", target_address}};
        } else {
            routed = route_address(transfer, endpoint.host, endpoint.port, target_address,
                                   target_route);
        }
        if (routed && options.network.proxy.enabled) {
            const auto& proxy = options.network.proxy;
            proxy_route = Object{{"scheme", proxy.tls ? "https" : "http"},
                {"auth", proxy.basic_auth ? "basic" : "none"}};
            if (strict_ipv4_spelling(proxy.host)) {
                proxy_address = proxy.host;
                proxy_route["mode"] = "numeric_url";
                proxy_route["selected"] = proxy_address;
            } else {
                routed = route_address(transfer, proxy.host, std::to_string(proxy.port),
                                       proxy_address, proxy_route);
            }
        }
        Object route{{"policy_sha256", result.transport_options_sha256},
            {"resolver", "host-ipv4-v1"},
            {"address_scope", options.network.loopback_only ? "loopback-only"
                                                            : "configured-endpoint"},
            {"dns_started", result.dns_started}, {"target", target_route}};
        if (options.network.proxy.enabled) route["proxy"] = proxy_route;
        result.network_route = route;
        result.network_route_sha256 = sha256_hex(canonical_json(route));
        if (!routed) {
            finalize(result, transfer);
            return result;
        }
    }

    const auto* version = initialize_curl();
    require(endpoint.numeric || !options.resolved_ipv4.empty() || options.network.enabled ||
            (version->ares != nullptr && *version->ares != '\0'), ErrorCode::Unsupported,
            "HTTPS DNS names need an explicit address mapping or a c-ares resolver for bounded cleanup");
    Handles handles;
    require(handles.easy != nullptr && handles.multi != nullptr, ErrorCode::Storage,
            "HTTPS handle allocation failed");
    handles.header("Content-Type: application/json");
    handles.header(encoded.stream ? "Accept: text/event-stream" : "Accept: application/json");
    handles.header("Accept-Encoding: identity");
    handles.header("Expect:");
    handles.header("Connection: close");
    if (!encoded.billing_challenge.empty())
        handles.header("X-WLS-Billing-Challenge: " + encoded.billing_challenge);
    handles.header("Authorization: Bearer " + std::string(bearer_token));

    auto* easy = handles.easy;
    option(easy, CURLOPT_URL, encoded.url.c_str());
    option(easy, CURLOPT_PROTOCOLS_STR, "https");
    option(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    option(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    option(easy, CURLOPT_HTTP09_ALLOWED, 0L);
    option(easy, CURLOPT_PROXY, "");
    option(easy, CURLOPT_PRE_PROXY, "");
    option(easy, CURLOPT_NOPROXY, "*");
    option(easy, CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    option(easy, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE));
    option(easy, CURLOPT_FOLLOWLOCATION, 0L);
    option(easy, CURLOPT_MAXREDIRS, 0L);
    option(easy, CURLOPT_UNRESTRICTED_AUTH, 0L);
    option(easy, CURLOPT_FRESH_CONNECT, 1L);
    option(easy, CURLOPT_FORBID_REUSE, 1L);
    option(easy, CURLOPT_NOSIGNAL, 1L);
    option(easy, CURLOPT_VERBOSE, 0L);
    option(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    option(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    option(easy, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    option(easy, CURLOPT_CAINFO, static_cast<const char*>(nullptr));
    option(easy, CURLOPT_CAPATH, static_cast<const char*>(nullptr));
    curl_blob trust{const_cast<char*>(options.ca_bundle_pem.data()), options.ca_bundle_pem.size(), CURL_BLOB_COPY};
    option(easy, CURLOPT_CAINFO_BLOB, &trust);
    option(easy, CURLOPT_HTTP_TRANSFER_DECODING, 1L);
    option(easy, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    option(easy, CURLOPT_FAILONERROR, 0L);
    option(easy, CURLOPT_POST, 1L);
    option(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(encoded.body.size()));
    option(easy, CURLOPT_HTTPHEADER, handles.headers);
    option(easy, CURLOPT_UPLOAD_BUFFERSIZE, 16L * 1024L);
    option(easy, CURLOPT_READFUNCTION, &supply_body);
    option(easy, CURLOPT_READDATA, &transfer);
    option(easy, CURLOPT_SEEKFUNCTION, &refuse_seek);
    option(easy, CURLOPT_HEADERFUNCTION, &receive_header);
    option(easy, CURLOPT_HEADERDATA, &transfer);
    option(easy, CURLOPT_WRITEFUNCTION, &receive_body);
    option(easy, CURLOPT_WRITEDATA, &transfer);
    option(easy, CURLOPT_PREREQFUNCTION, &prerequisite);
    option(easy, CURLOPT_PREREQDATA, &transfer);
    option(easy, CURLOPT_SOCKOPTFUNCTION, &socket_options);
    option(easy, CURLOPT_SOCKOPTDATA, &transfer);
    option(easy, CURLOPT_XFERINFOFUNCTION, &progress);
    option(easy, CURLOPT_XFERINFODATA, &transfer);
    option(easy, CURLOPT_NOPROGRESS, 0L);
    if (!options.resolved_ipv4.empty() && !endpoint.numeric) {
        handles.resolve(endpoint.host + ":" + endpoint.port + ":" + options.resolved_ipv4);
        option(easy, CURLOPT_RESOLVE, handles.resolution);
    } else if (options.network.enabled && !endpoint.numeric && !target_address.empty()) {
        // Exact host:port pin for the resolved address; no shared DNS cache
        // survives this call and no alternate address is retried.
        option(easy, CURLOPT_DNS_CACHE_TIMEOUT, 0L);
        handles.resolve(endpoint.host + ":" + endpoint.port + ":" + target_address);
        option(easy, CURLOPT_RESOLVE, handles.resolution);
    }
    if (options.network.proxy.enabled) {
        const auto& proxy = options.network.proxy;
        const auto authority = std::string(proxy.tls ? "https://" : "http://") + proxy.host +
            ":" + std::to_string(proxy.port);
        option(easy, CURLOPT_PROXY, authority.c_str()); // Original name keeps proxy SNI.
        option(easy, CURLOPT_PROXYTYPE, static_cast<long>(CURLPROXY_HTTP));
        option(easy, CURLOPT_HTTPPROXYTUNNEL, 1L);
        option(easy, CURLOPT_NOPROXY, static_cast<const char*>(nullptr));
        if (!strict_ipv4_spelling(proxy.host)) {
            handles.resolve(proxy.host + ":" + std::to_string(proxy.port) + ":" + proxy_address);
            option(easy, CURLOPT_RESOLVE, handles.resolution);
        }
        // CONNECT_TO pins the tunnel destination to the helper-selected
        // numeric address while URL, Host, SNI and origin verification keep
        // using the original frozen origin host:port.
        const auto tunnel = "+" + endpoint.host + ":" + endpoint.port + ":" + target_address +
            ":" + endpoint.port;
        handles.connect_to = curl_slist_append(nullptr, tunnel.c_str());
        require(handles.connect_to != nullptr, ErrorCode::Storage,
                "HTTPS CONNECT destination allocation failed");
        option(easy, CURLOPT_CONNECT_TO, handles.connect_to);
        if (proxy.tls) {
            option(easy, CURLOPT_PROXY_SSL_VERIFYPEER, 1L);
            option(easy, CURLOPT_PROXY_SSL_VERIFYHOST, 2L);
            option(easy, CURLOPT_PROXY_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
            option(easy, CURLOPT_PROXY_CAINFO, static_cast<const char*>(nullptr));
            curl_blob proxy_trust{const_cast<char*>(proxy.ca_bundle_pem.data()),
                proxy.ca_bundle_pem.size(), CURL_BLOB_COPY};
            option(easy, CURLOPT_PROXY_CAINFO_BLOB, &proxy_trust);
        }
        if (proxy.basic_auth) {
            // One explicit Basic pair; no negotiation and never a retry.
            option(easy, CURLOPT_PROXYAUTH, static_cast<long>(CURLAUTH_BASIC));
            const std::string username(proxy_basic_username);
            const std::string password(proxy_basic_password);
            option(easy, CURLOPT_PROXYUSERNAME, username.c_str());
            option(easy, CURLOPT_PROXYPASSWORD, password.c_str());
        } else {
            option(easy, CURLOPT_PROXYAUTH, static_cast<long>(CURLAUTH_NONE));
        }
        transfer.proxy_tunnel = true;
    }

    if (transfer.interrupted()) {
        classify_abort(result, transfer.aborted, CURLE_ABORTED_BY_CALLBACK);
        finalize(result, transfer);
        return result;
    }
    const auto remaining = std::chrono::ceil<Milliseconds>(transfer.deadline - Clock::now());
    const auto timeout_ms = static_cast<long>(std::max<std::int64_t>(1, remaining.count()));
    option(easy, CURLOPT_TIMEOUT_MS, timeout_ms);
    option(easy, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    require(curl_multi_setopt(handles.multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 1L) == CURLM_OK &&
            curl_multi_setopt(handles.multi, CURLMOPT_MAX_HOST_CONNECTIONS, 1L) == CURLM_OK,
            ErrorCode::Unsupported, "required HTTPS connection limits are unavailable");
    require(curl_multi_add_handle(handles.multi, easy) == CURLM_OK, ErrorCode::Storage,
            "HTTPS transport could not attach its one request");
    handles.attached = true;

    CURLcode outcome = CURLE_ABORTED_BY_CALLBACK;
    bool done = false;
    int running = 0;
    while (!transfer.interrupted()) {
        result.network_started = true;
        transfer.advance(HttpsTransportPhase::Connecting);
        const auto performed = curl_multi_perform(handles.multi, &running);
        collect_observations(easy, transfer);
        if (performed != CURLM_OK) {
            transfer.aborted = Abort::CallbackFailure;
            break;
        }
        int messages = 0;
        while (auto* message = curl_multi_info_read(handles.multi, &messages)) {
            if (message->msg == CURLMSG_DONE && message->easy_handle == easy) {
                outcome = message->data.result;
                done = true;
            }
        }
        if (transfer.interrupted() || done) break;
        if (running == 0) {
            transfer.aborted = Abort::CallbackFailure;
            break;
        }
        const auto wait_left = std::chrono::ceil<Milliseconds>(transfer.deadline - Clock::now());
        const auto wait_ms = static_cast<int>(std::max<std::int64_t>(0,
                std::min(transfer.options.poll_interval.count(), wait_left.count())));
        int descriptors = 0;
        if (curl_multi_poll(handles.multi, nullptr, 0, wait_ms, &descriptors) != CURLM_OK) {
            transfer.aborted = Abort::CallbackFailure;
            break;
        }
    }
    collect_observations(easy, transfer);
    // Detaching closes an interrupted transfer before protocol work. It cannot
    // authorize a retry or turn absent response bytes into proof of no billing.
    handles.close();
    result.transfer_complete = done && outcome == CURLE_OK && transfer.aborted == Abort::None &&
                               transfer.header.final && transfer.header.completed &&
                               (!transfer.proxy_tunnel || transfer.proxy_established);
    decode_retained(transfer, request);
    if (transfer.interrupted() || !done || outcome != CURLE_OK) {
        classify_abort(result, transfer.aborted, outcome);
    } else if (!result.transfer_complete) {
        if (transfer.proxy_tunnel && !transfer.proxy_established)
            fail(result, HttpsTransportStatus::ConnectionError, "proxy_connect_refused");
        else
            fail(result, HttpsTransportStatus::ProtocolError, "incomplete_http_framing");
    } else if (result.http_status != 200) {
        fail(result, HttpsTransportStatus::HttpError, "http_error");
    } else if (result.response.status != ModelResultStatus::Completed) {
        fail(result, HttpsTransportStatus::ProtocolError, "protocol_error");
    } else {
        result.status = HttpsTransportStatus::Completed;
        result.reason = "completed";
    }
    finalize(result, transfer);
    return result;
}

Json https_transport_summary(const HttpsTransportResult& result) {
    auto summary = summary_record(result);
    require(result.record_sha256 == sha256_hex(canonical_json(summary)), ErrorCode::Corrupt,
            "HTTPS report changed after its consistency fingerprint was recorded");
    summary["record_sha256"] = result.record_sha256;
    return summary;
}

} // namespace wls::s1
