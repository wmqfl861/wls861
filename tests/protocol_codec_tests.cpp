#include "protocol_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>

namespace {
using namespace wls;
using namespace wls::s1;
namespace fixture = wls::test::protocol;
using Object = boost::json::object;
using Array = boost::json::array;

void fails(ErrorCode expected, const std::function<void()>& action) {
    try { action(); } catch (const Error& error) {
        if (error.code() != expected) throw std::runtime_error(error.what());
        return;
    }
    throw std::runtime_error("expected codec refusal");
}
DecodedResponsesResponse decode(const Json& value, const ProviderProfile& p = fixture::profile(),
                                const ModelRequest& r = fixture::request()) {
    return decode_responses_response(p, r, 200, canonical_json(value));
}
void refused(const Json& value, const std::string& code = "invalid_response") {
    const auto result = decode(value);
    CHECK(result.response.status == ModelResultStatus::Error);
    CHECK(result.response.error_code == code);
    CHECK(result.response.tool_calls.empty()); CHECK(result.response.text.empty());
    CHECK(result.raw_body == canonical_json(value));
    CHECK(result.evidence.at("business_accepted") == false);
    CHECK(result.evidence.at("tool_dispatch_performed") == false);
}
ModelRequest with_context(ContextInput c,
                         const std::vector<StructuredContextMessage>& messages = {}) {
    auto r = fixture::request(); r.context = compile_structured_context(c, messages); r.model = c.model;
    return r;
}

void profile_round_trip_and_effective_hash() {
    const auto p = fixture::profile(); const auto j = to_json(p);
    CHECK(to_json(provider_profile_from_json(j)) == j);
    CHECK(provider_profile_sha256(p) == sha256_hex(canonical_json(j)));
    auto reordered = j;
    auto& efforts = reordered.at("capabilities").at("reasoning_efforts").as_array();
    std::reverse(efforts.begin(), efforts.end());
    CHECK(provider_profile_sha256(provider_profile_from_json(reordered)) == provider_profile_sha256(p));
    auto changed = p; changed.base_url = "https://other.example.invalid";
    CHECK(provider_profile_sha256(changed) != provider_profile_sha256(p));
    changed = p; changed.limits.max_sse_events -= 1;
    CHECK(provider_profile_sha256(changed) != provider_profile_sha256(p));
}
void profile_fields_versions_and_paid_refusals() {
    const auto p = to_json(fixture::profile());
    auto j = p; j.as_object().erase("api_key_env");
    fails(ErrorCode::InvalidInput, [&] { (void)provider_profile_from_json(j); });
    j = p; j.as_object()["api_key"] = "test-only-secret";
    fails(ErrorCode::InvalidInput, [&] { (void)provider_profile_from_json(j); });
    for (const auto* key : {"format", "adapter", "adapter_version", "protocol"}) {
        j = p; j.as_object()[key] = "unimplemented";
        fails(ErrorCode::Unsupported, [&] { (void)provider_profile_from_json(j); });
    }
    j = p; j.as_object()["real_calls_enabled"] = true;
    fails(ErrorCode::Unsupported, [&] { (void)provider_profile_from_json(j); });
    for (const auto retries : {-1, 1}) {
        j = p; j.as_object()["max_paid_retries"] = retries;
        fails(ErrorCode::Unsupported, [&] { (void)provider_profile_from_json(j); });
    }
}
void endpoint_and_credential_references_are_explicit() {
    for (const auto* url : {"", "http://gateway.example.invalid", "https://secret@gateway.example.invalid",
            "https://gateway.example.invalid?key=secret", "https://gateway.example.invalid/#fragment",
            "https://gateway.example.invalid/../api", "https://gateway.example.invalid/%2e%2e/api",
            "https://gateway.example.invalid:0", "https://gateway.example.invalid:65536",
            "https://gateway.example.invalid/", "https://-bad.invalid"}) {
        auto p = fixture::profile(); p.base_url = url;
        fails(ErrorCode::InvalidInput, [&] { validate_provider_profile(p); });
    }
    for (const auto* path : {"", "v1/responses", "//other.invalid/responses", "/../responses", "/v1/responses?key=x", "/v1/responses/"}) {
        auto p = fixture::profile(); p.responses_path = path;
        fails(ErrorCode::InvalidInput, [&] { validate_provider_profile(p); });
    }
    auto p = fixture::profile(); p.base_url = "https://gateway.example.invalid:8443/gateway";
    CHECK(encode_responses_request(p, fixture::request()).url == p.base_url + p.responses_path);
    p.api_key_env = "not-an-environment-name";
    fails(ErrorCode::InvalidInput, [&] { validate_provider_profile(p); });
}
void profile_capabilities_and_hard_limits_refuse() {
    const auto original = to_json(fixture::profile());
    auto j = original; j.at("capabilities").as_object()["streaming"] = "unknown";
    fails(ErrorCode::InvalidInput, [&] { (void)provider_profile_from_json(j); });
    j = original; j.at("capabilities").as_object()["tool_calling"] = false;
    fails(ErrorCode::InvalidInput, [&] { (void)provider_profile_from_json(j); });
    j = original; j.at("capabilities").as_object()["reasoning_efforts"] = Array{};
    fails(ErrorCode::InvalidInput, [&] { (void)provider_profile_from_json(j); });
    j = original; j.at("capabilities").at("reasoning_modes").as_array()[0] = "budget";
    fails(ErrorCode::Unsupported, [&] { (void)provider_profile_from_json(j); });
    j = original; j.at("capabilities").as_object()["reasoning_efforts"] = Array{"ultra"};
    fails(ErrorCode::Unsupported, [&] { (void)provider_profile_from_json(j); });
    j = original; j.at("capabilities").as_object()["reasoning_efforts"] = Array{"low", "low"};
    fails(ErrorCode::InvalidInput, [&] { (void)provider_profile_from_json(j); });
    for (const auto* key : {"max_request_bytes", "max_sse_events", "max_timeout_ms"}) {
        j = original; j.at("limits").as_object()[key] = 0;
        fails(ErrorCode::InvalidInput, [&] { (void)provider_profile_from_json(j); });
        j.at("limits").as_object()[key] = std::numeric_limits<std::uint64_t>::max();
        fails(ErrorCode::InvalidInput, [&] { (void)provider_profile_from_json(j); });
    }
}
void native_request_maps_complete_sections() {
    const auto p = fixture::profile(); const auto r = fixture::request();
    const auto encoded = encode_responses_request(p, r); const auto body = parse_json_strict(encoded.body);
    CHECK(body.at("model") == Json(p.model)); CHECK(body.at("stream") == false);
    CHECK(body.at("store") == false); CHECK(body.at("background") == false);
    CHECK(body.at("truncation") == "disabled"); CHECK(body.at("max_output_tokens") == r.max_output_tokens);
    CHECK(!body.as_object().contains("reasoning")); CHECK(!body.as_object().contains("previous_response_id"));
    const auto instructions = parse_json_strict(std::string(body.at("instructions").as_string()));
    CHECK(instructions.at("system") == r.context.logical_input.at("prefix").at("system"));
    CHECK(instructions.at("dependencies") == r.context.logical_input.at("prefix").at("dependencies"));
    CHECK(instructions.at("tool_versions").as_array().front().at("version") == "1");
    const auto& tool = body.at("tools").as_array().front();
    CHECK(tool.at("type") == "function"); CHECK(tool.at("name") == "note");
    CHECK(tool.at("strict") == false);
    CHECK(tool.at("parameters") == fixture::context().tools.front().definition);
    const auto contract = parse_json_strict(std::string(body.at("input").as_array().front().at("content").as_string()));
    CHECK(contract.at("task_contract") == Json(fixture::context().task_contract));
    CHECK(encoded.body == canonical_json(body)); CHECK(encoded.body_sha256 == sha256_hex(encoded.body));
}
void original_request_and_null_wire_remain_unchanged() {
    const auto r = fixture::request(); const auto original = to_json(r);
    const auto e = encode_responses_request(fixture::profile(), r, true);
    CHECK(to_json(r) == original); CHECK(original.at("wire_request_sha256").is_null());
    CHECK(original.at("context").at("wire_sha256").is_null());
    CHECK(e.logical_request_sha256 == original.at("logical_request_sha256").as_string());
    CHECK(e.body_sha256 != e.logical_request_sha256);
    CHECK(e.evidence.at("network_dispatched") == false); CHECK(e.evidence.at("paid_attempts") == 0);
    CHECK(e.evidence.at("wire_request_sha256").is_null()); CHECK(e.evidence.at("business_accepted") == false);
}
void credentials_are_never_resolved_or_encoded() {
    CHECK(::setenv("WLS_OFFLINE_FIXTURE_API_KEY", "TEST_ONLY_SECRET_NEVER_ENCODED_8731", 1) == 0);
    const auto encoded = encode_responses_request(fixture::profile(), fixture::request());
    const auto result = canonical_json(to_json(encoded));
    CHECK(::unsetenv("WLS_OFFLINE_FIXTURE_API_KEY") == 0);
    CHECK(result.find("TEST_ONLY_SECRET_NEVER_ENCODED_8731") == std::string::npos);
    CHECK(result.find("Authorization") == std::string::npos);
    CHECK(encoded.evidence.at("credentials_resolved") == false);
}
void native_history_preserves_ids_results_and_sources() {
    const Json source = Object{{"original_attempt", "old"}, {"not_execution_authority", true}};
    const std::vector<StructuredContextMessage> messages{
        {"user", "old question", {}, {}, nullptr},
        {"assistant", "old text", {{"history_123", "old_tool", Object{{"path", "原文😀"}}}}, {}, source},
        {"tool", "", {}, "history_123", source}};
    const auto r = fixture::request(messages); const auto body = parse_json_strict(encode_responses_request(fixture::profile(), r).body);
    const auto& input = body.at("input").as_array(); CHECK(input.size() == 5);
    CHECK(input[1].at("role") == "user" && input[1].at("content") == "old question");
    CHECK(input[2].at("role") == "assistant" && input[2].at("content") == "old text");
    CHECK(input[3].at("type") == "function_call" && input[3].at("call_id") == "history_123");
    CHECK(input[3].at("name") == "old_tool");
    CHECK(parse_json_strict(std::string(input[3].at("arguments").as_string())) == messages[1].tool_calls[0].arguments);
    CHECK(input[4].at("type") == "function_call_output" && input[4].at("output") == "");
    CHECK(input[4].at("call_id") == input[3].at("call_id"));
    const auto contract = parse_json_strict(std::string(input[0].at("content").as_string()));
    CHECK(contract.at("message_sources").as_array().size() == 2);
    CHECK(contract.at("message_sources").as_array()[0].at("source") == source);
    CHECK(body.at("tools").as_array().size() == 1 && body.at("tools").as_array()[0].at("name") == "note");
}
void frozen_profile_and_legacy_request_cannot_be_relabelled() {
    const auto p = fixture::profile();
    for (int kind = 0; kind < 3; ++kind) {
        auto c = fixture::context();
        if (kind == 0) c.protocol_version = "fake-v1";
        if (kind == 1) c.adapter_version = "2";
        if (kind == 2) c.model.model = "fake";
        const auto r = with_context(c); const auto original = to_json(r);
        fails(ErrorCode::Conflict, [&] { (void)encode_responses_request(p, r); });
        CHECK(to_json(r) == original);
    }
    auto r = fixture::request(); r.context = ContextCompiler().compile(fixture::context());
    fails(ErrorCode::Unsupported, [&] { (void)encode_responses_request(p, r); });
    r = fixture::request(); r.context.logical_input.at("task_contract") = "forged";
    fails(ErrorCode::InvalidInput, [&] { (void)encode_responses_request(p, r); });
}
void reasoning_mapping_and_refusal_are_explicit() {
    auto c = fixture::context(); c.model.reasoning = {ReasoningMode::Disabled, ""};
    auto body = parse_json_strict(encode_responses_request(fixture::profile(), with_context(c)).body);
    CHECK(body.at("reasoning").at("effort") == "none");
    auto p = fixture::profile(); p.reasoning_off = false;
    fails(ErrorCode::Unsupported, [&] { (void)encode_responses_request(p, with_context(c)); });
    c.model.reasoning = {ReasoningMode::Explicit, "high"};
    body = parse_json_strict(encode_responses_request(p, with_context(c)).body);
    CHECK(body.at("reasoning").at("effort") == "high");
    c.model.reasoning = {ReasoningMode::Explicit, "medium"};
    fails(ErrorCode::Unsupported, [&] { (void)encode_responses_request(p, with_context(c)); });
    p.reasoning_provider_default = false;
    fails(ErrorCode::Unsupported, [&] { (void)encode_responses_request(p, fixture::request()); });
}
void request_capabilities_bytes_reservations_and_deadline_are_bounded() {
    auto p = fixture::profile(); const auto r = fixture::request(); p.streaming = false;
    fails(ErrorCode::Unsupported, [&] { (void)encode_responses_request(p, r, true); });
    p = fixture::profile(); p.tool_calling = false; p.parallel_tool_calls = false;
    fails(ErrorCode::Unsupported, [&] { (void)encode_responses_request(p, r); });
    p = fixture::profile(); p.max_output_tokens = 64;
    fails(ErrorCode::Quota, [&] { (void)encode_responses_request(p, r); });
    p = fixture::profile(); p.context_window = 8192;
    fails(ErrorCode::Quota, [&] { (void)encode_responses_request(p, r); });
    p = fixture::profile(); p.limits.max_timeout_ms = 29999;
    fails(ErrorCode::Quota, [&] { (void)encode_responses_request(p, r); });
    p = fixture::profile(); p.limits.max_request_bytes = 256;
    fails(ErrorCode::Quota, [&] { (void)encode_responses_request(p, r); });
    auto c = fixture::context(); c.tools[0].name = "not.a.responses.function";
    fails(ErrorCode::Unsupported, [&] { (void)encode_responses_request(fixture::profile(), with_context(c)); });
}
void completed_text_has_original_tuple_and_known_usage_only() {
    const auto r = fixture::request(); const auto result = decode(fixture::text_response("原文😀"));
    CHECK(result.response.status == ModelResultStatus::Completed); CHECK(result.response.text == "原文😀");
    CHECK(result.response.call_id == r.call_id && result.response.attempt_id == r.attempt_id);
    CHECK(result.response.binding == r.binding && result.response.config_snapshot_id == r.config_snapshot_id);
    CHECK(result.response.role == r.role && result.response.role_call_index == r.role_call_index);
    CHECK(result.provider_response_id == "resp_offline_fixture"); CHECK(result.provider_response_id != r.call_id);
    CHECK(result.response.usage.input_tokens == 9 && result.response.usage.output_tokens == 3);
    CHECK(result.response.usage.cache_read_tokens == 2 && result.response.usage.cache_write_tokens == 1);
    CHECK(!result.response.usage.cost_micro_usd.has_value());
    CHECK(result.evidence.at("protocol_complete") == true && result.evidence.at("business_accepted") == false);
    validate_response(r, result.response);
}
void documented_empty_response_metadata_is_compatible_without_new_capabilities() {
    // Synthetic text, model and identities in the documented response envelope.
    // This fixture is not a captured provider response or a live API test.
    auto value = fixture::text_response("synthetic official-envelope fixture");
    value.as_object()["created_at"] = 1; value.as_object()["completed_at"] = 2;
    value.as_object()["instructions"] = nullptr; value.as_object()["max_output_tokens"] = nullptr;
    value.as_object()["previous_response_id"] = nullptr; value.as_object()["store"] = false;
    value.as_object()["temperature"] = 1; value.as_object()["text"] = Object{{"format", Object{{"type", "text"}}}};
    value.as_object()["tool_choice"] = "auto"; value.as_object()["tools"] = Array{};
    value.as_object()["top_p"] = 1; value.as_object()["truncation"] = "disabled";
    value.as_object()["user"] = nullptr; value.as_object()["metadata"] = Object{};
    value.as_object()["parallel_tool_calls"] = true;
    const auto result = decode(value);
    CHECK(result.response.status == ModelResultStatus::Completed);
    CHECK(result.raw_body == canonical_json(value));
    CHECK(result.evidence.at("business_accepted") == false);
    for (const auto* key : {"access_programs", "reasoning_effort"}) {
        auto unsupported = value; unsupported.as_object()[key] = "new capability";
        refused(unsupported, "unsupported_response");
    }
    auto unsupported = value; unsupported.as_object()["input"] = Array{Object{{"role", "user"}, {"content", "echo"}}};
    refused(unsupported, "unsupported_response");
    unsupported = value; unsupported.as_object()["input"] = nullptr;
    refused(unsupported, "unsupported_response");
    unsupported = value; unsupported.as_object()["new_provider_field"] = nullptr;
    refused(unsupported, "unsupported_response");
}
void complete_function_calls_preserve_provider_ids_and_arguments() {
    const auto result = decode(fixture::tool_response());
    CHECK(result.response.status == ModelResultStatus::Completed && result.response.tool_calls.size() == 1);
    CHECK(result.response.tool_calls[0].call_id == "call_provider_fixture");
    CHECK(result.response.tool_calls[0].name == "note");
    CHECK((result.response.tool_calls[0].arguments == Object{{"message", "fixture"}}));
    CHECK(result.response.text.empty()); CHECK(result.evidence.at("tool_dispatch_performed") == false);
}
void missing_and_partial_usage_stay_unknown() {
    auto value = fixture::text_response(); value.as_object().erase("usage");
    CHECK(to_json(decode(value).response.usage) == to_json(ModelUsage{}));
    value.as_object()["usage"] = nullptr;
    CHECK(to_json(decode(value).response.usage) == to_json(ModelUsage{}));
    value.as_object()["usage"] = Object{{"output_tokens", 0}};
    const auto u = decode(value).response.usage;
    CHECK(u.output_tokens == 0 && !u.input_tokens && !u.cache_read_tokens && !u.cache_write_tokens && !u.cost_micro_usd);
}
void malformed_usage_is_not_coerced_or_inferred() {
    for (const auto& invalid : {Json(-1), Json(1.5), Json("3"), Json(std::numeric_limits<std::uint64_t>::max())}) {
        auto value = fixture::text_response(); value.at("usage").as_object()["input_tokens"] = invalid;
        refused(value);
    }
    auto value = fixture::text_response(); value.at("usage").as_object()["total_tokens"] = 13;
    refused(value);
    value = fixture::text_response(); value.at("usage").at("input_tokens_details").as_object()["cached_tokens"] = 10;
    refused(value);
    value = fixture::text_response(); value.at("usage").as_object()["cost_usd"] = 0;
    refused(value, "unsupported_response");
}
void partial_usage_cannot_exceed_a_known_total_or_infer_missing_components() {
    const std::vector<Json> invalid{
        Object{{"input_tokens", 9}, {"total_tokens", 8}},
        Object{{"input_tokens", 9}, {"output_tokens", nullptr}, {"total_tokens", 8}},
        Object{{"output_tokens", 3}, {"total_tokens", 2}},
        Object{{"input_tokens", nullptr}, {"output_tokens", 3}, {"total_tokens", 2}},
        Object{{"input_tokens_details", Object{{"cached_tokens", 3}}}, {"total_tokens", 2}},
        Object{{"input_tokens_details", Object{{"cache_write_tokens", 3}}}, {"total_tokens", 2}},
        Object{{"output_tokens_details", Object{{"reasoning_tokens", 3}}}, {"total_tokens", 2}}
    };
    for (const auto& reported : invalid) {
        auto value = fixture::text_response(); value.as_object()["usage"] = reported;
        const auto result = decode(value);
        CHECK(result.response.status == ModelResultStatus::Error);
        CHECK(result.response.error_code == "invalid_response");
        CHECK(result.response.text.empty() && result.response.tool_calls.empty());
        CHECK(to_json(result.response.usage) == to_json(ModelUsage{}));
    }
    auto value = fixture::text_response();
    value.as_object()["usage"] = Object{{"input_tokens", 9}, {"total_tokens", 12}};
    auto result = decode(value);
    CHECK(result.response.status == ModelResultStatus::Completed);
    CHECK(result.response.usage.input_tokens == 9 && !result.response.usage.output_tokens);
    value.as_object()["usage"] = Object{{"output_tokens", 3}, {"total_tokens", 12}};
    result = decode(value);
    CHECK(result.response.status == ModelResultStatus::Completed);
    CHECK(!result.response.usage.input_tokens && result.response.usage.output_tokens == 3);
}
void http_error_statuses_have_no_retry_or_completed_output() {
    for (const int status : {301, 401, 429, 500, 503}) {
        const auto raw = canonical_json(Object{{"error", Object{{"type", "fixture_error"}, {"message", "synthetic"}}}});
        const auto d = decode_responses_response(fixture::profile(), fixture::request(), status, raw);
        CHECK(d.response.status == ModelResultStatus::Error && d.response.error_code == "http_" + std::to_string(status));
        CHECK(d.raw_body == raw && d.response.tool_calls.empty());
        CHECK(!d.response.usage.cost_micro_usd && !d.response.usage.input_tokens);
        CHECK(d.evidence.at("paid_attempts") == 0 && d.evidence.at("network_dispatched") == false);
    }
    fails(ErrorCode::InvalidInput, [&] { (void)decode_responses_response(fixture::profile(), fixture::request(), 0, "{}"); });
}
void incomplete_cancelled_and_nonterminal_never_return_partial_tools() {
    for (const auto* status : {"incomplete", "cancelled", "in_progress", "queued"}) {
        auto value = fixture::tool_response(); value.as_object()["status"] = status;
        value.at("output").as_array()[0].as_object()["arguments"] = "{";
        if (std::string(status) == "incomplete") value.as_object()["incomplete_details"] = Object{{"reason", "max_output_tokens"}};
        const auto d = decode(value);
        CHECK(d.response.status == ModelResultStatus::Interrupted && d.response.tool_calls.empty());
        CHECK(d.response.text.empty() && d.response.usage.input_tokens == 9);
        CHECK(d.raw_body == canonical_json(value));
    }
    auto value = fixture::tool_response(); value.as_object()["status"] = "failed";
    value.as_object()["error"] = Object{{"code", "server_error"}, {"message", "synthetic failure"}};
    const auto d = decode(value); CHECK(d.response.error_code == "provider_failed");
    CHECK(d.response.status == ModelResultStatus::Error && d.response.tool_calls.empty());
    CHECK(d.response.usage.output_tokens == 3);
}
void wrong_response_model_and_host_identity_fields_refuse() {
    auto value = fixture::tool_response(); value.as_object()["model"] = "another-alias";
    refused(value, "provider_model_mismatch");
    value = fixture::tool_response(); value.as_object()["call_id"] = "impersonated-host-call";
    refused(value, "unsupported_response");
    value = fixture::tool_response(); value.as_object()["id"] = "invalid\nresponse";
    refused(value);
    value = fixture::tool_response(); value.as_object()["object"] = "chat.completion";
    refused(value);
}
void unknown_tool_and_duplicate_provider_identifiers_refuse() {
    auto value = fixture::tool_response(); value.at("output").as_array()[0].as_object()["name"] = "run";
    refused(value, "unavailable_tool");
    value = fixture::tool_response(); value.at("output").as_array().push_back(value.at("output").as_array()[0]);
    refused(value);
    value.at("output").as_array()[1].as_object()["id"] = "different_item";
    refused(value);
    value = fixture::tool_response(); value.at("output").as_array()[0].as_object()["call_id"] = "";
    refused(value);
}
void partial_nonobject_or_duplicate_argument_keys_refuse() {
    for (const auto* raw : {"{", "{} trailing", "[]", "null", "{\"message\":1,\"message\":2}"}) {
        auto value = fixture::tool_response(); value.at("output").as_array()[0].as_object()["arguments"] = raw;
        refused(value);
    }
    auto value = fixture::tool_response(); value.at("output").as_array()[0].as_object()["status"] = "in_progress";
    refused(value);
}
void unsupported_output_is_retained_and_entire_result_is_refused() {
    for (const auto* type : {"reasoning", "web_search_call", "computer_call", "image_generation_call"}) {
        auto value = fixture::tool_response();
        value.at("output").as_array().push_back(Object{{"id", "unsupported_item"}, {"type", type}});
        refused(value, "unsupported_response");
    }
    auto value = fixture::text_response();
    value.at("output").as_array()[0].at("content").as_array()[0] = Object{{"type", "refusal"}, {"refusal", "cannot comply"}};
    refused(value, "unsupported_response");
    value = fixture::text_response(); value.at("output").as_array()[0].as_object()["extra"] = true;
    refused(value, "unsupported_response");
}
void annotations_and_logprobs_are_explicit_subset_boundaries() {
    auto value = fixture::text_response();
    auto& part = value.at("output").as_array()[0].at("content").as_array()[0];
    part.as_object()["logprobs"] = Array{};
    CHECK(decode(value).response.status == ModelResultStatus::Completed);
    part.as_object()["annotations"] = Array{Object{{"type", "citation"}}};
    refused(value, "unsupported_response");
    part.as_object()["annotations"] = Array{}; part.as_object()["logprobs"] = Array{0};
    refused(value, "unsupported_response");
}
void response_text_items_calls_and_arguments_are_bounded() {
    auto p = fixture::profile(); p.limits.max_text_bytes = 3;
    CHECK(decode(fixture::text_response("123"), p).response.status == ModelResultStatus::Completed);
    CHECK(decode(fixture::text_response("1234"), p).response.error_code == "response_limit");
    p = fixture::profile(); p.limits.max_tool_argument_bytes = 2;
    CHECK(decode(fixture::tool_response(), p).response.error_code == "response_limit");
    auto value = fixture::tool_response(); auto second = value.at("output").as_array()[0];
    second.as_object()["id"] = "second_item"; second.as_object()["call_id"] = "second_call";
    value.at("output").as_array().push_back(second);
    p = fixture::profile(); p.limits.max_output_items = 1;
    CHECK(decode(value, p).response.error_code == "response_limit");
    p = fixture::profile(); p.limits.max_tool_calls = 1;
    CHECK(decode(value, p).response.error_code == "response_limit");
    p = fixture::profile(); p.parallel_tool_calls = false;
    CHECK(decode(value, p).response.error_code == "response_limit");
    p = fixture::profile(); p.limits.max_response_bytes = 3;
    fails(ErrorCode::Quota, [&] { (void)decode(fixture::text_response(), p); });
}
void malformed_json_and_raw_invalid_utf8_remain_serializable() {
    const std::vector<std::string> bodies{"", "{", "{\"id\":1,\"id\":2}", "[]", "{}x", std::string(1, '\xff')};
    for (const auto& raw : bodies) {
        const auto d = decode_responses_response(fixture::profile(), fixture::request(), 200, raw);
        CHECK(d.response.status == ModelResultStatus::Error && d.response.error_code == "invalid_response");
        CHECK(d.raw_body == raw && d.raw_body_sha256 == sha256_hex(raw));
        const auto encoded = to_json(d); CHECK(encoded.at("raw_body_encoding") == "hex");
        CHECK(encoded.at("raw_body_hex").as_string().size() == raw.size() * 2);
        CHECK(parse_json_strict(canonical_json(encoded)) == encoded);
    }
}
void raw_fingerprint_preserves_whitespace_and_result_hashes_detect_tampering() {
    const auto raw = canonical_json(fixture::text_response());
    const auto a = decode_responses_response(fixture::profile(), fixture::request(), 200, raw);
    const auto b = decode_responses_response(fixture::profile(), fixture::request(), 200, "\n" + raw + "\n");
    CHECK(to_json(a.response) == to_json(b.response)); CHECK(a.raw_body_sha256 != b.raw_body_sha256);
    auto changed = a; changed.raw_body += " ";
    fails(ErrorCode::Corrupt, [&] { (void)to_json(changed); });
    auto encoded = encode_responses_request(fixture::profile(), fixture::request()); encoded.body += " ";
    fails(ErrorCode::Corrupt, [&] { (void)to_json(encoded); });
}
void encoded_record_binds_transport_description_identity_and_evidence() {
    const auto encoded = encode_responses_request(fixture::profile(), fixture::request(), true);
    auto record = to_json(encoded);
    CHECK(record.at("record_integrity") == "local_consistency_only");
    CHECK(record.at("record_sha256") == Json(encoded.record_sha256));
    record.as_object().erase("record_sha256");
    CHECK(sha256_hex(canonical_json(record)) == encoded.record_sha256);
    const std::vector<std::function<void(EncodedResponsesRequest&)>> mutations{
        [](auto& value) { value.method = "GET"; },
        [](auto& value) { value.url = "https://changed.example.invalid/v1/responses"; },
        [](auto& value) { value.stream = false; },
        [](auto& value) { value.profile_sha256 = sha256_hex("changed profile"); },
        [](auto& value) { value.logical_request_sha256 = sha256_hex("changed request"); },
        [](auto& value) { value.evidence.as_object()["call_id"] = "changed_call"; },
        [](auto& value) { value.evidence.as_object()["attempt_id"] = "changed_attempt"; },
        [](auto& value) { value.evidence.as_object()["network_dispatched"] = true; },
        [](auto& value) { value.evidence.as_object()["profile_sha256"] = sha256_hex("changed evidence"); },
        [](auto& value) { value.evidence.as_object()["encoded_body_bytes"] = 0; },
        [](auto& value) { value.body += " "; value.body_sha256 = sha256_hex(value.body); },
        [](auto& value) { value.record_sha256 = sha256_hex("changed record"); }
    };
    for (const auto& mutate : mutations) {
        auto changed = encoded; mutate(changed);
        fails(ErrorCode::Corrupt, [&] { (void)to_json(changed); });
    }
}
void decoded_record_binds_original_tuple_result_usage_and_raw_evidence() {
    const auto decoded = decode(fixture::text_response());
    auto record = to_json(decoded);
    CHECK(record.at("record_integrity") == "local_consistency_only");
    CHECK(record.at("record_sha256") == Json(decoded.record_sha256));
    record.as_object().erase("record_sha256");
    CHECK(sha256_hex(canonical_json(record)) == decoded.record_sha256);
    const std::vector<std::function<void(DecodedResponsesResponse&)>> mutations{
        [](auto& value) { value.provider_response_id = "resp_changed"; },
        [](auto& value) { value.response.call_id = "changed_call"; },
        [](auto& value) { value.response.attempt_id = "changed_attempt"; },
        [](auto& value) { ++value.response.role_call_index; },
        [](auto& value) { value.response.config_snapshot_id = sha256_hex("changed config"); },
        [](auto& value) { value.response.text = "changed output"; },
        [](auto& value) { value.response.usage.input_tokens = 10; },
        [](auto& value) { value.response.status = ModelResultStatus::Interrupted;
            value.response.error_code = "changed"; value.response.text.clear(); },
        [](auto& value) { value.evidence.as_object()["provider_response_id"] = "resp_changed"; },
        [](auto& value) { value.evidence.as_object()["http_status"] = 500; },
        [](auto& value) { value.evidence.as_object()["protocol_complete"] = false; },
        [](auto& value) { value.evidence.as_object()["business_accepted"] = true; },
        [](auto& value) { value.raw_body += " "; value.raw_body_sha256 = sha256_hex(value.raw_body); },
        [](auto& value) { value.record_sha256 = sha256_hex("changed record"); }
    };
    for (const auto& mutate : mutations) {
        auto changed = decoded; mutate(changed);
        fails(ErrorCode::Corrupt, [&] { (void)to_json(changed); });
    }
}
void empty_completed_and_contradictory_terminal_states_refuse() {
    auto value = fixture::response_with_output(Array{}); refused(value);
    value = fixture::text_response(); value.as_object()["error"] = Object{{"code", "oops"}}; refused(value);
    value = fixture::text_response(); value.as_object()["incomplete_details"] = Object{{"reason", "max_output_tokens"}}; refused(value);
    value = fixture::text_response(); value.as_object()["status"] = "failed"; refused(value);
    value = fixture::text_response(); value.at("output").as_array()[0].as_object()["role"] = "user"; refused(value);
}
} // namespace

int main() {
    return ::test::run({
        {"profile round trip binds effective capabilities and limits", profile_round_trip_and_effective_hash},
        {"profile rejects unknown fields versions credentials and paid execution", profile_fields_versions_and_paid_refusals},
        {"endpoint and authentication references are explicit and bounded", endpoint_and_credential_references_are_explicit},
        {"profile capability declarations and hard limits are strict", profile_capabilities_and_hard_limits_refuse},
        {"native request preserves system dependencies schema and task contract", native_request_maps_complete_sections},
        {"encoding preserves original request and null transport evidence", original_request_and_null_wire_remain_unchanged},
        {"encoding never resolves credential references", credentials_are_never_resolved_or_encoded},
        {"native history preserves call pairing raw outputs and provenance", native_history_preserves_ids_results_and_sources},
        {"fake wrong-profile and legacy requests cannot be relabelled", frozen_profile_and_legacy_request_cannot_be_relabelled},
        {"reasoning off default and effort map only declared capabilities", reasoning_mapping_and_refusal_are_explicit},
        {"request capabilities byte token and deadline limits reject before encoding", request_capabilities_bytes_reservations_and_deadline_are_bounded},
        {"complete text retains host tuple and only reported usage", completed_text_has_original_tuple_and_known_usage_only},
        {"documented empty response metadata preserves compatibility without new capabilities", documented_empty_response_metadata_is_compatible_without_new_capabilities},
        {"complete functions preserve provider IDs without execution authority", complete_function_calls_preserve_provider_ids_and_arguments},
        {"missing usage remains unknown and explicit zero stays zero", missing_and_partial_usage_stay_unknown},
        {"malformed or inconsistent usage cannot become known zero", malformed_usage_is_not_coerced_or_inferred},
        {"partial usage respects known totals without inferring missing components", partial_usage_cannot_exceed_a_known_total_or_infer_missing_components},
        {"HTTP failures and redirects do not authorize retries", http_error_statuses_have_no_retry_or_completed_output},
        {"incomplete cancelled failed and nonterminal responses cannot dispatch tools", incomplete_cancelled_and_nonterminal_never_return_partial_tools},
        {"wrong provider model and spoofed host identity fields reject", wrong_response_model_and_host_identity_fields_refuse},
        {"unavailable tools and duplicate provider IDs reject", unknown_tool_and_duplicate_provider_identifiers_refuse},
        {"partial nonobject and duplicate-key function arguments reject", partial_nonobject_or_duplicate_argument_keys_refuse},
        {"unsupported reasoning hosted tools and refusal retain raw evidence", unsupported_output_is_retained_and_entire_result_is_refused},
        {"annotations and logprobs remain explicit unsupported content", annotations_and_logprobs_are_explicit_subset_boundaries},
        {"response aggregate text items arguments and calls are bounded", response_text_items_calls_and_arguments_are_bounded},
        {"invalid JSON and UTF-8 bytes survive serializable raw evidence", malformed_json_and_raw_invalid_utf8_remain_serializable},
        {"raw fingerprints preserve exact bytes and detect later mutation", raw_fingerprint_preserves_whitespace_and_result_hashes_detect_tampering},
        {"encoding record binds URL stream identity and evidence", encoded_record_binds_transport_description_identity_and_evidence},
        {"decoding record binds tuple result usage and raw evidence", decoded_record_binds_original_tuple_result_usage_and_raw_evidence},
        {"empty completed and contradictory terminal results reject", empty_completed_and_contradictory_terminal_states_refuse}
    });
}
