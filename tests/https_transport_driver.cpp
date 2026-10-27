// Test-only fixture runner. It is intentionally not a production live CLI:
// endpoint hosts are restricted to loopback, credentials are synthetic, and it
// has no Store, model admission, tool execution or environment-secret lookup.
#include "wls/https_transport.hpp"
#include "protocol_test_support.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace {
using namespace wls::s1;
using Object = boost::json::object;
using namespace std::chrono_literals;
namespace fixture = wls::test::protocol;

std::string read_fixture(const std::filesystem::path& path, std::size_t limit) {
    const auto status = std::filesystem::symlink_status(path);
    if (!std::filesystem::is_regular_file(status) || std::filesystem::file_size(path) > limit)
        throw std::runtime_error("invalid test fixture file");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open test fixture");
    std::string bytes(limit + 1U, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = input.gcount();
    if (count < 0 || static_cast<std::size_t>(count) > limit || !input.eof())
        throw std::runtime_error("test fixture read was not complete within its bound");
    bytes.resize(static_cast<std::size_t>(count));
    return bytes;
}

std::int64_t integer(const Object& object, std::string_view key, std::int64_t fallback) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) return fallback;
    if (!value->is_int64()) throw std::runtime_error("test integer required");
    return value->as_int64();
}

std::size_t size(const Object& object, std::string_view key, std::size_t fallback) {
    const auto value = integer(object, key, static_cast<std::int64_t>(fallback));
    if (value < 0 || value > 16 * 1024 * 1024)
        throw std::runtime_error("test size outside fixture range");
    return static_cast<std::size_t>(value);
}

std::string text(const Object& object, std::string_view key, std::string fallback = {}) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) return fallback;
    if (!value->is_string()) throw std::runtime_error("test text required");
    return std::string(value->as_string());
}

bool boolean(const Object& object, std::string_view key, bool fallback = false) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) return fallback;
    if (!value->is_bool()) throw std::runtime_error("test boolean required");
    return value->as_bool();
}

Json micros(const std::optional<std::chrono::microseconds>& value) {
    return value ? Json(value->count()) : Json(nullptr);
}

std::string hex(std::string_view bytes) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const char value : bytes) {
        const auto byte = static_cast<unsigned char>(value);
        result += alphabet[byte >> 4U];
        result += alphabet[byte & 15U];
    }
    return result;
}

// Fixed spellings make the integration assertions independent of serializer
// implementation details and keep enum additions visible during compilation.
std::string status_name(HttpsTransportStatus status) {
    switch (status) {
    case HttpsTransportStatus::Completed: return "completed";
    case HttpsTransportStatus::HttpError: return "http_error";
    case HttpsTransportStatus::Cancelled: return "cancelled";
    case HttpsTransportStatus::DeadlineExceeded: return "deadline";
    case HttpsTransportStatus::CertificateError: return "certificate_error";
    case HttpsTransportStatus::ConnectionError: return "connection_error";
    case HttpsTransportStatus::TransferError: return "transfer_error";
    case HttpsTransportStatus::LimitExceeded: return "limit";
    case HttpsTransportStatus::ProtocolError: return "protocol_error";
    }
    throw std::runtime_error("unrecognized transport status");
}

std::string phase_name(HttpsTransportPhase phase) {
    switch (phase) {
    case HttpsTransportPhase::NotStarted: return "not_started";
    case HttpsTransportPhase::Connecting: return "connecting";
    case HttpsTransportPhase::TlsHandshake: return "tls_handshake";
    case HttpsTransportPhase::RequestWrite: return "request_write";
    case HttpsTransportPhase::ResponseHeaders: return "response_headers";
    case HttpsTransportPhase::ResponseBody: return "response_body";
    }
    throw std::runtime_error("unrecognized transport phase");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    bool cares_resolver = false;
    try {
        const auto configuration = parse_json_strict(read_fixture(argv[1], 16384));
        const auto& config = configuration.as_object();
        // A read-only feature query on the actual linked library; no global
        // initialization, TLS session, credential lookup or name resolution.
        const auto* curl_version = curl_version_info(CURLVERSION_NOW);
        cares_resolver = curl_version != nullptr && curl_version->ares != nullptr &&
                         *curl_version->ares != '\0';
        const auto host = text(config, "host", "127.0.0.1");
        const auto port = integer(config, "port", 0);
        if ((host != "127.0.0.1" && host != "localhost") || port < 1 || port > 65535)
            throw std::runtime_error("test transport endpoint must be loopback");

        auto profile = fixture::profile();
        profile.base_url = "https://" + host + ":" + std::to_string(port);
        profile.limits.max_response_bytes = size(config, "max_body_bytes", profile.limits.max_response_bytes);
        profile.limits.max_stream_bytes = size(config, "max_stream_bytes", profile.limits.max_stream_bytes);
        profile.limits.max_sse_line_bytes = size(config, "max_sse_line_bytes", profile.limits.max_sse_line_bytes);
        profile.limits.max_sse_event_bytes = size(config, "max_sse_event_bytes", profile.limits.max_sse_event_bytes);
        profile.limits.max_sse_events = size(config, "max_sse_events", profile.limits.max_sse_events);
        profile.limits.max_sse_chunks = size(config, "max_sse_chunks", profile.limits.max_sse_chunks);
        profile.limits.max_text_bytes = size(config, "max_text_bytes", profile.limits.max_text_bytes);
        // Existing request restoration still enforces its original 1 MiB
        // context bound; this only makes room for the captured envelope.
        profile.limits.max_request_bytes = 4U * 1024U * 1024U;

        auto request = fixture::request();
        request.timeout = std::chrono::milliseconds(integer(config, "timeout_ms", 3000));
        const auto large = size(config, "request_padding_bytes", 0);
        if (large != 0) {
            auto context = fixture::context();
            context.task_contract += std::string(large, 'p');
            request.context = compile_structured_context(context, {});
        }
        const bool stream = boolean(config, "stream");
        auto encoded = encode_responses_request(profile, request, stream);
        const auto original_body_size = encoded.body.size();
        const auto original_encoded_hash = encoded.record_sha256;
        const auto original_body_hash = encoded.body_sha256;
        const auto original_profile_hash = encoded.profile_sha256;
        const auto original_logical_hash = encoded.logical_request_sha256;

        HttpsTransportOptions options;
        options.ca_bundle_pem = read_fixture(text(config, "ca_path"), 1024U * 1024U);
        if (boolean(config, "empty_trust")) options.ca_bundle_pem.clear();
        const auto route = text(config, "route_kind", host == "localhost" ? "loopback" : "none");
        if (route == "loopback") options.resolved_ipv4 = "127.0.0.1";
        else if (route == "inconsistent") options.resolved_ipv4 = "127.0.0.2";
        else if (route == "invalid") options.resolved_ipv4 = "not-an-ip";
        else if (route == "newline") options.resolved_ipv4 = "127.0.0.1\n";
        else if (route != "none") throw std::runtime_error("unknown test route variant");
        options.max_header_bytes = size(config, "max_header_bytes", options.max_header_bytes);
        options.max_header_line_bytes = size(config, "max_header_line_bytes", options.max_header_line_bytes);
        options.max_total_response_bytes = size(config, "max_total_bytes", options.max_total_response_bytes);
        options.poll_interval = std::chrono::milliseconds(integer(config, "poll_ms", 25));

        const auto mutation = text(config, "mutation");
        if (mutation == "body") encoded.body += " ";
        else if (mutation == "url") encoded.url += "/different";
        else if (mutation == "record") encoded.record_sha256.assign(64U, '0');
        else if (mutation == "profile") profile.model = "different-synthetic-model";
        else if (mutation == "request") ++request.binding.task_revision;
        else if (mutation == "method") encoded.method = "GET";
        else if (!mutation.empty()) throw std::runtime_error("unknown test mutation");

        std::string bearer = "WLS_SYNTHETIC_BEARER_ONLY_31A7";
        const auto credential = text(config, "credential_kind");
        if (credential == "newline") bearer += "\r\nInjected: value";
        else if (credential == "nul") bearer.push_back('\0');
        else if (credential == "empty") bearer.clear();
        else if (credential == "nonascii") bearer += "秘密";
        else if (credential == "oversized") bearer.assign(65536U, 's');
        else if (!credential.empty()) throw std::runtime_error("unknown test credential variant");

        std::stop_source cancellation;
        if (boolean(config, "pre_cancel")) cancellation.request_stop();
        const auto cancel_after = integer(config, "cancel_after_ms", -1);
        const auto marker = text(config, "cancel_marker");
        std::jthread interruptor;
        if (cancel_after >= 0 || !marker.empty()) {
            interruptor = std::jthread([&, cancel_after, marker](std::stop_token done) {
                const auto started = std::chrono::steady_clock::now();
                while (!done.stop_requested()) {
                    const bool expired = cancel_after >= 0 && std::chrono::steady_clock::now() - started >=
                        std::chrono::milliseconds(cancel_after);
                    if (expired || (!marker.empty() && std::filesystem::exists(marker))) {
                        cancellation.request_stop();
                        return;
                    }
                    std::this_thread::sleep_for(2ms);
                }
            });
        }

        const auto result = perform_responses_https(profile, request, encoded, options, bearer,
                                                    cancellation.get_token());
        interruptor.request_stop();
        if (interruptor.joinable()) interruptor.join();
        Object integrity_checks;
        if (boolean(config, "verify_summary_integrity")) {
            const auto rejected = [](const HttpsTransportResult& changed) {
                try {
                    (void)https_transport_summary(changed);
                } catch (const Error& error) {
                    return error.code() == ErrorCode::Corrupt;
                }
                return false;
            };
            auto changed = result;
            changed.raw_body.push_back('x');
            integrity_checks["raw_body"] = rejected(changed);
            changed = result;
            changed.response.text += " altered fixture output";
            integrity_checks["decoded_response"] = rejected(changed);
            changed = result;
            changed.transport_options_sha256.assign(64U, '0');
            integrity_checks["transport_options"] = rejected(changed);
        }
        const Object report{
            {"cares_resolver", cares_resolver}, {"integrity_checks", integrity_checks},
            {"status", status_name(result.status)}, {"phase", phase_name(result.phase)},
            {"reason", result.reason}, {"http_status", result.http_status},
            {"network_started", result.network_started}, {"transfer_complete", result.transfer_complete},
            {"raw_headers_truncated", result.raw_headers_truncated},
            {"raw_body_truncated", result.raw_body_truncated},
            {"upload_may_have_occurred", result.upload_may_have_occurred},
            {"header_bytes_observed", result.header_bytes_observed},
            {"body_bytes_observed", result.body_bytes_observed},
            {"request_body_bytes_supplied", result.request_body_bytes_supplied},
            {"uploaded_body_bytes_reported", result.uploaded_body_bytes_reported},
            {"elapsed_us", result.elapsed.count()}, {"connected_at_us", micros(result.connected_at)},
            {"tls_verified_at_us", micros(result.tls_verified_at)},
            {"first_header_at_us", micros(result.first_header_at)},
            {"first_body_at_us", micros(result.first_body_at)},
            {"raw_headers_hex", hex(result.raw_headers)}, {"raw_body_hex", hex(result.raw_body)},
            {"record_sha256", result.record_sha256}, {"summary", https_transport_summary(result)},
            {"response", to_json(result.response)}, {"request_body_size", original_body_size},
            {"encoded_record_sha256", original_encoded_hash}, {"request_body_sha256", original_body_hash},
            {"profile_sha256", original_profile_hash}, {"logical_request_sha256", original_logical_hash},
            {"ca_bundle_sha256", sha256_hex(options.ca_bundle_pem)},
            {"transport_options_sha256", result.transport_options_sha256},
            {"expected_binding", to_json(request.binding)}, {"expected_call_id", request.call_id},
            {"expected_attempt_id", request.attempt_id}, {"expected_config_snapshot_id", request.config_snapshot_id}
        };
        std::cout << canonical_json(report) << '\n';
        return 0;
    } catch (const Error& error) {
        // Never echo arbitrary provider/input/credential diagnostics.
        std::cout << canonical_json(Object{{"threw", true}, {"error", error_name(error.code())},
                                          {"cares_resolver", cares_resolver}}) << '\n';
        return 0;
    } catch (const std::exception&) {
        std::cerr << "fixture driver failed\n";
        return 2;
    }
}
