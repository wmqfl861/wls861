#include "wls/config_loader.hpp"
#include "wls/migrations.hpp"
#include "wls/model.hpp"
#include "wls/offline_runtime.hpp"
#include "wls/runtime_resources.hpp"
#include "wls/schema.hpp"
#include "wls/store.hpp"
#include "test_support.hpp"
#include "persistent_event_faults.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <source_location>
#include <string>
#include <poll.h>
#include <sys/wait.h>

using namespace wls::s1;
using wls::Role;
using Object = boost::json::object;
using Array = boost::json::array;

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
const auto source = std::filesystem::absolute(WLS_SOURCE_DIR);
const RuntimeResources& resources() {
    static const auto value = RuntimeResources::from_source_tree(source);
    return value;
}
std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t number(const Json& value, std::string_view key) { return value.at(key).as_int64(); }
void expect(ErrorCode code, const std::function<void()>& action,
            const std::source_location location = std::source_location::current()) {
    try { action(); } catch (const Error& failure) {
        if (failure.code() != code)
            std::cerr << location.file_name() << ':' << location.line() << ": expected "
                      << error_name(code) << ", actual " << error_name(failure.code())
                      << ": " << failure.what() << std::endl;
        CHECK(failure.code() == code); return;
    }
    throw std::runtime_error("expected Store error");
}
void reject(const std::function<void()>& action) {
    bool rejected = false;
    try { action(); } catch (const Error&) { rejected = true; }
    CHECK(rejected);
}
void sql(const std::filesystem::path& path, const std::string& statement) {
    sqlite3* db{};
    CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    const auto result = sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db); CHECK(result == SQLITE_OK);
}
std::int64_t scalar(const std::filesystem::path& path, const std::string& statement) {
    sqlite3* db{};
    CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* query{};
    CHECK(sqlite3_prepare_v2(db, statement.c_str(), -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW);
    const auto value = static_cast<std::int64_t>(sqlite3_column_int64(query, 0));
    sqlite3_finalize(query); sqlite3_close(db); return value;
}
AdmissionInput input() {
    AdmissionInput value;
    value.tenant_id = "tenant"; value.project_id = "project"; value.root_id = "root";
    value.function_id = "general"; value.idempotency_key = new_id("admit");
    value.original_text = "preserve fixture observations";
    value.config_snapshot = load_config_catalog(source / "config/examples/system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general").json();
    value.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", value.original_text}, {"source_revision", 1}}};
    value.tool_allowlist = {"note", "ack_change", "op_status", "op_cancel"};
    return value;
}
Json plan_body() {
    Array nodes;
    for (const auto* name : {"alpha", "beta"}) nodes.emplace_back(Object{{"node_id", name}, {"kind", "team_task"},
        {"title", name}, {"depends_on", Array{}}, {"required", true}, {"brief", Object{{"title", name},
            {"goal", "preserve original observations"}, {"acceptance_criteria", Array{Object{{"id", "ac1"}, {"text", "retain original facts"}}}},
            {"constraint_refs", Array{"c1"}}, {"constraints_verbatim", Array{"preserve fixture observations"}}}}});
    return Object{{"summary", "two independent nodes"}, {"nodes", nodes}};
}
Json order_body() {
    return Object{{"approach", "record the current original tuple"},
        {"steps", Array{Object{{"id", "s1"}, {"text", "write one database note"}}}},
        {"done_definition", "original note and feedback are durable"}, {"constraint_refs", Array{"c1"}},
        {"constraints_verbatim", Array{"preserve fixture observations"}}};
}
AttemptReservation reservation_for(const CapturedModelCall& call) {
    const auto& request = call.request;
    AttemptReservation value;
    value.call_id = request.call_id; value.attempt_id = request.attempt_id;
    value.config_snapshot_sha256 = request.config_snapshot_id; value.model_ref = request.model.model;
    value.reserved_input_tokens = request.reserved_input_tokens; value.reserved_output_tokens = request.reserved_output_tokens;
    value.reserved_cost_micro_usd = request.reserved_cost_micro_usd; value.max_output_tokens = request.max_output_tokens;
    value.request_envelope = to_json(request); return value;
}
struct Bound {
    Json claim;
    CapturedModelCall captured;
    AttemptReservation reservation;
    EventCallAssociation association;
    Json link = nullptr;
};
// These negative cases change a trusted host's target, never model arguments
// or durable facts. Recompile all original context frames so rejection comes
// from the target guard rather than a deliberately broken envelope hash.
void recompile_host_action(Bound& value) {
    const auto original = value.captured.request.context;
    CHECK(!original.structured()); // This fixture explicitly freezes framed-v1.
    const auto frames = [](std::string_view bytes) {
        std::vector<std::pair<std::string, std::string>> result;
        while (!bytes.empty()) {
            const auto space = bytes.find(' '), newline = bytes.find('\n');
            CHECK(space != std::string_view::npos && newline != std::string_view::npos && space < newline);
            const auto size = static_cast<std::size_t>(std::stoull(std::string(bytes.substr(space + 1, newline - space - 1))));
            const auto name = std::string(bytes.substr(0, space)); bytes.remove_prefix(newline + 1);
            CHECK(size < bytes.size() && bytes[size] == '\n');
            result.emplace_back(name, std::string(bytes.substr(0, size))); bytes.remove_prefix(size + 1);
        }
        return result;
    };
    const std::string header = "WLS-CONTEXT/1\n"; CHECK(original.stable_prefix.starts_with(header));
    std::map<std::string, std::string> sections;
    for (const auto& [name, bytes] : frames(std::string_view(original.stable_prefix).substr(header.size()))) sections.emplace(name, bytes);
    ContextInput context;
    context.role = original.role; context.model = original.model; context.adapter_version = original.adapter_version;
    context.protocol_version = original.protocol_version; context.system_prompt = sections.at("system");
    context.role_prompt = sections.at("role_prompt"); context.function_prompt = sections.at("function");
    context.environment_card = sections.at("environment"); context.approved_rules = sections.at("rules");
    context.project_snapshot = sections.at("project");
    const auto tools = parse_json_strict(sections.at("tools"));
    for (const auto& tool : tools.as_array()) context.tools.push_back({text(tool, "name"), text(tool, "version"), tool.at("definition")});
    const auto dependencies = parse_json_strict(sections.at("dependencies"));
    for (const auto& dependency : dependencies.as_array()) context.dependencies.push_back({text(dependency, "name"), text(dependency, "version"), text(dependency, "content_sha256")});
    for (const auto& [name, bytes] : frames(original.dynamic_suffix)) {
        if (name == "task_contract") {
            auto contract = parse_json_strict(bytes); contract.as_object()["workflow_action"] = value.captured.workflow_action;
            context.task_contract = canonical_json(contract);
        } else {
            CHECK(name == "message"); const auto message = parse_json_strict(bytes);
            context.messages.push_back({text(message, "role"), text(message, "content")});
        }
    }
    value.captured.request.context = ContextCompiler().compile(context);
    CHECK(value.captured.request.context.prefix_sha256 == original.prefix_sha256);
    value.reservation = reservation_for(value.captured); value.association.workflow_action = value.captured.workflow_action;
}
struct Fixture {
    test::TempDir directory;
    std::filesystem::path path{directory.path / "store.sqlite"};
    SchemaRegistry schemas{resources().schema_documents()};
    std::unique_ptr<Store> store{std::make_unique<Store>(path, MigrationRegistry(resources()))};
    std::string id;
    explicit Fixture(const EventDriverLimits& limits = {}) { id = admit(limits); }
    std::string admit(const EventDriverLimits& limits = {}) {
        const auto request = text(store->admit(input()), "request_id");
        store->enable_event_driver(store->host_actor(Role::Root, request), limits, new_id("enable")); return request;
    }
    Json task(std::string_view node = "alpha") const {
        for (const auto& value : store->tasks(id)) if (value.at("node_id").as_string() == node) return value;
        throw std::runtime_error("task missing");
    }
    StoreActor actor(Role role, std::string_view node = "alpha") const {
        if (role == Role::Root || role == Role::Dispatcher) return store->host_actor(role, id);
        return store->host_actor(role, id, text(task(node), "team_id"));
    }
    Bound capture(Role role, std::string_view node = "alpha", std::int64_t index = 1, bool amendment = false) {
        Bound value;
        value.claim = store->claim_event_batch(actor(role, node));
        CHECK(!text(value.claim, "claim_id").empty());
        OfflineRuntime runtime(*store, schemas);
        value.captured = runtime.capture_call(id, role, role == Role::Dispatcher ? "" : text(task(node), "task_id"), index, {}, amendment);
        value.reservation = reservation_for(value.captured);
        value.association.claim_id = text(value.claim, "claim_id"); value.association.generation = number(value.claim, "generation");
        value.association.expected_state_version = value.captured.expected_state_version;
        value.association.workflow_action = value.captured.workflow_action;
        for (const auto& item : value.claim.at("items").as_array())
            if (item.at("event_requirements_revision") == value.captured.request.binding.requirements_revision)
                value.association.selected_ids.push_back(number(item, "inbox_id"));
        return value;
    }
    void reserve(Bound& value) { value.link = store->reserve_event_call(value.captured.actor, value.captured.request.binding, value.reservation, value.association); }
    ModelResponse observe(Bound& value, std::string tool, Json arguments, bool dispatch = true) {
        if (dispatch) CHECK(store->claim_event_dispatch(value.captured.actor, text(value.link, "link_id")).at("dispatch_claimed") == true);
        FakeModelStep step;
        step.role = value.captured.request.role; step.role_call_index = value.captured.request.role_call_index;
        step.tool_calls = {{"original-tool", std::move(tool), std::move(arguments)}};
        step.usage.input_tokens = 20; step.usage.output_tokens = 10; step.usage.cache_read_tokens = 0;
        step.usage.cache_write_tokens = 0; step.usage.cost_micro_usd = 0;
        FakeModel model({step}, value.captured.request.model.model);
        return model.complete(value.captured.request);
    }
    Json complete(Bound& value, const std::string& tool, const Json& arguments) {
        reserve(value); const auto response = observe(value, tool, arguments);
        store->settle_event_observation(text(value.link, "link_id"), response);
        return store->commit_event_action_and_finish(value.captured.actor, text(value.link, "link_id"), schemas);
    }
    void plan() { auto value = capture(Role::Dispatcher); CHECK(complete(value, "submit_plan", plan_body()).at("state") == "CLOSED"); }
    void ready(std::string_view node = "alpha") {
        auto value = capture(Role::TeamLead, node); CHECK(complete(value, "issue_work_order", order_body()).at("state") == "CLOSED");
        const auto current = task(node); store->acknowledge(actor(Role::Worker, node), store->binding(text(current, "task_id")), number(current, "state_version"), new_id("ack"));
    }
};

void opt_in_and_legacy_bypass_are_explicit() {
    Fixture f;
    CHECK(f.store->persistent_events_enabled(f.id));
    const auto actor = f.actor(Role::Dispatcher);
    expect(ErrorCode::Unsupported, [&] { f.store->claim_events(actor); });
    expect(ErrorCode::Unsupported, [&] { f.store->apply_independent_plan(actor, number(f.store->request(f.id), "state_version"), 1, plan_body(), Object{}, new_id("bypass")); });
    auto value = f.capture(Role::Dispatcher);
    expect(ErrorCode::Unsupported, [&] { f.store->reserve_attempt(actor, value.captured.request.binding, value.reservation); });
    expect(ErrorCode::Unsupported, [&] { f.store->finish_events(actor, value.association.claim_id, value.association.generation, value.association.selected_ids); });
    CHECK(f.store->budget(f.id).at("attempts") == 0);
    CHECK(f.store->tasks(f.id).empty());
    const auto released = f.store->release_event_batch(actor, value.association.claim_id, value.association.generation);
    CHECK(released.at("completed_ids").as_array().empty());
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_claims") == 1);
}
void two_requests_two_nodes_commit_original_proofs() {
    Fixture f;
    const auto first = f.id; const auto second = f.admit();
    for (const auto& id : {first, second}) {
        f.id = id; f.plan();
        for (const auto* node : {"alpha", "beta"}) {
            f.ready(node); auto value = f.capture(Role::Worker, node);
            const auto closed = f.complete(value, "note", Object{{"text", "same text, distinct original task"}});
            CHECK(closed.at("completed_ids").as_array().size() == 2);
            const auto before = f.store->budget(id);
            CHECK(f.store->commit_event_action_and_finish(value.captured.actor, text(value.link, "link_id"), f.schemas) == closed);
            CHECK(f.store->budget(id) == before);
            CHECK(f.store->claim_event_dispatch(value.captured.actor, text(value.link, "link_id")).at("dispatch_claimed") == false);
        }
        CHECK(f.store->request_event_links(id).size() == 5);
    }
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM ops WHERE status='SUCCEEDED'") == 4);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM model_tool_feedback") == 10);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_receipts WHERE kind='CLOSE'") == 10);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_open_items") == 0);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'") == 10);
}
void partial_capture_preserves_unselected_and_new_feedback_id() {
    Fixture f; f.plan(); f.ready(); auto value = f.capture(Role::Worker);
    CHECK(value.association.selected_ids.size() == 2);
    const auto omitted = value.association.selected_ids.back(); value.association.selected_ids.resize(1);
    const auto closed = f.complete(value, "note", Object{{"text", "only the selected original instruction"}});
    CHECK(closed.at("completed_ids").as_array().size() == 1);
    CHECK(closed.at("released_ids") == Array{omitted});
    CHECK(closed.at("dispositions").as_array()[1].at("disposition") == "NOT_SELECTED");
    const auto pending = f.store->inbox(f.actor(Role::Worker));
    CHECK(pending.size() == 2);
    CHECK(std::any_of(pending.begin(), pending.end(), [omitted](const Json& item) { return item.at("inbox_id") == omitted && item.at("status") == "PENDING"; }));
    CHECK(std::any_of(pending.begin(), pending.end(), [&](const Json& item) {
        return item.at("type") == "tool_feedback" && item.at("inbox_id").as_int64() > value.association.selected_ids.front() && item.at("status") == "PENDING";
    }));
}
void association_mutations_and_exact_replay_do_not_spend() {
    Fixture f; auto value = f.capture(Role::Dispatcher);
    const auto before = f.store->budget(f.id);
    for (const auto& mutate : std::vector<std::function<void(EventCallAssociation&)>>{
            [](auto& a) { ++a.generation; }, [](auto& a) { ++a.expected_state_version; },
            [](auto& a) { a.selected_ids.push_back(a.selected_ids.front()); }, [](auto& a) { a.selected_ids = {INT64_MAX}; },
            [](auto& a) { a.workflow_action = Object{{"kind", "foreign"}}; }}) {
        auto changed = value.association; mutate(changed);
        reject([&] { f.store->reserve_event_call(value.captured.actor, value.captured.request.binding, value.reservation, changed); });
        CHECK(f.store->budget(f.id) == before); CHECK(scalar(f.path, "SELECT COUNT(*) FROM call_attempts") == 0);
    }
    const auto original = value.captured.request.binding;
    for (const auto& mutate : std::vector<std::function<void(StoreActor&)>>{
            [](auto& a) { a.principal_id += "-foreign"; }, [](auto& a) { a.role = Role::Root; },
            [](auto& a) { a.request_id = new_id("req"); }, [](auto& a) { a.task_id = new_id("tsk"); },
            [](auto& a) { a.team_id = new_id("team"); }, [](auto& a) { ++a.dispatcher_epoch; },
            [](auto& a) { ++a.execution_epoch; }, [](auto& a) { a.writer_session = "foreign-session"; }}) {
        auto changed = value.captured.actor; mutate(changed);
        reject([&] { f.store->reserve_event_call(changed, original, value.reservation, value.association); });
        CHECK(f.store->budget(f.id) == before); CHECK(scalar(f.path, "SELECT COUNT(*) FROM call_attempts") == 0);
    }
    for (const auto& mutate : std::vector<std::function<void(ExecutionBinding&)>>{
            [](auto& b) { b.request_id = new_id("req"); }, [](auto& b) { b.task_id = new_id("tsk"); },
            [](auto& b) { b.team_id = new_id("team"); }, [](auto& b) { ++b.requirements_revision; },
            [](auto& b) { ++b.plan_revision; }, [](auto& b) { ++b.task_revision; }, [](auto& b) { ++b.instruction_revision; },
            [](auto& b) { ++b.dispatcher_epoch; }, [](auto& b) { ++b.execution_epoch; }, [](auto& b) { ++b.policy_epoch; }}) {
        auto changed = original; mutate(changed);
        reject([&] { f.store->reserve_event_call(value.captured.actor, changed, value.reservation, value.association); });
        CHECK(f.store->budget(f.id) == before);
    }
    f.reserve(value); const auto once = f.store->budget(f.id);
    CHECK(f.store->reserve_event_call(value.captured.actor, original, value.reservation, value.association) == value.link);
    CHECK(f.store->budget(f.id) == once);
    expect(ErrorCode::NotReady, [&] { f.store->release_event_batch(value.captured.actor, value.association.claim_id, value.association.generation); });
}
void dispatch_and_observation_replays_keep_original_identity() {
    Fixture f; auto value = f.capture(Role::Dispatcher); f.reserve(value);
    const auto response = f.observe(value, "submit_plan", plan_body());
    CHECK(f.store->claim_event_dispatch(value.captured.actor, text(value.link, "link_id")).at("dispatch_claimed") == false);
    auto wrong = response; ++wrong.role_call_index;
    expect(ErrorCode::Stale, [&] { f.store->settle_event_observation(text(value.link, "link_id"), wrong); });
    CHECK(f.store->attempt(value.reservation.attempt_id).at("settlement_sha256").is_null());
    const auto settled = f.store->settle_event_observation(text(value.link, "link_id"), response);
    const auto budget = f.store->budget(f.id);
    CHECK(f.store->settle_event_observation(text(value.link, "link_id"), response) == settled);
    CHECK(f.store->budget(f.id) == budget);
    wrong = response; wrong.text = "changed original";
    expect(ErrorCode::Conflict, [&] { f.store->settle_event_observation(text(value.link, "link_id"), wrong); });
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_receipts WHERE kind='DISPATCH'") == 1);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_receipts WHERE kind='SETTLEMENT'") == 1);
}
void unknown_observation_retains_id_and_budget_fences() {
    Fixture f; auto value = f.capture(Role::Dispatcher); f.reserve(value);
    auto response = f.observe(value, "submit_plan", plan_body()); response.usage = {};
    CHECK(f.store->settle_event_observation(text(value.link, "link_id"), response).at("state") == "HOLD_UNKNOWN");
    expect(ErrorCode::UnknownOutcome, [&] { f.store->commit_event_action_and_finish(value.captured.actor, text(value.link, "link_id"), f.schemas); });
    CHECK(f.store->tasks(f.id).empty());
    CHECK(f.store->event_link_status(text(value.link, "link_id")).at("state") == "HOLD_UNKNOWN");
    CHECK(f.store->claim_event_batch(f.actor(Role::Dispatcher)).at("items").as_array().empty());
    CHECK(f.store->budget(f.id).at("unknown_input").as_int64() > 0);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_open_items") == 1);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_receipts WHERE kind='CLOSE'") == 0);
    expect(ErrorCode::NotReady, [&] { f.store->commit_event_action_and_finish(value.captured.actor, text(value.link, "link_id"), f.schemas); });
}
void recovery_never_reissues_or_applies_old_links() {
    for (int boundary = 0; boundary != 3; ++boundary) {
        Fixture f; auto value = f.capture(Role::Dispatcher); f.reserve(value);
        ModelResponse response;
        if (boundary >= 1) response = f.observe(value, "submit_plan", plan_body());
        if (boundary == 2) f.store->settle_event_observation(text(value.link, "link_id"), response);
        const auto envelope = f.store->attempt(value.reservation.attempt_id).at("request_envelope");
        f.store.reset();
        {
            Store reader(f.path, MigrationRegistry(resources()), StoreOpenMode::ReadOnly);
            CHECK(reader.event_link_status(text(value.link, "link_id")).at("state") != "CLOSED");
            CHECK(reader.attempt(value.reservation.attempt_id).at("request_envelope") == envelope);
        }
        f.store = std::make_unique<Store>(f.path, MigrationRegistry(resources()), StoreOpenMode::Takeover, StoreRecoveryScope::request(f.id));
        expect(ErrorCode::Stale, [&] { f.store->claim_event_dispatch(value.captured.actor, text(value.link, "link_id")); });
        expect(ErrorCode::Stale, [&] { f.store->commit_event_action_and_finish(value.captured.actor, text(value.link, "link_id"), f.schemas); });
        if (boundary >= 1) f.store->settle_event_observation(text(value.link, "link_id"), response);
        CHECK(f.store->attempt(value.reservation.attempt_id).at("request_envelope") == envelope);
        CHECK(f.store->tasks(f.id).empty());
        CHECK(f.store->claim_event_batch(f.actor(Role::Dispatcher)).at("items").as_array().empty());
        CHECK(scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'") == (boundary >= 1 ? 1 : 0));
    }
}
void t1_t2_t3_failure_rolls_back_both_sides() {
    ::test::persistent_events::StorageFault fault;
    Fixture f; auto value = f.capture(Role::Dispatcher); const auto budget = f.store->budget(f.id);
    sql(f.path, "CREATE TRIGGER fail_link BEFORE INSERT ON n5_event_links BEGIN SELECT n5_test_storage_fault(); END");
    expect(ErrorCode::Storage, [&] { f.reserve(value); });
    CHECK(f.store->budget(f.id) == budget); CHECK(scalar(f.path, "SELECT COUNT(*) FROM call_attempts") == 0);
    sql(f.path, "DROP TRIGGER fail_link"); f.reserve(value);
    sql(f.path, "CREATE TRIGGER fail_dispatch BEFORE INSERT ON n5_event_receipts WHEN NEW.kind='DISPATCH' BEGIN SELECT n5_test_storage_fault(); END");
    expect(ErrorCode::Storage, [&] { f.store->claim_event_dispatch(value.captured.actor, text(value.link, "link_id")); });
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'") == 0);
    sql(f.path, "DROP TRIGGER fail_dispatch"); const auto response = f.observe(value, "submit_plan", plan_body());
    sql(f.path, "CREATE TRIGGER fail_settlement BEFORE INSERT ON n5_event_receipts WHEN NEW.kind='SETTLEMENT' BEGIN SELECT n5_test_storage_fault(); END");
    expect(ErrorCode::Storage, [&] { f.store->settle_event_observation(text(value.link, "link_id"), response); });
    CHECK(f.store->attempt(value.reservation.attempt_id).at("settlement_sha256").is_null());
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='settle_attempt'") == 0);
    sql(f.path, "DROP TRIGGER fail_settlement"); f.store->settle_event_observation(text(value.link, "link_id"), response);
}
void t4_faults_never_commit_note_without_feedback_and_proof() {
    ::test::persistent_events::StorageFault fault;
    for (const auto* trigger : {
            "CREATE TRIGGER fail_tail BEFORE INSERT ON events WHEN NEW.type='start_op' BEGIN SELECT n5_test_storage_fault(); END",
            "CREATE TRIGGER fail_tail BEFORE INSERT ON model_tool_feedback BEGIN SELECT n5_test_storage_fault(); END",
            "CREATE TRIGGER fail_tail BEFORE INSERT ON n5_event_receipts WHEN NEW.kind='CLOSE' BEGIN SELECT n5_test_storage_fault(); END"}) {
        Fixture f; f.plan(); f.ready(); auto value = f.capture(Role::Worker); f.reserve(value);
        const auto response = f.observe(value, "note", Object{{"text", "must roll back atomically"}});
        f.store->settle_event_observation(text(value.link, "link_id"), response);
        const auto budget = f.store->budget(f.id); const auto task = f.task(); const auto feedbacks = scalar(f.path, "SELECT COUNT(*) FROM model_tool_feedback");
        const auto raw_task = f.store->task(text(task, "task_id"));
        Array joined_only, raw_only;
        for (const auto& field : task.as_object()) if (!raw_task.as_object().contains(field.key())) joined_only.emplace_back(field.key());
        for (const auto& field : raw_task.as_object()) if (!task.as_object().contains(field.key())) raw_only.emplace_back(field.key());
        std::cout << "FIXTURE T4 task views joined_only=" << canonical_json(joined_only)
                  << " raw_only=" << canonical_json(raw_only) << std::endl;
        sql(f.path, trigger);
        expect(ErrorCode::Storage, [&] { f.store->commit_event_action_and_finish(value.captured.actor, text(value.link, "link_id"), f.schemas); });
        CHECK(scalar(f.path, "SELECT COUNT(*) FROM ops") == 0);
        CHECK(scalar(f.path, "SELECT COUNT(*) FROM model_tool_feedback") == feedbacks);
        CHECK(f.store->task(text(task, "task_id")) == raw_task); CHECK(f.task() == task);
        CHECK(f.store->budget(f.id) == budget);
        CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_open_items") == 2);
        sql(f.path, "DROP TRIGGER fail_tail");
        f.store->settle_event_observation(text(value.link, "link_id"), response); // evidence-only flush
        expect(ErrorCode::NotReady, [&] { f.store->commit_event_action_and_finish(value.captured.actor, text(value.link, "link_id"), f.schemas); });
        CHECK(scalar(f.path, "SELECT COUNT(*) FROM ops") == 0);
    }
}
void partial_amendment_defers_each_omitted_subject() {
    Fixture f; f.plan();
    const auto root = f.actor(Role::Root); const auto before_request = f.store->request(f.id); const auto command = new_id("amend");
    f.store->amend(root, number(before_request, "state_version"), "preserve fixture observations", input().constraint_catalog, command);
    auto first = f.capture(Role::Dispatcher, "alpha", 2, true);
    CHECK(first.association.selected_ids.size() == 2);
    const auto beta = f.task("beta");
    const auto result = f.complete(first, "submit_amendment_plan", Object{{"plan", plan_body()},
        {"dispositions", Object{{"alpha", Object{{"disposition", "keep"}, {"rationale", "explicit original alpha decision"}}}}}});
    CHECK(result.at("completed_ids").as_array().size() == 1);
    CHECK(result.at("dispositions").as_array()[1].at("disposition") == "DEFERRED_NO_EFFECT");
    CHECK(f.task("beta") == beta); const auto alpha = f.task("alpha");
    const auto original_control_events = scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='amend'");
    f.store->amend(root, number(before_request, "state_version"), "preserve fixture observations", input().constraint_catalog, command);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='amend'") == original_control_events);
    auto second = f.capture(Role::Dispatcher, "alpha", 3, true);
    CHECK(second.association.selected_ids.size() == 1);
    const auto closed = f.complete(second, "resolve_amendment", Object{{"dispositions", Object{{"beta", Object{{"disposition", "keep"}, {"rationale", "explicit original beta decision"}}}}}});
    CHECK(closed.at("completed_ids").as_array().size() == 1); CHECK(f.task("alpha") == alpha);
    CHECK(f.store->budget(f.id).at("attempts") == 3);
}
void nonpending_amendment_selection_is_rejected_before_reservation() {
    Fixture f; f.plan();
    f.store->amend(f.actor(Role::Root), number(f.store->request(f.id), "state_version"),
        "preserve fixture observations", input().constraint_catalog, new_id("amend"));
    auto first = f.capture(Role::Dispatcher, "alpha", 2, true);
    CHECK(first.association.selected_ids.size() == 2);
    const auto unselected_alpha = first.association.selected_ids.front();
    // A genuine model decision makes alpha current, while its original source
    // was outside S and therefore remains pending. No success rows are forged.
    first.association.selected_ids.erase(first.association.selected_ids.begin());
    const auto closed = f.complete(first, "submit_amendment_plan", Object{{"plan", plan_body()},
        {"dispositions", Object{{"alpha", Object{{"disposition", "keep"}, {"rationale", "explicit original alpha decision"}}}}}});
    CHECK(closed.at("completed_ids").as_array().empty());
    auto next = f.capture(Role::Dispatcher, "alpha", 3, true);
    CHECK(next.captured.workflow_action.at("target").at("pending_node_ids") == Array{"beta"});
    // A host-malformed capture includes current alpha as a potential task but
    // preserves the original pending set containing only beta. The selected
    // source is in C and target.tasks; only pending membership rejects it.
    next.captured.workflow_action.at("target").at("tasks").as_array().push_back(f.task("alpha"));
    recompile_host_action(next);
    next.association.selected_ids = {unselected_alpha};
    const auto budget = f.store->budget(f.id); const auto links = f.store->request_event_links(f.id).size();
    const auto attempts = scalar(f.path, "SELECT COUNT(*) FROM call_attempts");
    expect(ErrorCode::Unauthorized, [&] { f.reserve(next); });
    CHECK(f.store->budget(f.id) == budget); CHECK(f.store->request_event_links(f.id).size() == links);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM call_attempts") == attempts);
    f.store->release_event_batch(next.captured.actor, next.association.claim_id, next.association.generation);
}
void amendment_task_cas_is_checked_before_reservation_and_dispatch() {
    for (const bool after_reservation : {false, true}) {
        Fixture f; f.plan();
        f.store->amend(f.actor(Role::Root), number(f.store->request(f.id), "state_version"),
            "preserve fixture observations", input().constraint_catalog, new_id("amend"));
        auto amendment = f.capture(Role::Dispatcher, "alpha", 2, true);
        f.complete(amendment, "submit_amendment_plan", Object{{"plan", plan_body()},
            {"dispositions", Object{{"alpha", Object{{"disposition", "keep"}, {"rationale", "original alpha decision"}}}}}});
        auto instruction = f.capture(Role::TeamLead);
        f.complete(instruction, "issue_work_order", order_body());
        auto next = f.capture(Role::Dispatcher, "alpha", 3, true);
        // A normal same-plan capture contains only pending beta, which cannot
        // acknowledge an old requirement. Deliberately add current alpha to
        // the host target so the supported owner ack can advance one captured
        // task CAS without changing the Dispatcher request CAS.
        next.captured.workflow_action.at("target").at("tasks").as_array().push_back(f.task("alpha"));
        recompile_host_action(next);
        const auto request = f.store->request(f.id);
        if (after_reservation) f.reserve(next);
        const auto budget = f.store->budget(f.id); const auto alpha = f.task("alpha");
        f.store->acknowledge(f.actor(Role::Worker), f.store->binding(text(alpha, "task_id")), number(alpha, "state_version"), new_id("ack"));
        CHECK(f.store->request(f.id) == request);
        if (after_reservation) {
            expect(ErrorCode::Conflict, [&] { f.store->claim_event_dispatch(next.captured.actor, text(next.link, "link_id")); });
            CHECK(f.store->attempt(next.reservation.attempt_id).at("status") == "PENDING");
            CHECK(scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'") == 3);
            CHECK(f.store->event_link_status(text(next.link, "link_id")).at("state") == "RESERVED");
        } else {
            expect(ErrorCode::Conflict, [&] { f.reserve(next); });
            CHECK(scalar(f.path, "SELECT COUNT(*) FROM call_attempts") == 3);
            f.store->release_event_batch(next.captured.actor, next.association.claim_id, next.association.generation);
        }
        CHECK(f.store->budget(f.id) == budget);
        // No FakeModel observation/callback is requested for next in either
        // branch; T2 cannot return a new dispatch permission.
    }
}
void lifetime_limits_and_tail_capacity_reject_before_attempts() {
    Fixture claims(EventDriverLimits{64, 1, 16 * 1024 * 1024});
    auto claim = claims.store->claim_event_batch(claims.actor(Role::Dispatcher));
    claims.store->release_event_batch(claims.actor(Role::Dispatcher), text(claim, "claim_id"), number(claim, "generation"));
    expect(ErrorCode::Quota, [&] { claims.store->claim_event_batch(claims.actor(Role::Dispatcher)); });
    CHECK(scalar(claims.path, "SELECT COUNT(*) FROM n5_event_claims") == 1);
    Fixture bytes(EventDriverLimits{64, 128, 65536}); auto value = bytes.capture(Role::Dispatcher);
    expect(ErrorCode::Quota, [&] { bytes.reserve(value); });
    CHECK(bytes.store->budget(bytes.id).at("attempts") == 0);
    bytes.store->release_event_batch(value.captured.actor, value.association.claim_id, value.association.generation);
    Fixture links(EventDriverLimits{1, 128, 16 * 1024 * 1024}); links.plan(); auto lead = links.capture(Role::TeamLead);
    const auto budget = links.store->budget(links.id);
    expect(ErrorCode::Quota, [&] { links.reserve(lead); }); CHECK(links.store->budget(links.id) == budget);
    links.store->release_event_batch(lead.captured.actor, lead.association.claim_id, lead.association.generation);
    Fixture tail; auto plan = tail.capture(Role::Dispatcher); const auto state = tail.store->capacity_status(tail.id);
    const auto fill = state.at("limits").at("business_events").as_int64() - state.at("counts").at("business_events").as_int64() - 2;
    CHECK(fill > 0);
    sql(tail.path, "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<" + std::to_string(fill) +
        ") INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) "
        "SELECT 'tail-'||x,'" + tail.id + "','fixture','runtime','tail-'||x,printf('%064d',0),'{}','{}','fixture' FROM n");
    expect(ErrorCode::NotReady, [&] { tail.reserve(plan); }); CHECK(tail.store->budget(tail.id).at("attempts") == 0);
}
void corrupt_open_projection_fails_closed_and_facts_are_immutable() {
    Fixture f; auto value = f.capture(Role::Dispatcher); f.reserve(value);
    for (const auto* table : {"n5_event_requests", "n5_event_claims", "n5_event_links", "n5_event_items", "n5_event_receipts"}) {
        sqlite3* db{}; CHECK(sqlite3_open(f.path.c_str(), &db) == SQLITE_OK);
        const auto statement = "DELETE FROM " + std::string(table);
        CHECK(sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK); sqlite3_close(db);
    }
    sql(f.path, "DELETE FROM n5_event_open_items");
    expect(ErrorCode::Corrupt, [&] { f.store->event_link_status(text(value.link, "link_id")); });
    expect(ErrorCode::Corrupt, [&] { f.store->claim_event_batch(f.actor(Role::Dispatcher)); });
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'") == 0);
}
void same_transaction_config_change_rejects_and_rolls_back_dispatch() {
    Fixture f; auto value = f.capture(Role::Dispatcher); f.reserve(value);
    const auto id = text(value.link, "link_id");
    const auto original_request = f.store->request(f.id);
    const auto original_attempt = f.store->attempt(value.reservation.attempt_id);
    const auto budget = f.store->budget(f.id);
    // Synthetic database corruption after the original validation, within T2.
    // The unmodified snapshot hash must never validate these different bytes.
    sql(f.path, "CREATE TRIGGER n5_test_config_change AFTER INSERT ON n5_event_receipts "
        "WHEN NEW.kind='DISPATCH' BEGIN UPDATE requests SET config_snapshot="
        "json_set(config_snapshot,'$.system.limits.max_active_workers',9),"
        "state_version=state_version+1 WHERE request_id=(SELECT request_id FROM n5_event_links WHERE link_id=NEW.link_id); END");
    expect(ErrorCode::Corrupt, [&] { f.store->claim_event_dispatch(value.captured.actor, id); });
    CHECK(f.store->request(f.id) == original_request);
    CHECK(f.store->attempt(value.reservation.attempt_id) == original_attempt);
    CHECK(f.store->budget(f.id) == budget);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'") == 0);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_receipts WHERE kind='DISPATCH'") == 0);
    CHECK(f.store->event_link_status(id).at("state") == "RESERVED");
    sql(f.path, "DROP TRIGGER n5_test_config_change");
    CHECK(f.store->claim_event_dispatch(value.captured.actor, id).at("dispatch_claimed") == true);
    CHECK(f.store->claim_event_dispatch(value.captured.actor, id).at("dispatch_claimed") == false);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'") == 1);
}
void note_start_revalidates_config_and_rolls_back_the_original_tail() {
    ::test::persistent_events::StorageFault fault;
    Fixture f; f.plan(); f.ready(); auto value = f.capture(Role::Worker); f.reserve(value);
    const auto response = f.observe(value, "note", Object{{"text", "retain the original note boundary"}});
    const auto id = text(value.link, "link_id");
    f.store->settle_event_observation(id, response);
    const auto sibling = f.admit();
    const auto sibling_request = f.store->request(sibling);
    const auto request = f.store->request(f.id);
    CHECK(request.at("config_snapshot").at("system").at("limits").at("max_active_workers") == 8);
    const auto task = f.task();
    const auto original_task = f.store->task(text(task, "task_id"));
    const auto attempt = f.store->attempt(value.reservation.attempt_id);
    const auto budget = f.store->budget(f.id);
    const auto inbox = f.store->request_inbox(f.id);
    const auto link = f.store->event_link_status(id);
    const auto events = scalar(f.path, "SELECT COUNT(*) FROM events");
    const auto feedback = scalar(f.path, "SELECT COUNT(*) FROM model_tool_feedback");
    // Change bytes only after prepare's original fact is inserted. A start
    // that ignored the fresh configuration would hit SQLITE_IOERR instead of
    // the required Corrupt refusal, before any later link postcheck.
    sql(f.path, "CREATE TRIGGER n5_test_note_config AFTER INSERT ON events "
        "WHEN NEW.type='prepare_op' BEGIN UPDATE requests SET config_snapshot="
        "json_set(config_snapshot,'$.system.limits.max_active_workers',9),"
        "state_version=state_version+1 WHERE request_id=NEW.request_id; END");
    sql(f.path, "CREATE TRIGGER n5_test_note_forbidden_start BEFORE UPDATE ON ops "
        "WHEN NEW.status='RUNNING' BEGIN SELECT n5_test_storage_fault(); END");
    expect(ErrorCode::Corrupt, [&] { f.store->commit_event_action_and_finish(value.captured.actor, id, f.schemas); });
    CHECK(f.store->request(f.id) == request);
    CHECK(f.store->task(text(task, "task_id")) == original_task); CHECK(f.task() == task);
    CHECK(f.store->attempt(value.reservation.attempt_id) == attempt);
    CHECK(f.store->budget(f.id) == budget);
    CHECK(f.store->request_inbox(f.id) == inbox);
    CHECK(f.store->event_link_status(id) == link);
    CHECK(f.store->request(sibling) == sibling_request);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM ops") == 0);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM model_tool_feedback") == feedback);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events") == events);
    sql(f.path, "DROP TRIGGER n5_test_note_config");
    sql(f.path, "DROP TRIGGER n5_test_note_forbidden_start");
    expect(ErrorCode::NotReady, [&] { f.store->commit_event_action_and_finish(value.captured.actor, id, f.schemas); });
    CHECK(f.store->event_link_status(id) == link);
    CHECK(f.store->budget(f.id) == budget);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM ops") == 0);
}
void later_call_revalidates_config_after_a_successful_read() {
    Fixture f; auto value = f.capture(Role::Dispatcher); f.reserve(value);
    const auto id = text(value.link, "link_id");
    CHECK(f.store->event_link_status(id).at("state") == "RESERVED");
    const auto budget = f.store->budget(f.id);
    const auto attempt = f.store->attempt(value.reservation.attempt_id);
    sql(f.path, "UPDATE requests SET config_snapshot=json_set(config_snapshot,"
        "'$.system.limits.max_active_workers',9),state_version=state_version+1");
    expect(ErrorCode::Corrupt, [&] { f.store->event_link_status(id); });
    expect(ErrorCode::Corrupt, [&] { f.store->request_event_links(f.id); });
    expect(ErrorCode::Corrupt, [&] { f.store->claim_event_dispatch(value.captured.actor, id); });
    CHECK(f.store->budget(f.id) == budget);
    CHECK(f.store->attempt(value.reservation.attempt_id) == attempt);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'") == 0);
}
void control_after_settlement_invalidates_business_but_keeps_original_response() {
    for (const bool cancel : {false, true}) {
        Fixture f; auto value = f.capture(Role::Dispatcher); f.reserve(value);
        const auto response = f.observe(value, "submit_plan", plan_body());
        f.store->settle_event_observation(text(value.link, "link_id"), response);
        const auto original = f.store->attempt(value.reservation.attempt_id);
        const auto root = f.actor(Role::Root); const auto version = number(f.store->request(f.id), "state_version");
        const auto key = new_id("control");
        if (cancel) f.store->cancel_request(root, version, key);
        else f.store->amend(root, version, "preserve fixture observations", input().constraint_catalog, key);
        const auto events = f.store->events(f.id).size();
        if (cancel) f.store->cancel_request(root, version, key);
        else f.store->amend(root, version, "preserve fixture observations", input().constraint_catalog, key);
        CHECK(f.store->events(f.id).size() == events);
        expect(ErrorCode::Stale, [&] { f.store->commit_event_action_and_finish(value.captured.actor, text(value.link, "link_id"), f.schemas); });
        CHECK(f.store->tasks(f.id).empty());
        CHECK(f.store->attempt(value.reservation.attempt_id).at("response") == original.at("response"));
        CHECK(f.store->attempt(value.reservation.attempt_id).at("settlement_sha256") == original.at("settlement_sha256"));
        CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_receipts WHERE kind='CLOSE'") == 0);
        CHECK(scalar(f.path, "SELECT COUNT(*) FROM n5_event_open_items") == 1);
    }
}
void structural_410_upgrade_retains_history_and_requires_maintenance() {
    test::TempDir directory; const auto path = directory.path / "legacy.sqlite";
    std::string id; Json original, snapshot;
    {
        Store old(path, MigrationRegistry(resources(), 410)); id = text(old.admit(input()), "request_id");
        original = old.request(id); snapshot = old.request_snapshot(id);
    }
    {
        Store reader(path, MigrationRegistry(resources()), StoreOpenMode::ReadOnly);
        CHECK(reader.diagnostics().at("user_version") == 410); CHECK(reader.request(id) == original);
    }
    expect(ErrorCode::Unsupported, [&] { Store refused(path, MigrationRegistry(resources()), StoreOpenMode::Takeover, StoreRecoveryScope::request(id)); });
    expect(ErrorCode::Unsupported, [&] { Store refused(path, MigrationRegistry(resources()), StoreOpenMode::Takeover, StoreRecoveryScope::admission()); });
    {
        Store upgraded(path, MigrationRegistry(resources()));
        CHECK(upgraded.diagnostics().at("user_version") == 411); CHECK(!upgraded.persistent_events_enabled(id));
        CHECK(upgraded.request(id).at("config_snapshot") == original.at("config_snapshot"));
        CHECK(upgraded.request_snapshot(id).at("texts") == snapshot.at("texts"));
        CHECK(upgraded.request_event_links(id).empty()); CHECK(upgraded.event_runtime_enabled());
        expect(ErrorCode::NotReady, [&] { upgraded.enable_event_driver(upgraded.host_actor(Role::Root, id), {}, new_id("enable")); });
    }
    expect(ErrorCode::Unsupported, [&] { Store legacy(path, MigrationRegistry(resources(), 410), StoreOpenMode::ReadOnly); });
    CHECK(scalar(path, "PRAGMA user_version") == 411);
}
void failed_013_and_wrong_asset_leave_original_version_and_history() {
    test::TempDir directory; const auto path = directory.path / "migration.sqlite";
    const MigrationRegistry registered(resources());
    {
        Store original(path, MigrationRegistry(resources(), 410));
        CHECK(original.diagnostics().at("user_version") == 410);
    }
    const auto open = [&](StoreOpenMode mode, const std::string& sql013) {
        return std::make_unique<Store>(path, registered.base_schema_sql(), registered.migration_sql(402), mode,
            registered.migration_sql(403), registered.migration_sql(404), registered.migration_sql(405),
            registered.migration_sql(406), registered.migration_sql(407), StoreRecoveryScope{}, registered.migration_sql(408),
            registered.migration_sql(409), registered.migration_sql(410), sql013);
    };
    const auto valid = std::string(registered.migration_sql(411));
    expect(ErrorCode::Storage, [&] { (void)open(StoreOpenMode::Takeover, valid + "\nINSERT INTO deliberately_absent VALUES(1);\n"); });
    CHECK(scalar(path, "PRAGMA user_version") == 410);
    CHECK(scalar(path, "SELECT COUNT(*) FROM sqlite_master WHERE name='n5_event_requests'") == 0);
    {
        auto upgraded = open(StoreOpenMode::Takeover, valid);
        CHECK(upgraded->diagnostics().at("user_version") == 411);
    }
    expect(ErrorCode::Corrupt, [&] { (void)open(StoreOpenMode::ReadOnly, valid + "\n-- changed original asset\n"); });
    CHECK(scalar(path, "PRAGMA user_version") == 411);
}

#include "persistent_event_store_crash.inc"
} // namespace

int main() {
    return test::run({
        {"explicit opt-in rejects legacy reserve, business and finish bypasses", opt_in_and_legacy_bypass_are_explicit},
        {"two requests with two nodes retain original plan instruction ack note proof", two_requests_two_nodes_commit_original_proofs},
        {"partial capture leaves omitted and later feedback IDs pending", partial_capture_preserves_unselected_and_new_feedback_id},
        {"association and binding mutations roll back while exact reservation replay is free", association_mutations_and_exact_replay_do_not_spend},
        {"unique dispatch and observation replay retain the complete original identity", dispatch_and_observation_replays_keep_original_identity},
        {"UNKNOWN retains original budget and ID fences without another call", unknown_observation_retains_id_and_budget_fences},
        {"recovery at RESERVED DISPATCHED and SETTLED never reissues or reapplies", recovery_never_reissues_or_applies_old_links},
        {"T1 T2 T3 injected failures roll back both original and association facts", t1_t2_t3_failure_rolls_back_both_sides},
        {"T4 start feedback and proof faults cannot commit a partial note", t4_faults_never_commit_note_without_feedback_and_proof},
        {"partial AMEND individually defers omitted B and later bills a fresh B call", partial_amendment_defers_each_omitted_subject},
        {"AMEND nonpending subject in captured C is rejected before reservation or budget", nonpending_amendment_selection_is_rejected_before_reservation},
        {"original AMEND task CAS is checked before reservation and first dispatch", amendment_task_cas_is_checked_before_reservation_and_dispatch},
        {"claim link metadata and original event tail limits reject before billing", lifetime_limits_and_tail_capacity_reject_before_attempts},
        {"immutable facts and missing open projection fail closed", corrupt_open_projection_fails_closed_and_facts_are_immutable},
        {"changed config bytes inside dispatch reject and roll back the complete transaction", same_transaction_config_change_rejects_and_rolls_back_dispatch},
        {"note start revalidates changed config before dispatch and preserves the complete original tail", note_start_revalidates_config_and_rolls_back_the_original_tail},
        {"a later public call validates actual config bytes after a successful read", later_call_revalidates_config_after_a_successful_read},
        {"AMEND or cancel after settlement preserves response and rejects stale effects", control_after_settlement_invalidates_business_but_keeps_original_response},
        {"410 upgrade requires all-request maintenance and preserves legacy history", structural_410_upgrade_retains_history_and_requires_maintenance},
        {"failed013 and wrong immutable asset preserve original migration state", failed_013_and_wrong_asset_leave_original_version_and_history},
        {"process crashes W00 through W05 roll back whole claim reservation and dispatch steps", process_crash_claim_reservation_and_dispatch},
        {"process crashes W06 through W10 preserve actual dispatch callback and observation boundaries", process_crash_callback_and_observation},
        {"process crashes W11 through W15 preserve atomic note feedback and every captured ID", process_crash_atomic_action_tail},
        {"process crashes W16 retain committed AMEND or cancel and original observation", process_crash_control_after_observation},
        {"legacy process crash W19 preserves a real committed effect without feedback across upgrade", process_crash_legacy_feedback_gap_upgrade}
    });
}
