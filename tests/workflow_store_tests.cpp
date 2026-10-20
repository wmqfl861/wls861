#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "wls/model.hpp"
#include "test_support.hpp"

#include <sqlite3.h>
#include <sys/stat.h>

#include <barrier>
#include <fstream>
#include <future>
#include <iterator>

using namespace wls::s1;
using wls::Role;
using Object = boost::json::object;
using Array = boost::json::array;

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
std::filesystem::path source_root;
std::string base_sql, store_sql, tools_sql, events_sql, workflow_sql;
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary); stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(stream.good());
}
std::string s(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t n(const Json& value, std::string_view key) { return value.at(key).as_int64(); }
void error(ErrorCode expected, const std::function<void()>& operation) {
    try { operation(); }
    catch (const Error& value) { CHECK(value.code() == expected); return; }
    throw std::runtime_error("expected Store error");
}
void raw_exec(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* db = nullptr; CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    char* message = nullptr; const auto code = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    const std::string reason = message ? message : "SQLite fixture failure";
    sqlite3_free(message); sqlite3_close(db);
    if (code != SQLITE_OK) throw std::runtime_error(reason);
}
std::int64_t scalar(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* db = nullptr; CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr; CHECK(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const auto result = static_cast<std::int64_t>(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement); sqlite3_close(db); return result;
}
std::string base64(std::string_view value) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < value.size(); i += 3) {
        const auto first = static_cast<unsigned char>(value[i]);
        const auto second = i + 1 < value.size() ? static_cast<unsigned char>(value[i + 1]) : 0U;
        const auto third = i + 2 < value.size() ? static_cast<unsigned char>(value[i + 2]) : 0U;
        const auto word = (static_cast<unsigned int>(first) << 16U) | (second << 8U) | third;
        result.push_back(alphabet[(word >> 18U) & 63U]); result.push_back(alphabet[(word >> 12U) & 63U]);
        result.push_back(i + 1 < value.size() ? alphabet[(word >> 6U) & 63U] : '=');
        result.push_back(i + 2 < value.size() ? alphabet[word & 63U] : '=');
    }
    return result;
}
AdmissionInput admission() {
    AdmissionInput input;
    input.tenant_id = "tenant"; input.project_id = "project"; input.root_id = "root";
    input.function_id = "general"; input.idempotency_key = new_id("admission");
    input.original_text = "preserve fixture observations";
    input.config_snapshot = load_config_catalog(source_root / "config/examples/system.yaml",
        source_root / "config/examples/functions", source_root / "prompts").freeze("general").json();
    input.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", input.original_text}, {"source_revision", 1}}};
    input.tool_allowlist = {"note", "ask", "submit", "fs_read", "fs_write", "ack_change", "op_status", "op_cancel"};
    input.budget = {64, 1000000, 100000, 0}; return input;
}
Json brief(const std::string& title = "fixture") {
    return Object{{"title", title}, {"goal", "preserve original durable observations"},
        {"acceptance_criteria", Array{Object{{"id", "ac1"}, {"text", "preserve the fixture bytes"}}, Object{{"id", "ac2"}, {"text", "report checked observations"}}}},
        {"constraint_refs", Array{"c1"}}, {"constraints_verbatim", Array{"preserve fixture observations"}}};
}
Json plan_body() {
    Array nodes;
    for (const auto* id : {"alpha", "beta"}) nodes.emplace_back(Object{{"node_id", id}, {"kind", "team_task"}, {"title", id},
        {"depends_on", Array{}}, {"required", true}, {"brief", brief(id)}});
    return Object{{"summary", "independent workflow fixtures"}, {"nodes", nodes}};
}
Json order() {
    return Object{{"approach", "Inspect the saved task and preserve its bytes."},
        {"steps", Array{Object{{"id", "s1"}, {"text", "Write and inspect the fixture."}}}},
        {"constraint_refs", Array{"c1"}}, {"constraints_verbatim", Array{"preserve fixture observations"}},
        {"done_definition", "Both current acceptance criteria have evidence."}};
}
Json question_body(bool blocking = true) {
    return Object{{"text", "Which approved fixture should I use?"}, {"context", "The two fixture variants differ."},
        {"blocking", blocking}, {"options", Array{Object{{"id", "a"}, {"label", "original"}}, Object{{"id", "b"}, {"label", "alternate"}}}}};
}
Json submission_body() {
    return Object{{"summary", "Saved and checked fixture output."},
        {"deliverables", Array{Object{{"path", "/work/result.txt"}, {"description", "verified fixture"}}}},
        {"self_check", Array{Object{{"criterion_id", "ac1"}, {"met", "yes"}, {"evidence", "original fixture byte comparison"}},
                             Object{{"criterion_id", "ac2"}, {"met", "yes"}, {"evidence", "recorded read observation"}}}},
        {"verification", Array{Object{{"check", "fixture bytes"}, {"method", "bounded host read"}, {"result", "pass"}, {"evidence", "fixture SHA-256"}}}}};
}
Json check_body(bool passed = true) {
    return Object{{"passed", passed}, {"notes", passed ? "Both criteria have current evidence." : "Repair the current fixture."},
        {"criteria_results", Array{Object{{"criterion_id", "ac1"}, {"met", passed ? "yes" : "no"}}, Object{{"criterion_id", "ac2"}, {"met", "yes"}}}}};
}
Json snapshot_file(const std::string& path, const std::string& bytes) {
    return Object{{"path", path}, {"sha256", sha256_hex(bytes)}, {"size_bytes", static_cast<std::int64_t>(bytes.size())}, {"content_base64", base64(bytes)}};
}
struct Fixture {
    test::TempDir temp;
    std::filesystem::path path{temp.path / "state.sqlite"};
    std::unique_ptr<Store> store;
    std::string request_id;
    explicit Fixture(bool workflow = true) {
        store = std::make_unique<Store>(path, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql, workflow ? workflow_sql : "");
        request_id = s(store->admit(admission()), "request_id");
        store->apply_independent_plan(actor(Role::Dispatcher), rv(), 1, plan_body(), Object{}, new_id("plan"));
    }
    Json task(const std::string& node = "alpha") const {
        for (const auto& value : store->tasks(request_id)) if (s(value, "node_id") == node) return value;
        throw std::runtime_error("fixture node not allocated");
    }
    std::string tid(const std::string& node = "alpha") const { return s(task(node), "task_id"); }
    std::int64_t rv() const { return n(store->request(request_id), "state_version"); }
    std::int64_t tv(const std::string& node = "alpha") const { return n(task(node), "state_version"); }
    ExecutionBinding binding(const std::string& node = "alpha") const { return store->binding(tid(node)); }
    StoreActor actor(Role role, const std::string& node = "alpha") const {
        if (role == Role::Root || role == Role::Dispatcher) return store->host_actor(role, request_id);
        return store->host_actor(role, request_id, s(task(node), "team_id"));
    }
    ExecutionBinding actor_binding(Role role, const std::string& node = "alpha") const {
        return role == Role::Root || role == Role::Dispatcher ? store->request_binding(request_id) : binding(node);
    }
    void issue(const std::string& node = "alpha") { store->issue_instruction(actor(Role::TeamLead, node), binding(node), tv(node), order(), new_id("order")); }
    void ack(const std::string& node = "alpha") { store->acknowledge(actor(Role::Worker, node), binding(node), tv(node), new_id("ack")); }
    void ready(const std::string& node = "alpha") { issue(node); ack(node); }
    std::filesystem::path workspace(const std::string& node = "alpha") const { return temp.path / node; }
    void bind(const std::string& node = "alpha") {
        std::filesystem::create_directory(workspace(node)); struct stat info{};
        CHECK(::stat(workspace(node).c_str(), &info) == 0);
        store->bind_workspace(s(task(node), "team_id"), std::filesystem::canonical(workspace(node)).string(),
            static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino));
        write(workspace(node) / "result.txt", "original fixture\n");
    }
    Json snapshot(const std::string& node = "alpha") const { return Array{snapshot_file("/work/result.txt", read(workspace(node) / "result.txt"))}; }
    Json ask(bool blocking = true, const std::string& node = "alpha") {
        return store->ask_question(actor(Role::Worker, node), binding(node), tv(node), question_body(blocking), new_id("ask"));
    }
    Json answer(const Json& question, Role role = Role::TeamLead, const std::string& text = "Use the original fixture.") {
        return store->answer_question(actor(role), actor_binding(role), s(question, "question_id"), n(question, "state_version"), Object{{"text", text}}, new_id("answer"));
    }
    Json forward(const Json& question, Role role) {
        return store->forward_question(actor(role), actor_binding(role), s(question, "question_id"), n(question, "state_version"), "Original question retained.", new_id("forward"));
    }
    Json submit(const std::string& node = "alpha", const Json& body = submission_body()) {
        return store->submit_work(actor(Role::Worker, node), binding(node), tv(node), body, snapshot(node), new_id("submit"));
    }
    Json check(const Json& submission, bool passed = true) {
        return store->check_submission(actor(Role::TeamLead), binding(), tv(), s(submission, "submission_id"), n(submission, "state_version"),
            check_body(passed), passed ? Json(nullptr) : order(), new_id("check"));
    }
    void reopen() { store.reset(); store = std::make_unique<Store>(path, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql, workflow_sql); }
    void amend() { store->amend(actor(Role::Root), rv(), "review amended fixtures", admission().constraint_catalog, new_id("amend")); }
    Json prepare(const std::string& node = "alpha") {
        return store->prepare_tool_operation(actor(Role::Worker, node), binding(node), tv(node), new_id("business"), "note", Object{{"text", "fixture"}}, new_id("prepare"));
    }
};

void upgrade_readonly_rollback_and_fingerprints() {
    Fixture f(false); const auto original = f.store->request(f.request_id);
    CHECK(!f.store->workflow_runtime_enabled());
    error(ErrorCode::Unsupported, [&] { f.store->questions(f.request_id); });
    { Store observer(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql, events_sql, workflow_sql);
      CHECK(!observer.workflow_runtime_enabled()); CHECK(observer.request(f.request_id) == original); }
    f.store.reset();
    error(ErrorCode::Storage, [&] { Store broken(f.path, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql,
        workflow_sql + "\nSELECT * FROM missing_workflow_upgrade_fixture;\n"); });
    CHECK(scalar(f.path, "PRAGMA user_version") == 404);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM sqlite_master WHERE name='workflow_submissions'") == 0);
    f.reopen(); CHECK(f.store->workflow_runtime_enabled()); CHECK(n(f.store->diagnostics(), "user_version") == 405);
    CHECK(f.store->request(f.request_id).at("config_snapshot") == original.at("config_snapshot"));
    const auto current = f.store->request(f.request_id);
    { Store observer(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql, events_sql, workflow_sql);
      CHECK(observer.request(f.request_id) == current); CHECK(observer.questions(f.request_id).empty());
      error(ErrorCode::Unauthorized, [&] { observer.ask_question(observer.host_actor(Role::Worker, f.request_id, s(f.task(), "team_id")), observer.binding(f.tid()), f.tv(), question_body(), "readonly-ask"); }); }
    error(ErrorCode::Unsupported, [&] { Store observer(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql, events_sql); });
    error(ErrorCode::Corrupt, [&] { Store observer(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql, events_sql, workflow_sql + "\n-- drift"); });
}

void question_wait_is_atomic_scoped_and_not_bypassable() {
    Fixture f; f.ready(); f.ready("beta"); const auto beta = f.task("beta");
    const auto question = f.ask(); const auto id = s(question, "question_id");
    CHECK(s(question, "holder") == "team_lead"); CHECK(s(question, "status") == "OPEN");
    CHECK(s(f.store->worker_wait(f.tid()), "status") == "WAITING"); CHECK(f.task("beta") == beta);
    error(ErrorCode::NotReady, [&] { f.prepare(); });
    error(ErrorCode::NotReady, [&] { f.issue(); });
    error(ErrorCode::NotReady, [&] { f.store->wake_worker(f.actor(Role::Worker), f.binding(), n(question, "wait_generation"), "bypass-wake"); });
    error(ErrorCode::NotReady, [&] { f.store->wait_worker(f.actor(Role::Worker), f.binding(), f.tv(), "replace question wait", "bypass-wait"); });
    error(ErrorCode::NotReady, [&] { f.ask(); });
    CHECK(!f.prepare("beta").is_null());
    const auto answered = f.answer(question); CHECK(s(answered, "holder") == "answered");
    CHECK(s(answered, "status") == "ANSWERED"); CHECK(s(f.store->worker_wait(f.tid()), "status") == "AWAKE");
    CHECK(answered.at("body") == question.at("body")); CHECK(answered.at("original_binding") == question.at("original_binding"));
    CHECK(!f.prepare().is_null()); CHECK(f.store->question(id) == answered);
}

void question_forwards_preserve_original_chain_and_ownership() {
    Fixture f; f.ready(); f.ready("beta"); const auto original = f.ask();
    error(ErrorCode::Unauthorized, [&] { f.store->answer_question(f.actor(Role::TeamLead, "beta"), f.binding("beta"), s(original, "question_id"), 1, Object{{"text", "foreign answer"}}, "foreign-answer"); });
    error(ErrorCode::NotReady, [&] { f.answer(original, Role::Root); });
    auto question = f.forward(original, Role::TeamLead); CHECK(s(question, "holder") == "dispatcher");
    error(ErrorCode::NotReady, [&] { f.answer(question, Role::TeamLead); });
    question = f.forward(question, Role::Dispatcher); CHECK(s(question, "holder") == "user");
    const auto original_binding = question.at("original_binding"); f.reopen();
    question = f.store->question(s(question, "question_id")); CHECK(question.at("body") == original.at("body"));
    question = f.answer(question, Role::Root); CHECK(s(question.at("answer"), "answered_by") == "user");
    CHECK(n(question.at("answer"), "requirements_revision") == 1); CHECK(question.at("original_binding") == original_binding);
    CHECK(question.at("forwarded").as_array().size() == 2); CHECK(question.at("transitions").as_array().size() == 3);
    CHECK(n(f.task(), "paused_for_change") == 1); error(ErrorCode::NotReady, [&] { f.prepare(); });
}

void question_control_survives_unknown_without_resolving_it() {
    Fixture f; f.ready(); const auto op = f.prepare();
    const auto running = f.store->start_operation(f.actor(Role::Worker), s(op, "op_id"), n(op, "state_version"), "start-unknown");
    f.store->cancel_operation(f.actor(Role::Worker), s(op, "op_id"), n(running, "state_version"), "cancel-unknown");
    const auto question = f.ask(); const auto answered = f.answer(question);
    CHECK(s(answered, "status") == "ANSWERED"); CHECK(s(f.store->operation(s(op, "op_id")), "status") == "UNKNOWN");
    error(ErrorCode::UnknownOutcome, [&] { f.prepare(); });
    CHECK(s(f.store->worker_wait(f.tid()), "status") == "AWAKE");
}

void question_current_policy_control_rejects_old_capture_without_deadlock() {
    Fixture f; f.ready(); const auto question = f.ask(); const auto captured = f.binding();
    const auto lead = f.actor(Role::TeamLead);
    raw_exec(f.path, "UPDATE runtime_policy SET denied_tools='[\"run\"]',policy_epoch=policy_epoch+1,state_version=state_version+1 WHERE singleton=1;");
    error(ErrorCode::Stale, [&] { f.store->answer_question(lead, captured, s(question, "question_id"), 1, Object{{"text", "old captured answer"}}, "old-policy-answer"); });
    const auto answered = f.answer(question);
    CHECK(s(answered, "status") == "ANSWERED"); CHECK(answered.at("original_binding") == question.at("original_binding"));
    CHECK(s(f.store->worker_wait(f.tid()), "status") == "AWAKE");
    CHECK(f.store->worker_wait(f.tid()).at("original_binding") == question.at("original_binding"));
    CHECK(n(answered.at("transitions").as_array().back().at("captured_binding"), "policy_epoch") == captured.policy_epoch + 1);
    CHECK(!f.prepare().is_null());
}

void unanswered_question_and_submission_are_superseded_by_amend() {
    Fixture f; f.ready(); f.ready("beta"); f.bind("beta");
    const auto question = f.ask(); const auto submission = f.submit("beta");
    f.amend(); CHECK(s(f.store->question(s(question, "question_id")), "status") == "SUPERSEDED");
    CHECK(s(f.store->submission(s(submission, "submission_id")), "status") == "SUPERSEDED");
    CHECK(s(f.store->worker_wait(f.tid()), "status") == "SUPERSEDED");
    error(ErrorCode::Stale, [&] { f.answer(question); });
    error(ErrorCode::Stale, [&] { f.store->check_submission(f.actor(Role::TeamLead, "beta"), f.binding("beta"), f.tv("beta"), s(submission, "submission_id"), 1, check_body(), nullptr, "stale-submit-check"); });
    CHECK(f.store->current_submission(f.tid("beta")).is_null());
}

void question_idempotency_conflicts_and_exact_selection() {
    Fixture f; f.ready(); const auto worker = f.actor(Role::Worker); const auto binding = f.binding(); const auto tv = f.tv();
    const auto question = f.store->ask_question(worker, binding, tv, question_body(), "same-ask");
    CHECK(f.store->ask_question(worker, binding, tv, question_body(), "same-ask") == question);
    error(ErrorCode::Conflict, [&] { f.store->ask_question(worker, binding, tv, question_body(false), "same-ask"); });
    error(ErrorCode::InvalidInput, [&] { f.store->answer_question(f.actor(Role::TeamLead), f.binding(), s(question, "question_id"), 1, Object{{"selected_option", "missing"}}, "bad-option"); });
    const auto lead = f.actor(Role::TeamLead); const auto captured = f.binding();
    const auto answered = f.store->answer_question(lead, captured, s(question, "question_id"), 1, Object{{"selected_option", "a"}}, "same-answer");
    f.reopen(); CHECK(f.store->answer_question(lead, captured, s(question, "question_id"), 1, Object{{"selected_option", "a"}}, "same-answer") == answered);
    CHECK(f.store->questions(f.request_id).size() == 1);
}

void question_atomic_failure_leaves_no_wait_or_row() {
    Fixture f; f.ready(); const auto before = f.task();
    raw_exec(f.path, "CREATE TRIGGER reject_question_event BEFORE INSERT ON events WHEN NEW.type='ask_question' BEGIN SELECT RAISE(ABORT,'fixture refuses event'); END;");
    error(ErrorCode::Conflict, [&] { f.ask(); });
    CHECK(f.store->questions(f.request_id).empty()); CHECK(f.store->worker_wait(f.tid()).is_null()); CHECK(f.task() == before);
}

void nonblocking_question_keeps_worker_ready_and_uses_bounded_history() {
    Fixture f; f.ready(); const auto first = f.ask(false); CHECK(first.at("wait_generation").is_null());
    CHECK(f.store->worker_wait(f.tid()).is_null()); CHECK(!f.prepare().is_null());
    for (int i = 1; i < 32; ++i) f.ask(false);
    error(ErrorCode::Quota, [&] { f.ask(false); });
    error(ErrorCode::Quota, [&] { f.store->questions(f.request_id, {}, 31); });
    CHECK(f.store->questions(f.request_id).size() == 32);
    f.answer(first); CHECK(!f.ask(false).is_null());
}

void snapshot_bytes_are_immutable_across_file_changes_and_recovery() {
    Fixture f; f.ready(); f.bind(); const std::string original("\0\xff\x80\n", 4); write(f.workspace() / "result.txt", original);
    const auto snapshot = f.snapshot(); const auto binding = f.binding(); const auto submitted = f.submit();
    CHECK(submitted.at("artifact_snapshot") == snapshot); CHECK(submitted.at("original_binding") == to_json(binding));
    write(f.workspace() / "result.txt", "mutated current file"); f.reopen();
    const auto restored = f.store->submission(s(submitted, "submission_id")); CHECK(restored.at("artifact_snapshot") == snapshot);
    CHECK(restored.at("body") == submitted.at("body")); CHECK(restored.at("original_binding") == submitted.at("original_binding"));
    const auto checked = f.check(restored); CHECK(s(checked, "status") == "CANDIDATE_READY");
    CHECK(!checked.at("business_accepted").as_bool()); CHECK(!checked.at("business_delivered").as_bool()); CHECK(s(f.task(), "status") == "SUBMITTED");
    CHECK(s(f.store->request(f.request_id), "status") == "RUNNING");
    error(ErrorCode::NotReady, [&] { f.issue(); }); error(ErrorCode::NotReady, [&] { f.prepare(); });
}

void invalid_snapshots_never_submit_or_mutate_task() {
    Fixture f; f.ready(); f.bind(); const auto before = f.task(); const auto original = f.snapshot();
    auto snapshot = original; snapshot.as_array()[0].at("sha256") = std::string(64, '0');
    error(ErrorCode::InvalidInput, [&] { f.store->submit_work(f.actor(Role::Worker), f.binding(), f.tv(), submission_body(), snapshot, "bad-hash"); });
    snapshot = original; snapshot.as_array()[0].at("size_bytes") = 100;
    error(ErrorCode::InvalidInput, [&] { f.store->submit_work(f.actor(Role::Worker), f.binding(), f.tv(), submission_body(), snapshot, "bad-size"); });
    snapshot = original; snapshot.as_array()[0].at("content_base64") = "YQ==\n";
    error(ErrorCode::InvalidInput, [&] { f.store->submit_work(f.actor(Role::Worker), f.binding(), f.tv(), submission_body(), snapshot, "bad-base64"); });
    snapshot = Array{snapshot_file("/work/result.txt", "a")}; snapshot.as_array()[0].at("content_base64") = "YR==";
    error(ErrorCode::InvalidInput, [&] { f.store->submit_work(f.actor(Role::Worker), f.binding(), f.tv(), submission_body(), snapshot, "noncanonical-base64"); });
    snapshot = original; snapshot.as_array()[0].at("path") = "/work/../escape";
    error(ErrorCode::InvalidInput, [&] { f.store->submit_work(f.actor(Role::Worker), f.binding(), f.tv(), submission_body(), snapshot, "bad-path"); });
    error(ErrorCode::InvalidInput, [&] { f.store->submit_work(f.actor(Role::Worker), f.binding(), f.tv(), submission_body(), Array{}, "missing-snapshot"); });
    CHECK(f.task() == before); CHECK(f.store->submissions(f.tid()).empty());
}

void snapshot_capacity_bounds_and_atomic_failure() {
    Fixture f; f.ready(); f.bind(); const auto before = f.task();
    auto body = submission_body(); Array files; Array deliverables;
    for (int i = 0; i < 5; ++i) {
        const auto path = "/work/file" + std::to_string(i); files.emplace_back(snapshot_file(path, std::string(65536, 'a')));
        deliverables.emplace_back(Object{{"path", path}, {"description", "bounded snapshot fixture"}});
    }
    body.at("deliverables") = deliverables;
    error(ErrorCode::Quota, [&] { f.store->submit_work(f.actor(Role::Worker), f.binding(), f.tv(), body, files, "oversize-total"); });
    error(ErrorCode::InvalidInput, [&] { f.store->submit_work(f.actor(Role::Worker), f.binding(), f.tv(), submission_body(),
        Array{snapshot_file("/work/result.txt", std::string(65537, 'a'))}, "oversize-file"); });
    raw_exec(f.path, "CREATE TRIGGER reject_submission_event BEFORE INSERT ON events WHEN NEW.type='submit_work' BEGIN SELECT RAISE(ABORT,'fixture refuses event'); END;");
    error(ErrorCode::Conflict, [&] { f.submit(); }); CHECK(f.task() == before); CHECK(f.store->submissions(f.tid()).empty());
}

void submission_requires_grants_workspace_applied_and_settled_ops() {
    Fixture f; f.ready();
    error(ErrorCode::NotReady, [&] { f.store->submit_work(f.actor(Role::Worker), f.binding(), f.tv(), submission_body(), Array{snapshot_file("/work/result.txt", "fixture")}, "unbound-submit"); });
    f.bind(); const auto prepared = f.prepare();
    error(ErrorCode::NotReady, [&] { f.submit(); });
    f.store->cancel_operation(f.actor(Role::Worker), s(prepared, "op_id"), 1, "cancel-prepared");
    f.issue(); error(ErrorCode::NotReady, [&] { f.submit(); }); f.ack();
    raw_exec(f.path, "UPDATE runtime_policy SET denied_tools='[\"submit\"]',policy_epoch=policy_epoch+1,state_version=state_version+1 WHERE singleton=1;");
    error(ErrorCode::Unauthorized, [&] { f.submit(); });
}

void submitted_state_prevents_replacement_or_direct_execution() {
    Fixture f; f.ready(); f.bind(); f.ready("beta"); const auto submitted = f.submit();
    error(ErrorCode::NotReady, [&] { f.submit(); }); error(ErrorCode::NotReady, [&] { f.prepare(); });
    error(ErrorCode::NotReady, [&] { f.issue(); });
    const auto before = f.task();
    error(ErrorCode::Unauthorized, [&] { f.store->check_submission(f.actor(Role::Worker), f.binding(), f.tv(), s(submitted, "submission_id"), 1, check_body(), nullptr, "worker-self-approve"); });
    error(ErrorCode::Unauthorized, [&] { f.store->check_submission(f.actor(Role::TeamLead, "beta"), f.binding("beta"), f.tv("beta"), s(submitted, "submission_id"), 1, check_body(), nullptr, "foreign-check"); });
    CHECK(f.task() == before); CHECK(!f.prepare("beta").is_null());
}

void passing_check_requires_complete_noncontradictory_evidence() {
    Fixture f; f.ready(); f.bind(); const auto submitted = f.submit(); const auto before = f.task();
    auto check = check_body(); check.at("criteria_results").as_array().pop_back();
    error(ErrorCode::InvalidInput, [&] { f.store->check_submission(f.actor(Role::TeamLead), f.binding(), f.tv(), s(submitted, "submission_id"), 1, check, nullptr, "missing-criterion"); });
    check = check_body(); check.at("criteria_results").as_array()[1].at("criterion_id") = "ac1";
    error(ErrorCode::InvalidInput, [&] { f.store->check_submission(f.actor(Role::TeamLead), f.binding(), f.tv(), s(submitted, "submission_id"), 1, check, nullptr, "duplicate-criterion"); });
    check = check_body(); check.at("criteria_results").as_array()[1].at("met") = "partial";
    error(ErrorCode::InvalidInput, [&] { f.store->check_submission(f.actor(Role::TeamLead), f.binding(), f.tv(), s(submitted, "submission_id"), 1, check, nullptr, "contradictory-check"); });
    error(ErrorCode::InvalidInput, [&] { f.store->check_submission(f.actor(Role::TeamLead), f.binding(), f.tv(), s(submitted, "submission_id"), 1, Object{{"passed", true}}, nullptr, "no-criteria"); });
    CHECK(f.task() == before); CHECK(f.store->submission(s(submitted, "submission_id")).at("checks").as_array().empty());
    CHECK(s(f.check(submitted), "status") == "CANDIDATE_READY");
    for (int variant = 0; variant < 5; ++variant) {
        Fixture g; g.ready(); g.bind(); auto body = submission_body();
        if (variant == 0) body.at("self_check").as_array()[0].at("met") = "no";
        if (variant == 1) body.at("self_check").as_array()[0].as_object().erase("evidence");
        if (variant == 2) body.at("verification").as_array()[0].at("result") = "not_run";
        if (variant == 3) body.at("verification").as_array()[0].as_object().erase("evidence");
        if (variant == 4) body.as_object()["known_issues"] = Array{"unresolved fixture issue"};
        const auto negative = g.submit("alpha", body); error(ErrorCode::InvalidInput, [&] { g.check(negative); });
        CHECK(s(g.check(negative, false), "status") == "RETURNED_FOR_REWORK");
    }
}

void changed_policy_needs_current_negative_check_before_resubmission() {
    Fixture f; f.ready(); f.bind(); const auto submitted = f.submit();
    const auto lead = f.actor(Role::TeamLead); const auto old_binding = f.binding(); const auto old_version = f.tv();
    raw_exec(f.path, "UPDATE runtime_policy SET denied_tools='[\"run\"]',policy_epoch=policy_epoch+1,state_version=state_version+1 WHERE singleton=1;");
    error(ErrorCode::Stale, [&] { f.check(submitted); });
    error(ErrorCode::Stale, [&] { f.store->check_submission(lead, old_binding, old_version, s(submitted, "submission_id"), 1, check_body(false), order(), "old-policy-rework"); });
    const auto returned = f.check(submitted, false); CHECK(s(returned, "status") == "RETURNED_FOR_REWORK");
    CHECK(returned.at("original_binding") == submitted.at("original_binding"));
    CHECK(n(returned.at("checks").as_array().back().at("captured_binding"), "policy_epoch") == old_binding.policy_epoch + 1);
    CHECK(f.binding().policy_epoch == old_binding.policy_epoch + 1); f.ack();
    const auto current = f.submit(); CHECK(n(current.at("original_binding"), "policy_epoch") == old_binding.policy_epoch + 1);
    const auto ready = f.check(current); CHECK(s(ready, "status") == "CANDIDATE_READY");
    raw_exec(f.path, "UPDATE runtime_policy SET policy_epoch=policy_epoch+1,state_version=state_version+1 WHERE singleton=1;");
    CHECK(s(f.check(ready, false), "status") == "RETURNED_FOR_REWORK");
}

void rework_versions_preserve_target_require_ack_and_remain_bounded() {
    Fixture f; f.ready(); f.bind(); const auto original = f.binding(); const auto original_brief = f.task().at("brief");
    const auto first = f.submit(); const auto returned = f.check(first, false);
    CHECK(s(returned, "status") == "RETURNED_FOR_REWORK"); CHECK(n(returned, "team_rework_count") == 1);
    const auto current = f.binding(); CHECK(current.task_revision == original.task_revision + 1);
    CHECK(current.instruction_revision == original.instruction_revision + 1); CHECK(current.requirements_revision == original.requirements_revision);
    CHECK(current.plan_revision == original.plan_revision); CHECK(f.task().at("brief") == original_brief);
    CHECK(n(f.task(), "applied_task_revision") < current.task_revision); CHECK(n(f.task(), "applied_instruction_revision") < current.instruction_revision);
    error(ErrorCode::NotReady, [&] { f.submit(); }); f.ack();
    for (std::int64_t round = 2; round <= 3; ++round) {
        const auto next = f.check(f.submit(), false);
        CHECK(s(next, "status") == "RETURNED_FOR_REWORK"); CHECK(n(next, "team_rework_count") == round); f.ack();
    }
    const auto second = f.submit(); const auto blocked = f.check(second, false);
    CHECK(s(blocked, "status") == "BLOCKED"); CHECK(s(f.task(), "status") == "BLOCKED");
    CHECK(n(blocked, "team_rework_count") == 3); error(ErrorCode::NotReady, [&] { f.issue(); });
    f.reopen(); CHECK(s(f.store->current_submission(f.tid()), "status") == "BLOCKED");
    error(ErrorCode::NotReady, [&] { f.prepare(); });
    CHECK(f.store->submissions(f.tid()).size() == 4); error(ErrorCode::Quota, [&] { f.store->submissions(f.tid(), 1); });
}

void rework_invalid_constraints_and_storage_failures_roll_back() {
    Fixture f; f.ready(); f.bind(); const auto submitted = f.submit(); const auto task = f.task();
    auto broken = order(); broken.at("constraint_refs") = Array{}; broken.at("constraints_verbatim") = Array{};
    error(ErrorCode::InvalidInput, [&] { f.store->check_submission(f.actor(Role::TeamLead), f.binding(), f.tv(), s(submitted, "submission_id"), 1, check_body(false), broken, "bad-rework"); });
    error(ErrorCode::InvalidInput, [&] { f.store->check_submission(f.actor(Role::TeamLead), f.binding(), f.tv(), s(submitted, "submission_id"), 1, check_body(false), nullptr, "missing-rework"); });
    raw_exec(f.path, "CREATE TRIGGER reject_rework_event BEFORE INSERT ON events WHEN NEW.type='check_submission' BEGIN SELECT RAISE(ABORT,'fixture refuses event'); END;");
    error(ErrorCode::Conflict, [&] { f.check(submitted, false); });
    CHECK(f.task() == task); CHECK(f.store->submission(s(submitted, "submission_id")) == submitted);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM instructions") == 1); CHECK(scalar(f.path, "SELECT COUNT(*) FROM team_workflow_state") == 0);
}

void concurrent_checks_have_one_captured_winner_and_replay_is_exact() {
    Fixture f; f.ready(); f.bind(); const auto submitted = f.submit(); const auto lead = f.actor(Role::TeamLead);
    const auto binding = f.binding(); const auto version = f.tv(); std::barrier gate(3);
    const auto check = [&](const std::string& command) {
        gate.arrive_and_wait();
        try { f.store->check_submission(lead, binding, version, s(submitted, "submission_id"), 1, check_body(), nullptr, command); return true; }
        catch (const Error& value) { CHECK(value.code() == ErrorCode::NotReady || value.code() == ErrorCode::Conflict); return false; }
    };
    auto first = std::async(std::launch::async, [&] { return check("concurrent-check-a"); });
    auto second = std::async(std::launch::async, [&] { return check("concurrent-check-b"); });
    gate.arrive_and_wait(); const bool first_won = first.get(); CHECK(first_won != second.get());
    const auto ready = f.store->submission(s(submitted, "submission_id")); CHECK(ready.at("checks").as_array().size() == 1);
    f.reopen(); CHECK(f.store->check_submission(lead, binding, version, s(submitted, "submission_id"), 1, check_body(), nullptr,
        first_won ? "concurrent-check-a" : "concurrent-check-b") == ready);
}

void normal_check_capacity_does_not_borrow_adverse_revocation_reserve() {
    Fixture f; f.ready(); f.bind(); const auto submitted = f.submit();
    raw_exec(f.path, "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<4096-(SELECT COUNT(*) FROM events)) INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) SELECT 'capacity-'||x,(SELECT request_id FROM requests LIMIT 1),'fixture','runtime','capacity-'||x,printf('%064d',0),'{}','{}','2026-10-19T00:00:00Z' FROM n;");
    error(ErrorCode::Quota, [&] { f.check(submitted); }); error(ErrorCode::Quota, [&] { f.check(submitted, false); });
    CHECK(s(f.store->submission(s(submitted, "submission_id")), "status") == "SUBMITTED");
    const auto question = f.ask(); CHECK(s(f.answer(question), "status") == "ANSWERED");
    Fixture g; g.ready(); g.bind(); const auto ready = g.check(g.submit());
    raw_exec(g.path, "WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<4096-(SELECT COUNT(*) FROM events)) INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) SELECT 'capacity-'||x,(SELECT request_id FROM requests LIMIT 1),'fixture','runtime','capacity-'||x,printf('%064d',0),'{}','{}','2026-10-19T00:00:00Z' FROM n;");
    const auto returned = g.check(ready, false); CHECK(s(returned, "status") == "RETURNED_FOR_REWORK");
    CHECK(returned.at("checks").as_array().size() == 2); CHECK(!returned.at("business_accepted").as_bool());
}

void submitted_records_and_question_originals_reject_tampering() {
    Fixture f; f.ready(); f.bind(); const auto q = f.ask(false); const auto submitted = f.submit();
    try { raw_exec(f.path, "UPDATE questions SET body='{\"text\":\"changed\"}',state_version=state_version+1;"); throw std::runtime_error("expected immutable question trigger"); }
    catch (const std::runtime_error& value) { CHECK(std::string(value.what()).find("immutable original question") != std::string::npos); }
    try { raw_exec(f.path, "UPDATE workflow_submissions SET artifact_snapshot='[]',state_version=state_version+1;"); throw std::runtime_error("expected immutable submission trigger"); }
    catch (const std::runtime_error& value) { CHECK(std::string(value.what()).find("immutable original submission") != std::string::npos); }
    CHECK(f.store->question(s(q, "question_id")) == q); CHECK(f.store->submission(s(submitted, "submission_id")) == submitted);
}
}

int main() {
    source_root = std::filesystem::path(WLS_SOURCE_DIR);
    base_sql = read(source_root / "db/schema.sql"); store_sql = read(source_root / "db/migrations/004_s1_store.sql");
    tools_sql = read(source_root / "db/migrations/005_worker_tools.sql"); events_sql = read(source_root / "db/migrations/006_event_runtime.sql");
    workflow_sql = read(source_root / "db/migrations/007_workflow_runtime.sql");
    return test::run({
        {"405 upgrade, rollback, read-only and fingerprints", upgrade_readonly_rollback_and_fingerprints},
        {"question wait is atomic, scoped and cannot be bypassed", question_wait_is_atomic_scoped_and_not_bypassable},
        {"question forwarding preserves original chain and owners", question_forwards_preserve_original_chain_and_ownership},
        {"question controls do not resolve UNKNOWN", question_control_survives_unknown_without_resolving_it},
        {"current question policy control rejects stale captures without deadlock", question_current_policy_control_rejects_old_capture_without_deadlock},
        {"AMEND supersedes original question and submission", unanswered_question_and_submission_are_superseded_by_amend},
        {"question retries and exact answer selection", question_idempotency_conflicts_and_exact_selection},
        {"question failure rolls back wait and row", question_atomic_failure_leaves_no_wait_or_row},
        {"nonblocking questions and bounded history", nonblocking_question_keeps_worker_ready_and_uses_bounded_history},
        {"submission retains original binary snapshot across restart", snapshot_bytes_are_immutable_across_file_changes_and_recovery},
        {"invalid snapshot hashes, bytes, paths and coverage", invalid_snapshots_never_submit_or_mutate_task},
        {"snapshot capacities and submission atomic rollback", snapshot_capacity_bounds_and_atomic_failure},
        {"submission needs grants, workspace, applied and settled ops", submission_requires_grants_workspace_applied_and_settled_ops},
        {"submission hold and TeamLead ownership", submitted_state_prevents_replacement_or_direct_execution},
        {"passing check requires complete consistent evidence", passing_check_requires_complete_noncontradictory_evidence},
        {"changed policy permits current negative checks before resubmission", changed_policy_needs_current_negative_check_before_resubmission},
        {"rework revisions, acknowledgement and frozen bound", rework_versions_preserve_target_require_ack_and_remain_bounded},
        {"invalid rework and failed storage roll back", rework_invalid_constraints_and_storage_failures_roll_back},
        {"one concurrent captured check and exact replay", concurrent_checks_have_one_captured_winner_and_replay_is_exact},
        {"normal checks and reserved adverse revocation capacity", normal_check_capacity_does_not_borrow_adverse_revocation_reserve},
        {"original question and snapshot immutability", submitted_records_and_question_originals_reject_tampering}
    });
}
