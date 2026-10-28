#include "wls/cleanup_runtime.hpp"
#include "wls/cleanup.hpp"
#include "wls/delivery.hpp"
#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "test_support.hpp"
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
    void open(){tools.reset();store.reset();store=std::make_unique<Store>(db,base,migration,StoreOpenMode::Takeover,tools_sql,events_sql,workflow_sql,acceptance_sql,cleanup_sql);}
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
void success(){Fixture f;f.delivered();const auto result=cleanup_owned_workspaces(*f.store,f.id);
    CHECK(result.at("workspace_cleanup_recorded")==true);CHECK(result.at("workspace_absence_verified_now")==true);
    CHECK(result.at("cleanup_complete")==false&&result.at("private_data_retained")==true);
    CHECK(!std::filesystem::exists(f.work));CHECK(f.store->request(f.id).at("status")=="DELIVERED");
    CHECK(f.publication().at("verified")==true);CHECK(cleanup_owned_workspaces(*f.store,f.id).at("pass_removed_entries")==0);}
void disabled(){Fixture f(false);f.delivered();expect(ErrorCode::NotReady,[&]{(void)cleanup_owned_workspaces(*f.store,f.id);});CHECK(std::filesystem::exists(f.work/"result.txt"));}
void unowned(){Fixture f;f.delivered(false);expect(ErrorCode::NotReady,[&]{(void)cleanup_owned_workspaces(*f.store,f.id);});CHECK(std::filesystem::exists(f.work));}
void missing_package(){Fixture f;f.delivered();
    const auto package=f.dir.path/"delivery"/text(f.delivery,"delivery_id");
    const auto retained=f.dir.path/"saved-package";
    const auto permissions=std::filesystem::status(package).permissions();
    // Fault injection on this test's own directory only. Moving a read-only
    // directory across parents otherwise fails before the cleanup guard runs.
    std::filesystem::permissions(package,std::filesystem::perms::owner_write,std::filesystem::perm_options::add);
    std::filesystem::rename(package,retained);
    std::filesystem::permissions(retained,permissions);
    CHECK(!std::filesystem::exists(package));
    test::throws<Error>([&]{(void)cleanup_owned_workspaces(*f.store,f.id);});
    CHECK(!std::filesystem::exists(package));CHECK(std::filesystem::exists(f.work/"result.txt"));
    CHECK(std::filesystem::exists(retained/"manifest.json"));}
void corrupt_package(){Fixture f;f.delivered();
    const auto manifest=f.dir.path/"delivery"/text(f.delivery,"delivery_id")/"manifest.json";
    const auto permissions=std::filesystem::status(manifest).permissions();
    // Restore the original mode after injecting changed bytes: rejection must
    // detect the damaged manifest, not a fixture setup permission failure.
    std::filesystem::permissions(manifest,std::filesystem::perms::owner_write,std::filesystem::perm_options::add);
    write(manifest,"{}");std::filesystem::permissions(manifest,permissions);
    CHECK(read_runtime_file(manifest)=="{}");
    expect(ErrorCode::Conflict,[&]{(void)cleanup_owned_workspaces(*f.store,f.id);});
    CHECK(std::filesystem::exists(f.work/"result.txt"));}
void readonly_delivery_survives_cleanup(){Fixture f;f.delivered();
    const auto package=f.dir.path/"delivery"/text(f.delivery,"delivery_id");
    const auto check_modes=[&]{
        CHECK(std::filesystem::status(package).permissions()==(std::filesystem::perms::owner_read|std::filesystem::perms::owner_exec));
        for(const auto& item:std::filesystem::directory_iterator(package))
            CHECK(item.status().permissions()==std::filesystem::perms::owner_read);
    };
    check_modes();
    if(::geteuid()!=0){
        std::ofstream denied(package/"manifest.json",std::ios::binary|std::ios::app);
        CHECK(!denied.is_open());
    }
    CHECK(cleanup_owned_workspaces(*f.store,f.id).at("workspace_absence_verified_now")==true);
    check_modes();CHECK(f.publication().at("verified")==true);
}
void partial_restart(){Fixture f;f.delivered();write(f.work/"another","original");auto result=cleanup_owned_workspaces(*f.store,f.id,1);
    CHECK(result.at("workspace_cleanup_recorded")==false);CHECK(result.at("deletion_performed").is_null());CHECK(result.at("pass_removed_entries")==1);f.open();result=cleanup_owned_workspaces(*f.store,f.id);CHECK(result.at("workspace_cleanup_recorded")==true);}
void stale_claim(){Fixture f;f.delivered();const auto claim=f.begin();f.open();expect(ErrorCode::Stale,[&]{f.store->authorize_workspace_cleanup(f.root(),claim);});
    CHECK(cleanup_owned_workspaces(*f.store,f.id).at("workspace_cleanup_recorded")==true);}
void freeze_not_rescan(){Fixture f;f.delivered();(void)f.begin();write(f.work/"extra","not in original inventory");f.open();
    expect(ErrorCode::Conflict,[&]{(void)cleanup_owned_workspaces(*f.store,f.id);});CHECK(std::filesystem::exists(f.work/"result.txt"));}
void changed_inventory(){Fixture f;f.delivered();(void)f.begin();auto resource=f.store->owned_workspaces(f.id).at(0);auto inv=resource.at("inventory");inv.as_object()["entries"]=A{};
    expect(ErrorCode::Conflict,[&]{(void)f.store->begin_workspace_cleanup(f.root(),f.publication(),A{inv});});}
void failed_record_resumes(){Fixture f;f.delivered();raw(f.db,"CREATE TRIGGER fail_proof BEFORE UPDATE ON owned_workspaces WHEN NEW.proof IS NOT NULL BEGIN SELECT RAISE(ABORT,'injected disk failure'); END;");
    expect(ErrorCode::Conflict,[&]{(void)cleanup_owned_workspaces(*f.store,f.id);});CHECK(!std::filesystem::exists(f.work));
    CHECK(f.store->owned_workspaces(f.id).at(0).at("status")=="DELETING");raw(f.db,"DROP TRIGGER fail_proof;");f.open();CHECK(cleanup_owned_workspaces(*f.store,f.id).at("workspace_cleanup_recorded")==true);}
void unauthorized(){Fixture f;f.delivered();const auto resource=f.store->owned_workspaces(f.id).at(0);const auto inv=inventory_owned_workspace(resource);
    expect(ErrorCode::Unauthorized,[&]{(void)f.store->begin_workspace_cleanup(f.store->host_actor(Role::Dispatcher,f.id),f.publication(),A{inv});});}
void key_and_binding(){Fixture f;f.allocate();CHECK(allocate_owned_workspace(*f.store,f.id,f.task_id,f.dir.path/"owned","allocation").at("resource_id")==f.resource.at("resource_id"));
    expect(ErrorCode::Conflict,[&]{(void)allocate_owned_workspace(*f.store,f.id,f.task_id,f.dir.path/"owned","different");});
    const auto identity=f.resource.at("identity").at("workspace_identity");expect(ErrorCode::Unauthorized,[&]{(void)f.store->bind_workspace(text(f.resource,"team_id"),f.dir.path.string(),identity.at("device").to_number<std::uint64_t>(),identity.at("inode").to_number<std::uint64_t>());});}
void migration_check(){Fixture f;f.allocate();f.store.reset();expect(ErrorCode::Unsupported,[&]{Store old(f.db,base,migration,StoreOpenMode::ReadOnly,tools_sql,events_sql,workflow_sql,acceptance_sql);});
    expect(ErrorCode::Corrupt,[&]{Store changed(f.db,base,migration,StoreOpenMode::ReadOnly,tools_sql,events_sql,workflow_sql,acceptance_sql,cleanup_sql+"\n");});
    f.open();CHECK(f.store->diagnostics().at("user_version")==407);CHECK(f.store->owned_workspaces(f.id).size()==1);}
void killed_after_unlink(){Fixture f;f.delivered();f.store.reset();const auto child=::fork();CHECK(child>=0);
    if(child==0){try{f.open();const auto claim=f.begin();(void)claim;const auto resource=f.store->owned_workspaces(f.id).at(0);
        (void)erase_owned_workspace(resource,resource.at("inventory"));(void)::kill(::getpid(),SIGKILL);}catch(...){::_exit(3);}::_exit(4);}
    int status=0;CHECK(::waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    f.open();CHECK(cleanup_owned_workspaces(*f.store,f.id).at("workspace_cleanup_recorded")==true);}
void outcome_reconciliation_is_not_exit_proof(){Fixture f;f.delivered(true,false);
    expect(ErrorCode::NotReady,[&]{(void)cleanup_owned_workspaces(*f.store,f.id);});CHECK(std::filesystem::exists(f.work));
    CHECK(f.store->workspace_cleanup_status(f.id).at("workspace_eligible")==false);}
void original_exit_fact_is_bound(){Fixture f;f.delivered(true,true);
    CHECK(cleanup_owned_workspaces(*f.store,f.id).at("workspace_cleanup_recorded")==true);}
void original_pre_spawn_cancellation_permits_cleanup() {
    Fixture f;
    f.delivered(true,std::nullopt,true);
    const auto status=f.store->workspace_cleanup_status(f.id);
    if(!status.at("workspace_eligible").as_bool())
        throw std::runtime_error("pre-spawn cancellation blocks cleanup: "+canonical_json(status.at("workspace_blockers")));
    const auto& original=f.cancelled_run.at("execution_result");
    CHECK(original.at("observed_outcome")=="CANCELLED");
    CHECK(original.at("result").at("spawned")==false);
    CHECK(original.at("result").at("all_descendants_exited")==true);
    CHECK(cleanup_owned_workspaces(*f.store,f.id).at("workspace_cleanup_recorded")==true);
    CHECK(f.store->operation_result(text(f.cancelled_run.at("operation"),"op_id"))==original);
}
void populated_406_migration(){Fixture f(false);const auto path=f.dir.path/"old.sqlite";std::string id;Json original, original_texts;
    {Store old(path,base,migration,StoreOpenMode::Takeover,tools_sql,events_sql,workflow_sql,acceptance_sql);
        const auto admitted=old.admit(f.admission);id=text(admitted,"request_id");original=old.request(id);original_texts=old.request_snapshot(id);CHECK(old.diagnostics().at("user_version")==406);}
    {Store upgraded(path,base,migration,StoreOpenMode::Takeover,tools_sql,events_sql,workflow_sql,acceptance_sql,cleanup_sql);
        CHECK(upgraded.diagnostics().at("user_version")==407);const auto after=upgraded.request(id);
        CHECK(upgraded.request_snapshot(id)==original_texts);CHECK(after.at("config_snapshot")==original.at("config_snapshot"));
        CHECK(upgraded.owned_workspaces(id).empty());}}
void nested_allocations_refused(){Fixture f;f.allocate();
    // A fresh independent request cannot put ownership inside the old work.
    auto in=f.admission;in.idempotency_key="other-request";const auto other=text(f.store->admit(in),"request_id");
    const auto original=event_fake_script_from_json(parse_json_strict(read_runtime_file(source/"tests/fixtures/s15_fake_complete.json")));
    EventPumpOptions opts;opts.max_calls=1;(void)EventRuntime(*f.store,*schemas,{}).pump(other,original,opts);
    expect(ErrorCode::Unauthorized,[&]{(void)allocate_owned_workspace(*f.store,other,text(f.store->tasks(other).at(0),"task_id"),f.work,"nested");});
    CHECK(f.store->owned_workspaces(other).empty());}
void incomplete_creation(){Fixture f;Workspace parent(f.dir.path/"owned",WorkspaceAccess::ReadOnly);
    const auto resource=f.store->prepare_owned_workspace(f.root(),f.task_id,parent.root_identity(),"allocated");
    std::filesystem::create_directory(f.dir.path/"owned"/text(resource,"resource_id"));
    test::throws<Error>([&]{(void)allocate_owned_workspace(*f.store,f.id,f.task_id,f.dir.path/"owned","allocated");});
    CHECK(f.store->owned_workspaces(f.id).at(0).at("status")=="PREPARED");}
}
int main(){
    base=read_runtime_file(source/"db/schema.sql");migration=read_runtime_file(source/"db/migrations/004_s1_store.sql");tools_sql=read_runtime_file(source/"db/migrations/005_worker_tools.sql");
    events_sql=read_runtime_file(source/"db/migrations/006_event_runtime.sql");workflow_sql=read_runtime_file(source/"db/migrations/007_workflow_runtime.sql");acceptance_sql=read_runtime_file(source/"db/migrations/008_acceptance_runtime.sql");cleanup_sql=read_runtime_file(source/"db/migrations/009_owned_cleanup.sql");schemas=std::make_unique<SchemaRegistry>(source/"schemas");
    return test::run({{"real owned cleanup retains evidence",success},{"default disabled",disabled},{"legacy workspace not adopted",unowned},
        {"lost delivery never reconstructed",missing_package},{"changed delivery blocks deletion",corrupt_package},{"delivery stays read-only after cleanup",readonly_delivery_survives_cleanup},{"bounded pass survives restart",partial_restart},
        {"old claim fenced on takeover",stale_claim},{"frozen inventory cannot widen",freeze_not_rescan},{"inventory mismatch refused",changed_inventory},
        {"database write failure after deletion recovers",failed_record_resumes},{"only Root authorizes cleanup",unauthorized},{"allocation idempotency and binding",key_and_binding},
        {"407 migration fingerprints",migration_check},{"SIGKILL after physical deletion recovers",killed_after_unlink},{"missing ownership marker is not adopted",incomplete_creation},
        {"outcome reconciliation does not prove process exit",outcome_reconciliation_is_not_exit_proof},
        {"original exit fact fingerprint is bound",original_exit_fact_is_bound},
        {"actual pre-spawn cancellation preserves the cleanup exit",original_pre_spawn_cancellation_permits_cleanup},
        {"populated 406 migration retains immutable inputs",populated_406_migration},
        {"owned allocations cannot nest across requests",nested_allocations_refused}});
}
