#include "wls/worker_history.hpp"
#include "wls/offline_runtime.hpp"
#include "test_support.hpp"
#include <fstream>
#include <memory>
#include <sqlite3.h>

namespace {
using namespace wls::s1;
using wls::Role;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path source(WLS_SOURCE_DIR);
const Json policy = Object{{"mode", "window-v1"}, {"recent_attempts", 4}, {"inline_bytes", 32768}};
std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
std::int64_t num(const Json& value, std::string_view key) { return value.at(key).as_int64(); }
void expect(ErrorCode code, const std::function<void()>& f) {
    try { f(); } catch (const Error& e) { CHECK(e.code() == code); return; }
    throw std::runtime_error("expected failure");
}
void sql(const std::filesystem::path& path, const std::string& query) {
    sqlite3* db{}; CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    const auto code = sqlite3_exec(db, query.c_str(), nullptr, nullptr, nullptr);
    const std::string error = sqlite3_errmsg(db); sqlite3_close(db);
    if (code != SQLITE_OK) throw std::runtime_error(error);
}
struct Fixture {
    test::TempDir temp;
    SchemaRegistry schemas{source / "schemas"};
    std::unique_ptr<Store> store;
    std::unique_ptr<OfflineRuntime> runner;
    std::string request, task;
    std::filesystem::path db{temp.path / "state.db"};
    Fixture() {
        auto yaml = read_runtime_file(source / "config/examples/system.yaml");
        const std::string old = "  request_format: framed-v1\n";
        yaml.replace(yaml.find(old), old.size(), "  request_format: structured-v2\n"
            "  worker_history: {mode: window-v1, recent_attempts: 4, inline_bytes: 32768}\n");
        { std::ofstream f(temp.path / "system.yaml"); f << yaml; CHECK(f.good()); }
        const auto config = load_config_catalog(temp.path / "system.yaml", source / "config/examples/functions", source / "prompts").freeze("general");
        store = std::make_unique<Store>(db, read_runtime_file(source / "db/schema.sql"),
            read_runtime_file(source / "db/migrations/004_s1_store.sql"), StoreOpenMode::Takeover,
            read_runtime_file(source / "db/migrations/005_worker_tools.sql"),
            read_runtime_file(source / "db/migrations/006_event_runtime.sql"),
            read_runtime_file(source / "db/migrations/007_workflow_runtime.sql"),
            read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql"),
            read_runtime_file(source / "db/migrations/009_owned_cleanup.sql"));
        AdmissionInput a; a.tenant_id="test"; a.project_id="test"; a.root_id="root"; a.function_id="general";
        a.idempotency_key="test"; a.original_text=read_runtime_file(source/"tests/fixtures/s1_request.txt");
        a.constraint_catalog=parse_json_strict(read_runtime_file(source/"tests/fixtures/s1_constraints.json"));
        a.config_snapshot=config.json(); const auto tools=config.worker_tools(); a.tool_allowlist.assign(tools.begin(),tools.end());
        a.budget={500, 8000000, 800000, 0}; request=text(store->admit(a),"request_id");
        runner=std::make_unique<OfflineRuntime>(*store,schemas);
        const auto steps=fake_model_steps_from_json(parse_json_strict(read_runtime_file(source/"tests/fixtures/s1_fake_initial.json")));
        runner->run(request,steps); task=text(store->tasks(request).front(),"task_id");
    }
    StoreActor actor() { return store->host_actor(Role::Worker,request,store->binding(task).team_id); }
    FakeModel fake(std::string name="history_page", Json args=Object{{"after_sequence",0},{"through_sequence",0},{"limit",1}}) {
        FakeModelStep step; step.role=Role::Worker; step.role_call_index=1;
        step.tool_calls={{"lookup",std::move(name),std::move(args)}}; step.usage={1,1,0,0,0};
        return FakeModel({step});
    }
};
void empty_policy_defaults() {
    const auto c=load_config_catalog(source/"config/examples/system.yaml",source/"config/examples/functions",source/"prompts").freeze("general");
    CHECK(c.worker_history_policy().is_null()); CHECK(c.runtime_support().at("history_window_integrated")==false);
}
void tail_is_original_and_small_turn_is_paired() {
    Fixture f; const auto tail=f.store->attempt_history_tail(f.actor(),4);
    CHECK(tail.at("total_count")==1); CHECK(tail.at("omitted_count")==0);
    const auto messages=worker_history_messages(tail,policy);
    CHECK(messages.size()==3); CHECK(messages[0].role=="user"); CHECK(messages[1].role=="assistant");
    CHECK(messages[1].tool_calls.front().call_id==messages[2].tool_call_id);
    const auto id=text(tail.at("items").as_array().front(),"attempt_id");
    CHECK(parse_json_strict(messages[2].content)==f.store->tool_feedback(id).at("result"));
}
void large_turn_is_explicit_reference() {
    Fixture f; auto tail=f.store->attempt_history_tail(f.actor(),4);
    auto& item=tail.at("items").as_array().front();
    item.at("feedback").as_object()["result"]=Object{{"content",std::string(61100,'x')}};
    // Pure projection test: storage integrity is separately tested below. These
    // synthetic receipts are never inserted into a real Store.
    const auto messages=worker_history_messages(tail,policy);
    CHECK(messages.size()==2 && messages[1].role=="user" && messages[1].tool_calls.empty());
    const auto ref=parse_json_strict(messages[1].content);
    CHECK(ref.at("kind")=="history_reference"); CHECK(ref.at("not_execution_authority")==true);
    CHECK(text(ref.at("result"),"section_sha256")==sha256_hex(canonical_json(item.at("feedback").at("result"))));
    CHECK(ref.at("record_sha256")==item.at("feedback").at("record_sha256"));
}
void exact_control_replay_is_not_a_second_read_or_charge() {
    Fixture f; auto fake=f.fake(); const auto call=f.runner->reserve_call(fake,f.request,Role::Worker,f.task);
    const auto response=fake.complete(call.request); const auto result=f.runner->accept_response(call,response);
    const auto budget=f.store->budget(f.request); const auto receipt=f.store->tool_feedback(call.request.attempt_id);
    CHECK(result.at("format")=="wls-history-index-v1");
    CHECK(f.runner->accept_response(call,response)==result);
    CHECK(f.store->budget(f.request)==budget && f.store->tool_feedback(call.request.attempt_id)==receipt);
    CHECK(num(budget,"attempts")==4);
}
void changed_semantics_fence_captured_reads() {
    Fixture f; auto fake=f.fake(); const auto call=f.runner->reserve_call(fake,f.request,Role::Worker,f.task);
    const auto response=fake.complete(call.request);
    f.store->amend(f.store->host_actor(Role::Root,f.request),num(f.store->request(f.request),"state_version"),
        read_runtime_file(source/"tests/fixtures/s1_amendment.txt"),
        parse_json_strict(read_runtime_file(source/"tests/fixtures/s1_amended_constraints.json")),"amend");
    expect(ErrorCode::Stale,[&]{f.runner->accept_response(call,response);});
    CHECK(f.store->tool_feedback(call.request.attempt_id).at("result").at("runtime_error")==true);
}
void current_policy_refuses_window_before_reservation() {
    Fixture f; sql(f.db,"UPDATE runtime_policy SET denied_tools='[\"history_read\"]',state_version=state_version+1");
    const auto budget=f.store->budget(f.request); auto fake=f.fake();
    expect(ErrorCode::Unauthorized,[&]{f.runner->reserve_call(fake,f.request,Role::Worker,f.task);});
    CHECK(f.store->budget(f.request)==budget);
    expect(ErrorCode::Unauthorized,[&]{execute_history_control(*f.store,f.actor(),f.store->binding(f.task),"history_read",
        Object{{"attempt_id","other"},{"record_sha256",std::string(64,'0')},{"section","result"},{"offset",0},{"max_bytes",1}});});
}
void waiting_and_unknown_survive_window_projection() {
    Fixture f; const auto actor=f.actor();const auto binding=f.store->binding(f.task);
    const auto op=f.store->prepare_operation(actor,binding,num(f.store->task(f.task),"state_version"),"new-effect","note",Object{{"text","pending"}},"none","prepare");
    f.store->start_operation(actor,text(op,"op_id"),num(op,"state_version"),"start");
    sql(f.db,"UPDATE ops SET status='UNKNOWN',state_version=state_version+1 WHERE op_id='"+text(op,"op_id")+"'");
    const auto tail=f.store->attempt_history_tail(actor,4);
    CHECK(tail.at("unresolved_operations").as_array().size()==1);
    const auto messages=worker_history_messages(tail,policy);
    CHECK(parse_json_strict(messages.front().content).at("unresolved_operations").as_array().front().at("status")=="UNKNOWN");
    auto fake=f.fake(); const auto budget=f.store->budget(f.request);
    expect(ErrorCode::UnknownOutcome,[&]{f.runner->reserve_call(fake,f.request,Role::Worker,f.task);});
    CHECK(f.store->budget(f.request)==budget);
}
void missing_attempt_outside_tail_remains_pinned() {
    Fixture f; auto fake=f.fake();
    const auto pending=f.runner->reserve_call(fake,f.request,Role::Worker,f.task);
    for(int i=0;i<6;++i) {
        const auto current=f.runner->reserve_call(fake,f.request,Role::Worker,f.task);
        f.runner->accept_response(current,fake.complete(current.request));
    }
    const auto tail=f.store->attempt_history_tail(f.actor(),4);
    CHECK(tail.at("omitted_count").as_int64()>0);
    const auto& unresolved=tail.at("unresolved_attempts").as_array();
    CHECK(unresolved.size()==1 && text(unresolved.front(),"attempt_id")==pending.request.attempt_id);
    const auto messages=worker_history_messages(tail,policy);
    CHECK(parse_json_strict(messages.front().content).at("unresolved_attempts")==tail.at("unresolved_attempts"));
    for(const auto& item:tail.at("items").as_array())
        CHECK(text(item,"attempt_id")!=pending.request.attempt_id);
}
void policy_revocation_fences_an_already_captured_read() {
    Fixture f; auto fake=f.fake();
    const auto call=f.runner->reserve_call(fake,f.request,Role::Worker,f.task);
    sql(f.db,"UPDATE runtime_policy SET denied_tools='[\"history_page\"]',state_version=state_version+1");
    expect(ErrorCode::Unauthorized,[&]{f.runner->accept_response(call,fake.complete(call.request));});
    CHECK(f.store->tool_feedback(call.request.attempt_id).at("result").at("runtime_error")==true);
}
void original_corruption_is_not_hidden_by_windowing() {
    Fixture f; const auto id=text(f.store->attempt_history_tail(f.actor(),4).at("items").as_array().front(),"attempt_id");
    sqlite3* db{}; CHECK(sqlite3_open(f.db.c_str(),&db)==SQLITE_OK); sqlite3_stmt* stmt{};
    CHECK(sqlite3_prepare_v2(db,"SELECT name,sql FROM sqlite_master WHERE type='trigger' AND tbl_name='model_tool_feedback'",-1,&stmt,nullptr)==SQLITE_OK);
    std::vector<std::pair<std::string,std::string>> triggers;
    while(sqlite3_step(stmt)==SQLITE_ROW) triggers.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt,0)),reinterpret_cast<const char*>(sqlite3_column_text(stmt,1)));
    sqlite3_finalize(stmt);sqlite3_close(db);CHECK(!triggers.empty());
    for(const auto& [name,definition]:triggers) { (void)definition;sql(f.db,"DROP TRIGGER "+name); }
    sql(f.db,"UPDATE model_tool_feedback SET result='{}' WHERE attempt_id='"+id+"'");
    for(const auto& [name,definition]:triggers) { (void)name;sql(f.db,definition); }
    expect(ErrorCode::Corrupt,[&]{f.store->attempt_history_tail(f.actor(),4);});
}
} // namespace
int main() {return test::run({
    {"missing frozen window preserves legacy",empty_policy_defaults},
    {"scoped small original turn remains paired",tail_is_original_and_small_turn_is_paired},
    {"large whole turn becomes explicit original reference",large_turn_is_explicit_reference},
    {"readback exact replay preserves receipt and budget",exact_control_replay_is_not_a_second_read_or_charge},
    {"AMEND fences in-flight historical reads",changed_semantics_fence_captured_reads},
    {"revoked readback refuses window before reservation",current_policy_refuses_window_before_reservation},
    {"UNKNOWN is pinned and still blocks reservation",waiting_and_unknown_survive_window_projection},
    {"missing old attempt remains pinned outside tail",missing_attempt_outside_tail_remains_pinned},
    {"policy revocation fences captured read",policy_revocation_fences_an_already_captured_read},
    {"recent corruption remains fail closed",original_corruption_is_not_hidden_by_windowing}
});}
