#pragma once
#include "../live_test_support.hpp"
#include <openssl/evp.h>

// Synthetic pre-E2 materials. Fingerprints were captured once with the saved
// a83bdd8 libraries; no Store, endpoint or credentials participate in encoding.
namespace wls::test::e2 {
inline s1::ContextInput context(bool chat = false) {
    auto input = protocol::context();
    input.role = Role::Dispatcher; input.model.model = "local-model";
    input.protocol_version = chat ? "openai-chat-v1" : "openai-responses-v1";
    input.tools.front().description = s1::builtin_tool_description("note");
    input.tools.front().definition.at("properties").at("message").as_object()["description"] = "Original observation.";
    input.task_contract = s1::canonical_json(live::Object{
        {"request", live::Object{{"requirements_revision", 1}, {"texts", live::Array{"原始需求😀"}}, {"constraint_catalog", live::Array{}}}},
        {"task", nullptr}, {"instruction", nullptr}, {"binding", live::Object{{"execution_epoch", 1}}},
        {"pending_events", live::Array{live::Object{{"type", "future-event"}, {"result", "retain original"}}}},
        {"budget", live::Object{{"unknown_cost_attempts", 1}, {"total_cost_micro_usd", nullptr}}}});
    return input;
}
inline s1::ModelRequest request(bool chat, int format) {
    auto input = context(chat); if (format == 4) input.model_projection = "lean-v1";
    auto request = protocol::request(); request.role = input.role; request.model = input.model;
    request.context = format == 1 ? s1::ContextCompiler{}.compile(input) : s1::compile_structured_context(input, {}, {},
        format == 2 ? s1::StructuredContextFormat::V2 : s1::StructuredContextFormat::V3);
    request.max_output_tokens = request.reserved_output_tokens = 100;
    request.reserved_input_tokens = 1000; request.reserved_cost_micro_usd = 100;
    request.timeout = std::chrono::seconds(3);
    return request;
}
inline constexpr const char* hashes[2][4][3] = {
    {
        {"d0361fd57782dfde492788b21d705207c5a8d101f3f8164bcb294dc715765e35", "", ""},
        {"532d7d223ccd288fd765a834ea04d20d787bfdb6ea1ccc1efac6182f233a852a", "158828440e858d840e1369a0d71b9da7a3730442bbfb626bfe8ad6d8a9b35527", "b4135d00b966f50e3cbeeeff26f245c3df84f25b01c541833b2898f10a910b65"},
        {"d2d2e85493160272397051d15ced1ad1eff27be5eb95e7c1ba53997f03e91aa7", "efd1329ee5886b8fb39cfae6d8d6f76d9ab17b0a5090799500634ab9f0e9808c", "4a0eccdb8aff6cc73c8081e6d1ab0607abef5d7d585e9311e98b91f81df1bdc3"},
        {"911367577ce8b64a5cfa65a89c193cf3bcb30623bb788c25fe0f7d9b122a0acd", "52de10a7662c3ddd1cfb32f89c29050aa68da8b5d380811cfb38246da082faaf", "b47759077e48901f0528e8391c5b9975668f4d0e91ada1c1dae146423e979f5e"},
    },
    {
        {"fcf975e9e6816898d1b56dfd6c6ccb1cfbb9329db80da0148d3d061cda3190c9", "", ""},
        {"322d0bb3e08f9ed7bd8d1a06bc8d5689d0bbdb48d89217dbd218934bfa7724d7", "3d737e971555eaa0f117dd66d005013e0b214e86ae093b1805dd4d97d94b9f2b", "819c2e3d135eaa89850b9f267b602ce4c9c6207f1e740df127b3856612fd09fc"},
        {"bfa84137bc8038e9813f789cff27af2d350b7742604cbaf949dc1d50f3914b4f", "484e2bd7f51eef350d230355d02ef914f44dd72e5c49bf74d11793be5d8dc72c", "db9b778b40c785ba54f6ebdbdc0bac99d95dda9439899f9b78a4c385183fc0c1"},
        {"963d621f0ff96615193c33795b446f93ebe29dcaa91fb87dc144cbe89175b43f", "aba6c35fa7b80ddd6658503396241d086c308d1eae9b6eb417f5cc55133093c5", "4a9d5b9d7f81d1155140574543c86fefb9def07877a548876817ff853f93100f"},
    },
};
} // namespace wls::test::e2

// Self-created deterministic data. Built against the saved e81ca97 library.
// compact_model_state is captured; actionable_model_state is deliberately absent.
namespace wls::test::e81_compact {
using s1::Json;
using Object = boost::json::object;
using Array = boost::json::array;
struct Case { const char* name; Role role; const char* kind; bool cross_version; };
inline constexpr Case cases[] = {
    {"tl-submission-same", Role::TeamLead, "submission", false},
    {"dispatcher-acceptance-same", Role::Dispatcher, "acceptance", false},
    {"worker-events", Role::Worker, "none", false},
};
inline std::string artifact_bytes() {
    std::string bytes;
    for (int i = 0; i != 12; ++i) bytes += "Synthetic original bytes. 原始字节😀\n";
    return bytes;
}
inline std::string base64(const std::string& bytes) {
    std::string encoded(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    const auto count = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    if (count < 0) throw std::runtime_error("synthetic base64 failed");
    encoded.resize(static_cast<std::size_t>(count)); return encoded;
}
inline Json state(const Case& selected) {
    const Json binding = s1::to_json(protocol::request().binding);
    const auto bytes = artifact_bytes();
    const Json file = Object{{"path", "result/说明.txt"}, {"sha256", s1::sha256_hex(bytes)},
        {"size_bytes", static_cast<std::int64_t>(bytes.size())}, {"content_base64", base64(bytes)}};
    const Json view = Object{{"path", file.at("path")}, {"sha256", file.at("sha256")},
        {"size_bytes", file.at("size_bytes")}, {"content", bytes}, {"binary", false},
        {"source", "immutable_submission_snapshot"}};
    const Json body = Object{{"summary", "Original immutable submission. " + std::string(384, 's')},
        {"self_check", Array{"Original requirement and exact artifact checked."}},
        {"verification", "Original verification result."}, {"signed_zero", -0.0}};
    const Json check_body = Object{{"verification", "Original independent check. " + std::string(288, 'v')},
        {"passed", true}, {"missing_requirements", Array{}}};
    const Json acceptance = Object{{"submission_id", "submission-e81"}, {"captured_binding", binding},
        {"submission_version", 4}, {"task_version", 17}, {"business_accepted", true}, {"delivered", false},
        {"decision", Object{{"rationale", "Original acceptance. " + std::string(288, 'a')}}}};
    const Json submission = Object{{"submission_id", "submission-e81"}, {"request_id", binding.at("request_id")},
        {"task_id", binding.at("task_id")}, {"team_id", binding.at("team_id")}, {"state_version", 4},
        {"original_binding", binding}, {"record_sha256", s1::sha256_hex(s1::canonical_json(Object{
            {"original_binding", binding}, {"body", body}, {"artifact_snapshot", Array{file}}}))},
        {"body", body}, {"artifact_snapshot", Array{file}}, {"status", "CHECKED"},
        {"checks", Array{Object{{"submission_id", "submission-e81"}, {"captured_binding", binding},
            {"submission_version", 4}, {"status", "CHECKED"}, {"check_body", check_body}}}}, {"acceptance", acceptance}};
    auto current = submission;
    auto order = binding; order.as_object()["approach"] = std::string(384, 'i');
    order.as_object()["steps"] = Array{"Preserve original bytes", "Wait while UNKNOWN remains"};
    order.as_object()["done_definition"] = "Exact immutable artifact and settled original effects.";
    auto task = binding; task.as_object()["brief"] = Object{{"goal", "Synthetic immutable-artifact task."}};
    task.as_object()["based_on_requirements_revision"] = 1; task.as_object()["state_version"] = 17;
    task.as_object()["applied_instruction_revision"] = 0; task.as_object()["paused_for_change"] = true;
    task.as_object()["created_at"] = "2030-01-01T00:00:00Z"; task.as_object()["updated_at"] = "2030-01-01T00:01:00Z";
    auto instruction = binding; instruction.as_object()["body"] = order;
    instruction.as_object()["based_on_requirements_revision"] = 1; instruction.as_object()["state_version"] = 19;
    instruction.as_object()["created_at"] = "2030-01-01T00:00:00Z";
    const auto event = [&](const char* type, const Json& payload, const Json& result, int index) -> Json {
        return Object{{"inbox_id", 100 + index}, {"event_seq", 500 + index}, {"seq", 500 + index},
            {"request_id", binding.at("request_id")}, {"role", s1::role_name(selected.role)},
            {"actor_task_id", selected.role == Role::Dispatcher ? Json("") : binding.at("task_id")},
            {"subject_task_id", binding.at("task_id")}, {"event_requirements_revision", 1},
            {"status", "CLAIMED"}, {"claim_id", "synthetic-claim-e81"}, {"completed_at", nullptr},
            {"type", type}, {"created_at", "2030-01-01T00:02:00Z"},
            {"actor", Object{{"role", "worker"}, {"principal_id", "synthetic-worker"}, {"team_id", binding.at("team_id")}}},
            {"payload", payload}, {"result", result}, {"future_route_note", "Unknown original field remains."}};
    };
    Array events{
        event("submit_work", Object{{"type", "submit_work"}, {"binding", binding}, {"body", body}}, submission, 1),
        event("check_submission", Object{{"type", "check_submission"}, {"binding", binding},
            {"submission_id", "submission-e81"}, {"expected_submission_version", 3}, {"check_body", check_body}}, submission, 2),
        event("accept_candidate", Object{{"type", "accept_candidate"}, {"binding", binding},
            {"submission_id", "submission-e81"}, {"expected_submission_version", 4}, {"expected_task_version", 17},
            {"decision", acceptance.at("decision")}}, submission, 3)};
    if (selected.role == Role::Worker) {
        auto previous = binding; previous.at("instruction_revision") = 0;
        const Json feedback = Object{{"attempt_id", "synthetic-attempt-original"},
            {"record_sha256", s1::sha256_hex("synthetic original tool feedback")}};
        auto payload = feedback; payload.as_object()["type"] = "tool_feedback"; payload.as_object()["binding"] = binding;
        events = Array{
            event("instruction", Object{{"type", "instruction"}, {"binding", previous}, {"body", order},
                {"expected_task_version", 16}}, Object{{"binding", binding}, {"state_version", 17}}, 1),
            event("ack", Object{{"type", "ack"}, {"binding", binding}}, Object{{"binding", binding}, {"state_version", 18}}, 2),
            event("tool_feedback", payload, feedback, 3)};
    }
    events.push_back(event("future-synthetic-event", Object{{"type", "future-synthetic-event"},
        {"binding", binding}, {"expected_task_version", 17}}, Object{{"status", "UNKNOWN"},
        {"original_observation", nullptr}, {"operation_id", "synthetic-unknown-operation"}}, 4));
    Json result = Object{{"compact_model_state", true},
        {"request", Object{{"request_id", binding.at("request_id")}, {"requirements_revision", 1},
            {"texts", Array{"Synthetic original requirement. 保留原始约束😀"}}, {"constraint_catalog", Array{}}}},
        {"binding", binding}, {"captured_state_version", 23}, {"task", task}, {"instruction", instruction},
        {"submission", current}, {"workflow_action", Object{{"kind", selected.kind}, {"target", current}}},
        {"submission_file_views", Array{view}}, {"pending_events", events},
        {"budget", Object{{"request_id", binding.at("request_id")}, {"state_version", 9},
            {"max_attempts", 64}, {"attempts", 7}, {"input_limit", 100000}, {"output_limit", 10000}, {"cost_limit", 0},
            {"spent_input", 2400}, {"reserved_input", 700}, {"unknown_input", 200},
            {"spent_output", 240}, {"reserved_output", 70}, {"unknown_output", 20},
            {"spent_cost", 0}, {"reserved_cost", 0}, {"unknown_cost", 0}, {"cost_policy", "observe-only-v1"},
            {"money_limit_micro_usd", nullptr}, {"total_cost_micro_usd", nullptr}, {"unknown_cost_attempts", 1},
            {"cost_breakdown", Object{{"reported_micro_usd", nullptr}, {"unknown_attempts", 1}}}}},
        {"questions", Array{Object{{"question_id", "synthetic-question"}, {"status", "OPEN"}, {"text", "Original open question?"}}}},
        {"wait", Object{{"generation", 3}, {"reason", "question"}, {"question_id", "synthetic-question"}}},
        {"unresolved_operations", Array{Object{{"status", "UNKNOWN"}, {"operation_id", "synthetic-unknown-operation"}}}},
        {"unresolved_attempts", Array{"synthetic-attempt-original"}}, {"allowed_tools", Array{"note", "fs_read", "submit"}},
        {"file_path_policy", "work-relative-v1"}, {"execution_policy_sha256", s1::sha256_hex("synthetic policy")},
        {"history_policy", Object{{"mode", "window-v1"}, {"scope", "synthetic-original-scope"}}}};
    if (selected.role == Role::Worker) result.at("workflow_action") = nullptr;
    return result;
}
inline s1::ProviderProfile profile(bool chat) {
    auto result = protocol::profile();
    if (chat) {
        result.protocol = "openai-chat-v1"; result.responses_path.clear(); result.chat_completions_path = "/v1/chat/completions";
        result.streaming = false; result.chat_reasoning_policy = "reasoning-effort-v1";
        result.response_metadata_policy = "ignore-record-v1";
    }
    return result;
}
inline s1::ContextInput context(bool chat, const Case& selected) {
    auto input = protocol::context(); input.role = selected.role;
    input.role_prompt = std::string(s1::role_name(selected.role)) + ": preserve the captured original.";
    input.protocol_version = chat ? "openai-chat-v1" : "openai-responses-v1"; input.model_projection = "lean-v1";
    input.tools.front().description = s1::builtin_tool_description("note");
    input.tools.front().definition.at("properties").at("message").as_object()["description"] = "Original observation.";
    input.task_contract = s1::canonical_json(state(selected));
    return input;
}
inline s1::ModelRequest request(bool chat, const Case& selected) {
    const auto input = context(chat, selected);
    auto result = protocol::request(); result.role = selected.role;
    result.call_id = std::string("call-e81-") + selected.name; result.attempt_id = std::string("attempt-e81-") + selected.name;
    result.context = s1::compile_structured_context(input, {}, {}, s1::StructuredContextFormat::V3);
    return result;
}
// Independent saved e81 library; full originals: e2-e81-compact-golden-z8e2hwp0.
inline constexpr const char* hashes[2][3][3] = {
    {
        {"6ff5baa7e43a5f9ff7167504c679e3f7f2da59657682f91ba5f4dac9db63f627", "f8e1023eb6ba5feefa80f894382e80deae70ca71efef11d17ed3355388985843", "bdb4cd2a87767076d0666fc95c99710213349e9cfcaca20b9620b2300eec1479"},
        {"fd55f13a651dd9e47985a4100834b615d7815c2a55a88263de862035083ce67a", "2de1919e573ecae805a05b4fe8dfffbbed1b6ec189d0c8ea9a84ff2f9b04f514", "a2335d31d0ec49618aafea2e1f9e642e1b65fc7bbad3cbaf6c194dda7695e29c"},
        {"603e9c6a3278236bc0d7c4a202272e396648312949190e364a7194a094dd38c4", "61db15f1528129d5872e357ab5a2a8f598b34f33e8cbf469d911c34f501d0256", "862e8296ac12c1dc13b8aeb4e8fec06c61bacf21f0d25dbf10b0960148424c41"},
    },
    {
        {"b4ff100b8154c06cd5314c6bc9229536add1f4eac015c5ee657d52acefdd0c07", "b9e59f76d2a86007f11344218ee47042b4d9dd0b7c8c5ca7aa33f61fdbec0cfa", "5f503148d1ed100313c6e222b1ee6426a2488348e317992e120902ce5d48963f"},
        {"56094814bca2121d2275baff5cfbcf0e92af57064127b482350cf215e4d475dc", "b5082585e70f074899a595cc3499fa2137d235e9f800abda9c958a5c3735d5a0", "05b8f79afc29ef688972e423f6ce04c10a1879d1215b34e285881786be8d5f5e"},
        {"613845a4f22da77cf552d927e5c1fb434832d07aab450e90716e6ed0e847bd54", "16145692b67a0a61650a02018f3e8b24654b20521149f3b6c1577ccc70ed7ae5", "2cecfe7405e5d11fbb7b2145ab32e071e959ff47c61a6f5f3ba7665208292e2a"},
    },
};
} // namespace wls::test::e81_compact
