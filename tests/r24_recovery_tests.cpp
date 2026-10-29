// R2.4: real disposable SQLite fixtures; no provider or production access.
#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "wls/model.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <sqlite3.h>
#include <sys/stat.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <array>
#include <barrier>
#include <future>
#include <thread>

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
Json database(const Fixture& f) {
    Object snapshot;
    for (const auto& row: rows(f.path,"SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name")) {
        const auto begin=row.find(':',2)+1;
        const auto table=row.substr(begin,row.size()-begin-1);
        Array data;
        auto values=rows(f.path,"SELECT * FROM \""+table+"\"");
        std::sort(values.begin(),values.end());
        for (const auto& value:values) data.emplace_back(value);
        snapshot[table]=std::move(data);
    }
    return snapshot;
}
void idle_reopens_preserve_every_row() {
    Fixture f; const auto before=database(f);
    const auto epoch=num(f.store->request(f.a.request),"dispatcher_epoch");
    for (int i=0;i<4;++i) {
        f.reopen(StoreRecoveryScope::request(f.a.request));
        std::cout << "idle reopen=" << i << " epoch_before=" << epoch
                  << " epoch_after=" << num(f.store->request(f.a.request),"dispatcher_epoch") << '\n';
        CHECK(num(f.store->request(f.a.request),"dispatcher_epoch")==epoch);
        CHECK(database(f)==before);
    }
}
void settled_history_is_idle_and_immutable() {
    Fixture f; const auto call=f.pending(f.a);
    FakeModelStep step; step.text="original response with evidence";
    step.usage.input_tokens=1; step.usage.output_tokens=1; step.usage.cost_micro_usd=0;
    const auto response=FakeModel({step},call.model.model).complete(call);
    (void)f.store->settle_attempt(call.attempt_id,call.binding,f.config.sha256(),"COMPLETED",to_json(response.usage),to_json(response));
    (void)f.store->record_tool_feedback(call.attempt_id,call.binding,to_json(response),Object{{"observed","original feedback"}});
    auto op=f.running(f.a);
    (void)f.store->finish_operation(f.actor(f.a),text(op,"op_id"),f.store->binding(f.a.task),num(op,"state_version"),"SUCCEEDED","original effect evidence",new_id("finish"));
    const auto before=database(f); f.reopen(StoreRecoveryScope::request(f.a.request));
    CHECK(database(f)==before);
    CHECK(f.store->attempt(call.attempt_id).at("response")==to_json(response));
}
void pending_operations_and_calls_recover_once() {
    Fixture f; const auto old=f.actor(f.a); const auto b=f.store->binding(f.a.task);
    const auto prepared=f.prepare(f.a); const auto running=f.running(f.a); const auto call=f.pending(f.a);
    const auto before=f.snapshot(f.b); const auto epoch=b.dispatcher_epoch;
    const auto original=rows(f.path,"SELECT * FROM instructions");
    f.reopen(StoreRecoveryScope::request(f.a.request));
    CHECK(num(f.store->request(f.a.request),"dispatcher_epoch")==epoch+1);
    CHECK(f.store->operation(text(prepared,"op_id")).at("status")=="CANCELLED");
    CHECK(f.store->operation(text(running,"op_id")).at("status")=="UNKNOWN");
    CHECK(f.store->attempt(call.attempt_id).at("status")=="UNKNOWN");
    CHECK(f.snapshot(f.b)==before); CHECK(rows(f.path,"SELECT * FROM instructions")==original);
    const auto recovered=database(f);
    fails(ErrorCode::Stale,[&]{f.store->prepare_operation(old,b,num(f.store->task(f.a.task),"state_version"),new_id("key"),"note",Object{},"none",new_id("prepare"));});
    CHECK(database(f)==recovered);
    f.ack(f.a); fails(ErrorCode::UnknownOutcome,[&]{(void)f.prepare(f.a);});
    const auto late=f.store->settle_attempt(call.attempt_id,call.binding,f.config.sha256(),"COMPLETED",usage(),Object{{"text","late original"}});
    CHECK(num(late,"accepted")==0 && late.at("status")=="STALE");
    CHECK(f.store->operation(text(running,"op_id")).at("status")=="UNKNOWN");
}
void active_claim_requires_recovery() {
    Fixture f; const auto actor=f.actor(f.a,Role::Dispatcher);
    const auto claim=f.store->claim_events(actor); CHECK(!claim.at("items").as_array().empty());
    const auto epoch=num(f.store->request(f.a.request),"dispatcher_epoch");
    f.reopen(StoreRecoveryScope::request(f.a.request));
    CHECK(num(f.store->request(f.a.request),"dispatcher_epoch")==epoch+1);
    CHECK(rows(f.path,"SELECT status FROM event_actor_claims WHERE request_id=?",f.a.request)==std::vector<std::string>{"3:8:RELEASED|"});
    const auto before=database(f);
    fails(ErrorCode::Stale,[&]{f.store->finish_events(actor,text(claim,"claim_id"),num(claim,"generation"),{});});
    CHECK(database(f)==before);
}
void pending_call_alone_requires_recovery_then_unknown_is_idle() {
    Fixture f; const auto call=f.pending(f.a); const auto epoch=call.binding.dispatcher_epoch;
    CHECK(rows(f.path,"SELECT op_id FROM ops").empty());
    CHECK(rows(f.path,"SELECT claim_id FROM event_actor_claims WHERE status='ACTIVE'").empty());
    const auto original=rows(f.path,"SELECT call_id,attempt_id,frozen_binding,request_envelope,reservation_sha256 FROM call_attempts");
    f.reopen(StoreRecoveryScope::request(f.a.request));
    CHECK(num(f.store->request(f.a.request),"dispatcher_epoch")==epoch+1);
    CHECK(f.store->attempt(call.attempt_id).at("status")=="UNKNOWN");
    CHECK(rows(f.path,"SELECT call_id,attempt_id,frozen_binding,request_envelope,reservation_sha256 FROM call_attempts")==original);
    const auto recovered=database(f); f.reopen(StoreRecoveryScope::request(f.a.request));
    CHECK(database(f)==recovered);
}
void old_writer_actor_cannot_write_with_same_epochs() {
    Fixture f; const auto old=f.actor(f.a); const auto b=f.store->binding(f.a.task);
    f.reopen(StoreRecoveryScope::request(f.a.request));
    CHECK(f.store->binding(f.a.task)==b);
    const auto before=database(f);
    fails(ErrorCode::Stale,[&]{f.store->prepare_operation(old,b,num(f.store->task(f.a.task),"state_version"),new_id("key"),"note",Object{},"none",new_id("prepare"));});
    CHECK(database(f)==before); CHECK(f.prepare(f.a).at("status")=="PREPARED");
}
void reader_actor_never_authorizes_writer() {
    Fixture f; auto reader=f.open({},StoreOpenMode::ReadOnly);
    const auto alien=reader->host_actor(Role::Worker,f.a.request,f.a.team);
    const auto current=f.store->binding(f.a.task); const auto before=database(f);
    fails(ErrorCode::Stale,[&]{f.store->prepare_operation(alien,current,num(f.store->task(f.a.task),"state_version"),new_id("key"),"note",Object{},"none",new_id("prepare"));});
    CHECK(database(f)==before);
}
void reader_actor_cannot_bind_a_workspace() {
    Fixture f; auto reader=f.open({},StoreOpenMode::ReadOnly);
    const auto alien=reader->host_actor(Role::Worker,f.a.request,f.a.team);
    const auto path=f.dir.path/"business"; CHECK(std::filesystem::create_directory(path));
    WorkerTools tools(*f.store,path); const auto before=database(f);
    fails(ErrorCode::Stale,[&]{tools.bind(alien);});
    CHECK(database(f)==before); CHECK(std::filesystem::is_empty(path));
}
void foreign_control_session_does_not_consume_current_rate_limit() {
    Fixture f;
    (void)f.store->recover_request(f.a.request,num(f.store->request(f.a.request),"state_version"));
    auto reader=f.open({},StoreOpenMode::ReadOnly);
    const auto alien=reader->host_actor(Role::Worker,f.a.request,f.a.team), current=f.actor(f.a);
    const auto binding=f.store->binding(f.a.task); const auto expected=num(f.store->task(f.a.task),"state_version");
    const auto limit=num(f.store->authorize_control(current,binding,"ack_change"),"max_queries_per_minute");
    const auto path=f.dir.path/"business"; CHECK(std::filesystem::create_directory(path));
    WorkerTools tools(*f.store,path); const auto before=database(f);
    for(std::int64_t i=0;i<=limit;++i)
        fails(ErrorCode::Stale,[&]{(void)tools.execute(alien,binding,expected,"foreign-ack","ack_change",Object{},new_id("control"));});
    CHECK(database(f)==before);
    const auto ack=tools.execute(current,binding,expected,"current-ack","ack_change",Object{},"current-control");
    CHECK(num(ack,"state_version")==expected+1);
    const auto acknowledged=database(f);
    // Same-session exact replay across a forced epoch remains read-only.
    (void)f.store->recover_request(f.a.request,num(f.store->request(f.a.request),"state_version"));
    const auto recovered=database(f); CHECK(recovered!=acknowledged);
    CHECK(tools.execute(current,binding,expected,"current-ack","ack_change",Object{},"current-control")==ack);
    CHECK(database(f)==recovered);
}
void all_roles_keep_session_fence_even_for_replay() {
    Fixture f;
    const auto root=f.actor(f.a,Role::Root), dispatcher=f.actor(f.a,Role::Dispatcher), lead=f.actor(f.a,Role::TeamLead);
    const auto worker=f.actor(f.a); const auto b=f.store->binding(f.a.task);
    const auto expected=num(f.store->task(f.a.task),"state_version");
    const auto wait=f.store->wait_worker(worker,b,expected,"external input","stable-wait");
    f.reopen(StoreRecoveryScope::request(f.a.request)); const auto before=database(f);
    fails(ErrorCode::Stale,[&]{f.store->cancel_request(root,num(f.store->request(f.a.request),"state_version"),new_id("cancel"));});
    fails(ErrorCode::Stale,[&]{f.store->publish_plan(dispatcher,num(f.store->request(f.a.request),"state_version"),1,f.brief,new_id("plan"));});
    fails(ErrorCode::Stale,[&]{f.store->issue_instruction(lead,b,num(f.store->task(f.a.task),"state_version"),f.brief,new_id("instruction"));});
    fails(ErrorCode::Stale,[&]{f.store->wait_worker(worker,b,expected,"external input","stable-wait");});
    CHECK(database(f)==before);
    CHECK(f.store->wait_worker(f.actor(f.a),b,expected,"external input","stable-wait")==wait);
    CHECK(database(f)==before);
}
void forced_recovery_preserves_originals_and_witness_guard() {
    Fixture f; const auto before=database(f); const auto old=f.actor(f.a); const auto b=f.store->binding(f.a.task);
    const auto epoch=b.dispatcher_epoch;
    (void)f.store->recover_request(f.a.request,num(f.store->request(f.a.request),"state_version"));
    CHECK(num(f.store->request(f.a.request),"dispatcher_epoch")==epoch+1);
    CHECK(rows(f.path,"SELECT * FROM instructions").size()==2);
    CHECK(database(f).at("instructions")==before.at("instructions"));
    CHECK(database(f).at("request_texts")==before.at("request_texts"));
    fails(ErrorCode::Stale,[&]{f.store->acknowledge(old,b,num(f.store->task(f.a.task),"state_version"),new_id("ack"));});
    CHECK(f.store->instruction(f.a.task).at("dispatcher_epoch")==epoch+1);
    // Damage only a disposable recovery witness, after proving the projection.
    sql(f.path,"DROP TRIGGER events_immutable; UPDATE events SET result=json_set(result,'$.rebound_instructions[0].body_sha256','"+std::string(64,'0')+"') WHERE request_id='"+f.a.request+"' AND type='recover';");
    fails(ErrorCode::Stale,[&]{f.ack(f.a);});
}
void waiting_and_amendment_are_not_recovery_authority() {
    Fixture f; const auto wait=f.store->wait_worker(f.actor(f.a),f.store->binding(f.a.task),num(f.store->task(f.a.task),"state_version"),"external wait",new_id("wait"));
    const auto before=database(f); f.reopen(StoreRecoveryScope::request(f.a.request)); CHECK(database(f)==before);
    fails(ErrorCode::NotReady,[&]{(void)f.prepare(f.a);});
    f.store->wake_worker(f.actor(f.a),f.store->binding(f.a.task),num(wait,"generation"),new_id("wake"));
    f.store->amend(f.actor(f.a,Role::Root),num(f.store->request(f.a.request),"state_version"),"change pending",Array{Object{{"constraint_id","c1"},{"text","preserve observations"},{"source_revision",1}}},new_id("amend"));
    const auto amended=database(f); f.reopen(StoreRecoveryScope::request(f.a.request)); CHECK(database(f)==amended);
    fails(ErrorCode::NotReady,[&]{(void)f.prepare(f.a);}); fails(ErrorCode::NotReady,[&]{f.ack(f.a);});
}
void simultaneous_open_keeps_exclusive_writer() {
    Fixture f; (void)f.running(f.a); const auto epoch=num(f.store->request(f.a.request),"dispatcher_epoch"); f.store.reset();
    std::barrier start(2), held(2); std::array<int,2> outcome{};
    std::array<std::exception_ptr,2> errors{};
    const auto run=[&](std::size_t i) {
        start.arrive_and_wait(); std::unique_ptr<Store> store;
        try { store=f.open(StoreRecoveryScope::request(f.a.request)); outcome[i]=1; }
        catch (const Error& e) { if(e.code()==ErrorCode::Conflict) outcome[i]=2; else errors[i]=std::current_exception(); }
        catch (...) { errors[i]=std::current_exception(); }
        held.arrive_and_wait();
    };
    std::thread a(run,0),b(run,1); a.join(); b.join();
    for(const auto& e:errors) if(e) std::rethrow_exception(e);
    CHECK(outcome[0]+outcome[1]==3);
    f.store=f.open(StoreRecoveryScope::request(f.a.request));
    CHECK(num(f.store->request(f.a.request),"dispatcher_epoch")==epoch+1);
}
void recovery_races_prepare_with_captured_identity() {
    Fixture f; const auto actor=f.actor(f.a); const auto binding=f.store->binding(f.a.task);
    const auto task_version=num(f.store->task(f.a.task),"state_version"), version=num(f.store->request(f.a.request),"state_version");
    std::barrier start(2); Json prepared;
    auto worker=std::async(std::launch::async,[&]{start.arrive_and_wait(); try {
        prepared=f.store->prepare_operation(actor,binding,task_version,"racing","note",Object{},"none","racing-prepare");
    } catch (const Error& e) { if(e.code()!=ErrorCode::Stale) throw; }});
    start.arrive_and_wait(); (void)f.store->recover_request(f.a.request,version); worker.get();
    if(!prepared.is_null()) CHECK(f.store->operation(text(prepared,"op_id")).at("status")=="CANCELLED");
    CHECK(rows(f.path,"SELECT op_id FROM ops WHERE status IN ('PREPARED','RUNNING')").empty());
}
void recovery_races_claim_without_losing_work() {
    Fixture f; const auto actor=f.actor(f.a,Role::Dispatcher);
    const auto version=num(f.store->request(f.a.request),"state_version");
    std::barrier start(2);
    auto worker=std::async(std::launch::async,[&]{start.arrive_and_wait(); try {(void)f.store->claim_events(actor);}
        catch(const Error& e){if(e.code()!=ErrorCode::Stale)throw;}});
    start.arrive_and_wait(); (void)f.store->recover_request(f.a.request,version); worker.get();
    CHECK(rows(f.path,"SELECT claim_id FROM event_actor_claims WHERE request_id=? AND status='ACTIVE'",f.a.request).empty());
    CHECK(!f.store->inbox(f.actor(f.a,Role::Dispatcher)).empty());
}
void failed_demand_recovery_rolls_back_every_table() {
    Fixture f; (void)f.running(f.a); (void)f.pending(f.a); f.store.reset();
    sql(f.path,"CREATE TRIGGER r24_fail BEFORE INSERT ON events WHEN NEW.type='recover' BEGIN SELECT RAISE(ABORT,'injected r24 failure'); END;");
    const auto before=database(f);
    fails(ErrorCode::Conflict,[&]{auto unused=f.open(StoreRecoveryScope::request(f.a.request));}); CHECK(database(f)==before);
    sql(f.path,"DROP TRIGGER r24_fail"); f.store=f.open(StoreRecoveryScope::request(f.a.request));
    CHECK(f.store->request(f.a.request).at("dispatcher_epoch").as_int64()>1);
}
void terminal_pending_cleanup_is_idle_and_never_business_authority() {
    Fixture f; f.store->cancel_request(f.actor(f.a,Role::Root),num(f.store->request(f.a.request),"state_version"),new_id("cancel"));
    const auto before=database(f);
    for(int i=0;i<3;++i) {f.reopen(StoreRecoveryScope::request(f.a.request)); CHECK(database(f)==before);}
    fails(ErrorCode::NotReady,[&]{(void)f.prepare(f.a);});
    fails(ErrorCode::NotReady,[&]{f.store->begin_workspace_cleanup(f.actor(f.a,Role::Root),Object{},Array{});});
    CHECK(database(f)==before);
}
}
int main() {
    return test::run({
        {"idle repeated open preserves every persistent row",idle_reopens_preserve_every_row},
        {"settled FakeModel response feedback and effects remain idle",settled_history_is_idle_and_immutable},
        {"unsettled operations and pending calls recover with original evidence",pending_operations_and_calls_recover_once},
        {"active event claim requires recovery and rejects old completion",active_claim_requires_recovery},
        {"PENDING call alone recovers and fenced UNKNOWN stays idle",pending_call_alone_requires_recovery_then_unknown_is_idle},
        {"old writer cannot write even when execution epochs stay equal",old_writer_actor_cannot_write_with_same_epochs},
        {"read-only actor cannot authorize a live writer",reader_actor_never_authorizes_writer},
        {"read-only actor cannot mutate workspace binding through WorkerTools",reader_actor_cannot_bind_a_workspace},
        {"foreign controls cannot consume current quota and same-session replay stays read-only",foreign_control_session_does_not_consume_current_rate_limit},
        {"all roles enforce writer identity before exact replay",all_roles_keep_session_fence_even_for_replay},
        {"explicit force recovery and tampered instruction witness",forced_recovery_preserves_originals_and_witness_guard},
        {"waiting and AMEND guards survive idle opens",waiting_and_amendment_are_not_recovery_authority},
        {"concurrent request opens preserve exclusive writer and single recovery",simultaneous_open_keeps_exclusive_writer},
        {"recovery versus captured write has no stale CAS commit",recovery_races_prepare_with_captured_identity},
        {"recovery versus claim preserves pending work",recovery_races_claim_without_losing_work},
        {"failed recovery rolls back all persistent tables",failed_demand_recovery_rolls_back_every_table},
        {"terminal pending closeout is idle and cannot restore business",terminal_pending_cleanup_is_idle_and_never_business_authority}});
}
