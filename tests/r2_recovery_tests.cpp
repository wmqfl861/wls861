#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "wls/model.hpp"
#include "test_support.hpp"
#include <sqlite3.h>
#include <sys/stat.h>
#include <fstream>
#include <iterator>
#include <memory>

namespace {
using namespace wls::s1;
using wls::Role;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path source = WLS_SOURCE_DIR;
std::string read(const std::filesystem::path& p) {
    std::ifstream stream(p, std::ios::binary); CHECK(stream.good());
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::string text(const Json& v, std::string_view k) { return std::string(v.at(k).as_string()); }
std::int64_t num(const Json& v, std::string_view k) { return v.at(k).as_int64(); }
void fails(ErrorCode expected, const std::function<void()>& action) {
    try { action(); } catch (const Error& e) {
        if (e.code() != expected) throw std::runtime_error(std::string("wrong rejection: ") + e.what());
        return;
    }
    throw std::runtime_error("expected rejection");
}
void sql(const std::filesystem::path& path, const std::string& statement) {
    sqlite3* db = nullptr; CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    char* message = nullptr;
    const auto rc = sqlite3_exec(db, statement.c_str(), nullptr, nullptr, &message);
    const std::string detail = message ? message : "fixture SQL";
    sqlite3_free(message); sqlite3_close(db);
    if (rc != SQLITE_OK) throw std::runtime_error(detail);
}
std::vector<std::string> rows(const std::filesystem::path& path, const std::string& query,
                              const std::string& id = {}) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    CHECK(sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    if (!id.empty()) CHECK(sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
    std::vector<std::string> result;
    int rc{};
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        std::string row;
        for (int col = 0; col < sqlite3_column_count(stmt); ++col) {
            const auto type = sqlite3_column_type(stmt, col);
            const auto* data = static_cast<const char*>(sqlite3_column_blob(stmt, col));
            const auto size = static_cast<std::size_t>(sqlite3_column_bytes(stmt, col));
            row += std::to_string(type) + ":" + std::to_string(size) + ":";
            if (size) row.append(data, size);
            row += "|";
        }
        result.push_back(row);
    }
    sqlite3_finalize(stmt); sqlite3_close(db); CHECK(rc == SQLITE_DONE); return result;
}
struct Node { std::string request, task, team; };
struct Fixture {
    test::TempDir dir;
    std::filesystem::path path{dir.path / "state.sqlite"};
    std::unique_ptr<Store> store;
    FrozenRequestConfig config{load_config_catalog(source / "config/examples/system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general")};
    Json brief = Object{{"goal", "preserve observations"}, {"constraint_refs", Array{"c1"}},
                        {"constraints_verbatim", Array{"preserve observations"}}};
    Node a, b;
    std::unique_ptr<Store> open(StoreRecoveryScope scope = {}, StoreOpenMode mode = StoreOpenMode::Takeover,
                                bool full = true) const {
        return std::make_unique<Store>(path, read(source / "db/schema.sql"), read(source / "db/migrations/004_s1_store.sql"), mode,
            read(source / "db/migrations/005_worker_tools.sql"), full ? read(source / "db/migrations/006_event_runtime.sql") : "",
            full ? read(source / "db/migrations/007_workflow_runtime.sql") : "",
            full ? read(source / "db/migrations/008_acceptance_runtime.sql") : "",
            full ? read(source / "db/migrations/009_owned_cleanup.sql") : "", std::move(scope));
    }
    AdmissionInput input(const std::string& key) const {
        AdmissionInput in; in.tenant_id="tenant"; in.project_id="project"; in.root_id="root"; in.function_id="general";
        in.idempotency_key=key; in.original_text="preserve observations"; in.config_snapshot=config.json();
        in.constraint_catalog=Array{Object{{"constraint_id","c1"},{"text",in.original_text},{"source_revision",1}}};
        in.tool_allowlist={"note","fs_write","ack_change"}; return in;
    }
    StoreActor actor(const Node& n, Role role = Role::Worker) const {
        return store->host_actor(role, n.request, role == Role::Worker || role == Role::TeamLead ? n.team : "");
    }
    Node create(const std::string& key) {
        Node n; n.request=text(store->admit(input(key)), "request_id");
        store->publish_plan(actor(n,Role::Dispatcher), num(store->request(n.request),"state_version"),1,brief,new_id("plan"));
        const auto task=store->create_task(actor(n,Role::Dispatcher),num(store->request(n.request),"state_version"),1,1,"node",brief,new_id("task"));
        n.task=text(task,"task_id"); n.team=text(task,"team_id");
        store->issue_instruction(actor(n,Role::TeamLead),store->binding(n.task),num(store->task(n.task),"state_version"),brief,new_id("order"));
        ack(n); return n;
    }
    explicit Fixture(bool full = true) {
        store=open({},StoreOpenMode::Takeover,full);
        if (full) { a=create("a"); b=create("b"); }
    }
    void reopen(StoreRecoveryScope scope = {}) { store.reset(); store=open(std::move(scope)); }
    void ack(const Node& n) {
        store->acknowledge(actor(n),store->binding(n.task),num(store->task(n.task),"state_version"),new_id("ack"));
    }
    Json prepare(const Node& n) {
        return store->prepare_operation(actor(n),store->binding(n.task),num(store->task(n.task),"state_version"),new_id("key"),
                                       "fs_write",Object{{"fixture",true}},"workspace",new_id("prepare"));
    }
    Json running(const Node& n) {
        const auto op=prepare(n);
        return store->start_operation(actor(n),text(op,"op_id"),num(op,"state_version"),new_id("start"));
    }
    ModelRequest pending(const Node& n) {
        ContextInput c; c.role=Role::Worker; c.model=config.resolve(Role::Worker);
        const auto profile=config.model_profile(Role::Worker);
        c.adapter_version=text(profile,"adapter_version"); c.protocol_version=text(profile,"protocol");
        c.system_prompt="fixture"; c.role_prompt="Worker"; c.function_prompt="preserve observations"; c.task_contract="current complete contract";
        ModelRequest r; r.call_id=new_id("call"); r.attempt_id=new_id("attempt"); r.role=Role::Worker;
        r.binding=store->binding(n.task); r.config_snapshot_id=config.sha256(); r.model=c.model;
        r.context=ContextCompiler().compile(c); r.max_output_tokens=20; r.reserved_input_tokens=100; r.reserved_output_tokens=20;
        AttemptReservation reserve; reserve.call_id=r.call_id; reserve.attempt_id=r.attempt_id;
        reserve.config_snapshot_sha256=config.sha256(); reserve.model_ref=r.model.model;
        reserve.reserved_input_tokens=100; reserve.reserved_output_tokens=20; reserve.max_output_tokens=20;
        reserve.request_envelope=to_json(r); store->reserve_attempt(actor(n),r.binding,reserve); return r;
    }
    Json snapshot(const Node& n) const {
        Object result;
        for (const auto* table : {"requests","request_budgets","teams","tasks","ops","call_attempts","events",
                                "event_inbox","event_actor_claims","experience_jobs","cleanup_jobs","worker_waits"}) {
            Array data;
            for (const auto& row:rows(path,std::string("SELECT * FROM ")+table+" WHERE request_id=? ORDER BY rowid",n.request)) data.emplace_back(row);
            result[table]=std::move(data);
        }
        return result;
    }
};
Json usage() { return Object{{"input_tokens",1},{"output_tokens",1},{"cost_micro_usd",0}}; }
void selected_recovery_preserves_sibling() {
    Fixture f;
    const auto old=f.actor(f.a); const auto binding=f.store->binding(f.a.task);
    const auto pa=f.pending(f.a); (void)f.pending(f.b);
    const auto running=f.running(f.a); (void)f.running(f.b); const auto prepared=f.prepare(f.a);
    (void)f.store->claim_events(f.actor(f.b,Role::Dispatcher));
    const auto before=f.snapshot(f.b); const auto original=f.store->instruction(f.a.task).at("body");
    f.reopen(StoreRecoveryScope::request(f.a.request));
    CHECK(f.snapshot(f.b)==before);
    CHECK(f.store->operation(text(running,"op_id")).at("status")=="UNKNOWN");
    CHECK(f.store->operation(text(prepared,"op_id")).at("status")=="CANCELLED");
    CHECK(f.store->attempt(pa.attempt_id).at("status")=="UNKNOWN");
    fails(ErrorCode::Stale,[&]{f.store->acknowledge(old,binding,num(f.store->task(f.a.task),"state_version"),new_id("ack"));});
    f.ack(f.a); CHECK(f.store->instruction(f.a.task).at("body")==original);
    fails(ErrorCode::UnknownOutcome,[&]{(void)f.prepare(f.a);});
}
void sibling_recovers_before_late_result() {
    Fixture f; const auto old=f.actor(f.b); const auto call=f.pending(f.b); const auto op=f.running(f.b);
    f.reopen(StoreRecoveryScope::request(f.a.request));
    const auto before=f.snapshot(f.a);
    f.reopen(StoreRecoveryScope::request(f.b.request));
    CHECK(f.snapshot(f.a)==before); CHECK(f.store->operation(text(op,"op_id")).at("status")=="UNKNOWN");
    fails(ErrorCode::Stale,[&]{f.store->acknowledge(old,call.binding,num(f.store->task(f.b.task),"state_version"),new_id("ack"));});
    const auto settled=f.store->settle_attempt(call.attempt_id,call.binding,f.config.sha256(),"COMPLETED",usage(),Object{{"text","late"}});
    CHECK(settled.at("status")=="STALE"); CHECK(num(settled,"accepted")==0);
    f.ack(f.b); fails(ErrorCode::UnknownOutcome,[&]{(void)f.prepare(f.b);});
}
void scope_blocks_actorless_mutations_and_foreign_actors() {
    Fixture f; const auto bactor=f.actor(f.b); const auto call=f.pending(f.b); const auto op=f.running(f.b);
    f.reopen(StoreRecoveryScope::request(f.a.request)); const auto before=f.snapshot(f.b);
    fails(ErrorCode::Unauthorized,[&]{(void)f.actor(f.b);});
    fails(ErrorCode::Unauthorized,[&]{f.store->cancel_request(bactor,1,new_id("cancel"));});
    fails(ErrorCode::Unauthorized,[&]{f.store->recover_request(f.b.request,1);});
    fails(ErrorCode::Unauthorized,[&]{f.store->reconcile_operation(text(op,"op_id"),num(op,"state_version"),"FAILED","fixture inspection",new_id("reconcile"));});
    fails(ErrorCode::Unauthorized,[&]{f.store->settle_attempt(call.attempt_id,call.binding,f.config.sha256(),"COMPLETED",usage(),Object{{"text","late"}});});
    fails(ErrorCode::Unauthorized,[&]{f.store->record_tool_feedback(call.attempt_id,call.binding,Object{},Object{});});
    fails(ErrorCode::Unauthorized,[&]{f.store->record_operation_result(text(op,"op_id"),call.binding,"FAILED",Object{},Object{{"observed",true}},"", "",false,false);});
    struct stat info{}; CHECK(::stat(f.dir.path.c_str(),&info)==0);
    fails(ErrorCode::Unauthorized,[&]{f.store->bind_workspace(f.b.team,f.dir.path.string(),static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino));});
    fails(ErrorCode::Unauthorized,[&]{(void)f.store->admit(f.input("no-new"));});
    CHECK(f.snapshot(f.b)==before);
}
void admission_only_has_no_execution_authority() {
    Fixture f; const auto actor=f.actor(f.a); const auto before_a=f.snapshot(f.a); const auto before_b=f.snapshot(f.b);
    f.reopen(StoreRecoveryScope::admission());
    const auto input=f.input("third"); const auto admitted=f.store->admit(input);
    CHECK(f.store->admit(input)==admitted); CHECK(f.snapshot(f.a)==before_a && f.snapshot(f.b)==before_b);
    fails(ErrorCode::Unauthorized,[&]{(void)f.store->host_actor(Role::Root,text(admitted,"request_id"));});
    fails(ErrorCode::Unauthorized,[&]{f.store->recover_request(f.a.request,1);});
    fails(ErrorCode::Unauthorized,[&]{f.store->acknowledge(actor,ExecutionBinding{},1,new_id("ack"));});
    CHECK(f.snapshot(f.a)==before_a && f.snapshot(f.b)==before_b);
}
void invalid_request_cannot_recover_anything() {
    Fixture f; const auto a=f.snapshot(f.a), b=f.snapshot(f.b); f.store.reset();
    fails(ErrorCode::InvalidInput,[&]{auto unused=f.open(StoreRecoveryScope::request("req_missing"));});
    fails(ErrorCode::InvalidInput,[&]{auto unused=f.open(StoreRecoveryScope::request(""));});
    fails(ErrorCode::InvalidInput,[&]{auto unused=f.open(StoreRecoveryScope::request(f.a.request),StoreOpenMode::ReadOnly);});
    CHECK(f.snapshot(f.a)==a && f.snapshot(f.b)==b);
}
void selected_scope_cannot_initialize_missing_or_empty_database() {
    Fixture f; f.store.reset();
    const auto missing=f.dir.path / "missing.sqlite";
    const auto saved=f.path; f.path=missing;
    fails(ErrorCode::Storage,[&]{auto unused=f.open(StoreRecoveryScope::request(f.a.request));});
    CHECK(!std::filesystem::exists(missing));
    { std::ofstream empty(missing); CHECK(empty.good()); }
    fails(ErrorCode::Unsupported,[&]{auto unused=f.open(StoreRecoveryScope::request(f.a.request));});
    CHECK(rows(missing,"SELECT name FROM sqlite_master").empty());
    CHECK(rows(missing,"PRAGMA user_version")==std::vector<std::string>{"1:1:0|"});
    f.path=saved;
}
void selected_recovery_preserves_wait_and_unhandled_amendment() {
    Fixture f;
    const auto wait=f.store->wait_worker(f.actor(f.a),f.store->binding(f.a.task),
        num(f.store->task(f.a.task),"state_version"),"explicit pause",new_id("wait"));
    const auto before=f.snapshot(f.b);
    f.reopen(StoreRecoveryScope::request(f.a.request)); f.ack(f.a);
    CHECK(f.store->worker_wait(f.a.task)==wait);
    fails(ErrorCode::NotReady,[&]{(void)f.prepare(f.a);});
    f.store->wake_worker(f.actor(f.a),f.store->binding(f.a.task),num(wait,"generation"),new_id("wake"));
    const auto catalog=Array{Object{{"constraint_id","c1"},{"text","preserve observations"},{"source_revision",1}}};
    f.store->amend(f.actor(f.a,Role::Root),num(f.store->request(f.a.request),"state_version"),"change pending",catalog,new_id("amend"));
    f.reopen(StoreRecoveryScope::request(f.a.request));
    fails(ErrorCode::NotReady,[&]{f.ack(f.a);});
    CHECK(f.snapshot(f.b)==before);
    CHECK(num(f.store->request(f.a.request),"requirements_revision")==2);
}

void selected_transaction_failure_rolls_back() {
    Fixture f; const auto pa=f.pending(f.a); (void)f.running(f.a);
    const auto before_a=f.snapshot(f.a), before_b=f.snapshot(f.b); f.store.reset();
    sql(f.path,"CREATE TRIGGER r2_fail BEFORE INSERT ON events WHEN NEW.type='recover' BEGIN SELECT RAISE(ABORT,'fixture recovery failure'); END;");
    fails(ErrorCode::Conflict,[&]{auto unused=f.open(StoreRecoveryScope::request(f.a.request));});
    CHECK(f.snapshot(f.a)==before_a && f.snapshot(f.b)==before_b);
    sql(f.path,"DROP TRIGGER r2_fail"); f.store=f.open(StoreRecoveryScope::request(f.a.request));
    CHECK(f.store->attempt(pa.attempt_id).at("status")=="UNKNOWN");
}
void distinct_scopes_still_share_one_writer_lease() {
    Fixture f; f.reopen(StoreRecoveryScope::request(f.a.request));
    fails(ErrorCode::Conflict,[&]{auto unused=f.open(StoreRecoveryScope::request(f.b.request));});
    auto reader=f.open({},StoreOpenMode::ReadOnly); CHECK(text(reader->request(f.b.request),"request_id")==f.b.request);
    fails(ErrorCode::Unauthorized,[&]{reader->recover_request(f.b.request,1);});
}
void legacy_all_request_mode_remains_compatible() {
    Fixture f; const auto a=f.store->request(f.a.request), b=f.store->request(f.b.request);
    f.reopen();
    CHECK(num(f.store->request(f.a.request),"dispatcher_epoch")==num(a,"dispatcher_epoch")+1);
    CHECK(num(f.store->request(f.b.request),"dispatcher_epoch")==num(b,"dispatcher_epoch")+1);
    f.ack(f.a); f.ack(f.b); CHECK(f.prepare(f.a).at("status")=="PREPARED");
}
void terminal_job_recovery_is_targeted() {
    Fixture f;
    for (const auto& n:{f.a,f.b}) {
        f.store->cancel_request(f.actor(n,Role::Root),num(f.store->request(n.request),"state_version"),new_id("cancel"));
        // Synthetic unfinished closeout claim: only tests lease recovery, not
        // business acceptance/source validation. All rows are disposable.
        sql(f.path,"INSERT INTO experience_jobs(experience_id,request_id,task_id,team_id,based_on_requirements_revision,plan_revision,task_revision,instruction_revision,status,source_snapshot,claimed_by,created_at,updated_at) VALUES ('"+new_id("exp")+"','"+n.request+"','"+n.task+"','"+n.team+"',1,1,1,1,'RUNNING','{}','previous-writer','fixture','fixture');");
    }
    const auto a=f.store->request(f.a.request); const auto before=f.snapshot(f.b);
    f.reopen(StoreRecoveryScope::request(f.a.request));
    CHECK(f.snapshot(f.b)==before); CHECK(f.store->request(f.a.request)==a);
    CHECK(rows(f.path,"SELECT status,lease_epoch,state_version,claimed_by FROM experience_jobs WHERE request_id=?",f.a.request)==std::vector<std::string>{"3:7:PENDING|1:1:1|1:1:2|5:0:|"});
    CHECK(!rows(f.path,"SELECT seq FROM events WHERE request_id=? AND type='recover_experience'",f.a.request).empty());
}
void populated_migration_requires_explicit_maintenance() {
    Fixture f(false); const auto request=f.store->admit(f.input("old-403")); const auto id=text(request,"request_id");
    f.store.reset(); const auto before=rows(f.path,"SELECT * FROM requests WHERE request_id=?",id);
    for (const auto& scope:{StoreRecoveryScope::request(id),StoreRecoveryScope::admission()})
        fails(ErrorCode::Unsupported,[&]{auto unused=f.open(scope);});
    CHECK(rows(f.path,"SELECT * FROM requests WHERE request_id=?",id)==before);
    CHECK(rows(f.path,"PRAGMA user_version")==std::vector<std::string>{"1:3:403|"});
    f.store=f.open(); CHECK(num(f.store->diagnostics(),"user_version")==407);
    f.store.reset(); f.store=f.open(StoreRecoveryScope::request(id)); CHECK(text(f.store->request(id),"request_id")==id);
}
}
int main() {
    return test::run({{"selected recovery preserves all sibling facts",selected_recovery_preserves_sibling},
        {"later sibling takeover still fences operations and late responses",sibling_recovers_before_late_result},
        {"scope also guards actorless settlement and feedback APIs",scope_blocks_actorless_mutations_and_foreign_actors},
        {"admission-only does not issue execution authority",admission_only_has_no_execution_authority},
        {"invalid request and read-only scope cannot trigger recovery",invalid_request_cannot_recover_anything},
        {"selected scope cannot initialize missing or empty databases",selected_scope_cannot_initialize_missing_or_empty_database},
        {"selected recovery preserves WAITING and unhandled AMEND",selected_recovery_preserves_wait_and_unhandled_amendment},
        {"scoped recovery is atomic when its event fails",selected_transaction_failure_rolls_back},
        {"different scopes cannot bypass the database writer lease",distinct_scopes_still_share_one_writer_lease},
        {"default all-request recovery remains compatible",legacy_all_request_mode_remains_compatible},
        {"terminal closeout recovery only reclaims selected jobs",terminal_job_recovery_is_targeted},
        {"populated upgrades require explicit maintenance",populated_migration_requires_explicit_maintenance}});
}
