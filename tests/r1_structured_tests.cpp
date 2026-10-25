#include "wls/structured_context.hpp"
#include "wls/offline_runtime.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>

namespace {
using namespace wls;
using namespace wls::s1;
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
void same_provider_ids_have_distinct_logical_ids() {
    const auto a = attempt(), b = attempt(); const auto m = structured_attempt_history({a, b});
    CHECK(m.size() == 4); CHECK(m[0].tool_calls.front().call_id != m[2].tool_calls.front().call_id);
    CHECK(m[0].source.at("original_tool_call_id") == "reused");
    CHECK(m[0].source.at("frozen_binding") == a.at("frozen_binding"));
    CHECK(m[1].tool_call_id == m[0].tool_calls.front().call_id);
    CHECK(parse_json_strict(m[1].content) == a.at("feedback").at("result"));
    CHECK(compile_structured_context(base(), m).structured());
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
    test::TempDir tmp;
    FrozenRequestConfig load(const std::string& format) {
        auto yaml = read_runtime_file(root / "config/examples/system.yaml");
        const std::string marker = "  request_format: framed-v1\n";
        const auto pos = yaml.find(marker); CHECK(pos != std::string::npos);
        yaml.replace(pos, marker.size(), format.empty() ? "" : "  request_format: " + format + "\n");
        { std::ofstream out(tmp.path / "system.yaml"); out << yaml; CHECK(out.good()); }
        return load_config_catalog(tmp.path / "system.yaml", root / "config/examples/functions", root / "prompts").freeze("general");
    }
};
void frozen_choice_and_legacy_default() {
    ConfigFixture f; const auto old = f.load(""); const auto next = f.load("structured-v2");
    CHECK(old.context_request_format() == "framed-v1"); CHECK(next.context_request_format() == "structured-v2");
    CHECK(FrozenRequestConfig::restore(old.serialize(), old.sha256()).context_request_format() == "framed-v1");
    CHECK(FrozenRequestConfig::restore(next.serialize(), next.sha256()).context_request_format() == "structured-v2");
    CHECK(next.runtime_support().at("history_window_integrated") == false);
    fails(ErrorCode::InvalidInput, [&] { (void)f.load("unknown-v99"); });
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
}
} // namespace

int main() {
    return test::run({
        {"separate structure and exact restoration", separate_fields_and_round_trip},
        {"canonical tools and dependencies", canonical_tools_and_dependencies},
        {"logical request and prefix cache fingerprints", cache_and_full_request_are_distinct},
        {"pairing and dynamic authority refusals", pairing_and_role_refusals},
        {"parallel turns need every result", parallel_turn_requires_all_results},
        {"self-consistent forged hash does not authorize malformed structure", forged_hash_does_not_authorize_structure},
        {"metadata and projection tampering reject", metadata_tampering_rejects},
        {"capacity and unconverted legacy input reject", bounds_and_legacy_messages_refuse},
        {"legacy serialized bytes unchanged", legacy_bytes_unchanged},
        {"FakeModel consumes restored structured envelope", fake_uses_same_persisted_structure},
        {"full envelope hash covers reservation and format", full_envelope_integrity_covers_reservations},
        {"provider tool IDs isolated by original attempt", same_provider_ids_have_distinct_logical_ids},
        {"missing and unapplied attempts remain observations", missing_late_and_invalid_are_observations},
        {"source corruption refuses", corrupted_source_binding_refuses},
        {"frozen format and legacy defaults", frozen_choice_and_legacy_default},
        {"Store refuses frozen format mismatch before reservation", store_rejects_format_policy_mismatch}
    });
}
