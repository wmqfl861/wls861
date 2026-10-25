#include "wls/schema.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
using namespace wls::s1;
std::filesystem::path schemas;
std::unique_ptr<SchemaRegistry> registry;

Json read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(static_cast<bool>(stream));
    return parse_json_strict(std::string(std::istreambuf_iterator<char>(stream),
                                        std::istreambuf_iterator<char>()));
}

Json example(const std::string& name) { return read(schemas / "examples" / (name + ".json")); }

void write(const std::filesystem::path& path, const Json& value) {
    std::ofstream stream(path, std::ios::binary);
    stream << canonical_json(value);
    CHECK(static_cast<bool>(stream));
}

void reject(const std::function<void()>& action) { test::throws<Error>(action); }

void remove_execution_fields(boost::json::object& object) {
    for (const auto* name : {"request_id", "task_id", "team_id", "requirements_revision",
             "plan_revision", "task_revision", "instruction_revision", "dispatcher_epoch",
             "execution_epoch", "policy_epoch"}) object.erase(name);
}

Json model_submission() {
    auto submission = example("submission");
    remove_execution_fields(submission.as_object());
    submission.as_object().erase("submission_id");
    for (auto& memory : submission.as_object().at("used_memories").as_array()) {
        memory.as_object().erase("revision");
    }
    return submission;
}

Json basic_schema(const std::string& name, Json properties = boost::json::object{}) {
    return boost::json::object{
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"$id", "https://schemas.invalid/" + name + ".schema.json"},
        {"type", "object"}, {"properties", std::move(properties)}};
}

void full_examples_and_registry() {
    CHECK(registry->schema_names().size() == 18);
    const std::vector<std::pair<std::string, std::string>> documents{
        {"change_order.dispatcher", "change_order"}, {"change_order.team_lead", "change_order"},
        {"delivery_manifest", "delivery_manifest"}, {"experience_report", "experience_report"},
        {"memory_frontmatter", "memory_frontmatter"}, {"plan", "plan"}, {"question", "question"},
        {"request_texts", "request_snapshot"}, {"review_request", "review_request"},
        {"review_verdict", "review_verdict"}, {"submission", "submission"},
        {"task_brief", "task_brief"}, {"work_order", "work_order"}};
    for (const auto& [file, schema] : documents) registry->validate(schema, example(file));
    reject([] { registry->validate("unknown", boost::json::object{}); });
}

void complete_record_requires_trusted_tuple() {
    auto submission = example("submission");
    submission.as_object().erase("instruction_revision");
    reject([&] { registry->validate("submission", submission); });
    submission = example("submission");
    submission.as_object()["requirements_revision"] = "2";
    reject([&] { registry->validate("submission", submission); });
    submission = example("submission");
    submission.as_object()["unexpected"] = true;
    reject([&] { registry->validate("submission", submission); });
    submission = example("submission");
    submission.as_object().at("used_memories").as_array()[0].as_object().erase("revision");
    reject([&] { registry->validate("submission", submission); });
}

void model_input_cannot_forge_host_or_approval() {
    const auto submission = model_submission();
    registry->validate("submission", submission, true);
    auto forged = submission;
    forged.as_object()["requirements_revision"] = 999;
    reject([&] { registry->validate("submission", forged, true); });
    forged = submission;
    forged.as_object()["team_lead_check"] = boost::json::object{{"passed", true}};
    reject([&] { registry->validate("submission", forged, true); });
    forged = submission;
    forged.as_object().at("used_memories").as_array()[0].as_object()["revision"] = std::string(40, 'a');
    reject([&] { registry->validate("submission", forged, true); });
    const auto projected = registry->project_model_input("submission");
    CHECK(!projected.as_object().at("properties").as_object().contains("requirements_revision"));
    CHECK(registry->full_schema("submission").as_object().at("properties").as_object().contains("requirements_revision"));
}

void runtime_conditions_are_projected_but_business_conditions_remain() {
    auto change = example("change_order.team_lead");
    change.as_object().erase("parent_change_id");
    reject([&] { registry->validate("change_order", change); });
    for (const auto* name : {"change_id", "request_id", "task_id", "from_role",
            "based_on_requirements_revision", "target_task_revision", "target_plan_revision",
            "dispatcher_epoch", "target_instruction_revision", "execution_epoch"}) {
        change.as_object().erase(name);
    }
    registry->validate("change_order", change, true);
    auto& changes = change.as_object()["acceptance_changes"];
    changes = boost::json::array{boost::json::object{{"op", "modify"}, {"text", "changed"}}};
    reject([&] { registry->validate("change_order", change, true); });
    changes.as_array()[0].as_object()["criterion_id"] = "ac1";
    registry->validate("change_order", change, true);
}

void projection_keeps_same_named_fields_in_separate_instance_scopes() {
    test::TempDir directory;
    const Json business = parse_json_strict(R"({
        "type":"object",
        "properties":{
            "host_epoch":{"type":"integer"},
            "proof":{"type":"string"},
            "inner_token":{"type":"string","x-runtime-field":true}
        },
        "required":["host_epoch","inner_token"],
        "allOf":[{"if":{"required":["host_epoch"]},"then":{"required":["proof"]}}]
    })");
    const Json literal = parse_json_strict(R"({
        "id":"business literal", "$ref":"https://unregistered.invalid/literal",
        "properties":{"literal":{"x-runtime-field":true}}, "required":["literal"]
    })");
    auto schema = basic_schema("scopes", boost::json::object{
        {"host_epoch", boost::json::object{{"type", "integer"}, {"x-runtime-field", true}}},
        {"inner_token", boost::json::object{{"type", "string"}}},
        {"business", business},
        {"entries", boost::json::object{{"type", "array"}, {"items", business}}},
        {"literal", boost::json::object{{"const", literal}}}});
    schema.as_object()["required"] = boost::json::array{"host_epoch", "inner_token", "business", "entries", "literal"};
    schema.as_object()["allOf"] = parse_json_strict(R"([
        {"required":["host_epoch"]},
        {"if":{"required":["host_epoch"]},"then":{"required":["host_only_detail"]}}
    ])");
    write(directory.path / "scopes.schema.json", schema);
    const SchemaRegistry local(directory.path);
    const Json good = boost::json::object{
        {"inner_token", "normal root business value"},
        {"business", boost::json::object{{"host_epoch", 7}, {"proof", "yes"}}},
        {"entries", boost::json::array{boost::json::object{{"host_epoch", 8}, {"proof", "yes"}}}},
        {"literal", literal}};
    local.validate("scopes", good, true);
    for (const auto* field : {"host_epoch", "proof"}) {
        auto bad = good;
        bad.as_object().at("business").as_object().erase(field);
        reject([&] { local.validate("scopes", bad, true); });
        bad = good;
        bad.as_object().at("entries").as_array()[0].as_object().erase(field);
        reject([&] { local.validate("scopes", bad, true); });
    }
    auto bad = good;
    bad.as_object().erase("inner_token");
    reject([&] { local.validate("scopes", bad, true); });
    bad = good;
    bad.as_object()["host_epoch"] = 99;
    reject([&] { local.validate("scopes", bad, true); });
    bad = good;
    bad.as_object().at("business").as_object()["inner_token"] = "forged";
    reject([&] { local.validate("scopes", bad, true); });
    CHECK(local.project_model_input("scopes").as_object().at("properties").as_object()
              .at("literal").as_object().at("const") == literal);
}

void nested_briefs_and_draft7_conditionals() {
    auto plan = example("plan");
    plan.as_object().erase("plan_revision");
    plan.as_object().erase("based_on_requirements_revision");
    registry->validate("plan", plan, true);
    auto broken = plan;
    broken.as_object().at("nodes").as_array()[0].as_object().at("brief").as_object().erase("constraint_refs");
    reject([&] { registry->validate("plan", broken, true); });
    broken = plan;
    broken.as_object().at("nodes").as_array()[0].as_object().erase("brief");
    reject([&] { registry->validate("plan", broken, true); });
    broken = plan;
    auto& node = broken.as_object().at("nodes").as_array()[0].as_object();
    node["kind"] = "capability_call";
    node.erase("brief");
    reject([&] { registry->validate("plan", broken, true); });
    node["capability"] = "review.review_code";
    node["input"] = boost::json::object{};
    registry->validate("plan", broken, true);
}

void findings_capacity_and_unique_items() {
    auto verdict = example("review_verdict");
    boost::json::array findings;
    for (int number = 1; number <= 100; ++number) {
        findings.emplace_back(boost::json::object{
            {"id", "f" + std::to_string(number)}, {"severity", "major"}, {"text", "a concrete finding"}});
    }
    verdict.as_object()["findings"] = findings;
    registry->validate("review_verdict", verdict);
    auto change = example("change_order.team_lead");
    change.as_object()["findings"] = findings;
    registry->validate("change_order", change);
    findings.emplace_back(boost::json::object{{"id", "f101"}, {"severity", "major"}, {"text", "overflow"}});
    verdict.as_object()["findings"] = findings;
    reject([&] { registry->validate("review_verdict", verdict); });
    auto brief = example("task_brief");
    brief.as_object()["constraint_refs"] = boost::json::array{"c1", "c1"};
    reject([&] { registry->validate("task_brief", brief); });
}

void closed_local_references_and_projection() {
    test::TempDir directory;
    auto child = basic_schema("child", boost::json::object{
        {"host_epoch", boost::json::object{{"type", "integer"}, {"x-runtime-field", true}}},
        {"text", boost::json::object{{"type", "string"}, {"minLength", 1}}}});
    child.as_object()["required"] = boost::json::array{"host_epoch", "text"};
    child.as_object()["additionalProperties"] = true;
    auto parent = basic_schema("parent", boost::json::object{
        {"result", boost::json::object{{"$ref", "child.schema.json"}}}});
    parent.as_object()["required"] = boost::json::array{"result"};
    write(directory.path / "child.schema.json", child);
    write(directory.path / "parent.schema.json", parent);
    const SchemaRegistry local(directory.path);
    Json complete = boost::json::object{{"result", boost::json::object{{"host_epoch", 1}, {"text", "ready"}}}};
    local.validate("parent", complete);
    reject([&] { local.validate("parent", complete, true); });
    complete.as_object().at("result").as_object().erase("host_epoch");
    local.validate("parent", complete, true);
    reject([&] { local.validate("parent", complete); });
    // additionalProperties=true in child must not permit forged host metadata.
    complete.as_object().at("result").as_object()["host_epoch"] = 99;
    reject([&] { local.validate("parent", complete, true); });
}

void local_pointer_and_boolean_schema() {
    test::TempDir directory;
    auto schema = basic_schema("pointer", boost::json::object{
        {"result", boost::json::object{{"$ref", "#/definitions/result"}}}, {"forbidden", false}});
    schema.as_object()["definitions"] = boost::json::object{
        {"result", boost::json::object{{"type", "string"}, {"pattern", "^ok-[0-9]+$"}}}};
    write(directory.path / "pointer.schema.json", schema);
    const SchemaRegistry local(directory.path);
    local.validate("pointer", boost::json::object{{"result", "ok-12"}});
    reject([&] { local.validate("pointer", boost::json::object{{"result", "bad"}}); });
    reject([&] { local.validate("pointer", boost::json::object{{"result", "ok-12"}, {"forbidden", true}}); });
}

void all_references_checked_before_validation() {
    test::TempDir directory;
    auto schema = basic_schema("test");
    schema.as_object()["if"] = boost::json::object{{"properties", boost::json::object{
        {"unused", boost::json::object{{"const", true}}}}}};
    schema.as_object()["then"] = boost::json::object{{"$ref", "https://outside.invalid/private"}};
    write(directory.path / "test.schema.json", schema);
    reject([&] { (void)SchemaRegistry(directory.path); });
    schema.as_object()["then"] = boost::json::object{{"$ref", "file:///etc/passwd"}};
    write(directory.path / "test.schema.json", schema);
    reject([&] { (void)SchemaRegistry(directory.path); });
    schema.as_object()["then"] = boost::json::object{{"$ref", "../other.schema.json"}};
    write(directory.path / "test.schema.json", schema);
    reject([&] { (void)SchemaRegistry(directory.path); });
}

void malformed_ids_documents_and_missing_pointer_reject() {
    test::TempDir directory;
    auto schema = basic_schema("first");
    write(directory.path / "first.schema.json", schema);
    write(directory.path / "second.schema.json", schema);
    reject([&] { (void)SchemaRegistry(directory.path); });
    std::filesystem::remove(directory.path / "second.schema.json");
    schema.as_object()["$ref"] = "#/missing";
    write(directory.path / "first.schema.json", schema);
    reject([&] { (void)SchemaRegistry(directory.path); });
    std::ofstream stream(directory.path / "first.schema.json", std::ios::binary);
    stream << R"({"$schema":"http://json-schema.org/draft-07/schema#","$id":"https://schemas.invalid/first","type":"object","type":"string"})";
    stream.close();
    reject([&] { (void)SchemaRegistry(directory.path); });
}

void schema_paths_and_declared_draft_are_checked() {
    test::TempDir directory;
    const auto original = directory.path / "original.json";
    write(original, basic_schema("test"));
    std::filesystem::create_symlink(original, directory.path / "test.schema.json");
    reject([&] { (void)SchemaRegistry(directory.path); });
    std::filesystem::remove(directory.path / "test.schema.json");
    auto old = basic_schema("test");
    old.as_object()["$schema"] = "http://json-schema.org/draft-04/schema#";
    write(directory.path / "test.schema.json", old);
    reject([&] { (void)SchemaRegistry(directory.path); });
}

} // namespace

int main(int argc, char** argv) {
    schemas = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(WLS_SOURCE_DIR) / "schemas";
    try { registry = std::make_unique<SchemaRegistry>(schemas); }
    catch (const std::exception& error) { std::cerr << "FAIL registry setup: " << error.what() << '\n'; return 1; }
    return test::run({
        {"all 13 complete examples validate with the 17-schema closed registry", full_examples_and_registry},
        {"complete records enforce binding, usage and strong types", complete_record_requires_trusted_tuple},
        {"model input cannot forge host metadata or self-approval", model_input_cannot_forge_host_or_approval},
        {"projection removes host conditions and preserves business conditions", runtime_conditions_are_projected_but_business_conditions_remain},
        {"projection scopes host names without weakening nested business fields", projection_keeps_same_named_fields_in_separate_instance_scopes},
        {"plan team briefs and draft-07 conditional requirements validate", nested_briefs_and_draft7_conditionals},
        {"100 findings remain legal and unique constraints are enforced", findings_capacity_and_unique_items},
        {"registered relative references preserve model projection and deny host fields", closed_local_references_and_projection},
        {"local JSON Pointers, boolean schemas and patterns are enforced", local_pointer_and_boolean_schema},
        {"unvisited remote, file and traversal refs fail before validation", all_references_checked_before_validation},
        {"duplicate IDs, duplicate JSON keys and missing pointers reject", malformed_ids_documents_and_missing_pointer_reject},
        {"schema symlinks and unsupported declared drafts reject", schema_paths_and_declared_draft_are_checked}
    });
}
