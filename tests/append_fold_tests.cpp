#include "wls/offline_runtime.hpp"
#include "wls/worker_history.hpp"
#include "wls/worker_tools.hpp"
#include "live_test_support.hpp"
#include "test_support.hpp"
#include "fake_script_support.hpp"
#include "../src/file_batch_runtime.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sys/syscall.h>

namespace {
std::function<void(const wls::s1::Json&)> batch_prepare_observer;
std::function<void()> batch_staged_observer;
void batch_prepared(const wls::s1::Json& operation) {
    if (batch_prepare_observer) batch_prepare_observer(operation);
}
struct ResetBatchObserver { ~ResetBatchObserver() { batch_prepare_observer = {}; batch_staged_observer = {}; } };
}
// Stop after a real staged file sync; SQLite syncs cannot trigger this phase.
extern "C" int fsync(int descriptor) {
    const int result = static_cast<int>(::syscall(SYS_fsync, descriptor));
    if (result == 0 && batch_staged_observer) {
        std::array<char, 4096> path{};
        const auto count = ::readlink(("/proc/self/fd/" + std::to_string(descriptor)).c_str(), path.data(), path.size());
        if (count > 0 && std::string_view(path.data(), static_cast<std::size_t>(count)).find("/.wls-tmp-") != std::string_view::npos) {
            auto action = std::move(batch_staged_observer); batch_staged_observer = {}; action();
        }
    }
    return result;
}
// Exercise the real executor's existing private phase, without a production hook.
#define WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE(key, prepared, stop) ::batch_prepared(prepared)
#include "../src/worker_tools.cpp"
#undef WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE

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
    explicit Fixture(int window = 200000, std::string policy = "fixed-role-v1", std::string history = "append-fold-v1",
                     std::optional<LiveModelSettings> provider = std::nullopt, bool narrow_store = false) {
        std::filesystem::create_directory(workspace);
        auto admitting = provider.value_or(live::settings()); admitting.provider.parallel_tool_calls = true;
        auto input = live::admission(assets, admitting, new_id("append"), provider.has_value(), !provider);
        input.original_text = read_runtime_file(root / "tests/fixtures/s1_request.txt");
        input.constraint_catalog = parse_json_strict(read_runtime_file(root / "tests/fixtures/s1_constraints.json"));
        input.budget = {64, 10000000, 1000000, provider ? 10000 : 0};
        auto& system = input.config_snapshot.at("system");
        if (narrow_store) {
            auto& limits = system.at("limits");
            limits.at("worker_max_tool_calls_per_order") = 3;
            limits.at("max_teams_per_request") = 1; limits.at("team_internal_rework_max") = 0;
            limits.at("store_capacity").at("operations") = 3;
        }
        if (provider) {
            auto profile = live::model_profile(*provider);
            profile.at("capabilities").at("parallel_tool_calls") = provider->provider.parallel_tool_calls ? "supported" : "unsupported";
            system.at("models").as_object()[provider->provider.model_ref] = profile;
        }
        auto& context = system.at("context").as_object();
        context["request_format"] = "structured-v3";
        context["model_projection"] = "lean-v1";
        context["tool_policy"] = policy;
        context["worker_history"] = Object{{"mode", "append-fold-v1"}, {"read_max_bytes", 24576},
            {"read_token_budget", 196608}};
        if (history == "full") context.erase("worker_history");
        if (history == "window-v1") context["worker_history"] = Object{{"mode", history}, {"recent_attempts", 2}, {"inline_bytes", 32768}};
        system.at("models").at("fake").at("capabilities").as_object()["context_window"] = window;
        refresh_system(input.config_snapshot);
        store = assets.open(database);
        request_id = text(store->admit(input), "request_id");
        open_runtime();
        if (provider) {
            const auto script = initial_steps();
            (void)store->apply_independent_plan(store->host_actor(Role::Dispatcher, request_id),
                number(store->request(request_id), "state_version"), 1, script[0].tool_calls[0].arguments, Object{}, new_id("plan"));
            task_id = text(store->tasks(request_id).front(), "task_id");
            (void)store->issue_instruction(store->host_actor(Role::TeamLead, request_id, store->binding(task_id).team_id),
                store->binding(task_id), number(store->task(task_id), "state_version"), script[1].tool_calls[0].arguments, new_id("order"));
            (void)store->acknowledge(actor(), store->binding(task_id), number(store->task(task_id), "state_version"), new_id("ack"));
            tools->bind(actor());
        } else (void)runtime->run(request_id, initial_steps("s1_fake_initial.json", policy == "fixed-role-file-batch-v1"), "", 1);
        task_id = text(store->tasks(request_id).front(), "task_id");
    }

    static std::vector<FakeModelStep> initial_steps(const std::string& name = "s1_fake_initial.json", bool batch = false) {
        auto result = fake_model_steps_from_json(test::v3_fake_script(parse_json_strict(read_runtime_file(root / "tests/fixtures" / name))));
        if (batch) result[2].tool_calls = {{"batch-a", "fs_write", Object{{"path", "batch-a.txt"}, {"content", "first batch"}}},
            {"batch-b", "fs_write", Object{{"path", "batch-b.txt"}, {"content", "second batch"}}}};
        return result;
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
    std::pair<CapturedModelCall, ModelResponse> pending_batch(std::int64_t index, const Json& first, const Json& second) {
        FakeModelStep step; step.role = Role::Worker; step.role_call_index = index;
        step.tool_calls = {{"batch-fail-a", "fs_write", first}, {"batch-fail-b", "fs_write", second}};
        step.usage = {1, 1, 0, 0, 0};
        FakeModel fake({step});
        const auto call = runtime->reserve_call(fake, request_id, Role::Worker, task_id, index);
        return {call, fake.complete(call.request)};
    }
    CapturedModelCall batch(std::int64_t index, const Json& first, const Json& second) {
        const auto [call, response] = pending_batch(index, first, second);
        (void)runtime->accept_response(call, response);
        return call;
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

void file_batch_runtime_writes_two_distinct_targets_with_one_feedback() {
    Fixture fixture(200000, "fixed-role-file-batch-v1");
    CHECK(read_runtime_file(fixture.workspace / "batch-a.txt") == "first batch");
    CHECK(read_runtime_file(fixture.workspace / "batch-b.txt") == "second batch");
    const auto history = fixture.store->attempt_history(fixture.actor());
    CHECK(!history.empty());
    const auto& attempt = history.back();
    const auto feedback = fixture.store->tool_feedback(std::string(attempt.at("attempt_id").as_string()));
    const auto& aggregate = feedback.at("result");
    CHECK(aggregate.at("format") == "wls-file-batch-feedback-v1");
    CHECK(aggregate.at("status") == "SUCCEEDED" && aggregate.at("runtime_error") == false);
    CHECK(!aggregate.as_object().contains("error_code") && !aggregate.as_object().contains("message"));
    CHECK(aggregate.at("observation_complete") == true && aggregate.at("items").as_array().size() == 2);
    CHECK(aggregate.at("items").as_array()[0].at("state") == "SUCCEEDED");
    CHECK(aggregate.at("items").as_array()[1].at("state") == "SUCCEEDED");
    const auto single = fixture.tool_call(2, "note", Object{{"text", "single-call compatibility"}});
    const auto single_feedback = fixture.store->tool_feedback(single.request.attempt_id);
    CHECK(!single_feedback.at("result").as_object().contains("format"));
}

void file_batch_preflight_failure_keeps_both_unexecuted() {
    Fixture fixture(200000, "fixed-role-file-batch-v1");
    const auto call = fixture.batch(2,
        Object{{"path", "batch-a.txt"}, {"content", "must not overwrite"}},
        Object{{"path", "batch-c.txt"}, {"content", "must not run"}});
    CHECK(read_runtime_file(fixture.workspace / "batch-a.txt") == "first batch");
    CHECK(!std::filesystem::exists(fixture.workspace / "batch-c.txt"));
    const auto feedback = fixture.store->tool_feedback(call.request.attempt_id);
    const auto& aggregate = feedback.at("result");
    CHECK(aggregate.at("format") == "wls-file-batch-feedback-v1");
    CHECK(aggregate.at("status") == "NOT_EXECUTED" && aggregate.at("runtime_error") == true);
    CHECK(aggregate.at("observation_complete") == true);
    CHECK(aggregate.at("items").as_array()[0].at("state") == "NOT_EXECUTED");
    CHECK(aggregate.at("items").as_array()[1].at("state") == "NOT_EXECUTED");
}

FileBatchHistoryContext batch_host(const Fixture& fixture) {
    return {fixture.request_id, text(fixture.store->request(fixture.request_id), "config_snapshot_sha256"),
        Role::Worker, "structured-v3", "fixed-role-file-batch-v1"};
}

void file_batch_preserves_prefix_on_executor_and_storage_faults() {
    for (const std::string fault : {"second_cas", "second_symlink", "task_cas", "revoke", "amend", "result_storage", "feedback_storage", "oversized_host", "unaccepted"}) {
        Fixture fixture(2000000, "fixed-role-file-batch-v1"); ResetBatchObserver reset;
        const auto [call, response] = fixture.pending_batch(2, Object{{"path", "left"}, {"content", "L"}},
            Object{{"path", "right"}, {"content", "R"}});
        int prepares = 0;
        batch_prepare_observer = [&](const Json&) {
            ++prepares;
            if (fault == "task_cas" && prepares == 1)
                live::sql(fixture.database, "UPDATE tasks SET state_version=state_version+1");
            if (prepares != 2) return;
            if (fault == "second_cas") write_bytes(fixture.workspace / "right", "external");
            if (fault == "second_symlink") CHECK(::symlink("left", (fixture.workspace / "right").c_str()) == 0);
            if (fault == "revoke") live::sql(fixture.database, "UPDATE runtime_policy SET denied_tools='[\"fs_write\"]',policy_epoch=policy_epoch+1");
            if (fault == "amend") (void)fixture.store->amend(fixture.store->host_actor(Role::Root, fixture.request_id),
                number(fixture.store->request(fixture.request_id), "state_version"),
                read_runtime_file(root / "tests/fixtures/s1_amendment.txt"),
                parse_json_strict(read_runtime_file(root / "tests/fixtures/s1_amended_constraints.json")), new_id("amend"));
            if (fault == "result_storage") live::sql(fixture.database,
                "CREATE TRIGGER batch_result_fault BEFORE INSERT ON events WHEN NEW.type='record_op_result' "
                "BEGIN SELECT RAISE(ABORT,'fixture result persistence failure'); END;");
            if (fault == "oversized_host") throw std::runtime_error(std::string(2048, 'x'));
        };
        if (fault == "feedback_storage") live::sql(fixture.database,
            "CREATE TRIGGER batch_feedback_fault BEFORE INSERT ON model_tool_feedback "
            "BEGIN SELECT RAISE(ABORT,'fixture feedback persistence failure'); END;");
        if (fault == "unaccepted") live::sql(fixture.database,
            "CREATE TRIGGER batch_epoch AFTER UPDATE OF status ON ops WHEN NEW.status='RUNNING' "
            "BEGIN UPDATE teams SET execution_epoch=execution_epoch+1,state_version=state_version+1; END;");
        bool threw = false;
        try { (void)fixture.runtime->accept_response(call, response); } catch (const std::exception&) { threw = true; }
        CHECK(threw == (fault == "feedback_storage" || fault == "oversized_host"));
        if (!std::filesystem::exists(fixture.workspace / "left")) throw std::runtime_error(
            "missing prefix in " + fault + ": " + canonical_json(fixture.store->tool_feedback(call.request.attempt_id)));
        CHECK(read_runtime_file(fixture.workspace / "left") == "L");
        CHECK(prepares == ((fault == "task_cas" || fault == "unaccepted") ? 1 : 2));
        const auto feedback = fixture.store->tool_feedback(call.request.attempt_id);
        const auto originals = live::sql_text(fixture.database, "SELECT group_concat(op_id || ':' || status) FROM ops");
        if (threw) {
            CHECK(feedback.is_null());
            expect(ErrorCode::NotReady, [&] { (void)fixture.runtime->accept_response(call, response); });
            expect(ErrorCode::NotReady, [&] { (void)fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, 3); });
            fixture.reopen();
            expect(ErrorCode::NotReady, [&] { (void)fixture.runtime->accept_response(call, response); });
            CHECK(read_runtime_file(fixture.workspace / "left") == "L");
            CHECK(fixture.store->tool_feedback(call.request.attempt_id).is_null());
        } else {
            const auto& group = feedback.at("result"); const auto& items = group.at("items").as_array();
            CHECK(items[0].at("state") == (fault == "unaccepted" ? "UNKNOWN" : "SUCCEEDED"));
            CHECK(items[1].at("state") == (fault == "unaccepted" ? "NOT_EXECUTED" : fault.starts_with("second_") ? "FAILED" : "UNKNOWN"));
            CHECK(group.at("observation_complete") == fault.starts_with("second_"));
            CHECK(group.as_object().contains("correction_feedback") == (fault == "second_cas"));
            if (fault == "unaccepted") CHECK(items[0].at("result").at("execution_result").at("accepted") == 0);
            CHECK(fixture.runtime->accept_response(call, response) == group);
            CHECK(live::sql_text(fixture.database, "SELECT group_concat(op_id || ':' || status) FROM ops") == originals);
            const auto host = batch_host(fixture);
            CHECK(structured_attempt_history(fixture.store->attempt_history(fixture.actor()), &host).size() >= 6);
        }
        if (fault == "second_cas") CHECK(read_runtime_file(fixture.workspace / "right") == "external");
        else CHECK(std::filesystem::exists(fixture.workspace / "right") == (fault == "result_storage" || fault == "feedback_storage" || fault == "second_symlink"));
    }
}

void file_batch_replay_is_observation_not_second_execution() {
    Fixture fixture(2000000, "fixed-role-file-batch-v1");
    const auto [call, response] = fixture.pending_batch(2, Object{{"path", "left"}, {"content", "L"}},
        Object{{"path", "right"}, {"content", "R"}});
    const auto& first = response.tool_calls.front();
    const auto key = "batch:" + sha256_hex(canonical_json(Object{{"attempt_id", call.request.attempt_id}, {"tool_call_id", first.call_id}}));
    const auto original = fixture.tools->execute(call.actor, call.request.binding, call.expected_state_version, key,
        first.name, first.arguments, "original-execution");
    // CAS preflight must observe the old target as absent, then execute can only
    // return its immutable original key; no recreated file or second effect.
    std::filesystem::remove(fixture.workspace / "left");
    const auto group = fixture.runtime->accept_response(call, response);
    CHECK(group.at("status") == "UNKNOWN" && group.at("observation_complete") == false);
    const auto& replay = group.at("items").as_array()[0].at("result");
    CHECK(replay.at("operation") == original.at("operation") && replay.at("replayed") == true);
    CHECK(group.at("items").as_array()[1].at("state") == "NOT_EXECUTED");
    CHECK(!std::filesystem::exists(fixture.workspace / "left") && !std::filesystem::exists(fixture.workspace / "right"));
    const auto saved = fixture.store->tool_feedback(call.request.attempt_id);
    fixture.reopen();
    CHECK(fixture.runtime->accept_response(call, response) == group);
    CHECK(fixture.store->tool_feedback(call.request.attempt_id) == saved);
}

void file_batch_rejections_and_old_single_call_behavior() {
    Fixture fixture(2000000, "fixed-role-file-batch-v1"); ResetBatchObserver reset;
    int prepares = 0; batch_prepare_observer = [&](const Json&) { ++prepares; };
    int index = 2;
    for (const Json& second : {Json(Object{{"path", "left"}, {"content", "same target"}}),
            Json(Object{{"path", "/work/right"}, {"content", "alias"}}), Json(Object{{"path", "../outside"}, {"content", "escape"}}),
            Json(Object{{"path", "missing/right"}, {"content", "parent"}}), Json(Object{{"path", "right"}, {"content", std::string(65537, 'x')}})}) {
        const auto call = fixture.batch(index++, Object{{"path", "left"}, {"content", "L"}}, second);
        const auto group = fixture.store->tool_feedback(call.request.attempt_id).at("result");
        CHECK(group.at("status") == "NOT_EXECUTED" && group.at("observation_complete") == true);
        for (const auto& item : group.at("items").as_array()) CHECK(item.at("state") == "NOT_EXECUTED" && item.at("result").is_null());
    }
    CHECK(prepares == 0 && !std::filesystem::exists(fixture.workspace / "left"));
    const auto single = fixture.tool_call(index++, "fs_write", Object{{"path", "/work/single"}, {"content", "kept"}});
    CHECK(fixture.store->tool_feedback(single.request.attempt_id).at("result").at("operation").at("status") == "SUCCEEDED");
    const auto read = fixture.tool_call(index, "fs_read", Object{{"path", "/work/single"}});
    CHECK(canonical_json(fixture.store->tool_feedback(read.request.attempt_id)).find("kept") != std::string::npos);
    batch_prepare_observer = {};
    Fixture old;
    const auto rejected = old.batch(2, Object{{"path", "left"}, {"content", "L"}}, Object{{"path", "right"}, {"content", "R"}});
    CHECK(old.store->tool_feedback(rejected.request.attempt_id).at("result").at("correction_feedback").at("reason") == "tool_selection");
    CHECK(!std::filesystem::exists(old.workspace / "left") && !std::filesystem::exists(old.workspace / "right"));
}

void file_batch_capacity_boundaries_preserve_originals() {
    Fixture fixture(2000000, "fixed-role-file-batch-v1");
    std::string escaped_parent;
    for (int depth = 0; depth != 16; ++depth) {
        escaped_parent += std::string(200, '\1') + std::to_string(depth) + "/";
        std::filesystem::create_directory(fixture.workspace / escaped_parent);
    }
    for (int index = 2; index != 5; ++index) {
        const auto parent = index == 2 ? escaped_parent : std::string{};
        auto [call, response] = fixture.pending_batch(index,
            Object{{"path", parent + "left-" + std::to_string(index)}, {"content", std::string(6000, '\1')}},
            Object{{"path", parent + "right-" + std::to_string(index)}, {"content", std::string(6000, '\2')}});
        const auto target_size = index == 2 ? 128U * 1024U : index == 3 ? 128U * 1024U + 1 : 600U * 1024U;
        response.text.assign(target_size - canonical_json(to_json(response)).size(), 'x');
        CHECK(canonical_json(to_json(response)).size() == target_size);
        const auto group = fixture.runtime->accept_response(call, response);
        CHECK(group.at("status") == (index == 2 ? "SUCCEEDED" : "NOT_EXECUTED"));
        CHECK(std::filesystem::exists(fixture.workspace / (parent + "left-" + std::to_string(index))) == (index == 2));
        const auto saved = fixture.store->tool_feedback(call.request.attempt_id);
        CHECK(saved.at("response") == to_json(response) && fixture.runtime->accept_response(call, response) == group);
        const auto host = batch_host(fixture);
        auto original = fixture.store->attempt_history(fixture.actor()).back();
        CHECK(!structured_attempt_history({original}, &host).empty());
        if (index == 3) {
            const auto unexecuted = original;
            auto& forged = original.at("feedback").at("result");
            forged.at("items").as_array()[0].at("state") = "UNKNOWN";
            forged.at("status") = "UNKNOWN"; forged.at("observation_complete") = false;
            auto& feedback = original.at("feedback");
            feedback.at("record_sha256") = sha256_hex(canonical_json(Object{{"attempt_id", original.at("attempt_id")},
                {"original_binding", feedback.at("original_binding")}, {"response", feedback.at("response")}, {"result", forged}}));
            expect(ErrorCode::Corrupt, [&] { (void)structured_attempt_history({original}, &host); });
            original = unexecuted;
            original.at("feedback").at("result").at("items").as_array()[0].at("result") = Object{};
            auto& zero = original.at("feedback");
            zero.at("record_sha256") = sha256_hex(canonical_json(Object{{"attempt_id", original.at("attempt_id")},
                {"original_binding", zero.at("original_binding")}, {"response", zero.at("response")}, {"result", zero.at("result")}}));
            expect(ErrorCode::Corrupt, [&] { (void)structured_attempt_history({original}, &host); });
        }
    }
    const auto budget = fixture.store->budget(fixture.request_id);
    expect(ErrorCode::Quota, [&] { (void)fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, 5); });
    CHECK(fixture.store->budget(fixture.request_id) == budget);
    Fixture window(2000000, "fixed-role-file-batch-v1", "window-v1");
    const auto call = window.batch(2, Object{{"path", "large-a"}, {"content", std::string(12000, 'x')}},
        Object{{"path", "large-b"}, {"content", std::string(12000, 'y')}});
    CHECK(window.store->tool_feedback(call.request.attempt_id).at("result").at("status") == "NOT_EXECUTED");
    CHECK(!std::filesystem::exists(window.workspace / "large-a") && !std::filesystem::exists(window.workspace / "large-b"));
    for (bool new_order : {false, true}) {
        Fixture limited(2000000, "fixed-role-file-batch-v1", "append-fold-v1", std::nullopt, true);
        if (new_order) {
            (void)limited.store->issue_instruction(limited.store->host_actor(Role::TeamLead, limited.request_id,
                limited.store->binding(limited.task_id).team_id), limited.store->binding(limited.task_id),
                number(limited.store->task(limited.task_id), "state_version"), Fixture::initial_steps()[1].tool_calls[0].arguments, new_id("order"));
            (void)limited.store->acknowledge(limited.actor(), limited.store->binding(limited.task_id),
                number(limited.store->task(limited.task_id), "state_version"), new_id("ack"));
        }
        ResetBatchObserver reset; int prepares = 0; batch_prepare_observer = [&](const Json&) { ++prepares; };
        const auto rejected = limited.batch(2, Object{{"path", "left"}, {"content", "L"}}, Object{{"path", "right"}, {"content", "R"}});
        const auto group = limited.store->tool_feedback(rejected.request.attempt_id).at("result");
        CHECK(group.at("status") == "NOT_EXECUTED" && group.at("observation_complete") == true && group.at("error_code") == "quota");
        CHECK(text(group, "message").find(new_order ? "operation slots" : "instruction tool calls") != std::string::npos);
        CHECK(prepares == 0 && limited.store->capacity_status(limited.request_id).at("counts").at("operations") == 2);
        CHECK(!std::filesystem::exists(limited.workspace / "left") && !std::filesystem::exists(limited.workspace / "right"));
    }
}

void file_batch_live_hook_stop_deadline_and_profile_gates() {
    const auto provider = live::settings();
    for (const std::string mode : {"success", "hook", "tamper", "stop", "deadline", "second_cancel", "second_deadline", "between_stop", "between_deadline"}) {
        Fixture fixture(200000, "fixed-role-file-batch-v1", "append-fold-v1", provider); ResetBatchObserver reset;
        const auto call = fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, 1, {}, false,
            {100, std::chrono::seconds(3)});
        (void)fixture.store->reserve_attempt(call.actor, call.request.binding, live::Fixture::reservation(call.request));
        const auto encoded = encode_protocol_request(provider.provider, call.request, provider.stream);
        const auto claim = fixture.store->claim_live_send(call.actor, call.request.attempt_id, 1, encoded, provider.transport, new_id("claim"));
        std::stop_source no_network; no_network.request_stop();
        auto transport = perform_protocol_https(provider.provider, call.request, encoded, provider.transport,
            "SYNTHETIC_NO_NETWORK_FIXTURE", no_network.get_token());
        CHECK(!transport.network_started);
        // Explicit synthetic host observation for Store/application tests. The
        // independent protocol suite tests decoding; this is no TLS/provider evidence.
        auto summary = https_transport_summary(transport); summary.as_object().erase("record_sha256");
        auto& response = transport.response;
        response.status = ModelResultStatus::Completed; response.error_code.clear(); response.error_message.clear();
        response.tool_calls = {{"left", "fs_write", Object{{"path", "left"}, {"content", "L"}}},
            {"right", "fs_write", Object{{"path", "right"}, {"content", "R"}}}};
        response.usage = {1, 1, 0, 0, 0}; transport.status = HttpsTransportStatus::Completed;
        transport.reason = "synthetic_host_observation";
        summary.at("status") = to_string(transport.status); summary.at("reason") = transport.reason;
        summary.at("response_usable") = true; summary.at("decoded_response_sha256") = sha256_hex(canonical_json(to_json(response)));
        transport.record_sha256 = sha256_hex(canonical_json(summary));
        (void)fixture.store->record_live_transport(call.request.attempt_id, text(claim, "claim_id"), transport);
        std::stop_source stop; if (mode == "stop") stop.request_stop();
        const auto deadline = mode == "deadline" ? std::chrono::steady_clock::now() :
            (mode == "second_deadline" || mode == "between_deadline") ? std::chrono::steady_clock::now() + std::chrono::seconds(2) : std::chrono::steady_clock::time_point::max();
        int prepares = 0, hooks = 0;
        batch_prepare_observer = [&](const Json&) {
            if (++prepares != 2) return;
            if (mode == "second_cancel") stop.request_stop();
            if (mode == "second_deadline") std::this_thread::sleep_until(deadline);
        };
        if (mode.starts_with("between_")) batch_staged_observer = [&] {
            if (mode == "between_stop") stop.request_stop();
            else std::this_thread::sleep_until(deadline);
        };
        const AfterModelSettlement hook = [&](const Json& original) {
            ++hooks; CHECK(original.at("response") == to_json(response));
            if (mode == "hook") throw Error(ErrorCode::Storage, "synthetic accounting hook failure");
            if (mode == "tamper") response.tool_calls[0].arguments.at("content") = "changed-after-settlement";
        };
        if (mode == "tamper") {
            const auto original = response;
            expect(ErrorCode::Conflict, [&] { (void)fixture.runtime->accept_live_response(call, response, hook); });
            CHECK(hooks == 1 && prepares == 0 && fixture.store->tool_feedback(call.request.attempt_id).is_null());
            CHECK(!std::filesystem::exists(fixture.workspace / "left") && !std::filesystem::exists(fixture.workspace / "right"));
            expect(ErrorCode::NotReady, [&] { (void)fixture.runtime->accept_live_response(call, original, hook); });
            continue;
        }
        const auto group = fixture.runtime->accept_live_response(call, response, hook, false, stop.get_token(), deadline);
        CHECK(hooks == 1);
        const bool starts = mode == "success" || mode.starts_with("second_") || mode.starts_with("between_");
        CHECK(prepares == (mode.starts_with("between_") ? 1 : starts ? 2 : 0));
        CHECK(std::filesystem::exists(fixture.workspace / "left") == starts);
        CHECK(std::filesystem::exists(fixture.workspace / "right") == (mode == "success"));
        CHECK(group.at("status") == (mode == "success" ? "SUCCEEDED" : mode.starts_with("second_") ? "CANCELLED" : "NOT_EXECUTED"));
        if (mode.starts_with("between_")) CHECK(group.at("items").as_array()[0].at("state") == "SUCCEEDED" &&
            group.at("items").as_array()[1].at("state") == "NOT_EXECUTED");
        CHECK(group.at("observation_complete") == true);
        if (mode == "hook") CHECK(group.at("error_code") == "storage" && group.at("message") == "synthetic accounting hook failure");
        CHECK(fixture.runtime->accept_live_response(call, response, hook) == group && hooks == 1);
        const auto host = batch_host(fixture);
        CHECK(structured_attempt_history(fixture.store->attempt_history(fixture.actor()), &host).size() == 3);
    }
    for (bool parallel : {false, true}) {
        auto limited = provider;
        limited.provider.parallel_tool_calls = parallel;
        if (parallel) limited.provider.limits.max_tool_calls = 1;
        Fixture fixture(200000, "fixed-role-file-batch-v1", "append-fold-v1", limited);
        expect(ErrorCode::Unsupported, [&] { (void)fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id); });
        CHECK(number(fixture.store->budget(fixture.request_id), "attempts") == 0);
    }
}

void file_batch_native_history_modes_reopen_and_fold() {
    for (const std::string mode : {"full", "window-v1", "append-fold-v1"}) {
        Fixture fixture(2000000, "fixed-role-file-batch-v1", mode);
        auto [call, response] = fixture.pending_batch(2, Object{{"path", "left"}, {"content", "L"}},
            Object{{"path", "right"}, {"content", "R"}});
        Array output{Object{{"id", "reason-batch"}, {"type", "reasoning"}, {"summary", Array{}},
            {"encrypted_content", "Zml4dHVyZS1yZWFzb25pbmc="}, {"status", "completed"}}};
        for (const auto& tool : response.tool_calls) output.push_back(Object{{"id", "native-" + tool.call_id},
            {"type", "function_call"}, {"call_id", tool.call_id}, {"name", tool.name},
            {"arguments", canonical_json(tool.arguments)}, {"status", "completed"}});
        response.responses_output = wls::test::protocol::compat_output_payload("o3-2025-04-16", output);
        const auto group = fixture.runtime->accept_response(call, response);
        CHECK(group.at("status") == "SUCCEEDED");
        const auto next = fixture.note(3, 100); // Includes Store's independent append recomputation.
        CHECK(canonical_json(next.request.context.logical_input).find("Zml4dHVyZS1yZWFzb25pbmc=") != std::string::npos);
        if (mode == "append-fold-v1") {
            const auto capture = fixture.store->append_history_capture(fixture.actor()); const auto host = batch_host(fixture);
            const auto folded = append_fold_history(capture.at("previous").at("input"), capture.at("delta"), true, &host);
            CHECK(!folded.at("folded_entries").as_array().empty());
        }
        const auto original = fixture.store->tool_feedback(call.request.attempt_id);
        fixture.reopen();
        (void)fixture.runtime->capture_call(fixture.request_id, Role::Worker, fixture.task_id, 4);
        CHECK(fixture.runtime->accept_response(call, response) == group);
        CHECK(fixture.store->tool_feedback(call.request.attempt_id) == original);
    }
}

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

int main(int argc, char** argv) {
    const std::string group = argc == 3 && std::string_view(argv[1]) == "--group" ? argv[2] : "";
    if (argc != 1 && group.empty()) return 2;
    const std::vector<test::Case> cases{
        {"file batch writes two distinct targets with one immutable aggregate", file_batch_runtime_writes_two_distinct_targets_with_one_feedback},
        {"file batch preflight failure keeps both items unexecuted", file_batch_preflight_failure_keeps_both_unexecuted},
        {"file batch preserves prefixes across executor and storage faults", file_batch_preserves_prefix_on_executor_and_storage_faults},
        {"file batch replay is only an observation", file_batch_replay_is_observation_not_second_execution},
        {"file batch rejection preserves old single-call behavior", file_batch_rejections_and_old_single_call_behavior},
        {"file batch capacity boundaries preserve original facts", file_batch_capacity_boundaries_preserve_originals},
        {"file batch live hook, cancellation, deadline and profile gates", file_batch_live_hook_stop_deadline_and_profile_gates},
        {"file batch native history survives modes, reopen and fold", file_batch_native_history_modes_reopen_and_fold},
        {"append fold persists originals across reopen and amendment", append_fold_reopen_prefix_and_amend},
        {"result_data pages preserve UTF-8, binary bytes, and hash scope", paged_original_fs_read_is_utf8_and_binary_safe},
        {"stale captures and one-turn capacity fail before reservation", stale_capture_and_capacity_stop_before_reservation},
        {"append policy rejects incompatible history choices", append_policy_rejects_single_agent_and_old_window}
    };
    const std::array<std::string_view, 12> groups{"guards", "guards", "faults", "guards", "guards", "capacity", "live", "history",
        "legacy", "legacy", "legacy", "legacy"};
    std::vector<test::Case> selected;
    for (std::size_t i = 0; i != cases.size(); ++i)
        if (group.empty() || group == groups[i]) selected.push_back(cases[i]);
    return selected.empty() ? 2 : test::run(selected);
}
