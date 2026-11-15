#pragma once
#include "../live_test_support.hpp"

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
