#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "wls/model.hpp"
#include "test_support.hpp"
#include "store_actor_support.hpp"
#include "capacity_test_support.hpp"

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
std::string base_sql, store_sql, tools_sql, events_sql, workflow_sql, acceptance_sql;
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
    explicit Fixture(bool acceptance = true, const Json& initial_plan = plan_body()) {
        store = std::make_unique<Store>(path, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql, workflow_sql, acceptance ? acceptance_sql : "");
        request_id = s(store->admit(admission()), "request_id");
        store->apply_independent_plan(actor(Role::Dispatcher), rv(), 1, initial_plan, Object{}, new_id("plan"));
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
    void reopen() { store.reset(); store = std::make_unique<Store>(path, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql, workflow_sql, acceptance_sql); }
    void amend() { store->amend(actor(Role::Root), rv(), "review amended fixtures", admission().constraint_catalog, new_id("amend")); }
    Json prepare(const std::string& node = "alpha") {
        return store->prepare_tool_operation(actor(Role::Worker, node), binding(node), tv(node), new_id("business"), "note", Object{{"text", "fixture"}}, new_id("prepare"));
    }
};

Json candidate(Fixture& f, const std::string& node = "alpha") {
    f.ready(node); f.bind(node); const auto submitted = f.submit(node);
    return f.store->check_submission(f.actor(Role::TeamLead, node), f.binding(node), f.tv(node), s(submitted, "submission_id"),
        n(submitted, "state_version"), check_body(), nullptr, new_id("check"));
}
Json decision(bool passed = true) {
    return Object{{"passed", passed}, {"notes", "Dispatcher inspected the original fixture evidence."}, {"criteria_results", submission_body().at("self_check")}};
}
Json accept(Fixture& f, const Json& submitted, const std::string& node = "alpha") {
    return f.store->accept_candidate(f.actor(Role::Dispatcher), f.store->request_binding(f.request_id), f.tv(node),
        s(submitted, "submission_id"), n(submitted, "state_version"), decision(), new_id("accept"));
}
Json root_identity(Fixture& f) {
    const auto path = f.temp.path / "delivery"; std::filesystem::create_directories(path); struct stat info{};
    CHECK(::stat(path.c_str(), &info) == 0);
    return Object{{"path", path.string()}, {"device", static_cast<std::int64_t>(info.st_dev)}, {"inode", static_cast<std::int64_t>(info.st_ino)}};
}
Json prepare(Fixture& f) {
    return f.store->prepare_delivery(f.actor(Role::Root), f.store->request_binding(f.request_id), f.rv(), root_identity(f), new_id("delivery"));
}
Json publication(const Json& delivery) {
    const auto& plan = delivery.at("frozen_plan");
    return Object{{"delivery_id", delivery.at("delivery_id")}, {"root_identity", plan.at("root_identity")},
        {"path", s(plan.at("root_identity"), "path") + "/" + s(delivery, "delivery_id")},
        {"manifest_sha256", plan.at("manifest_sha256")}, {"files_sha256", sha256_hex(canonical_json(plan.at("manifest").at("files")))},
        {"verified", true}, {"durable", true}};
}
Json publish(Fixture& f, const Json& delivery) {
    return f.store->record_delivery_publication(f.actor(Role::Root), f.store->request_binding(f.request_id), s(delivery, "delivery_id"),
        n(delivery, "state_version"), publication(delivery), new_id("publication"));
}
Json ack(Fixture& f, const Json& delivery, const std::string& key = "exact-receipt") {
    return f.store->acknowledge_delivery(f.actor(Role::Root), f.store->request_binding(f.request_id), s(delivery, "delivery_id"),
        s(delivery, "manifest_sha256"), n(delivery, "requirements_revision"), n(delivery, "plan_revision"), "local-owner", "durable_local", key, publication(delivery));
}
Json report_body(const Json& job) {
    return Object{{"report_id", new_id("rep")}, {"request_id", job.at("request_id")}, {"task_id", job.at("task_id")},
        {"team_id", job.at("team_id")}, {"no_new_experience", true}, {"entries", Array{}}, {"memory_assessments", Array{}}};
}
Json report(Fixture& f, const Json& job, const Json& body, const std::string& command = "report") {
    return f.store->report_experience(f.store->host_actor(Role::TeamLead, f.request_id, s(job, "team_id")), f.store->binding(s(job, "task_id")),
        s(job, "experience_id"), n(job, "state_version"), s(job, "source_sha256"), body, command);
}
void accept_both(Fixture& f) { accept(f, candidate(f)); accept(f, candidate(f, "beta"), "beta"); }

void migration_readonly_rollback_and_asset_integrity() {
    Fixture f(false); CHECK(!f.store->acceptance_runtime_enabled()); const auto before = f.store->request(f.request_id);
    error(ErrorCode::Unsupported, [&] { f.store->deliveries(f.request_id); });
    { Store ro(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql, events_sql, workflow_sql, acceptance_sql);
      CHECK(!ro.acceptance_runtime_enabled()); CHECK(ro.request(f.request_id) == before); }
    f.store.reset();
    error(ErrorCode::Storage, [&] { Store bad(f.path, base_sql, store_sql, StoreOpenMode::Takeover, tools_sql, events_sql, workflow_sql,
        acceptance_sql + "\nSELECT * FROM missing_acceptance_upgrade_fixture;\n"); });
    CHECK(scalar(f.path, "PRAGMA user_version") == 405);
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM sqlite_master WHERE name='candidate_acceptances'") == 0);
    f.reopen(); CHECK(f.store->acceptance_runtime_enabled()); CHECK(n(f.store->diagnostics(), "user_version") == 406);
    CHECK(f.store->request(f.request_id).at("config_snapshot") == before.at("config_snapshot"));
    error(ErrorCode::Unsupported, [&] { Store ro(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql, events_sql, workflow_sql); });
    error(ErrorCode::Corrupt, [&] { Store ro(f.path, base_sql, store_sql, StoreOpenMode::ReadOnly, tools_sql, events_sql, workflow_sql, acceptance_sql + "\n-- changed"); });
}
void positive_acceptance_atomic_job_and_original_bytes() {
    Fixture f; const auto c = candidate(f); const auto before = f.store->submission(s(c, "submission_id"));
    CHECK(!before.at("business_accepted").as_bool());
    const auto accepted = accept(f, c); CHECK(accepted.at("business_accepted").as_bool());
    CHECK(!accepted.at("business_delivered").as_bool()); CHECK(s(f.task(), "status") == "ACCEPTED");
    const auto jobs = f.store->experience_jobs(f.request_id, f.tid()); CHECK(jobs.size() == 1);
    CHECK(jobs[0].at("source_snapshot").at("submission").at("artifact_snapshot") == before.at("artifact_snapshot"));
    CHECK(s(jobs[0], "status") == "PENDING"); CHECK(s(f.task("beta"), "status") == "READY");
    CHECK(scalar(f.path, "SELECT COUNT(*) FROM candidate_acceptances WHERE status='ACCEPTED'") == 1);
    error(ErrorCode::NotReady, [&] { f.prepare(); });
}
void role_scope_binding_and_cas_cannot_accept() {
    Fixture f; const auto c = candidate(f); const auto b = f.store->request_binding(f.request_id);
    error(ErrorCode::Unauthorized, [&] { f.store->accept_candidate(f.actor(Role::Root), b, f.tv(), s(c,"submission_id"), n(c,"state_version"), decision(), "root-accept"); });
    auto stale = b; ++stale.policy_epoch;
    error(ErrorCode::Stale, [&] { f.store->accept_candidate(f.actor(Role::Dispatcher), stale, f.tv(), s(c,"submission_id"), n(c,"state_version"), decision(), "stale-policy"); });
    error(ErrorCode::Conflict, [&] { f.store->accept_candidate(f.actor(Role::Dispatcher), b, f.tv()+1, s(c,"submission_id"), n(c,"state_version"), decision(), "stale-task"); });
    error(ErrorCode::Conflict, [&] { f.store->accept_candidate(f.actor(Role::Dispatcher), b, f.tv(), s(c,"submission_id"), n(c,"state_version")+1, decision(), "stale-sub"); });
    CHECK(f.store->experience_jobs(f.request_id).empty()); CHECK(s(f.task(),"status") == "SUBMITTED");
}
void evidence_checks_and_required_review_fail_closed() {
    Fixture f; const auto c = candidate(f); auto missing = decision(); missing.as_object().erase("criteria_results");
    error(ErrorCode::InvalidInput, [&] { f.store->accept_candidate(f.actor(Role::Dispatcher), f.store->request_binding(f.request_id), f.tv(), s(c,"submission_id"), n(c,"state_version"), missing, "missing-criteria"); });
    auto contradictory = decision(); contradictory.at("criteria_results").as_array()[0].as_object()["met"] = "no";
    error(ErrorCode::InvalidInput, [&] { f.store->accept_candidate(f.actor(Role::Dispatcher), f.store->request_binding(f.request_id), f.tv(), s(c,"submission_id"), n(c,"state_version"), contradictory, "failed-criteria"); });
    raw_exec(f.path, "UPDATE tasks SET review_config='{}',state_version=state_version+1 WHERE node_id='alpha'");
    error(ErrorCode::Unsupported, [&] { accept(f,c); });
    CHECK(f.store->experience_jobs(f.request_id).empty());
}
void exact_replay_and_recovery_preserve_original_acceptance() {
    Fixture f; const auto c = candidate(f); const auto actor = f.actor(Role::Dispatcher); const auto binding = f.store->request_binding(f.request_id); const auto tv = f.tv();
    const auto first = f.store->accept_candidate(actor,binding,tv,s(c,"submission_id"),n(c,"state_version"),decision(),"one-accept");
    CHECK(f.store->accept_candidate(actor,binding,tv,s(c,"submission_id"),n(c,"state_version"),decision(),"one-accept") == first);
    f.reopen();
    const auto events = f.store->events(f.request_id);
    const auto original = f.store->submission(s(c,"submission_id"));
    const auto jobs = f.store->experience_jobs(f.request_id);
    error(ErrorCode::Stale, [&] { f.store->accept_candidate(actor,binding,tv,s(c,"submission_id"),n(c,"state_version"),decision(),"one-accept"); });
    CHECK(f.store->events(f.request_id) == events && f.store->submission(s(c,"submission_id")) == original);
    CHECK(f.store->experience_jobs(f.request_id) == jobs);
    const auto replay_dispatcher = test::replay_actor(*f.store, actor);
    CHECK(f.store->accept_candidate(replay_dispatcher,binding,tv,s(c,"submission_id"),n(c,"state_version"),decision(),"one-accept") == first);
    error(ErrorCode::Stale, [&] { f.store->accept_candidate(replay_dispatcher,binding,tv,s(c,"submission_id"),n(c,"state_version"),decision(),"new-accept-old-epoch"); });
    CHECK(f.store->submission(s(c,"submission_id")).at("acceptance").at("producer_binding") == c.at("original_binding"));
    CHECK(f.store->experience_jobs(f.request_id).size() == 1);
    auto changed = decision(); changed.as_object()["notes"] = "different decision";
    error(ErrorCode::Conflict, [&] { f.store->accept_candidate(replay_dispatcher,binding,tv,s(c,"submission_id"),n(c,"state_version"),changed,"one-accept"); });
    CHECK(f.store->events(f.request_id) == events && f.store->submission(s(c,"submission_id")) == original);
    CHECK(f.store->experience_jobs(f.request_id) == jobs);
}
void negative_control_revokes_accepted_delivery() {
    Fixture f; accept_both(f); const auto delivery = prepare(f);
    const auto candidate_now = f.store->current_submission(f.tid());
    const auto rejected = f.store->accept_candidate(f.actor(Role::Dispatcher), f.store->request_binding(f.request_id), f.tv(),
        s(candidate_now,"submission_id"),n(candidate_now,"state_version"),decision(false),"negative-revoke");
    CHECK(!rejected.at("business_accepted").as_bool()); CHECK(s(f.task(),"status") == "BLOCKED");
    CHECK(s(f.store->delivery(s(delivery,"delivery_id")),"status") == "SUPERSEDED");
    error(ErrorCode::NotReady, [&] { prepare(f); });
    CHECK(f.store->experience_jobs(f.request_id).size() == 2);
}
void amendment_invalidates_package_but_historical_experience_survives() {
    Fixture f; accept_both(f); const auto d = prepare(f); const auto job = f.store->experience_jobs(f.request_id,f.tid()).front();
    f.amend(); CHECK(s(f.store->delivery(s(d,"delivery_id")),"status") == "SUPERSEDED");
    CHECK(!f.store->submission(s(job.at("source_snapshot").at("submission"),"submission_id")).at("business_accepted").as_bool());
    const auto done = report(f,job,report_body(job)); CHECK(s(done,"status") == "DONE"); CHECK(done.at("source_snapshot") == job.at("source_snapshot"));
    f.store->apply_independent_plan(f.actor(Role::Dispatcher),f.rv(),2,plan_body(),Object{{"alpha",Object{{"disposition","keep"},{"rationale","inspect original fixture under amendment"}}}},"new-plan");
    CHECK(s(f.task(),"status") == "READY"); CHECK(n(f.task(),"task_revision") == 2); CHECK(s(f.task("beta"),"status") == "BLOCKED");
}
void required_tasks_and_frozen_original_package_contents() {
    Fixture f; accept(f,candidate(f)); error(ErrorCode::NotReady,[&] { prepare(f); });
    accept(f,candidate(f,"beta"),"beta"); write(f.workspace()/"result.txt","changed after submission\n");
    const auto d = prepare(f); const auto plan = d.at("frozen_plan"); CHECK(s(plan.at("manifest"),"format") == "wls.delivery-package.v1");
    bool original = false;
    for (const auto& file : plan.at("files").as_array()) if (s(file,"name") == "artifact-alpha-1.bin") original = s(file,"content_base64") == base64("original fixture\n");
    CHECK(original); CHECK(s(plan,"manifest_sha256") == sha256_hex(canonical_json(plan.at("manifest"))));
    const auto again = prepare(f); CHECK(s(again,"delivery_id") == s(d,"delivery_id")); CHECK(again.at("frozen_plan") == plan);
    CHECK(f.store->deliveries(f.request_id).size() == 1);
}
void optional_task_exclusion_is_explicit() {
    Fixture f; accept(f,candidate(f)); raw_exec(f.path,"UPDATE tasks SET required=0,state_version=state_version+1 WHERE node_id='beta'");
    const auto d = prepare(f); CHECK(d.at("frozen_plan").at("manifest").at("limitations").as_array().size() == 1);
    CHECK(d.at("frozen_plan").at("manifest").at("artifacts").as_array().size() == 1);
}
void publication_guards_recovery_and_readonly_queries() {
    Fixture f; accept_both(f); auto d = prepare(f); auto bad = publication(d); bad.as_object()["durable"] = false;
    error(ErrorCode::NotReady,[&] { f.store->record_delivery_publication(f.actor(Role::Root),f.store->request_binding(f.request_id),s(d,"delivery_id"),n(d,"state_version"),bad,"not-durable"); });
    bad = publication(d); bad.as_object()["files_sha256"] = std::string(64,'0');
    error(ErrorCode::Conflict,[&] { f.store->record_delivery_publication(f.actor(Role::Root),f.store->request_binding(f.request_id),s(d,"delivery_id"),n(d,"state_version"),bad,"wrong-files"); });
    const auto frozen = d.at("frozen_plan"); f.reopen(); d = f.store->delivery(s(d,"delivery_id")); CHECK(d.at("frozen_plan") == frozen);
    const auto stored = publish(f,d); CHECK(s(stored,"status") == "STORED"); CHECK(stored.at("receipt").is_null());
    const auto before = f.store->request(f.request_id);
    { Store ro(f.path,base_sql,store_sql,StoreOpenMode::ReadOnly,tools_sql,events_sql,workflow_sql,acceptance_sql);
      CHECK(ro.delivery(s(d,"delivery_id")).at("receipt").is_null()); CHECK(ro.request(f.request_id) == before); }
}
void exact_receipt_and_closeout_are_atomic_and_replayable() {
    Fixture f; accept_both(f); auto d = publish(f,prepare(f));
    error(ErrorCode::Stale,[&] { f.store->acknowledge_delivery(f.actor(Role::Root),f.store->request_binding(f.request_id),s(d,"delivery_id"),std::string(64,'0'),1,1,"local-owner","durable_local","wrong-hash",publication(d)); });
    CHECK(s(f.store->request(f.request_id),"status") == "DELIVERING"); CHECK(f.store->closeout(f.request_id).at("cleanup_job").is_null());
    const auto confirmed = ack(f,d); CHECK(s(confirmed,"status") == "CONFIRMED"); CHECK(s(f.store->request(f.request_id),"status") == "DELIVERED");
    CHECK(!f.store->closeout(f.request_id).at("cleanup_job").is_null()); CHECK(!f.store->closeout(f.request_id).at("cleanup_complete").as_bool());
    f.reopen(); CHECK(ack(f,d) == confirmed);
    error(ErrorCode::Conflict,[&] { f.store->acknowledge_delivery(f.actor(Role::Root),f.store->request_binding(f.request_id),s(d,"delivery_id"),s(d,"manifest_sha256"),1,1,"different-recipient","durable_local","exact-receipt",publication(d)); });
    CHECK(scalar(f.path,"SELECT COUNT(*) FROM delivery_receipts") == 1); CHECK(scalar(f.path,"SELECT COUNT(*) FROM cleanup_jobs") == 1);
}
void terminal_experience_and_takeover_fence_own_job() {
    Fixture f; accept_both(f); ack(f,publish(f,prepare(f)));
    const auto old = f.store->experience_jobs(f.request_id,f.tid()).front(); const auto body = report_body(old);
    f.reopen(); error(ErrorCode::Conflict,[&] { report(f,old,body); });
    const auto current = f.store->experience_job(s(old,"experience_id")); CHECK(n(current,"lease_epoch") > n(old,"lease_epoch"));
    const auto done = report(f,current,body); CHECK(s(done,"status") == "DONE");
    CHECK(done.at("source_snapshot") == old.at("source_snapshot")); CHECK(s(f.store->request(f.request_id),"status") == "DELIVERED");
    CHECK(s(f.store->closeout(f.request_id).at("cleanup_job"),"status") == "PENDING");
}
void report_identity_source_and_explicit_no_experience_guards() {
    Fixture f; const auto a = accept(f,candidate(f)); const auto job = f.store->experience_job(s(a,"experience_id"));
    auto body = report_body(job); body.as_object()["no_new_experience"] = false;
    error(ErrorCode::InvalidInput,[&] { report(f,job,body); });
    body = report_body(job); body.as_object()["team_id"] = "team-other";
    error(ErrorCode::Unauthorized,[&] { report(f,job,body); });
    body = report_body(job);
    error(ErrorCode::Conflict,[&] { f.store->report_experience(f.actor(Role::TeamLead),f.binding(),s(job,"experience_id"),n(job,"state_version"),std::string(64,'0'),body,"source-forgery"); });
    const auto done = report(f,job,body); CHECK(report(f,job,body) == done);
    CHECK(scalar(f.path,"SELECT COUNT(*) FROM memory_meta") == 0); CHECK(scalar(f.path,"SELECT COUNT(*) FROM experience_reports") == 1);
}
void cancellation_retains_jobs_unknown_and_recovery() {
    Fixture f; f.ready(); const auto prepared = f.prepare(); const auto op = f.store->start_operation(f.actor(Role::Worker), s(prepared,"op_id"), n(prepared,"state_version"), "run-fixture");
    (void)op;
    f.store->cancel_request(f.actor(Role::Root),f.rv(),"cancel");
    auto state = f.store->closeout(f.request_id); CHECK(!state.at("cleanup_job").is_null());
    CHECK(s(f.store->operation(s(prepared,"op_id")),"status") == "UNKNOWN");
    f.reopen(); state = f.store->closeout(f.request_id); CHECK(!state.at("cleanup_complete").as_bool());
    CHECK(n(state.at("cleanup_job"),"lease_epoch") > 0); CHECK(s(state.at("receipt"),"final_status") == "CANCELLED");
}
void delivery_workspace_database_isolation_is_bidirectional() {
    Fixture f; accept_both(f); const auto root = root_identity(f);
    struct stat info{}; CHECK(::stat(f.workspace().c_str(),&info) == 0);
    const Json inside = Object{{"path",f.workspace().string()},{"device",static_cast<std::int64_t>(info.st_dev)},{"inode",static_cast<std::int64_t>(info.st_ino)}};
    error(ErrorCode::Unauthorized,[&] { f.store->prepare_delivery(f.actor(Role::Root),f.store->request_binding(f.request_id),f.rv(),inside,"workspace-delivery"); });
    auto db_root = root; db_root.as_object()["path"] = f.temp.path.string();
    error(ErrorCode::Unauthorized,[&] { f.store->prepare_delivery(f.actor(Role::Root),f.store->request_binding(f.request_id),f.rv(),db_root,"database-delivery"); });
    (void)prepare(f);
    auto other = admission(); const auto request_id = s(f.store->admit(other),"request_id");
    f.store->apply_independent_plan(f.store->host_actor(Role::Dispatcher,request_id),n(f.store->request(request_id),"state_version"),1,plan_body(),Object{},"other-plan");
    const auto team = s(f.store->tasks(request_id).front(),"team_id");
    error(ErrorCode::Unauthorized,[&] { f.store->bind_workspace(team,s(root,"path"),static_cast<std::uint64_t>(n(root,"device")),static_cast<std::uint64_t>(n(root,"inode"))); });
}
void acceptance_job_storage_failure_rolls_back_whole_decision() {
    Fixture f; const auto c = candidate(f); const auto original_task = f.task();
    raw_exec(f.path,"CREATE TRIGGER fixture_no_experience BEFORE INSERT ON experience_jobs BEGIN SELECT RAISE(ABORT,'fixture job write failure'); END");
    error(ErrorCode::Conflict,[&] { accept(f,c); });
    CHECK(f.task() == original_task); CHECK(f.store->submission(s(c,"submission_id")) == c);
    CHECK(scalar(f.path,"SELECT COUNT(*) FROM candidate_acceptances") == 0);
    raw_exec(f.path,"DROP TRIGGER fixture_no_experience"); CHECK(accept(f,c).at("business_accepted").as_bool());
}
void concurrent_acceptance_has_one_captured_winner() {
    Fixture f; const auto c = candidate(f); const auto actor = f.actor(Role::Dispatcher);
    const auto binding = f.store->request_binding(f.request_id); const auto task_version = f.tv(); std::barrier gate(2);
    const auto invoke = [&](const std::string& command) {
        gate.arrive_and_wait();
        try { (void)f.store->accept_candidate(actor,binding,task_version,s(c,"submission_id"),n(c,"state_version"),decision(),command); return true; }
        catch (const Error& e) { CHECK(e.code() == ErrorCode::NotReady || e.code() == ErrorCode::Conflict); return false; }
    };
    auto first = std::async(std::launch::async,invoke,"accept-first"); auto second = std::async(std::launch::async,invoke,"accept-second");
    const bool one = first.get(); const bool two = second.get(); CHECK(one != two);
    CHECK(scalar(f.path,"SELECT COUNT(*) FROM candidate_acceptances WHERE status='ACCEPTED'") == 1); CHECK(f.store->experience_jobs(f.request_id).size() == 1);
}
void ordinary_event_exhaustion_preserves_bounded_finalization_controls() {
    const auto fill = [](Fixture& f) { test::fill_business_capacity(*f.store, f.path, f.request_id); };
    Fixture f; const auto c = candidate(f); fill(f); error(ErrorCode::NotReady,[&] { accept(f,c); });
    CHECK(f.store->experience_jobs(f.request_id).empty());
    const auto rejected = f.store->accept_candidate(f.actor(Role::Dispatcher),f.store->request_binding(f.request_id),f.tv(),s(c,"submission_id"),n(c,"state_version"),decision(false),"quota-negative");
    CHECK(s(rejected,"status") == "BLOCKED");
    Fixture g; accept_both(g); const auto d = prepare(g); fill(g);
    const auto stored = publish(g,d); CHECK(s(ack(g,stored),"status") == "CONFIRMED");
    const auto job = g.store->experience_jobs(g.request_id,g.tid()).front(); CHECK(s(report(g,job,report_body(job)),"status") == "DONE");
}
void acceptance_experience_and_delivery_records_are_immutable() {
    Fixture f; accept_both(f); const auto d = prepare(f); (void)d;
    const auto deny = [&](const std::string& sql) {
        bool denied = false;
        try { raw_exec(f.path,sql); } catch (const std::runtime_error& e) { denied = std::string(e.what()).find("immutable") != std::string::npos; }
        CHECK(denied);
    };
    deny("UPDATE candidate_acceptances SET decision='{}',state_version=state_version+1");
    deny("UPDATE experience_jobs SET source_snapshot='{}',state_version=state_version+1");
    deny("UPDATE delivery_runtime SET frozen_plan='{}',state_version=state_version+1");
    const auto job = f.store->experience_jobs(f.request_id,f.tid()).front(); (void)report(f,job,report_body(job));
    deny("UPDATE experience_reports SET body='{}'");
    CHECK(s(f.store->experience_job(s(job,"experience_id")),"status") == "DONE");
}
void publication_preflight_and_fresh_ack_proof_are_required() {
    Fixture f; accept_both(f); auto d = prepare(f);
    CHECK(f.store->authorize_delivery_publication(f.actor(Role::Root),f.store->request_binding(f.request_id),s(d,"delivery_id")) == d);
    d = publish(f,d); auto unverified = publication(d); unverified.as_object()["verified"] = false;
    error(ErrorCode::NotReady,[&] { f.store->acknowledge_delivery(f.actor(Role::Root),f.store->request_binding(f.request_id),s(d,"delivery_id"),s(d,"manifest_sha256"),1,1,"local-owner","durable_local","exact-receipt",unverified); });
    const auto receipt = ack(f,d); CHECK(receipt.at("receipt").at("publication") == publication(d));
    CHECK(s(f.store->authorize_delivery_publication(f.actor(Role::Root),f.store->request_binding(f.request_id),s(d,"delivery_id")),"status") == "CONFIRMED");
    error(ErrorCode::NotReady,[&] { f.store->acknowledge_delivery(f.actor(Role::Root),f.store->request_binding(f.request_id),s(d,"delivery_id"),s(d,"manifest_sha256"),1,1,"local-owner","durable_local","exact-receipt",unverified); });
    Fixture g; accept_both(g); const auto old = prepare(g); g.amend();
    error(ErrorCode::Stale,[&] { g.store->authorize_delivery_publication(g.actor(Role::Root),g.store->request_binding(g.request_id),s(old,"delivery_id")); });
}
void ordinary_acceptance_history_cannot_exhaust_negative_control() {
    Fixture f; accept(f,candidate(f)); const auto second = candidate(f,"beta");
    raw_exec(f.path,"WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<255) INSERT INTO candidate_acceptances(acceptance_id,request_id,task_id,submission_id,submission_version,task_version,producer_binding,actor,captured_binding,decision,record_sha256,status,created_at,updated_at) SELECT 'history-'||n.x,a.request_id,a.task_id,a.submission_id,a.submission_version,a.task_version,a.producer_binding,a.actor,a.captured_binding,a.decision,a.record_sha256,'REJECTED',a.created_at,a.updated_at FROM n CROSS JOIN (SELECT * FROM candidate_acceptances LIMIT 1) a");
    error(ErrorCode::NotReady,[&] { accept(f,second,"beta"); });
    const auto current = f.store->current_submission(f.tid());
    const auto rejected = f.store->accept_candidate(f.actor(Role::Dispatcher),f.store->request_binding(f.request_id),f.tv(),s(current,"submission_id"),n(current,"state_version"),decision(false),"negative-after-history-limit");
    CHECK(!rejected.at("business_accepted").as_bool()); CHECK(scalar(f.path,"SELECT COUNT(*) FROM candidate_acceptances") == 257);
}
void nonblocking_question_plan_and_usage_are_retained_in_package() {
    Fixture f; f.ready(); f.bind(); const auto q = f.ask(false); const auto submitted = f.submit();
    const auto c = f.check(submitted); accept(f,c); accept(f,candidate(f,"beta"),"beta");
    const auto d = prepare(f); const auto& manifest = d.at("frozen_plan").at("manifest");
    CHECK(manifest.at("open_questions").as_array().size() == 1); CHECK(manifest.at("open_questions").as_array()[0] == q.at("question_id"));
    CHECK(s(manifest,"questions_file") == "questions.json"); CHECK(s(manifest,"usage_file") == "usage.json"); CHECK(s(manifest,"plan_file") == "plan.json");
    CHECK(!manifest.at("limitations").as_array().empty());
    for (const auto& file : d.at("frozen_plan").at("files").as_array()) {
        if (s(file,"name") == "questions.json") CHECK(s(file,"content_base64") == base64(canonical_json(Array{f.store->question(s(q,"question_id"))})));
        if (s(file,"name") == "plan.json") CHECK(s(file,"content_base64") == base64(canonical_json(f.store->plan(f.request_id))));
    }
}
void receipt_expiry_uses_frozen_policy_days() {
    test::TempDir temp; auto config_text = read(source_root/"config/examples/system.yaml");
    const auto at = config_text.find("receipt_ttl_days: 30"); CHECK(at != std::string::npos); config_text.replace(at,std::string("receipt_ttl_days: 30").size(),"receipt_ttl_days: 7");
    write(temp.path/"system.yaml",config_text);
    auto input = admission(); input.config_snapshot = load_config_catalog(temp.path/"system.yaml",source_root/"config/examples/functions",source_root/"prompts").freeze("general").json();
    Store store(temp.path/"state.sqlite",base_sql,store_sql,StoreOpenMode::Takeover,tools_sql,events_sql,workflow_sql,acceptance_sql);
    const auto admitted = store.admit(input); const auto id = s(admitted,"request_id");
    store.cancel_request(store.host_actor(Role::Root,id),n(store.request(id),"state_version"),"cancel-frozen-ttl");
    CHECK(scalar(temp.path/"state.sqlite","SELECT CAST(julianday(expires_at)-julianday(closed_at) AS INTEGER) FROM receipts") == 7);
    CHECK(n(store.closeout(id).at("cleanup_job").at("manifest_snapshot"),"receipt_ttl_days") == 7);
}
void receipt_atomically_supersedes_other_delivery_intents() {
    for (const bool published_sibling : {false, true}) {
        Fixture f; accept_both(f); auto first = prepare(f);
        if (published_sibling) first = publish(f,first);
        const auto other_path = f.temp.path/"delivery-two"; std::filesystem::create_directory(other_path);
        struct stat info{}; CHECK(::stat(other_path.c_str(),&info) == 0);
        const Json other_root = Object{{"path",other_path.string()},{"device",static_cast<std::int64_t>(info.st_dev)},{"inode",static_cast<std::int64_t>(info.st_ino)}};
        auto selected = f.store->prepare_delivery(f.actor(Role::Root),f.store->request_binding(f.request_id),f.rv(),other_root,"prepare-second-root");
        selected = publish(f,selected); CHECK(s(selected,"delivery_id") != s(first,"delivery_id"));
        const auto confirmed = ack(f,selected);
        const auto sibling = f.store->delivery(s(first,"delivery_id"));
        CHECK(s(sibling,"status") == "SUPERSEDED"); CHECK(n(sibling,"state_version") == n(first,"state_version")+1);
        CHECK(sibling.at("frozen_plan") == first.at("frozen_plan")); CHECK(sibling.at("publication") == first.at("publication"));
        CHECK(s(f.store->delivery(s(selected,"delivery_id")),"status") == "CONFIRMED");
        CHECK(scalar(f.path,"SELECT COUNT(*) FROM delivery_runtime WHERE status='CONFIRMED'") == 1);
        CHECK(scalar(f.path,"SELECT COUNT(*) FROM delivery_receipts") == 1);
        const auto closeout = f.store->closeout(f.request_id);
        for (const auto& blocker : closeout.at("blockers").as_array()) CHECK(blocker.as_string() != "delivery_unconfirmed");
        CHECK(f.store->pending_recovery().at("delivery_intents").as_array().empty());
        f.reopen(); CHECK(ack(f,selected) == confirmed);
        CHECK(f.store->delivery(s(first,"delivery_id")) == sibling);
    }
}
void new_blocking_question_is_checked_at_publication_and_receipt() {
    auto initial_plan = plan_body(); initial_plan.at("nodes").as_array()[1].as_object()["required"] = false;
    Fixture f(true,initial_plan); accept(f,candidate(f));
    f.ready("beta"); const auto stored = publish(f,prepare(f));
    const auto frozen_plan = stored.at("frozen_plan"); const auto question = f.ask(true,"beta");
    error(ErrorCode::NotReady,[&] { f.store->authorize_delivery_publication(f.actor(Role::Root),f.store->request_binding(f.request_id),s(stored,"delivery_id")); });
    error(ErrorCode::NotReady,[&] { publish(f,stored); });
    error(ErrorCode::NotReady,[&] { ack(f,stored); });
    CHECK(s(f.store->request(f.request_id),"status") == "DELIVERING");
    CHECK(s(f.store->question(s(question,"question_id")),"status") == "OPEN");
    CHECK(scalar(f.path,"SELECT COUNT(*) FROM delivery_receipts") == 0); CHECK(scalar(f.path,"SELECT COUNT(*) FROM receipts") == 0);
    CHECK(f.store->delivery(s(stored,"delivery_id")).at("frozen_plan") == frozen_plan);
    f.store->answer_question(f.actor(Role::TeamLead,"beta"),f.binding("beta"),s(question,"question_id"),n(question,"state_version"),Object{{"text","The optional fixture remains excluded from this delivery."}},"answer-late-blocker");
    CHECK(s(f.store->question(s(question,"question_id")),"status") == "ANSWERED");
    const auto allowed = f.store->authorize_delivery_publication(f.actor(Role::Root),f.store->request_binding(f.request_id),s(stored,"delivery_id"));
    CHECK(allowed.at("frozen_plan") == frozen_plan);
    const auto confirmed = ack(f,stored); CHECK(s(confirmed,"status") == "CONFIRMED");
    // The bundle stays the original preparation snapshot. Resolving a later
    // control question does not rewrite its manifest, bytes or receipt hash.
    CHECK(confirmed.at("frozen_plan") == frozen_plan); CHECK(confirmed.at("manifest_sha256") == stored.at("manifest_sha256"));
}

} // namespace
int main() {
    source_root = std::filesystem::path(WLS_SOURCE_DIR);
    base_sql = read(source_root/"db/schema.sql"); store_sql = read(source_root/"db/migrations/004_s1_store.sql");
    tools_sql = read(source_root/"db/migrations/005_worker_tools.sql"); events_sql = read(source_root/"db/migrations/006_event_runtime.sql");
    workflow_sql = read(source_root/"db/migrations/007_workflow_runtime.sql"); acceptance_sql = read(source_root/"db/migrations/008_acceptance_runtime.sql");
    return test::run({
        {"acceptance migration readonly rollback and fingerprints",migration_readonly_rollback_and_asset_integrity},
        {"positive business acceptance atomic experience and original bytes",positive_acceptance_atomic_job_and_original_bytes},
        {"acceptance identity original binding and object CAS",role_scope_binding_and_cas_cannot_accept},
        {"acceptance complete evidence and required review refusal",evidence_checks_and_required_review_fail_closed},
        {"acceptance exact replay and original recovery binding",exact_replay_and_recovery_preserve_original_acceptance},
        {"negative control revokes accepted candidate and package",negative_control_revokes_accepted_delivery},
        {"amend invalidates delivery but retains historical experience",amendment_invalidates_package_but_historical_experience_survives},
        {"delivery required tasks frozen original bytes and intent reuse",required_tasks_and_frozen_original_package_contents},
        {"optional unaccepted task explicit delivery limitation",optional_task_exclusion_is_explicit},
        {"publication proof guards recovery and read only queries",publication_guards_recovery_and_readonly_queries},
        {"exact receipt and retained closeout atomic replay",exact_receipt_and_closeout_are_atomic_and_replayable},
        {"terminal experience survives and takeover fences old job",terminal_experience_and_takeover_fence_own_job},
        {"experience identity source and explicit no experience",report_identity_source_and_explicit_no_experience_guards},
        {"cancellation retains cleanup unknown and recovery",cancellation_retains_jobs_unknown_and_recovery},
        {"delivery and workspace isolation both directions",delivery_workspace_database_isolation_is_bidirectional},
        {"acceptance job insert failure rolls back decision",acceptance_job_storage_failure_rolls_back_whole_decision},
        {"one concurrent captured business acceptance winner",concurrent_acceptance_has_one_captured_winner},
        {"ordinary event limit preserves bounded closeout controls",ordinary_event_exhaustion_preserves_bounded_finalization_controls},
        {"acceptance source report and package immutable facts",acceptance_experience_and_delivery_records_are_immutable},
        {"publication preflight and fresh exact ACK proof",publication_preflight_and_fresh_ack_proof_are_required},
        {"negative revoke retains capacity after normal history fills",ordinary_acceptance_history_cannot_exhaust_negative_control},
        {"package retains open question original plan and usage",nonblocking_question_plan_and_usage_are_retained_in_package},
        {"retained receipt uses frozen expiry policy",receipt_expiry_uses_frozen_policy_days},
        {"exact receipt supersedes sibling prepared and stored packages",receipt_atomically_supersedes_other_delivery_intents},
        {"late blocking question gates publication and receipt",new_blocking_question_is_checked_at_publication_and_receipt}
    });
}
