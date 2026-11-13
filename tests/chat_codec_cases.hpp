#pragma once
#include "protocol_test_support.hpp"
#include "test_support.hpp"
#include "wls/live_model.hpp"
#include "wls/worker_history.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace wls::test::chat {
using namespace s1;
using Object = boost::json::object;
using Array = boost::json::array;

inline void fails(ErrorCode code, const std::function<void()>& action) {
    try { action(); } catch (const Error& error) {
        if (error.code() != code) throw std::runtime_error(error.what());
        return;
    }
    throw std::runtime_error("expected Chat codec refusal");
}
inline ProviderProfile profile() {
    auto p = protocol::profile(); p.protocol = "openai-chat-v1";
    p.profile_id = "offline-chat-fixture"; p.responses_path.clear();
    p.chat_completions_path = "/v1/chat/completions"; p.model_ref = "offline-chat";
    p.model = "fixture-chat-model"; p.streaming = false;
    p.reasoning_off = false; p.reasoning_efforts.clear();
    p.response_metadata_policy = "ignore-record-v1";
    return p;
}
inline ContextInput context() {
    auto c = protocol::context(); c.protocol_version = "openai-chat-v1";
    c.model.model = "offline-chat";
    c.tools.front().description = "Retain an observation without executing effects.";
    c.tools.front().definition.at("properties").at("message").as_object()["description"] = "The original observation.";
    return c;
}
inline ModelRequest request(const std::vector<StructuredContextMessage>& messages = {},
                            ContextInput c = context(), bool v3 = false) {
    auto r = protocol::request();
    r.context = compile_structured_context(c, messages, {}, v3 ? StructuredContextFormat::V3 : StructuredContextFormat::V2);
    r.model = c.model; return r;
}
inline Json completion(bool tool = false) {
    Object message{{"role", "assistant"}, {"content", tool ? Json(nullptr) : Json("original text 😀")}, {"refusal", nullptr}};
    if (tool) message["tool_calls"] = Array{Object{{"id", "call_chat_one"}, {"type", "function"},
        {"function", Object{{"name", "note"}, {"arguments", "{\"message\":\"original\"}"}}}}};
    return Object{{"id", "chatcmpl-fixture"}, {"object", "chat.completion"}, {"created", 1}, {"model", profile().model},
        {"choices", Array{Object{{"index", 0}, {"message", message}, {"logprobs", nullptr}, {"finish_reason", tool ? "tool_calls" : "stop"}}}},
        {"usage", Object{{"prompt_tokens", 9}, {"completion_tokens", 3}, {"total_tokens", 12},
            {"prompt_tokens_details", Object{{"cached_tokens", 2}, {"cache_write_tokens", 1}}},
            {"completion_tokens_details", Object{{"reasoning_tokens", 1}}}}}};
}
inline Json& choice(Json& value) { return value.at("choices").as_array().front(); }
inline Json& message(Json& value) { return choice(value).at("message"); }
inline Json& call(Json& value) { return message(value).at("tool_calls").as_array().front(); }
inline DecodedProtocolResponse decode(const Json& value, ProviderProfile p = profile(), const ModelRequest& r = request()) {
    return decode_protocol_response(p, r, 200, canonical_json(value));
}
inline void refused(const Json& value, const std::string& reason = "invalid_response", ProviderProfile p = profile()) {
    const auto d = decode(value, p);
    CHECK(d.response.status == ModelResultStatus::Error && d.response.error_code == reason);
    CHECK(d.response.text.empty() && d.response.tool_calls.empty());
    CHECK(d.raw_body == canonical_json(value) && d.raw_body_sha256 == sha256_hex(d.raw_body));
    CHECK(d.evidence.at("business_accepted") == false && d.evidence.at("tool_dispatch_performed") == false);
}
inline void profile_and_runtime_boundaries() {
    const auto p = profile(); const auto saved = to_json(p);
    CHECK(saved.at("adapter") == "openai-chat" && !saved.as_object().contains("responses_path"));
    CHECK(to_json(provider_profile_from_json(saved)) == saved);
    auto aliases = saved; aliases.as_object()["chat_model_aliases"] = Array{"snapshot-z", "snapshot-a"};
    auto normalized = provider_profile_from_json(aliases);
    CHECK(normalized.chat_model_aliases == std::vector<std::string>({"snapshot-a", "snapshot-z"}));
    CHECK(provider_profile_sha256(normalized) != provider_profile_sha256(p));
    aliases.at("chat_model_aliases") = Array{"same", "same"};
    fails(ErrorCode::InvalidInput, [&] { (void)provider_profile_from_json(aliases); });
    for (int variant = 0; variant != 5; ++variant) {
        auto invalid = p;
        if (variant == 0) invalid.streaming = true;
        if (variant == 1) invalid.responses_path = "/v1/responses";
        if (variant == 2) invalid.response_metadata_policy.clear();
        if (variant == 3) invalid.billing_challenge = "wls-billing-challenge-v1";
        if (variant == 4) invalid.chat_reasoning_policy = "guess-by-model-name";
        fails(ErrorCode::Unsupported, [&] { validate_provider_profile(invalid); });
    }
    auto paid = saved; paid.at("real_calls_enabled") = true;
    fails(ErrorCode::Unsupported, [&] { (void)provider_profile_from_json(paid); });
    paid = saved; paid.at("max_paid_retries") = 1;
    fails(ErrorCode::Unsupported, [&] { (void)provider_profile_from_json(paid); });
    auto old = protocol::profile(); old.chat_completions_path = p.chat_completions_path;
    fails(ErrorCode::Unsupported, [&] { validate_provider_profile(old); });
    fails(ErrorCode::Unsupported, [&] { (void)encode_responses_request(p, request()); });
    fails(ErrorCode::Unsupported, [&] { (void)decode_responses_response(p, request(), 200, "{}"); });
    fails(ErrorCode::Unsupported, [&] { (void)encode_protocol_request(p, request(), true); });
    fails(ErrorCode::Unsupported, [&] { validate_responses_https_target(p, {}); });
    validate_protocol_https_target(p, {});
    LiveModelSettings live; live.provider = p; live.transport.ca_bundle_pem = "offline-only";
    live.transport.resolved_ipv4 = "127.0.0.1";
    const Json outer = Object{{"adapter", "openai-responses"}, {"adapter_version", "1"}, {"protocol", "openai-responses-v1"},
        {"model", p.model}, {"capabilities", Object{}}, {"responses", live_model_settings_json(live)}};
    fails(ErrorCode::Unsupported, [&] { (void)live_model_settings_from_json(outer, p.model_ref); });
    const auto old_p = protocol::profile(); const auto old_r = protocol::request();
    CHECK(to_json(encode_protocol_request(old_p, old_r)) == to_json(encode_responses_request(old_p, old_r)));
    const auto raw = canonical_json(protocol::text_response());
    CHECK(to_json(decode_protocol_response(old_p, old_r, 200, raw)) == to_json(decode_responses_response(old_p, old_r, 200, raw)));
}
inline void reasoning_requires_explicit_frozen_declarations() {
    auto p = profile(); auto c = context();
    CHECK(!parse_json_strict(encode_protocol_request(p, request()).body).as_object().contains("reasoning_effort"));
    c.model.reasoning = {ReasoningMode::Disabled, ""};
    fails(ErrorCode::Unsupported, [&] { (void)encode_protocol_request(p, request({}, c)); });
    p.reasoning_off = true;
    fails(ErrorCode::Unsupported, [&] { validate_provider_profile(p); });
    p.chat_reasoning_policy = "reasoning-effort-v1";
    CHECK(parse_json_strict(encode_protocol_request(p, request({}, c)).body).at("reasoning_effort") == "none");
    c.model.reasoning = {ReasoningMode::Explicit, "high"};
    fails(ErrorCode::Unsupported, [&] { (void)encode_protocol_request(p, request({}, c)); });
    p.reasoning_efforts.insert("high");
    CHECK(parse_json_strict(encode_protocol_request(p, request({}, c)).body).at("reasoning_effort") == "high");
    CHECK(to_json(provider_profile_from_json(to_json(p))) == to_json(p));
    p.chat_reasoning_policy.clear();
    fails(ErrorCode::Unsupported, [&] { validate_provider_profile(p); });
}
inline void request_mapping_and_record_identity() {
    const auto p = profile(); const auto r = request(); const auto original = to_json(r);
    const auto e = encode_protocol_request(p, r); const auto body = parse_json_strict(e.body);
    CHECK(e.url == p.base_url + p.chat_completions_path && e.body_sha256 == sha256_hex(e.body));
    CHECK(e.profile_sha256 == provider_profile_sha256(p));
    CHECK(body.at("n") == 1 && body.at("stream") == false && body.at("store") == false);
    CHECK(body.at("max_completion_tokens") == r.max_output_tokens && body.at("parallel_tool_calls") == true);
    CHECK(!body.as_object().contains("max_tokens") && !body.as_object().contains("input"));
    const auto& messages = body.at("messages").as_array();
    CHECK(messages.size() == 2 && messages.front().at("role") == "system");
    const std::string system(messages.front().at("content").as_string());
    for (const auto& item : r.context.logical_input.at("prefix").at("system").as_object())
        CHECK(system.find(std::string(item.value().as_string())) != std::string::npos);
    CHECK(parse_json_strict(std::string(messages.back().at("content").as_string())) == Json(context().task_contract));
    const auto& tool = body.at("tools").as_array().front();
    CHECK(tool.at("type") == "function" && tool.at("function").at("name") == "note");
    CHECK(tool.at("function").at("parameters") == context().tools.front().definition);
    CHECK(tool.at("function").at("strict") == false);
    CHECK(to_json(r) == original && e.logical_request_sha256 == original.at("logical_request_sha256").as_string());
    CHECK(e.evidence.at("binding") == original.at("binding") && e.evidence.at("credentials_resolved") == false);
    CHECK(e.evidence.at("network_dispatched") == false && e.evidence.at("wire_request_sha256").is_null());
    std::uint64_t sum = 0;
    for (const auto& item : e.body_segments.as_object()) if (item.key() != "total") sum += item.value().to_number<std::uint64_t>();
    CHECK(sum == e.body.size() && e.body_segments.at("total") == e.body.size());
    CHECK(to_json(e).at("format") == "wls-encoded-chat-request-v1");
    for (int variant = 0; variant != 4; ++variant) {
        auto changed = e;
        if (variant == 0) changed.body += " ";
        if (variant == 1) changed.url += "/other";
        if (variant == 2) changed.codec.clear();
        if (variant == 3) changed.evidence.at("paid_attempts") = 1;
        fails(ErrorCode::Corrupt, [&] { (void)to_json(changed); });
    }
    auto c = context(); c.tools.front().definition.as_object()["$ref"] = "external.json";
    fails(ErrorCode::Unsupported, [&] { (void)encode_protocol_request(p, request({}, c)); });
    auto limited = p; limited.limits.max_request_bytes = 64;
    fails(ErrorCode::Quota, [&] { (void)encode_protocol_request(limited, r); });
}
inline void native_rounds_keep_original_ids_and_results() {
    const Json source = Object{{"attempt_id", "attempt_old"}};
    std::vector<StructuredContextMessage> messages{
        {"assistant", "", {{"a", "note", Object{{"message", "one"}}}, {"b", "note", Object{{"message", "two"}}}}, {}, source},
        {"tool", "result A 😀", {}, "a", source}, {"tool", "result B", {}, "b", source},
        {"assistant", "again", {{"c", "note", Object{{"message", "three"}}}}, {}, nullptr},
        {"tool", "original C", {}, "c", nullptr}};
    const auto r = request(messages); const auto saved = to_json(r);
    const auto body = parse_json_strict(encode_protocol_request(profile(), r).body);
    const auto& wire = body.at("messages").as_array();
    CHECK(wire.size() == 7 && wire[2].at("content").is_null());
    CHECK(wire[2].at("tool_calls").as_array()[1].at("id") == "b");
    CHECK(wire[3].at("tool_call_id") == "a" && wire[3].at("content") == "result A 😀");
    CHECK(wire[4].at("tool_call_id") == "b" && wire[6].at("tool_call_id") == "c");
    CHECK(to_json(r) == saved);
    for (int variant = 0; variant != 4; ++variant) {
        auto broken = messages;
        if (variant == 0) broken.pop_back();
        if (variant == 1) std::swap(broken[1], broken[2]);
        if (variant == 2) broken[4].tool_call_id = "a";
        if (variant == 3) broken[3].tool_calls.front().call_id = "a";
        fails(ErrorCode::InvalidInput, [&] { (void)encode_protocol_request(profile(), request(broken)); });
    }
    const auto old = decode_responses_response(protocol::compat_profile(), protocol::request(), 200,
        protocol::compat_bytes("reasoning-tool-response.json"));
    CHECK(old.response.status == ModelResultStatus::Completed);
    const auto& old_call = old.response.tool_calls.front();
    const std::vector<StructuredContextMessage> opaque{
        {"assistant", old.response.text, {{old_call.call_id, old_call.name, old_call.arguments}}, {}, nullptr, old.response.responses_output},
        {"tool", "original result", {}, old_call.call_id, nullptr}};
    fails(ErrorCode::Unsupported, [&] { (void)encode_protocol_request(profile(), request(opaque)); });
}
inline void v3_contract_updates_lean_and_grants() {
    auto c = context(); c.model_projection = "lean-v1"; c.tool_policy = "fixed-role-v1"; c.allowed_tools = {"note"};
    c.tools.push_back({"fs_read", "1", Object{{"type", "object"}, {"properties", Object{{"path", Object{{"type", "string"}, {"description", "Original path."}}}}}}, "Read original bytes."});
    Json contract = Object{{"request", Object{{"requirements_revision", 1}, {"texts", Array{"original 😀"}}, {"constraint_catalog", Array{}}}},
        {"task", Object{{"task_revision", 1}, {"state_version", 1}, {"brief", Object{{"goal", "preserve originals"}}}}},
        {"instruction", Object{{"instruction_revision", 1}, {"body", Object{{"steps", Array{"read"}}}}}},
        {"binding", Object{{"execution_epoch", 1}}}};
    c.task_contract = canonical_json(contract); const auto first = request({}, c, true);
    const Json previous = Object{{"attempt_id", "attempt_first"}, {"input", first.context.logical_input}};
    contract.at("request").at("requirements_revision") = 2;
    contract.at("request").at("texts").as_array().push_back("complete AMEND original");
    c.task_contract = canonical_json(contract); continue_structured_contract(c, previous);
    const Json page = Object{{"path", "a"}, {"offset", 0}, {"bytes_returned", 1}, {"size_bytes", 1}, {"sha256", sha256_hex("x")},
        {"content", "x"}, {"content_base64", "eA=="}, {"eof", true}, {"next_offset", nullptr}};
    auto messages = worker_history_messages(Object{{"format", "wls-history-tail-v1"}, {"items", Array{}},
        {"total_count", 1}, {"omitted_count", 1}, {"through_sequence", 7}},
        Object{{"mode", "window-v1"}, {"recent_attempts", 1}, {"inline_bytes", 32768}});
    const Json source = Object{{"attempt_id", "attempt_first"}};
    messages.push_back({"assistant", "", {{"read_one", "fs_read", Object{{"path", "a"}}}}, {}, source});
    messages.push_back({"tool", canonical_json(page), {}, "read_one", source});
    messages.push_back({"user", "UNKNOWN remains unresolved", {}, {}, nullptr});
    const auto r = request(messages, c, true); const auto saved = to_json(r);
    auto p = profile(); p.tool_choice_policy = "allowed-tools-v1";
    const auto e = encode_protocol_request(p, r); const auto body = parse_json_strict(e.body);
    CHECK(body.at("tools").as_array().size() == 1 && body.at("tool_choice") == "required");
    CHECK(body.at("tools").as_array().front().at("function").at("name") == "note");
    const auto& wire = body.at("messages").as_array();
    CHECK(wire.size() == 7 && wire[2].at("role") == "assistant" && wire[3].at("tool_call_id") == "read_one");
    CHECK(!parse_json_strict(std::string(wire[3].at("content").as_string())).as_object().contains("content_base64"));
    const auto update = parse_json_strict(std::string(wire[4].at("content").as_string()));
    CHECK(update.at("request") == r.context.logical_input.at("contract_updates").as_array().front().at("contract").at("request"));
    CHECK(update.at("semantic_versions").at("requirements_revision") == 2);
    CHECK(wire[5].at("content") == "UNKNOWN remains unresolved");
    const auto state = parse_json_strict(std::string(wire.back().at("content").as_string()));
    CHECK(!state.as_object().contains("message_sources") && !state.at("history_windows").as_array().front().as_object().contains("source"));
    CHECK(to_json(r) == saved && e.body_segments.at("current_state").to_number<std::uint64_t>() > 0);
}
inline void folded_contract_boundary_keeps_chronology() {
    Array turns;
    const auto r = request();
    for (int sequence = 1; sequence != 5; ++sequence) {
        auto response = decode(completion(true)).response;
        response.attempt_id = "attempt_fold_" + std::to_string(sequence);
        response.call_id = "call_fold_" + std::to_string(sequence);
        const auto original = to_json(response), binding = to_json(r.binding);
        const Json result = Object{{"ok", true}, {"sequence", sequence}};
        const auto hash = sha256_hex(canonical_json(Object{{"attempt_id", response.attempt_id},
            {"original_binding", binding}, {"response", original}, {"result", result}}));
        turns.push_back(Object{{"sequence", sequence}, {"attempt_id", response.attempt_id}, {"call_id", response.call_id},
            {"status", "COMPLETED"}, {"accepted", 1}, {"accounting_attention", 0}, {"frozen_binding", binding},
            {"response", original}, {"feedback", Object{{"record_sha256", hash}, {"original_binding", binding},
                {"response", original}, {"result", result}}}});
    }
    auto projection = append_fold_history(nullptr, Object{{"scope", "offline-fold"}, {"through_sequence", 4}, {"items", turns}}, true);
    const auto messages = append_fold_messages(projection); projection.as_object().erase("messages");
    CHECK(projection.at("folded_entries").as_array().size() == 2);
    auto c = context(); c.model_projection = "lean-v1";
    const auto first = request({}, c, true);
    c.task_contract = "complete replacement original";
    continue_structured_contract(c, Object{{"attempt_id", "attempt_fold_1"}, {"input", first.context.logical_input}});
    c.worker_history = projection;
    const auto amended = request(messages, c, true), saved = amended;
    const auto encoded = encode_protocol_request(profile(), amended);
    const auto body = parse_json_strict(encoded.body); const auto& wire = body.at("messages").as_array();
    const auto at = [&](std::size_t index) { return parse_json_strict(std::string(wire[index].at("content").as_string())); };
    CHECK(wire.size() == 10 && at(2).at("through_sequence") == 1);
    CHECK(at(3).at("task_contract") == "complete replacement original");
    CHECK(at(4).at("from_sequence") == 2 && at(4).at("through_sequence") == 2);
    CHECK(wire[5].at("role") == "assistant" && wire[6].at("role") == "tool");
    CHECK(to_json(amended) == to_json(saved));
    std::uint64_t total = 0;
    for (const auto& item : encoded.body_segments.as_object()) if (item.key() != "total") total += item.value().to_number<std::uint64_t>();
    CHECK(total == encoded.body.size());
}
inline void official_examples_and_unknown_usage() {
    const auto bytes = [](const std::string& name) {
        std::ifstream input(std::string(WLS_SOURCE_DIR) + "/tests/fixtures/chat/" + name, std::ios::binary);
        CHECK(input.good()); return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto provenance = parse_json_strict(bytes("provenance.json"));
    for (const auto* name : {"official-text-response.json", "official-tool-response.json"}) {
        const auto raw = bytes(name); auto p = profile(); p.model = "gpt-6-astra";
        CHECK(sha256_hex(raw) == provenance.at("fixtures").at(name).at("sha256").as_string());
        const auto d = decode_protocol_response(p, request(), 200, raw);
        CHECK(d.response.status == ModelResultStatus::Completed && d.raw_body == raw);
        CHECK(d.raw_body_sha256 == sha256_hex(raw) && d.response.responses_output.is_null());
        CHECK(!d.response.usage.cost_micro_usd && !d.response.usage.cache_write_tokens);
        CHECK(to_json(d).at("format") == "wls-decoded-chat-response-v1");
        if (std::string_view(name) == "official-text-response.json") {
            CHECK(d.response.text == "Hello! How can I assist you today?");
            CHECK(d.response.usage.input_tokens == 19 && d.response.usage.output_tokens == 10 && d.response.usage.cache_read_tokens == 0);
        } else {
            CHECK(d.response.tool_calls.size() == 1 && d.response.tool_calls.front().call_id == "call_abc123");
            CHECK(d.response.tool_calls.front().name == "get_current_weather");
            CHECK(d.response.tool_calls.front().arguments == Json(Object{{"location", "Boston, MA"}}));
            CHECK(d.response.usage.input_tokens == 82 && d.response.usage.output_tokens == 17 && !d.response.usage.cache_read_tokens);
        }
    }
    auto value = completion(); value.as_object().erase("usage");
    CHECK(!decode(value).response.usage.input_tokens && !decode(value).response.usage.cost_micro_usd);
    value.as_object()["usage"] = Object{{"prompt_tokens", 0}};
    CHECK(decode(value).response.usage.input_tokens == 0 && !decode(value).response.usage.output_tokens);
    const auto d = decode(completion());
    CHECK(d.response.usage.input_tokens == 9 && d.response.usage.output_tokens == 3);
    CHECK(d.response.usage.cache_read_tokens == 2 && d.response.usage.cache_write_tokens == 1 && !d.response.usage.cost_micro_usd);
    CHECK(to_json(d.response).at("binding") == to_json(request()).at("binding"));
    auto changed = d; changed.response.usage.input_tokens = 10;
    fails(ErrorCode::Corrupt, [&] { (void)to_json(changed); });
    changed = d; changed.codec.clear(); fails(ErrorCode::Corrupt, [&] { (void)to_json(changed); });
}
inline void metadata_tolerance_keeps_known_fields_strict() {
    auto value = completion(true);
    value.as_object()["top/~extra"] = Object{{"nested", true}};
    choice(value).as_object()["choice_extra"] = 1; message(value).as_object()["message_extra"] = 2;
    call(value).as_object()["call_extra"] = 3; call(value).at("function").as_object()["function_extra"] = 4;
    value.at("usage").as_object()["usage_extra"] = 5;
    value.at("usage").at("prompt_tokens_details").as_object()["detail_extra"] = 6;
    const auto d = decode(value); CHECK(d.response.status == ModelResultStatus::Completed);
    CHECK(d.evidence.at("unknown_fields").as_array().size() == 7 && d.evidence.at("unknown_fields_truncated") == false);
    const auto paths = canonical_json(d.evidence.at("unknown_fields"));
    CHECK(paths.find("/top~1~0extra") != std::string::npos && paths.find("/choices/0/message/tool_calls/0/function/function_extra") != std::string::npos);
    CHECK(d.raw_body == canonical_json(value));
    for (int i = 0; i != 140; ++i) value.as_object()["extension_" + std::to_string(i)] = i;
    CHECK(decode(value).evidence.at("unknown_fields_truncated") == true);
    CHECK(decode(value).evidence.at("unknown_fields").as_array().size() == 128);
    for (const auto* field : {"audio", "function_call", "reasoning", "reasoning_content", "thinking", "encrypted_content"}) {
        auto bad = completion(); message(bad).as_object()[field] = Object{}; refused(bad, "unsupported_response");
    }
    auto bad = completion(); message(bad).as_object()["annotations"] = Array{Object{}}; refused(bad, "unsupported_response");
    bad = completion(); choice(bad).at("logprobs") = Object{}; refused(bad, "unsupported_response");
    bad = completion(); message(bad).at("content") = Array{}; refused(bad);
    bad = completion(); bad.as_object()["system_fingerprint"] = 7; refused(bad);
    bad = completion(); bad.as_object()["metadata"] = 7; refused(bad);
}
inline void terminal_results_preserve_usage_without_partial_tools() {
    for (const auto* finish : {"length", "content_filter"}) {
        auto value = completion(true); choice(value).at("finish_reason") = finish;
        call(value).at("function").at("arguments") = "{\"partial\":";
        const auto d = decode(value);
        CHECK(d.response.tool_calls.empty() && d.response.text.empty() && d.response.usage.input_tokens == 9);
        CHECK(d.response.error_code == (std::string_view(finish) == "length" ? "response_max_output_tokens" : "model_refusal"));
        CHECK(d.response.status == (std::string_view(finish) == "length" ? ModelResultStatus::Interrupted : ModelResultStatus::Error));
    }
    auto value = completion(true); message(value).at("refusal") = "cannot comply"; refused(value, "model_refusal");
    auto limited = profile(); limited.limits.max_text_bytes = 3; refused(value, "response_limit", limited);
    value = completion(); choice(value).at("finish_reason") = "length"; message(value).as_object()["tool_calls"] = 1; refused(value);
    value = completion(); choice(value).at("finish_reason") = "function_call"; refused(value, "unsupported_response");
    for (const int status : {302, 400, 429, 500}) {
        const auto d = decode_protocol_response(profile(), request(), status, canonical_json(completion(true)));
        CHECK(d.response.status == ModelResultStatus::Error && d.response.error_code == "http_" + std::to_string(status));
        CHECK(d.response.usage.input_tokens == 9 && d.response.tool_calls.empty() && !d.response.usage.cost_micro_usd);
    }
}
inline void malformed_identity_usage_and_call_limits() {
    auto value = completion(); value.at("model") = "snapshot"; refused(value, "provider_model_mismatch");
    auto p = profile(); p.chat_model_aliases = {"snapshot"}; CHECK(decode(value, p).response.status == ModelResultStatus::Completed);
    auto missing = completion(); choice(missing).as_object().erase("message"); refused(missing);
    missing = completion(true); call(missing).as_object().erase("function"); refused(missing);
    for (int variant = 0; variant != 7; ++variant) {
        auto bad = completion();
        if (variant == 0) choice(bad).at("index") = 1;
        if (variant == 1) bad.at("choices").as_array().push_back(choice(bad));
        if (variant == 2) bad.at("object") = "response";
        if (variant == 3) bad.at("id") = "";
        if (variant == 4) bad.as_object().erase("created");
        if (variant == 5) message(bad).at("role") = "user";
        if (variant == 6) choice(bad).at("finish_reason") = "tool_calls";
        refused(bad);
    }
    for (const auto& invalid : std::vector<Json>{-1, 1.5, "9", true, std::numeric_limits<std::uint64_t>::max()}) {
        auto bad = completion(); bad.at("usage").at("prompt_tokens") = invalid; refused(bad);
    }
    for (int variant = 0; variant != 4; ++variant) {
        auto bad = completion();
        if (variant == 0) bad.at("usage").at("total_tokens") = 11;
        if (variant == 1) bad.at("usage").at("prompt_tokens_details").at("cached_tokens") = 10;
        if (variant == 2) bad.at("usage").at("completion_tokens_details").at("reasoning_tokens") = 4;
        if (variant == 3) bad.at("usage").at("prompt_tokens_details").as_object()["audio_tokens"] = "unknown";
        refused(bad);
    }
    for (const auto* arguments : {"{", "[]", "null", "{\"x\":1,\"x\":2}", "{} trailing"}) {
        auto bad = completion(true); call(bad).at("function").at("arguments") = arguments; refused(bad);
    }
    auto calls = completion(true); message(calls).at("tool_calls").as_array().push_back(call(calls)); refused(calls);
    message(calls).at("tool_calls").as_array().back().at("id") = "call_two";
    CHECK(decode(calls).response.tool_calls.size() == 2);
    for (int variant = 0; variant != 4; ++variant) {
        p = profile();
        if (variant == 0) p.limits.max_tool_calls = 1;
        if (variant == 1) p.parallel_tool_calls = false;
        if (variant == 2) p.limits.max_output_items = 1;
        if (variant == 3) p.limits.max_tool_argument_bytes = 24;
        refused(calls, "response_limit", p);
    }
    p = profile(); p.limits.max_response_bytes = 16;
    fails(ErrorCode::Quota, [&] { (void)decode(completion(), p); });
    for (const auto& raw : std::vector<std::string>{"", "{", "[]", "{\"x\":1,\"x\":2}", std::string(1, '\xff')}) {
        const auto d = decode_protocol_response(profile(), request(), 200, raw);
        CHECK(d.response.error_code == "invalid_response" && d.raw_body == raw);
        CHECK(to_json(d).at("raw_body_hex").as_string().size() == raw.size() * 2);
    }
}
} // namespace wls::test::chat
