#include "wls/offline_runtime.hpp"
#include "wls/worker_history.hpp"
#include "wls/worker_tools.hpp"
#include "live_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace wls::s1;
using wls::Role;
namespace live = wls::test::live;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path root(WLS_SOURCE_DIR);

std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t number(const Json& value, std::string_view key) { return value.at(key).to_number<std::int64_t>(); }
void expect(ErrorCode code, const std::function<void()>& operation) {
    try { operation(); }
    catch (const Error& error) { CHECK(error.code() == code); return; }
    throw std::runtime_error("expected Store error");
}
void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(static_cast<bool>(output));
}
void refresh_system(Json& snapshot) {
    const auto bytes = canonical_json(snapshot.at("system"));
    for (auto& dependency : snapshot.at("dependencies").as_array())
        if (dependency.at("kind") == "system_config") {
            dependency.at("content") = bytes; dependency.at("sha256") = sha256_hex(bytes);
        }
}

// Pure offline encoding of a captured FakeModel context; no send or live
// reservation occurs. Only the copy's protocol selection changes.
Json wire(ModelRequest request) {
    auto profile = wls::test::protocol::profile(); profile.model_ref = request.model.model;
    profile.context_window = 2000000; profile.max_output_tokens = 8192;
    auto document = to_json(request.context);
    document.at("protocol_version") = profile.protocol;
    auto& logical = document.at("input"); logical.at("selection").at("protocol_version") = profile.protocol;
    document.at("cache_identity") = sha256_hex(canonical_json(Object{{"format", "wls-context-cache-v3"},
        {"selection", logical.at("selection")}, {"prefix_sha256", document.at("prefix_sha256")},
        {"tool_policy", logical.at("tool_policy")}, {"model_projection", logical.at("model_projection")}}));
    document.at("logical_input_sha256") = sha256_hex(canonical_json(logical));
    request.context = compiled_context_from_json(document);
    return parse_json_strict(encode_responses_request(profile, request).body);
}

struct Fixture {
    test::TempDir directory;
    live::Assets assets{root};
    SchemaRegistry schemas{root / "schemas"};
    std::filesystem::path database{directory.path / "append-fold.sqlite"};
    std::filesystem::path workspace{directory.path / "workspace"};
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> tools;
    std::unique_ptr<OfflineRuntime> runtime;
    std::string request_id;
    std::string task_id;
    explicit Fixture(int window = 200000) {
        std::filesystem::create_directory(workspace);
        auto input = live::admission(assets, live::settings(), new_id("append"), false, true);
        input.original_text = read_runtime_file(root / "tests/fixtures/s1_request.txt");
        input.constraint_catalog = parse_json_strict(read_runtime_file(root / "tests/fixtures/s1_constraints.json"));
        input.budget = {64, 10000000, 1000000, 0};
        auto& system = input.config_snapshot.at("system");
        auto& context = system.at("context").as_object();
        context["request_format"] = "structured-v3";
        context["model_projection"] = "lean-v1";
        context["tool_policy"] = "fixed-role-v1";
        context["worker_history"] = Object{{"mode", "append-fold-v1"}, {"read_max_bytes", 24576},
            {"read_token_budget", 196608}};
        system.at("models").at("fake").at("capabilities").as_object()["context_window"] = window;
        refresh_system(input.config_snapshot);
        store = assets.open(database);
        request_id = text(store->admit(input), "request_id");
        open_runtime();
        (void)runtime->run(request_id, initial_steps(), "", 1);
        task_id = text(store->tasks(request_id).front(), "task_id");
    }

    static std::vector<FakeModelStep> initial_steps(const std::string& name = "s1_fake_initial.json") {
        auto steps = fake_model_steps_from_json(parse_json_strict(read_runtime_file(root / "tests/fixtures" / name)));
        for (auto& step : steps) for (auto& tool : step.tool_calls) if (tool.name == "submit_plan")
            for (auto& node : tool.arguments.at("nodes").as_array()) node.at("brief").at("title") = node.at("title");
        return steps; // V3-only derived copy; preserve the archived fixture bytes.
    }
    void open_runtime() {
        tools = std::make_unique<WorkerTools>(*store, workspace);
        runtime = std::make_unique<OfflineRuntime>(*store, schemas, tools.get());
    }
    StoreActor actor() const {
        return store->host_actor(Role::Worker, request_id, store->binding(task_id).team_id);
    }
    CapturedModelCall tool_call(std::int64_t index, const std::string& name, const Json& args,
                               bool native = false, std::string reply = {}) {
        FakeModelStep step; step.role = Role::Worker; step.role_call_index = index;
        step.text = std::move(reply);
        step.tool_calls = {{"tool-" + std::to_string(index), name, args}}; step.usage = {1, 1, 0, 0, 0};
        FakeModel fake({step});
        const auto call = runtime->reserve_call(fake, request_id, Role::Worker, task_id, index);
        auto response = fake.complete(call.request);
        if (native) response.responses_output = wls::test::protocol::compat_output_payload("o3-2025-04-16", Array{
            Object{{"id", "reason-" + std::to_string(index)}, {"type", "reasoning"}, {"summary", Array{}},
                {"encrypted_content", "Zml4dHVyZS1yZWFzb25pbmc="}, {"status", "completed"}},
            Object{{"id", "native-" + std::to_string(index)}, {"type", "function_call"},
                {"call_id", response.tool_calls.front().call_id}, {"name", name},
                {"arguments", canonical_json(args)}, {"status", "completed"}}});
        (void)runtime->accept_response(call, response);
        return call;
    }
    CapturedModelCall note(std::int64_t index, std::size_t bytes = 3200) {
        return tool_call(index, "note", Object{{"text", std::string(bytes, static_cast<char>('a' + index % 20))}}, true);
    }
    std::int64_t sequence(const std::string& attempt_id) const {
        const auto tail = store->attempt_history_tail(actor(), 8);
        for (const auto& item : tail.at("items").as_array())
            if (text(item, "attempt_id") == attempt_id) return number(item, "sequence");
        throw std::runtime_error("attempt sequence not found");
    }
    void reopen() {
        runtime.reset(); tools.reset();
        store.reset();
        store = assets.open(database);
        (void)store->acknowledge(actor(), store->binding(task_id), number(store->task(task_id), "state_version"), new_id("ack"));
        open_runtime();
    }
};

void append_fold_reopen_prefix_and_amend() {
    Fixture fixture;
    std::vector<std::pair<std::string, Json>> originals;
    std::size_t folded_at = 0;
    std::size_t generation = 0;
    for (int index = 2; index != 14 && generation == 0; ++index) {
        const auto preview = fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, index);
        // A 3200-byte native response grows this fixture by 16856 bytes and
        // would cross 120000 again. The first folded call returns 100 bytes.
        const auto call = fixture.note(index, number(preview.request.context.logical_input.at("worker_history"), "fold_generation") ? 100 : 3200);
        originals.emplace_back(call.request.attempt_id, fixture.store->attempt(call.request.attempt_id));
        const auto& logical = call.request.context.logical_input;
        const auto& history = logical.at("worker_history");
        CHECK(history.as_object().size() == 5);
        CHECK(!history.as_object().contains("messages"));
        CHECK(logical.at("messages").is_array());
        generation = static_cast<std::size_t>(number(history, "fold_generation"));
        if (generation != 0) {
            folded_at = originals.size();
            CHECK(!history.at("folded_entries").as_array().empty());
            CHECK(history.at("visible_turns").as_array().back().at("native_reasoning") == true);
            CHECK(history.at("visible_turns").as_array().size() < originals.size() + 1);
            CHECK(std::any_of(logical.at("messages").as_array().begin(), logical.at("messages").as_array().end(),
                [](const auto& message) { return message.at("content").as_string().find("history_fold") != std::string_view::npos; }));
        }
    }
    CHECK(generation == 1 && folded_at > 0);
    const auto folded = fixture.store->previous_context(fixture.actor());
    const auto folded_input = folded.at("input");
    const auto appended = fixture.note(20, 600);
    CHECK(number(appended.request.context.logical_input.at("worker_history"), "fold_generation") == 1);
    const auto& before_messages = folded_input.at("messages").as_array();
    const auto& after_messages = appended.request.context.logical_input.at("messages").as_array();
    CHECK(after_messages.size() > before_messages.size());
    CHECK(std::equal(before_messages.begin(), before_messages.end(), after_messages.begin()));
    auto before_wire = wire(model_request_from_json(originals.back().second.at("request_envelope")));
    auto after_wire = wire(appended.request);
    before_wire.at("input").as_array().pop_back(); after_wire.at("input").as_array().pop_back();
    CHECK(before_wire.at("tools") == after_wire.at("tools") && before_wire.at("instructions") == after_wire.at("instructions"));
    CHECK(after_wire.at("input").as_array().size() > before_wire.at("input").as_array().size());
    CHECK(std::equal(before_wire.at("input").as_array().begin(), before_wire.at("input").as_array().end(),
        after_wire.at("input").as_array().begin()));
    const auto continued = fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, 21);
    fixture.reopen();
    const auto reopened = fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, 21);
    CHECK(reopened.request.context.logical_input.at("worker_history") == continued.request.context.logical_input.at("worker_history"));
    CHECK(reopened.request.context.logical_input.at("messages") == continued.request.context.logical_input.at("messages"));
    for (const auto& [id, original] : originals)
        CHECK(fixture.store->attempt(id) == original);
    // Same original Store history through the frozen v2 layout, with the
    // same offline codec profile. This is a body-size comparison, not cache evidence.
    auto legacy = reopened.request; ContextInput input;
    input.role = Role::Worker; input.model = legacy.model; input.adapter_version = "1";
    input.protocol_version = "openai-responses-v1"; input.task_contract = compiled_task_contract(legacy.context);
    const auto& prefix = legacy.context.logical_input.at("prefix"); const auto& system = prefix.at("system");
    input.system_prompt = text(system, "system_prompt"); input.role_prompt = text(system, "role_prompt");
    input.function_prompt = text(system, "function_prompt"); input.environment_card = text(system, "environment_card");
    input.approved_rules = text(system, "approved_rules"); input.project_snapshot = text(system, "project_snapshot");
    for (const auto& tool : prefix.at("tools").as_array())
        input.tools.push_back({text(tool, "name"), text(tool, "version"), tool.at("parameters")});
    legacy.context = compile_structured_context(input, structured_attempt_history(fixture.store->attempt_history(fixture.actor())));
    auto profile = wls::test::protocol::profile(); profile.model_ref = legacy.model.model;
    profile.context_window = 2000000; profile.max_output_tokens = 8192;
    const auto v2_bytes = encode_responses_request(profile, legacy).body.size(), lean_bytes = canonical_json(wire(reopened.request)).size();
    std::cout << "append-fold body bytes v2=" << v2_bytes << " v3=" << lean_bytes << " ratio=" << double(lean_bytes) / double(v2_bytes) << '\n';
    CHECK(lean_bytes < v2_bytes);

    const auto before_revision = number(fixture.store->request(fixture.request_id), "requirements_revision");
    (void)fixture.store->amend(fixture.store->host_actor(Role::Root, fixture.request_id),
        number(fixture.store->request(fixture.request_id), "state_version"),
        read_runtime_file(root / "tests/fixtures/s1_amendment.txt"),
        parse_json_strict(read_runtime_file(root / "tests/fixtures/s1_amended_constraints.json")), new_id("amend"));
    const auto amended = Fixture::initial_steps("s1_fake_amended.json");
    (void)fixture.runtime->run(fixture.request_id, amended, "update", 1);
    CHECK(number(fixture.store->request(fixture.request_id), "requirements_revision") == before_revision + 1);
    const auto updated = fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, 31);
    CHECK(updated.request.context.logical_input.at("contract_updates").as_array().size() == 1);
    CHECK(updated.request.context.logical_input.at("messages").as_array().size() >= after_messages.size());
    CHECK(number(updated.request.context.logical_input.at("worker_history"), "fold_generation") == 2);
    const auto encoded = wire(updated.request); const auto serialized = canonical_json(encoded);
    CHECK(serialized.find("改为中文") != std::string::npos);
    CHECK(text(updated.request.context.logical_input.at("contract_updates").as_array().front(), "after_attempt_id") == appended.request.attempt_id);
    const auto update = updated.request.context.logical_input.at("contract_updates").as_array().front();
    std::int64_t folded_boundary = 0; Json folded_wire = nullptr;
    for (int index = 70; index != 82 && folded_boundary == 0; ++index) {
        const auto growth = fixture.note(index, 1600); const auto& history = growth.request.context.logical_input.at("worker_history");
        for (const auto& entry : history.at("folded_entries").as_array()) if (entry.at("attempt_id") == update.at("after_attempt_id")) {
            folded_boundary = number(entry, "sequence"); folded_wire = wire(growth.request);
            CHECK(number(history, "fold_generation") >= 2);
        }
    }
    CHECK(folded_boundary > 0);
    auto expected_update = update.at("contract"); expected_update.as_object()["semantic_versions"] = update.at("semantic_versions");
    const auto& ordered = folded_wire.at("input").as_array(); bool ordered_update = false;
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        const auto* content = ordered[index].as_object().if_contains("content");
        if (!content || !content->is_string() || !content->as_string().starts_with("{")) continue;
        if (parse_json_strict(std::string(content->as_string())) == expected_update) {
            const auto prior = parse_json_strict(text(ordered[index - 1], "content"));
            CHECK(prior.at("kind") == "history_fold" && number(prior, "through_sequence") == folded_boundary); ordered_update = true;
        }
    }
    CHECK(ordered_update);
}

void paged_original_fs_read_is_utf8_and_binary_safe() {
    Fixture fixture(2000000);
    std::string utf8(24575, 'a'); utf8 += "\xF0\x9F\x98\x80"; utf8.append(61440 - utf8.size(), 'z');
    write_bytes(fixture.workspace / "utf8.bin", utf8);
    const auto read_call = fixture.tool_call(40, "fs_read", Object{{"path", "utf8.bin"}, {"offset", 0}, {"max_bytes", 65536}});
    const auto sequence = fixture.sequence(read_call.request.attempt_id);
    const auto original = fixture.store->tool_feedback(read_call.request.attempt_id);
    write_bytes(fixture.workspace / "utf8.bin", "new live content");
    const auto before_reads = fixture.store->budget(fixture.request_id);
    std::vector<Json> pages;
    std::int64_t offset = 0;
    while (true) {
        CHECK(pages.size() < 3);
        const auto page_call = fixture.tool_call(100 + static_cast<std::int64_t>(pages.size()), "history_read",
            Object{{"sequence", sequence}, {"section", "result_data"}, {"offset", offset}, {"max_bytes", 24576}});
        const auto result = fixture.store->tool_feedback(page_call.request.attempt_id).at("result");
        pages.push_back(result);
        CHECK(result.at("encoding") == "utf-8");
        CHECK(text(result, "payload_sha256") == sha256_hex(utf8));
        CHECK(result.at("record_sha256") == original.at("record_sha256"));
        CHECK(result.at("original_binding") == original.at("original_binding"));
        CHECK(number(result, "offset") == offset && number(result, "size_bytes") == 61440);
        if (result.at("eof").as_bool()) break;
        CHECK(number(result, "next_offset") > offset); offset = number(result, "next_offset");
    }
    CHECK(pages.size() == 3);
    CHECK(number(fixture.store->budget(fixture.request_id), "attempts") == number(before_reads, "attempts") + 3);
    std::string joined;
    for (const auto& page : pages) joined += text(page, "content");
    CHECK(joined == utf8);
    CHECK(number(pages.front(), "bytes_returned") == 24575);
    CHECK(pages.back().at("eof") == true && pages.back().at("next_offset").is_null());
    expect(ErrorCode::InvalidInput, [&] { (void)fixture.store->read_history_sequence(fixture.actor(), sequence, "result_data", 24576, 10); });

    const std::string binary("\0\xff\x80" "bin", 6);
    write_bytes(fixture.workspace / "binary.bin", binary);
    const auto binary_call = fixture.tool_call(140, "fs_read", Object{{"path", "binary.bin"}, {"offset", 0}, {"max_bytes", 65536}});
    const auto binary_sequence = fixture.sequence(binary_call.request.attempt_id);
    const auto binary_page = fixture.store->read_history_sequence(fixture.actor(), binary_sequence, "result_data", 0, 24576);
    CHECK(binary_page.at("encoding") == "base64");
    CHECK(binary_page.at("content_base64") == "AP+AYmlu");
    CHECK(text(binary_page, "payload_sha256") == sha256_hex(binary));
    write_bytes(fixture.workspace / "binary.bin", "changed-live-file");
    const auto replay = fixture.store->read_history_sequence(fixture.actor(), binary_sequence, "result_data", 0, 24576);
    CHECK(replay == binary_page);
    expect(ErrorCode::InvalidInput, [&] { (void)fixture.store->read_history_sequence(fixture.actor(), binary_sequence, "request_envelope"); });
    expect(ErrorCode::Unauthorized, [&] { (void)fixture.store->read_history_sequence(fixture.actor(), 1, "result_data"); });
    // Damage only this fixture and restore its immutable trigger before the guard runs.
    live::sql(fixture.database, "DROP TRIGGER model_tool_feedback_immutable; UPDATE model_tool_feedback SET result='{}' WHERE attempt_id='" +
        binary_call.request.attempt_id + "'; CREATE TRIGGER model_tool_feedback_immutable BEFORE UPDATE ON model_tool_feedback "
        "BEGIN SELECT RAISE(ABORT, 'immutable model tool feedback'); END;");
    CHECK(live::sql_text(fixture.database, "SELECT result FROM model_tool_feedback WHERE attempt_id='" + binary_call.request.attempt_id + "'") == "{}");
    expect(ErrorCode::Corrupt, [&] { (void)fixture.store->read_history_sequence(fixture.actor(), binary_sequence, "result_data"); });
}

void stale_capture_and_capacity_stop_before_reservation() {
    Fixture fixture(400000);
    const auto stale = fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, 50);
    (void)fixture.note(51, 500);
    const auto budget = fixture.store->budget(fixture.request_id);
    expect(ErrorCode::Conflict, [&] { (void)fixture.store->reserve_attempt(stale.actor, stale.request.binding,
        live::Fixture::reservation(stale.request, true)); });
    CHECK(fixture.store->budget(fixture.request_id) == budget);
    const auto pending = fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, 60);
    (void)fixture.store->reserve_attempt(pending.actor, pending.request.binding, live::Fixture::reservation(pending.request, true));
    const auto predecessor = fixture.store->previous_context(fixture.actor());
    expect(ErrorCode::NotReady, [&] { (void)fixture.store->append_history_capture(fixture.actor()); });
    CHECK(fixture.store->previous_context(fixture.actor()) == predecessor);
    FakeModelStep step; step.role = Role::Worker; step.role_call_index = 60;
    step.tool_calls = {{"stale", "note", Object{{"text", "stale"}}}}; step.usage = {1, 1, 0, 0, 0};
    FakeModel fake({step});
    (void)fixture.runtime->accept_response(pending, fake.complete(pending.request));
    const auto high = fixture.sequence(pending.request.attempt_id);
    const auto ready = fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, 61);
    CHECK(number(ready.request.context.logical_input.at("worker_history"), "last_sequence") == high);
    const auto actor = fixture.actor(); const auto binding = fixture.store->binding(fixture.task_id);
    const auto operation = fixture.store->prepare_operation(actor, binding, number(fixture.store->task(fixture.task_id), "state_version"),
        new_id("effect"), "note", Object{{"text", "unresolved original operation"}}, "none", new_id("prepare"));
    const auto id = text(operation, "op_id");
    (void)fixture.store->start_operation(actor, id, number(operation, "state_version"), new_id("start"));
    live::sql(fixture.database, "UPDATE ops SET status='UNKNOWN',state_version=state_version+1 WHERE op_id='" + id + "'");
    const auto unknown = fixture.store->operation(id), boundary = fixture.store->previous_context(actor), before_unknown = fixture.store->budget(fixture.request_id);
    expect(ErrorCode::UnknownOutcome, [&] { (void)fixture.note(62, 100); });
    CHECK(fixture.store->operation(id) == unknown && fixture.store->previous_context(actor) == boundary);
    CHECK(fixture.store->budget(fixture.request_id) == before_unknown);
    Fixture narrow;
    (void)narrow.tool_call(2, "note", Object{{"text", "large indivisible response"}}, false, std::string(160000, 'x'));
    const auto before = narrow.store->budget(narrow.request_id);
    expect(ErrorCode::Quota, [&] { (void)narrow.note(3, 100); });
    CHECK(narrow.store->budget(narrow.request_id) == before);
}

void append_policy_rejects_single_agent_and_old_window() {
    Json append = Object{{"mode", "append-fold-v1"}, {"read_max_bytes", 24576}, {"read_token_budget", 196608}};
    CHECK(load_config_catalog(root / "config/examples/system.yaml", root / "config/examples/functions", root / "prompts").freeze("general").worker_history_policy() == append);
    CHECK(append_fold_policy(append));
    CHECK(!append_fold_policy(Object{{"mode", "window-v1"}, {"recent_attempts", 2}, {"inline_bytes", 32768}}));
    CHECK(history_control_declarations(append).back().definition.at("required").as_array().front() == "sequence");
    auto input = live::admission(live::Assets(root), live::settings(), new_id("single"), false, true);
    auto& context = input.config_snapshot.at("system").at("context").as_object();
    context["request_format"] = "structured-v3"; context["model_projection"] = "lean-v1";
    context["worker_history"] = append;
    live::Assets assets(root); test::TempDir directory; auto store = assets.open(directory.path / "reject.sqlite");
    const auto valid = input.config_snapshot;
    for (int variant = 0; variant != 4; ++variant) {
        auto changed = input; auto& policy = changed.config_snapshot.at("system").at("context");
        if (variant == 0) policy.at("worker_history").as_object().erase("read_token_budget");
        if (variant == 1) { policy.at("request_format") = "structured-v2"; policy.as_object().erase("model_projection"); }
        if (variant == 2) policy.as_object().erase("model_projection");
        if (variant == 3) policy.as_object()["logical_agent"] = Object{{"mode", "single-agent-role-broker-v1"}, {"max_attempts", 8}, {"max_history_bytes", 65536}};
        refresh_system(changed.config_snapshot);
        expect(ErrorCode::InvalidInput, [&] { (void)store->admit(changed); });
    }
    CHECK(history_control_declarations().back().definition.at("properties").at("max_bytes").at("maximum") == 4096);
    input.config_snapshot = valid;
    auto& window = input.config_snapshot.at("system").at("context");
    window.at("worker_history") = Object{{"mode", "window-v1"}, {"recent_attempts", 2}, {"inline_bytes", 32768}};
    window.as_object()["logical_agent"] = Object{{"mode", "single-agent-role-broker-v1"}, {"max_attempts", 8}, {"max_history_bytes", 65536}};
    refresh_system(input.config_snapshot);
    const auto bytes = canonical_json(input.config_snapshot); CHECK(!append_fold_policy(FrozenRequestConfig::restore(bytes, sha256_hex(bytes)).worker_history_policy()));
}
} // namespace

int main() {
    return test::run({
        {"append fold persists originals across reopen and amendment", append_fold_reopen_prefix_and_amend},
        {"result_data pages preserve UTF-8, binary bytes, and hash scope", paged_original_fs_read_is_utf8_and_binary_safe},
        {"stale captures and one-turn capacity fail before reservation", stale_capture_and_capacity_stop_before_reservation},
        {"append policy rejects incompatible history choices", append_policy_rejects_single_agent_and_old_window}
    });
}
