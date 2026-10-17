#include "wls/config.hpp"
#include "test_support.hpp"

#include <utility>

using namespace wls;

namespace {

CapabilityRegistry registry() {
    CapabilityRegistry value;
    value.add("fixture/root", {{"low", "high"}, true, true});
    value.add("fixture/alternate", {{"low", "high", "max"}, true, true});
    value.add("fixture/default-only", {{}, false, true});
    value.add("fixture/explicit-only", {{"high"}, false, false});
    return value;
}

template<class Function>
void expect_error(ConfigurationErrorCode code, Function&& function) {
    bool caught = false;
    try { function(); }
    catch (const ConfigurationError& error) {
        CHECK(error.code() == code);
        caught = true;
    }
    CHECK(caught);
}

void direct_frozen_root_inheritance() {
    const auto capabilities = registry();
    ModelSelection current_root{"fixture/root", {ReasoningMode::Explicit, "high"}};
    const ModelSelection frozen_root = current_root;
    const ModelSelection dispatcher_override{
        "fixture/alternate", {ReasoningMode::Explicit, "max"}};
    const auto dispatcher = resolve_model(frozen_root, dispatcher_override, capabilities);
    CHECK(dispatcher == dispatcher_override);

    const ModelSelection team_lead_override{"", {ReasoningMode::Disabled, ""}};
    const auto team_lead = resolve_model(frozen_root, team_lead_override, capabilities);
    CHECK(team_lead.model == "fixture/root");
    CHECK(team_lead.reasoning.mode == ReasoningMode::Disabled);

    // Changing current Root or intermediate role configuration cannot mutate a
    // request's separately held snapshot or make Worker inherit parent fields.
    current_root = {"fixture/alternate", {ReasoningMode::ProviderDefault, ""}};
    const auto worker = resolve_model(frozen_root, {}, capabilities);
    CHECK(worker == frozen_root);
    CHECK(worker != dispatcher);
    CHECK(worker != team_lead);
    CHECK(worker != current_root);

    const auto model_only = resolve_model(frozen_root,
        {"fixture/alternate", {}}, capabilities);
    CHECK(model_only.model == "fixture/alternate");
    CHECK(model_only.reasoning == frozen_root.reasoning);
    const auto reasoning_only = resolve_model(frozen_root,
        {"", {ReasoningMode::Explicit, "low"}}, capabilities);
    CHECK(reasoning_only.model == frozen_root.model);
    CHECK(reasoning_only.reasoning.effort == "low");
}

void reasoning_modes_remain_distinct() {
    const auto capabilities = registry();
    const ModelSelection root{"fixture/root", {ReasoningMode::Explicit, "high"}};
    const auto inherited = resolve_model(root, {}, capabilities);
    const auto disabled = resolve_model(root,
        {"", {ReasoningMode::Disabled, ""}}, capabilities);
    const auto provider_default = resolve_model(root,
        {"", {ReasoningMode::ProviderDefault, ""}}, capabilities);
    const auto explicit_low = resolve_model(root,
        {"", {ReasoningMode::Explicit, "low"}}, capabilities);
    CHECK(inherited.reasoning == root.reasoning);
    CHECK(disabled.reasoning.mode == ReasoningMode::Disabled);
    CHECK(provider_default.reasoning.mode == ReasoningMode::ProviderDefault);
    CHECK(explicit_low.reasoning.mode == ReasoningMode::Explicit);
    CHECK(disabled.reasoning != provider_default.reasoning);
    CHECK(inherited.reasoning != explicit_low.reasoning);
}

void registry_rejects_unknown_and_unsupported() {
    const auto capabilities = registry();
    const ModelSelection root{"fixture/root", {ReasoningMode::Explicit, "high"}};
    expect_error(ConfigurationErrorCode::UnknownModel, [&] {
        (void)resolve_model(root, {"fixture/unknown", {}}, capabilities);
    });
    expect_error(ConfigurationErrorCode::UnsupportedReasoning, [&] {
        (void)resolve_model(root, {"", {ReasoningMode::Explicit, "max"}}, capabilities);
    });
    // A model override still inherits Root reasoning independently, and must
    // fail when that setting is unsupported by the selected model.
    expect_error(ConfigurationErrorCode::UnsupportedReasoning, [&] {
        (void)resolve_model(root, {"fixture/default-only", {}}, capabilities);
    });
    const auto valid_default = resolve_model(root,
        {"fixture/default-only", {ReasoningMode::ProviderDefault, ""}}, capabilities);
    CHECK(valid_default.reasoning.mode == ReasoningMode::ProviderDefault);
    expect_error(ConfigurationErrorCode::UnsupportedReasoning, [&] {
        (void)resolve_model(root,
            {"fixture/default-only", {ReasoningMode::Disabled, ""}}, capabilities);
    });
    expect_error(ConfigurationErrorCode::UnsupportedReasoning, [&] {
        (void)resolve_model(root,
            {"fixture/explicit-only", {ReasoningMode::ProviderDefault, ""}}, capabilities);
    });
}

void invalid_configuration_is_explicit() {
    auto capabilities = registry();
    const ModelSelection root{"fixture/root", {ReasoningMode::ProviderDefault, ""}};
    expect_error(ConfigurationErrorCode::InvalidRootDefaults, [&] {
        validate_root_defaults({"fixture/root", {}}, capabilities);
    });
    expect_error(ConfigurationErrorCode::InvalidRootDefaults, [&] {
        validate_root_defaults({"", {ReasoningMode::Disabled, ""}}, capabilities);
    });
    expect_error(ConfigurationErrorCode::InvalidOverride, [&] {
        (void)resolve_model(root, {"", {ReasoningMode::Inherit, "high"}}, capabilities);
    });
    expect_error(ConfigurationErrorCode::InvalidOverride, [&] {
        (void)resolve_model(root, {"", {ReasoningMode::Disabled, "high"}}, capabilities);
    });
    expect_error(ConfigurationErrorCode::InvalidOverride, [&] {
        (void)resolve_model(root, {"", {ReasoningMode::ProviderDefault, "high"}}, capabilities);
    });
    expect_error(ConfigurationErrorCode::InvalidOverride, [&] {
        (void)resolve_model(root, {"", {ReasoningMode::Explicit, ""}}, capabilities);
    });
    expect_error(ConfigurationErrorCode::InvalidOverride, [&] {
        (void)resolve_model(root, {"", {static_cast<ReasoningMode>(99), ""}}, capabilities);
    });
    expect_error(ConfigurationErrorCode::InvalidModelCapabilities, [&] {
        capabilities.add("", {});
    });
    expect_error(ConfigurationErrorCode::InvalidModelCapabilities, [&] {
        capabilities.add("fixture/invalid", {{""}, true, true});
    });
    expect_error(ConfigurationErrorCode::DuplicateModel, [&] {
        capabilities.add("fixture/root", {});
    });
}

void permissions_are_three_way_intersection_and_worker_only() {
    const ToolSet role{"read", "write", "role-only", ""};
    const ToolSet system{"read", "write", "system-only", ""};
    const ToolSet request{"read", "request-only", ""};
    CHECK(effective_tool_grants(Role::Worker, role, system, request) == ToolSet{"read"});
    CHECK(tool_allowed(Role::Worker, "read", role, system, request));
    CHECK(!tool_allowed(Role::Worker, "write", role, system, request));
    CHECK(!tool_allowed(Role::Worker, "role-only", role, system, request));
    CHECK(!tool_allowed(Role::Worker, "system-only", role, system, request));
    CHECK(!tool_allowed(Role::Worker, "request-only", role, system, request));
    CHECK(!tool_allowed(Role::Worker, "", role, system, request));
    CHECK(effective_tool_grants(Role::Worker, {}, system, request).empty());
    CHECK(effective_tool_grants(Role::Worker, role, {}, request).empty());
    CHECK(effective_tool_grants(Role::Worker, role, system, {}).empty());
    for (const auto manager : {Role::Root, Role::Dispatcher, Role::TeamLead,
                                static_cast<Role>(99)}) {
        CHECK(effective_tool_grants(manager, role, system, request).empty());
        CHECK(!tool_allowed(manager, "read", role, system, request));
    }
}

} // namespace

int main() {
    return test::run({
        {"model and reasoning resolve independently from frozen Root", direct_frozen_root_inheritance},
        {"inherit disabled provider-default and explicit remain distinct", reasoning_modes_remain_distinct},
        {"unknown models and unsupported reasoning reject", registry_rejects_unknown_and_unsupported},
        {"invalid configurations report explicit errors", invalid_configuration_is_explicit},
        {"tool grants require Worker and all three allowlists", permissions_are_three_way_intersection_and_worker_only}
    });
}
