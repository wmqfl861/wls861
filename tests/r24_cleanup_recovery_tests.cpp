// R2.4 cleanup: actual temporary owned files, FakeModel, Store and delivery.
#include "wls/cleanup_runtime.hpp"
#include "wls/cleanup.hpp"
#include "wls/delivery.hpp"
#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <fstream>
#include <sqlite3.h>
#include <sys/wait.h>
#include <csignal>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif
namespace {
using namespace wls;
using namespace wls::s1;
using O=boost::json::object;
using A=boost::json::array;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::string base, migration, tools_sql, events_sql, workflow_sql, acceptance_sql, cleanup_sql;
std::unique_ptr<SchemaRegistry> schemas;
std::string text(const Json& j,std::string_view k){return std::string(j.at(k).as_string());}
std::int64_t num(const Json& j,std::string_view k){return j.at(k).to_number<std::int64_t>();}
void write(const std::filesystem::path& p,const std::string& s){std::ofstream f(p,std::ios::binary);f<<s;CHECK(f.good());}
void raw(const std::filesystem::path& path,const std::string& sql){sqlite3* db=nullptr;CHECK(sqlite3_open(path.c_str(),&db)==SQLITE_OK);
    const int code=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr);sqlite3_close(db);CHECK(code==SQLITE_OK);}
void expect(ErrorCode code,const std::function<void()>& action){try{action();}catch(const Error& e){
    if(e.code()!=code) { throw std::runtime_error(std::string("wrong code: ")+e.what()); }
    return;}throw std::runtime_error("missing error");}
struct Fixture {
    test::TempDir dir; std::filesystem::path db{dir.path/"state.sqlite"};
    std::unique_ptr<Store> store; std::unique_ptr<WorkerTools> tools;
    std::string id,task_id;Json resource,delivery,cancelled_run;AdmissionInput admission;std::filesystem::path work;
    EventFakeScript script;
    explicit Fixture(bool enabled=true){
        std::filesystem::create_directory(dir.path/"owned");std::filesystem::create_directory(dir.path/"delivery");
        auto bytes=read_runtime_file(source/"config/examples/system.yaml");
        const auto at=bytes.find("cleanup:\n  enabled: false");CHECK(at!=std::string::npos);
        if(enabled)bytes.replace(at,std::string("cleanup:\n  enabled: false").size(),"cleanup:\n  enabled: true");
        write(dir.path/"system.yaml",bytes);
        const auto config=load_config_catalog(dir.path/"system.yaml",source/"config/examples/functions",source/"prompts").freeze("general");
        open();AdmissionInput& in=admission;in.tenant_id="fixture";in.project_id="project";in.root_id="root";in.function_id="general";in.idempotency_key="request";
        in.original_text=read_runtime_file(source/"tests/fixtures/s14_request.txt");in.config_snapshot=config.json();
        in.constraint_catalog=parse_json_strict(read_runtime_file(source/"tests/fixtures/s14_constraints.json"));
        const auto grants=config.worker_tools();in.tool_allowlist.assign(grants.begin(),grants.end());in.budget={64,20000000,1000000,0};
        id=text(store->admit(in),"request_id");
        script=event_fake_script_from_json(parse_json_strict(read_runtime_file(source/"tests/fixtures/s15_fake_complete.json")));
        EventPumpOptions opts;opts.max_calls=1;opts.max_worker_calls_per_task=2;
        (void)EventRuntime(*store,*schemas,{}).pump(id,script,opts);
        task_id=text(store->tasks(id).at(0),"task_id");
        std::erase_if(script.default_steps,[](const auto& step){return step.role==Role::Dispatcher && step.role_call_index==1;});
        for(auto& step:script.default_steps) if(step.role==Role::Dispatcher)--step.role_call_index;
    }
    void open(StoreRecoveryScope scope={}){tools.reset();store.reset();store=std::make_unique<Store>(db,base,migration,StoreOpenMode::Takeover,tools_sql,events_sql,workflow_sql,acceptance_sql,cleanup_sql,std::move(scope));}
    StoreActor root(){return store->host_actor(Role::Root,id);}
    void allocate(){resource=allocate_owned_workspace(*store,id,task_id,dir.path/"owned","allocation");work=text(resource.at("identity").at("workspace_identity"),"path");}
    void accepted(bool owned=true, std::optional<bool> process_proof={}, bool cancel_before_run=false){
        if(owned)allocate();else {work=dir.path/"legacy";std::filesystem::create_directory(work);}
        tools=std::make_unique<WorkerTools>(*store,work);
        if(process_proof.has_value() || cancel_before_run) {
            EventPumpOptions first;first.max_calls=1;
            (void)EventRuntime(*store,*schemas,{{"alpha",tools.get()}}).pump(id,script,first);
            const auto binding=store->binding(task_id);const auto actor=store->host_actor(Role::Worker,id,binding.team_id);
            tools->bind(actor);
            if(cancel_before_run) {
                std::stop_source cancellation;
                cancellation.request_stop();
                cancelled_run=tools->execute(actor,binding,num(store->task(task_id),"state_version"),"cancelled-run",
                    "run",O{{"argv",A{"/bin/true"}}},"cancelled-run-command",cancellation.get_token());
            } else {
                auto op=store->prepare_tool_operation(actor,binding,num(store->task(task_id),"state_version"),"synthetic-run","run",O{{"argv",A{"/bin/true"}}},"synthetic-prepare");
                (void)store->start_tool_operation(actor,text(op,"op_id"),num(op,"state_version"),"synthetic-claim");
                // Controlled original-evidence fixture: no process is launched.
                (void)store->record_operation_result(text(op,"op_id"),binding,"UNKNOWN",O{{"spawned",true},{"all_descendants_exited",*process_proof},{"status","CLEANUP_UNKNOWN"}},O{{"fixture","synthetic-original-exit-observation"}});
                op=store->operation(text(op,"op_id"));
                (void)store->reconcile_operation(text(op,"op_id"),num(op,"state_version"),"FAILED","synthetic-outcome-only","synthetic-reconcile");
            }
            auto& steps=script.by_node.at("alpha");
            std::erase_if(steps,[](const auto& step){return step.role==Role::TeamLead && step.role_call_index==1;});
            for(auto& step:steps)if(step.role==Role::TeamLead)--step.role_call_index;
        }
        EventPumpOptions opts;opts.max_calls=12;opts.max_worker_calls_per_task=2;
        const auto result=EventRuntime(*store,*schemas,{{"alpha",tools.get()}}).pump(id,script,opts);
        if(store->task(task_id).at("status")!="ACCEPTED") { throw std::runtime_error(canonical_json(result)); }
        CHECK(store->experience_jobs(id).at(0).at("status")=="DONE");
        CHECK(result.at("real_model_calls")==0);tools.reset();
    }
    void delivered(bool owned=true, std::optional<bool> process_proof={}, bool cancel_before_run=false){accepted(owned,process_proof,cancel_before_run);DeliveryDirectory target(dir.path/"delivery");
        delivery=store->prepare_delivery(root(),store->request_binding(id),num(store->request(id),"state_version"),target.root_identity(),"prepare");
        auto result=target.publish(delivery.at("frozen_plan"));CHECK(result.status==DeliveryMutationStatus::Succeeded);
        delivery=store->record_delivery_publication(root(),store->request_binding(id),text(delivery,"delivery_id"),num(delivery,"state_version"),result.publication,"publish");
        delivery=store->acknowledge_delivery(root(),store->request_binding(id),text(delivery,"delivery_id"),text(delivery,"manifest_sha256"),
            num(delivery,"requirements_revision"),num(delivery,"plan_revision"),"fixture-recipient","durable_local","receipt",result.publication);
    }
    Json publication(){return DeliveryDirectory(dir.path/"delivery").verify_and_sync_existing(delivery.at("frozen_plan"));}
    Json begin(){resource=store->owned_workspaces(id).at(0);auto inv=resource.at("inventory");if(inv.is_null())inv=inventory_owned_workspace(resource);
        return store->begin_workspace_cleanup(root(),publication(),A{inv});}
};
Json snapshot(const Fixture& f) {
    sqlite3* db=nullptr; CHECK(sqlite3_open_v2(f.db.c_str(),&db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK);
    sqlite3_stmt* tables=nullptr;
    CHECK(sqlite3_prepare_v2(db,"SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name",-1,&tables,nullptr)==SQLITE_OK);
    O result;
    int rc{};
    while((rc=sqlite3_step(tables))==SQLITE_ROW) {
        const std::string name(reinterpret_cast<const char*>(sqlite3_column_text(tables,0)));
        const auto query="SELECT * FROM \""+name+"\"";
        sqlite3_stmt* statement=nullptr; CHECK(sqlite3_prepare_v2(db,query.c_str(),-1,&statement,nullptr)==SQLITE_OK);
        A rows; int step{};
        while((step=sqlite3_step(statement))==SQLITE_ROW) {
            std::string row;
            for(int col=0;col<sqlite3_column_count(statement);++col) {
                const auto size=sqlite3_column_bytes(statement,col);
                row+=std::to_string(sqlite3_column_type(statement,col))+":"+std::to_string(size)+":";
                if(size) row.append(static_cast<const char*>(sqlite3_column_blob(statement,col)),static_cast<std::size_t>(size));
                row+='|';
            }
            // Binary facts are encoded without assuming their UTF-8 validity.
            rows.emplace_back(sha256_hex(row));
        }
        CHECK(step==SQLITE_DONE); sqlite3_finalize(statement);
        std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.as_string()<b.as_string();});
        result[name]=std::move(rows);
    }
    CHECK(rc==SQLITE_DONE); sqlite3_finalize(tables); sqlite3_close(db); return result;
}
void clean_pending_job_has_no_recovery_writes() {
    Fixture f; f.delivered(); const auto before=snapshot(f);
    for(int i=0;i<3;++i){f.open(StoreRecoveryScope::request(f.id));CHECK(snapshot(f)==before);}
    CHECK(cleanup_owned_workspaces(*f.store,f.id).at("workspace_absence_verified_now")==true);
    CHECK(!std::filesystem::exists(f.work)); CHECK(f.publication().at("verified")==true);
}
void interrupted_cleanup_fences_actor_and_original_claim() {
    Fixture f; f.delivered(); const auto root=f.root(); const auto claim=f.begin();
    const auto resource=f.store->owned_workspaces(f.id).at(0);
    const auto old_request=f.store->request(f.id);
    f.open(StoreRecoveryScope::request(f.id));
    CHECK(f.store->request(f.id)==old_request);
    const auto job=f.store->closeout(f.id).at("cleanup_job");
    CHECK(num(job,"lease_epoch")==num(claim,"lease_epoch")+1);
    CHECK(job.at("status")=="PENDING" && job.at("claimed_by").is_null());
    const auto before=snapshot(f);
    expect(ErrorCode::Stale,[&]{f.store->authorize_workspace_cleanup(root,claim);});
    expect(ErrorCode::Stale,[&]{f.store->authorize_workspace_cleanup(f.root(),claim);});
    CHECK(snapshot(f)==before); CHECK(f.store->owned_workspaces(f.id).at(0).at("inventory")==resource.at("inventory"));
    CHECK(std::filesystem::exists(f.work/"result.txt"));
    CHECK(cleanup_owned_workspaces(*f.store,f.id).at("workspace_absence_verified_now")==true);
}
void idle_reopen_cannot_invent_process_exit() {
    Fixture f; f.delivered(true,false);
    const auto before=snapshot(f); f.open(StoreRecoveryScope::request(f.id)); CHECK(snapshot(f)==before);
    expect(ErrorCode::NotReady,[&]{(void)cleanup_owned_workspaces(*f.store,f.id);});
    CHECK(std::filesystem::exists(f.work/"result.txt")); CHECK(snapshot(f)==before);
}
void unowned_workspace_is_still_retained() {
    Fixture f; f.delivered(false); const auto before=snapshot(f);
    f.open(StoreRecoveryScope::request(f.id)); CHECK(snapshot(f)==before);
    expect(ErrorCode::NotReady,[&]{(void)cleanup_owned_workspaces(*f.store,f.id);});
    CHECK(std::filesystem::exists(f.work/"result.txt")); CHECK(snapshot(f)==before);
}
void incomplete_allocation_keeps_intent_without_adoption() {
    Fixture f; Workspace parent(f.dir.path/"owned",WorkspaceAccess::ReadOnly);
    const auto old=f.root(); const auto resource=f.store->prepare_owned_workspace(old,f.task_id,parent.root_identity(),"interrupted");
    const auto epoch=num(f.store->request(f.id),"dispatcher_epoch");
    f.open(StoreRecoveryScope::request(f.id));
    CHECK(num(f.store->request(f.id),"dispatcher_epoch")==epoch);
    CHECK(f.store->owned_workspaces(f.id).at(0)==resource);
    CHECK(!std::filesystem::exists(f.dir.path/"owned"/text(resource,"resource_id")));
    const auto before=snapshot(f);
    expect(ErrorCode::Stale,[&]{(void)f.store->prepare_owned_workspace(old,f.task_id,parent.root_identity(),"another-allocation");});
    CHECK(snapshot(f)==before);
}
void failed_cleanup_reclaim_rolls_back_all_rows() {
    Fixture f; f.delivered(); (void)f.begin(); f.tools.reset(); f.store.reset();
    raw(f.db,"CREATE TRIGGER r24_cleanup_fail BEFORE UPDATE ON cleanup_jobs BEGIN SELECT RAISE(ABORT,'r24 cleanup reclaim failure'); END;");
    const auto before=snapshot(f);
    expect(ErrorCode::Conflict,[&]{f.open(StoreRecoveryScope::request(f.id));}); CHECK(snapshot(f)==before);
    CHECK(std::filesystem::exists(f.work/"result.txt")); raw(f.db,"DROP TRIGGER r24_cleanup_fail");
    f.open(StoreRecoveryScope::request(f.id)); CHECK(cleanup_owned_workspaces(*f.store,f.id).at("workspace_absence_verified_now")==true);
}
}
int main(){
    base=read_runtime_file(source/"db/schema.sql");migration=read_runtime_file(source/"db/migrations/004_s1_store.sql");tools_sql=read_runtime_file(source/"db/migrations/005_worker_tools.sql");
    events_sql=read_runtime_file(source/"db/migrations/006_event_runtime.sql");workflow_sql=read_runtime_file(source/"db/migrations/007_workflow_runtime.sql");acceptance_sql=read_runtime_file(source/"db/migrations/008_acceptance_runtime.sql");cleanup_sql=read_runtime_file(source/"db/migrations/009_owned_cleanup.sql");schemas=std::make_unique<SchemaRegistry>(source/"schemas");
    return test::run({{"clean pending closeout is unchanged by repeated selected open",clean_pending_job_has_no_recovery_writes},
        {"interrupted cleanup fences old actor and original claim",interrupted_cleanup_fences_actor_and_original_claim},
        {"idle reopen never replaces original process exit proof",idle_reopen_cannot_invent_process_exit},
        {"idle reopen never adopts a legacy workspace",unowned_workspace_is_still_retained},
        {"incomplete allocation preserves its intent and rejects the old session",incomplete_allocation_keeps_intent_without_adoption},
        {"cleanup reclaim failure is atomic and leaves work intact",failed_cleanup_reclaim_rolls_back_all_rows}});
}
