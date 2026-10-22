#include "wls/config_loader.hpp"
#include "test_support.hpp"

#include <fstream>
#include <iterator>
#include <string_view>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

using namespace wls;
using namespace wls::s1;

namespace {

using Object = boost::json::object;
using Array = boost::json::array;

std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("test fixture read failed");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("test fixture write failed");
    file << bytes;
    if (!file) throw std::runtime_error("test fixture write failed");
}

void replace_once(std::string& text, std::string_view needle, std::string_view replacement) {
    const auto position = text.find(needle);
    CHECK(position != std::string::npos);
    text.replace(position, needle.size(), replacement);
}

template<class Function>
void expect_error(ErrorCode code, Function&& function) {
    bool caught = false;
    try { function(); }
    catch (const Error& error) { CHECK(error.code() == code); caught = true; }
    CHECK(caught);
}

class Fixture {
public:
    Fixture() {
        const auto source = std::filesystem::path(WLS_SOURCE_DIR);
        std::filesystem::copy(source / "config/examples", config, std::filesystem::copy_options::recursive);
        std::filesystem::copy(source / "prompts", prompts, std::filesystem::copy_options::recursive);
    }
    ConfigCatalog load(const ConfigLoadOptions& options = {}) const {
        return load_config_catalog(system_path(), functions, prompts, options);
    }
    FrozenRequestConfig freeze() const { return load().freeze("general"); }
    std::filesystem::path system_path() const { return config / "system.yaml"; }
    std::filesystem::path function_path() const { return general / "function.yaml"; }
    Json system_json() const { return freeze().json().at("system"); }
    Json function_json() const { return freeze().json().at("function"); }
    void system(const Json& value) const { write(system_path(), canonical_json(value)); }
    void function(const Json& value) const { write(function_path(), canonical_json(value)); }

    test::TempDir directory;
    std::filesystem::path config{directory.path / "config"};
    std::filesystem::path functions{config / "functions"};
    std::filesystem::path general{functions / "general"};
    std::filesystem::path prompts{directory.path / "prompts"};
};

Json& field(Json& value, std::string_view name) { return value.as_object().at(name); }

void sample_loads_with_provenance_and_no_environment_expansion() {
    Fixture fixture;
    const auto catalog = fixture.load();
    CHECK(catalog.functions() == std::vector<std::string>{"general"});
    const auto frozen = catalog.freeze("general");
    CHECK(frozen.function_id() == "general");
    CHECK(frozen.capability_id() == "run");
    CHECK(frozen.sha256().size() == 64);
    CHECK(frozen.sha256() == sha256_hex(frozen.serialize()));
    CHECK(frozen.dependencies().size() == 10);
    CHECK(frozen.resolve(Role::Worker).model == "fake");
    CHECK(frozen.resolve(Role::Worker).reasoning.mode == ReasoningMode::ProviderDefault);
    CHECK(frozen.resolve(Role::Worker) == frozen.resolve(Role::Root));
    CHECK(frozen.worker_tools().contains("fs_read"));
    const auto explanation = frozen.explain(Role::Worker);
    CHECK(explanation.at("resolution").at("model_source").as_string() == "system.defaults.model_ref");
    CHECK(explanation.at("resolution").at("inheritance").as_string() == "direct_frozen_root");
    CHECK(!explanation.at("environment_checks_executed").as_bool());
    CHECK(frozen.model_profile(Role::Worker).at("protocol").as_string() == "fake-v1");
    CHECK(frozen.model_profile(Role::Worker).at("adapter_version").as_string() == "1");
    CHECK(frozen.json().at("system").at("paths").at("socket").as_string() == "${XDG_RUNTIME_DIR}/wls.sock");
    CHECK(frozen.json().at("system").at("budget_defaults").at("max_cost_usd").is_null());
}

void roles_resolve_independently_and_explain_each_field() {
    Fixture fixture;
    auto system = fixture.system_json();
    auto& root_profile = field(field(system, "models"), "fake");
    field(root_profile, "capabilities").as_object()["reasoning_modes"] = Array{"provider_default", "off", "effort"};
    field(root_profile, "capabilities").as_object()["reasoning_efforts"] = Array{"low", "high", "max"};
    field(system, "models").as_object()["alternate"] = root_profile;
    field(system, "defaults") = Object{{"model_ref", "fake"}, {"reasoning", Object{{"mode", "effort"}, {"effort", "high"}}}};
    auto function = fixture.function_json();
    field(function, "root") = Object{{"model_ref", "alternate"}, {"reasoning", Object{{"mode", "off"}}}};
    auto& roles = field(function, "roles");
    field(roles, "dispatcher").as_object()["model_ref"] = "fake";
    field(roles, "dispatcher").as_object()["reasoning"] = Object{{"mode", "effort"}, {"effort", "max"}};
    field(roles, "team_lead").as_object()["reasoning"] = Object{{"mode", "provider_default"}};
    fixture.system(system);
    fixture.function(function);
    const auto frozen = fixture.freeze();
    CHECK((frozen.resolve(Role::Root) == ModelSelection{"alternate", {ReasoningMode::Disabled, ""}}));
    CHECK((frozen.resolve(Role::Dispatcher) == ModelSelection{"fake", {ReasoningMode::Explicit, "max"}}));
    CHECK((frozen.resolve(Role::TeamLead) == ModelSelection{"alternate", {ReasoningMode::ProviderDefault, ""}}));
    CHECK(frozen.resolve(Role::Worker) == frozen.resolve(Role::Root));
    const auto explanation = frozen.explain(Role::Worker);
    CHECK(explanation.at("resolution").at("model_source").as_string() == "function.root.model_ref");
    CHECK(explanation.at("resolution").at("reasoning_source").as_string() == "function.root.reasoning");

    field(roles, "worker").as_object()["model_ref"] = "fake";
    fixture.function(function);
    const auto model_only = fixture.freeze();
    CHECK((model_only.resolve(Role::Worker) == ModelSelection{"fake", {ReasoningMode::Disabled, ""}}));
    CHECK(model_only.explain(Role::Worker).at("resolution").at("reasoning_source").as_string() == "function.root.reasoning");
}

void loaded_catalog_and_restoration_ignore_later_disk_changes() {
    Fixture fixture;
    const auto old_catalog = fixture.load();
    const auto old = old_catalog.freeze("general");
    auto system = old.json().at("system");
    field(system, "models").as_object()["alternate"] = field(field(system, "models"), "fake");
    field(field(system, "defaults"), "model_ref") = "alternate";
    fixture.system(system);
    write(fixture.prompts / "worker.md", read(fixture.prompts / "worker.md") + "\nNew global role instruction.\n");
    write(fixture.general / "prompts/worker.md", read(fixture.general / "prompts/worker.md") + "\nNew function instruction.\n");
    write(fixture.general / "env.yaml", read(fixture.general / "env.yaml") + "  - New environment note.\n");
    const auto current = fixture.freeze();
    CHECK(current.sha256() != old.sha256());
    CHECK(current.resolve(Role::Worker).model == "alternate");
    CHECK(old_catalog.freeze("general").serialize() == old.serialize());
    CHECK(old.resolve(Role::Worker).model == "fake");
    std::filesystem::remove_all(fixture.config);
    std::filesystem::remove_all(fixture.prompts);
    const auto restored = FrozenRequestConfig::restore(old.serialize(), old.sha256());
    CHECK(restored.serialize() == old.serialize());
    CHECK(restored.resolve(Role::Worker) == old.resolve(Role::Worker));
    CHECK(restored.dependencies().size() == old.dependencies().size());
}

void same_configuration_in_another_checkout_has_same_hash() {
    Fixture first;
    Fixture second;
    const auto one = first.freeze();
    const auto two = second.freeze();
    CHECK(first.directory.path != second.directory.path);
    CHECK(one.sha256() == two.sha256());
    CHECK(one.serialize().find(first.directory.path.string()) == std::string::npos);
    CHECK(two.serialize().find(second.directory.path.string()) == std::string::npos);
}

void snapshot_integrity_and_derived_resolutions_are_verified() {
    Fixture fixture;
    const auto frozen = fixture.freeze();
    expect_error(ErrorCode::Corrupt, [&] { (void)FrozenRequestConfig::restore(frozen.serialize() + " ", frozen.sha256()); });
    const auto spaced = frozen.serialize() + " ";
    expect_error(ErrorCode::Corrupt, [&] { (void)FrozenRequestConfig::restore(spaced, sha256_hex(spaced)); });

    auto forged = frozen.json();
    field(field(field(forged, "role_resolutions"), "worker"), "model_ref") = "alternate";
    auto bytes = canonical_json(forged);
    expect_error(ErrorCode::InvalidInput, [&] { (void)FrozenRequestConfig::restore(bytes, sha256_hex(bytes)); });

    forged = frozen.json();
    field(forged, "dependencies").as_array().front().as_object()["content"] = "altered dependency";
    bytes = canonical_json(forged);
    expect_error(ErrorCode::InvalidInput, [&] { (void)FrozenRequestConfig::restore(bytes, sha256_hex(bytes)); });

    forged = frozen.json();
    field(forged, "dependencies").as_array().pop_back();
    bytes = canonical_json(forged);
    expect_error(ErrorCode::InvalidInput, [&] { (void)FrozenRequestConfig::restore(bytes, sha256_hex(bytes)); });

    forged = frozen.json();
    field(field(field(forged, "system"), "limits"), "max_active_workers") = 9;
    bytes = canonical_json(forged);
    expect_error(ErrorCode::InvalidInput, [&] { (void)FrozenRequestConfig::restore(bytes, sha256_hex(bytes)); });
}

void yaml_rejects_ambiguous_or_compound_unauthorized_structures() {
    for (const auto change : {0, 1, 2, 3, 4, 5, 6}) {
        Fixture fixture;
        auto bytes = read(fixture.system_path());
        switch (change) {
        case 0: bytes += "\nschema_version: 1\n"; break;
        case 1: replace_once(bytes, "models:\n", "models: &models\n"); break;
        case 2: bytes += "\n---\nschema_version: 1\n"; break;
        case 3: bytes += "\n123: forbidden\n"; break;
        case 4: bytes += "\nunknown_option: true\n"; break;
        case 5: replace_once(bytes, "[provider_default, \"off\"]", "[provider_default, off]"); break;
        case 6: replace_once(bytes, "schema_version: 1", "schema_version: !!int 1"); break;
        default: CHECK(false);
        }
        write(fixture.system_path(), bytes);
        expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    }
}

void scalar_types_and_reasoning_capabilities_fail_explicitly() {
    Fixture fixture;
    const auto original = fixture.system_json();
    auto system = original;
    field(field(system, "model_calls"), "real_calls_enabled") = "false";
    fixture.system(system);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    system = original;
    field(system, "schema_version") = "1";
    fixture.system(system);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    system = original;
    field(field(system, "defaults"), "model_ref") = "unknown_model";
    fixture.system(system);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    system = original;
    field(field(system, "defaults"), "reasoning") = Object{{"mode", "effort"}, {"effort", "max"}};
    fixture.system(system);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    system = original;
    field(field(system, "defaults"), "reasoning") = nullptr;
    fixture.system(system);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    system = original;
    field(field(system, "defaults"), "reasoning") = Object{{"mode", "off"}, {"effort", "high"}};
    fixture.system(system);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    system = original;
    field(field(system, "models"), "fake").as_object().erase("adapter_version");
    fixture.system(system);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    system = original;
    field(field(system, "models"), "fake").as_object().erase("protocol");
    fixture.system(system);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
}

void control_amendment_and_review_reservations_cannot_be_disabled() {
    Fixture fixture;
    const auto original = fixture.system_json();
    for (const auto change : {0, 1, 2, 3, 4, 5, 6, 7}) {
        auto system = original;
        switch (change) {
        case 0: field(field(system, "limits"), "reserve_dispatchers_for_review") = 0; break;
        case 1: field(field(system, "limits"), "reserve_workers_for_review") = 8; break;
        case 2: field(field(system, "request_config"), "freeze_at_request_creation") = false; break;
        case 3: field(field(system, "control"), "diagnostic_tools") = Array{"run"}; break;
        case 4: field(field(system, "control"), "ordinary_budget_blocks_control") = true; break;
        case 5: field(field(system, "amendment"), "unhandled_task_policy") = "resume"; break;
        case 6: field(field(system, "amendment"), "dispositions") = Array{"update", "cancel"}; break;
        case 7: field(field(system, "limits"), "manager_max_tool_calls_per_wake") = 13; break;
        default: CHECK(false);
        }
        fixture.system(system);
        expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    }
}

void unsupported_runtime_features_never_silently_enable() {
    Fixture fixture;
    const auto original = fixture.system_json();
    for (const auto change : {0, 1, 2, 4, 5, 6}) {
        auto system = original;
        switch (change) {
        case 0: field(field(system, "model_calls"), "real_calls_enabled") = true; break;
        case 1: field(field(system, "model_calls"), "max_paid_retries") = 1; break;
        case 2: field(field(system, "memory"), "enabled") = true; break;
        case 3: field(field(system, "cleanup"), "enabled") = true; break;
        case 4: field(field(field(system, "models"), "fake"), "adapter") = "https"; break;
        case 5: field(field(system, "sandbox"), "extra_ro_binds") = Array{"/"}; break;
        case 6: field(field(field(field(system, "models"), "fake"), "capabilities"), "streaming") = "supported"; break;
        default: CHECK(false);
        }
        fixture.system(system);
        expect_error(ErrorCode::Unsupported, [&] { (void)fixture.load(); });
    }
}

void cleanup_opt_in_is_frozen_but_not_an_execution_authorization() {
    Fixture fixture;
    auto system = fixture.system_json();
    field(field(system, "cleanup"), "enabled") = true;
    fixture.system(system);
    const auto frozen = fixture.load().freeze("general");
    CHECK(frozen.json().at("system").at("cleanup").at("enabled") == true);
    const auto restored = FrozenRequestConfig::restore(frozen.serialize(), frozen.sha256());
    CHECK(restored.json().at("system").at("cleanup").at("enabled") == true);
}

void budgets_durability_and_isolation_values_are_checked() {
    Fixture fixture;
    const auto original = fixture.system_json();
    for (const auto change : {0, 1, 2, 3, 4}) {
        auto system = original;
        switch (change) {
        case 0: field(field(system, "budget_defaults"), "max_input_tokens") = -1; break;
        case 1: field(field(system, "budget_defaults"), "max_input_tokens") = 3.5; break;
        case 2: field(field(system, "budget_defaults"), "max_cost_usd") = 0; break;
        case 3: field(field(system, "storage"), "synchronous") = "NORMAL"; break;
        case 4: field(field(system, "sandbox"), "run_default_timeout_s") = 601; break;
        default: CHECK(false);
        }
        fixture.system(system);
        expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    }
    fixture.system(original);
    auto function = fixture.function_json();
    field(field(field(function, "team_templates"), "default"), "isolation") = "weak";
    fixture.function(function);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    auto allowed = original;
    field(field(allowed, "sandbox"), "allow_weak_isolation") = true;
    fixture.system(allowed);
    const auto frozen = fixture.freeze();
    CHECK(frozen.json().at("function").at("team_templates").at("default").at("isolation").as_string() == "weak");
    CHECK(!frozen.explain(Role::Worker).at("environment_checks_executed").as_bool());
}

void template_paths_and_symbolic_links_cannot_escape_config_roots() {
    for (const auto& path : {"../outside.md", "/tmp/outside.md", "prompts/../worker.md", "prompts\\worker.md"}) {
        Fixture fixture;
        auto function = fixture.function_json();
        field(field(field(function, "roles"), "worker"), "prompt_append") = path;
        fixture.function(function);
        expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    }
    {
        Fixture fixture;
        const auto target = fixture.directory.path / "outside.md";
        write(target, "test-only outside content");
        std::filesystem::remove(fixture.general / "prompts/worker.md");
        std::filesystem::create_symlink(target, fixture.general / "prompts/worker.md");
        expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    }
    {
        Fixture fixture;
        const auto target = fixture.directory.path / "relocated-prompts";
        std::filesystem::rename(fixture.general / "prompts", target);
        std::filesystem::create_directory_symlink(target, fixture.general / "prompts");
        expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    }
    {
        Fixture fixture;
        const auto target = fixture.directory.path / "relocated-general";
        std::filesystem::rename(fixture.general, target);
        std::filesystem::create_directory_symlink(target, fixture.general);
        expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    }
    {
        Fixture fixture;
        const auto link = fixture.directory.path / "alias";
        std::filesystem::create_directory_symlink(fixture.config, link);
        expect_error(ErrorCode::InvalidInput, [&] {
            (void)load_config_catalog(link / "../config/system.yaml", fixture.functions, fixture.prompts);
        });
    }
}

void parser_and_snapshot_capacity_are_bounded() {
    Fixture fixture;
    ConfigLoadOptions options;
    options.max_file_bytes = 32;
    options.max_total_bytes = 64;
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(options); });
    options = {};
    options.max_yaml_depth = 1;
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(options); });
    options = {};
    options.max_yaml_nodes = 8;
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(options); });
    const auto frozen = fixture.freeze();
    options = {};
    options.max_file_bytes = 1024;
    options.max_total_bytes = 2048;
    expect_error(ErrorCode::InvalidInput, [&] { (void)FrozenRequestConfig::restore(frozen.serialize(), frozen.sha256(), options); });
    const auto original = read(fixture.system_path());
    auto long_number = original;
    replace_once(long_number, "schema_version: 1", "schema_version: " + std::string(100000, '9'));
    write(fixture.system_path(), long_number);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
    write(fixture.system_path(), original);
    for (int index = 0; index < 64; ++index) std::filesystem::create_directory(fixture.functions / ("extra" + std::to_string(index)));
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
}

void template_dispatch_requires_registered_deterministic_acceptance() {
    Fixture fixture;
    auto function = fixture.function_json();
    field(function, "capabilities") = Array{Object{{"id", "review"}, {"dispatch", "template"}, {"default", true},
        {"input_schema", "review_request"}, {"output_schema", "review_verdict"},
        {"acceptance_validator", "current_review_evidence_v1"}, {"required_checks_source", "review_request.criteria"},
        {"fail_on_required_not_run", true}, {"team_template", "default"}, {"brief_template", "briefs/review.md"}}};
    write(fixture.general / "briefs/review.md", "Review only the current frozen candidate and required evidence.\n");
    fixture.function(function);
    expect_error(ErrorCode::Unsupported, [&] { (void)fixture.load(); });
    ConfigLoadOptions options;
    options.acceptance_validators.insert("current_review_evidence_v1");
    const auto frozen = fixture.load(options).freeze("general", "review");
    CHECK(frozen.capability_id() == "review");
    CHECK(frozen.dependencies().size() == 11);
    const auto restored = FrozenRequestConfig::restore(frozen.serialize(), frozen.sha256(), options);
    CHECK(restored.sha256() == frozen.sha256());
    expect_error(ErrorCode::Unsupported, [&] { (void)FrozenRequestConfig::restore(frozen.serialize(), frozen.sha256()); });
    field(function, "capabilities").as_array().front().as_object()["fail_on_required_not_run"] = false;
    fixture.function(function);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(options); });
}

void environment_checks_are_frozen_as_data_and_never_executed() {
    Fixture fixture;
    const auto sentinel = fixture.directory.path / "should-not-be-created";
    const Json environment = Object{{"schema_version", 1}, {"tools", Array{Object{{"name", "fixture"},
        {"check", "touch " + sentinel.string()}, {"usage", "read-only configuration loading must not run this"}}}}, {"notes", "fixture"}};
    write(fixture.general / "env.yaml", canonical_json(environment));
    const auto frozen = fixture.freeze();
    CHECK(!std::filesystem::exists(sentinel));
    bool found_environment = false;
    for (const auto& dependency : frozen.dependencies()) {
        if (dependency.kind == "environment_template") {
            found_environment = true;
            CHECK(dependency.content == canonical_json(environment));
        }
    }
    CHECK(found_environment);
    (void)FrozenRequestConfig::restore(frozen.serialize(), frozen.sha256());
    CHECK(!std::filesystem::exists(sentinel));
}

void unknown_functions_capabilities_and_tools_are_rejected() {
    Fixture fixture;
    const auto catalog = fixture.load();
    expect_error(ErrorCode::InvalidInput, [&] { (void)catalog.freeze("missing"); });
    expect_error(ErrorCode::InvalidInput, [&] { (void)catalog.freeze("general", "missing"); });
    expect_error(ErrorCode::InvalidInput, [&] { (void)catalog.freeze("general").resolve(static_cast<Role>(99)); });
    expect_error(ErrorCode::InvalidInput, [&] { (void)catalog.freeze("general").worker_tools("missing"); });
    auto function = fixture.function_json();
    field(field(field(function, "team_templates"), "default"), "worker_tools").as_array().emplace_back("unregistered_tool");
    fixture.function(function);
    expect_error(ErrorCode::InvalidInput, [&] { (void)fixture.load(); });
}

} // namespace

int main() {
    return test::run({
        {"configuration sample loads with field provenance and literal environment paths", sample_loads_with_provenance_and_no_environment_expansion},
        {"role fields resolve independently from frozen Root", roles_resolve_independently_and_explain_each_field},
        {"loaded configuration and restored requests ignore later disk changes", loaded_catalog_and_restoration_ignore_later_disk_changes},
        {"snapshot fingerprints are independent of checkout location", same_configuration_in_another_checkout_has_same_hash},
        {"snapshot integrity checks dependencies and recomputed role bindings", snapshot_integrity_and_derived_resolutions_are_verified},
        {"strict YAML rejects ambiguous strings duplicate keys anchors and extra documents", yaml_rejects_ambiguous_or_compound_unauthorized_structures},
        {"scalar types unknown models and unsupported reasoning reject", scalar_types_and_reasoning_capabilities_fail_explicitly},
        {"control amendment and review capacity policies remain mandatory", control_amendment_and_review_reservations_cannot_be_disabled},
        {"cleanup opt-in is frozen and requires separate execution guards", cleanup_opt_in_is_frozen_but_not_an_execution_authorization},
        {"unimplemented real calls memory and paid retries reject", unsupported_runtime_features_never_silently_enable},
        {"budget durability and explicit weak isolation policies validate", budgets_durability_and_isolation_values_are_checked},
        {"template references reject traversal and file or directory symlinks", template_paths_and_symbolic_links_cannot_escape_config_roots},
        {"configuration parsing and frozen payload have explicit capacity bounds", parser_and_snapshot_capacity_are_bounded},
        {"template dispatch requires a host registered deterministic validator", template_dispatch_requires_registered_deterministic_acceptance},
        {"environment check commands are data and never execute on load or restore", environment_checks_are_frozen_as_data_and_never_executed},
        {"unknown functions capabilities roles and tools reject", unknown_functions_capabilities_and_tools_are_rejected}
    });
}
