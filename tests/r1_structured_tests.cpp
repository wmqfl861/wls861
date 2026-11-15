#include "legacy_shell_config.hpp"
#include "wls/structured_context.hpp"
#include "wls/offline_runtime.hpp"
#include "wls/worker_history.hpp"
#include "wls/management_context.hpp"
#include "wls/single_agent.hpp"
#include "wls/worker_tools.hpp"
#include "protocol_test_support.hpp"
#include "live_test_support.hpp"
#include "test_support.hpp"
#include "fake_script_support.hpp"
#include "fixtures/e2_state_golden.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <regex>
#include <set>

namespace {
using namespace wls;
using namespace wls::s1;
namespace fixture = wls::test::protocol;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path root(WLS_SOURCE_DIR);
void fails(ErrorCode code, const std::function<void()>& operation) {
    try { operation(); } catch (const Error& error) {
        if (error.code() != code) throw std::runtime_error(error.what());
        return;
    }
    throw std::runtime_error("expected refusal");
}
std::string str(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
ContextInput base() {
    ContextInput input;
    input.role = Role::Worker; input.model = {"fake", {ReasoningMode::Disabled, ""}};
    input.adapter_version = "1"; input.protocol_version = "fake-v1";
    input.system_prompt = "trusted system"; input.role_prompt = "Worker only";
    input.function_prompt = "retain exact source evidence"; input.environment_card = "offline";
    input.approved_rules = "UNKNOWN blocks new business"; input.project_snapshot = "project revision 1";
    input.task_contract = "原始约束😀\nAMEND must remain exact";
    input.tools = {{"note", "1", Object{{"type", "object"}}}, {"fs_read", "2", Object{{"type", "object"}}}};
    input.dependencies = {{"z", "1", sha256_hex("z")}, {"a", "1", sha256_hex("a")}};
    return input;
}
std::vector<StructuredContextMessage> turn() {
    return {{"assistant", "read original", {{"one", "fs_read", Object{{"path", "a.txt"}}}}, {}, nullptr},
            {"tool", "{\"original\":\"字节😀\"}", {}, "one", Object{{"old_revision", 1}}}};
}
ModelRequest request() {
    ModelRequest r; r.call_id = new_id("call"); r.attempt_id = new_id("attempt");
    r.role = Role::Worker; r.binding.request_id = new_id("req"); r.binding.task_id = new_id("tsk");
    r.binding.team_id = new_id("team"); r.binding.plan_revision = 1;
    r.binding.task_revision = 1; r.binding.instruction_revision = 1;
    r.config_snapshot_id = sha256_hex("fixture"); r.context = compile_structured_context(base(), turn());
    r.model = r.context.model; r.max_output_tokens = 128;
    r.reserved_input_tokens = 8192; r.reserved_output_tokens = 128;
    return r;
}
Json attempt(const std::string& tool_id = "reused") {
    const auto r = request();
    ModelResponse response; response.call_id = r.call_id; response.attempt_id = r.attempt_id;
    response.role = r.role; response.role_call_index = r.role_call_index; response.binding = r.binding;
    response.config_snapshot_id = r.config_snapshot_id; response.text = "original assistant";
    response.tool_calls = {{tool_id, "fs_read", Object{{"path", "original.txt"}}}};
    return Object{{"attempt_id", r.attempt_id}, {"call_id", r.call_id}, {"accepted", 1},
        {"frozen_binding", to_json(r.binding)}, {"response", to_json(response)}, {"status", "COMPLETED"},
        {"feedback", Object{{"original_binding", to_json(r.binding)}, {"response", to_json(response)},
            {"record_sha256", sha256_hex("test receipt")}, {"result", Object{{"content", "original evidence"}}}}},
        {"feedback_missing", false}};
}

void separate_fields_and_round_trip() {
    const auto input = base(); const auto c = compile_structured_context(input, turn());
    CHECK(c.structured()); CHECK(!c.prompt().starts_with("WLS-CONTEXT/1"));
    CHECK(c.logical_input.at("prefix").at("tools").as_array().front().at("name") == "fs_read");
    CHECK(str(c.logical_input.at("prefix").at("system"), "system_prompt") == input.system_prompt);
    CHECK(compiled_task_contract(c) == input.task_contract);
    CHECK(c.logical_input_sha256 == sha256_hex(c.prompt()));
    CHECK(to_json(c).at("wire_sha256").is_null());
    CHECK(to_json(compiled_context_from_json(to_json(c))) == to_json(c));
}
void canonical_tools_and_dependencies() {
    auto input = base(); const auto original = compile_structured_context(input, turn());
    std::reverse(input.tools.begin(), input.tools.end()); std::reverse(input.dependencies.begin(), input.dependencies.end());
    CHECK(to_json(compile_structured_context(input, turn())) == to_json(original));
    input.tools.push_back(input.tools.front());
    fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(input, {}); });
}
void cache_and_full_request_are_distinct() {
    auto input = base(); const auto first = compile_structured_context(input, turn());
    input.task_contract += "\nnew requirement"; auto messages = turn(); messages.back().content += " more";
    const auto changed = compile_structured_context(input, messages);
    CHECK(changed.prefix_sha256 == first.prefix_sha256); CHECK(changed.cache_identity == first.cache_identity);
    CHECK(changed.logical_input_sha256 != first.logical_input_sha256);
    input.protocol_version = "different-v2"; const auto protocol = compile_structured_context(input, messages);
    CHECK(protocol.cache_identity != changed.cache_identity);
    input.model.reasoning = {ReasoningMode::Explicit, "max"};
    CHECK(compile_structured_context(input, messages).cache_identity != protocol.cache_identity);
    input.approved_rules += "revised";
    CHECK(compile_structured_context(input, messages).prefix_sha256 != protocol.prefix_sha256);
}
void pairing_and_role_refusals() {
    auto m = turn(); m.pop_back(); fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(base(), m); });
    m = turn(); m.erase(m.begin()); fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(base(), m); });
    m = turn(); m.back().tool_call_id = "other"; fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(base(), m); });
    m = turn(); auto repeated = turn(); m.insert(m.end(), repeated.begin(), repeated.end());
    fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(base(), m); });
    m = turn(); m.front().role = "system"; fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(base(), m); });
    m = turn(); m.front().role = "user"; fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(base(), m); });
    m = turn(); m.insert(m.begin() + 1, {"user", "interrupt", {}, {}, nullptr});
    fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(base(), m); });
}
void parallel_turn_requires_all_results() {
    auto m = turn(); m.front().tool_calls.push_back({"two", "note", Object{}});
    m.push_back({"tool", "null", {}, "two", nullptr});
    CHECK(compile_structured_context(base(), m).structured());
    m.back().tool_call_id = "one"; fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(base(), m); });
}
void forged_hash_does_not_authorize_structure() {
    auto value = to_json(compile_structured_context(base(), turn()));
    value.at("input").at("messages").as_array().back().as_object()["tool_call_id"] = "forged";
    value.as_object()["logical_input_sha256"] = sha256_hex(canonical_json(value.at("input")));
    fails(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(value); });
    value = to_json(compile_structured_context(base(), turn())); value.as_object()["wire_sha256"] = value.at("logical_input_sha256");
    fails(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(value); });
    value = to_json(compile_structured_context(base(), turn())); value.at("input").at("prefix").as_object()["authority"] = "root";
    fails(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(value); });
}
void metadata_tampering_rejects() {
    for (const auto* key : {"role", "model_ref", "adapter_version", "prefix_sha256", "cache_identity", "logical_input_sha256"}) {
        auto value = to_json(compile_structured_context(base(), turn())); value.as_object()[key] = "tampered";
        fails(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(value); });
    }
    auto c = compile_structured_context(base(), turn()); c.dynamic_suffix += "x";
    fails(ErrorCode::InvalidInput, [&] { (void)to_json(c); });
}
void bounds_and_legacy_messages_refuse() {
    auto input = base(); input.messages.push_back({"tool", "unconverted"});
    fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(input, {}); });
    input = base(); auto m = turn(); m.back().content = std::string(1048576, 'x');
    fails(ErrorCode::Quota, [&] { (void)compile_structured_context(input, m); });
    auto limits = ContextLimits{}; limits.max_messages = 1;
    fails(ErrorCode::Quota, [&] { (void)compile_structured_context(base(), turn(), limits); });
    CHECK(input.task_contract == base().task_contract);
}
void legacy_bytes_unchanged() {
    auto input = base(); input.messages = {{"tool", "legacy record"}};
    const auto c = ContextCompiler().compile(input); const auto bytes = canonical_json(to_json(c));
    CHECK(!c.structured()); CHECK(c.prompt() == c.stable_prefix + c.dynamic_suffix);
    CHECK(compiled_task_contract(c) == input.task_contract);
    CHECK(canonical_json(to_json(compiled_context_from_json(parse_json_strict(bytes)))) == bytes);
    CHECK(!to_json(c).as_object().contains("logical_input_sha256"));
}
void legacy_full_byte_fingerprints() {
    // Captured from the unchanged ce6fe0d product sources, not this implementation.
    auto r = fixture::request();
    r.context = ContextCompiler{}.compile(fixture::context());
    CHECK(sha256_hex(canonical_json(to_json(r))) == "3d599a427fb8f068a7f3f4cef920de0ab82436587e9694474ca1c3a6d008661c");
    CHECK(sha256_hex(canonical_json(to_json(r.context))) == "ea5b7ce1ffa8b5753dbe8703d915cf5eb2ecc24378163bc6387615a12bc264a4");
    CHECK(r.context.prefix_sha256 == "5cee3d963ba92db4540c562c8f70ff12cbbbeadc9b7d9c926ffa663c81a0f205");
    CHECK(r.context.cache_identity == "5470dda1ff3e946b78e07b3263303324977aa9f7c7dbd79925b3b8b8e12d33f1");
    CHECK(to_json(model_request_from_json(to_json(r))) == to_json(r));
    r = fixture::request();
    CHECK(sha256_hex(canonical_json(to_json(r))) == "3a45973e2c457421a0a9a9302735c780a9224d645b5a684b304742183a0e2b2a");
    CHECK(sha256_hex(canonical_json(to_json(r.context))) == "68205fcecda7704acd27291dcddf53cebd18b0487ad51b15cb216a3d8f67ea90");
    CHECK(r.context.prefix_sha256 == "7ec1a07fa60837358643b390a768bce18bfd373d83baa624c979028fcf2aaa3e");
    CHECK(r.context.cache_identity == "564585c2d02ee8a98fddf014568f56caff921c41021d7519a04a7c727d2a182c");
    CHECK(r.context.logical_input_sha256 == "128e9a2535f757b12479a2501a17aad699958dcf3c0641ce15fb6e4818c1e7d2");
    CHECK(to_json(r).at("logical_request_sha256") == "33bbc68d70389621f98051ccae9899cb846ea49f405371c712c1e7c5210037ac");
    CHECK(encode_responses_request(fixture::profile(), r).body_sha256 == "9c2517780d9e1b96f29af56247fd474fdc8391ad5f09fb5f86c37ce2be4b8d82");
    CHECK(to_json(model_request_from_json(to_json(r))) == to_json(r));
}
void pre_e2_wire_and_envelope_goldens() {
    for (const bool chat : {false, true}) for (int format = 1; format <= 4; ++format) {
        const auto request = wls::test::e2::request(chat, format);
        const auto before = canonical_json(to_json(request));
        const auto& expected = wls::test::e2::hashes[chat][format - 1];
        CHECK(sha256_hex(before) == expected[0]);
        if (format != 1) {
            const auto profile = wls::test::live::settings("synthetic trust", 443, chat).provider;
            const auto encoded = encode_protocol_request(profile, request);
            CHECK(encoded.body_sha256 == expected[1]); CHECK(encoded.record_sha256 == expected[2]);
        }
        CHECK(canonical_json(to_json(request)) == before);
    }
    namespace e81 = wls::test::e81_compact;
    for (const bool chat : {false, true}) for (std::size_t i = 0; i < std::size(e81::cases); ++i) {
        const auto request = e81::request(chat, e81::cases[i]); const auto before = canonical_json(to_json(request));
        const auto encoded = encode_protocol_request(e81::profile(chat), request); const auto& expected = e81::hashes[chat][i];
        CHECK(sha256_hex(before) == expected[0]); CHECK(encoded.body_sha256 == expected[1]);
        CHECK(encoded.record_sha256 == expected[2]); CHECK(canonical_json(to_json(request)) == before);
        CHECK(canonical_json(to_json(encode_protocol_request(e81::profile(chat), model_request_from_json(to_json(request))))) ==
            canonical_json(to_json(encoded)));
    }
}
Json e2_submission() {
    const auto binding = to_json(fixture::request().binding);
    return Object{{"submission_id", "submission-current"}, {"request_id", binding.at("request_id")},
        {"task_id", binding.at("task_id")}, {"team_id", binding.at("team_id")}, {"state_version", 4},
        {"original_binding", binding}, {"record_sha256", sha256_hex("immutable original submission")},
        {"body", Object{{"summary", std::string(400, 's')}, {"signed_zero", -0.0}, {"self_check", Array{"original self-check"}},
            {"verification", "original verification"}}}, {"artifact_snapshot", Array{}},
        {"status", "CHECKED"}, {"checks", Array{Object{{"check_body", Object{{"verification", std::string(300, 'v')}}},
            {"submission_id", "submission-current"}, {"captured_binding", binding}, {"submission_version", 4}, {"status", "CHECKED"}}}},
        {"acceptance", Object{{"decision", std::string(300, 'a')}, {"business_accepted", true}, {"delivered", false}}}};
}
Json e2_contract(Role role, const std::string& kind) {
    auto state = parse_json_strict(wls::test::e2::context().task_contract);
    const auto submission = e2_submission(); const auto binding = submission.at("original_binding");
    state.as_object()["compact_model_state"] = true; state.at("binding") = binding;
    state.as_object()["captured_state_version"] = 7;
    state.as_object()["submission"] = submission;
    state.as_object()["workflow_action"] = Object{{"kind", kind}, {"target", submission}};
    state.at("pending_events") = Array{Object{{"type", "submit_work"}, {"event_id", "event-original"},
        {"request_id", binding.at("request_id")}, {"subject_task_id", binding.at("task_id")},
        {"role", role_name(role)}, {"status", "PENDING"}, {"claim_id", "captured-claim"},
        {"event_requirements_revision", 1}, {"actor", Object{{"principal_id", "original-principal"}}},
        {"payload", Object{{"type", "submit_work"}, {"binding", binding}, {"body", submission.at("body")}}},
        {"result", submission}, {"future_field", Object{{"source", "business source"}, {"metadata", "retain"}}}}};
    state.as_object()["questions"] = Array{Object{{"status", "OPEN"}, {"text", "original question?"},
        {"forwarded_notes", Array{"original forward"}}, {"answer", "answer only this generation"}}};
    state.as_object()["wait"] = Object{{"generation", 3}, {"reason", "question"}};
    state.as_object()["unresolved_operations"] = Array{Object{{"status", "UNKNOWN"}, {"operation_id", "op-original"}}};
    state.as_object()["unresolved_attempts"] = Array{"unknown-original-attempt"};
    state.as_object()["last_correction"] = Object{{"original_binding", binding}, {"reason", "missing_leaf"}};
    state.as_object()["allowed_tools"] = Array{"fs_read", "submit"};
    state.as_object()["file_path_policy"] = "work-relative-v1";
    state.as_object()["execution_policy_sha256"] = sha256_hex("original policy");
    state.as_object()["history_policy"] = Object{{"mode", "window-v1"}, {"scope", "original-scope"}};
    state.as_object()["history_catalog"] = Array{"original-history-reference"};
    state.as_object()["logical_agent_scope"] = "original-shared-scope";
    state.as_object()["tasks"] = Array{Object{{"node_id", "required-node"}, {"required", true},
        {"review", Object{{"required", true}}}, {"depends_on", Array{"earlier"}}, {"brief", "original goal"}}};
    state.as_object()["plan"] = Object{{"nodes", state.at("tasks")}, {"plan_revision", 1}};
    state.at("budget").as_object()["cost_policy"] = "observe-only-v1";
    state.at("budget").as_object()["input_limit"] = 10000;
    state.at("budget").as_object()["unknown_input"] = 100;
    state.at("budget").as_object()["over_reserved"] = true;
    if (kind == "experience") {
        auto current = binding; current.at("requirements_revision") = 3;
        state.at("binding") = current; state.at("request").at("requirements_revision") = 3;
        state.at("workflow_action").at("target") = Object{
            {"control_binding", current}, {"original_source_binding", binding}, {"source_sha256", sha256_hex("old source")},
            {"source_snapshot", Object{{"submission", submission}, {"acceptance", submission.at("acceptance")},
                {"requirements", "original requirement"}, {"brief", "original brief"}, {"instruction", "original instruction"}}}};
    }
    return state;
}
Json e2_wire_state(Json contract, Role role, bool chat) {
    auto input = wls::test::e2::context(chat); input.role = role; input.model_projection = "lean-v1";
    input.task_contract = canonical_json(contract); auto request = wls::test::e2::request(chat, 4);
    const std::vector<StructuredContextMessage> history{{"user", "original history-window index", {}, {}, Object{{"kind", "history_window"}}}};
    request.role = role; request.context = compile_structured_context(input, history, {}, StructuredContextFormat::V3);
    const auto before = canonical_json(to_json(request));
    const auto body = parse_json_strict(encode_protocol_request(wls::test::live::settings("synthetic trust", 443, chat).provider, request).body);
    CHECK(canonical_json(to_json(request)) == before);
    CHECK(!request.context.logical_input.at("contract_anchor").as_object().contains("compact_model_state"));
    CHECK(!request.context.logical_input.at("contract_anchor").as_object().contains("actionable_model_state"));
    const auto state = parse_json_strict(str(body.at(chat ? "messages" : "input").as_array().back(), "content"));
    return chat ? state : state.at("current_state");
}
Json actionable_wire(Json contract, Role role, bool chat) {
    contract.as_object()["actionable_model_state"] = true;
    return e2_wire_state(std::move(contract), role, chat);
}
void actionable_markers_and_budget_boundaries() {
    namespace e81 = wls::test::e81_compact;
    for (const bool chat : {false, true}) {
        auto base = e81::state(e81::cases[2]);
        for (const auto* key : {"compact_model_state", "actionable_model_state"})
            for (const auto& invalid : {Json(nullptr), Json(false), Json("true"), Json(1)}) {
                auto changed = base; changed.as_object()["actionable_model_state"] = true; changed.as_object()[key] = invalid;
                fails(ErrorCode::InvalidInput, [&] { (void)e2_wire_state(changed, Role::Worker, chat); });
            }
        auto missing = base; missing.as_object().erase("compact_model_state"); missing.as_object()["actionable_model_state"] = true;
        fails(ErrorCode::InvalidInput, [&] { (void)e2_wire_state(missing, Role::Worker, chat); });
        auto input = e81::context(chat, e81::cases[2]); const auto old = compile_structured_context(input, {}, {}, StructuredContextFormat::V3);
        auto next_contract = base; next_contract.as_object()["actionable_model_state"] = true; input.task_contract = canonical_json(next_contract);
        continue_structured_contract(input, Object{{"attempt_id", "e81-original"}, {"input", old.logical_input}});
        const auto next = compile_structured_context(input, {}, {}, StructuredContextFormat::V3);
        CHECK(next.logical_input.at("contract_updates").as_array().empty()); CHECK(next.logical_input.at("contract_anchor") == old.logical_input.at("contract_anchor"));
        CHECK(next.logical_input.at("current_state").at("semantic_versions") == old.logical_input.at("current_state").at("semantic_versions"));
        CHECK(next.logical_input_sha256 != old.logical_input_sha256);
        base.at("budget").as_object()["cost_breakdown"] = Object{{"estimated_micro_usd", nullptr}, {"reported_micro_usd", 0},
            {"reservation_charged_micro_usd", 0}, {"pricing_sha256", Array{sha256_hex("synthetic prices")}},
            {"unknown_reserved_micro_usd", 0}, {"known_cost_micro_usd", 0}, {"cost_policy", "observe-only-v1"},
            {"total_cost_micro_usd", nullptr}, {"unknown_cost_attempts", 1}};
        const auto budget = actionable_wire(base, Role::Worker, chat).at("budget");
        const Json remaining = Object{{"calls", 57}, {"input_tokens", 96900}, {"output_tokens", 9690}};
        CHECK(budget.at("remaining_at_capture") == remaining); // At capture, including this call; UNKNOWN is already reserved.
        for (const auto* key : {"unknown_input", "unknown_output", "unknown_cost", "unknown_cost_attempts", "total_cost_micro_usd", "money_limit_micro_usd", "cost_policy"})
            CHECK(budget.at(key) == base.at("budget").at(key));
        for (const auto* key : {"request_id", "state_version", "max_attempts", "attempts", "input_limit", "spent_input", "reserved_input", "cost_limit", "cost_breakdown"})
            CHECK(!budget.as_object().contains(key));
        const std::vector<std::pair<std::string, Json>> invalid{
            {"attempts", 65}, {"max_attempts", 64.0}, {"spent_input", 100001}, {"reserved_output", 10000},
            {"input_limit", std::uint64_t(INT64_MAX) + 1}, {"unknown_input", -1}, {"unknown_input", 701},
            {"unknown_output", 71}, {"unknown_cost", 1}, {"unknown_output", nullptr}, {"unknown_cost_attempts", "1"},
            {"total_cost_micro_usd", "0"}, {"remaining_at_capture", Object{}}, {"future_budget", 0}, {"over_reserved", true},
            {"cost_breakdown", Object{{"future_cost", 0}}}, {"cost_breakdown", Object{{"unknown_cost_attempts", 2}}},
            {"cost_breakdown", Object{{"unknown_reserved_micro_usd", 1}}}, {"cost_breakdown", Object{{"reported_micro_usd", 0.0}}}};
        for (const auto& [key, value] : invalid) {
            auto changed = base; changed.at("budget").as_object()[key] = value;
            CHECK(canonical_json(actionable_wire(changed, Role::Worker, chat).at("budget")) == canonical_json(changed.at("budget")));
        }
        auto limit = base; limit.at("budget").at("input_limit") = INT64_MAX;
        CHECK(actionable_wire(limit, Role::Worker, chat).at("budget").at("remaining_at_capture").at("input_tokens") == Json(INT64_MAX - 3100));
        auto priced = base; auto& ledger = priced.at("budget").as_object(); ledger.erase("cost_policy");
        ledger["cost_limit"] = 1000; ledger["spent_cost"] = 100; ledger["reserved_cost"] = 20; ledger["unknown_cost"] = 5;
        ledger["cost_breakdown"] = Object{{"estimated_micro_usd", 100}, {"reported_micro_usd", 0}, {"reservation_charged_micro_usd", 20}};
        const auto priced_wire = actionable_wire(priced, Role::Worker, chat).at("budget");
        CHECK(priced_wire.at("remaining_at_capture") == remaining);
        for (const auto* key : {"cost_limit", "spent_cost", "reserved_cost", "unknown_cost", "cost_breakdown"}) CHECK(priced_wire.at(key) == priced.at("budget").at(key));
        base.at("budget").at("cost_breakdown").as_object()["known_cost_micro_usd"] = 7;
        CHECK(actionable_wire(base, Role::Worker, chat).at("budget").at("cost_breakdown").at("known_cost_micro_usd") == 7);
    }
}
void actionable_events_and_task_boundaries() {
    namespace e81 = wls::test::e81_compact;
    for (const bool chat : {false, true}) {
        auto base = e81::state(e81::cases[2]); const auto binding = base.at("binding"); auto& task = base.at("task").as_object();
        task["kind"] = "team_task"; task["capability"] = nullptr; task["capability_input"] = nullptr; task["callee_request_id"] = nullptr;
        task["template_id"] = "synthetic-template"; task["review_config"] = nullptr; task["review_round"] = 0;
        const Json actor = Object{{"role", "team_lead"}, {"principal_id", "synthetic-lead"}, {"request_id", binding.at("request_id")},
            {"task_id", binding.at("task_id")}, {"team_id", binding.at("team_id")}, {"dispatcher_epoch", binding.at("dispatcher_epoch")}, {"execution_epoch", binding.at("execution_epoch")}};
        base.at("pending_events").as_array()[0].at("payload").as_object()["actor"] = actor;
        const auto wire = actionable_wire(base, Role::Worker, chat); const auto& events = wire.at("pending_events").as_array();
        CHECK(events.size() == base.at("pending_events").as_array().size());
        for (std::size_t i = 0; i != events.size(); ++i) for (const auto* key : {"type", "status", "subject_task_id", "event_requirements_revision", "future_route_note"})
            CHECK(events[i].at(key) == base.at("pending_events").as_array()[i].at(key));
        CHECK(events.back() == base.at("pending_events").as_array().back());
        for (const auto* key : {"inbox_id", "event_seq", "request_id", "role", "actor_task_id", "claim_id", "created_at", "completed_at"}) CHECK(!events[0].as_object().contains(key));
        CHECK(events[0].at("payload").at("actor").as_object().size() == 1 && events[0].at("payload").at("actor").at("role") == "team_lead");
        CHECK(events[2].at("payload").at("current_binding") == true && !events[2].as_object().contains("result"));
        for (const auto* key : {"attempt_id", "record_sha256"}) CHECK(events[2].at("payload").at(key) == base.at("pending_events").as_array()[2].at("payload").at(key));
        for (const auto* key : {"capability", "capability_input", "callee_request_id", "template_id", "review_round"}) CHECK(!wire.at("task").as_object().contains(key));
        for (const auto* key : {"state_version", "applied_instruction_revision", "paused_for_change", "kind"}) CHECK(wire.at("task").at(key) == base.at("task").at(key));
        for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{{"type", "AMEND"}, {"type", "cancel"}, {"type", "recovery"},
                {"type", "question"}, {"type", "future-event"}, {"request_id", "other-request"}, {"subject_task_id", "other-task"}, {"event_requirements_revision", 2}}) {
            auto changed = base; changed.at("pending_events").as_array()[0].as_object()[key] = value;
            CHECK(actionable_wire(changed, Role::Worker, chat).at("pending_events").as_array()[0] == changed.at("pending_events").as_array()[0]);
        }
        auto unknown_actor = base; auto& original_actor = unknown_actor.at("pending_events").as_array()[0].at("payload").at("actor");
        original_actor.as_object()["future_authority"] = "retain";
        CHECK(actionable_wire(unknown_actor, Role::Worker, chat).at("pending_events").as_array()[0].at("payload").at("actor") == original_actor);
        for (int variant = 0; variant != 4; ++variant) {
            auto changed = base;
            if (variant < 3) changed.at("task").at(variant == 0 ? "capability" : variant == 1 ? "capability_input" : "callee_request_id") = "non-null lane";
            else changed.at("task").as_object().erase("capability");
            CHECK(actionable_wire(changed, Role::Worker, chat).at("task").at("template_id") == "synthetic-template");
        }
        for (const auto& round : {Json(0.0), Json(-0.0), Json(1)}) {
            auto changed = base; changed.at("task").at("review_round") = round;
            CHECK(canonical_json(actionable_wire(changed, Role::Worker, chat).at("task").at("review_round")) == canonical_json(round));
        }
        auto missing = base; missing.at("pending_events").as_array()[2].as_object().erase("result");
        CHECK(!actionable_wire(missing, Role::Worker, chat).at("pending_events").as_array()[2].as_object().contains("result"));
        auto collision = base; collision.at("pending_events").as_array()[2].at("payload").as_object()["current_binding"] = "original business field";
        CHECK(actionable_wire(collision, Role::Worker, chat).at("pending_events").as_array()[2].at("payload").at("current_binding") == "original business field");
    }
}
void actionable_dispatcher_originals_and_references() {
    namespace e81 = wls::test::e81_compact;
    for (const bool chat : {false, true}) {
        auto base = e81::state(e81::cases[1]); const auto producer = base.at("submission").at("original_binding");
        // Inserting keys can reallocate base, so own both source values first.
        const auto acceptance_task = base.at("task");
        const auto acceptance_instruction = base.at("instruction");
        base.as_object()["acceptance_task"] = acceptance_task;
        base.as_object()["acceptance_instruction"] = acceptance_instruction;
        auto& source_check = base.at("workflow_action").at("target").at("checks").as_array()[0];
        source_check.as_object()["actor"] = Object{{"role", "team_lead"}, {"principal_id", "synthetic-lead"}, {"request_id", producer.at("request_id")},
            {"task_id", producer.at("task_id")}, {"team_id", producer.at("team_id")}, {"dispatcher_epoch", producer.at("dispatcher_epoch")}, {"execution_epoch", producer.at("execution_epoch")}};
        base.at("request").at("texts") = Array{Object{{"kind", "ORIGINAL"}, {"requirements_revision", 1}, {"text", "Original admit text."}}};
        auto admit = base.at("pending_events").as_array()[0]; admit.at("type") = "admit";
        admit.at("payload") = Object{{"type", "admit"}, {"immutable", Object{{"original_text", "Original admit text."}, {"constraint_catalog", Array{}},
            {"tool_allowlist", Array{"fs_write"}}, {"function_id", "general"}, {"config_snapshot_sha256", sha256_hex("synthetic config")},
            {"budget", Object{{"original_admission", 17}}}, {"future_business", "keep"}}}};
        base.at("pending_events").as_array().push_back(admit);
        auto absent = base; absent.at("workflow_action").as_object().erase("target");
        CHECK(actionable_wire(absent, Role::Dispatcher, chat).at("workflow_action") == absent.at("workflow_action"));
        const auto wire = actionable_wire(base, Role::Dispatcher, chat);
        const auto& immutable = wire.at("pending_events").as_array().back().at("payload").at("immutable");
        CHECK(immutable.at("original_text").at("semantic_contract_ref") == "request.texts[0].text");
        CHECK(immutable.at("constraint_catalog").at("semantic_contract_ref") == "request.constraint_catalog");
        CHECK(immutable.at("tool_allowlist") == admit.at("payload").at("immutable").at("tool_allowlist") && immutable.at("future_business") == "keep");
        const auto& instruction = wire.at("acceptance_instruction"); CHECK(instruction.at("body").at("binding_ref") == "current_state.workflow_action.target.original_binding");
        for (const auto* key : {"approach", "steps", "done_definition"}) CHECK(instruction.at("body").at(key) == base.at("acceptance_instruction").at("body").at(key));
        CHECK(wire.at("workflow_action").at("target").at("checks").as_array()[0].at("captured_binding").at("binding_ref") == "current_state.workflow_action.target.original_binding");
        for (int variant = 0; variant != 3; ++variant) {
            auto changed = base;
            if (variant == 0) changed.at("acceptance_instruction").at("body").at("instruction_revision") = 2;
            if (variant == 1) changed.at("workflow_action").at("target").at("checks").as_array()[0].at("captured_binding").at("requirements_revision") = 2;
            if (variant == 2) changed.at("pending_events").as_array().back().at("payload").at("immutable").at("original_text") = "Different original.";
            const auto projected = actionable_wire(changed, Role::Dispatcher, chat);
            if (variant == 0) CHECK(projected.at("acceptance_instruction").at("body") == changed.at("acceptance_instruction").at("body"));
            if (variant == 1) CHECK(projected.at("workflow_action").at("target").at("checks").as_array()[0].at("captured_binding") == changed.at("workflow_action").at("target").at("checks").as_array()[0].at("captured_binding"));
            if (variant == 2) CHECK(projected.at("pending_events").as_array().back().at("payload").at("immutable").at("original_text") == "Different original.");
        }
    }
}
void compact_state_respects_role_target_and_original_facts() {
    for (const bool chat : {false, true}) for (const auto& [role, kind] :
            std::vector<std::pair<Role, std::string>>{{Role::TeamLead, "submission"}, {Role::Dispatcher, "acceptance"},
                {Role::Dispatcher, "rejection"}, {Role::TeamLead, "experience"}, {Role::Worker, "submission"}}) {
        const auto contract = e2_contract(role, kind); const auto state = e2_wire_state(contract, role, chat);
        CHECK(!state.as_object().contains("compact_model_state"));
        for (const auto* key : {"binding", "captured_state_version", "questions", "wait", "unresolved_operations",
                "unresolved_attempts", "last_correction", "allowed_tools", "file_path_policy", "execution_policy_sha256", "budget",
                "history_policy", "history_catalog", "logical_agent_scope", "tasks", "plan"})
            CHECK(state.at(key) == contract.at(key));
        CHECK(state.at("history_windows").as_array()[0].at("content") == "original history-window index");
        const auto& event = state.at("pending_events").as_array()[0];
        for (const auto* key : {"event_id", "status", "claim_id", "actor", "event_requirements_revision", "subject_task_id", "future_field"})
            CHECK(event.at(key) == contract.at("pending_events").as_array()[0].at(key));
        if (role == Role::Worker) CHECK(state.at("submission") == contract.at("submission"));
        else {
            CHECK(!state.at("submission").as_object().contains("body"));
            CHECK(state.at("submission").at("duplicate_fields").as_object().contains("body"));
            CHECK(state.at("submission").at("original_binding") == contract.at("submission").at("original_binding"));
        }
        if (kind == "experience") {
            const auto& original = contract.at("workflow_action").at("target");
            const auto& projected = state.at("workflow_action").at("target");
            for (const auto* key : {"control_binding", "original_source_binding", "source_sha256"}) CHECK(projected.at(key) == original.at(key));
            for (const auto* key : {"requirements", "brief", "instruction", "acceptance"})
                CHECK(projected.at("source_snapshot").at(key) == original.at("source_snapshot").at(key));
        }
        auto checked = contract; auto& check_event = checked.at("pending_events").as_array()[0];
        check_event.at("type") = "check_submission"; check_event.at("payload") = Object{{"type", "check_submission"},
            {"binding", checked.at("submission").at("original_binding")}, {"submission_id", "submission-current"},
            {"expected_submission_version", 3}, {"check_body", Object{{"verification", std::string(300, 'v')}}}};
        const auto check_state = e2_wire_state(checked, role, chat);
        CHECK(!check_state.at("pending_events").as_array()[0].at("payload").as_object().contains("check_body"));
        check_event.at("payload").at("expected_submission_version") = 4;
        CHECK(e2_wire_state(checked, role, chat).at("pending_events").as_array()[0].at("payload").at("check_body") == check_event.at("payload").at("check_body"));
        for (int variant = 0; variant != 10; ++variant) {
            auto different = contract; auto& submission = different.at("submission");
            auto& result = different.at("pending_events").as_array()[0].at("result");
            if (variant == 0) submission.at("state_version") = 5;
            if (variant == 1) submission.at("original_binding").at("instruction_revision") = 2;
            if (variant == 2) submission.at("record_sha256") = sha256_hex("other original");
            if (variant == 3) submission.at("body").at("summary") = "different original text";
            if (variant == 4) different.at("pending_events").as_array()[0].at("type") = "future-event";
            if (variant == 5) result.at("original_binding").at("task_revision") = 2;
            if (variant == 6) result.at("body").at("summary") = "different event original";
            if (variant == 7) submission.at("body").at("signed_zero") = 0.0;
            if (variant == 8) submission.at("original_binding").at("task_revision") = 1.0;
            if (variant == 9) submission.as_object()["duplicate_fields"] = Object{{"body", "original business field"}};
            const auto projected = e2_wire_state(different, role, chat);
            if (variant < 4 || variant >= 7) CHECK(canonical_json(projected.at("submission").at("body")) == canonical_json(submission.at("body")));
            else if (variant == 4) CHECK(projected.at("pending_events") == different.at("pending_events"));
            else CHECK(projected.at("pending_events").as_array()[0].at("result").at("body") == result.at("body"));
            if (variant == 9) CHECK(projected.at("submission").at("duplicate_fields") == submission.at("duplicate_fields"));
        }
        auto amend = contract; amend.at("workflow_action") = Object{{"kind", "amendment"}, {"target", Object{
            {"tasks", Array{Object{{"task_id", "one"}, {"state_version", 4}, {"applied_instruction_revision", 1}},
                Object{{"task_id", "omitted"}, {"paused_for_change", true}, {"required", true}, {"review_required", true}}}},
            {"pending_node_ids", Array{"one", "omitted"}}}}};
        CHECK(e2_wire_state(amend, role, chat).at("workflow_action") == amend.at("workflow_action"));
    }
    auto input = wls::test::e2::context(); input.model_projection = "lean-v1";
    const auto old = compile_structured_context(input, {}, {}, StructuredContextFormat::V3);
    auto contract = parse_json_strict(input.task_contract); contract.as_object()["compact_model_state"] = true;
    input.task_contract = canonical_json(contract); continue_structured_contract(input, Object{{"attempt_id", "old-capture"}, {"input", old.logical_input}});
    const auto next = compile_structured_context(input, {}, {}, StructuredContextFormat::V3);
    CHECK(next.logical_input.at("contract_updates").as_array().empty());
    CHECK(next.logical_input.at("contract_anchor") == old.logical_input.at("contract_anchor"));
    CHECK(next.logical_input.at("current_state").at("semantic_versions") == old.logical_input.at("current_state").at("semantic_versions"));
    for (const auto& invalid : {Json(false), Json("true"), Json(nullptr)}) {
        contract.at("compact_model_state") = invalid;
        fails(ErrorCode::InvalidInput, [&] { (void)e2_wire_state(contract, Role::Dispatcher, false); });
    }
}
void compact_file_views_require_complete_matching_originals() {
    for (const bool chat : {false, true}) for (const auto& bytes : {std::string("原文😀"), std::string{}}) {
        ::test::TempDir directory; { std::ofstream file(directory.path / "original"); file << bytes; }
        const auto page = Workspace(directory.path, WorkspaceAccess::ReadOnly).read("original", 0, 65536);
        auto contract = e2_contract(Role::TeamLead, "submission");
        const Json file = Object{{"path", "original"}, {"size_bytes", bytes.size()}, {"sha256", sha256_hex(bytes)},
            {"content_base64", page.at("content_base64")}};
        const Json view = Object{{"path", "original"}, {"size_bytes", bytes.size()}, {"sha256", sha256_hex(bytes)},
            {"content", bytes}, {"binary", false}, {"source", "immutable_submission_snapshot"}};
        contract.at("submission").at("artifact_snapshot") = Array{file};
        contract.at("workflow_action").at("target").at("artifact_snapshot") = Array{file};
        contract.as_object()["submission_file_views"] = Array{view};
        for (int variant = 0; variant != 9; ++variant) {
            auto changed = contract; auto& target = changed.at("workflow_action").at("target");
            auto& projected_view = changed.at("submission_file_views").as_array()[0];
            if (variant == 1) projected_view.at("content") = nullptr;
            if (variant == 2) projected_view.at("binary") = true;
            if (variant == 3) projected_view.at("sha256") = sha256_hex("different");
            if (variant == 4) target.at("artifact_snapshot").as_array()[0].at("content_base64") = "!!!";
            if (variant == 5) projected_view.at("path") = "other";
            if (variant == 6) projected_view.at("size_bytes") = bytes.size() + 1;
            if (variant == 7) changed.at("submission").at("submission_id") = "other-submission-same-path";
            if (variant == 8) projected_view.at("sha256") = target.at("artifact_snapshot").as_array()[0].at("sha256") = sha256_hex("same wrong hash");
            const auto state = e2_wire_state(changed, Role::TeamLead, chat);
            const auto& snapshot = state.at("workflow_action").at("target").at("artifact_snapshot");
            CHECK(state.at("submission_file_views") == changed.at("submission_file_views"));
            if (variant == 0) {
                CHECK(!snapshot.as_array()[0].as_object().contains("content_base64"));
                CHECK(snapshot.as_array()[0].at("content_ref") == "current_state.submission_file_views[0].content");
            } else CHECK(snapshot == target.at("artifact_snapshot"));
        }
        contract.at("submission_file_views").as_array()[0].at("content") = nullptr;
        contract.at("submission_file_views").as_array()[0].at("binary") = true;
        auto binary = file; binary.at("content_base64") = "/wA="; binary.at("size_bytes") = 2;
        binary.at("sha256") = sha256_hex(std::string("\xff\0", 2));
        contract.at("workflow_action").at("target").at("artifact_snapshot") = Array{binary};
        CHECK(e2_wire_state(contract, Role::TeamLead, chat).at("workflow_action").at("target").at("artifact_snapshot") == Array{binary});
    }
}
void compact_worker_events_require_the_original_semantic_binding() {
    for (const bool chat : {false, true}) {
        auto contract = e2_contract(Role::Worker, "none"); contract.at("workflow_action") = nullptr;
        const auto binding = contract.at("binding"); auto previous = binding;
        contract.at("request").as_object()["request_id"] = binding.at("request_id");
        contract.at("request").as_object()["created_at"] = "unknown request business field";
        contract.at("request").as_object()["policy_epoch"] = binding.at("policy_epoch");
        previous.at("instruction_revision") = 0;
        auto order = binding; order.as_object()["approach"] = std::string(400, 'i');
        order.as_object()["signed_zero"] = -0.0;
        order.as_object()["steps"] = Array{"retain each original step"};
        order.as_object()["done_definition"] = "original completion requirement";
        contract.at("task") = binding; contract.at("task").as_object()["brief"] = Object{{"goal", "original task"}};
        contract.at("task").as_object()["based_on_requirements_revision"] = 1;
        contract.at("task").as_object()["state_version"] = 17;
        contract.at("task").as_object()["applied_instruction_revision"] = 0;
        contract.at("task").as_object()["paused_for_change"] = true;
        contract.at("instruction") = binding; contract.at("instruction").as_object()["body"] = order;
        contract.at("instruction").as_object()["based_on_requirements_revision"] = 1;
        contract.at("instruction").as_object()["state_version"] = 19;
        contract.at("instruction").as_object()["created_at"] = "synthetic creation";
        contract.at("instruction").as_object()["updated_at"] = "unknown instruction business field";
        const auto event = [&](const char* type, const Json& payload, const Json& result) -> Json {
            return Object{{"type", type}, {"event_id", std::string("event-") + type}, {"role", "worker"},
                {"request_id", binding.at("request_id")}, {"subject_task_id", binding.at("task_id")},
                {"event_requirements_revision", 1}, {"status", "PENDING"}, {"claim_id", "original-claim"},
                {"payload", payload}, {"result", result}};
        };
        const Json feedback = Object{{"attempt_id", "original-attempt"}, {"record_sha256", sha256_hex("original feedback")}};
        auto payload = feedback; payload.as_object()["type"] = "tool_feedback"; payload.as_object()["binding"] = binding;
        contract.at("pending_events") = Array{
            event("instruction", Object{{"type", "instruction"}, {"binding", previous}, {"body", order},
                {"expected_task_version", 16}}, Object{{"binding", binding}, {"state_version", 17}}),
            event("tool_feedback", payload, feedback),
            event("ack", Object{{"type", "ack"}, {"binding", binding}}, Object{{"binding", binding}, {"state_version", 18}})};
        for (int variant = 0; variant != 10; ++variant) {
            auto changed = contract; auto& instruction = changed.at("pending_events").as_array()[0];
            if (variant == 1) instruction.at("payload").at("body").at("instruction_revision") = 2;
            if (variant == 2) changed.at("instruction").at("body").at("instruction_revision") = 2;
            if (variant == 3) instruction.at("result").at("binding").at("instruction_revision") = 2;
            if (variant == 4) instruction.at("event_requirements_revision") = 2;
            if (variant == 5) instruction.at("payload").at("body").at("done_definition") = "different original";
            if (variant == 6) instruction.at("type") = "future-instruction";
            if (variant == 7) changed.at("task").at("task_revision") = 2;
            if (variant == 8) changed.at("instruction").at("based_on_requirements_revision") = 2;
            if (variant == 9) instruction.at("payload").at("body").at("signed_zero") = 0.0;
            const auto state = e2_wire_state(changed, Role::Worker, chat);
            const auto& projected = state.at("pending_events").as_array()[0];
            CHECK(projected.at("payload").at("binding") == previous); // Issuing revision is not current revision.
            CHECK(state.at("task").at("state_version") == 17 && state.at("task").at("paused_for_change") == true);
            CHECK(state.at("task").at("applied_instruction_revision") == 0);
            CHECK(state.at("instruction").at("state_version") == 19);
            CHECK(state.at("request").at("created_at") == "unknown request business field");
            CHECK(state.at("instruction").at("updated_at") == "unknown instruction business field");
            CHECK(state.at("request").at("policy_epoch") == binding.at("policy_epoch"));
            for (const auto* key : {"team_id", "dispatcher_epoch", "execution_epoch", "policy_epoch", "requirements_revision"})
                CHECK(state.at("task").at(key) == binding.at(key));
            for (const auto* key : {"policy_epoch", "requirements_revision"}) CHECK(state.at("instruction").at(key) == binding.at(key));
            if (variant > 0) CHECK(canonical_json(projected.at("payload").at("body")) == canonical_json(instruction.at("payload").at("body")));
            else {
                CHECK(projected.at("payload").at("body") == Json(Object{{"semantic_contract_ref", "instruction"},
                    {"original_binding_ref", "current_state.binding"}}));
                CHECK(state.at("task").at("binding_ref") == "current_state.binding");
            }
            if (variant == 7) CHECK(state.at("task").at("task_id") == binding.at("task_id"));
            if (variant == 8) CHECK(state.at("instruction").at("task_id") == binding.at("task_id"));
            const auto& compact_feedback = state.at("pending_events").as_array()[1];
            CHECK(compact_feedback.at("payload").at("attempt_id") == feedback.at("attempt_id"));
            CHECK(compact_feedback.at("payload").at("record_sha256") == feedback.at("record_sha256"));
            CHECK(compact_feedback.at("result").at("payload_fields") == Json(Array{"attempt_id", "record_sha256"}));
            CHECK(state.at("pending_events").as_array()[2].at("result").at("state_version") == 18);
            CHECK(state.at("pending_events").as_array()[2].at("payload").at("duplicate_fields").at("binding") == "current_state.binding");
        }
        for (int variant = 0; variant != 3; ++variant) {
            auto changed = contract; auto& feedback_event = changed.at("pending_events").as_array()[1];
            if (variant == 0) feedback_event.at("result").as_object()["future_original"] = "must remain";
            if (variant == 1) feedback_event.at("payload").at("record_sha256") = sha256_hex("different feedback");
            if (variant == 2) feedback_event.at("event_requirements_revision") = 2;
            CHECK(e2_wire_state(changed, Role::Worker, chat).at("pending_events").as_array()[1].at("result") == feedback_event.at("result"));
        }
        auto experience = contract; const auto submission = e2_submission();
        experience.as_object()["experience_scope"] = Object{{"control_binding", binding}, {"original_source_binding", binding}, {"source_sha256", sha256_hex("source")}};
        experience.at("workflow_action") = Object{{"kind", "experience"}, {"target", Object{{"source_sha256", sha256_hex("source")},
            {"source_snapshot", Object{{"submission", submission}, {"instruction", contract.at("instruction")}, {"brief", contract.at("task").at("brief")}}}}}};
        const auto source = e2_wire_state(experience, Role::TeamLead, chat).at("workflow_action").at("target").at("source_snapshot");
        CHECK(source.at("instruction").at("body").at("semantic_contract_ref") == "instruction");
        experience.at("experience_scope").at("original_source_binding").at("instruction_revision") = 2;
        CHECK(e2_wire_state(experience, Role::TeamLead, chat).at("workflow_action").at("target").at("source_snapshot").at("instruction").at("body") == order);
    }
}
void compact_dispatcher_plan_keeps_target_versions() {
    for (const bool chat : {false, true}) {
        auto contract = e2_contract(Role::Dispatcher, "acceptance"); const auto binding = contract.at("binding");
        const Json brief = Object{{"goal", std::string(350, 'g')}, {"constraints_verbatim", Array{std::string(300, 'c')}}};
        auto task = binding; task.as_object()["state_version"] = 4; task.as_object()["based_on_requirements_revision"] = 1;
        task.as_object()["node_id"] = "original-node"; task.as_object()["brief"] = brief;
        contract.at("tasks") = Array{task}; contract.as_object()["acceptance_task"] = task;
        auto instruction = task; instruction.as_object().erase("brief"); instruction.as_object()["body"] = binding;
        instruction.at("body").as_object()["constraints_verbatim"] = brief.at("constraints_verbatim");
        instruction.at("body").as_object()["unique_order"] = "preserve distinct order";
        contract.as_object()["acceptance_instruction"] = instruction;
        contract.at("plan") = Object{{"request_id", binding.at("request_id")}, {"plan_revision", 1},
            {"based_on_requirements_revision", 1}, {"body", Object{{"nodes", Array{Object{{"node_id", "original-node"},
                {"brief", brief}, {"required", true}, {"review", Object{{"required", true}}}, {"depends_on", Array{"earlier"}}}}}}}};
        for (int variant = 0; variant != 4; ++variant) {
            auto changed = contract;
            if (variant == 1) changed.at("tasks").as_array()[0].at("plan_revision") = 2;
            if (variant == 2) changed.at("acceptance_task").at("state_version") = 5;
            if (variant == 3) changed.at("acceptance_instruction").at("body").at("instruction_revision") = 2;
            const auto state = e2_wire_state(changed, Role::Dispatcher, chat);
            CHECK(state.at("plan") == changed.at("plan"));
            CHECK(state.at("acceptance_instruction").at("body").at("unique_order") == "preserve distinct order");
            if (variant == 0) {
                CHECK(!state.at("tasks").as_array()[0].as_object().contains("brief"));
                CHECK(state.at("acceptance_task").at("duplicate_fields").at("brief") == "current_state.plan.body.nodes[0].brief");
                CHECK(state.at("acceptance_instruction").at("body").at("duplicate_fields").at("constraints_verbatim") == "current_state.plan.body.nodes[0].brief.constraints_verbatim");
            }
            if (variant == 1) CHECK(state.at("tasks").as_array()[0].at("brief") == brief);
            if (variant == 2) CHECK(state.at("acceptance_task").at("brief") == brief);
            if (variant == 3) CHECK(state.at("acceptance_instruction").at("body").at("constraints_verbatim") == brief.at("constraints_verbatim"));
        }
    }
}
void check_parameter_descriptions(const Json& schema) {
    if (!schema.is_object()) return;
    const auto& object = schema.as_object();
    if (const auto* properties = object.if_contains("properties"))
        for (const auto& property : properties->as_object()) {
            CHECK(property.value().at("description").is_string());
            CHECK(!property.value().at("description").as_string().empty());
            check_parameter_descriptions(property.value());
        }
    for (const auto child : {"items", "additionalProperties", "contains", "not", "if", "then", "else"})
        if (const auto* value = object.if_contains(child)) check_parameter_descriptions(*value);
    for (const auto child : {"allOf", "anyOf", "oneOf"})
        if (const auto* values = object.if_contains(child))
            for (const auto& value : values->as_array()) check_parameter_descriptions(value);
}
void all_builtin_v3_descriptions_preserve_registry() {
    SchemaRegistry schemas(root / "schemas");
    auto input = base(); input.tools.clear();
    for (const auto& tool : worker_tool_registry())
        input.tools.push_back({tool.name, tool.version, tool.input_schema, builtin_tool_description(tool.name)});
    const std::map<std::string, std::string> management{{"submit_plan", "plan"}, {"issue_work_order", "work_order"},
        {"ask", "question"}, {"submit", "submission"}, {"answer_question", "answer_question"},
        {"forward_question", "forward_question"}, {"accept_candidate", "accept_candidate"},
        {"change_task", "change_task"}, {"escalate_task", "escalate_task"}, {"report_experience", "experience_report"},
        {"check_submission", "check_submission"}, {"resolve_amendment", "resolve_amendment"},
        {"submit_amendment_plan", "submit_amendment_plan"}};
    for (const auto& [tool, schema] : management)
        input.tools.push_back({tool, "1", schemas.project_model_input(schema), builtin_tool_description(tool)});
    for (const auto& tool : history_control_declarations()) input.tools.push_back(tool);
    for (const auto& tool : management_history_declarations()) input.tools.push_back(tool);
    CHECK(input.tools.size() == 29);
    const auto original = input.tools;
    const auto v1 = ContextCompiler{}.compile(input);
    const auto v2 = compile_structured_context(input, {});
    auto minimal = input;
    for (auto& tool : minimal.tools) tool.definition = schemas.project_v3_input(tool.definition);
    const auto v3 = compile_structured_context(minimal, {}, {}, StructuredContextFormat::V3);
    for (const auto& tool : v3.logical_input.at("prefix").at("tools").as_array()) {
        CHECK(!tool.at("description").as_string().empty());
        check_parameter_descriptions(tool.at("parameters"));
        CHECK(canonical_json(tool.at("parameters")).find("\"$ref\"") == std::string::npos);
    }
    for (std::size_t index = 0; index < input.tools.size(); ++index) {
        CHECK(input.tools[index].definition == original[index].definition);
        input.tools[index].description += " Modified wording.";
    }
    CHECK(to_json(compile_structured_context(input, {})) == to_json(v2));
    CHECK(to_json(ContextCompiler{}.compile(input)) == to_json(v1));
    CHECK(to_json(compiled_context_from_json(to_json(v3))) == to_json(v3));
    // Restoring frozen descriptions must not replace them with today's catalog.
    auto frozen = v3;
    frozen.logical_input.at("prefix").at("tools").as_array().front().at("parameters").at("properties").at("passed").at("description") = "Original frozen acceptance decision wording.";
    frozen.stable_prefix = canonical_json(Object{{"prefix", frozen.logical_input.at("prefix")},
        {"contract_anchor", frozen.logical_input.at("contract_anchor")}});
    frozen.prefix_sha256 = sha256_hex(frozen.stable_prefix);
    frozen.cache_identity = sha256_hex(canonical_json(Object{{"format", "wls-context-cache-v3"},
        {"selection", frozen.logical_input.at("selection")}, {"prefix_sha256", frozen.prefix_sha256}}));
    frozen.logical_input_sha256 = sha256_hex(frozen.prompt());
    CHECK(to_json(compiled_context_from_json(to_json(frozen))) == to_json(frozen));
    for (const auto& tool : worker_tool_registry())
        CHECK(tool.input_schema == std::find_if(original.begin(), original.end(),
            [&](const auto& item) { return item.name == tool.name; })->definition);
    for (const auto& [tool, schema] : management)
        CHECK(schemas.project_model_input(schema) == std::find_if(original.begin(), original.end(),
            [&](const auto& item) { return item.name == tool; })->definition);
}
ContextInput described_fixture() {
    auto input = fixture::context();
    input.tools.front().description = builtin_tool_description("note");
    input.tools.front().definition.at("properties").at("message").as_object()["description"] = "Observation to retain in this synthetic fixture.";
    return input;
}
void v3_hash_roundtrip_and_wire() {
    auto input = described_fixture(); auto r = fixture::request();
    r.context = compile_structured_context(input, {}, {}, StructuredContextFormat::V3);
    const auto c = r.context; const auto document = to_json(c);
    CHECK(document.at("format") == "wls-compiled-context-v3");
    CHECK(c.logical_input.at("format") == "wls-logical-input-v3");
    CHECK(c.logical_input.at("prefix").at("format") == "wls-logical-prefix-v3");
    CHECK(c.prefix_sha256 == sha256_hex(c.stable_prefix));
    CHECK(c.logical_input_sha256 == sha256_hex(c.prompt()));
    CHECK(to_json(compiled_context_from_json(document)) == document);
    const auto encoded = encode_responses_request(fixture::profile(), r);
    // Captured with the merged E binary, before the optional F policy existed.
    CHECK(sha256_hex(canonical_json(document)) == "a5468d6021eb86bd7cd72e72c800680d10c94b4c3f304fa198bdc71db37334ca");
    CHECK(sha256_hex(canonical_json(to_json(r))) == "97d680a0037304cedeecb686687e26bf37d454b38ead30c519b50efd1e51dd1c");
    CHECK(encoded.body_sha256 == "3dea3be602fce76cc5d0b2d664d15854ecdd0dea52d7aa40862036bfc5251cfc");
    const auto body = parse_json_strict(encoded.body);
    const auto& tool = body.at("tools").as_array().front();
    CHECK(tool.at("description") == Json(input.tools.front().description));
    CHECK(tool.at("parameters") == input.tools.front().definition);
    CHECK(encoded.body_sha256 == sha256_hex(encoded.body));
    for (int change = 0; change < 2; ++change) {
        auto updated = input;
        if (change == 0) updated.tools.front().description += " More detail.";
        else updated.tools.front().definition.at("properties").at("message").at("description") = "A changed parameter explanation.";
        const auto next = compile_structured_context(updated, {}, {}, StructuredContextFormat::V3);
        CHECK(next.prefix_sha256 != c.prefix_sha256);
        CHECK(next.cache_identity != c.cache_identity);
        CHECK(next.logical_input_sha256 != c.logical_input_sha256);
    }
    auto limits = ContextLimits{}; limits.max_prefix_bytes = c.stable_prefix.size() - 1;
    fails(ErrorCode::Quota, [&] { (void)compile_structured_context(input, {}, limits, StructuredContextFormat::V3); });
    auto profile = fixture::profile(); profile.limits.max_request_bytes = encoded.body.size() - 1;
    fails(ErrorCode::Quota, [&] { (void)encode_responses_request(profile, r); });
    CHECK(to_json(model_request_from_json(to_json(r))) == to_json(r));
    CHECK(to_json(r).at("format") == "wls-model-request-v3");
}
void v3_missing_invalid_and_oversized_descriptions_refuse() {
    auto input = described_fixture();
    const auto compile = [&](const ContextInput& value) {
        return compile_structured_context(value, {}, {}, StructuredContextFormat::V3);
    };
    input.tools.front().description.clear();
    fails(ErrorCode::InvalidInput, [&] { (void)compile(input); });
    input.tools.front().description = " \t\r\n";
    fails(ErrorCode::InvalidInput, [&] { (void)compile(input); });
    input = described_fixture(); input.tools.front().name = "unregistered";
    input.tools.front().definition.at("properties").at("message").as_object().erase("description");
    fails(ErrorCode::InvalidInput, [&] { (void)compile(input); });
    input = described_fixture(); input.tools.front().description = std::string(1024, 'x');
    input.tools.front().definition.at("properties").at("message").at("description") = std::string(1024, 'y');
    CHECK(compile(input).structured());
    for (const auto& invalid : {Json(""), Json(" \n"), Json(7), Json(std::string(1025, 'x'))}) {
        input = described_fixture();
        input.tools.front().definition.at("properties").at("message").at("description") = invalid;
        const auto code = invalid.is_string() && invalid.as_string().size() > 1024 ? ErrorCode::Quota : ErrorCode::InvalidInput;
        fails(code, [&] { (void)compile(input); });
        auto c = compile(described_fixture());
        c.logical_input.at("prefix").at("tools").as_array().front().at("description") = invalid;
        c.stable_prefix = canonical_json(Object{{"prefix", c.logical_input.at("prefix")},
            {"contract_anchor", c.logical_input.at("contract_anchor")}});
        fails(code, [&] { (void)to_json(c); });
        auto value = to_json(compile(described_fixture()));
        value.at("input").at("prefix").at("tools").as_array().front().at("description") = invalid;
        fails(code, [&] { (void)compiled_context_from_json(value); });
        value = to_json(compile(described_fixture()));
        value.at("input").at("prefix").at("tools").as_array().front().at("parameters").at("properties").at("message").at("description") = invalid;
        fails(code, [&] { (void)compiled_context_from_json(value); });
    }
    auto value = to_json(compile(described_fixture()));
    value.at("input").at("prefix").at("tools").as_array().front().as_object().erase("description");
    fails(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(value); });
    value = to_json(compile(described_fixture())); value.at("format") = "wls-compiled-context-v2";
    fails(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(value); });
}
void fake_uses_same_persisted_structure() {
    const auto r = request(); const auto restored = model_request_from_json(to_json(r));
    FakeModelStep step; step.role = Role::Worker; step.text = "response"; step.usage = {};
    FakeModel fake({step}); fake.validate(restored);
    const auto reply = fake.complete(restored); CHECK(reply.binding == r.binding);
    CHECK(!reply.usage.input_tokens.has_value()); CHECK(reply.text == "response");
    CHECK(to_json(restored) == to_json(r));
}
void full_envelope_integrity_covers_reservations() {
    const auto original = request(); const auto encoded = to_json(original);
    CHECK(encoded.at("format") == "wls-model-request-v2");
    CHECK(encoded.at("wire_request_sha256").is_null());
    auto changed = original; changed.max_output_tokens = 64;
    CHECK(to_json(changed).at("logical_request_sha256") != encoded.at("logical_request_sha256"));
    CHECK(to_json(changed).at("context") == encoded.at("context"));
    auto tampered = encoded; tampered.as_object()["max_output_tokens"] = 64;
    fails(ErrorCode::InvalidInput, [&] { (void)model_request_from_json(tampered); });
    tampered = encoded; tampered.as_object()["wire_request_sha256"] = encoded.at("logical_request_sha256");
    fails(ErrorCode::InvalidInput, [&] { (void)model_request_from_json(tampered); });
    tampered = encoded; tampered.as_object()["format"] = "wls-model-request-v1";
    fails(ErrorCode::InvalidInput, [&] { (void)model_request_from_json(tampered); });
}
void restored_requests_keep_deep_and_public_validation() {
    const auto encoded = to_json(request());
    CHECK(to_json(model_request_from_json(encoded)) == encoded);
    const auto rehash = [](Json& value) {
        auto original = value.as_object();
        original.erase("logical_request_sha256"); original.erase("wire_request_sha256");
        value.as_object()["logical_request_sha256"] = sha256_hex(canonical_json(original));
    };
    // Self-consistent envelope hashes do not replace identity, reservation or
    // native-turn checks when the original context is restored only once.
    for (int change = 0; change < 7; ++change) {
        auto tampered = encoded;
        if (change == 0) tampered.at("call_id") = tampered.at("attempt_id");
        if (change == 1) tampered.at("role") = "dispatcher";
        if (change == 2) tampered.at("role_call_index") = 0;
        if (change == 3) tampered.at("reserved_output_tokens") = 127;
        if (change == 4) tampered.at("timeout_ms") = 0;
        if (change == 5) tampered.at("config_snapshot_id") = "not-a-frozen-hash";
        if (change == 6) tampered.at("binding").at("task_id") = "";
        rehash(tampered);
        fails(ErrorCode::InvalidInput, [&] { (void)model_request_from_json(tampered); });
    }
    auto malformed = encoded;
    malformed.at("context").at("input").at("messages").as_array().back().at("tool_call_id") = "orphan";
    malformed.at("context").at("logical_input_sha256") = sha256_hex(canonical_json(malformed.at("context").at("input")));
    rehash(malformed);
    fails(ErrorCode::InvalidInput, [&] { (void)model_request_from_json(malformed); });

    auto mutable_request = model_request_from_json(encoded);
    FakeModelStep step; step.role = Role::Worker; step.text = "original reply";
    const auto response = FakeModel({step}).complete(mutable_request);
    auto& context = mutable_request.context;
    context.logical_input.at("messages").as_array().back().at("tool_call_id") = "orphan";
    context.dynamic_suffix = canonical_json(Object{{"task_contract", context.logical_input.at("task_contract")},
        {"messages", context.logical_input.at("messages")}});
    context.logical_input_sha256 = sha256_hex(context.prompt());
    // The public mutable-object entry points must still perform deep checks,
    // even after this same object previously passed complete restoration.
    fails(ErrorCode::InvalidInput, [&] { (void)to_json(mutable_request); });
    fails(ErrorCode::InvalidInput, [&] { validate_response(mutable_request, response); });

    auto numeric_request = fixture::request(); auto input = described_fixture();
    input.task_contract = canonical_json(Object{{"request", Object{{"texts", Array{}},
        {"constraint_catalog", Array{}}, {"requirements_revision", 1}}}, {"measurement", -0.0}});
    numeric_request.context = compile_structured_context(input, {}, {}, StructuredContextFormat::V3);
    const auto expected = to_json(numeric_request);
    const auto expected_bytes = canonical_json(expected);
    auto equivalent = expected;
    equivalent.at("context").at("input").at("current_state").at("measurement") = 0.0;
    // Equal JSON values need not have equal bytes. Restore the projection from
    // the complete original contract, including its negative-zero spelling.
    CHECK(equivalent == expected); CHECK(canonical_json(equivalent) != expected_bytes);
    const auto rebuilt = model_request_from_json(equivalent);
    CHECK(canonical_json(to_json(rebuilt)) == expected_bytes);
    rehash(equivalent);
    fails(ErrorCode::InvalidInput, [&] { (void)model_request_from_json(equivalent); });

    // Public serialization validates afresh while retaining the mutable
    // caller's original representation; restoration returns the rebuilt one.
    numeric_request.context.logical_input = equivalent.at("context").at("input");
    auto dynamic = numeric_request.context.logical_input.as_object();
    for (const auto* key : {"prefix", "selection", "format"}) dynamic.erase(key);
    numeric_request.context.dynamic_suffix = canonical_json(dynamic);
    CHECK(canonical_json(to_json(numeric_request)) == canonical_json(equivalent));
}
void same_provider_ids_have_distinct_logical_ids() {
    const auto a = attempt(), b = attempt(); const auto m = structured_attempt_history({a, b});
    CHECK(m.size() == 4); CHECK(m[0].tool_calls.front().call_id != m[2].tool_calls.front().call_id);
    CHECK(m[0].source.at("original_tool_call_id") == "reused");
    CHECK(m[0].source.at("frozen_binding") == a.at("frozen_binding"));
    CHECK(m[1].tool_call_id == m[0].tool_calls.front().call_id);
    CHECK(parse_json_strict(m[1].content) == a.at("feedback").at("result"));
    CHECK(compile_structured_context(base(), m).structured());
}

// ---- G1-2 opaque responses output in context/history/measurement ---------

Json compat_record(const std::string& ciphertext = {}) {
    const auto first = decode_responses_response(fixture::compat_profile(), fixture::request(), 200,
        fixture::compat_bytes("reasoning-tool-response.json"));
    CHECK(first.response.status == ModelResultStatus::Completed);
    Json response = to_json(first.response);
    if (!ciphertext.empty())
        response.at("responses_output").at("output").as_array()[0].as_object()["encrypted_content"] = ciphertext;
    const auto binding = to_json(fixture::request().binding);
    return Object{{"attempt_id", response.at("attempt_id")}, {"call_id", response.at("call_id")},
        {"frozen_binding", binding}, {"accepted", 1}, {"response", response},
        {"feedback", Object{{"original_binding", binding}, {"response", response},
            {"result", Object{{"ok", true}}}, {"record_sha256", sha256_hex("compat r1 receipt")}}}};
}
void responses_output_roundtrip_restore_and_hash() {
    const auto record = compat_record();
    const auto payload = record.at("response").at("responses_output");
    const auto messages = structured_attempt_history({record});
    CHECK(messages.size() == 2);
    const auto context = compile_structured_context(fixture::context(), messages);
    const auto& stored = context.logical_input.at("messages").as_array();
    CHECK(stored[0].at("role") == "assistant" && stored[0].at("responses_output") == payload);
    CHECK(!stored[1].as_object().contains("responses_output"));
    CHECK(context.logical_input_sha256 == sha256_hex(context.prompt()));
    const auto document = structured_context_to_json(context);
    CHECK(structured_context_to_json(structured_context_from_json(document)) == document);
    auto broken = document;
    broken.at("input").at("messages").as_array()[0].at("responses_output").at("output").as_array()[0].as_object().erase("encrypted_content");
    fails(ErrorCode::InvalidInput, [&] { (void)structured_context_from_json(broken); });
    auto forged = document;
    forged.at("input").at("messages").as_array()[1].as_object()["responses_output"] = payload;
    fails(ErrorCode::InvalidInput, [&] { (void)structured_context_from_json(forged); });
    auto unknown = document;
    unknown.at("input").at("messages").as_array()[0].at("responses_output").as_object()["extra"] = 1;
    fails(ErrorCode::InvalidInput, [&] { (void)structured_context_from_json(unknown); });
    auto mismatch = document;
    mismatch.at("input").at("messages").as_array()[0].at("content") = "changed projection";
    fails(ErrorCode::InvalidInput, [&] { (void)structured_context_from_json(mismatch); });
    auto huge = compat_record(std::string(1400 * 1024, 'x'));
    fails(ErrorCode::Quota, [&] { (void)compile_structured_context(fixture::context(), structured_attempt_history({huge})); });
}
Json compat_window_items(std::size_t count, const std::string& first_ciphertext) {
    Array items;
    for (std::size_t index = 0; index < count; ++index) {
        auto record = compat_record(index == 0 ? first_ciphertext : std::string{"gAAAAABsmall"});
        record.as_object()["sequence"] = static_cast<std::int64_t>(index + 1);
        record.as_object()["status"] = "COMPLETED";
        record.as_object()["feedback_missing"] = false;
        record.as_object()["management_record_sha256"] = sha256_hex("compat record " + std::to_string(index));
        items.push_back(record);
    }
    return items;
}
void window_measurement_counts_payload_and_quota_guards_recent_reasoning() {
    const Json policy = Object{{"mode", "window-v1"}, {"recent_attempts", 2}, {"inline_bytes", 32768}};
    auto inline_messages = worker_history_messages(Object{{"format", "wls-history-tail-v1"},
        {"items", compat_window_items(1, "gAAAAABsmall")}, {"total_count", 1}, {"omitted_count", 0},
        {"through_sequence", 1}}, policy);
    const auto compiled = compile_structured_context(fixture::context(), inline_messages);
    CHECK(compiled.logical_input.at("messages").as_array()[1].as_object().contains("responses_output"));
    fails(ErrorCode::Quota, [&] {
        (void)worker_history_messages(Object{{"format", "wls-history-tail-v1"},
            {"items", compat_window_items(1, std::string(64 * 1024, 'x'))}, {"total_count", 1},
            {"omitted_count", 0}, {"through_sequence", 1}}, policy); });
    // An older oversized completed round keeps the original reference policy;
    // only the most recent reasoning-bearing round is quota-fenced.
    auto kept = worker_history_messages(Object{{"format", "wls-history-tail-v1"},
        {"items", compat_window_items(2, std::string(64 * 1024, 'x'))}, {"total_count", 2},
        {"omitted_count", 0}, {"through_sequence", 2}}, policy);
    const auto older = compile_structured_context(fixture::context(), kept);
    const auto& older_messages = older.logical_input.at("messages").as_array();
    CHECK(older_messages[1].at("role") == "user" &&
          older_messages[1].at("source").at("kind") == Json("history_reference"));
    CHECK(canonical_json(older_messages[1]).find("gAAAAABsmall") == std::string::npos);
    CHECK(older_messages[2].at("role") == "assistant" &&
          older_messages[2].as_object().contains("responses_output"));
    const Json management = Object{{"mode", "window-v1"}, {"recent_attempts", 2},
        {"inline_bytes", 32768}, {"max_context_bytes", 1048576}};
    auto view = Object{{"format", "wls-management-view-v1"}, {"scope", Object{{"request_id", "req"}}},
        {"history_catalogs", Array{}}, {"recent_attempts", compat_window_items(1, "gAAAAABsmall")}};
    auto management_messages = management_history_messages(view, management);
    CHECK(canonical_json(single_agent_messages_json(management_messages)).find("gAAAAABsmall") != std::string::npos);
    view.at("recent_attempts") = compat_window_items(1, std::string(64 * 1024, 'x'));
    fails(ErrorCode::Quota, [&] { (void)management_history_messages(view, management); });
    // Single-Agent message measurement and integrity include the new field.
    const auto encoded = single_agent_messages_json(structured_attempt_history({compat_record()}));
    CHECK(encoded.as_array()[0].as_object().contains("responses_output"));
    CHECK(canonical_json(encoded).find("gAAAAAB3bHlzLXN5bnRoZXRpYy1lbmNyeXB0ZWQtcmVhc29uaW5nLW8zLTAwMDE=") != std::string::npos);
}
void missing_late_and_invalid_are_observations() {
    for (int kind = 0; kind != 4; ++kind) {
        auto value = attempt();
        if (kind == 0) { value.as_object()["feedback"] = nullptr; value.as_object()["feedback_missing"] = true; }
        if (kind == 1) value.as_object()["accepted"] = 0;
        if (kind == 2) value.at("feedback").as_object()["result"] = Object{{"runtime_error", true}, {"message", "invalid output"}};
        if (kind == 3) { value.as_object()["response"] = nullptr; value.as_object()["feedback"] = nullptr; }
        const auto m = structured_attempt_history({value}); CHECK(m.size() == 1 && m.front().role == "user");
        CHECK(m.front().tool_calls.empty()); CHECK(parse_json_strict(m.front().content) == value);
    }
}
void corrupted_source_binding_refuses() {
    auto value = attempt(); value.at("feedback").as_object()["original_binding"] = Object{};
    fails(ErrorCode::Corrupt, [&] { (void)structured_attempt_history({value}); });
    value = attempt(); value.at("feedback").at("response").as_object()["text"] = "changed";
    fails(ErrorCode::Corrupt, [&] { (void)structured_attempt_history({value}); });
    value = attempt(); value.as_object().erase("accepted");
    fails(ErrorCode::Corrupt, [&] { (void)structured_attempt_history({value}); });
    value = attempt(); value.at("feedback").as_object().erase("result");
    fails(ErrorCode::Corrupt, [&] { (void)structured_attempt_history({value}); });
    value = attempt(); value.as_object()["attempt_id"] = 23;
    fails(ErrorCode::Corrupt, [&] { (void)structured_attempt_history({value}); });
}
struct ConfigFixture {
    ::test::TempDir tmp;
    FrozenRequestConfig load(const std::string& format, const std::string& projection = {}) {
        auto yaml = read_runtime_file(root / "config/examples/system.yaml");
        const std::string tool_policy = "  tool_policy: fixed-role-v1\n";
        yaml.erase(yaml.find(tool_policy), tool_policy.size());
        yaml.erase(yaml.find("  model_projection: lean-v1\n"), sizeof("  model_projection: lean-v1\n") - 1);
        yaml.erase(yaml.find("  worker_history: {mode: append-fold-v1, read_max_bytes: 24576, read_token_budget: 196608}\n"), sizeof("  worker_history: {mode: append-fold-v1, read_max_bytes: 24576, read_token_budget: 196608}\n") - 1);
        const std::string marker = "  request_format: structured-v3\n";
        const auto pos = yaml.find(marker); CHECK(pos != std::string::npos);
        yaml.replace(pos, marker.size(), format.empty() ? "" : "  request_format: " + format + "\n");
        if (!projection.empty()) yaml.insert(yaml.find("  compaction_at_fraction:"), "  model_projection: " + projection + "\n");
        { std::ofstream out(tmp.path / "system.yaml"); out << yaml; CHECK(out.good()); }
        return ::test::legacy_shell_catalog(tmp.path / "system.yaml", root / "config/examples/functions", root / "prompts").freeze("general");
    }
};
void frozen_choice_and_legacy_default() {
    ConfigFixture f; const auto old = f.load(""); const auto next = f.load("structured-v2");
    CHECK(old.context_request_format() == "framed-v1"); CHECK(next.context_request_format() == "structured-v2");
    CHECK(FrozenRequestConfig::restore(old.serialize(), old.sha256()).context_request_format() == "framed-v1");
    CHECK(FrozenRequestConfig::restore(next.serialize(), next.sha256()).context_request_format() == "structured-v2");
    CHECK(next.runtime_support().at("history_window_integrated") == false);
    fails(ErrorCode::InvalidInput, [&] { (void)f.load("unknown-v99"); });
    CHECK(f.load("structured-v3").context_request_format() == "structured-v3");
}
void lean_frozen_choice_and_policy() {
    ConfigFixture f;
    CHECK(f.load("structured-v3").context_model_projection().empty());
    for (const auto* format : {"", "framed-v1", "structured-v2"})
        fails(ErrorCode::InvalidInput, [&] { (void)f.load(format, "lean-v1"); });
    fails(ErrorCode::InvalidInput, [&] { (void)f.load("structured-v3", "lean-v99"); });
}
void store_rejects_format_policy_mismatch() {
    ConfigFixture f; const auto config = f.load("structured-v2");
    Store store(f.tmp.path / "store.sqlite", read_runtime_file(root / "db/schema.sql"),
        read_runtime_file(root / "db/migrations/004_s1_store.sql"));
    AdmissionInput admission; admission.tenant_id = "test"; admission.project_id = "test";
    admission.root_id = "root"; admission.function_id = config.function_id(); admission.idempotency_key = "key";
    admission.original_text = "preserve task"; admission.config_snapshot = config.json();
    admission.constraint_catalog = Array{}; const auto tools = config.worker_tools();
    admission.tool_allowlist.assign(tools.begin(), tools.end());
    const auto id = str(store.admit(admission), "request_id");
    auto c = base(); c.role = Role::Dispatcher; c.model = config.resolve(Role::Dispatcher);
    ModelRequest r; r.call_id = new_id("call"); r.attempt_id = new_id("attempt"); r.role = Role::Dispatcher;
    r.binding = store.request_binding(id); r.config_snapshot_id = config.sha256(); r.model = c.model;
    r.context = ContextCompiler().compile(c); r.max_output_tokens = 128;
    r.reserved_input_tokens = 8192; r.reserved_output_tokens = 128;
    AttemptReservation reservation; reservation.call_id = r.call_id; reservation.attempt_id = r.attempt_id;
    reservation.config_snapshot_sha256 = r.config_snapshot_id; reservation.model_ref = r.model.model;
    reservation.reserved_input_tokens = r.reserved_input_tokens; reservation.reserved_output_tokens = r.reserved_output_tokens;
    reservation.max_output_tokens = r.max_output_tokens; reservation.request_envelope = to_json(r);
    const auto budget = store.budget(id);
    fails(ErrorCode::Unauthorized, [&] { (void)store.reserve_attempt(store.host_actor(Role::Dispatcher, id), r.binding, reservation); });
    CHECK(store.budget(id) == budget);
    c.tools.clear();
    r.context = compile_structured_context(c, {}, {}, StructuredContextFormat::V3);
    reservation.request_envelope = to_json(r);
    fails(ErrorCode::Unauthorized, [&] { (void)store.reserve_attempt(store.host_actor(Role::Dispatcher, id), r.binding, reservation); });
    CHECK(store.budget(id) == budget);
}

void guided_prompt_identifiers_are_registered_tools() {
    const std::set<std::string> non_tools{};
    const std::regex identifier("`([a-z][a-z0-9_]*)`");
    for (const auto* path : {"prompts/common.md", "prompts/dispatcher.md", "prompts/team_lead.md", "prompts/worker.md",
            "config/examples/functions/general/prompts/dispatcher.md", "config/examples/functions/general/prompts/team_lead.md",
            "config/examples/functions/general/prompts/worker.md"}) {
        const auto bytes = read_runtime_file(root / path);
        for (auto i = std::sregex_iterator(bytes.begin(), bytes.end(), identifier); i != std::sregex_iterator(); ++i) {
            const auto name = (*i)[1].str();
            if (!non_tools.contains(name)) CHECK(!builtin_tool_description(name).empty());
        }
    }
}

void legacy_frozen_capture_keeps_complete_context_and_wire_bytes() {
    // Captured before D with C's final library; context/config/codec sources
    // match c783d9e. Different transport sources were not used by this capture.
    const auto cases = parse_json_strict(read_runtime_file(root / "tests/fixtures/g1d_legacy_frozen.json"));
    const wls::test::live::Assets assets(root); SchemaRegistry schemas(root / "schemas");
    for (const auto& item : cases.as_array()) {
        const auto original = model_request_from_json(item.at("request"));
        const auto snapshot = canonical_json(item.at("snapshot"));
        const auto frozen = FrozenRequestConfig::restore(snapshot, original.config_snapshot_id);
        CHECK(frozen.context_prompt_policy().empty()); CHECK(frozen.serialize() == snapshot);
        CHECK(!frozen.json().at("system").at("context").as_object().contains("prompt_policy"));
        ::test::TempDir directory; auto store = assets.open(directory.path / "legacy.sqlite");
        auto admission = wls::test::live::admission(assets, wls::test::live::settings(), "legacy-guidance-fixture");
        admission.config_snapshot = frozen.json();
        const auto id = str(store->admit(admission), "request_id");
        auto current = OfflineRuntime(*store, schemas).capture_call(id, Role::Dispatcher).request;
        CHECK(current.context.stable_prefix == original.context.stable_prefix);
        auto contract = parse_json_strict(compiled_task_contract(current.context));
        const auto expected = parse_json_strict(compiled_task_contract(original.context));
        // Replace only generated request/event identities and observation time.
        for (const auto* key : {"binding", "budget", "request"})
            contract.at(key).at("request_id") = expected.at(key).at("request_id");
        auto& events = contract.at("pending_events").as_array();
        const auto& old_events = expected.at("pending_events").as_array();
        CHECK(events.size() == old_events.size());
        for (std::size_t i = 0; i < events.size(); ++i) {
            for (const auto* key : {"request_id", "event_id", "created_at"}) events[i].at(key) = old_events[i].at(key);
            events[i].at("result").at("request_id") = old_events[i].at("result").at("request_id");
        }
        CHECK(contract == expected);
        if (current.context.structured()) {
            current.context.logical_input.at("task_contract") = canonical_json(contract);
            current.context.dynamic_suffix = canonical_json(Object{{"task_contract", canonical_json(contract)}, {"messages", Array{}}});
            current.context.logical_input_sha256 = sha256_hex(current.context.prompt());
        } else {
            const auto bytes = canonical_json(contract);
            current.context.dynamic_suffix = "task_contract " + std::to_string(bytes.size()) + "\n" + bytes + "\n";
        }
        current.call_id = original.call_id; current.attempt_id = original.attempt_id; current.binding = original.binding;
        CHECK(canonical_json(to_json(current.context)) == canonical_json(item.at("request").at("context")));
        CHECK(canonical_json(to_json(current)) == canonical_json(item.at("request")));
        if (item.as_object().contains("wire_body")) {
            const auto profile = live_model_settings_from_json(frozen.model_profile(Role::Dispatcher), frozen.resolve(Role::Dispatcher).model).provider;
            const auto wire = encode_responses_request(profile, current);
            CHECK(wire.body == str(item, "wire_body")); CHECK(wire.body_sha256 == str(item, "wire_sha256"));
        }
        store.reset(); store = assets.open(directory.path / "legacy.sqlite");
        CHECK(store->request(id).at("config_snapshot") == frozen.json());
    }
}

CompiledContext rehashed_context(Json document);
void guided_capture_uses_real_adapter_and_stable_role_rules(bool lean = false) {
    const wls::test::live::Assets assets(root); SchemaRegistry schemas(root / "schemas");
    for (const bool fake : {true, false}) {
        if (lean && fake) continue;
        ::test::TempDir directory; auto store = assets.open(directory.path / "guided.sqlite");
        auto admission = wls::test::live::admission(assets, wls::test::live::settings(), "guided", true, fake);
        admission.original_text = "保留原始需求；不要调用外部服务。";
        admission.constraint_catalog = Array{
            Object{{"constraint_id", "c1"}, {"text", "保留原始需求"}, {"source_revision", 1}},
            Object{{"constraint_id", "c2"}, {"text", "不要调用外部服务"}, {"source_revision", 1}}};
        auto& context = admission.config_snapshot.at("system").at("context").as_object();
        context["request_format"] = lean ? "structured-v3" : "structured-v2";
        if (lean) context["model_projection"] = "lean-v1";
        if (!fake)
            for (auto& role : admission.config_snapshot.at("function").at("roles").as_object())
                role.value().as_object().erase("prompt_append");
        const auto system_path = directory.path / "system.yaml";
        const auto functions = directory.path / "functions";
        std::filesystem::copy(root / "config/examples/functions", functions, std::filesystem::copy_options::recursive);
        std::ofstream(system_path) << canonical_json(admission.config_snapshot.at("system"));
        std::ofstream(functions / "general/function.yaml") << canonical_json(admission.config_snapshot.at("function"));
        admission.config_snapshot = ::test::legacy_shell_catalog(system_path, functions, root / "prompts").freeze("general").json();
        const auto snapshot = canonical_json(admission.config_snapshot);
        const auto frozen = FrozenRequestConfig::restore(snapshot, sha256_hex(snapshot));
        CHECK(frozen.context_prompt_policy() == "guided-v1");
        CHECK(frozen.context_model_projection() == (lean ? "lean-v1" : ""));
        CHECK(frozen.runtime_support().at("context_prompt_policy") == "guided-v1");
        auto invalid = admission.config_snapshot;
        invalid.at("system").at("context").at("prompt_policy") = "guided-v99";
        const auto invalid_bytes = canonical_json(invalid);
        fails(ErrorCode::InvalidInput, [&] { (void)FrozenRequestConfig::restore(invalid_bytes, sha256_hex(invalid_bytes)); });
        const auto id = str(store->admit(admission), "request_id");
        OfflineRuntime runtime(*store, schemas);
        const auto first = runtime.capture_call(id, Role::Dispatcher, {}, 1, {}, false,
            {.reserved_cost_micro_usd = 100}).request;
        const auto contract = parse_json_strict(compiled_task_contract(first.context));
        CHECK(contract.as_object().contains("compact_model_state") == lean);
        CHECK(contract.as_object().contains("actionable_model_state") == lean);
        if (lean) CHECK(contract.at("compact_model_state") == true && contract.at("actionable_model_state") == true);
        const auto system = first.context.logical_input.at("prefix").at("system");
        const auto environment = parse_json_strict(str(system, "environment_card"));
        CHECK(environment.at("adapter") == frozen.model_profile(Role::Dispatcher).at("adapter"));
        CHECK(!environment.as_object().contains("checks_executed"));
        CHECK(str(system, "approved_rules").find("decode base64") == std::string::npos);
        if (!fake) {
            const auto profile = live_model_settings_from_json(frozen.model_profile(Role::Dispatcher), frozen.resolve(Role::Dispatcher).model).provider;
            const auto wire = encode_responses_request(profile, first).body;
            const auto body = parse_json_strict(wire);
            CHECK(str(body, "instructions").find("fake") == std::string::npos);
            if (lean) {
                const auto actor = store->host_actor(Role::Dispatcher, id);
                auto document = to_json(first.context); document.at("input").as_object().erase("model_projection");
                auto mismatch = first; mismatch.context = rehashed_context(document);
                fails(ErrorCode::Unauthorized, [&] { (void)store->reserve_attempt(actor, first.binding, wls::test::live::Fixture::reservation(mismatch)); });
                CHECK(store->budget(id).at("attempts") == 0);
                (void)store->reserve_attempt(actor, first.binding, wls::test::live::Fixture::reservation(first));
                store.reset(); store = assets.open(directory.path / "guided.sqlite");
                const auto restored = model_request_from_json(store->attempt(first.attempt_id).at("request_envelope"));
                CHECK(encode_responses_request(profile, restored).body == wire);
            }
            continue;
        }
        const auto steps = fake_model_steps_from_json(parse_json_strict(read_runtime_file(root / "tests/fixtures/s1_fake_initial.json")));
        (void)runtime.run(id, steps);
        const auto task_id = str(store->tasks(id).front(), "task_id");
        std::filesystem::create_directory(directory.path / "work");
        WorkerTools worker(*store, directory.path / "work");
        const auto call = OfflineRuntime(*store, schemas, &worker).capture_call(id, Role::Worker, task_id);
        const auto projection = parse_json_strict(str(call.request.context.logical_input.at("prefix").at("system"), "environment_card"));
        const auto evidence = worker.environment_card(call.actor);
        CHECK(evidence.at("environment_checks_executed") == false); CHECK(evidence.at("process_isolation_verified") == false);
        CHECK(!projection.at("execution").as_object().contains("environment_checks_executed"));
        CHECK(!projection.at("execution").as_object().contains("process_isolation_verified"));
        CHECK(projection.at("execution").at("execution_policy_sha256") == evidence.at("execution_policy_sha256"));
        CHECK(projection.at("execution").at("logical_workspace") == "/work");
        (void)store->amend(store->host_actor(Role::Root, id), store->request(id).at("state_version").as_int64(),
            "Preserve the amended original requirement.", Array{}, new_id("amend"));
        const auto amendment = runtime.capture_call(id, Role::Dispatcher, {}, 2, {}, true);
        CHECK(amendment.workflow_action.at("kind") == "amendment");
        CHECK(amendment.request.context.logical_input.at("prefix").at("system") == system);
        CHECK(compiled_task_contract(amendment.request.context).find("Preserve the amended original requirement.") != std::string::npos);
    }
}
void lean_guided_capture_uses_real_adapter_and_stable_role_rules() {
    guided_capture_uses_real_adapter_and_stable_role_rules(true);
}

void v3_five_round_encoded_prefix_and_originals(bool lean = false) {
    for (const bool fixed : {false, true}) {
    auto input = described_fixture();
    std::unique_ptr<::test::TempDir> pages;
    std::unique_ptr<Workspace> workspace;
    if (lean) input.model_projection = "lean-v1";
    if (lean || fixed) {
        const auto& read = worker_tool_definition("fs_read");
        input.tools.push_back({read.name, read.version, read.input_schema, builtin_tool_description(read.name)});
    }
    if (fixed) input.tool_policy = "fixed-role-v1";
    if (lean) {
        pages = std::make_unique<::test::TempDir>();
        workspace = std::make_unique<Workspace>(pages->path, WorkspaceAccess::ReadOnly);
    }
    Json contract = Object{{"request", Object{{"requirements_revision", 1},
        {"texts", Array{Object{{"text", "original request"}, {"kind", "ORIGINAL"}}}}, {"constraint_catalog", Array{}}}},
        {"task", Object{{"brief", Object{{"goal", "retain original bytes"}}}, {"task_revision", 1}, {"state_version", 1}}},
        {"instruction", Object{{"body", Object{{"steps", Array{"read"}}}}, {"instruction_revision", 1}}},
        {"binding", Object{{"execution_epoch", 1}}}, {"budget", Object{{"spent_input", 0}}}};
    auto profile = fixture::compat_profile();
    std::vector<StructuredContextMessage> messages;
    if (lean) {
        messages = worker_history_messages(Object{{"format", "wls-history-tail-v1"}, {"items", Array{}},
            {"total_count", 1}, {"omitted_count", 1}, {"through_sequence", 7}},
            Object{{"mode", "window-v1"}, {"recent_attempts", 1}, {"inline_bytes", 32768}});
        messages.push_back({"user", "UNKNOWN: original observation missing; source business field stays readable.", {}, {},
            Object{{"private_source", "must-stay-host-side"}}});
    }
    Json previous = nullptr, anchor = nullptr, previous_body = nullptr;
    Array previous_input;
    for (int round = 0; round != 5; ++round) {
        if (fixed) input.allowed_tools = {round % 2 ? "fs_read" : "note"};
        contract.at("budget").at("spent_input") = round * 17;
        contract.at("binding").at("execution_epoch") = round + 1;
        contract.at("task").at("state_version") = round + 1;
        input.task_contract = canonical_json(contract);
        continue_structured_contract(input, previous);
        auto request = fixture::request();
        request.context = compile_structured_context(input, messages, {}, StructuredContextFormat::V3);
        const auto logical = request.context.logical_input;
        const auto body = parse_json_strict(encode_responses_request(profile, request).body);
        if (lean) {
            CHECK(canonical_json(body).find("must-stay-host-side") == std::string::npos);
            CHECK(parse_json_strict(str(body.at("input").as_array().front(), "content")) == logical.at("contract_anchor"));
            const auto state = parse_json_strict(str(body.at("input").as_array().back(), "content")).at("current_state");
            CHECK(!state.as_object().contains("message_sources"));
            CHECK(!state.at("history_windows").as_array().front().as_object().contains("source"));
            CHECK(str(state.at("history_windows").as_array().front(), "content").find("through_sequence") != std::string::npos);
            CHECK(canonical_json(body).find("UNKNOWN") != std::string::npos);
        }
        if (fixed) {
            CHECK(!body.as_object().contains("tool_choice"));
            auto opted = profile; opted.tool_choice_policy = "allowed-tools-v1";
            const auto controlled = parse_json_strict(encode_responses_request(opted, request).body);
            CHECK(controlled.at("tools") == body.at("tools"));
            CHECK(controlled.at("tool_choice") == Json(Object{{"type", "allowed_tools"}, {"mode", "required"},
                {"tools", Array{Object{{"type", "function"}, {"name", input.allowed_tools.front()}}}}}));
            CHECK(to_json(provider_profile_from_json(to_json(opted))) == to_json(opted));
            opted.tool_choice_policy = "required-v1";
            const auto required = parse_json_strict(encode_responses_request(opted, request).body);
            CHECK(required.at("tool_choice") == "required" && required.at("tools") == body.at("tools"));
            CHECK(to_json(provider_profile_from_json(to_json(opted))) == to_json(opted));
        }
        auto ordered = body.at("input").as_array();
        CHECK(parse_json_strict(str(ordered.back(), "content")).at("format") == "wls-responses-current-state-v3");
        ordered.pop_back();
        if (round == 0) anchor = logical.at("contract_anchor");
        else {
            CHECK(body.at("tools") == previous_body.at("tools"));
            CHECK(body.at("instructions") == previous_body.at("instructions"));
            CHECK(ordered.size() > previous_input.size());
            CHECK(std::equal(previous_input.begin(), previous_input.end(), ordered.begin()));
        }
        CHECK(logical.at("contract_anchor") == anchor);
        CHECK(logical.at("contract_updates").as_array().empty());
        CHECK(compiled_task_contract(request.context) == canonical_json(contract));
        CHECK(to_json(model_request_from_json(to_json(request))) == to_json(request));
        previous = Object{{"attempt_id", "attempt_round_" + std::to_string(round)}, {"input", logical}};
        auto original = compat_record();
        original.at("attempt_id") = previous.at("attempt_id");
        original.at("response").at("attempt_id") = previous.at("attempt_id");
        auto& payload = original.at("response").at("responses_output");
        for (auto& item : payload.at("output").as_array()) {
            item.at("id") = str(item, "id") + std::to_string(round);
            if (item.at("type") == "function_call") item.at("call_id") = "provider_" + std::to_string(round);
        }
        original.at("response").at("tool_calls").as_array()[0].at("call_id") = "provider_" + std::to_string(round);
        original.at("feedback").at("response") = original.at("response");
        if (lean) {
            const std::string bytes = round == 0 ? "原始UTF-8😀" : round == 1 ? "" :
                round == 2 ? std::string("\xff\0", 2) : round == 3 ? "😀" : std::string(2048, 'a');
            const auto path = "page" + std::to_string(round);
            { std::ofstream file(pages->path / path, std::ios::binary); file << bytes; CHECK(file.good()); }
            const auto page = workspace->read(path, 0, round == 3 ? 1 : 4096);
            auto& response = original.at("response");
            response.at("tool_calls").as_array()[0].at("name") = "fs_read";
            response.at("tool_calls").as_array()[0].at("arguments") = Object{{"path", path}};
            auto& native = response.at("responses_output").at("output").as_array().back();
            native.at("name") = "fs_read"; native.at("arguments") = canonical_json(Object{{"path", path}});
            original.at("feedback").at("response") = response; original.at("feedback").at("result") = page;
        }
        const auto turn = structured_attempt_history({original});
        CHECK(turn.front().responses_output == payload);
        messages.insert(messages.end(), turn.begin(), turn.end());
        previous_body = body; previous_input = ordered;
    }
    contract.at("request").at("requirements_revision") = 2;
    contract.at("request").at("texts").as_array().push_back(Object{{"text", "complete AMEND replacement"}, {"kind", "AMEND"}});
    contract.at("instruction").at("instruction_revision") = 2;
    contract.at("instruction").at("body") = Object{{"steps", Array{"new complete instruction"}}};
    input.task_contract = canonical_json(contract); continue_structured_contract(input, previous);
    auto amended = fixture::request();
    amended.context = compile_structured_context(input, messages, {}, StructuredContextFormat::V3);
    const auto encoded = parse_json_strict(encode_responses_request(profile, amended).body);
    const auto& ordered = encoded.at("input").as_array();
    CHECK(std::equal(previous_input.begin(), previous_input.end(), ordered.begin()));
    CHECK(amended.context.logical_input.at("contract_anchor") == anchor);
    CHECK(amended.context.logical_input.at("contract_updates").as_array().size() == 1);
    CHECK(amended.context.logical_input.at("contract_updates").as_array()[0].at("after_attempt_id") == previous.at("attempt_id"));
    const auto update_text = parse_json_strict(str(ordered[ordered.size() - 2], "content"));
    if (lean) {
        CHECK(!update_text.as_object().contains("task_contract"));
        CHECK(update_text.at("request") == amended.context.logical_input.at("contract_updates").as_array()[0].at("contract").at("request"));
        CHECK(update_text.at("semantic_versions").at("instruction_revision") == 2);
        for (const auto& item : ordered) if (item.as_object().contains("output")) {
            const auto page = parse_json_strict(str(item, "output"));
            const auto name = str(page, "path"); const bool text = name == "page0" || name == "page1" || name == "page4";
            CHECK(page.as_object().contains("content") == text);
            CHECK(page.as_object().contains("content_base64") != text);
            CHECK(str(item, "call_id").starts_with("provider_"));
        }
        auto legacy = input; legacy.model_projection.clear();
        auto e = amended; e.context = compile_structured_context(legacy, messages, {}, StructuredContextFormat::V3);
        legacy.tool_policy.clear(); legacy.allowed_tools.clear();
        auto v2 = amended; v2.context = compile_structured_context(legacy, messages);
        const auto e_bytes = encode_responses_request(profile, e).body.size();
        const auto v2_bytes = encode_responses_request(profile, v2).body.size();
        const auto lean_bytes = canonical_json(encoded).size();
        std::cout << "lean fixture bytes v2=" << v2_bytes << " E-v3=" << e_bytes << " lean=" << lean_bytes
            << " ratios=" << double(lean_bytes) / double(v2_bytes) << ',' << double(lean_bytes) / double(e_bytes) << '\n';
        CHECK(lean_bytes < e_bytes && lean_bytes < v2_bytes);
    } else CHECK(update_text.at("format") == "wls-responses-contract-update-v3");
    CHECK(amended.context.logical_input.at("messages") == single_agent_messages_json(messages));
    CHECK(encode_responses_request(profile, model_request_from_json(to_json(amended))).body == canonical_json(encoded));
    }
}
void lean_wire_projection_preserves_originals_and_ordered_prefix() {
    v3_five_round_encoded_prefix_and_originals(true);
}

struct V3Fixture {
    ::test::TempDir directory;
    wls::test::live::Assets assets{root};
    SchemaRegistry schemas{root / "schemas"};
    std::filesystem::path database{directory.path / "v3.sqlite"};
    std::unique_ptr<Store> store;
    std::string id, task;
    explicit V3Fixture(const std::string& mode, int recent = 6, bool fixed = false, bool lean = false) {
        auto yaml = read_runtime_file(root / "config/examples/system.yaml");
        const std::string tool_policy = "  tool_policy: fixed-role-v1\n";
        yaml.erase(yaml.find(tool_policy), tool_policy.size());
        yaml.erase(yaml.find("  model_projection: lean-v1\n"), sizeof("  model_projection: lean-v1\n") - 1);
        yaml.erase(yaml.find("  worker_history: {mode: append-fold-v1, read_max_bytes: 24576, read_token_budget: 196608}\n"), sizeof("  worker_history: {mode: append-fold-v1, read_max_bytes: 24576, read_token_budget: 196608}\n") - 1);
        const auto begin = yaml.find("  request_format: "); CHECK(begin != std::string::npos);
        yaml.replace(begin, yaml.find('\n', begin) - begin + 1, "  request_format: structured-v3\n");
        const auto position = yaml.find("  compaction_at_fraction:"); CHECK(position != std::string::npos);
        std::string policies;
        if (lean) policies += "  model_projection: lean-v1\n";
        if (fixed) policies += "  tool_policy: fixed-role-v1\n";
        if (mode == "worker" || mode == "all") policies += "  worker_history: {mode: window-v1, recent_attempts: " +
            std::to_string(recent) + ", inline_bytes: 32768}\n";
        if (mode == "manager" || mode == "all") policies += "  management_history: {mode: window-v1, recent_attempts: 2, inline_bytes: 32768, max_context_bytes: 1048576}\n";
        if (mode == "single" || mode == "all") policies += "  logical_agent: {mode: single-agent-role-broker-v1, max_attempts: 64, max_history_bytes: 524288}\n";
        yaml.insert(position, policies);
        { std::ofstream file(directory.path / "system.yaml"); file << yaml; CHECK(file.good()); }
        const auto frozen = ::test::legacy_shell_catalog(directory.path / "system.yaml", root / "config/examples/functions", root / "prompts").freeze("general");
        AdmissionInput admission;
        admission.tenant_id = "test"; admission.project_id = "test"; admission.root_id = "root";
        admission.function_id = frozen.function_id(); admission.idempotency_key = "v3";
        admission.original_text = read_runtime_file(root / "tests/fixtures/s1_request.txt");
        admission.constraint_catalog = parse_json_strict(read_runtime_file(root / "tests/fixtures/s1_constraints.json"));
        admission.config_snapshot = frozen.json();
        const auto tools = frozen.worker_tools(); admission.tool_allowlist.assign(tools.begin(), tools.end());
        admission.budget = {64, 10000000, 1000000, 0};
        reopen(); id = str(store->admit(admission), "request_id");
        (void)OfflineRuntime(*store, schemas).run(id, script("s1_fake_initial.json"));
        task = str(store->tasks(id).front(), "task_id");
    }
    static std::vector<FakeModelStep> script(const std::string& name) {
        return fake_model_steps_from_json(::test::v3_fake_script(parse_json_strict(read_runtime_file(root / "tests/fixtures" / name))));
    }
    void reopen() {
        store.reset(); store = assets.open(database);
        if (!task.empty()) {
            const auto binding = store->binding(task);
            const auto actor = store->host_actor(Role::Worker, id, binding.team_id);
            (void)store->acknowledge(actor, binding, store->task(task).at("state_version").as_int64(), new_id("ack"));
        }
    }
    FakeModel note(std::int64_t index = 1) {
        FakeModelStep step; step.role = Role::Worker; step.role_call_index = index;
        step.tool_calls = {{"original_note", "note", Object{{"text", "persistent original feedback"}}}};
        step.usage = {1, 1, 0, 0, 0}; return FakeModel({step});
    }
};

void v3_capture_reserve_restore_policy_combinations(bool lean = false) {
    for (const bool fixed : {false, true}) for (const auto* mode : {"worker", "manager", "single", "all"}) {
        if (lean && std::string_view(mode) != "all") continue; // One combined fixture covers all three independent history choices.
        V3Fixture fixture(mode, 6, fixed, lean);
        Json prefix = nullptr;
        for (int round = 1; round <= (lean ? 2 : 5); ++round) {
            auto fake = fixture.note(round);
            const auto call = OfflineRuntime(*fixture.store, fixture.schemas).reserve_call(fake, fixture.id, Role::Worker, fixture.task, round);
            const auto envelope = fixture.store->attempt(call.request.attempt_id).at("request_envelope");
            CHECK(envelope.at("format") == "wls-model-request-v3");
            CHECK(to_json(model_request_from_json(envelope)) == envelope);
            const auto& logical = call.request.context.logical_input;
            CHECK(logical.at("current_state").as_object().contains("compact_model_state") == lean);
            CHECK(logical.at("current_state").as_object().contains("actionable_model_state") == lean);
            CHECK(canonical_json(logical.at("contract_anchor")).find("compact_model_state") == std::string::npos);
            CHECK(canonical_json(logical.at("contract_updates")).find("compact_model_state") == std::string::npos);
            CHECK(canonical_json(logical.at("contract_anchor")).find("actionable_model_state") == std::string::npos);
            CHECK(canonical_json(logical.at("contract_updates")).find("actionable_model_state") == std::string::npos);
            if (fixed) {
                CHECK(call.request.context.logical_input.at("tool_policy") == "fixed-role-v1");
                if (prefix.is_null()) prefix = call.request.context.logical_input.at("prefix");
                CHECK(call.request.context.logical_input.at("prefix") == prefix);
                const auto names = call.request.context.tool_names;
                CHECK(std::find(names.begin(), names.end(), "recall") == names.end());
                CHECK(std::find(names.begin(), names.end(), "load_memory") == names.end());
            }
            CHECK(OfflineRuntime(*fixture.store, fixture.schemas).accept_response(call, fake.complete(call.request)).is_object());
            if (round == 2) fixture.reopen();
        }
        const auto frozen = restore_request_config(fixture.store->request(fixture.id));
        const auto worker = fixture.store->host_actor(Role::Worker, fixture.id, fixture.store->binding(fixture.task).team_id);
        const auto prior = fixture.store->previous_context(worker);
        fixture.reopen();
        const auto current_worker = fixture.store->host_actor(Role::Worker, fixture.id, fixture.store->binding(fixture.task).team_id);
        CHECK(fixture.store->previous_context(current_worker) == prior);
        const auto call = OfflineRuntime(*fixture.store, fixture.schemas).capture_call(fixture.id, Role::Worker, fixture.task);
        CHECK(call.request.context.logical_input.at("contract_anchor") == prior.at("input").at("contract_anchor"));
        CHECK(call.request.context.logical_input.at("contract_updates") == prior.at("input").at("contract_updates"));
        if (!frozen.logical_agent_policy().is_null()) {
            const auto transcript = single_agent_transcript(fixture.id, frozen, fixture.store->logical_agent_history(current_worker));
            CHECK(call.request.context.logical_input.at("messages") == single_agent_messages_json(transcript.messages));
            CHECK(call.request.context.role == Role::Worker);
        }
    }
}
void lean_v3_capture_reserve_restore_policy_combinations() {
    v3_capture_reserve_restore_policy_combinations(true);
}

void v3_amend_reopen_and_original_cas() {
    V3Fixture fixture("worker", 1); auto fake = fixture.note();
    auto runtime = [&] { return OfflineRuntime(*fixture.store, fixture.schemas); };
    const auto old = runtime().reserve_call(fake, fixture.id, Role::Worker, fixture.task);
    const auto anchor = old.request.context.logical_input.at("contract_anchor");
    (void)fixture.store->amend(fixture.store->host_actor(Role::Root, fixture.id),
        fixture.store->request(fixture.id).at("state_version").as_int64(),
        read_runtime_file(root / "tests/fixtures/s1_amendment.txt"),
        parse_json_strict(read_runtime_file(root / "tests/fixtures/s1_amended_constraints.json")), new_id("amend"));
    fails(ErrorCode::Stale, [&] { (void)runtime().accept_response(old, fake.complete(old.request)); });
    CHECK(fixture.store->attempt(old.request.attempt_id).at("request_envelope") == to_json(old.request));
    (void)runtime().run(fixture.id, V3Fixture::script("s1_fake_amended.json"), "update");
    const auto first = runtime().capture_call(fixture.id, Role::Worker, fixture.task);
    CHECK(first.request.context.logical_input.at("contract_anchor") == anchor);
    CHECK(first.request.context.logical_input.at("contract_updates").as_array().size() == 1);
    CHECK(canonical_json(first.request.context.logical_input.at("contract_updates")).find("改为中文") != std::string::npos);
    fixture.reopen();
    const auto reopened = runtime().capture_call(fixture.id, Role::Worker, fixture.task);
    CHECK(reopened.request.context.logical_input.at("contract_anchor") == anchor);
    CHECK(reopened.request.context.logical_input.at("contract_updates") == first.request.context.logical_input.at("contract_updates"));
    for (int round = 0; round != 3; ++round) {
        const auto call = runtime().reserve_call(fake, fixture.id, Role::Worker, fixture.task);
        (void)runtime().accept_response(call, fake.complete(call.request));
    }
    fixture.reopen();
    const auto moved = runtime().capture_call(fixture.id, Role::Worker, fixture.task);
    CHECK(moved.request.context.logical_input.at("contract_anchor") == anchor);
    CHECK(moved.request.context.logical_input.at("contract_updates") == first.request.context.logical_input.at("contract_updates"));
    auto input = described_fixture();
    input.task_contract = compiled_task_contract(moved.request.context);
    input.contract_anchor = anchor;
    input.contract_updates = moved.request.context.logical_input.at("contract_updates");
    const auto frozen = restore_request_config(fixture.store->request(fixture.id));
    const auto messages = worker_history_messages(fixture.store->attempt_history_tail(moved.actor, 1), frozen.worker_history_policy());
    auto projection = fixture::request();
    projection.context = compile_structured_context(input, messages, {}, StructuredContextFormat::V3);
    const auto body = parse_json_strict(encode_responses_request(fixture::profile(), projection).body);
    CHECK(parse_json_strict(str(body.at("input").as_array()[1], "content")).at("format") == "wls-responses-contract-update-v3");
    const auto latest = fixture.store->previous_context(moved.actor);
    const auto attempt_id = str(latest, "attempt_id");
    const auto original = canonical_json(fixture.store->attempt(attempt_id).at("request_envelope"));
    wls::test::live::sql(fixture.database, "UPDATE call_attempts SET state_version=state_version+1,request_envelope=json_set(request_envelope,'$.oversized','" +
        std::string(2097152, 'x') + "') WHERE attempt_id='" + attempt_id + "'");
    fails(ErrorCode::Quota, [&] { (void)runtime().capture_call(fixture.id, Role::Worker, fixture.task); });
    std::string escaped;
    for (char ch : original) { escaped += ch; if (ch == '\'') escaped += '\''; }
    wls::test::live::sql(fixture.database, "UPDATE call_attempts SET state_version=state_version+1,request_envelope='" + escaped + "' WHERE attempt_id='" + attempt_id + "'");
    wls::test::live::sql(fixture.database, "UPDATE call_attempts SET state_version=state_version+1,request_envelope=json_remove(request_envelope,'$.context.input.contract_anchor') "
        "WHERE rowid=(SELECT MAX(rowid) FROM call_attempts WHERE role='worker')");
    fails(ErrorCode::InvalidInput, [&] { (void)runtime().capture_call(fixture.id, Role::Worker, fixture.task); });
}

void v3_cross_version_and_projection_corruption_reject() {
    for (int version = 1; version <= 3; ++version) {
        auto request = fixture::request(); const auto input = described_fixture();
        request.context = version == 1 ? ContextCompiler{}.compile(input) : compile_structured_context(input, {}, {},
            version == 2 ? StructuredContextFormat::V2 : StructuredContextFormat::V3);
        const auto original = to_json(request);
        for (int outer = 1; outer <= 3; ++outer) if (outer != version) {
            auto changed = original; changed.at("format") = "wls-model-request-v" + std::to_string(outer);
            fails(ErrorCode::InvalidInput, [&] { (void)model_request_from_json(changed); });
        }
        if (version != 3) continue;
        for (const auto* key : {"contract_anchor", "contract_updates", "current_state"}) {
            auto changed = original; changed.at("context").at("input").as_object().erase(key);
            fails(ErrorCode::InvalidInput, [&] { (void)model_request_from_json(changed); });
        }
        auto changed = original;
        changed.at("context").at("input").at("current_state").as_object()["forged_authority"] = "root";
        changed.at("context").at("logical_input_sha256") = sha256_hex(canonical_json(changed.at("context").at("input")));
        fails(ErrorCode::InvalidInput, [&] { (void)model_request_from_json(changed); });
    }
}

void lean_projection_preserves_errors_and_checks_originals() {
    auto input = described_fixture(); input.model_projection = "lean-v1";
    auto original = attempt();
    original.at("feedback").at("result") = Object{{"content", "not a normal read page"}, {"content_base64", "must remain"},
        {"source", "business field"}, {"metadata", Object{{"original", true}}}};
    const auto messages = structured_attempt_history({original});
    auto request = fixture::request(); request.context = compile_structured_context(input, messages, {}, StructuredContextFormat::V3);
    const auto saved = to_json(request.context);
    const auto body = parse_json_strict(encode_responses_request(fixture::profile(), request).body);
    CHECK(parse_json_strict(str(body.at("input").as_array()[3], "output")) == original.at("feedback").at("result"));
    CHECK(to_json(request.context) == saved);
    for (const auto* policy : {"", "lean-v99"}) {
        auto broken = saved;
        if (*policy) broken.at("input").at("model_projection") = policy;
        else broken.at("input").as_object().erase("model_projection");
        fails(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(broken); });
    }
    auto broken = saved; broken.at("input").at("messages").as_array()[1].at("source").as_object()["record_sha256"] = sha256_hex("forged");
    fails(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(broken); });
    fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(input, messages); });
    fails(ErrorCode::InvalidInput, [&] { (void)ContextCompiler{}.compile(input); });
    auto quota = ContextLimits{}; quota.max_total_bytes = quota.max_prefix_bytes = 1024;
    fails(ErrorCode::Quota, [&] { (void)compile_structured_context(input, messages, quota, StructuredContextFormat::V3); });
    auto profile = fixture::profile(); profile.limits.max_request_bytes = canonical_json(body).size() - 1;
    fails(ErrorCode::Quota, [&] { (void)encode_responses_request(profile, request); });
}

CompiledContext rehashed_context(Json document) {
    const auto& logical = document.at("input");
    document.at("prefix_sha256") = sha256_hex(canonical_json(Object{{"prefix", logical.at("prefix")},
        {"contract_anchor", logical.at("contract_anchor")}}));
    Object identity{{"format", "wls-context-cache-v3"}, {"selection", logical.at("selection")},
        {"prefix_sha256", document.at("prefix_sha256")}};
    if (logical.as_object().contains("tool_policy")) identity["tool_policy"] = logical.at("tool_policy");
    if (logical.as_object().contains("model_projection")) identity["model_projection"] = logical.at("model_projection");
    document.at("cache_identity") = sha256_hex(canonical_json(identity));
    document.at("logical_input_sha256") = sha256_hex(canonical_json(logical));
    return compiled_context_from_json(document);
}
void reserve_original(V3Fixture& f, const CapturedModelCall& call) {
    const auto& r = call.request;
    AttemptReservation reservation;
    reservation.call_id = r.call_id; reservation.attempt_id = r.attempt_id;
    reservation.config_snapshot_sha256 = r.config_snapshot_id; reservation.model_ref = r.model.model;
    reservation.reserved_input_tokens = r.reserved_input_tokens; reservation.reserved_output_tokens = r.reserved_output_tokens;
    reservation.reserved_cost_micro_usd = r.reserved_cost_micro_usd; reservation.max_output_tokens = r.max_output_tokens;
    reservation.request_envelope = to_json(r);
    (void)f.store->reserve_attempt(call.actor, r.binding, reservation);
}
void file_path_policy_preserves_originals_and_legacy_replay() {
    for (const auto* policy : {"work-relative-v1", "", "future-v2"}) {
        V3Fixture f("", 6, true);
        const auto work = f.directory.path / "work"; std::filesystem::create_directory(work);
        WorkerTools worker(*f.store, work); OfflineRuntime runtime(*f.store, f.schemas, &worker);
        const auto invoke = [&](const std::string& name, const Json& args) {
            auto call = runtime.capture_call(f.id, Role::Worker, f.task);
            auto document = to_json(call.request.context);
            auto contract = parse_json_strict(str(document.at("input"), "task_contract"));
            CHECK(contract.at("file_path_policy") == "work-relative-v1");
            if (*policy) {
                contract.at("file_path_policy") = policy;
                document.at("input").at("current_state").at("file_path_policy") = policy;
            } else {
                contract.as_object().erase("file_path_policy");
                document.at("input").at("current_state").as_object().erase("file_path_policy");
            }
            document.at("input").at("task_contract") = canonical_json(contract);
            call.request.context = rehashed_context(document); reserve_original(f, call);
            auto response = f.note().complete(call.request);
            response.tool_calls.front().name = name; response.tool_calls.front().arguments = args;
            if (std::string_view(policy) == "future-v2") {
                fails(ErrorCode::Unsupported, [&] { (void)runtime.accept_response(call, response); });
                CHECK(f.store->operation_for_key(call.actor, call.request.binding, call.request.attempt_id, name, args).is_null());
                CHECK(!std::filesystem::exists(work / "a.md")); return Json(nullptr);
            }
            const auto result = runtime.accept_response(call, response);
            CHECK(f.store->attempt(call.request.attempt_id).at("response") == to_json(response));
            CHECK(runtime.accept_response(call, response) == result);
            return result;
        };
        const Json args = Object{{"path", "/work/a.md"}, {"content", "original bytes"}};
        const auto result = invoke("fs_write", args); if (result.is_null()) continue;
        const bool normalized = *policy; auto stored = args;
        if (normalized) stored.at("path") = "a.md";
        CHECK(str(result.at("operation"), "args_sha256") == sha256_hex(canonical_json(stored)));
        CHECK(str(result.at("operation"), "status") == (normalized ? "SUCCEEDED" : "FAILED"));
        const auto binding = f.store->binding(f.task); const auto actor = f.store->host_actor(Role::Worker, f.id, binding.team_id);
        CHECK(worker.execute(actor, binding, f.store->task(f.task).at("state_version").as_int64(),
            str(result.at("operation"), "business_idempotency_key"), "fs_write", stored, "replay").at("replayed") == true);
        CHECK(std::filesystem::exists(work / "a.md") == normalized); if (!normalized) continue;
        CHECK(read_runtime_file(work / "a.md") == "original bytes");
        CHECK(invoke("fs_list", Object{{"path", "/work"}}).at("entries").as_array().size() == 1);
        CHECK(invoke("fs_list", Object{{"path", "/work/"}}).at("entries").as_array().size() == 1);
        const auto denied = invoke("fs_write", Object{{"path", "/elsewhere/a.md"}, {"content", "x"}});
        CHECK(denied.at("correction_feedback").at("reason") == "model_arguments");
        CHECK(str(denied.at("correction_feedback"), "observation").find("workspace root or start with /work/") != std::string::npos);
        CHECK(invoke("fs_write", Object{{"path", "/work/成果.md"}, {"content", "ok"}}).at("operation").at("status") == "SUCCEEDED");
        CHECK(read_runtime_file(work / "成果.md") == "ok");
        std::filesystem::create_directory_symlink(f.directory.path, work / "link");
        for (const auto* path : {"/work/../escaped", "/work/link/escaped"})
            CHECK(invoke("fs_write", Object{{"path", path}, {"content", "x"}}).at("operation").at("status") == "FAILED");
        CHECK(!std::filesystem::exists(f.directory.path / "escaped"));
    }
}
void fixed_role_selection_settles_once_and_keeps_original_history() {
    V3Fixture f("single", 6, true); OfflineRuntime runtime(*f.store, f.schemas);
    auto fake = f.note(); const auto call = runtime.reserve_call(fake, f.id, Role::Worker, f.task);
    auto response = fake.complete(call.request);
    response.tool_calls.front().name = "fs_read";
    response.tool_calls.front().arguments = Object{{"path", "must-not-be-read"}};
    CHECK(std::find(call.request.context.tool_names.begin(), call.request.context.tool_names.end(), "fs_read") != call.request.context.tool_names.end());
    fails(ErrorCode::InvalidInput, [&] { validate_response(call.request, response); });
    const auto result = runtime.accept_response(call, response);
    CHECK(result.at("correction_feedback").at("reason") == "tool_selection");
    const auto budget = f.store->budget(f.id);
    CHECK(runtime.accept_response(call, response) == result); CHECK(f.store->budget(f.id) == budget);
    CHECK(f.store->attempt(call.request.attempt_id).at("response") == to_json(response));
    f.reopen(); auto next_fake = f.note(2);
    OfflineRuntime reopened(*f.store, f.schemas);
    const auto next = reopened.reserve_call(next_fake, f.id, Role::Worker, f.task, 2);
    const auto messages = next.request.context.logical_input.at("messages").as_array();
    CHECK(messages[messages.size() - 2].at("source").at("rejected_tool_selection") == true);
    CHECK(messages[messages.size() - 2].at("tool_calls").as_array().front().at("name") == "fs_read");
    CHECK(reopened.accept_response(next, next_fake.complete(next.request)).is_object());
    CHECK(f.store->tool_feedback(next.request.attempt_id).at("result").as_object().if_contains("correction_feedback") == nullptr);
}
void fixed_role_policy_tampering_and_current_revocation_reject() {
    auto input = described_fixture(); input.tool_policy = "fixed-role-v1"; input.allowed_tools = {"note"};
    const auto valid = compile_structured_context(input, {}, {}, StructuredContextFormat::V3);
    for (const auto& allowed : {Array{}, Array{"undeclared"}, Array{"note", "note"}, Array{false}, Array(65, "note")}) {
        auto changed = to_json(valid); changed.at("input").at("current_state").at("allowed_tools") = allowed;
        fails(ErrorCode::InvalidInput, [&] { (void)rehashed_context(changed); });
    }
    auto changed = to_json(valid); changed.at("input").at("current_state").as_object().erase("allowed_tools");
    fails(ErrorCode::InvalidInput, [&] { (void)rehashed_context(changed); });
    changed = to_json(valid); changed.at("input").at("tool_policy") = "future-v2";
    fails(ErrorCode::InvalidInput, [&] { (void)rehashed_context(changed); });
    fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(input, {}); });
    input.tools.front().name = "submit_plan"; input.allowed_tools = {"submit_plan"};
    fails(ErrorCode::InvalidInput, [&] { (void)compile_structured_context(input, {}, {}, StructuredContextFormat::V3); });
    auto profile = fixture::profile(); auto profile_json = to_json(profile);
    profile_json.as_object()["tool_choice_policy"] = "";
    fails(ErrorCode::InvalidInput, [&] { (void)provider_profile_from_json(profile_json); });
    for (const bool fixed : {false, true}) {
        V3Fixture f("", 6, fixed); OfflineRuntime runtime(*f.store, f.schemas);
        auto call = runtime.capture_call(f.id, Role::Worker, f.task);
        auto document = to_json(call.request.context);
        if (fixed) {
            document.at("input").as_object().erase("tool_policy");
            document.at("input").at("current_state").as_object().erase("allowed_tools");
        } else {
            document.at("input").as_object()["tool_policy"] = "fixed-role-v1";
            document.at("input").at("current_state").as_object()["allowed_tools"] = document.at("tool_names");
        }
        call.request.context = rehashed_context(document);
        const auto budget = f.store->budget(f.id);
        fails(ErrorCode::Unauthorized, [&] { reserve_original(f, call); }); CHECK(f.store->budget(f.id) == budget);
    }
    V3Fixture f("", 6, true); OfflineRuntime runtime(*f.store, f.schemas);
    auto call = runtime.capture_call(f.id, Role::Worker, f.task);
    auto document = to_json(call.request.context);
    auto& definitions = document.at("input").at("prefix").at("tools").as_array();
    definitions.erase(std::find_if(definitions.begin(), definitions.end(), [](const auto& tool) { return tool.at("name") == "run"; }));
    auto& names = document.at("tool_names").as_array();
    names.erase(std::find(names.begin(), names.end(), Json("run")));
    auto forged = call; forged.request.context = rehashed_context(document);
    fails(ErrorCode::Unauthorized, [&] { reserve_original(f, forged); });
    wls::test::live::sql(f.database, "UPDATE runtime_policy SET denied_tools='[\"note\"]',state_version=state_version+1");
    fails(ErrorCode::Unauthorized, [&] { reserve_original(f, call); });
    const auto next = runtime.capture_call(f.id, Role::Worker, f.task);
    CHECK(next.request.context.tool_names == call.request.context.tool_names);
    CHECK(model_allowed_tools(next.request.context) == std::vector<std::string>{"ask"});
}

} // namespace

namespace {
using TestCase = ::test::Case;

std::vector<TestCase> legacy_cases() {
    return {
        {"captured path policy preserves original responses and legacy replay", file_path_policy_preserves_originals_and_legacy_replay},
        {"fixed role disallowed selection settles once and preserves original history", fixed_role_selection_settles_once_and_keeps_original_history},
        {"fixed role policy tampering and current revocation reject", fixed_role_policy_tampering_and_current_revocation_reject},
        {"v3 encoded five round prefix preserves native originals", [] { v3_five_round_encoded_prefix_and_originals(); }},
        {"v3 capture reserve restore history policy combinations", [] { v3_capture_reserve_restore_policy_combinations(); }},
        {"v3 AMEND and reopen preserve anchor and original CAS", v3_amend_reopen_and_original_cas},
        {"v3 cross version and projection corruption reject", v3_cross_version_and_projection_corruption_reject},
        {"separate structure and exact restoration", separate_fields_and_round_trip},
        {"canonical tools and dependencies", canonical_tools_and_dependencies},
        {"logical request and prefix cache fingerprints", cache_and_full_request_are_distinct},
        {"pairing and dynamic authority refusals", pairing_and_role_refusals},
        {"parallel turns need every result", parallel_turn_requires_all_results},
        {"self-consistent forged hash does not authorize malformed structure", forged_hash_does_not_authorize_structure},
        {"metadata and projection tampering reject", metadata_tampering_rejects},
        {"capacity and unconverted legacy input reject", bounds_and_legacy_messages_refuse},
        {"legacy serialized bytes unchanged", legacy_bytes_unchanged},
        {"legacy full canonical and wire fingerprints", legacy_full_byte_fingerprints},
        {"guided prompt identifiers name registered tools", guided_prompt_identifiers_are_registered_tools},
        {"legacy frozen captures keep complete context hashes and wire bytes", legacy_frozen_capture_keeps_complete_context_and_wire_bytes},
        {"guided capture projects real adapters and stable role rules", [] { guided_capture_uses_real_adapter_and_stable_role_rules(); }},
        {"all builtin v3 descriptions preserve original schemas", all_builtin_v3_descriptions_preserve_registry},
        {"v3 description hashes roundtrip and native encoding", v3_hash_roundtrip_and_wire},
        {"v3 missing malformed or oversized descriptions reject", v3_missing_invalid_and_oversized_descriptions_refuse},
        {"FakeModel consumes restored structured envelope", fake_uses_same_persisted_structure},
        {"full envelope hash covers reservation and format", full_envelope_integrity_covers_reservations},
        {"restored envelopes and later mutable requests keep all validation gates", restored_requests_keep_deep_and_public_validation},
        {"provider tool IDs isolated by original attempt", same_provider_ids_have_distinct_logical_ids},
        {"missing and unapplied attempts remain observations", missing_late_and_invalid_are_observations},
        {"source corruption refuses", corrupted_source_binding_refuses},
        {"frozen format and legacy defaults", frozen_choice_and_legacy_default},
        {"Store refuses frozen format mismatch before reservation", store_rejects_format_policy_mismatch},
        {"responses output roundtrips restore and hash coverage", responses_output_roundtrip_restore_and_hash},
        {"window measurement counts payload and quota-fences recent reasoning", window_measurement_counts_payload_and_quota_guards_recent_reasoning}
    };
}

std::vector<TestCase> lean_cases() {
    return {
        {"actionable markers and budget fallback preserve original uncertainty", actionable_markers_and_budget_boundaries},
        {"actionable events and task fields retain typed boundaries", actionable_events_and_task_boundaries},
        {"actionable Dispatcher references preserve original contracts", actionable_dispatcher_originals_and_references},
        {"compact Dispatcher plan copies keep distinct target revisions", compact_dispatcher_plan_keeps_target_versions},
        {"compact Worker event references require the original semantic binding", compact_worker_events_require_the_original_semantic_binding},
        {"compact state preserves typed targets revisions and unresolved facts", compact_state_respects_role_target_and_original_facts},
        {"compact file views require complete matching original bytes", compact_file_views_require_complete_matching_originals},
        {"pre-E2 v1 v2 nonlean and unmarked lean bytes match independent old-library goldens", pre_e2_wire_and_envelope_goldens},
        {"lean wire projection preserves originals and ordered prefix", lean_wire_projection_preserves_originals_and_ordered_prefix},
        {"lean projection preserves errors and checks originals", lean_projection_preserves_errors_and_checks_originals},
        {"guided capture projects lean adapter and stable role rules", lean_guided_capture_uses_real_adapter_and_stable_role_rules},
        {"lean v3 capture reserve restore policy combinations", lean_v3_capture_reserve_restore_policy_combinations},
        {"lean frozen format and policy selection", lean_frozen_choice_and_policy}
    };
}

std::vector<TestCase> all_cases() {
    return {
        {"actionable markers and budget fallback preserve original uncertainty", actionable_markers_and_budget_boundaries},
        {"actionable events and task fields retain typed boundaries", actionable_events_and_task_boundaries},
        {"actionable Dispatcher references preserve original contracts", actionable_dispatcher_originals_and_references},
        {"compact Dispatcher plan copies keep distinct target revisions", compact_dispatcher_plan_keeps_target_versions},
        {"compact Worker event references require the original semantic binding", compact_worker_events_require_the_original_semantic_binding},
        {"compact state preserves typed targets revisions and unresolved facts", compact_state_respects_role_target_and_original_facts},
        {"compact file views require complete matching original bytes", compact_file_views_require_complete_matching_originals},
        {"pre-E2 v1 v2 nonlean and unmarked lean bytes match independent old-library goldens", pre_e2_wire_and_envelope_goldens},
        {"captured path policy preserves original responses and legacy replay", file_path_policy_preserves_originals_and_legacy_replay},
        {"v3 encoded five round prefix preserves native originals", [] { v3_five_round_encoded_prefix_and_originals(); }},
        {"lean wire projection preserves originals and ordered prefix", lean_wire_projection_preserves_originals_and_ordered_prefix},
        {"lean projection preserves errors and checks originals", lean_projection_preserves_errors_and_checks_originals},
        {"fixed role disallowed selection settles once and preserves original history", fixed_role_selection_settles_once_and_keeps_original_history},
        {"fixed role policy tampering and current revocation reject", fixed_role_policy_tampering_and_current_revocation_reject},
        {"v3 capture reserve restore history policy combinations", [] { v3_capture_reserve_restore_policy_combinations(); }},
        {"lean v3 capture reserve restore policy combinations", lean_v3_capture_reserve_restore_policy_combinations},
        {"v3 AMEND and reopen preserve anchor and original CAS", v3_amend_reopen_and_original_cas},
        {"v3 cross version and projection corruption reject", v3_cross_version_and_projection_corruption_reject},
        {"separate structure and exact restoration", separate_fields_and_round_trip},
        {"canonical tools and dependencies", canonical_tools_and_dependencies},
        {"logical request and prefix cache fingerprints", cache_and_full_request_are_distinct},
        {"pairing and dynamic authority refusals", pairing_and_role_refusals},
        {"parallel turns need every result", parallel_turn_requires_all_results},
        {"self-consistent forged hash does not authorize malformed structure", forged_hash_does_not_authorize_structure},
        {"metadata and projection tampering reject", metadata_tampering_rejects},
        {"capacity and unconverted legacy input reject", bounds_and_legacy_messages_refuse},
        {"legacy serialized bytes unchanged", legacy_bytes_unchanged},
        {"legacy full canonical and wire fingerprints", legacy_full_byte_fingerprints},
        {"guided prompt identifiers name registered tools", guided_prompt_identifiers_are_registered_tools},
        {"legacy frozen captures keep complete context hashes and wire bytes", legacy_frozen_capture_keeps_complete_context_and_wire_bytes},
        {"guided capture projects real adapters and stable role rules", [] { guided_capture_uses_real_adapter_and_stable_role_rules(); }},
        {"guided capture projects lean adapter and stable role rules", lean_guided_capture_uses_real_adapter_and_stable_role_rules},
        {"all builtin v3 descriptions preserve original schemas", all_builtin_v3_descriptions_preserve_registry},
        {"v3 description hashes roundtrip and native encoding", v3_hash_roundtrip_and_wire},
        {"v3 missing malformed or oversized descriptions reject", v3_missing_invalid_and_oversized_descriptions_refuse},
        {"FakeModel consumes restored structured envelope", fake_uses_same_persisted_structure},
        {"full envelope hash covers reservation and format", full_envelope_integrity_covers_reservations},
        {"restored envelopes and later mutable requests keep all validation gates", restored_requests_keep_deep_and_public_validation},
        {"provider tool IDs isolated by original attempt", same_provider_ids_have_distinct_logical_ids},
        {"missing and unapplied attempts remain observations", missing_late_and_invalid_are_observations},
        {"source corruption refuses", corrupted_source_binding_refuses},
        {"frozen format and legacy defaults", frozen_choice_and_legacy_default},
        {"lean frozen format and policy selection", lean_frozen_choice_and_policy},
        {"Store refuses frozen format mismatch before reservation", store_rejects_format_policy_mismatch},
        {"responses output roundtrips restore and hash coverage", responses_output_roundtrip_restore_and_hash},
        {"window measurement counts payload and quota-fences recent reasoning", window_measurement_counts_payload_and_quota_guards_recent_reasoning}
    };
}
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::string_view group = "all";
    if (argc != 1) {
        if (argc != 3 || std::string_view(argv[1]) != "--group") {
            std::cerr << "usage: r1_structured_tests [--group all|legacy|legacy-rest|legacy-capture|lean]\n";
            return 2;
        }
        group = argv[2];
    }
    if (group != "all" && group != "legacy" && group != "legacy-rest" &&
        group != "legacy-capture" && group != "lean") {
        std::cerr << "unknown group: " << group << "\n"
                  << "usage: r1_structured_tests [--group all|legacy|legacy-rest|legacy-capture|lean]\n";
        return 2;
    }

    const auto legacy = legacy_cases();
    const auto lean = lean_cases();
    std::vector<TestCase> legacy_rest, legacy_capture;
    std::set<std::string> legacy_names;
    for (const auto& test_case : legacy) {
        legacy_names.insert(test_case.first);
        if (test_case.first == "v3 capture reserve restore history policy combinations")
            legacy_capture.push_back(test_case);
        else legacy_rest.push_back(test_case);
    }
    if (legacy.size() != 33 || legacy_names.size() != legacy.size() ||
        legacy_rest.size() != 32 || legacy_capture.size() != 1 ||
        legacy_rest.size() + legacy_capture.size() != legacy.size()) {
        std::cerr << "invalid legacy case partition: expected 32 rest and one unique capture case\n";
        return 2;
    }
    std::vector<TestCase> selected;
    if (group == "all") selected = all_cases();
    else if (group == "legacy-rest") selected = legacy_rest;
    else if (group == "legacy-capture") selected = legacy_capture;
    else selected = group == "legacy" ? legacy : lean;
    std::cout << "GROUP r1_structured " << group << ": " << selected.size() << " top-level cases\n";
    return ::test::run(selected);
}
