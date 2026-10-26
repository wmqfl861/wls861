#pragma once

#include "wls/protocol_codec.hpp"
#include "wls/structured_context.hpp"

#include <string>
#include <vector>

// Deliberately synthetic, deterministic fixtures. The .invalid endpoint is
// never contacted, the credential name is never resolved, and these requests
// are not copied from a FakeModel Store reservation or a live provider run.
namespace wls::test::protocol {
using s1::Json;
using Object = boost::json::object;
using Array = boost::json::array;

inline s1::ProviderProfile profile() {
    s1::ProviderProfile p;
    p.profile_id = "offline-responses-fixture";
    p.base_url = "https://gateway.example.invalid";
    p.responses_path = "/v1/responses";
    p.model_ref = "offline-responses";
    p.model = "fixture-responses-model";
    p.api_key_env = "WLS_OFFLINE_FIXTURE_API_KEY";
    p.streaming = true; p.tool_calling = true; p.parallel_tool_calls = true;
    p.reasoning_provider_default = true; p.reasoning_off = true;
    p.reasoning_efforts = {"high", "low"};
    p.context_window = 100000; p.max_output_tokens = 4096;
    return p;
}

inline s1::ContextInput context() {
    s1::ContextInput c;
    c.role = Role::Worker;
    c.model = {"offline-responses", {ReasoningMode::ProviderDefault, ""}};
    c.adapter_version = "1"; c.protocol_version = "openai-responses-v1";
    c.system_prompt = "Synthetic offline protocol fixture.";
    c.role_prompt = "Worker: preserve current host authorization.";
    c.function_prompt = "Return a note; do not execute effects.";
    c.environment_card = "No network and no credentials are used.";
    c.approved_rules = "UNKNOWN blocks business; original constraints remain.";
    c.project_snapshot = "Synthetic fixture revision 1.";
    c.task_contract = "原始任务😀\nPreserve exact current requirements.";
    c.tools = {{"note", "1", Object{{"type", "object"},
        {"properties", Object{{"message", Object{{"type", "string"}}}}},
        {"required", Array{"message"}}, {"additionalProperties", false}}}};
    c.dependencies = {{"fixture", "1", s1::sha256_hex("synthetic offline fixture")}};
    return c;
}

inline s1::ModelRequest request(
        const std::vector<s1::StructuredContextMessage>& messages = {}) {
    s1::ModelRequest r;
    r.call_id = "call_offline_fixture"; r.attempt_id = "attempt_offline_fixture";
    r.role = Role::Worker; r.role_call_index = 1;
    r.binding.request_id = "req_00000000000000000000000001";
    r.binding.task_id = "tsk_00000000000000000000000002";
    r.binding.team_id = "team_00000000000000000000000003";
    r.binding.plan_revision = 1; r.binding.task_revision = 1;
    r.binding.instruction_revision = 1;
    r.config_snapshot_id = s1::sha256_hex("synthetic offline config; no Store admission");
    r.context = s1::compile_structured_context(context(), messages);
    r.model = r.context.model; r.max_output_tokens = 128;
    r.reserved_input_tokens = 8192; r.reserved_output_tokens = 128;
    r.timeout = std::chrono::milliseconds(30000);
    return r;
}

inline Json response_with_output(Array output) {
    return Object{{"id", "resp_offline_fixture"}, {"object", "response"},
        {"access_programs", nullptr}, {"input", Array{}}, {"reasoning_effort", nullptr},
        {"status", "completed"}, {"model", "fixture-responses-model"},
        {"output", std::move(output)}, {"error", nullptr}, {"incomplete_details", nullptr},
        {"usage", Object{{"input_tokens", 9}, {"output_tokens", 3}, {"total_tokens", 12},
            {"input_tokens_details", Object{{"cached_tokens", 2}, {"cache_write_tokens", 1}}},
            {"output_tokens_details", Object{{"reasoning_tokens", 0}}}}}};
}

inline Json text_response(const std::string& text = "fixture response") {
    return response_with_output(Array{Object{{"id", "msg_offline_fixture"},
        {"type", "message"}, {"role", "assistant"}, {"status", "completed"},
        {"content", Array{Object{{"type", "output_text"}, {"text", text},
                                {"annotations", Array{}}}}}}});
}

inline Json tool_response() {
    return response_with_output(Array{Object{{"id", "fc_offline_fixture"},
        {"type", "function_call"}, {"call_id", "call_provider_fixture"},
        {"name", "note"}, {"arguments", "{\"message\":\"fixture\"}"},
        {"status", "completed"}}});
}
} // namespace wls::test::protocol
