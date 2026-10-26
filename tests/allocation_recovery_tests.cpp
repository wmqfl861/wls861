#include "wls/cleanup.hpp"
#include "wls/cleanup_runtime.hpp"
#include "wls/config_loader.hpp"
#include "wls/delivery.hpp"
#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "capacity_test_support.hpp"
#include "test_support.hpp"

#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

// Faults apply only to one disposable fixture in this test executable. In
// particular, SQLite writes/fsyncs and unrelated descriptors are never hooked.
// These real process interruptions do not claim physical power-loss testing.
namespace allocation_fault {
thread_local std::string partial_marker;
thread_local bool kill_before_parent_sync = false;
thread_local bool kill_after_parent_sync = false;
thread_local dev_t parent_device = 0;
thread_local ino_t parent_inode = 0;
thread_local std::string final_leaf;
thread_local std::string stage_leaf;
}
extern "C" ssize_t write(int fd, const void* bytes, std::size_t length) {
    if (!allocation_fault::partial_marker.empty()) {
        std::array<char, 4096> actual{};
        const auto descriptor = "/proc/self/fd/" + std::to_string(fd);
        const auto size = ::readlink(descriptor.c_str(), actual.data(), actual.size());
        if (size > 0 && static_cast<std::size_t>(size) < actual.size() &&
            allocation_fault::partial_marker == std::string(actual.data(), static_cast<std::size_t>(size))) {
            const auto wrote = static_cast<ssize_t>(::syscall(SYS_write, fd, bytes, std::min(length, std::size_t{7})));
            if (wrote > 0) (void)::kill(::getpid(), SIGKILL);
            return wrote;
        }
    }
    return static_cast<ssize_t>(::syscall(SYS_write, fd, bytes, length));
}
extern "C" int fsync(int fd) {
    struct stat parent{}, final{}, stage{};
    const bool watched = (allocation_fault::kill_before_parent_sync || allocation_fault::kill_after_parent_sync) &&
        ::fstat(fd, &parent) == 0 && parent.st_dev == allocation_fault::parent_device && parent.st_ino == allocation_fault::parent_inode &&
        ::fstatat(fd, allocation_fault::final_leaf.c_str(), &final, AT_SYMLINK_NOFOLLOW) == 0 &&
        ::fstatat(fd, allocation_fault::stage_leaf.c_str(), &stage, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT;
    if (watched && allocation_fault::kill_before_parent_sync) (void)::kill(::getpid(), SIGKILL);
    const auto result = static_cast<int>(::syscall(SYS_fsync, fd));
    if (watched && result == 0 && allocation_fault::kill_after_parent_sync) (void)::kill(::getpid(), SIGKILL);
    return result;
}

namespace {
using namespace wls;
using namespace wls::s1;
using O = boost::json::object;
using A = boost::json::array;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::string base_sql, store_sql, tools_sql, events_sql, workflow_sql, acceptance_sql, cleanup_sql, allocation_sql;
std::unique_ptr<SchemaRegistry> schemas;
std::string text(const Json& v, std::string_view key) { return std::string(v.at(key).as_string()); }
std::int64_t num(const Json& v, std::string_view key) { return v.at(key).to_number<std::int64_t>(); }
std::string quote(std::string_view value) {
    std::string result = "'";
    for (const auto c : value) { result += c; if (c == '\'') result += c; }
    return result + "'";
}
void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); CHECK(out.good());
}
std::string read_file(const std::filesystem::path& path) { return read_runtime_file(path); }
int sql_result(const std::filesystem::path& path, const std::string& statement) {
    sqlite3* db = nullptr; CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    const auto rc = sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db); return rc;
}
void sql(const std::filesystem::path& path, const std::string& statement) { CHECK(sql_result(path, statement) == SQLITE_OK); }
std::int64_t scalar(const std::filesystem::path& path, const std::string& query) {
    sqlite3* db = nullptr; CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr; CHECK(sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW); const auto result = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt); sqlite3_close(db); return result;
}
std::vector<std::string> rows(const std::filesystem::path& path, const std::string& query) {
    sqlite3* db = nullptr; CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr; CHECK(sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    std::vector<std::string> result; int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        std::string row;
        for (int c = 0; c < sqlite3_column_count(stmt); ++c) {
            const auto type = sqlite3_column_type(stmt, c);
            const auto size = sqlite3_column_bytes(stmt, c);
            row += std::to_string(type) + ":" + std::to_string(size) + ":";
            if (size > 0) row.append(static_cast<const char*>(sqlite3_column_blob(stmt, c)), static_cast<std::size_t>(size));
            row += "|";
        }
        result.push_back(std::move(row));
    }
    sqlite3_finalize(stmt); sqlite3_close(db); CHECK(rc == SQLITE_DONE);
    std::sort(result.begin(), result.end()); return result;
}
void rejects(ErrorCode code, const std::function<void()>& action) {
    try { action(); }
    catch (const Error& error) {
        if (error.code() != code) throw std::runtime_error(std::string("wrong error: ") + error.what());
        return;
    }
    throw std::runtime_error("expected rejection was not thrown");
}
struct Node { std::string request, task, team; };
struct Fixture {
    test::TempDir dir;
    std::filesystem::path db{dir.path / "state.sqlite"}, parent{dir.path / "owned"};
    std::unique_ptr<Store> store;
    Json frozen;
    Node node;
    bool recovery;
    Json brief = O{{"goal", "retain exact allocation facts"}, {"constraint_refs", A{"c1"}},
                   {"constraints_verbatim", A{"retain exact allocation facts"}}};
    explicit Fixture(bool upgraded = true, bool small = false, bool legacy_capacity = false) : recovery(upgraded) {
        std::filesystem::create_directory(parent);
        auto system = load_config_catalog(source / "config/examples/system.yaml", source / "config/examples/functions", source / "prompts").freeze("general").json().at("system");
        system.at("cleanup").as_object()["enabled"] = true;
        if (small || legacy_capacity) {
            auto& limits = system.at("limits").as_object();
            limits["max_teams_per_request"] = 1;
            limits["worker_max_tool_calls_per_order"] = 4;
            limits["manager_max_tool_calls_per_wake"] = 1;
            limits["team_internal_rework_max"] = 0;
            if (legacy_capacity) limits.erase("store_capacity");
            else {
                limits.at("store_capacity").as_object()["business_events"] = 64;
                limits.at("store_capacity").as_object()["control_events"] = 64;
                limits.at("store_capacity").as_object()["operations"] = 4;
            }
        }
        // Freeze matching dependency bytes; no edited parsed snapshot retains
        // a fingerprint for the old configuration file.
        write_file(dir.path / "system.json", canonical_json(system));
        frozen = load_config_catalog(dir.path / "system.json", source / "config/examples/functions", source / "prompts").freeze("general").json();
        store = open(); node = create("request");
    }
    std::unique_ptr<Store> open(StoreRecoveryScope scope = {}, StoreOpenMode mode = StoreOpenMode::Takeover,
                                std::optional<bool> upgrade = {}) const {
        return std::make_unique<Store>(db, base_sql, store_sql, mode, tools_sql, events_sql, workflow_sql,
            acceptance_sql, cleanup_sql, std::move(scope), upgrade.value_or(recovery) ? allocation_sql : "");
    }
    void reopen(StoreRecoveryScope scope = {}) { store.reset(); store = open(std::move(scope)); }
    StoreActor actor(const Node& n, Role role = Role::Root) const {
        return store->host_actor(role, n.request, role == Role::TeamLead || role == Role::Worker ? n.team : "");
    }
    StoreActor root() const { return actor(node); }
    AdmissionInput input(const std::string& key) const {
        AdmissionInput in; in.tenant_id="fixture"; in.project_id="allocation"; in.root_id="root"; in.function_id="general";
        in.idempotency_key=key; in.original_text="retain exact allocation facts"; in.config_snapshot=frozen;
        in.constraint_catalog=A{O{{"constraint_id","c1"},{"text",in.original_text},{"source_revision",1}}};
        in.tool_allowlist={"note","fs_write","ack_change"}; return in;
    }
    Node create(const std::string& key) {
        Node n; n.request=text(store->admit(input(key)),"request_id");
        store->publish_plan(actor(n,Role::Dispatcher),num(store->request(n.request),"state_version"),1,brief,new_id("plan"));
        const auto task=store->create_task(actor(n,Role::Dispatcher),num(store->request(n.request),"state_version"),1,1,"node",brief,new_id("task"));
        n.task=text(task,"task_id"); n.team=text(task,"team_id");
        store->issue_instruction(actor(n,Role::TeamLead),store->binding(n.task),num(store->task(n.task),"state_version"),brief,new_id("instruction"));
        store->acknowledge(actor(n,Role::Worker),store->binding(n.task),num(store->task(n.task),"state_version"),new_id("ack"));
        return n;
    }
    Json prepare(const std::string& key = "allocation") {
        const auto identity = recovery ? allocation_parent_identity(parent) : Workspace(parent,WorkspaceAccess::ReadOnly).root_identity();
        return store->prepare_owned_workspace(root(),node.task,identity,key);
    }
    Json current(const Json& resource) const {
        for (const auto& item : store->owned_workspaces(node.request)) if (item.at("resource_id") == resource.at("resource_id")) return item;
        throw std::runtime_error("missing allocation resource");
    }
    Json stage(Json resource) {
        const auto receipt=stage_owned_workspace(resource.at("intent"));
        return store->record_owned_allocation_receipt(root(),text(resource,"resource_id"),num(resource,"state_version"),receipt);
    }
    Json complete(Json resource) {
        resource=store->authorize_owned_allocation(root(),text(resource,"resource_id"),num(resource,"state_version"));
        const auto identity=publish_owned_workspace(resource);
        return store->register_owned_workspace(root(),text(resource,"resource_id"),identity,num(resource,"state_version"));
    }
    Json abandon(const Json& resource, const std::string& key="abandon", const std::string& reason="retain the interrupted allocation for inspection") {
        return store->abandon_owned_workspace(root(),text(resource,"resource_id"),num(resource,"state_version"),key,reason);
    }
    std::filesystem::path path(const Json& resource, bool staging = false) const {
        return parent / text(resource.at("intent"), staging ? "stage_leaf" : "leaf");
    }
    void kill_child(const std::function<void()>& operation) {
        store.reset(); const auto pid=::fork(); CHECK(pid>=0);
        if (pid==0) {
            try { store=open(StoreRecoveryScope::request(node.request)); operation(); (void)::kill(::getpid(),SIGKILL); }
            catch (const std::exception& error) { (void)::syscall(SYS_write,STDERR_FILENO,error.what(),std::strlen(error.what())); ::_exit(31); }
            ::_exit(32);
        }
        int status=0; CHECK(::waitpid(pid,&status,0)==pid); CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
        store=open(StoreRecoveryScope::request(node.request));
    }
};

void ordinary_allocation_exact_replay() {
    Fixture f;
    const auto resource=allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation");
    CHECK(resource.at("status")=="OWNED" && !resource.at("allocation_receipt").is_null());
    CHECK(!std::filesystem::exists(f.path(resource,true)) && std::filesystem::is_empty(f.path(resource)/"work"));
    const auto identity=resource.at("identity");
    for (const auto& path:{f.path(resource),f.path(resource)/"work"}) {
        struct stat info{}; CHECK(::stat(path.c_str(),&info)==0); CHECK((info.st_mode&0777)==0700);
    }
    CHECK(inspect_owned_workspace(resource)==identity);
    const auto before=rows(f.db,"SELECT * FROM owned_workspaces");
    CHECK(allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation")==resource);
    CHECK(rows(f.db,"SELECT * FROM owned_workspaces")==before);
    rejects(ErrorCode::Conflict,[&]{(void)f.prepare("another-key");});
}
void partial_marker_kill_requires_explicit_abandonment() {
    Fixture f; const auto resource=f.prepare(); const auto marker=f.path(resource,true)/"owner.json";
    f.kill_child([&]{allocation_fault::partial_marker=marker.string();(void)stage_owned_workspace(resource.at("intent"));});
    CHECK(std::filesystem::is_directory(f.path(resource,true)) && read_file(marker).size()==7);
    CHECK(f.current(resource).at("allocation_receipt").is_null());
    rejects(ErrorCode::Conflict,[&]{(void)allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation");});
    const auto retained=read_file(marker); const auto abandoned=f.abandon(f.current(resource));
    CHECK(abandoned.at("status")=="ABANDONED" && read_file(marker)==retained);
    CHECK(!std::filesystem::exists(f.path(resource)) && abandoned.at("proof").is_null());
    const auto next=allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"replacement");
    CHECK(next.at("resource_id")!=resource.at("resource_id") && next.at("status")=="OWNED");
    CHECK(read_file(marker)==retained && f.store->owned_workspaces(f.node.request).size()==2);
    CHECK(f.store->active_owned_workspaces(f.node.request).size()==1);
}
void killed_complete_stage_without_receipt_is_not_adopted() {
    Fixture f; const auto resource=f.prepare();
    f.kill_child([&]{(void)stage_owned_workspace(resource.at("intent"));});
    CHECK(read_file(f.path(resource,true)/"owner.json")==canonical_json(resource.at("intent").at("marker")));
    CHECK(std::filesystem::is_empty(f.path(resource,true)/"work"));
    rejects(ErrorCode::Conflict,[&]{(void)allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation");});
    CHECK(f.current(resource).at("status")=="PREPARED" && f.current(resource).at("allocation_receipt").is_null());
}
void killed_after_receipt_recovers_exact_generation() {
    Fixture f; const auto resource=f.prepare();
    f.kill_child([&]{(void)f.stage(resource);});
    const auto saved=f.current(resource); const auto receipt=saved.at("allocation_receipt");
    CHECK(!receipt.is_null() && std::filesystem::exists(f.path(resource,true)));
    const auto done=allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation");
    CHECK(done.at("status")=="OWNED" && done.at("allocation_receipt")==receipt);
    CHECK(done.at("identity").at("envelope_identity").at("inode")==receipt.at("stage_identity").at("inode"));
    CHECK(done.at("identity").at("workspace_identity").at("inode")==receipt.at("workspace_identity").at("inode"));
}
void killed_during_publication_is_recoverable() {
    for (const bool after : {false,true}) {
        Fixture f; const auto resource=f.stage(f.prepare()); const auto receipt=resource.at("allocation_receipt");
        f.kill_child([&]{
            struct stat info{}; CHECK(::stat(f.parent.c_str(),&info)==0);
            allocation_fault::parent_device=info.st_dev; allocation_fault::parent_inode=info.st_ino;
            allocation_fault::stage_leaf=text(resource.at("intent"),"stage_leaf"); allocation_fault::final_leaf=text(resource.at("intent"),"leaf");
            allocation_fault::kill_before_parent_sync=!after; allocation_fault::kill_after_parent_sync=after;
            (void)publish_owned_workspace(resource); ::_exit(33); // must die inside the watched real syscall boundary
        });
        CHECK(std::filesystem::exists(f.path(resource)) && !std::filesystem::exists(f.path(resource,true)));
        CHECK(f.current(resource).at("status")=="PREPARED" && f.current(resource).at("allocation_receipt")==receipt);
        CHECK(allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation").at("status")=="OWNED");
    }
}
void killed_after_final_sync_before_registration() {
    Fixture f; const auto resource=f.stage(f.prepare());
    f.kill_child([&]{(void)publish_owned_workspace(resource);});
    const auto stored=f.current(resource); CHECK(stored.at("status")=="PREPARED");
    const auto done=allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation");
    CHECK(done.at("allocation_receipt")==stored.at("allocation_receipt") && done.at("status")=="OWNED");
}
void receipt_write_failure_cannot_publish() {
    Fixture f;
    sql(f.db,"CREATE TRIGGER fail_receipt BEFORE UPDATE ON owned_workspaces WHEN NEW.allocation_receipt IS NOT NULL BEGIN SELECT RAISE(ABORT,'fixture receipt failure'); END;");
    rejects(ErrorCode::Conflict,[&]{(void)allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation");});
    const auto resource=f.store->owned_workspaces(f.node.request).at(0);
    CHECK(resource.at("status")=="PREPARED" && resource.at("allocation_receipt").is_null());
    CHECK(std::filesystem::exists(f.path(resource,true)) && !std::filesystem::exists(f.path(resource)));
    sql(f.db,"DROP TRIGGER fail_receipt;"); f.reopen();
    rejects(ErrorCode::Conflict,[&]{(void)allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation");});
    CHECK(f.abandon(f.current(resource)).at("status")=="ABANDONED");
}
void registration_write_failure_recovers_without_recreation() {
    Fixture f;
    sql(f.db,"CREATE TRIGGER fail_registration BEFORE UPDATE ON owned_workspaces WHEN NEW.status='OWNED' BEGIN SELECT RAISE(ABORT,'fixture registration failure'); END;");
    rejects(ErrorCode::Conflict,[&]{(void)allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation");});
    const auto resource=f.store->owned_workspaces(f.node.request).at(0); const auto receipt=resource.at("allocation_receipt");
    CHECK(!receipt.is_null() && std::filesystem::exists(f.path(resource)) && !std::filesystem::exists(f.path(resource,true)));
    const auto marker=read_file(f.path(resource)/"owner.json");
    sql(f.db,"DROP TRIGGER fail_registration;"); f.reopen();
    const auto done=allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation");
    CHECK(done.at("allocation_receipt")==receipt && done.at("status")=="OWNED");
    CHECK(done.at("identity").at("envelope_identity").at("inode")==receipt.at("stage_identity").at("inode"));
    CHECK(read_file(f.path(resource)/"owner.json")==marker);
}
void unreceipted_external_paths_are_never_adopted() {
    for (const bool stage : {false,true}) {
        Fixture f; const auto resource=f.prepare(); const auto path=f.path(resource,stage);
        std::filesystem::create_directory(path); write_file(path/"keep","external bytes");
        rejects(ErrorCode::Conflict,[&]{(void)stage_owned_workspace(resource.at("intent"));});
        CHECK(read_file(path/"keep")=="external bytes" && f.current(resource).at("allocation_receipt").is_null());
    }
}
void copied_markers_do_not_prove_generation() {
    for (const bool final : {false,true}) {
        Fixture f; const auto resource=f.stage(f.prepare());
        if (final) (void)publish_owned_workspace(resource);
        const auto original=f.path(resource,!final); const auto retained=f.parent/"retained-original";
        std::filesystem::rename(original,retained);
        std::filesystem::create_directory(original); std::filesystem::create_directory(original/"work");
        write_file(original/"owner.json",read_file(retained/"owner.json"));
        rejects(ErrorCode::Conflict,[&]{(void)publish_owned_workspace(resource);});
        CHECK(read_file(original/"owner.json")==read_file(retained/"owner.json"));
        CHECK(f.current(resource).at("status")=="PREPARED" && std::filesystem::is_empty(retained/"work"));
    }
}
void replaced_work_marker_or_parent_and_nonempty_work_refuse() {
    for (int damage=0;damage<4;++damage) {
        Fixture f; const auto resource=f.stage(f.prepare()); const auto stage=f.path(resource,true);
        if(damage==0){std::filesystem::rename(stage/"work",stage/"old-work");std::filesystem::create_directory(stage/"work");std::filesystem::rename(stage/"old-work",f.parent/"retained-work");}
        if(damage==1)write_file(stage/"owner.json","different marker");
        if(damage==2){std::filesystem::rename(f.parent,f.dir.path/"retained-parent");std::filesystem::create_directory(f.parent);}
        if(damage==3)write_file(stage/"work"/"new-file","not empty");
        rejects(ErrorCode::Conflict,[&]{(void)publish_owned_workspace(resource);});
        CHECK(f.current(resource).at("status")=="PREPARED" && !std::filesystem::exists(f.path(resource)));
        if(damage==3)CHECK(read_file(stage/"work"/"new-file")=="not empty");
    }
}
void both_or_neither_receipted_paths_refuse() {
    for (const bool both : {false,true}) {
        Fixture f; const auto resource=f.stage(f.prepare());
        if(both)std::filesystem::create_directory(f.path(resource));
        else std::filesystem::rename(f.path(resource,true),f.parent/"retained-stage");
        rejects(ErrorCode::Conflict,[&]{(void)publish_owned_workspace(resource);});
        CHECK(f.current(resource).at("status")=="PREPARED");
        CHECK(std::filesystem::exists(both?f.path(resource):f.parent/"retained-stage"));
    }
}
void receipt_and_registration_cas_and_originals_are_enforced() {
    Fixture f; const auto resource=f.prepare(); const auto receipt=stage_owned_workspace(resource.at("intent"));
    rejects(ErrorCode::Conflict,[&]{(void)f.store->record_owned_allocation_receipt(f.root(),text(resource,"resource_id"),num(resource,"state_version")+1,receipt);});
    auto forged=receipt; forged.as_object()["marker_sha256"]=std::string(64,'0');
    rejects(ErrorCode::Conflict,[&]{(void)f.store->record_owned_allocation_receipt(f.root(),text(resource,"resource_id"),num(resource,"state_version"),forged);});
    auto saved=f.store->record_owned_allocation_receipt(f.root(),text(resource,"resource_id"),num(resource,"state_version"),receipt);
    CHECK(saved.at("allocation_receipt")==receipt);
    CHECK(sql_result(f.db,"UPDATE owned_workspaces SET allocation_receipt='{}',state_version=state_version+1;")!=SQLITE_OK);
    const auto identity=publish_owned_workspace(saved);
    rejects(ErrorCode::Conflict,[&]{(void)f.store->register_owned_workspace(f.root(),text(saved,"resource_id"),identity,num(saved,"state_version")+1);});
    const auto done=f.store->register_owned_workspace(f.root(),text(saved,"resource_id"),identity,num(saved,"state_version"));
    CHECK(done.at("status")=="OWNED");
    CHECK(sql_result(f.db,"UPDATE owned_workspaces SET state_version=state_version+2;")!=SQLITE_OK);
    CHECK(sql_result(f.db,"UPDATE owned_workspaces SET intent='{}',state_version=state_version+1;")!=SQLITE_OK);
    CHECK(sql_result(f.db,"UPDATE owned_workspaces SET identity='{}',state_version=state_version+1;")!=SQLITE_OK);
}
void abandonment_replay_is_exact_and_irreversible() {
    Fixture f; const auto resource=f.stage(f.prepare()); const auto receipt=resource.at("allocation_receipt");
    const auto result=f.abandon(resource); const auto before=rows(f.db,"SELECT * FROM owned_workspaces");
    f.reopen(); CHECK(f.abandon(resource)==result);
    CHECK(rows(f.db,"SELECT * FROM owned_workspaces")==before);
    rejects(ErrorCode::Conflict,[&]{(void)f.abandon(resource,"abandon","changed reason");});
    rejects(ErrorCode::Conflict,[&]{(void)f.store->abandon_owned_workspace(f.root(),text(resource,"resource_id"),num(resource,"state_version")+1,"abandon","retain the interrupted allocation for inspection");});
    rejects(ErrorCode::NotReady,[&]{(void)f.store->authorize_owned_allocation(f.root(),text(resource,"resource_id"),num(result,"state_version"));});
    rejects(ErrorCode::NotReady,[&]{(void)f.store->record_owned_allocation_receipt(f.root(),text(resource,"resource_id"),1,receipt);});
    rejects(ErrorCode::NotReady,[&]{(void)f.store->register_owned_workspace(f.root(),text(resource,"resource_id"),O{},num(resource,"state_version"));});
    CHECK(sql_result(f.db,"UPDATE owned_workspaces SET status='PREPARED',abandonment=NULL,state_version=state_version+1;")!=SQLITE_OK);
    CHECK(f.current(resource).at("allocation_receipt")==receipt && f.current(resource).at("proof").is_null());
    CHECK(std::filesystem::is_empty(f.path(resource,true)/"work"));
    CHECK(f.prepare().at("resource_id")==resource.at("resource_id"));
    CHECK(f.prepare().at("status")=="ABANDONED");
    CHECK(allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"replacement").at("status")=="OWNED");
    CHECK(f.current(resource).at("status")=="ABANDONED");
}
void abandonment_rollback_and_preflight_have_no_effect() {
    Fixture f; const auto resource=f.prepare(); const auto before=rows(f.db,"SELECT * FROM owned_workspaces");
    rejects(ErrorCode::InvalidInput,[&]{(void)f.abandon(resource,"blank"," \t ");});
    rejects(ErrorCode::Conflict,[&]{(void)f.store->abandon_owned_workspace(f.root(),text(resource,"resource_id"),num(resource,"state_version")+1,"wrong-cas","reason");});
    f.store->validate_abandon_owned_workspace(f.root(),text(resource,"resource_id"),num(resource,"state_version"),"abandon","reason");
    CHECK(rows(f.db,"SELECT * FROM owned_workspaces")==before);
    sql(f.db,"CREATE TRIGGER fail_abandon BEFORE INSERT ON events WHEN NEW.type='allocation_abandoned' BEGIN SELECT RAISE(ABORT,'fixture abandonment failure'); END;");
    rejects(ErrorCode::Conflict,[&]{(void)f.abandon(resource);}); CHECK(rows(f.db,"SELECT * FROM owned_workspaces")==before);
    sql(f.db,"DROP TRIGGER fail_abandon;"); CHECK(f.abandon(resource).at("status")=="ABANDONED");
}
void abandonment_terminal_missing_parent_and_retired_team() {
    Fixture f; const auto resource=f.stage(f.prepare()); const auto kept=f.dir.path/"kept-parent";
    f.store->cancel_request(f.root(),num(f.store->request(f.node.request),"state_version"),"cancel");
    std::filesystem::rename(f.parent,kept); f.reopen(StoreRecoveryScope::request(f.node.request));
    const auto request=f.store->request(f.node.request); const auto task=f.store->task(f.node.task);
    const auto result=f.abandon(f.current(resource)); CHECK(result.at("status")=="ABANDONED");
    CHECK(f.store->request(f.node.request)==request && f.store->task(f.node.task)==task);
    CHECK(request.at("status")=="CANCELLED" && task.at("status")=="CANCELLED");
    CHECK(std::filesystem::exists(kept/text(resource.at("intent"),"stage_leaf")/"owner.json"));
    CHECK(result.at("proof").is_null());
}
void stale_root_foreign_scope_and_nonroot_cannot_abandon() {
    Fixture f; const auto resource=f.prepare(); const auto root=f.root(); const auto other=f.create("sibling"); const auto other_root=f.actor(other);
    rejects(ErrorCode::Unauthorized,[&]{(void)f.store->abandon_owned_workspace(f.actor(f.node,Role::Dispatcher),text(resource,"resource_id"),num(resource,"state_version"),"bad-role","reason");});
    rejects(ErrorCode::Unauthorized,[&]{(void)f.store->abandon_owned_workspace(other_root,text(resource,"resource_id"),num(resource,"state_version"),"foreign","reason");});
    f.reopen(StoreRecoveryScope::request(f.node.request));
    rejects(ErrorCode::Stale,[&]{(void)f.store->abandon_owned_workspace(root,text(resource,"resource_id"),num(resource,"state_version"),"old-root","reason");});
    CHECK(f.abandon(resource).at("status")=="ABANDONED");
    rejects(ErrorCode::Unauthorized,[&]{(void)f.store->abandon_owned_workspace(other_root,text(resource,"resource_id"),num(resource,"state_version"),"abandon","retain the interrupted allocation for inspection");});
}
void owned_or_bound_resources_cannot_be_abandoned() {
    Fixture f; const auto owned=allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"owned");
    rejects(ErrorCode::NotReady,[&]{(void)f.abandon(owned);});
    const auto identity=owned.at("identity").at("workspace_identity");
    f.store->bind_workspace(f.node.team,text(identity,"path"),identity.at("device").to_number<std::uint64_t>(),identity.at("inode").to_number<std::uint64_t>());
    rejects(ErrorCode::NotReady,[&]{(void)f.abandon(owned,"bound");});
    CHECK(inspect_owned_workspace(owned)==owned.at("identity"));
}
void capacity_hold_keeps_receipt_registration_and_abandonment_open() {
    Fixture f(true,true); auto resource=f.prepare();
    CHECK(f.store->capacity_status(f.node.request).at("reserved_owned_allocation")==2);
    test::fill_business_capacity(*f.store,f.db,f.node.request);
    CHECK(f.store->capacity_status(f.node.request).at("status")=="WAITING_INPUT");
    resource=f.stage(resource); CHECK(f.store->capacity_status(f.node.request).at("reserved_owned_allocation")==1);
    CHECK(f.complete(resource).at("status")=="OWNED");
    CHECK(f.store->capacity_status(f.node.request).at("reserved_owned_allocation")==0);
    Fixture abandoned(true,true); const auto prepared=abandoned.prepare();
    test::fill_business_capacity(*abandoned.store,abandoned.db,abandoned.node.request);
    CHECK(abandoned.abandon(prepared).at("status")=="ABANDONED");
    CHECK(abandoned.store->capacity_status(abandoned.node.request).at("status")=="WAITING_INPUT");
}
void owned_paths_remain_protected_after_abandonment() {
    Fixture f; const auto abandoned=f.stage(f.prepare()); (void)f.abandon(abandoned);
    const auto other=f.create("other"); const auto stage=f.path(abandoned,true);
    const auto gen=allocation_parent_identity(stage/"work");
    rejects(ErrorCode::Unauthorized,[&]{(void)f.store->bind_workspace(other.team,text(gen,"path"),gen.at("device").to_number<std::uint64_t>(),gen.at("inode").to_number<std::uint64_t>());});
    rejects(ErrorCode::Unauthorized,[&]{(void)f.store->bind_workspace(f.node.team,text(gen,"path"),gen.at("device").to_number<std::uint64_t>(),gen.at("inode").to_number<std::uint64_t>());});
    rejects(ErrorCode::Unauthorized,[&]{(void)allocate_owned_workspace(*f.store,other.request,other.task,stage/"work","nested");});
    std::filesystem::create_directory(f.path(abandoned));
    rejects(ErrorCode::Unauthorized,[&]{(void)allocate_owned_workspace(*f.store,other.request,other.task,f.path(abandoned),"nested-final");});
    CHECK(f.store->active_owned_workspaces(f.node.request).empty());
    const auto status=f.store->workspace_cleanup_status(f.node.request);
    CHECK(canonical_json(status).find("ABANDONED")!=std::string::npos || canonical_json(status).find("abandoned")!=std::string::npos);
    CHECK(status.at("cleanup_complete")==false && status.at("private_data_retained")==true);
}
void legacy_complete_creation_and_partial_abandonment() {
    Fixture complete(false); const auto old=complete.prepare(); const auto observed=create_owned_workspace(old.at("intent"));
    const auto intent=old.at("intent"); complete.recovery=true; complete.reopen();
    const auto registered=allocate_owned_workspace(*complete.store,complete.node.request,complete.node.task,complete.parent,"allocation");
    CHECK(registered.at("identity")==observed && registered.at("intent")==intent && registered.at("allocation_receipt").is_null());
    Fixture partial(false); const auto prepared=partial.prepare(); std::filesystem::create_directory(partial.path(prepared));
    write_file(partial.path(prepared)/"owner.json","partial"); partial.recovery=true; partial.reopen();
    test::throws<Error>([&]{(void)allocate_owned_workspace(*partial.store,partial.node.request,partial.node.task,partial.parent,"allocation");});
    CHECK(partial.abandon(partial.current(prepared)).at("status")=="ABANDONED");
    CHECK(read_file(partial.path(prepared)/"owner.json")=="partial");
    CHECK(allocate_owned_workspace(*partial.store,partial.node.request,partial.node.task,partial.parent,"new-key").at("status")=="OWNED");
}
void populated_migration_requires_explicit_maintenance() {
    Fixture f(false); const auto resource=f.prepare(); const auto original=rows(f.db,"SELECT * FROM owned_workspaces");
    const auto requests=rows(f.db,"SELECT * FROM requests"), events=rows(f.db,"SELECT * FROM events"); f.store.reset();
    for(const auto& scope:{StoreRecoveryScope::request(f.node.request),StoreRecoveryScope::admission()}) {
        rejects(ErrorCode::Unsupported,[&]{auto rejected=f.open(scope,StoreOpenMode::Takeover,true);});
        CHECK(rows(f.db,"SELECT * FROM requests")==requests && rows(f.db,"SELECT * FROM events")==events);
        CHECK(rows(f.db,"SELECT * FROM owned_workspaces")==original);
    }
    {auto reader=f.open({},StoreOpenMode::ReadOnly,true);CHECK(reader->diagnostics().at("user_version")==407);CHECK(!reader->allocation_recovery_enabled());}
    f.recovery=true; f.reopen(); CHECK(f.store->diagnostics().at("user_version")==408);
    const auto migrated=f.current(resource); CHECK(migrated.at("intent")==resource.at("intent"));
    CHECK(migrated.at("state_version")==resource.at("state_version") && migrated.at("allocation_receipt").is_null());
    CHECK(f.abandon(migrated).at("status")=="ABANDONED");
}
void old_checkpoint_and_missing_capacity_policy_remain_exact() {
    Fixture f(false,true); const auto resource=f.prepare();
    CHECK(!f.store->capacity_status(f.node.request).as_object().contains("reserved_owned_allocation"));
    test::fill_business_capacity(*f.store,f.db,f.node.request);
    const auto checkpoint=f.store->checkpoint_capacity(f.root(),0,"checked original settled audit","checkpoint");
    const auto checkpoints=rows(f.db,"SELECT * FROM events WHERE type='capacity_checkpoint'");
    const auto frozen=f.store->request(f.node.request).at("config_snapshot");
    f.recovery=true; f.reopen();
    CHECK(f.store->capacity_status(f.node.request).at("window")==1);
    CHECK(f.store->capacity_status(f.node.request).at("reserved_owned_allocation")==1);
    CHECK(rows(f.db,"SELECT * FROM events WHERE type='capacity_checkpoint'")==checkpoints);
    CHECK(f.store->checkpoint_capacity(f.root(),0,"checked original settled audit","checkpoint")==checkpoint);
    CHECK(f.store->request(f.node.request).at("config_snapshot")==frozen);
    CHECK(f.abandon(f.current(resource)).at("status")=="ABANDONED");
    Fixture legacy(false,false,true); const auto saved=legacy.store->request(legacy.node.request).at("config_snapshot");
    legacy.recovery=true; legacy.reopen();
    CHECK(legacy.store->request(legacy.node.request).at("config_snapshot")==saved);
    CHECK(!saved.at("system").at("limits").as_object().contains("store_capacity"));
    CHECK(legacy.store->capacity_status(legacy.node.request).at("limits").at("business_events")==4096);
}
void all_old_states_and_frozen_manifest_migrate_byte_exactly() {
    Fixture f(false); std::vector<Json> original_resources;
    const auto parent=Workspace(f.parent,WorkspaceAccess::ReadOnly).root_identity();
    for(int index=0;index<4;++index) {
        std::string task=f.node.task;
        if(index>0)task=text(f.store->create_task(f.actor(f.node,Role::Dispatcher),num(f.store->request(f.node.request),"state_version"),
            1,1,"node-"+std::to_string(index),f.brief,"task-"+std::to_string(index)),"task_id");
        auto resource=f.store->prepare_owned_workspace(f.root(),task,parent,"legacy-"+std::to_string(index));
        if(index>0)resource=f.store->register_owned_workspace(f.root(),text(resource,"resource_id"),create_owned_workspace(resource.at("intent")));
        if(index>=2) {
            // Structural migration fixtures are labelled synthetic. They test
            // preservation of legacy stored bytes, never claim real deletion.
            const auto inventory=canonical_json(O{{"fixture","synthetic legacy inventory"},{"resource_id",resource.at("resource_id")}});
            sql(f.db,"UPDATE owned_workspaces SET inventory="+quote(inventory)+",status='DELETING',state_version=state_version+1 WHERE resource_id="+quote(text(resource,"resource_id")));
        }
        if(index==3)sql(f.db,"UPDATE owned_workspaces SET proof='{\"fixture\":\"synthetic legacy proof\"}',status='REMOVED',state_version=state_version+1 WHERE resource_id="+quote(text(resource,"resource_id")));
        original_resources.push_back(f.current(resource));
    }
    f.store->cancel_request(f.root(),num(f.store->request(f.node.request),"state_version"),"terminal-fixture");
    const auto manifest=canonical_json(O{{"fixture","synthetic frozen interrupted cleanup"},{"targets",A{original_resources[2],original_resources[3]}}});
    sql(f.db,"INSERT INTO workspace_cleanup_runs VALUES("+quote(f.node.request)+","+quote(manifest)+","+quote(sha256_hex(manifest))+",'RUNNING',7,'original-created','original-updated')");
    const std::string columns="resource_id,request_id,team_id,command_key,intent,identity,inventory,proof,status,state_version,created_at,updated_at";
    const auto old_rows=rows(f.db,"SELECT "+columns+" FROM owned_workspaces");
    const auto manifests=rows(f.db,"SELECT * FROM workspace_cleanup_runs");
    const auto end=scalar(f.db,"SELECT MAX(seq) FROM events"); const auto events=rows(f.db,"SELECT * FROM events");
    const auto frozen=f.store->request_snapshot(f.node.request);
    f.recovery=true; f.reopen();
    CHECK(rows(f.db,"SELECT "+columns+" FROM owned_workspaces")==old_rows);
    CHECK(rows(f.db,"SELECT * FROM workspace_cleanup_runs")==manifests);
    CHECK(rows(f.db,"SELECT * FROM events WHERE seq<="+std::to_string(end))==events);
    CHECK(f.store->request_snapshot(f.node.request)==frozen);
    CHECK(f.store->owned_workspaces(f.node.request).size()==4);
    for(const auto& resource:f.store->owned_workspaces(f.node.request))CHECK(resource.at("allocation_receipt").is_null()&&resource.at("abandonment").is_null());
    CHECK(sql_result(f.db,"UPDATE owned_workspaces SET state_version=state_version+2;")!=SQLITE_OK);
    CHECK(sql_result(f.db,"UPDATE owned_workspaces SET intent='{}',state_version=state_version+1;")!=SQLITE_OK);
    CHECK(sql_result(f.db,"UPDATE owned_workspaces SET inventory='{}',state_version=state_version+1 WHERE status='DELETING';")!=SQLITE_OK);
    CHECK(sql_result(f.db,"UPDATE owned_workspaces SET proof='{}',state_version=state_version+1 WHERE status='REMOVED';")!=SQLITE_OK);
    CHECK(sql_result(f.db,"UPDATE workspace_cleanup_runs SET manifest='{}',state_version=state_version+1;")!=SQLITE_OK);
    f.store.reset();
    rejects(ErrorCode::Unsupported,[&]{auto old=f.open({},StoreOpenMode::ReadOnly,false);});
    rejects(ErrorCode::Corrupt,[&]{Store bad(f.db,base_sql,store_sql,StoreOpenMode::ReadOnly,tools_sql,events_sql,workflow_sql,acceptance_sql,cleanup_sql,{},allocation_sql+"\n");});
}
void failed_structural_migration_rolls_back_original_tables() {
    Fixture f(false); (void)f.prepare(); const auto resources=rows(f.db,"SELECT * FROM owned_workspaces");
    const auto requests=rows(f.db,"SELECT * FROM requests"), events=rows(f.db,"SELECT * FROM events"), schema=rows(f.db,"SELECT * FROM sqlite_master");
    f.store.reset();
    test::throws<Error>([&]{Store bad(f.db,base_sql,store_sql,StoreOpenMode::Takeover,tools_sql,events_sql,workflow_sql,acceptance_sql,cleanup_sql,{},
        allocation_sql+"\nSELECT nonexistent_fixture_column FROM owned_workspaces;\n");});
    CHECK(scalar(f.db,"PRAGMA user_version")==407);
    CHECK(rows(f.db,"SELECT * FROM owned_workspaces")==resources);
    CHECK(rows(f.db,"SELECT * FROM requests")==requests && rows(f.db,"SELECT * FROM events")==events);
    CHECK(rows(f.db,"SELECT * FROM sqlite_master")==schema);
    f.recovery=true; f.reopen(); CHECK(f.store->diagnostics().at("user_version")==408);
}
void insert_inert_allocations(Fixture& f,const Json& original,std::size_t count,bool abandoned) {
    // Count fixtures contain no filesystem effect or adoption claim. All rows
    // obey the real schema; actual prepare/abandon APIs exercise the boundary.
    std::string statements="BEGIN;";
    for(std::size_t index=0;index<count;++index) {
        const auto id="res_quota_fixture_"+std::to_string(index); const auto team="team_quota_fixture_"+std::to_string(index);
        auto intent=original.at("intent"); intent.as_object()["resource_id"]=id;intent.as_object()["leaf"]=id;
        intent.as_object()["stage_leaf"]="stage_quota_fixture_"+std::to_string(index);intent.as_object()["team_id"]=team;
        intent.at("marker").as_object()["resource_id"]=id;intent.at("marker").as_object()["team_id"]=team;
        const auto key="quota-fixture-"+std::to_string(index);
        statements+="INSERT INTO owned_workspaces(resource_id,request_id,team_id,command_key,intent,status,state_version,created_at,updated_at,abandonment) VALUES("+
            quote(id)+","+quote(f.node.request)+","+quote(team)+","+quote(key)+","+quote(canonical_json(intent))+","+
            quote(abandoned?"ABANDONED":"PREPARED")+",1,'fixture-created','fixture-updated',"+(abandoned?"'{\"fixture\":\"inert retained history\"}'":"NULL")+");";
    }
    sql(f.db,statements+"COMMIT;");
}
void active_and_history_limits_are_separate_and_bounded() {
    Fixture active; const auto first=active.prepare();
    const auto second_task=text(active.store->create_task(active.actor(active.node,Role::Dispatcher),num(active.store->request(active.node.request),"state_version"),
        1,1,"second-node",active.brief,"second-task"),"task_id");
    insert_inert_allocations(active,first,63,false);
    CHECK(active.store->capacity_status(active.node.request).at("owned_allocation").at("active")==64);
    rejects(ErrorCode::Quota,[&]{(void)active.store->prepare_owned_workspace(active.root(),second_task,allocation_parent_identity(active.parent),"next");});
    CHECK(active.abandon(first).at("status")=="ABANDONED");
    CHECK(active.store->prepare_owned_workspace(active.root(),second_task,allocation_parent_identity(active.parent),"next").at("status")=="PREPARED");
    CHECK(active.store->capacity_status(active.node.request).at("owned_allocation").at("active")==64);
    CHECK(active.store->capacity_status(active.node.request).at("owned_allocation").at("history")==65);

    Fixture history(true,true); const auto pending=history.prepare(); insert_inert_allocations(history,pending,255,true);
    CHECK(history.store->capacity_status(history.node.request).at("owned_allocation").at("history")==256);
    CHECK(history.abandon(pending).at("status")=="ABANDONED");
    rejects(ErrorCode::Quota,[&]{(void)history.prepare("after-lifetime-limit");});
    test::fill_business_capacity(*history.store,history.db,history.node.request);
    (void)history.store->checkpoint_capacity(history.root(),0,"inert history retained and no operations pending","checkpoint");
    CHECK(history.store->capacity_status(history.node.request).at("window")==1);
    CHECK(history.store->capacity_status(history.node.request).at("owned_allocation").at("history")==256);
    rejects(ErrorCode::Quota,[&]{(void)history.prepare("checkpoint-cannot-reset-history");});
    CHECK(history.store->owned_workspaces(history.node.request).size()==256 && history.store->active_owned_workspaces(history.node.request).empty());
}
void continuation_at_control_watermark_never_blocks_existing_abandonment() {
    Fixture f(true,true); const auto resource=f.prepare();
    const auto status=f.store->capacity_status(f.node.request);
    const auto remaining=num(status.at("limits"),"control_events")-num(status.at("counts"),"control_events");
    sql(f.db,"WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<"+std::to_string(remaining)+") "
        "INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) SELECT 'allocation-control-'||x,"+
        quote(f.node.request)+",'finish_op','runtime','allocation-control-'||x,printf('%064d',0),'{}','{}','fixture' FROM n;");
    CHECK(f.store->capacity_status(f.node.request).at("status")=="WAITING_INPUT");
    CHECK(f.abandon(resource).at("status")=="ABANDONED");
    CHECK(f.store->capacity_status(f.node.request).at("reserved_owned_allocation")==0);
    CHECK(num(f.store->capacity_status(f.node.request).at("counts"),"control_events")==num(status.at("limits"),"control_events")+1);
}
void abandonment_never_clears_unknown_or_unhandled_amendment() {
    Fixture unknown; const auto resource=unknown.prepare(); const auto binding=unknown.store->binding(unknown.node.task);
    auto operation=unknown.store->prepare_operation(unknown.actor(unknown.node,Role::Worker),binding,num(unknown.store->task(unknown.node.task),"state_version"),
        "synthetic-operation","fs_write",O{{"fixture","synthetic running operation"}},"workspace","prepare-operation");
    operation=unknown.store->start_operation(unknown.actor(unknown.node,Role::Worker),text(operation,"op_id"),num(operation,"state_version"),"start-operation");
    unknown.reopen(StoreRecoveryScope::request(unknown.node.request));
    const auto original=unknown.store->operation(text(operation,"op_id")); CHECK(original.at("status")=="UNKNOWN");
    CHECK(unknown.abandon(resource).at("status")=="ABANDONED");
    CHECK(unknown.store->operation(text(operation,"op_id"))==original);
    unknown.store->acknowledge(unknown.actor(unknown.node,Role::Worker),unknown.store->binding(unknown.node.task),num(unknown.store->task(unknown.node.task),"state_version"),"fresh-ack");
    rejects(ErrorCode::UnknownOutcome,[&]{(void)unknown.store->prepare_operation(unknown.actor(unknown.node,Role::Worker),unknown.store->binding(unknown.node.task),
        num(unknown.store->task(unknown.node.task),"state_version"),"forbidden-operation","fs_write",O{{"fixture",true}},"workspace","forbidden-prepare");});

    Fixture amendment; const auto pending=amendment.prepare();
    amendment.store->amend(amendment.root(),num(amendment.store->request(amendment.node.request),"state_version"),"new requirement pending disposition",amendment.input("unused").constraint_catalog,"amend");
    const auto request=amendment.store->request(amendment.node.request),task=amendment.store->task(amendment.node.task);
    CHECK(amendment.abandon(pending).at("status")=="ABANDONED");
    CHECK(amendment.store->request(amendment.node.request)==request && amendment.store->task(amendment.node.task)==task);
    rejects(ErrorCode::NotReady,[&]{(void)amendment.store->acknowledge(amendment.actor(amendment.node,Role::Worker),amendment.store->binding(amendment.node.task),
        num(task,"state_version"),"must-not-apply-amend");});
}
void projected_allocation_path_length_refuses_before_intent_commit() {
    Fixture f; auto parent=allocation_parent_identity(f.parent);
    std::string path;
    for(int part=0;part<40;++part)path+="/"+std::string(100,'a');
    path+="/"+std::string(49,'b'); CHECK(path.size()==4090);
    // Synthetic host identity exercises Store admission only. No filesystem
    // access is attempted at this long path and no adoption is claimed.
    parent.as_object()["path"]=path;
    const auto events=rows(f.db,"SELECT * FROM events");
    rejects(ErrorCode::InvalidInput,[&]{(void)f.store->prepare_owned_workspace(f.root(),f.node.task,parent,"long-parent");});
    CHECK(f.store->owned_workspaces(f.node.request).empty());
    CHECK(rows(f.db,"SELECT * FROM events")==events && std::filesystem::is_empty(f.parent));
}
void abandoned_key_replays_without_observing_missing_or_replaced_parent() {
    for(const bool replacement:{false,true}) {
        Fixture f; const auto resource=f.stage(f.prepare()); const auto abandoned=f.abandon(resource);
        const auto another_task=text(f.store->create_task(f.actor(f.node,Role::Dispatcher),num(f.store->request(f.node.request),"state_version"),
            1,1,"second-node",f.brief,"second-task"),"task_id");
        const auto retained=f.dir.path/"retained-parent";
        std::filesystem::rename(f.parent,retained);
        if(replacement)write_file(f.parent,"unrelated replacement is a regular file");
        const auto resources=rows(f.db,"SELECT * FROM owned_workspaces"),events=rows(f.db,"SELECT * FROM events");
        const auto task=f.store->task(f.node.task),request=f.store->request(f.node.request);
        CHECK(allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent,"allocation")==abandoned);
        rejects(ErrorCode::Conflict,[&]{(void)allocate_owned_workspace(*f.store,f.node.request,f.node.task,retained,"allocation");});
        rejects(ErrorCode::Conflict,[&]{(void)allocate_owned_workspace(*f.store,f.node.request,another_task,f.parent,"allocation");});
        rejects(ErrorCode::Unauthorized,[&]{(void)allocate_owned_workspace(*f.store,f.node.request,f.node.task,f.parent/".."/"owned","allocation");});
        CHECK(rows(f.db,"SELECT * FROM owned_workspaces")==resources && rows(f.db,"SELECT * FROM events")==events);
        CHECK(f.store->task(f.node.task)==task && f.store->request(f.node.request)==request);
        CHECK(read_file(retained/text(resource.at("intent"),"stage_leaf")/"owner.json")==canonical_json(resource.at("intent").at("marker")));
        if(replacement)CHECK(read_file(f.parent)=="unrelated replacement is a regular file");
        else CHECK(!std::filesystem::exists(f.parent));
    }
}
}

int main() {
    if (::geteuid()==0) { std::cerr << "allocation permission regressions require a non-root test user\n"; return 2; }
    base_sql=read_file(source/"db/schema.sql"); store_sql=read_file(source/"db/migrations/004_s1_store.sql");
    tools_sql=read_file(source/"db/migrations/005_worker_tools.sql"); events_sql=read_file(source/"db/migrations/006_event_runtime.sql");
    workflow_sql=read_file(source/"db/migrations/007_workflow_runtime.sql"); acceptance_sql=read_file(source/"db/migrations/008_acceptance_runtime.sql");
    cleanup_sql=read_file(source/"db/migrations/009_owned_cleanup.sql"); allocation_sql=read_file(source/"db/migrations/010_owned_allocation_recovery.sql");
    schemas=std::make_unique<SchemaRegistry>(source/"schemas");
    return test::run({
        {"v2 allocation and exact replay retain private generations",ordinary_allocation_exact_replay},
        {"SIGKILL during partial marker requires explicit abandonment",partial_marker_kill_requires_explicit_abandonment},
        {"SIGKILL after full stage without receipt never adopts it",killed_complete_stage_without_receipt_is_not_adopted},
        {"SIGKILL after persisted receipt recovers exact stage",killed_after_receipt_recovers_exact_generation},
        {"SIGKILL before and after publication parent sync recovers",killed_during_publication_is_recoverable},
        {"SIGKILL after final sync before registration recovers",killed_after_final_sync_before_registration},
        {"failed receipt transaction never publishes the stage",receipt_write_failure_cannot_publish},
        {"failed OWNED transaction recovers existing final generation",registration_write_failure_recovers_without_recreation},
        {"external stage or final paths without receipt are retained",unreceipted_external_paths_are_never_adopted},
        {"copied identical marker does not substitute filesystem generation",copied_markers_do_not_prove_generation},
        {"replaced work marker parent and populated work refuse publication",replaced_work_marker_or_parent_and_nonempty_work_refuse},
        {"both or neither saved allocation path refuses publication",both_or_neither_receipted_paths_refuse},
        {"receipt registration CAS and immutable evidence are enforced",receipt_and_registration_cas_and_originals_are_enforced},
        {"abandonment exact replay and old key never resurrect allocation",abandonment_replay_is_exact_and_irreversible},
        {"abandonment preflight CAS and write rollback are atomic",abandonment_rollback_and_preflight_have_no_effect},
        {"terminal request missing parent and retired team can abandon",abandonment_terminal_missing_parent_and_retired_team},
        {"stale Root wrong role and foreign scope cannot abandon",stale_root_foreign_scope_and_nonroot_cannot_abandon},
        {"OWNED and bound resources cannot be abandoned",owned_or_bound_resources_cannot_be_abandoned},
        {"business hold preserves allocation settlement reserve",capacity_hold_keeps_receipt_registration_and_abandonment_open},
        {"abandoned stage and final remain protected boundaries",owned_paths_remain_protected_after_abandonment},
        {"v1 complete retry and partial explicit abandonment survive upgrade",legacy_complete_creation_and_partial_abandonment},
        {"populated scoped migration refuses before durable changes",populated_migration_requires_explicit_maintenance},
        {"old checkpoint bytes and absent capacity policy remain compatible",old_checkpoint_and_missing_capacity_policy_remain_exact},
        {"all old statuses and frozen manifest migrate with byte identity",all_old_states_and_frozen_manifest_migrate_byte_exactly},
        {"failed structural migration rolls back all original tables",failed_structural_migration_rolls_back_original_tables},
        {"active and lifetime history quotas remain separate bounded limits",active_and_history_limits_are_separate_and_bounded},
        {"control watermark cannot block existing allocation abandonment",continuation_at_control_watermark_never_blocks_existing_abandonment},
        {"abandonment never clears UNKNOWN or unhandled AMEND",abandonment_never_clears_unknown_or_unhandled_amendment},
        {"projected allocation path length refuses before intent commit",projected_allocation_path_length_refuses_before_intent_commit},
        {"abandoned key replays without observing missing or replaced parent",abandoned_key_replays_without_observing_missing_or_replaced_parent}
    });
}
