#include "wls/live_model.hpp"

#include "wls/config_loader.hpp"
#include "wls/store.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace wls::s1 {
namespace {
using Object = boost::json::object;

void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
const Object& object(const Json& value) {
    require(value.is_object(), ErrorCode::InvalidInput, "live model configuration must be an object");
    return value.as_object();
}
const Json& field(const Json& value, std::string_view key) {
    const auto* found = object(value).if_contains(key);
    require(found != nullptr, ErrorCode::InvalidInput, "missing live model configuration field");
    return *found;
}
std::string text(const Json& value, std::string_view key, std::size_t maximum = 1024) {
    const auto& item = field(value, key);
    require(item.is_string() && item.as_string().size() <= maximum &&
            item.as_string().find('\0') == boost::json::string::npos,
            ErrorCode::InvalidInput, "invalid bounded live model text");
    return std::string(item.as_string());
}
std::int64_t number(const Json& value, std::string_view key) {
    const auto& item = field(value, key);
    if (item.is_int64()) return item.as_int64();
    if (item.is_uint64() && item.as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(item.as_uint64());
    throw Error(ErrorCode::InvalidInput, "live model configuration requires a bounded integer");
}
bool boolean(const Json& value, std::string_view key) {
    const auto& item = field(value, key);
    require(item.is_bool(), ErrorCode::InvalidInput, "live model configuration requires a boolean");
    return item.as_bool();
}
void exact(const Json& value, const std::set<std::string>& keys) {
    require(object(value).size() == keys.size(), ErrorCode::InvalidInput, "missing or extra live model fields");
    for (const auto& entry : object(value))
        require(keys.contains(std::string(entry.key())), ErrorCode::InvalidInput, "unknown live model field");
}
bool hash(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool bounded_text(const std::string& value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum && value.find('\0') == std::string::npos &&
        std::all_of(value.begin(), value.end(), [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return byte >= 32U && byte != 127U;
        });
}
void validate_transport(const HttpsTransportOptions& options) {
    require(!options.ca_bundle_pem.empty() && options.ca_bundle_pem.size() <= 1024U * 1024U &&
            options.ca_bundle_pem.find('\0') == std::string::npos &&
            options.max_header_bytes > 0 && options.max_header_bytes <= 1024U * 1024U &&
            options.max_header_line_bytes > 0 && options.max_header_line_bytes <= 64U * 1024U &&
            options.max_total_response_bytes > 0 && options.max_total_response_bytes <= 32U * 1024U * 1024U &&
            options.poll_interval.count() > 0 && options.poll_interval.count() <= 25,
            ErrorCode::InvalidInput, "live HTTPS configuration is outside hard bounds");
    // The transport validates the numeric route again immediately before use.
    require(options.resolved_ipv4.size() <= 15 &&
        std::all_of(options.resolved_ipv4.begin(), options.resolved_ipv4.end(), [](char c) {
            return (c >= '0' && c <= '9') || c == '.';
        }), ErrorCode::InvalidInput, "live HTTPS route must be a bounded IPv4 spelling");
}
std::string endpoint_host(const ProviderProfile& provider) {
    auto authority = std::string_view(provider.base_url).substr(8);
    authority = authority.substr(0, authority.find('/'));
    return std::string(authority.substr(0, authority.find(':')));
}
} // namespace

Json live_transport_options_json(const HttpsTransportOptions& options) {
    validate_transport(options);
    return Object{{"ca_bundle_pem", options.ca_bundle_pem}, {"resolved_ipv4", options.resolved_ipv4},
        {"max_header_bytes", options.max_header_bytes}, {"max_header_line_bytes", options.max_header_line_bytes},
        {"max_total_response_bytes", options.max_total_response_bytes},
        {"poll_interval_ms", options.poll_interval.count()}};
}

std::string live_transport_options_sha256(const HttpsTransportOptions& options) {
    validate_transport(options);
    // The versioned transport observation uses this exact descriptor. Tests
    // compare the helper to an executed local TLS result to detect drift.
    return sha256_hex(canonical_json(Object{{"format", "wls-https-transport-options-v1"},
        {"ca_bundle_sha256", sha256_hex(options.ca_bundle_pem)}, {"resolved_ipv4", options.resolved_ipv4},
        {"max_header_bytes", options.max_header_bytes}, {"max_header_line_bytes", options.max_header_line_bytes},
        {"max_total_response_bytes", options.max_total_response_bytes},
        {"poll_interval_ms", options.poll_interval.count()}, {"socket_send_buffer_bytes", 64 * 1024},
        {"upload_buffer_bytes", 16U * 1024U}, {"dns_policy", "strict_ipv4_or_explicit_mapping_or_cares"}}));
}

Json live_model_settings_json(const LiveModelSettings& settings) {
    return Object{{"provider_profile", to_json(settings.provider)},
        {"transport", live_transport_options_json(settings.transport)}, {"stream", settings.stream},
        {"execution_kind", settings.execution_kind},
        {"capability_evidence_source", settings.capability_evidence_source},
        {"capability_evidence_sha256", settings.capability_evidence_sha256}};
}

LiveModelSettings live_model_settings_from_json(const Json& profile, std::string_view model_ref) {
    exact(profile, {"adapter", "adapter_version", "protocol", "model", "capabilities", "responses"});
    require(text(profile, "adapter") == "openai-responses" && text(profile, "adapter_version") == "1" &&
            text(profile, "protocol") == "openai-responses-v1", ErrorCode::Unsupported,
            "unsupported live model adapter or protocol");
    const auto& source = field(profile, "responses");
    exact(source, {"provider_profile", "transport", "stream", "execution_kind",
                   "capability_evidence_source", "capability_evidence_sha256"});
    LiveModelSettings settings;
    settings.provider = provider_profile_from_json(field(source, "provider_profile"));
    require(settings.provider.model_ref == model_ref && settings.provider.model == text(profile, "model"),
            ErrorCode::Conflict, "live model registration differs from its embedded provider profile");
    const auto& transport = field(source, "transport");
    exact(transport, {"ca_bundle_pem", "resolved_ipv4", "max_header_bytes", "max_header_line_bytes",
                      "max_total_response_bytes", "poll_interval_ms"});
    settings.transport.ca_bundle_pem = text(transport, "ca_bundle_pem", 1024U * 1024U);
    settings.transport.resolved_ipv4 = text(transport, "resolved_ipv4", 15);
    const auto size = [&](std::string_view key) {
        const auto value = number(transport, key);
        require(value > 0, ErrorCode::InvalidInput, "live HTTPS byte limits must be positive");
        return static_cast<std::size_t>(value);
    };
    settings.transport.max_header_bytes = size("max_header_bytes");
    settings.transport.max_header_line_bytes = size("max_header_line_bytes");
    settings.transport.max_total_response_bytes = size("max_total_response_bytes");
    settings.transport.poll_interval = std::chrono::milliseconds(number(transport, "poll_interval_ms"));
    validate_transport(settings.transport);
    // This Store integration has a narrower envelope than the independent
    // offline/transport components. Reserve durable space before any send:
    // at most 6 MiB of raw hex, bounded decoded output and bounded SSE/HTTP
    // evidence fit the 12 MiB combined row and 4 MiB individual JSON limits.
    const auto& limits = settings.provider.limits;
    require(settings.transport.max_total_response_bytes <= 3U * 1024U * 1024U &&
            settings.transport.max_header_bytes <= 64U * 1024U &&
            settings.transport.max_header_line_bytes <= 8U * 1024U &&
            limits.max_response_bytes <= 1024U * 1024U &&
            limits.max_stream_bytes <= 4U * 1024U * 1024U &&
            limits.max_text_bytes <= 256U * 1024U && limits.max_tool_argument_bytes <= 256U * 1024U &&
            limits.max_output_items <= 128 && limits.max_tool_calls <= 32 &&
            limits.max_sse_line_bytes <= 256U * 1024U && limits.max_sse_event_bytes <= 1024U * 1024U &&
            limits.max_sse_events <= 4096 && limits.max_sse_chunks <= 16384,
            ErrorCode::InvalidInput, "live response bounds exceed durable Store evidence capacity");
    settings.stream = boolean(source, "stream");
    require(!settings.stream || settings.provider.streaming, ErrorCode::Unsupported,
            "streaming was not declared by the frozen provider");
    settings.execution_kind = text(source, "execution_kind");
    require(settings.execution_kind == "local_tls" || settings.execution_kind == "provider",
            ErrorCode::InvalidInput, "live execution kind must be explicit");
    if (settings.execution_kind == "local_tls") {
        const auto host = endpoint_host(settings.provider);
        require((host == "127.0.0.1" && (settings.transport.resolved_ipv4.empty() ||
                                        settings.transport.resolved_ipv4 == "127.0.0.1")) ||
                (host == "localhost" && settings.transport.resolved_ipv4 == "127.0.0.1"),
                ErrorCode::Unauthorized, "local TLS profiles must use an explicit loopback destination");
    }
    settings.capability_evidence_source = text(source, "capability_evidence_source");
    settings.capability_evidence_sha256 = text(source, "capability_evidence_sha256", 64);
    require(bounded_text(settings.capability_evidence_source, 1024) && hash(settings.capability_evidence_sha256),
            ErrorCode::InvalidInput, "live profile requires an explicit capability evidence reference and hash");
    const auto& capabilities = field(profile, "capabilities");
    for (const auto& [key, supported] : {std::pair<std::string_view, bool>{"streaming", settings.provider.streaming},
            {"tool_calling", settings.provider.tool_calling}, {"parallel_tool_calls", settings.provider.parallel_tool_calls}}) {
        const auto declared = text(capabilities, key);
        require((declared == "supported" || declared == "unsupported" || declared == "unknown") &&
                (declared == "supported") == supported, ErrorCode::Conflict,
                "live capability declarations differ from the embedded provider profile");
    }
    require(number(capabilities, "context_window") == settings.provider.context_window &&
            number(capabilities, "max_output_tokens") == settings.provider.max_output_tokens,
            ErrorCode::Conflict, "live model token capabilities differ from provider profile");
    const auto& modes = field(capabilities, "reasoning_modes");
    require(modes.is_array(), ErrorCode::InvalidInput, "live reasoning modes must be an array");
    std::set<std::string> declared_modes;
    for (const auto& mode : modes.as_array()) {
        require(mode.is_string(), ErrorCode::InvalidInput, "invalid live reasoning mode");
        require(declared_modes.insert(std::string(mode.as_string())).second,
                ErrorCode::InvalidInput, "duplicate live reasoning mode");
    }
    std::set<std::string> expected_modes;
    if (settings.provider.reasoning_provider_default) expected_modes.insert("provider_default");
    if (settings.provider.reasoning_off) expected_modes.insert("off");
    if (!settings.provider.reasoning_efforts.empty()) expected_modes.insert("effort");
    require(declared_modes == expected_modes, ErrorCode::Conflict, "live reasoning modes differ from provider profile");
    std::set<std::string> declared_efforts;
    if (const auto* efforts = object(capabilities).if_contains("reasoning_efforts")) {
        require(efforts->is_array(), ErrorCode::InvalidInput, "live reasoning efforts must be an array");
        for (const auto& effort : efforts->as_array()) {
            require(effort.is_string() && declared_efforts.insert(std::string(effort.as_string())).second,
                    ErrorCode::InvalidInput, "invalid or duplicate live reasoning effort");
        }
    }
    require(declared_efforts == settings.provider.reasoning_efforts, ErrorCode::Conflict,
            "live reasoning effort declarations differ from provider profile");
    return settings;
}

ObservedLiveAttempt observe_live_attempt(Store& store, const StoreActor& actor,
        const ModelRequest& request, std::string_view bearer_token,
        const LiveExecutionAuthorization& authorization,
        const LiveExperienceBinding& experience, std::stop_token stop) {
    require(authorization.network_enabled, ErrorCode::Unauthorized, "live network execution has no host authorization");
    require(!bearer_token.empty() && bearer_token.size() <= 8192 &&
            std::all_of(bearer_token.begin(), bearer_token.end(), [](char c) {
                const auto byte = static_cast<unsigned char>(c); return byte >= 33U && byte <= 126U;
            }), ErrorCode::InvalidInput, "invalid bounded live credential");
    const auto persisted_request = store.request(request.binding.request_id);
    const auto config = FrozenRequestConfig::restore(canonical_json(persisted_request.at("config_snapshot")),
                                                    request.config_snapshot_id);
    const auto settings = live_model_settings_from_json(config.model_profile(request.role), request.model.model);
    require(settings.execution_kind == "local_tls" || authorization.provider_enabled,
            ErrorCode::Unauthorized, "provider network execution has no explicit host authorization");
    const auto encoded = encode_responses_request(settings.provider, request, settings.stream);
    AttemptReservation reservation;
    reservation.call_id = request.call_id; reservation.attempt_id = request.attempt_id;
    reservation.config_snapshot_sha256 = request.config_snapshot_id;
    reservation.model_ref = request.model.model; reservation.adapter_kind = "openai-responses";
    reservation.reserved_input_tokens = request.reserved_input_tokens;
    reservation.reserved_output_tokens = request.reserved_output_tokens;
    reservation.reserved_cost_micro_usd = request.reserved_cost_micro_usd;
    reservation.max_output_tokens = request.max_output_tokens;
    reservation.request_envelope = to_json(request);
    reservation.experience_job_id = experience.experience_job_id;
    reservation.experience_job_version = experience.experience_job_version;
    reservation.experience_source_sha256 = experience.experience_source_sha256;
    ObservedLiveAttempt result;
    result.reservation = store.reserve_attempt(actor, request.binding, reservation);
    result.send_claim = store.claim_live_send(actor, request.attempt_id,
        result.reservation.at("state_version").as_int64(), encoded, settings.transport,
        "send:" + request.attempt_id);
    if (!result.send_claim.at("new_send_authority").as_bool()) return result;
    const auto transport = perform_responses_https(settings.provider, request, encoded,
                                                   settings.transport, bearer_token, stop);
    result.sent = transport.network_started;
    result.transport_summary = https_transport_summary(transport);
    (void)store.record_live_transport(request.attempt_id,
        std::string(result.send_claim.at("claim_id").as_string()), transport);
    result.response = transport.response;
    return result;
}

LiveAttemptResult execute_live_attempt(Store& store, const StoreActor& actor,
        const ModelRequest& request, std::string_view bearer_token,
        const LiveExecutionAuthorization& authorization, std::stop_token stop) {
    auto observed = observe_live_attempt(store, actor, request, bearer_token, authorization, {}, stop);
    LiveAttemptResult result;
    result.reservation = std::move(observed.reservation);
    result.send_claim = std::move(observed.send_claim);
    result.transport_summary = std::move(observed.transport_summary);
    result.sent = observed.sent;
    if (!observed.response) return result;
    const auto& response = *observed.response;
    const auto outcome = response.status == ModelResultStatus::Completed ? "COMPLETED" :
        response.status == ModelResultStatus::Interrupted ? "INTERRUPTED" : "ERROR";
    result.settlement = store.settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
                                            outcome, to_json(response.usage), to_json(response));
    result.response_accepted = result.settlement.at("accepted").as_int64() == 1;
    return result;
}

VerifiedUsageEvidence verify_usage_evidence(std::string source_kind, std::string source_reference,
        std::string bytes, const UsageEvidenceVerifier& verifier) {
    require((source_kind == "loopback_fixture" || source_kind == "host_verified_provider" || source_kind == "host_no_dispatch") &&
            bounded_text(source_reference, 1024) && !bytes.empty() && bytes.size() <= 256U * 1024U &&
            static_cast<bool>(verifier), ErrorCode::InvalidInput,
            "usage reconciliation requires bounded source evidence and a host verifier");
    auto statement = verifier(bytes);
    require(bounded_text(statement.attempt_id, 512) && hash(statement.settlement_sha256),
            ErrorCode::InvalidInput, "usage verifier did not bind an original attempt and settlement");
    (void)model_usage_from_json(to_json(statement.usage));
    VerifiedUsageEvidence result;
    result.statement_ = std::move(statement); result.source_kind_ = std::move(source_kind);
    result.source_reference_ = std::move(source_reference); result.sha256_ = sha256_hex(bytes);
    result.bytes_ = std::move(bytes);
    return result;
}

} // namespace wls::s1
