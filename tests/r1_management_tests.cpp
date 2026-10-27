#include "wls/management_context.hpp"
#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"
#include "live_test_support.hpp"

#include <openssl/evp.h>
#include <sqlite3.h>

#include <algorithm>
#include <exception>
#include <fstream>
#include <memory>

namespace {
using namespace wls::s1;
using wls::Role;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path source(WLS_SOURCE_DIR);

std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t num(const Json& value, std::string_view key) {
    const auto& number = value.at(key);
    return number.is_int64() ? number.as_int64() : static_cast<std::int64_t>(number.as_uint64());
}
Json fixture(const std::string& name) { return parse_json_strict(read_runtime_file(source / "tests/fixtures" / name)); }
std::string expect(ErrorCode code, const std::function<void()>& action) {
    try { action(); } catch (const Error& error) {
        if (error.code() != code) throw std::runtime_error(std::string("expected ") + std::string(error_name(code)) +
            ", got " + std::string(error_name(error.code())) + ": " + error.what());
        return error.what();
    }
    throw std::runtime_error("expected failure");
}
void replace(std::string& bytes, const std::string& before, const std::string& after) {
    const auto where = bytes.find(before); CHECK(where != std::string::npos);
    bytes.replace(where, before.size(), after);
}
void sql(const std::filesystem::path& path, const std::string& query) {
    sqlite3* db{}; CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    const auto code = sqlite3_exec(db, query.c_str(), nullptr, nullptr, nullptr);
    const std::string detail = sqlite3_errmsg(db); sqlite3_close(db);
    if (code != SQLITE_OK) throw std::runtime_error(detail);
}
std::string decoded(std::string_view value) {
    CHECK(value.size() % 4 == 0);
    std::string bytes(value.size() / 4 * 3, '\0');
    if (value.empty()) return bytes;
    const auto count = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(bytes.data()),
        reinterpret_cast<const unsigned char*>(value.data()), static_cast<int>(value.size()));
    CHECK(count >= 0);
    auto size = static_cast<std::size_t>(count);
    if (value.back() == '=') --size;
    if (value[value.size() - 2] == '=') --size;
    bytes.resize(size); return bytes;
}
std::string encoded(std::string_view bytes) {
    std::string result((bytes.size() + 2) / 3 * 4, '\0');
    const auto count = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    CHECK(count >= 0); result.resize(static_cast<std::size_t>(count)); return result;
}
Json page_args(std::string kind = "question") {
    return Object{{"kind", std::move(kind)}, {"after_sequence", 0}, {"through_sequence", 0}, {"limit", 4}};
}
Json read_args(const Json& entry, std::int64_t offset = 0) {
    return Object{{"kind", entry.at("kind")}, {"record_id", entry.at("record_id")},
        {"record_sha256", entry.at("record_sha256")}, {"record_version", entry.at("record_version")},
        {"offset", offset}, {"max_bytes", 4096}};
}
FakeModel fake(Role role, const std::string& tool, const Json& arguments, std::int64_t index = 1) {
    FakeModelStep step; step.role = role; step.role_call_index = index;
    step.tool_calls = {{"management-fixture", tool, arguments}}; step.usage = {1, 1, 0, 0, 0};
    return FakeModel({step});
}
FakeModelStep step(Role role, const std::string& tool, const Json& arguments, std::int64_t index = 1) {
    FakeModelStep value; value.role = role; value.role_call_index = index;
    value.tool_calls = {{"management-fixture", tool, arguments}}; value.usage = {1, 1, 0, 0, 0}; return value;
}
Json contract(const CapturedModelCall& call) {
    return parse_json_strict(compiled_task_contract(call.request.context), 1024U * 1024U);
}

struct Fixture {
    test::TempDir temporary;
    SchemaRegistry schemas{source / "schemas"};
    std::filesystem::path database{temporary.path / "state.db"};
    std::unique_ptr<Store> store;
    Json frozen;
    std::string request, task;
    explicit Fixture(bool window = true, std::int64_t query_limit = 60) {
        auto yaml = read_runtime_file(source / "config/examples/system.yaml");
        if (window) replace(yaml, "  request_format: framed-v1\n", "  request_format: structured-v2\n"
            "  management_history: {mode: window-v1, recent_attempts: 2, inline_bytes: 32768, max_context_bytes: 131072}\n");
        replace(yaml, "    questions: 128\n", "    questions: 512\n");
        replace(yaml, "    open_questions: 32\n", "    open_questions: 64\n");
        replace(yaml, "  max_queries_per_minute: 60\n", "  max_queries_per_minute: " + std::to_string(query_limit) + "\n");
        { std::ofstream file(temporary.path / "system.yaml"); file << yaml; CHECK(file.good()); }
        frozen = load_config_catalog(temporary.path / "system.yaml", source / "config/examples/functions", source / "prompts").freeze("general").json();
        reopen(); request = admit();
        OfflineRuntime(*store, schemas).run(request, fake_model_steps_from_json(fixture("s1_fake_initial.json")));
        task = text(store->tasks(request).front(), "task_id"); drain();
    }
    void reopen() {
        store.reset();
        store = std::make_unique<Store>(database, read_runtime_file(source / "db/schema.sql"),
            read_runtime_file(source / "db/migrations/004_s1_store.sql"), StoreOpenMode::Takeover,
            read_runtime_file(source / "db/migrations/005_worker_tools.sql"),
            read_runtime_file(source / "db/migrations/006_event_runtime.sql"),
            read_runtime_file(source / "db/migrations/007_workflow_runtime.sql"),
            read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql"),
            read_runtime_file(source / "db/migrations/009_owned_cleanup.sql"));
    }
    std::string admit() {
        AdmissionInput input; input.tenant_id = "fixture"; input.project_id = "fixture"; input.root_id = "root";
        input.function_id = "general"; input.idempotency_key = new_id("management");
        input.original_text = read_runtime_file(source / "tests/fixtures/s1_request.txt");
        input.constraint_catalog = fixture("s1_constraints.json"); input.config_snapshot = frozen;
        const auto config = FrozenRequestConfig::restore(canonical_json(frozen), sha256_hex(canonical_json(frozen)));
        const auto tools = config.worker_tools(); input.tool_allowlist.assign(tools.begin(), tools.end());
        input.budget = {1000, 40000000, 1000000, 0}; return text(store->admit(input), "request_id");
    }
    ExecutionBinding binding(Role role = Role::TeamLead) const {
        return role == Role::Dispatcher ? store->request_binding(request) : store->binding(task);
    }
    StoreActor actor(Role role = Role::TeamLead) const {
        return store->host_actor(role, request, role == Role::Dispatcher ? std::string{} : store->binding(task).team_id);
    }
    void drain() {
        if (task.empty()) return;
        for (const auto role : {Role::Dispatcher, Role::TeamLead, Role::Worker}) {
            const auto owner = actor(role);
            while (!store->inbox(owner).empty()) {
                const auto claim = store->claim_events(owner); std::vector<std::int64_t> ids;
                for (const auto& item : claim.at("items").as_array()) ids.push_back(num(item, "inbox_id"));
                CHECK(!ids.empty()); store->finish_events(owner, text(claim, "claim_id"), num(claim, "generation"), ids);
            }
        }
    }
    Json ask(std::size_t bytes = 100, bool blocking = false) {
        return store->ask_question(actor(Role::Worker), binding(), num(store->task(task), "state_version"),
            Object{{"text", "Original question " + std::string(bytes, 'q')}, {"context", "Immutable original context"},
                {"blocking", blocking}}, new_id("ask"));
    }
    void host_answer(const Json& question) {
        store->answer_question(actor(), binding(), text(question, "question_id"), num(question, "state_version"),
            Object{{"text", "Keep the original requirement."}}, new_id("answer"));
    }
    CapturedModelCall capture(Role role, const FakeModel& model, bool amendment = false) {
        return OfflineRuntime(*store, schemas).reserve_call(model, request, role,
            role == Role::Dispatcher ? std::string{} : task, 1, {}, amendment);
    }
    Json invoke(Role role, const std::string& tool, const Json& arguments) {
        auto model = fake(role, tool, arguments); const auto call = capture(role, model);
        return OfflineRuntime(*store, schemas).accept_response(call, model.complete(call.request));
    }
    Json page(Role role = Role::TeamLead, const std::string& kind = "question") {
        return store->management_history_page(actor(role), binding(role), kind);
    }
    Json direct_read(const Json& entry, Role role = Role::TeamLead) {
        return store->read_management_history(actor(role), binding(role), text(entry, "kind"), text(entry, "record_id"),
            text(entry, "record_sha256"), num(entry, "record_version"), 0, 4096);
    }
};

void legacy_policy_is_unchanged_and_frozen_selection_is_explicit() {
    Fixture old(false);
    const auto old_config = restore_request_config(old.store->request(old.request));
    CHECK(old_config.management_history_policy().is_null());
    CHECK(old_config.runtime_support().at("management_window_integrated") == false);
    expect(ErrorCode::Unauthorized, [&] { old.store->management_view(old.actor(), old.binding()); });
    Fixture selected;
    const auto selected_config = restore_request_config(selected.store->request(selected.request));
    CHECK(selected_config.management_history_policy().at("recent_attempts") == 2);
    CHECK(selected_config.runtime_support().at("management_window_integrated") == true);
    const auto snapshot_hash = selected.store->request(selected.request).at("config_snapshot_sha256");
    { std::ofstream changed(selected.temporary.path / "system.yaml"); changed << "changed external file\n"; }
    CHECK(restore_request_config(selected.store->request(selected.request)).management_history_policy().at("max_context_bytes") == 131072);
    CHECK(selected.store->request(selected.request).at("config_snapshot_sha256") == snapshot_hash);
}

void long_management_workflows_cross_restart_and_keep_original_readback() {
    Fixture f; Json first_reference; std::string first_question;
    std::size_t maximum = 0;
    for (int index = 0; index < 130; ++index) {
        const auto question = f.ask(6000);
        const auto role = index % 2 == 0 ? Role::TeamLead : Role::Dispatcher;
        if (role == Role::Dispatcher)
            f.store->forward_question(f.actor(), f.binding(), text(question, "question_id"), num(question, "state_version"),
                "Request-level clarification is required.", new_id("forward"));
        auto model = fake(role, "answer_question", Object{{"text", "Preserve the exact original requirement."}});
        const auto call = f.capture(role, model); const auto current = contract(call);
        CHECK(current.at("request") == f.store->request_snapshot(f.request));
        CHECK(current.at("questions").as_array().size() == 1);
        CHECK(current.at("workflow_action").at("target").at("original_sha256") == question.at("original_sha256"));
        CHECK(current.at("unresolved_operations").as_array().empty());
        CHECK(current.at("unresolved_attempts").as_array().empty());
        maximum = std::max(maximum, call.request.context.prompt().size());
        CHECK(maximum <= 131072);
        const auto result = OfflineRuntime(*f.store, f.schemas).accept_response(call, model.complete(call.request));
        CHECK(result.at("status") == "ANSWERED");
        if (index == 0) { first_question = text(question, "question_id"); first_reference = f.page().at("items").as_array().front(); }
        f.drain();
        if (index == 64) { f.reopen(); f.drain(); }
    }
    const auto view = f.store->management_view(f.actor(), f.binding());
    CHECK(view.at("questions").as_array().empty());
    CHECK(view.at("history_catalogs").at("question").at("omitted_completed_count") == 130);
    CHECK(view.at("recent_attempts").as_array().size() == 2);
    CHECK(view.at("history_catalogs").at("attempt").at("omitted_completed_count").as_int64() > 0);
    expect(ErrorCode::Quota, [&] { f.store->questions(f.request, f.task); });
    const auto page = f.invoke(Role::TeamLead, "management_history_page", page_args());
    CHECK(page.at("items").as_array().front().at("record_sha256") == first_reference.at("record_sha256"));
    std::string original; std::int64_t offset = 0;
    const auto before = num(f.store->budget(f.request), "attempts"); std::int64_t calls = 0;
    while (true) {
        const auto fragment = f.invoke(Role::TeamLead, "management_history_read", read_args(first_reference, offset)); ++calls;
        original += decoded(text(fragment, "content_base64"));
        if (fragment.at("eof").as_bool()) break;
        offset = num(fragment, "next_offset");
    }
    CHECK(sha256_hex(original) == text(first_reference, "record_sha256"));
    const auto retained = parse_json_strict(original);
    CHECK(retained.at("original") == f.store->question(first_question));
    CHECK(num(f.store->budget(f.request), "attempts") == before + calls);
    CHECK(maximum < 131072);
}

void old_complete_history_still_stops_without_a_frozen_window() {
    Fixture f(false);
    for (int index = 0; index < 130; ++index) { f.host_answer(f.ask(20)); f.drain(); }
    const auto before = f.store->budget(f.request);
    auto model = fake(Role::TeamLead, "issue_work_order", fixture("s1_fake_initial.json").as_array()[1].at("tool_calls").as_array()[0].at("arguments"));
    expect(ErrorCode::Quota, [&] { f.capture(Role::TeamLead, model); });
    CHECK(f.store->budget(f.request) == before);
    CHECK(restore_request_config(f.store->request(f.request)).management_history_policy().is_null());
}

void exact_query_replay_preserves_the_original_feedback_and_charge() {
    Fixture f; f.host_answer(f.ask());
    auto model = fake(Role::TeamLead, "management_history_page", page_args()); const auto call = f.capture(Role::TeamLead, model);
    const auto response = model.complete(call.request);
    const auto result = OfflineRuntime(*f.store, f.schemas).accept_response(call, response);
    const auto budget = f.store->budget(f.request); const auto receipt = f.store->tool_feedback(call.request.attempt_id);
    CHECK(OfflineRuntime(*f.store, f.schemas).accept_response(call, response) == result);
    CHECK(f.store->budget(f.request) == budget && f.store->tool_feedback(call.request.attempt_id) == receipt);
}

void scope_hash_original_version_and_current_epoch_are_checked() {
    Fixture f; const auto question = f.ask(); const auto open = f.page().at("items").as_array().front();
    f.host_answer(question);
    expect(ErrorCode::Stale, [&] { f.direct_read(open); });
    const auto original = f.page().at("items").as_array().front();
    auto changed = original; changed.as_object()["record_sha256"] = std::string(64, '0');
    expect(ErrorCode::Conflict, [&] { f.direct_read(changed); });
    expect(ErrorCode::Unauthorized, [&] { f.store->management_history_page(f.actor(Role::Worker), f.binding(), "question"); });
    // Both managers can observe their own question scope, but a TeamLead's
    // reference cannot be replayed as a Dispatcher reference.
    expect(ErrorCode::Conflict, [&] { f.direct_read(original, Role::Dispatcher); });
    const auto own_attempt = f.page(Role::TeamLead, "attempt").at("items").as_array().front();
    expect(ErrorCode::Unauthorized, [&] { f.direct_read(own_attempt, Role::Dispatcher); });
    const auto other = f.admit();
    expect(ErrorCode::Unauthorized, [&] {
        f.store->read_management_history(f.store->host_actor(Role::Dispatcher, other), f.store->request_binding(other),
            "question", text(original, "record_id"), text(original, "record_sha256"), num(original, "record_version"));
    });
    const auto actor = f.actor(); const auto binding = f.binding(); f.reopen();
    expect(ErrorCode::Stale, [&] {
        f.store->read_management_history(actor, binding, "question", text(original, "record_id"),
            text(original, "record_sha256"), num(original, "record_version"));
    });
    CHECK(f.direct_read(original).at("record_sha256") == original.at("record_sha256"));
}

void current_open_unknown_and_missing_feedback_are_never_omitted() {
    Fixture f; for (int index = 0; index < 4; ++index) f.ask();
    auto model = fake(Role::TeamLead, "management_history_page", page_args());
    const auto missing = f.capture(Role::TeamLead, model); const auto response = model.complete(missing.request);
    f.store->settle_attempt(response.attempt_id, response.binding, response.config_snapshot_id,
        "COMPLETED", to_json(response.usage), to_json(response));
    for (int index = 0; index < 4; ++index) f.invoke(Role::TeamLead, "management_history_page", page_args());
    const auto operation = f.store->prepare_operation(f.actor(Role::Worker), f.binding(), num(f.store->task(f.task), "state_version"),
        "unknown-effect", "note", Object{{"text", "pending original effect"}}, "none", "prepare-unknown");
    f.store->start_operation(f.actor(Role::Worker), text(operation, "op_id"), num(operation, "state_version"), "start-unknown");
    sql(f.database, "UPDATE ops SET status='UNKNOWN',state_version=state_version+1 WHERE op_id='" + text(operation, "op_id") + "'");
    const auto view = f.store->management_view(f.actor(), f.binding());
    CHECK(view.at("questions").as_array().size() == 4);
    CHECK(view.at("unresolved_operations").as_array().front().at("status") == "UNKNOWN");
    CHECK(view.at("unresolved_attempts").as_array().size() == 1);
    CHECK(text(view.at("unresolved_attempts").as_array().front(), "attempt_id") == missing.request.attempt_id);
    CHECK(view.at("unresolved_attempts").as_array().front().at("response") == to_json(response));
    for (const auto& item : view.at("recent_attempts").as_array()) CHECK(text(item, "attempt_id") != missing.request.attempt_id);
    expect(ErrorCode::NotReady, [&] {
        f.store->read_management_history(f.actor(), f.binding(), "attempt", missing.request.attempt_id, std::string(64, '0'), 1);
    });
}

void full_current_contract_overflow_reports_sections_before_reservation() {
    Fixture f;
    for (int index = 0; index < 12; ++index) f.ask(15000);
    const auto before = f.store->budget(f.request);
    const auto count = f.page(Role::TeamLead, "attempt").at("catalog").at("total_count");
    auto model = fake(Role::TeamLead, "answer_question", Object{{"text", "must not be called"}});
    const auto message = expect(ErrorCode::Quota, [&] { f.capture(Role::TeamLead, model); });
    CHECK(message.find("management_context_capacity") != std::string::npos);
    CHECK(message.find("section_bytes") != std::string::npos && message.find("questions") != std::string::npos);
    CHECK(message.find("required_context_bytes") != std::string::npos);
    CHECK(message.find("\"reservation_created\":false") != std::string::npos);
    CHECK(f.store->budget(f.request) == before);
    CHECK(f.page(Role::TeamLead, "attempt").at("catalog").at("total_count") == count);
    CHECK(f.store->management_view(f.actor(), f.binding()).at("questions").as_array().size() == 12);
}

void policy_revocation_blocks_new_and_already_captured_queries() {
    Fixture f; f.host_answer(f.ask());
    auto model = fake(Role::TeamLead, "management_history_page", page_args()); const auto call = f.capture(Role::TeamLead, model);
    sql(f.database, "UPDATE runtime_policy SET denied_tools='[\"management_history_read\",\"management_history_page\"]',state_version=state_version+1");
    const auto before = f.store->budget(f.request);
    expect(ErrorCode::Unauthorized, [&] { f.capture(Role::TeamLead, model); });
    CHECK(f.store->budget(f.request) == before);
    expect(ErrorCode::Unauthorized, [&] { OfflineRuntime(*f.store, f.schemas).accept_response(call, model.complete(call.request)); });
    CHECK(f.store->tool_feedback(call.request.attempt_id).at("result").at("runtime_error") == true);
}

void failed_queries_count_toward_a_persistent_limit_after_restart() {
    Fixture f(true, 2); f.host_answer(f.ask());
    const auto page = f.invoke(Role::TeamLead, "management_history_page", page_args());
    auto invalid = read_args(page.at("items").as_array().front()); invalid.as_object()["record_id"] = "absent-original";
    expect(ErrorCode::Unauthorized, [&] { f.invoke(Role::TeamLead, "management_history_read", invalid); });
    f.reopen();
    const auto before = num(f.store->budget(f.request), "attempts");
    expect(ErrorCode::Quota, [&] { f.invoke(Role::TeamLead, "management_history_page", page_args()); });
    CHECK(num(f.store->budget(f.request), "attempts") == before + 1);
}

struct DuringRead {
    inline static DuringRead* active{};
    std::function<void()> action;
    std::exception_ptr failure;
    bool fired{};
    static int trace(unsigned kind, void* context, void* statement, void*) noexcept {
        auto& self = *static_cast<DuringRead*>(context); const auto* query = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if (kind == SQLITE_TRACE_STMT && self.action && !self.fired && query &&
            std::string_view(query).starts_with("SELECT q.*,x.original_binding,x.original_sha256")) {
            self.fired = true;
            try { self.action(); } catch (...) { self.failure = std::current_exception(); }
        }
        return 0;
    }
    static int install(sqlite3* database, char**, const sqlite3_api_routines*) noexcept {
        return active ? sqlite3_trace_v2(database, SQLITE_TRACE_STMT, trace, active) : SQLITE_OK;
    }
    DuringRead() {
        CHECK(active == nullptr); active = this;
        CHECK(sqlite3_auto_extension(reinterpret_cast<void(*)()>(install)) == SQLITE_OK);
    }
    ~DuringRead() { sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(install)); active = nullptr; }
    DuringRead(const DuringRead&) = delete;
    DuringRead& operator=(const DuringRead&) = delete;
};

void policy_change_inside_a_read_cannot_release_the_captured_bytes() {
    DuringRead interleave; Fixture f; f.host_answer(f.ask()); const auto entry = f.page().at("items").as_array().front();
    auto model = fake(Role::TeamLead, "management_history_read", read_args(entry)); const auto call = f.capture(Role::TeamLead, model);
    interleave.action = [&] {
        sql(f.database, "UPDATE runtime_policy SET denied_tools='[\"management_history_read\"]',state_version=state_version+1");
    };
    expect(ErrorCode::Unauthorized, [&] { OfflineRuntime(*f.store, f.schemas).accept_response(call, model.complete(call.request)); });
    if (interleave.failure) std::rethrow_exception(interleave.failure);
    CHECK(interleave.fired);
    const auto result = f.store->tool_feedback(call.request.attempt_id).at("result");
    CHECK(result.at("runtime_error") == true && !result.as_object().contains("content_base64"));
}

void amendment_and_old_task_cas_never_refresh_a_captured_decision() {
    Fixture f; const auto question = f.ask();
    auto model = fake(Role::TeamLead, "answer_question", Object{{"text", "old captured answer"}});
    const auto old = f.capture(Role::TeamLead, model);
    const auto original = contract(old).at("workflow_action");
    f.store->amend(f.store->host_actor(Role::Root, f.request), num(f.store->request(f.request), "state_version"),
        read_runtime_file(source / "tests/fixtures/s1_amendment.txt"), fixture("s1_amended_constraints.json"), "amend-current");
    expect(ErrorCode::Stale, [&] { OfflineRuntime(*f.store, f.schemas).accept_response(old, model.complete(old.request)); });
    CHECK(f.store->question(text(question, "question_id")).at("answer").is_null());
    CHECK(contract(old).at("workflow_action") == original);
    CHECK(f.store->tool_feedback(old.request.attempt_id).at("result").at("runtime_error") == true);
    auto dispatcher = fake(Role::Dispatcher, "submit_plan", fixture("s1_fake_initial.json").as_array()[0].at("tool_calls").as_array()[0].at("arguments"));
    const auto amended = f.capture(Role::Dispatcher, dispatcher, true);
    CHECK(contract(amended).at("request") == f.store->request_snapshot(f.request));
    CHECK(contract(amended).at("request").at("texts").as_array().size() == 2);
    // AMEND captures Store::tasks, including the current team/lead/worker and
    // execution epoch. Store::task is the separate single-task projection.
    CHECK(contract(amended).at("workflow_action").at("target").at("tasks").as_array().front() == f.store->tasks(f.request).front());

    Fixture cas;
    auto order = fake(Role::TeamLead, "issue_work_order", fixture("s1_fake_initial.json").as_array()[1].at("tool_calls").as_array()[0].at("arguments"));
    const auto captured = cas.capture(Role::TeamLead, order);
    const auto instruction = cas.store->instruction(cas.task);
    // Owned fault fixture: advance only the authoritative task CAS while its
    // semantic binding remains unchanged, so the final transaction must catch it.
    sql(cas.database, "UPDATE tasks SET state_version=state_version+1 WHERE task_id='" + cas.task + "'");
    expect(ErrorCode::Conflict, [&] { OfflineRuntime(*cas.store, cas.schemas).accept_response(captured, order.complete(captured.request)); });
    CHECK(cas.store->instruction(cas.task) == instruction);
    CHECK(cas.store->tool_feedback(captured.request.attempt_id).at("result").at("runtime_error") == true);
}

void event_pump_readbacks_preserve_pending_targets_then_answer() {
    Fixture f; f.host_answer(f.ask()); f.drain(); const auto pending = f.ask(200, true);
    const auto node = text(f.store->task(f.task), "node_id");
    EventFakeScript script;
    script.by_node[node] = {step(Role::TeamLead, "management_history_page", page_args(), 1),
        step(Role::TeamLead, "answer_question", Object{{"text", "Exact current answer after reading originals."}}, 2)};
    EventPumpOptions options; options.max_calls = 1;
    const auto first = EventRuntime(*f.store, f.schemas).pump(f.request, script, options);
    CHECK(first.at("calls").as_array().size() == 1);
    CHECK(f.store->question(text(pending, "question_id")).at("status") == "OPEN");
    CHECK(!f.store->inbox(f.actor()).empty());
    f.reopen();
    script.by_node[node] = {step(Role::TeamLead, "answer_question", Object{{"text", "Current answer after restart."}})};
    const auto second = EventRuntime(*f.store, f.schemas).pump(f.request, script, options);
    CHECK(second.at("calls").as_array().size() == 1);
    CHECK(f.store->question(text(pending, "question_id")).at("status") == "ANSWERED");
    CHECK(second.as_object().contains("management_history"));
}

void captured_submission_keeps_complete_original_files() {
    Fixture f; std::filesystem::create_directory(f.temporary.path / "work");
    WorkerTools tools(*f.store, f.temporary.path / "work"); tools.bind(f.actor(Role::Worker));
    const auto body = fixture("s14_fake_submit.json").at("nodes").at("alpha").as_array()[2].at("tool_calls").as_array()[0].at("arguments");
    const std::string bytes = "immutable candidate " + std::string(3000, 'c');
    const Json files = Array{Object{{"path", "/work/result.txt"}, {"sha256", sha256_hex(bytes)},
        {"size_bytes", bytes.size()}, {"content_base64", encoded(bytes)}}};
    const auto submission = f.store->submit_work(f.actor(Role::Worker), f.binding(), num(f.store->task(f.task), "state_version"),
        body, files, "submit-original");
    auto model = fake(Role::TeamLead, "check_submission", Object{{"passed", true}, {"notes", "original bytes checked"},
        {"criteria_results", Array{Object{{"criterion_id", "ac1"}, {"met", "yes"}}}}});
    const auto call = f.capture(Role::TeamLead, model); const auto captured = contract(call);
    CHECK(captured.at("workflow_action").at("target") == submission);
    CHECK(captured.at("submission").at("artifact_snapshot") == files);
    CHECK(text(captured.at("submission_file_views").as_array().front(), "content") == bytes);
    f.store->amend(f.store->host_actor(Role::Root, f.request), num(f.store->request(f.request), "state_version"),
        read_runtime_file(source / "tests/fixtures/s1_amendment.txt"), fixture("s1_amended_constraints.json"), "supersede-candidate");
    expect(ErrorCode::Stale, [&] { OfflineRuntime(*f.store, f.schemas).accept_response(call, model.complete(call.request)); });
    CHECK(f.store->submission(text(submission, "submission_id")).at("artifact_snapshot") == files);
    const auto page = f.page(Role::Dispatcher, "submission");
    CHECK(page.at("items").as_array().front().at("completed") == true);
    CHECK(f.direct_read(page.at("items").as_array().front(), Role::Dispatcher).at("bytes_returned").as_uint64() > 0);
}

void live_usage_projection_keeps_cache_unknown_without_rewriting_originals() {
    namespace live = wls::test::live;
    const live::Assets assets(source);
    live::Fixture f(assets);
    auto admission = live::admission(assets, f.live, "management-live-fixture");
    auto system = admission.config_snapshot.at("system");
    const Json policy = Object{{"mode", "window-v1"}, {"recent_attempts", 2},
        {"inline_bytes", 32768}, {"max_context_bytes", 131072}};
    system.at("context").as_object()["management_history"] = policy;
    std::filesystem::copy(source / "config/examples", f.directory.path / "config", std::filesystem::copy_options::recursive);
    const auto path = f.directory.path / "config/system.yaml";
    { std::ofstream file(path); file << canonical_json(system); CHECK(file.good()); }
    admission.config_snapshot = load_config_catalog(path, f.directory.path / "config/functions", source / "prompts").freeze("general").json();
    f.request_id = text(f.store->admit(admission), "request_id");
    auto request = f.request("management-live");
    ContextInput context;
    context.role = Role::Dispatcher; context.model = request.model;
    context.adapter_version = request.context.adapter_version; context.protocol_version = request.context.protocol_version;
    context.system_prompt = "Synthetic pre-cancel management fixture; no network is permitted.";
    context.role_prompt = "Keep original usage separate from the trusted usage projection.";
    context.function_prompt = "Read only the original Store evidence.";
    context.tools = management_history_declarations();
    context.task_contract = canonical_json(Object{{"request", f.store->request_snapshot(f.request_id)},
        {"binding", to_json(request.binding)}, {"management_history_policy", policy}});
    request.context = compile_management_context(context, {}, policy);
    const auto settled = f.interrupted(request);
    CHECK(f.store->live_attempt(request.attempt_id).at("transport").at("network_started") == false);
    f.store->record_tool_feedback(request.attempt_id, request.binding, settled.at("response"),
        Object{{"runtime_error", true}, {"reason", "synthetic pre-cancelled transport"}});
    const auto original = f.store->attempt(request.attempt_id);
    ModelUsage main_usage; main_usage.input_tokens = 0; main_usage.output_tokens = 0; main_usage.cost_micro_usd = 0;
    f.store->reconcile_usage(f.actor(Role::Root), live::evidence(settled, main_usage), {"main-usage", "main-usage", 1});
    auto view = f.store->management_view(f.actor(), request.binding);
    CHECK(view.at("unresolved_attempts").as_array().size() == 1);
    const auto& fact = view.at("unresolved_attempts").as_array().front();
    CHECK(fact.at("usage") == original.at("usage"));
    CHECK(fact.at("live_usage_projection").at("input_tokens") == 0);
    CHECK(fact.at("live_usage_projection").at("cache_read_tokens").is_null());
    CHECK(fact.at("live_usage_projection").at("cache_write_tokens").is_null());
    CHECK(fact.at("original_usage_immutable") == true);
    CHECK(view.at("history_catalogs").at("attempt").at("completed_count") == 0);
    main_usage.cache_read_tokens = 0; main_usage.cache_write_tokens = 0;
    f.store->reconcile_usage(f.actor(Role::Root), live::evidence(settled, main_usage, "caches"), {"cache-usage", "cache-usage", 2});
    view = f.store->management_view(f.actor(), request.binding);
    CHECK(view.at("unresolved_attempts").as_array().empty());
    CHECK(view.at("history_catalogs").at("attempt").at("completed_count") == 1);
    CHECK(f.store->attempt(request.attempt_id) == original);

    // The pre-existing Worker window uses the same original Store ledger.
    // Create its task through ordinary management APIs, then prove the new
    // projection interpretation without retagging a Fake reservation.
    const Json brief = Object{{"goal", admission.original_text}, {"constraint_refs", Array{"c1"}},
        {"constraints_verbatim", Array{admission.original_text}}};
    f.store->apply_independent_plan(f.actor(), num(f.store->request(f.request_id), "state_version"), 1,
        Object{{"nodes", Array{Object{{"node_id", "worker-evidence"}, {"kind", "team_task"}, {"required", true},
            {"depends_on", Array{}}, {"brief", brief}}}}}, Object{}, "worker-evidence-plan");
    const auto task_id = text(f.store->tasks(f.request_id).front(), "task_id");
    const auto lead = f.store->host_actor(Role::TeamLead, f.request_id, f.store->binding(task_id).team_id);
    f.store->issue_instruction(lead, f.store->binding(task_id), num(f.store->task(task_id), "state_version"), brief, "worker-evidence-order");
    const auto worker = f.store->host_actor(Role::Worker, f.request_id, f.store->binding(task_id).team_id);
    f.store->acknowledge(worker, f.store->binding(task_id), num(f.store->task(task_id), "state_version"), "worker-evidence-ack");
    auto worker_request = f.request("worker-live-projection");
    worker_request.role = Role::Worker; worker_request.binding = f.store->binding(task_id);
    const auto frozen = restore_request_config(f.store->request(f.request_id));
    context.role = Role::Worker; context.model = frozen.resolve(Role::Worker); context.tools.clear();
    context.task_contract = canonical_json(Object{{"request", f.store->request_snapshot(f.request_id)},
        {"task", f.store->task(task_id)}, {"instruction", f.store->instruction(task_id)},
        {"binding", to_json(worker_request.binding)}});
    worker_request.model = context.model; worker_request.context = compile_structured_context(context, {});
    f.store->reserve_attempt(worker, worker_request.binding, f.reservation(worker_request));
    const auto claim = f.store->claim_live_send(worker, worker_request.attempt_id, 1,
        encode_responses_request(f.live.provider, worker_request, false), f.live.transport, "worker-evidence-send");
    const auto transport = f.cancelled(worker_request); CHECK(!transport.network_started);
    f.store->record_live_transport(worker_request.attempt_id, text(claim, "claim_id"), transport);
    const auto worker_settled = f.settle(worker_request, transport);
    f.store->record_tool_feedback(worker_request.attempt_id, worker_request.binding, worker_settled.at("response"),
        Object{{"runtime_error", true}, {"reason", "synthetic pre-cancelled Worker transport"}});
    const auto worker_original = f.store->attempt(worker_request.attempt_id);
    main_usage.cache_read_tokens.reset(); main_usage.cache_write_tokens.reset();
    f.store->reconcile_usage(f.actor(Role::Root), live::evidence(worker_settled, main_usage, "worker-main"),
        {"worker-main", "worker-main", 1});
    const auto tail = f.store->attempt_history_tail(worker, 1);
    CHECK(tail.at("unresolved_attempts").as_array().size() == 1);
    CHECK(tail.at("unresolved_attempts").as_array().front().at("live_usage_projection").at("cache_read_tokens").is_null());
    CHECK(tail.at("unresolved_attempts").as_array().front().at("live_usage_projection").at("input_tokens") == 0);
    main_usage.cache_read_tokens = 0; main_usage.cache_write_tokens = 0;
    f.store->reconcile_usage(f.actor(Role::Root), live::evidence(worker_settled, main_usage, "worker-cache"),
        {"worker-cache", "worker-cache", 2});
    CHECK(f.store->attempt_history_tail(worker, 1).at("unresolved_attempts").as_array().empty());
    CHECK(f.store->attempt(worker_request.attempt_id) == worker_original);
}
} // namespace

int main() { return test::run({
    {"management policy is explicit frozen and legacy-compatible", legacy_policy_is_unchanged_and_frozen_selection_is_explicit},
    {"130 management turns cross restart with exact original readback", long_management_workflows_cross_restart_and_keep_original_readback},
    {"legacy full management history still stops explicitly", old_complete_history_still_stops_without_a_frozen_window},
    {"exact management query replay keeps receipt and budget", exact_query_replay_preserves_the_original_feedback_and_charge},
    {"management reader scope hash version and epoch guards", scope_hash_original_version_and_current_epoch_are_checked},
    {"all open UNKNOWN and missing original facts stay pinned", current_open_unknown_and_missing_feedback_are_never_omitted},
    {"full current contract overflow has zero new reservations", full_current_contract_overflow_reports_sections_before_reservation},
    {"current policy fences new and captured management reads", policy_revocation_blocks_new_and_already_captured_queries},
    {"failed read queries consume persistent cross-restart allowance", failed_queries_count_toward_a_persistent_limit_after_restart},
    {"policy change during read refuses original bytes", policy_change_inside_a_read_cannot_release_the_captured_bytes},
    {"AMEND and independent old task CAS refuse stale decisions", amendment_and_old_task_cas_never_refresh_a_captured_decision},
    {"event pump readback keeps pending target across restart", event_pump_readbacks_preserve_pending_targets_then_answer},
    {"captured candidate files stay complete through AMEND", captured_submission_keeps_complete_original_files},
    {"live cache unknown and reconciliation preserve original management facts", live_usage_projection_keeps_cache_unknown_without_rewriting_originals}
}); }
