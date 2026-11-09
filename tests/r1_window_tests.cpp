#include "wls/worker_history.hpp"
#include "wls/offline_runtime.hpp"
#include "test_support.hpp"
#include <cstdint>
#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <string>

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

// ---- G1-4 preparation: frozen utf8-token-v1 readback (Worker side) --------
// These cases drive the new contract only through existing public boundaries
// (frozen configuration, FakeModel control calls, Store receipts). They fail
// today at the frozen configuration load because `readback` is still an
// unknown context key; that rejection is the recorded RED for this suite.

std::filesystem::path write_readback_system_yaml(const std::filesystem::path& directory,
        std::int64_t input_budget, std::int64_t page_tokens, std::int64_t output_budget) {
    auto yaml = read_runtime_file(source / "config/examples/system.yaml");
    const std::string marker = "  request_format: framed-v1\n";
    const auto at = yaml.find(marker); CHECK(at != std::string::npos);
    yaml.replace(at, marker.size(), "  request_format: structured-v2\n"
        "  worker_history: {mode: window-v1, recent_attempts: 3, inline_bytes: 65536}\n"
        "  readback: {mode: utf8-token-v1, token_estimator: utf8-byte-v1, max_page_tokens: " +
        std::to_string(page_tokens) + "}\n");
    const std::string input_marker = "  max_input_tokens: null\n";
    const auto input_at = yaml.find(input_marker); CHECK(input_at != std::string::npos);
    yaml.replace(input_at, input_marker.size(), "  max_input_tokens: " + std::to_string(input_budget) + "\n");
    const std::string output_marker = "  max_output_tokens: null\n";
    const auto output_at = yaml.find(output_marker); CHECK(output_at != std::string::npos);
    yaml.replace(output_at, output_marker.size(), "  max_output_tokens: " + std::to_string(output_budget) + "\n");
    { std::ofstream file(directory / "system.yaml"); file << yaml; CHECK(file.good()); }
    return directory / "system.yaml";
}

Json v2_read_arguments(const std::string& attempt, const std::string& hash,
                       std::int64_t offset, std::int64_t max_tokens) {
    return Object{{"attempt_id", attempt}, {"record_sha256", hash}, {"section", "result"},
        {"offset", offset}, {"max_tokens", max_tokens}};
}

FakeModel v2_control(const std::string& name, const Json& arguments) {
    FakeModelStep step; step.role=Role::Worker; step.role_call_index=1;
    step.tool_calls={{"lookup", name, arguments}}; step.usage={1,1,0,0,0};
    return FakeModel({step});
}

FakeModel worker_note(const std::string& message) {
    FakeModelStep step; step.role=Role::Worker; step.role_call_index=1;
    step.tool_calls={{"probe", "note", Object{{"text", message}}}}; step.usage={1,1,0,0,0};
    return FakeModel({step});
}

std::string base64_bytes(std::string_view bytes) {
    std::string result((bytes.size() + 2) / 3 * 4, '\0');
    const auto count = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    CHECK(count >= 0);
    result.resize(static_cast<std::size_t>(count));
    return result;
}
std::string base64_text(std::string_view value) {
    CHECK(!value.empty() && value.size() % 4 == 0);
    std::string bytes(value.size() / 4 * 3, '\0');
    const auto count = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(bytes.data()),
        reinterpret_cast<const unsigned char*>(value.data()), static_cast<int>(value.size()));
    CHECK(count >= 0);
    auto size = static_cast<std::size_t>(count);
    if (value.back() == '=') --size;
    if (value[value.size() - 2] == '=') --size;
    bytes.resize(size);
    return bytes;
}
// The complete result of a page must enter the immediately following actually
// captured model input as that structured tool-result message: compare the
// parsed tool message of the captured structured context with the exact page,
// never a prefix or a whole-prompt substring of possibly escaped bytes.
void require_captured_tool_result(const CompiledContext& context, const Json& result) {
    CHECK(context.structured());
    std::size_t matches = 0;
    const Json* latest_tool = nullptr;
    for (const auto& message : context.logical_input.at("messages").as_array()) {
        if (message.at("role") != "tool") continue;
        if (parse_json_strict(std::string(message.at("content").as_string())) == result) ++matches;
        latest_tool = &message;
    }
    CHECK(matches == 1);
    CHECK(latest_tool != nullptr);
    CHECK(parse_json_strict(std::string(latest_tool->at("content").as_string())) == result);
}

struct ReadbackFixture {
    test::TempDir temp;
    SchemaRegistry schemas{source / "schemas"};
    std::int64_t input_budget, page_tokens, output_budget;
    std::filesystem::path yaml{write_readback_system_yaml(temp.path, input_budget, page_tokens, output_budget)};
    FrozenRequestConfig config{load_config_catalog(yaml, source / "config/examples/functions",
        source / "prompts").freeze("general")};
    std::unique_ptr<Store> store;
    std::unique_ptr<OfflineRuntime> runner;
    std::string request, task;
    std::filesystem::path db{temp.path / "state.db"};
    explicit ReadbackFixture(std::int64_t input = 120000, std::int64_t page = 24576,
                             std::int64_t output = 8192)
        : input_budget(input), page_tokens(page), output_budget(output) {
        reopen();
        AdmissionInput a; a.tenant_id="test"; a.project_id="test"; a.root_id="root"; a.function_id="general";
        a.idempotency_key="test"; a.original_text=read_runtime_file(source/"tests/fixtures/s1_request.txt");
        a.constraint_catalog=parse_json_strict(read_runtime_file(source/"tests/fixtures/s1_constraints.json"));
        a.config_snapshot=config.json(); const auto tools=config.worker_tools(); a.tool_allowlist.assign(tools.begin(),tools.end());
        a.budget={500, 8000000, 800000, 0}; request=text(store->admit(a),"request_id");
        runner=std::make_unique<OfflineRuntime>(*store,schemas);
        const auto steps=fake_model_steps_from_json(parse_json_strict(read_runtime_file(source/"tests/fixtures/s1_fake_initial.json")));
        runner->run(request,steps); task=text(store->tasks(request).front(),"task_id");
    }
    void reopen() {
        store.reset();
        store = std::make_unique<Store>(db, read_runtime_file(source / "db/schema.sql"),
            read_runtime_file(source / "db/migrations/004_s1_store.sql"), StoreOpenMode::Takeover,
            read_runtime_file(source / "db/migrations/005_worker_tools.sql"),
            read_runtime_file(source / "db/migrations/006_event_runtime.sql"),
            read_runtime_file(source / "db/migrations/007_workflow_runtime.sql"),
            read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql"),
            read_runtime_file(source / "db/migrations/009_owned_cleanup.sql"));
        if (!request.empty()) runner=std::make_unique<OfflineRuntime>(*store,schemas);
    }
    StoreActor actor() { return store->host_actor(Role::Worker,request,store->binding(task).team_id); }
    // A settled Worker attempt with an immutable original result, admitted
    // through the same public Store reservation path as production attempts.
    std::string add_result(const Json& result) {
        const auto a = actor(); const auto b = store->binding(task);
        ContextInput context; context.role = Role::Worker; context.model = config.resolve(Role::Worker);
        const auto profile = config.model_profile(Role::Worker);
        context.adapter_version = text(profile, "adapter_version");
        context.protocol_version = text(profile, "protocol");
        context.system_prompt = "readback fixture"; context.role_prompt = "Worker";
        context.function_prompt = "preserve originals"; context.task_contract = "complete original test contract";
        ModelRequest call; call.call_id = new_id("call"); call.attempt_id = new_id("attempt");
        call.role = Role::Worker; call.binding = b; call.config_snapshot_id = config.sha256();
        call.model = context.model; call.context = compile_structured_context(context, {});
        call.max_output_tokens = 50;
        call.reserved_input_tokens = static_cast<std::int64_t>(call.context.prompt().size());
        call.reserved_output_tokens = 50;
        AttemptReservation reserve; reserve.call_id = call.call_id; reserve.attempt_id = call.attempt_id;
        reserve.config_snapshot_sha256 = config.sha256(); reserve.model_ref = call.model.model;
        reserve.max_output_tokens = 50; reserve.reserved_input_tokens = call.reserved_input_tokens;
        reserve.reserved_output_tokens = 50;
        reserve.request_envelope = to_json(call); store->reserve_attempt(a, b, reserve);
        const Json response = Object{{"text", "original model output"}, {"tool_call_id", "readback-source-id"}};
        store->settle_attempt(call.attempt_id, b, config.sha256(), "COMPLETED",
            Object{{"input_tokens", 1}, {"output_tokens", 1}, {"cost_micro_usd", 0}}, response);
        store->record_tool_feedback(call.attempt_id, b, response, result);
        return call.attempt_id;
    }
};
// Deterministic, test-only interleaving on a separate read-only connection.
// SQLite's documented auto-extension entry point is registered before that
// connection opens; the existing writer is never traced or reconfigured.
// A statement trace fires before the tail SELECT reads its first row.
struct TailInterleave {
    inline static TailInterleave* active{};
    std::function<void()> append;
    std::exception_ptr failure;
    bool fired{};
    static int trace(unsigned event, void* context, void* statement, void*) noexcept {
        auto& self = *static_cast<TailInterleave*>(context);
        const char* query = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
        if (event == SQLITE_TRACE_STMT && !self.fired && query != nullptr &&
            std::string_view(query).starts_with("SELECT rowid AS sequence,attempt_id,call_id,frozen_binding,status,accepted,response ")) {
            self.fired = true;
            try { self.append(); } catch (...) { self.failure = std::current_exception(); }
        }
        return 0;
    }
    static int install(sqlite3* db, char**, const sqlite3_api_routines*) noexcept {
        return active == nullptr ? SQLITE_OK : sqlite3_trace_v2(db, SQLITE_TRACE_STMT, trace, active);
    }
    explicit TailInterleave(std::function<void()> action) : append(std::move(action)) {
        CHECK(active == nullptr);
        active = this;
        if (sqlite3_auto_extension(reinterpret_cast<void(*)()>(install)) != SQLITE_OK) {
            active = nullptr;
            throw std::runtime_error("cannot install test SQLite trace");
        }
    }
    void stop_installing() { sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(install)); }
    ~TailInterleave() { stop_installing(); active = nullptr; }
    TailInterleave(const TailInterleave&) = delete;
    TailInterleave& operator=(const TailInterleave&) = delete;
};
void tail_capture_excludes_a_later_insert() {
    Fixture f;
    const auto actor = f.actor();
    const auto original = f.store->attempt_history_tail(actor, 4);
    std::string appended_id;
    TailInterleave interleave([&] {
        auto fake = f.fake();
        const auto call = f.runner->reserve_call(fake, f.request, Role::Worker, f.task);
        appended_id = call.request.attempt_id;
        f.runner->accept_response(call, fake.complete(call.request));
    });
    // Keep interleave alive until the traced reader has been destroyed.
    Store reader(f.db, read_runtime_file(source / "db/schema.sql"),
        read_runtime_file(source / "db/migrations/004_s1_store.sql"), StoreOpenMode::ReadOnly,
        read_runtime_file(source / "db/migrations/005_worker_tools.sql"),
        read_runtime_file(source / "db/migrations/006_event_runtime.sql"),
        read_runtime_file(source / "db/migrations/007_workflow_runtime.sql"),
        read_runtime_file(source / "db/migrations/008_acceptance_runtime.sql"),
        read_runtime_file(source / "db/migrations/009_owned_cleanup.sql"));
    interleave.stop_installing();
    const auto captured = reader.attempt_history_tail(actor, 4);
    if (interleave.failure) std::rethrow_exception(interleave.failure);
    CHECK(interleave.fired && !appended_id.empty());
    CHECK(captured.at("through_sequence") == original.at("through_sequence"));
    CHECK(captured.at("total_count") == original.at("total_count"));
    CHECK(captured.at("items") == original.at("items"));
    CHECK(captured.at("omitted_count") == 0);
    // A fresh capture sees the append; the old capture remains internally
    // consistent, and its complete small turn still compiles without truncation.
    CHECK(worker_history_messages(captured, policy).size() == 3);
    const auto next = reader.attempt_history_tail(actor, 4);
    CHECK(next.at("total_count") == 2);
    CHECK(text(next.at("items").as_array().back(), "attempt_id") == appended_id);
    CHECK(num(next, "through_sequence") > num(captured, "through_sequence"));
}
void inconsistent_window_membership_is_rejected() {
    Fixture f;
    auto tail = f.store->attempt_history_tail(f.actor(), 4);
    CHECK(tail.at("items").as_array().size() == 1);
    tail.as_object()["total_count"] = 0;
    tail.as_object()["omitted_count"] = -1;
    expect(ErrorCode::InvalidInput, [&] { (void)worker_history_messages(tail, policy); });
}
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

void readback_freezes_v2_control_declarations_and_continuation_prompt() {
    ReadbackFixture f;
    const auto call = f.runner->reserve_call(worker_note("probe v2 declarations"),
        f.request, Role::Worker, f.task);
    const auto& prompt = call.request.context.prompt();
    CHECK(prompt.find("wls-history-control-v2") != std::string::npos);
    CHECK(prompt.find("max_tokens") != std::string::npos);
    CHECK(prompt.find("wls-history-control-v1") == std::string::npos);
    CHECK(prompt.find("decode base64") == std::string::npos);
    const auto contract = canonical_json(parse_json_strict(compiled_task_contract(call.request.context)));
    CHECK(contract.find("utf8-token-v1") != std::string::npos);
    // The existing v1 byte/base64 window keeps its identifiers and notices.
    Fixture legacy;
    const auto legacy_call = legacy.runner->reserve_call(legacy.fake(), legacy.request, Role::Worker, legacy.task);
    const auto& legacy_prompt = legacy_call.request.context.prompt();
    CHECK(legacy_prompt.find("wls-history-control-v1") != std::string::npos);
    CHECK(legacy_prompt.find("decode base64") != std::string::npos);
}

void v2_history_read_returns_host_verified_utf8_pages() {
    ReadbackFixture f;
    // The section carries deterministic 2/3/4-byte codepoints, quotes,
    // backslash, newline and an embedded NUL (canonical JSON escapes it as
    // \u0000, so reassembly must restore the escape, not a raw zero byte).
    std::string payload = "证据";
    payload += std::string(4000, 'a');
    payload += "😀";
    payload.append("NUL\0end", 7);
    const Json result = Object{{"text", "中é文原文😀 \"quoted\" \\slash\nline"},
        {"payload", payload}};
    const auto id = f.add_result(result);
    const auto hash = text(f.store->tool_feedback(id), "record_sha256");
    const auto original = canonical_json(f.store->tool_feedback(id).at("result"));
    std::string reassembled; std::int64_t offset = 0;
    while (true) {
        const auto model = v2_control("history_read", v2_read_arguments(id, hash, offset, 512));
        const auto call = f.runner->reserve_call(model, f.request, Role::Worker, f.task);
        const auto page = f.runner->accept_response(call, model.complete(call.request));
        CHECK(text(page, "format") == "wls-history-text-v1");
        CHECK(page.at("host_verified") == true);
        CHECK(text(page, "encoding") == "utf8-canonical-json");
        CHECK(text(page, "record_id") == id);
        CHECK(text(page, "record_sha256") == hash);
        CHECK(text(page, "section") == "result");
        CHECK(text(page, "section_sha256") == sha256_hex(original));
        CHECK(num(page, "size_bytes") == static_cast<std::int64_t>(original.size()));
        CHECK(num(page, "offset") == offset);
        const auto content = text(page, "content");
        CHECK(num(page, "bytes_returned") == static_cast<std::int64_t>(content.size()));
        reassembled += content;
        if (page.at("eof").as_bool()) { CHECK(page.at("next_offset").is_null()); break; }
        CHECK(page.at("next_offset").is_int64());
        CHECK(offset + num(page, "bytes_returned") == num(page, "next_offset"));
        offset = num(page, "next_offset");
        CHECK(offset == static_cast<std::int64_t>(reassembled.size()));
        CHECK(offset <= static_cast<std::int64_t>(original.size()));
    }
    CHECK(reassembled == original);
    const auto end_model = v2_control("history_read", v2_read_arguments(id, hash,
        static_cast<std::int64_t>(original.size()), 64));
    const auto end_call = f.runner->reserve_call(end_model, f.request, Role::Worker, f.task);
    const auto end_response = end_model.complete(end_call.request);
    const auto end = f.runner->accept_response(end_call, end_response);
    CHECK(end.at("eof").as_bool() && text(end, "content").empty());
    // An exact replay of the same captured response is not a second read or
    // charge: the durable receipt is returned unchanged.
    const auto attempts_before_replay = num(f.store->budget(f.request), "attempts");
    CHECK(f.runner->accept_response(end_call, end_response) == end);
    CHECK(num(f.store->budget(f.request), "attempts") == attempts_before_replay);
    // Deterministic mid-codepoint offsets: one probe strictly inside each
    // 2-, 3- and 4-byte sequence of the canonical original, plus past-end,
    // negative and INT64_MAX overflow.
    const auto two_byte = static_cast<std::int64_t>(original.find("é"));
    const auto three_byte = static_cast<std::int64_t>(original.find("中"));
    const auto four_byte = static_cast<std::int64_t>(original.find("😀"));
    CHECK(two_byte > 0 && three_byte > 0 && four_byte > 0);
    for (const auto bad : {two_byte + 1,
                           three_byte + 1, three_byte + 2,
                           four_byte + 1, four_byte + 2, four_byte + 3,
                           static_cast<std::int64_t>(original.size()) + 1,
                           std::int64_t(-1), INT64_MAX}) {
        const auto model = v2_control("history_read", v2_read_arguments(id, hash, bad, 64));
        const auto call = f.runner->reserve_call(model, f.request, Role::Worker, f.task);
        expect(ErrorCode::InvalidInput, [&] {
            (void)f.runner->accept_response(call, model.complete(call.request)); });
    }
    // A well-formed but mismatched record hash, and another record's hash,
    // are both rejected while the immutable original stays unchanged.
    const auto other_id = f.add_result(Object{{"text", "second original"}});
    const auto other_hash = text(f.store->tool_feedback(other_id), "record_sha256");
    for (const auto& wrong : {std::string(64, '0'), other_hash}) {
        const auto model = v2_control("history_read", v2_read_arguments(id, wrong, 0, 64));
        const auto call = f.runner->reserve_call(model, f.request, Role::Worker, f.task);
        bool rejected = false;
        try { (void)f.runner->accept_response(call, model.complete(call.request)); }
        catch (const Error& error) {
            rejected = error.code() == ErrorCode::InvalidInput || error.code() == ErrorCode::Conflict;
        }
        CHECK(rejected);
    }
    CHECK(canonical_json(f.store->tool_feedback(id).at("result")) == original);
}

void sixty_kb_originals_close_within_three_v2_reads_across_reopen() {
    for (int variant = 0; variant < 2; ++variant) {
        ReadbackFixture f;
        std::string body;
        for (int index = 0; index < 5120; ++index) body += "历史证据";
        Json result;
        if (variant == 0) {
            result = Object{{"text", body + "结尾😀"}, {"kind", "canonical-utf8"}};
        } else {
            // A genuine legacy double-field original: the base64 twin is the
            // strict encoding of the very same content bytes, never filler.
            std::string content = body.substr(0, 26568);
            content += "证";
            CHECK(content.size() == 26571);
            result = Object{{"content", content},
                {"content_base64", base64_bytes(content)}, {"kind", "legacy-double-field"}};
        }
        const auto id = f.add_result(result);
        const auto hash = text(f.store->tool_feedback(id), "record_sha256");
        const auto original = canonical_json(f.store->tool_feedback(id).at("result"));
        CHECK(original.size() >= 60000 && original.size() <= 65536);
        if (variant == 1) {
            const auto legacy = parse_json_strict(original);
            CHECK(base64_text(text(legacy, "content_base64")) == text(legacy, "content"));
        }
        std::int64_t offset = 0; int reads = 0; bool reopened = false;
        std::string reassembled; Json previous_page; std::string section_hash;
        while (true) {
            const auto model = v2_control("history_read", v2_read_arguments(id, hash, offset, 24576));
            const auto call = f.runner->reserve_call(model, f.request, Role::Worker, f.task);
            // The complete previous page result enters this immediately
            // following captured input as its structured tool-result message.
            if (!previous_page.is_null())
                require_captured_tool_result(call.request.context, previous_page);
            const auto page = f.runner->accept_response(call, model.complete(call.request));
            ++reads;
            CHECK(reads <= 3);
            CHECK(num(page, "offset") == offset);
            if (section_hash.empty()) section_hash = text(page, "section_sha256");
            CHECK(text(page, "section_sha256") == section_hash);
            const auto content = text(page, "content");
            CHECK(num(page, "bytes_returned") == static_cast<std::int64_t>(content.size()));
            reassembled += content;
            previous_page = page;
            if (page.at("eof").as_bool()) {
                CHECK(page.at("next_offset").is_null());
                CHECK(static_cast<std::int64_t>(reassembled.size()) == num(page, "size_bytes"));
                break;
            }
            CHECK(page.at("next_offset").is_int64());
            CHECK(offset + num(page, "bytes_returned") == num(page, "next_offset"));
            CHECK(num(page, "next_offset") == static_cast<std::int64_t>(reassembled.size()));
            offset = num(page, "next_offset");
            if (reads == 2) { f.reopen(); reopened = true; }
        }
        // The closure crossed a real close/reopen and kept reading afterwards.
        CHECK(reopened);
        CHECK(reassembled == original);
        CHECK(sha256_hex(reassembled) == section_hash);
        // After the final page the next captured call is a normal tool call,
        // not a fourth history_read, and it carries the EOF page result whole.
        const auto after = worker_note("normal call after the final page");
        const auto note_call = f.runner->reserve_call(after, f.request, Role::Worker, f.task);
        require_captured_tool_result(note_call.request.context, previous_page);
        static_cast<void>(f.runner->accept_response(note_call, after.complete(note_call.request)));
    }
}

// 22000 x "证据" is a 132000-byte canonical section (each codepoint is three
// UTF-8 bytes), so the window keeps it as a hash-pinned reference rather than
// inline content and only a returned page adds request bytes.
std::string budget_section() {
    std::string body;
    for (int index = 0; index < 22000; ++index) body += "证据";
    return body;
}

// Every fixture below performs the identical call sequence (initial run,
// result turn, settled probe turn) before its measured read, so the envelope
// measured in one fixture is the envelope the budgeted fixture will issue.
std::int64_t settled_probe_envelope(ReadbackFixture& f) {
    const auto model = worker_note("measure read-time envelope");
    const auto call = f.runner->reserve_call(model, f.request, Role::Worker, f.task);
    const auto envelope = static_cast<std::int64_t>(call.request.context.prompt().size());
    static_cast<void>(f.runner->accept_response(call, model.complete(call.request)));
    return envelope;
}

void readback_budget_mandates_a_successful_smaller_page() {
    const Json result = Object{{"text", budget_section()}};
    ReadbackFixture ample(4000000, 24576, 8192);
    const auto wide_id = ample.add_result(result);
    const auto wide_hash = text(ample.store->tool_feedback(wide_id), "record_sha256");
    static_cast<void>(settled_probe_envelope(ample));
    const auto wide_model = v2_control("history_read", v2_read_arguments(wide_id, wide_hash, 0, 24576));
    const auto wide_call = ample.runner->reserve_call(wide_model, ample.request, Role::Worker, ample.task);
    // The read-time envelope is measured from the actual captured structured
    // context (compile_structured_context's canonical logical JSON), never a
    // fabricated constant or a smaller stand-in input.
    const auto envelope = static_cast<std::int64_t>(wide_call.request.context.prompt().size());
    const auto wide = ample.runner->accept_response(wide_call, wide_model.complete(wide_call.request));
    const auto wide_bytes = num(wide, "bytes_returned");
    CHECK(wide_bytes > 24000 && wide_bytes <= 24576);
    // Half a page of headroom must produce a strictly smaller real page; a
    // shrink-or-Quota shortcut cannot satisfy this mandatory success row.
    ReadbackFixture tight(envelope + wide_bytes / 2, 24576, 8192);
    const auto id = tight.add_result(result);
    const auto hash = text(tight.store->tool_feedback(id), "record_sha256");
    static_cast<void>(settled_probe_envelope(tight));
    const auto model = v2_control("history_read", v2_read_arguments(id, hash, 0, 24576));
    const auto call = tight.runner->reserve_call(model, tight.request, Role::Worker, tight.task);
    const auto narrow = tight.runner->accept_response(call, model.complete(call.request));
    CHECK(text(narrow, "format") == "wls-history-text-v1");
    CHECK(num(narrow, "offset") == 0);
    CHECK(num(narrow, "bytes_returned") > 0);
    CHECK(num(narrow, "bytes_returned") < wide_bytes);
    CHECK(!narrow.at("eof").as_bool());
}

void readback_no_progress_quota_adds_no_attempt_or_charge() {
    const Json result = Object{{"text", budget_section()}};
    ReadbackFixture ample(4000000, 24576, 8192);
    const auto wide_id = ample.add_result(result);
    const auto wide_hash = text(ample.store->tool_feedback(wide_id), "record_sha256");
    static_cast<void>(settled_probe_envelope(ample));
    const auto wide_model = v2_control("history_read", v2_read_arguments(wide_id, wide_hash, 0, 24576));
    const auto wide_call = ample.runner->reserve_call(wide_model, ample.request, Role::Worker, ample.task);
    const auto envelope = static_cast<std::int64_t>(wide_call.request.context.prompt().size());
    static_cast<void>(ample.runner->accept_response(wide_call, wide_model.complete(wide_call.request)));
    // Two bytes of headroom beyond the complete fixed envelope cannot carry
    // the page framing plus even one three-byte codepoint, so this Quota is
    // deterministic, and the failure must add nothing: no attempt, no charge,
    // no page and no next_offset from the no-progress stop.
    ReadbackFixture starved(envelope + 2, 24576, 8192);
    const auto id = starved.add_result(result);
    const auto hash = text(starved.store->tool_feedback(id), "record_sha256");
    static_cast<void>(settled_probe_envelope(starved));
    const auto model = v2_control("history_read", v2_read_arguments(id, hash, 0, 24576));
    const auto before = starved.store->budget(starved.request);
    bool quota_at_reservation = false;
    std::optional<CapturedModelCall> call;
    try {
        call = starved.runner->reserve_call(model, starved.request, Role::Worker, starved.task);
    } catch (const Error& error) {
        CHECK(error.code() == ErrorCode::Quota);
        quota_at_reservation = true;
    }
    if (!quota_at_reservation)
        expect(ErrorCode::Quota, [&] {
            (void)starved.runner->accept_response(*call, model.complete(call->request)); });
    CHECK(starved.store->budget(starved.request) == before);
}

void v2_read_guards_and_fences() {
    ReadbackFixture f;
    const Json result = Object{{"text", "guarded original"}};
    const auto id = f.add_result(result);
    const auto hash = text(f.store->tool_feedback(id), "record_sha256");
    const auto arguments = v2_read_arguments(id, hash, 0, 64);
    // A Worker wait is a dispatch gate: no new v2 read may be reserved while
    // the worker is waiting, and the wait survives unchanged.
    const auto wait = f.store->wait_worker(f.actor(), f.store->binding(f.task),
        num(f.store->task(f.task), "state_version"), "explicit wait during readback", new_id("wait"));
    bool wait_blocked = false;
    try {
        (void)f.runner->reserve_call(v2_control("history_read", arguments), f.request, Role::Worker, f.task);
    } catch (const Error& error) {
        wait_blocked = error.code() == ErrorCode::NotReady || error.code() == ErrorCode::UnknownOutcome;
    }
    CHECK(wait_blocked);
    CHECK(f.store->worker_wait(f.task) == wait);
    // Denied by name before reservation: no attempt, no charge.
    sql(f.db, "UPDATE runtime_policy SET denied_tools='[\"history_read\"]',state_version=state_version+1");
    const auto before = f.store->budget(f.request);
    expect(ErrorCode::Unauthorized, [&] {
        (void)f.runner->reserve_call(v2_control("history_read", arguments), f.request, Role::Worker, f.task); });
    CHECK(f.store->budget(f.request) == before);
    // Read-then-revoke: a captured v2 read is fenced at settlement.
    sql(f.db, "UPDATE runtime_policy SET denied_tools='[]',state_version=state_version+1");
    auto model = v2_control("history_read", arguments);
    const auto call = f.runner->reserve_call(model, f.request, Role::Worker, f.task);
    sql(f.db, "UPDATE runtime_policy SET denied_tools='[\"history_read\"]',state_version=state_version+1");
    expect(ErrorCode::Unauthorized, [&] {
        (void)f.runner->accept_response(call, model.complete(call.request)); });
    CHECK(f.store->tool_feedback(call.request.attempt_id).at("result").at("runtime_error") == true);
    // AMEND after capture fences the read as stale original evidence.
    sql(f.db, "UPDATE runtime_policy SET denied_tools='[]',state_version=state_version+1");
    auto stale_model = v2_control("history_read", arguments);
    const auto stale = f.runner->reserve_call(stale_model, f.request, Role::Worker, f.task);
    f.store->amend(f.store->host_actor(Role::Root, f.request), num(f.store->request(f.request), "state_version"),
        read_runtime_file(source / "tests/fixtures/s1_amendment.txt"),
        parse_json_strict(read_runtime_file(source / "tests/fixtures/s1_amended_constraints.json")), "amend-v2-read");
    expect(ErrorCode::Stale, [&] {
        (void)f.runner->accept_response(stale, stale_model.complete(stale.request)); });
    CHECK(f.store->tool_feedback(stale.request.attempt_id).at("result").at("runtime_error") == true);
    // Corrupted persisted feedback fails closed on the v2 read path too.
    sqlite3* db{}; CHECK(sqlite3_open(f.db.c_str(), &db) == SQLITE_OK); sqlite3_stmt* stmt{};
    CHECK(sqlite3_prepare_v2(db, "SELECT name,sql FROM sqlite_master WHERE type='trigger' AND tbl_name='model_tool_feedback'", -1, &stmt, nullptr) == SQLITE_OK);
    std::vector<std::pair<std::string, std::string>> triggers;
    while (sqlite3_step(stmt) == SQLITE_ROW) triggers.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)), reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    sqlite3_finalize(stmt); sqlite3_close(db); CHECK(!triggers.empty());
    for (const auto& [name, definition] : triggers) { (void)definition; sql(f.db, "DROP TRIGGER " + name); }
    sql(f.db, "UPDATE model_tool_feedback SET result='{}' WHERE attempt_id='" + id + "'");
    for (const auto& [name, definition] : triggers) { (void)name; sql(f.db, definition); }
    expect(ErrorCode::Corrupt, [&] {
        (void)execute_history_control(*f.store, f.actor(), f.store->binding(f.task), "history_read", arguments); });
}
} // namespace
int main() {return test::run({
    {"tail capture excludes an interleaved later insert",tail_capture_excludes_a_later_insert},
    {"negative omission count is rejected",inconsistent_window_membership_is_rejected},
    {"missing frozen window preserves legacy",empty_policy_defaults},
    {"scoped small original turn remains paired",tail_is_original_and_small_turn_is_paired},
    {"large whole turn becomes explicit original reference",large_turn_is_explicit_reference},
    {"readback exact replay preserves receipt and budget",exact_control_replay_is_not_a_second_read_or_charge},
    {"AMEND fences in-flight historical reads",changed_semantics_fence_captured_reads},
    {"revoked readback refuses window before reservation",current_policy_refuses_window_before_reservation},
    {"UNKNOWN is pinned and still blocks reservation",waiting_and_unknown_survive_window_projection},
    {"missing old attempt remains pinned outside tail",missing_attempt_outside_tail_remains_pinned},
    {"policy revocation fences captured read",policy_revocation_fences_an_already_captured_read},
    {"recent corruption remains fail closed",original_corruption_is_not_hidden_by_windowing},
    {"readback freezes v2 control declarations and continuation prompt",readback_freezes_v2_control_declarations_and_continuation_prompt},
    {"v2 history_read returns host-verified utf8 pages",v2_history_read_returns_host_verified_utf8_pages},
    {"sixty KB originals close within three v2 reads across reopen",sixty_kb_originals_close_within_three_v2_reads_across_reopen},
    {"readback budget mandates a successful smaller page",readback_budget_mandates_a_successful_smaller_page},
    {"readback no-progress Quota adds no attempt or charge",readback_no_progress_quota_adds_no_attempt_or_charge},
    {"v2 read wait denial revocation AMEND and corruption fences",v2_read_guards_and_fences}
});}
