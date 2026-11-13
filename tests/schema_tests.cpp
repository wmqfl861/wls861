#include "wls/schema.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/inotify.h>

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

std::string rejection_message(const std::function<void()>& action) {
    try { action(); }
    catch (const Error& error) { CHECK(error.code() == ErrorCode::InvalidInput); return error.what(); }
    throw std::runtime_error("expected invalid input");
}

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
    CHECK(registry->schema_names().size() == 21);
    const std::vector<std::pair<std::string, std::string>> documents{
        {"change_order.dispatcher", "change_order"}, {"change_order.team_lead", "change_order"},
        {"delivery_manifest", "delivery_manifest"}, {"experience_report", "experience_report"},
        {"memory_frontmatter", "memory_frontmatter"}, {"plan", "plan"}, {"question", "question"},
        {"request_texts", "request_snapshot"}, {"review_request", "review_request"},
        {"review_verdict", "review_verdict"}, {"submission", "submission"},
        {"task_brief", "task_brief"}, {"work_order", "work_order"},
        {"change_task", "change_task"}, {"escalate_task", "escalate_task"}};
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

void v3_minimal_contracts_are_closed_and_legacy_schemas_unchanged() {
    const auto old_plan = registry->project_model_input("plan");
    const auto full_plan = registry->full_schema("plan");
    const auto projected = registry->project_v3_input(old_plan);
    const auto& node = projected.at("properties").at("nodes").at("items");
    CHECK(node.at("properties").as_object().size() == 4);
    CHECK(node.at("properties").at("brief").at("properties").as_object().size() == 6);
    for (const auto* name : {"plan", "work_order", "submit_amendment_plan", "check_submission"}) {
        const auto schema = registry->project_v3_input(registry->project_model_input(name));
        const auto bytes = canonical_json(schema);
        CHECK(bytes.find("\"$ref\"") == std::string::npos);
        CHECK(bytes.find("\"$id\"") == std::string::npos);
        CHECK(bytes.find("\"$schema\"") == std::string::npos);
        CHECK(bytes.find("\"x-runtime-field\"") == std::string::npos);
        CHECK(bytes.find("\"request_id\"") == std::string::npos && !schema.as_object().contains("title"));
    }
    const auto plan = parse_json_strict(R"({"summary":"One file","nodes":[{"node_id":"alpha","title":"File","brief":{"goal":"Write a file","acceptance_criteria":[{"text":"Contains the requested text"}]}}]})");
    registry->validate_v3_input("plan", plan);
    reject([&] { registry->validate("plan", plan, true); });
    auto invalid = plan;
    invalid.at("nodes").as_array()[0].at("brief").as_object().erase("goal");
    CHECK(rejection_message([&] { registry->validate_v3_input("plan", invalid); }).find("/nodes/0/brief/goal") != std::string::npos);
    invalid = plan;
    invalid.at("nodes").as_array()[0].at("brief").at("acceptance_criteria").as_array()[0] = "not an object";
    CHECK(rejection_message([&] { registry->validate_v3_input("plan", invalid); }).find("/acceptance_criteria/0") != std::string::npos);
    for (const auto* field : {"kind", "review", "capability", "input", "unexpected"}) {
        invalid = plan; invalid.at("nodes").as_array()[0].as_object()[field] = "forged";
        reject([&] { registry->validate_v3_input("plan", invalid); });
    }
    invalid = plan; invalid.at("nodes").as_array()[0].at("brief").as_object()["inputs"] = boost::json::array{};
    reject([&] { registry->validate_v3_input("plan", invalid); });
    const auto order = parse_json_strict(R"({"approach":"Write","steps":[{"text":"Write and check"}],"done_definition":"File exists"})");
    registry->validate_v3_input("work_order", order);
    registry->validate_v3_input("check_submission", boost::json::object{{"passed", false}, {"work_order", order}});
    reject([&] { registry->validate_v3_input("check_submission", boost::json::object{{"passed", true}, {"work_order", order}}); });
    reject([&] { registry->validate_v3_input("check_submission", boost::json::object{{"passed", false}}); });
    registry->validate_v3_input("submit_amendment_plan", boost::json::object{{"plan", plan},
        {"dispositions", boost::json::object{{"alpha", boost::json::object{{"disposition", "update"}, {"rationale", "Apply change"}}}}}});
    CHECK(registry->project_model_input("plan") == old_plan && registry->full_schema("plan") == full_plan);
    const Json literal = boost::json::object{{"const", boost::json::object{{"$ref", "literal-instance-data"}, {"title", "keep"}}}};
    CHECK(registry->project_v3_input(literal) == literal);
    boost::json::array repeated;
    for (int i = 0; i < 256; ++i) repeated.push_back(boost::json::object{{"$ref", full_plan.at("$id")}});
    reject([&] { (void)registry->project_v3_input(boost::json::object{{"allOf", repeated}}); });
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
    CHECK(rejection_message([&] { registry->validate("plan", broken, true); })
        .find("/nodes/0/brief/constraint_refs: required property is missing") != std::string::npos);
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
    CHECK(rejection_message([&] { registry->validate("task_brief", brief); })
        .find("/constraint_refs: array items must be unique") != std::string::npos);
    for (int index = 2; index < 100; ++index) brief.at("constraint_refs").as_array().emplace_back("c1");
    const auto limited = rejection_message([&] { registry->validate("task_brief", brief); });
    CHECK(limited.find("schema constraint failed (diagnostics_limited)") != std::string::npos);
    CHECK(limited.size() <= 600);
}

void diagnostics_identify_nested_brief_errors_without_values() {
    for (const bool model_input : {false, true}) {
        auto plan = example("plan");
        if (model_input) {
            plan.as_object().erase("plan_revision");
            plan.as_object().erase("based_on_requirements_revision");
        }
        auto& nodes = plan.as_object().at("nodes").as_array();
        const auto copy = nodes.front(); nodes.push_back(copy);
        auto& brief = nodes[1].as_object().at("brief").as_object();
        brief.erase("goal");
        brief["objective"] = "private goal value";
        brief["acceptance_criteria"] = boost::json::array{"private criterion value"};
        const auto message = rejection_message([&] { registry->validate("plan", plan, model_input); });
        CHECK(message.find("/nodes/1/brief/acceptance_criteria/0: type mismatch") != std::string::npos);
        CHECK(message.find("/nodes/1/brief/objective: property is not allowed") != std::string::npos);
        CHECK(message.find("/nodes/1/brief/goal: required property is missing") != std::string::npos);
        CHECK(message.find("private") == std::string::npos && message.size() <= 600);
    }
}

void diagnostics_escape_pointers_and_limit_errors_and_fragments() {
    test::TempDir directory;
    auto schema = basic_schema("diagnostics", parse_json_strict(R"({
        "a~/b":{"type":"string","format":"date"},
        "b":{"type":"string","pattern":"^allowed$"},
        "c":{"type":"integer"},"d":{"type":"integer"}
    })"));
    write(directory.path / "diagnostics.schema.json", schema);
    const SchemaRegistry local(directory.path);
    const Json bad = boost::json::object{{"a~/b", "private date"}, {"b", "private pattern"},
        {"c", "private type"}, {"d", "private fourth"}};
    const auto message = rejection_message([&] { local.validate("diagnostics", bad); });
    CHECK(message.find("/a~0~1b: format mismatch") != std::string::npos);
    CHECK(message.find("/b: pattern mismatch") != std::string::npos);
    CHECK(message.find("/c: type mismatch") != std::string::npos);
    CHECK(message.find("/d:") == std::string::npos && message.find("private") == std::string::npos);
    CHECK(message.size() <= 600);

    const std::string long_key = std::string(36, 'k') + "界界界" + std::string(200, 'k');
    Json shape = boost::json::object{{"type", "integer"}}, value = "private nested value";
    for (int depth = 0; depth < 8; ++depth) {
        shape = boost::json::object{{"type", "object"}, {"properties", boost::json::object{{long_key, shape}}}};
        value = boost::json::object{{long_key, value}};
    }
    write(directory.path / "bounded.schema.json", basic_schema("bounded", shape.at("properties")));
    const SchemaRegistry bounded(directory.path);
    const auto nested = rejection_message([&] { bounded.validate("bounded", value); });
    CHECK(nested.size() <= 600 && nested.find(std::string(36, 'k') + "...") != std::string::npos);
    CHECK(nested.find(long_key) == std::string::npos && nested.find("private") == std::string::npos);
    CHECK(parse_json_strict(canonical_json(Json(nested))).as_string() == nested);

    const std::string extra = "unexpected~/\n" + std::string(80, 'x');
    auto brief = example("task_brief"); brief.as_object()[extra] = "private extra value";
    const auto unknown = rejection_message([&] { registry->validate("task_brief", brief); });
    CHECK(unknown.find("/unexpected~0~1?") != std::string::npos);
    CHECK(unknown.find(std::string(40, 'x')) == std::string::npos && unknown.find('\n') == std::string::npos);
    CHECK(unknown.find("private") == std::string::npos && unknown.size() <= 600);
    brief = example("task_brief"); brief.as_object()[std::string(80, '~')] = "private extra value";
    const auto escaped = rejection_message([&] { registry->validate("task_brief", brief); });
    CHECK(escaped.find("/~0~0") != std::string::npos && escaped.find("~...") == std::string::npos);
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
    local.validate_v3_input("parent", complete);
    reject([&] { local.validate("parent", complete); });
    // additionalProperties=true in child must not permit forged host metadata.
    complete.as_object().at("result").as_object()["host_epoch"] = 99;
    reject([&] { local.validate("parent", complete, true); });
    reject([&] { local.validate_v3_input("parent", complete); });
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
    const auto projected = local.project_v3_input(local.project_model_input("pointer"));
    CHECK(projected.at("properties").at("result").at("pattern") == "^ok-[0-9]+$");
    CHECK(projected.at("properties").at("forbidden") == false);
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

void schema_parent_symlinks_reject_before_reading() {
    test::TempDir directory;
    const auto source = directory.path / "source";
    std::filesystem::create_directory(source);
    const auto file = source / "test.schema.json";
    write(file, basic_schema("test"));
    const auto link = directory.path / "linked";
    std::filesystem::create_directory_symlink(source, link);
    struct Watch {
        int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        ~Watch() { if (fd >= 0) ::close(fd); }
    } watch;
    CHECK(watch.fd >= 0);
    CHECK(::inotify_add_watch(watch.fd, file.c_str(), IN_OPEN) >= 0);
    bool rejected = false;
    try { (void)SchemaRegistry(link); }
    catch (const Error&) { rejected = true; }
    alignas(inotify_event) char events[4096];
    const auto count = ::read(watch.fd, events, sizeof events);
    // A parent symlink must fail before any schema data channel is opened.
    CHECK(count < 0 && errno == EAGAIN);
    CHECK(rejected);
    // Walking the original path must not erase the symlink with lexical '..'.
    reject([&] { (void)SchemaRegistry(link / ".." / "source"); });
    CHECK(::read(watch.fd, events, sizeof events) < 0 && errno == EAGAIN);
    const SchemaRegistry valid(source);
    valid.validate("test", boost::json::object{});
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
        {"v3 minimal contracts inline references and retain closed legacy schemas", v3_minimal_contracts_are_closed_and_legacy_schemas_unchanged},
        {"projection removes host conditions and preserves business conditions", runtime_conditions_are_projected_but_business_conditions_remain},
        {"projection scopes host names without weakening nested business fields", projection_keeps_same_named_fields_in_separate_instance_scopes},
        {"plan team briefs and draft-07 conditional requirements validate", nested_briefs_and_draft7_conditionals},
        {"schema diagnostics locate nested brief errors without parameter values", diagnostics_identify_nested_brief_errors_without_values},
        {"schema diagnostics escape pointers and bound errors and UTF-8 fragments", diagnostics_escape_pointers_and_limit_errors_and_fragments},
        {"100 findings remain legal and unique constraints are enforced", findings_capacity_and_unique_items},
        {"registered relative references preserve model projection and deny host fields", closed_local_references_and_projection},
        {"local JSON Pointers, boolean schemas and patterns are enforced", local_pointer_and_boolean_schema},
        {"unvisited remote, file and traversal refs fail before validation", all_references_checked_before_validation},
        {"duplicate IDs, duplicate JSON keys and missing pointers reject", malformed_ids_documents_and_missing_pointer_reject},
        {"schema symlinks and unsupported declared drafts reject", schema_paths_and_declared_draft_are_checked},
        {"schema parent symlinks reject before opening schema contents", schema_parent_symlinks_reject_before_reading}
    });
}
