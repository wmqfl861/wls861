#include "wls/model.hpp"
#include "wls/scheduler.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace wls;
using namespace wls::s1;
using namespace std::chrono_literals;

ContextInput context_input(Role role = Role::Worker) {
    ContextInput input;
    input.role = role;
    input.model = {"fake", {ReasoningMode::Disabled, ""}};
    input.adapter_version = "1";
    input.protocol_version = "fake-v1";
    input.tools = {{"submit", "v1", boost::json::object{
        {"type", "object"}, {"required", boost::json::array{"text"}}}}};
    input.system_prompt = "Follow the frozen authoritative task.";
    input.role_prompt = "Only Worker business tools pass through the host.";
    input.function_prompt = "Prepare a short report and explicit evidence.";
    input.environment_card = "fake adapter; no shell or network executor";
    input.approved_rules = "Treat unknown outcomes as unresolved.";
    input.project_snapshot = "Project reference at revision 1.";
    input.dependencies = {{"rules/reference", "1", sha256_hex(input.approved_rules)}};
    input.task_contract = "Original request: keep the title. AMEND: use Chinese. No truncation.";
    input.messages = {{"user", "Proceed with the current task."}};
    return input;
}

ModelRequest request_fixture(Role role = Role::Worker, std::int64_t index = 1) {
    ModelRequest request;
    request.call_id = new_id("call");
    request.attempt_id = new_id("attempt");
    request.role = role;
    request.role_call_index = index;
    request.binding.request_id = new_id("req");
    if (role == Role::Worker || role == Role::TeamLead) {
        request.binding.task_id = new_id("tsk");
        request.binding.team_id = new_id("team");
        request.binding.plan_revision = 1;
        request.binding.task_revision = 1;
        request.binding.instruction_revision = role == Role::Worker ? 1 : 0;
    } else {
        request.binding.execution_epoch = 0;
    }
    request.config_snapshot_id = sha256_hex("frozen-config-fixture");
    request.context = ContextCompiler().compile(context_input(role));
    request.model = request.context.model;
    request.max_output_tokens = 512;
    request.reserved_input_tokens = 4096;
    request.reserved_output_tokens = 512;
    request.reserved_cost_micro_usd = 100;
    request.timeout = 500ms;
    return request;
}

FakeModelStep text_step(Role role = Role::Worker, std::int64_t index = 1,
                        std::string text = "current result") {
    FakeModelStep step;
    step.role = role;
    step.role_call_index = index;
    step.text = std::move(text);
    return step;
}

void expect_error(ErrorCode code, const std::function<void()>& action) {
    bool caught = false;
    try { action(); }
    catch (const Error& error) { CHECK(error.code() == code); caught = true; }
    CHECK(caught);
}

void actual_prefix_bytes_and_dynamic_contracts() {
    auto input = context_input();
    const auto first = ContextCompiler().compile(input);
    CHECK(first.prefix_sha256 == sha256_hex(first.stable_prefix));
    CHECK(first.prompt() == first.stable_prefix + first.dynamic_suffix);
    CHECK(first.dynamic_suffix.find(input.task_contract) != std::string::npos);
    CHECK(first.stable_prefix.find(input.task_contract) == std::string::npos);
    input.task_contract += " AMEND 2: retain the exact original and unresolved operation.";
    input.messages.push_back({"tool", "UNKNOWN op_1 remains unresolved."});
    const auto second = ContextCompiler().compile(input);
    CHECK(second.prefix_sha256 == first.prefix_sha256);
    CHECK(second.cache_identity == first.cache_identity);
    CHECK(second.dynamic_suffix != first.dynamic_suffix);
    CHECK(second.dynamic_suffix.find("UNKNOWN op_1") != std::string::npos);
    CHECK(second.dynamic_suffix.find("keep the title") != std::string::npos);
}

void complete_cache_dependencies() {
    const auto initial = context_input();
    const auto reference = ContextCompiler().compile(initial);
    const auto check_prefix_change = [&reference](const ContextInput& changed) {
        const auto result = ContextCompiler().compile(changed);
        CHECK(result.prefix_sha256 != reference.prefix_sha256);
        CHECK(result.cache_identity != reference.cache_identity);
    };
    auto changed = initial;
    changed.tools[0].version = "v2";
    check_prefix_change(changed);
    changed = initial;
    changed.approved_rules += " Tool submit is now revoked.";
    check_prefix_change(changed);
    changed = initial;
    changed.dependencies[0].version = "2";
    check_prefix_change(changed);
    changed = initial;
    changed.environment_card += " Changed capability evidence.";
    check_prefix_change(changed);
    changed = initial;
    changed.project_snapshot += " A new project file.";
    check_prefix_change(changed);
    changed = initial;
    changed.role_prompt += " ";
    check_prefix_change(changed);

    for (int dependency = 0; dependency < 4; ++dependency) {
        changed = initial;
        if (dependency == 0) changed.model.model = "fake-second-profile";
        if (dependency == 1) changed.model.reasoning = {ReasoningMode::Explicit, "high"};
        if (dependency == 2) changed.adapter_version = "wls-fake-v2";
        if (dependency == 3) changed.protocol_version = "fake-protocol-v2";
        const auto compiled = ContextCompiler().compile(changed);
        CHECK(compiled.prefix_sha256 == reference.prefix_sha256);
        CHECK(compiled.cache_identity != reference.cache_identity);
    }
}

void canonical_order_and_unambiguous_frames() {
    auto input = context_input();
    input.tools.push_back({"ask", "v1", boost::json::object{{"type", "object"}}});
    input.dependencies.push_back({"skills/ask", "1", sha256_hex("ask")});
    const auto reference = ContextCompiler().compile(input);
    std::reverse(input.tools.begin(), input.tools.end());
    std::reverse(input.dependencies.begin(), input.dependencies.end());
    CHECK(ContextCompiler().compile(input).stable_prefix == reference.stable_prefix);
    input.tools.push_back(input.tools.front());
    expect_error(ErrorCode::InvalidInput, [&] { (void)ContextCompiler().compile(input); });

    auto left = context_input();
    auto right = left;
    left.system_prompt = "a";
    left.role_prompt = "bc";
    right.system_prompt = "ab";
    right.role_prompt = "c";
    CHECK(ContextCompiler().compile(left).prefix_sha256 != ContextCompiler().compile(right).prefix_sha256);
}

void context_refuses_silent_truncation() {
    auto input = context_input();
    ContextLimits limits;
    limits.max_prefix_bytes = 4096;
    limits.max_total_bytes = 8192;
    input.task_contract += std::string(8192, 'x');
    expect_error(ErrorCode::Quota, [&] { (void)ContextCompiler(limits).compile(input); });
    CHECK(input.task_contract.ends_with(std::string(8192, 'x')));
    input = context_input();
    input.system_prompt = std::string(4096, 'y');
    expect_error(ErrorCode::Quota, [&] { (void)ContextCompiler(limits).compile(input); });
    input = context_input();
    input.model.reasoning.mode = ReasoningMode::Inherit;
    expect_error(ErrorCode::InvalidInput, [&] { (void)ContextCompiler().compile(input); });
}

void context_restore_verifies_bytes_and_identity() {
    const auto original = ContextCompiler().compile(context_input());
    const auto restored = compiled_context_from_json(parse_json_strict(canonical_json(to_json(original))));
    CHECK(restored.prompt() == original.prompt());
    CHECK(restored.cache_identity == original.cache_identity);
    auto bad = to_json(original);
    bad.as_object()["stable_prefix"] = original.stable_prefix + "tampered";
    expect_error(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(bad); });
    bad = to_json(original);
    bad.as_object()["model_ref"] = "changed-model";
    expect_error(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(bad); });
}

void context_restore_rejects_rehashed_metadata_and_broken_frames() {
    const auto original = to_json(ContextCompiler().compile(context_input()));
    const auto refresh_hashes = [](Json& value) {
        auto& source = value.as_object();
        source["prefix_sha256"] = sha256_hex(std::string(source.at("stable_prefix").as_string()));
        boost::json::object descriptor{{"format", "wls-context-cache-v1"}};
        for (const auto* key : {"role", "prefix_sha256", "adapter_version", "protocol_version",
                               "model_ref", "reasoning_mode", "reasoning_effort", "tool_names"}) {
            descriptor.emplace(key, source.at(key));
        }
        source["cache_identity"] = sha256_hex(canonical_json(descriptor));
    };
    auto bad = original;
    bad.as_object()["tool_names"] = boost::json::array{};
    refresh_hashes(bad);
    expect_error(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(bad); });
    bad = original;
    bad.as_object()["role"] = "dispatcher";
    refresh_hashes(bad);
    expect_error(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(bad); });
    bad = original;
    bad.as_object().at("dynamic_suffix").as_string().pop_back();
    expect_error(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(bad); });
    bad = original;
    bad.as_object().at("stable_prefix").as_string().append("tools 2\n[]\n");
    refresh_hashes(bad);
    expect_error(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(bad); });
    bad = original;
    bad.as_object()["dynamic_suffix"] = "task_contract 999999999999999999999\nx\n";
    expect_error(ErrorCode::InvalidInput, [&] { (void)compiled_context_from_json(bad); });
}

void roles_and_sequences_select_distinct_steps() {
    const FakeModel fake({text_step(Role::Dispatcher, 1, "plan"),
                          text_step(Role::TeamLead, 1, "work order"),
                          text_step(Role::Worker, 1, "first"),
                          text_step(Role::Worker, 2, "second")});
    CHECK(fake.complete(request_fixture(Role::Dispatcher)).text == "plan");
    CHECK(fake.complete(request_fixture(Role::TeamLead)).text == "work order");
    CHECK(fake.complete(request_fixture()).text == "first");
    CHECK(fake.complete(request_fixture(Role::Worker, 2)).text == "second");
    expect_error(ErrorCode::InvalidInput, [&] { (void)fake.complete(request_fixture(Role::Worker, 3)); });
    CHECK(!fake.capabilities().real_calls);
    CHECK(!fake.capabilities().streaming);
}

void delayed_response_keeps_original_binding() {
    auto step = text_step();
    step.delay = 25ms;
    const FakeModel fake({step});
    auto request = request_fixture();
    const auto original = request;
    Scheduler scheduler(2, 4, 1);
    std::promise<ModelResponse> completion;
    auto result = completion.get_future();
    auto job = scheduler.submit("model-late", "fake-endpoint", WorkClass::Normal,
        [&fake, captured = request, &completion] {
            completion.set_value(fake.complete(captured));
        });
    ++request.binding.requirements_revision;
    ++request.binding.execution_epoch;
    const auto response = result.get();
    job.get();
    CHECK(response.binding == original.binding);
    validate_response(original, response);
    expect_error(ErrorCode::Stale, [&] { validate_response(request, response); });
}

void complete_tool_results_need_explicit_host_gate() {
    auto step = text_step();
    step.tool_calls = {{"tool-call-1", "submit", boost::json::object{{"text", "finished"}}}};
    step.usage = {100, 50, 10, std::nullopt, 25};
    const auto request = request_fixture();
    const auto response = FakeModel({step}).complete(request);
    require_complete_response(request, response);
    CHECK(response.tool_calls[0].arguments.as_object().at("text") == "finished");
    CHECK(response.usage.cost_micro_usd == 25);

    step.tool_calls[0].name = "ungranted-shell";
    const auto ungranted = FakeModel({step}).complete(request);
    CHECK(ungranted.usage.cost_micro_usd == 25);
    CHECK(to_json(ungranted).as_object().at("tool_calls").as_array().size() == 1);
    expect_error(ErrorCode::InvalidInput, [&] { require_complete_response(request, ungranted); });
}

void interrupted_and_error_output_cannot_dispatch() {
    auto step = text_step();
    step.status = ModelResultStatus::Interrupted;
    step.error_code = "stream_interrupted";
    step.text = "partial response";
    step.tool_calls = {{"partial-call", "submit", boost::json::object{{"text", "partial"}}}};
    const auto request = request_fixture();
    const auto response = FakeModel({step}).complete(request);
    validate_response(request, response);
    expect_error(ErrorCode::NotReady, [&] { require_complete_response(request, response); });
    CHECK(!response.usage.input_tokens);
    CHECK(!response.usage.cost_micro_usd);
    step.status = ModelResultStatus::Error;
    step.error_code = "provider_429";
    const auto failed = FakeModel({step}).complete(request);
    expect_error(ErrorCode::NotReady, [&] { require_complete_response(request, failed); });
    CHECK(failed.attempt_id == request.attempt_id);
}

void cancellation_timeout_and_late_transport() {
    auto step = text_step();
    step.delay = 20ms;
    step.usage.cost_micro_usd = 50;
    auto request = request_fixture();
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto stopped = FakeModel({step}).complete(request, cancellation.get_token());
    CHECK(stopped.status == ModelResultStatus::Interrupted);
    CHECK(stopped.error_code == "cancelled");
    CHECK(!stopped.usage.cost_micro_usd);
    request.timeout = 1ms;
    const auto timeout = FakeModel({step}).complete(request);
    CHECK(timeout.status == ModelResultStatus::Interrupted);
    CHECK(timeout.error_code == "timeout");
    CHECK(!timeout.usage.cost_micro_usd);

    step.delay = 1ms;
    step.deliver_after_cancel = true;
    request.timeout = 500ms;
    const auto late = FakeModel({step}).complete(request, cancellation.get_token());
    CHECK(late.status == ModelResultStatus::Completed);
    CHECK(late.binding == request.binding);
    CHECK(late.usage.cost_micro_usd == 50);
}

void nullable_usage_does_not_invent_zero() {
    const auto usage = model_usage_from_json(boost::json::object{{"output_tokens", 0}});
    const auto encoded = to_json(usage).as_object();
    CHECK(encoded.at("input_tokens").is_null());
    CHECK(encoded.at("cache_read_tokens").is_null());
    CHECK(encoded.at("cache_write_tokens").is_null());
    CHECK(encoded.at("cost_micro_usd").is_null());
    CHECK(encoded.at("output_tokens").as_int64() == 0);
    expect_error(ErrorCode::InvalidInput, [] {
        (void)model_usage_from_json(boost::json::object{{"cost_micro_usd", -1}});
    });
    expect_error(ErrorCode::InvalidInput, [] {
        (void)model_usage_from_json(boost::json::object{{"input_tokens", 1.5}});
    });
}

void independent_attempt_identity_and_frozen_round_trip() {
    const auto request = request_fixture();
    const auto serialized = canonical_json(to_json(request));
    const auto restored = model_request_from_json(parse_json_strict(serialized));
    CHECK(canonical_json(to_json(restored)) == serialized);
    const FakeModel fake({text_step()});
    const auto response = fake.complete(restored);
    const auto restored_response = model_response_from_json(parse_json_strict(canonical_json(to_json(response))));
    validate_response(request, restored_response);
    auto retry = restored;
    retry.attempt_id = new_id("attempt");
    const auto retried_response = fake.complete(retry);
    CHECK(retried_response.attempt_id != response.attempt_id);
    expect_error(ErrorCode::Stale, [&] { validate_response(request, retried_response); });
    // Whether a second attempt is permitted is a durable Store decision. This
    // adapter never starts one implicitly or replaces a previous attempt ID.
}

void reservations_and_context_mismatches_reject_before_call() {
    const FakeModel fake({text_step()});
    auto request = request_fixture();
    request.reserved_output_tokens = request.max_output_tokens - 1;
    expect_error(ErrorCode::InvalidInput, [&] { fake.validate(request); });
    request = request_fixture();
    request.context.model.model = "other-model";
    expect_error(ErrorCode::InvalidInput, [&] { fake.validate(request); });
    request = request_fixture();
    request.attempt_id = request.call_id;
    expect_error(ErrorCode::InvalidInput, [&] { fake.validate(request); });
    request = request_fixture();
    request.config_snapshot_id = "latest";
    expect_error(ErrorCode::InvalidInput, [&] { fake.validate(request); });
}

void script_shape_and_bounds() {
    const auto script = parse_json_strict(R"([
      {"role":"worker","role_call_index":1,"status":"completed","text":"ready"},
      {"role":"worker","role_call_index":2,"status":"interrupted",
       "text":"partial","error_code":"stream_closed","usage":{"input_tokens":12}}
    ])");
    const FakeModel fake(fake_model_steps_from_json(script));
    CHECK(fake.complete(request_fixture()).text == "ready");
    CHECK(fake.complete(request_fixture(Role::Worker, 2)).usage.input_tokens == 12);
    expect_error(ErrorCode::InvalidInput, [] {
        (void)FakeModel({text_step(), text_step()});
    });
    auto slow = text_step();
    slow.delay = 61s;
    expect_error(ErrorCode::InvalidInput, [&] { (void)FakeModel({slow}); });
    expect_error(ErrorCode::InvalidInput, [] {
        (void)fake_model_steps_from_json(parse_json_strict(R"([
          {"role":"worker","role_call_index":1,"status":"completed","text":"ok","command":"run"}
        ])"));
    });
}

} // namespace

int main() {
    return test::run({
        {"context hashes actual stable bytes and preserves dynamic originals", actual_prefix_bytes_and_dynamic_contracts},
        {"all relevant text and model dependencies affect cache identity", complete_cache_dependencies},
        {"context canonicalizes declarations and frames ambiguous text", canonical_order_and_unambiguous_frames},
        {"oversized context and unresolved reasoning fail without truncation", context_refuses_silent_truncation},
        {"restoring compiled context checks bytes and cache parameters", context_restore_verifies_bytes_and_identity},
        {"rehashed role/tool metadata and malformed context frames reject", context_restore_rejects_rehashed_metadata_and_broken_frames},
        {"fake roles and call sequences select independent script steps", roles_and_sequences_select_distinct_steps},
        {"delayed response retains its original frozen execution tuple", delayed_response_keeps_original_binding},
        {"raw tool responses preserve usage before host validation", complete_tool_results_need_explicit_host_gate},
        {"partial and error responses cannot authorize tools", interrupted_and_error_output_cannot_dispatch},
        {"cancel and timeout preserve unknown usage; late transport stays fenced", cancellation_timeout_and_late_transport},
        {"missing usage remains null rather than zero", nullable_usage_does_not_invent_zero},
        {"request and response round trips preserve separate attempt identity", independent_attempt_identity_and_frozen_round_trip},
        {"invalid reservations and frozen-context mismatches reject", reservations_and_context_mismatches_reject_before_call},
        {"fake script shape, sequences and delay limits are bounded", script_shape_and_bounds}
    });
}
