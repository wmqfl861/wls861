#include "wls/worker_tools.hpp"
#include "wls/offline_runtime.hpp"
#include "test_support.hpp"

#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {
void mkdir_prepared_phase(std::string_view key, const wls::s1::Json& prepared);
}

// Observe the existing private pre-dispatch phase in this executable only.
// Production WorkerTools has no callback and still supplies the implementation
// used here. The static archive does not also pull its worker_tools.o.
#define WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE(key, prepared) ::mkdir_prepared_phase(key, prepared)
#include "../src/worker_tools.cpp"
#undef WLS_WORKER_TOOLS_PRIVATE_TEST_PHASE

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

// Private syscall interposition, restricted to the current test thread and the
// exact fixture parent/child inodes. SQLite synchronization and unrelated paths
// continue to use real syscalls. These are injected failures, not power-loss
// durability claims.
namespace mkdir_fault {
struct State {
    bool active{false};
    dev_t parent_device{};
    ino_t parent_inode{};
    dev_t child_device{};
    ino_t child_inode{};
    unsigned int mkdir_calls{0};
    unsigned int directory_syncs{0};
    unsigned int fail_sync{0};
    bool callback_failed{false};
    std::function<void(int, const char*)> before_mkdir;
    std::function<void(int, const char*)> after_mkdir;
    std::function<void()> after_child_sync;
};
thread_local State state;
bool parent(const struct stat& info) {
    return info.st_dev == state.parent_device && info.st_ino == state.parent_inode;
}
bool child(const struct stat& info) {
    return info.st_dev == state.child_device && info.st_ino == state.child_inode;
}
struct Scope {
    explicit Scope(const std::filesystem::path& path) {
        CHECK(!state.active);
        struct stat info{};
        CHECK(::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode));
        state = State{};
        state.active = true;
        state.parent_device = info.st_dev;
        state.parent_inode = info.st_ino;
    }
    ~Scope() { state = State{}; }
};
}

extern "C" int mkdirat(int descriptor, const char* path, mode_t mode) noexcept {
    struct stat parent_info{};
    const bool watched = mkdir_fault::state.active && ::fstat(descriptor, &parent_info) == 0 &&
        mkdir_fault::parent(parent_info);
    if (!watched) return static_cast<int>(::syscall(SYS_mkdirat, descriptor, path, mode));
    auto& state = mkdir_fault::state;
    ++state.mkdir_calls;
    try {
        if (state.before_mkdir) {
            auto action = std::move(state.before_mkdir);
            state.before_mkdir = {};
            action(descriptor, path);
        }
        const int result = static_cast<int>(::syscall(SYS_mkdirat, descriptor, path, mode));
        if (result != 0) return result;
        struct stat created{};
        if (::fstatat(descriptor, path, &created, AT_SYMLINK_NOFOLLOW) != 0) return -1;
        state.child_device = created.st_dev;
        state.child_inode = created.st_ino;
        if (state.after_mkdir) {
            auto action = std::move(state.after_mkdir);
            state.after_mkdir = {};
            action(descriptor, path);
        }
        return result;
    } catch (...) {
        state.callback_failed = true;
        errno = EIO;
        return -1;
    }
}

extern "C" int fsync(int descriptor) {
    struct stat info{};
    if (::fstat(descriptor, &info) != 0) return -1;
    auto& state = mkdir_fault::state;
    const bool watched = state.active && S_ISDIR(info.st_mode) &&
        (mkdir_fault::parent(info) || mkdir_fault::child(info));
    if (watched) {
        ++state.directory_syncs;
        if (state.fail_sync == state.directory_syncs) { errno = EIO; return -1; }
    }
    const int result = static_cast<int>(::syscall(SYS_fsync, descriptor));
    if (watched && result == 0 && mkdir_fault::child(info) && state.after_child_sync) {
        auto action = std::move(state.after_child_sync);
        state.after_child_sync = {};
        try { action(); }
        catch (...) { state.callback_failed = true; errno = EIO; return -1; }
    }
    return result;
}

namespace {
using namespace wls;
using namespace wls::s1;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::unique_ptr<SchemaRegistry> schemas;
std::unique_ptr<FrozenRequestConfig> frozen;
std::string base_sql, migration_sql, tools_sql;
Json initial_script, constraints;
std::function<void(std::string_view, const Json&)> prepared_observer;

void mkdir_prepared_phase(std::string_view key, const Json& prepared) {
    if (prepared_observer) prepared_observer(key, prepared);
}
struct ResetObserver { ~ResetObserver() { prepared_observer = {}; } };
struct Umask {
    explicit Umask(mode_t value) : previous(::umask(value)) {}
    ~Umask() { ::umask(previous); }
    mode_t previous;
};
std::string text(const Json& value) { return std::string(value.as_string()); }
std::int64_t number(const Json& value) { return value.to_number<std::int64_t>(); }
void expect(ErrorCode code, const std::function<void()>& action) {
    try { action(); }
    catch (const Error& error) { CHECK(error.code() == code); return; }
    throw std::runtime_error("expected runtime error");
}
struct stat observed(const std::filesystem::path& path) {
    struct stat result{};
    CHECK(::lstat(path.c_str(), &result) == 0);
    return result;
}
void host_write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(static_cast<bool>(stream));
}
std::string host_read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(static_cast<bool>(stream));
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void no_effect(const WorkspaceMutationResult& result, std::optional<ErrorCode> code = std::nullopt) {
    CHECK(result.status == WorkspaceMutationStatus::EffectNotStarted);
    if (code) CHECK(result.error_code == code);
    CHECK(result.result.is_null());
    CHECK(result.evidence.at("mkdir_attempted") == false);
    CHECK(result.evidence.at("created") == false);
    CHECK(result.evidence.at("durable") == false);
}
void uncertain(const WorkspaceMutationResult& result) {
    CHECK(result.status == WorkspaceMutationStatus::OutcomeUnknown);
    CHECK(result.error_code == ErrorCode::UnknownOutcome);
    CHECK(result.result.is_null());
    CHECK(result.evidence.at("mkdir_attempted") == true);
    // Completed synchronization is a historical observation, not proof that
    // the final pathname still denotes the object that this operation made.
    CHECK(result.evidence.at("durable") == Json(result.evidence.at("child_synced").as_bool() &&
        result.evidence.at("parent_synced").as_bool()));
}
void complete(const WorkspaceMutationResult& result, const std::filesystem::path& target) {
    CHECK(result.status == WorkspaceMutationStatus::Succeeded && !result.error_code);
    CHECK(result.result.at("created") == true && result.result.at("type") == "directory");
    CHECK(result.result.at("mode") == 0700);
    CHECK(result.evidence.at("mkdir_attempted") == true && result.evidence.at("created") == true);
    CHECK(result.evidence.at("child_synced") == true && result.evidence.at("parent_synced") == true);
    CHECK(result.evidence.at("durable") == true);
    const auto info = observed(target);
    CHECK(S_ISDIR(info.st_mode) && (info.st_mode & 0777) == 0700);
    CHECK(result.evidence.at("directory_device").to_number<std::uint64_t>() == static_cast<std::uint64_t>(info.st_dev));
    CHECK(result.evidence.at("directory_inode").to_number<std::uint64_t>() == static_cast<std::uint64_t>(info.st_ino));
}

void private_directory_and_following_write() {
    for (const auto mask : {0002, 0022, 0077}) {
        test::TempDir fixture;
        Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
        Umask selected(static_cast<mode_t>(mask));
        mkdir_fault::Scope fault(fixture.path);
        const auto result = workspace.mkdir("output");
        complete(result, fixture.path / "output");
        CHECK(result.result.at("path") == "output");
        CHECK(mkdir_fault::state.mkdir_calls == 1 && mkdir_fault::state.directory_syncs == 2);
        CHECK(std::filesystem::is_empty(fixture.path / "output"));
        CHECK(workspace.write_atomic("output/result.txt", "directory result\n").status == WorkspaceMutationStatus::Succeeded);
        CHECK(workspace.read("output/result.txt").at("content") == "directory result\n");
        CHECK(workspace.list("output").at("entries").as_array().size() == 1);
        CHECK(workspace.search(".", "directory result").at("matches").as_array().size() == 1);
    }
}

void existing_targets_and_missing_parent() {
    test::TempDir fixture;
    std::filesystem::create_directory(fixture.path / "existing");
    host_write(fixture.path / "existing" / "retained", "keep original bytes");
    host_write(fixture.path / "file", "not a directory");
    const auto directory_before = observed(fixture.path / "existing");
    const auto file_before = observed(fixture.path / "file");
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    mkdir_fault::Scope fault(fixture.path);
    no_effect(workspace.mkdir("existing"), ErrorCode::Conflict);
    no_effect(workspace.mkdir("file"), ErrorCode::Conflict);
    no_effect(workspace.mkdir("missing/child"));
    CHECK(mkdir_fault::state.mkdir_calls == 0);
    CHECK(!std::filesystem::exists(fixture.path / "missing"));
    CHECK(observed(fixture.path / "existing").st_ino == directory_before.st_ino);
    CHECK(observed(fixture.path / "file").st_ino == file_before.st_ino);
    CHECK(host_read(fixture.path / "existing" / "retained") == "keep original bytes");
    CHECK(host_read(fixture.path / "file") == "not a directory");
}

void paths_and_read_only_have_no_effect() {
    test::TempDir fixture;
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    Workspace reader(fixture.path, WorkspaceAccess::ReadOnly);
    mkdir_fault::Scope fault(fixture.path);
    no_effect(reader.mkdir("denied"), ErrorCode::Unauthorized);
    for (const std::string path : {"", ".", "..", "../outside", "/absolute", "./child", "a//b", "a/../b", "a/", ".wls-tmp-reserved"})
        no_effect(workspace.mkdir(path));
    no_effect(workspace.mkdir(std::string("bad\0name", 8)));
    no_effect(workspace.mkdir(std::string(256, 'x')));
    CHECK(mkdir_fault::state.mkdir_calls == 0 && std::filesystem::is_empty(fixture.path));
}

void links_and_untrusted_parent_are_not_followed() {
    test::TempDir fixture, outside;
    host_write(outside.path / "retained", "external fixture");
    std::filesystem::create_directory_symlink(outside.path, fixture.path / "redirect");
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    mkdir_fault::Scope fault(fixture.path);
    no_effect(workspace.mkdir("redirect/new"), ErrorCode::Unauthorized);
    no_effect(workspace.mkdir("redirect"), ErrorCode::Unauthorized);
    // Whole-tree admission also rejects an unrelated link before mkdir.
    no_effect(workspace.mkdir("unrelated"), ErrorCode::Unauthorized);
    std::filesystem::remove(fixture.path / "redirect");
    CHECK(::link((outside.path / "retained").c_str(), (fixture.path / "hardlink").c_str()) == 0);
    no_effect(workspace.mkdir("hardlink"), ErrorCode::Unauthorized);
    no_effect(workspace.mkdir("another"), ErrorCode::Unauthorized);
    std::filesystem::remove(fixture.path / "hardlink");
    CHECK(::mkfifo((fixture.path / "fifo").c_str(), 0600) == 0);
    no_effect(workspace.mkdir("fifo"), ErrorCode::Unauthorized);
    std::filesystem::remove(fixture.path / "fifo");
    std::filesystem::create_directory(fixture.path / "parent");
    CHECK(::chmod((fixture.path / "parent").c_str(), 0777) == 0);
    const auto refused = workspace.mkdir("parent/new");
    CHECK(::chmod((fixture.path / "parent").c_str(), 0700) == 0);
    no_effect(refused, ErrorCode::Unauthorized);
    CHECK(mkdir_fault::state.mkdir_calls == 0);
    CHECK(!std::filesystem::exists(outside.path / "new") && !std::filesystem::exists(fixture.path / "parent" / "new"));
    CHECK(host_read(outside.path / "retained") == "external fixture");
}

void projected_entries_and_directory_depth_are_bounded() {
    test::TempDir entry_fixture, depth_fixture, shallow_fixture;
    WorkspaceLimits entries;
    entries.max_entries = 2;
    Workspace entry_workspace(entry_fixture.path, WorkspaceAccess::ReadWrite, entries);
    complete(entry_workspace.mkdir("first"), entry_fixture.path / "first");
    CHECK(entry_workspace.write_atomic("first/kept", "kept").status == WorkspaceMutationStatus::Succeeded);
    no_effect(entry_workspace.mkdir("overflow"), ErrorCode::Quota);
    CHECK(!std::filesystem::exists(entry_fixture.path / "overflow"));
    CHECK(entry_workspace.read("first/kept").at("content") == "kept");

    WorkspaceLimits depth;
    depth.max_depth = 3;
    Workspace depth_workspace(depth_fixture.path, WorkspaceAccess::ReadWrite, depth);
    complete(depth_workspace.mkdir("one"), depth_fixture.path / "one");
    complete(depth_workspace.mkdir("one/two"), depth_fixture.path / "one" / "two");
    no_effect(depth_workspace.mkdir("one/two/three"), ErrorCode::Quota);
    CHECK(!std::filesystem::exists(depth_fixture.path / "one" / "two" / "three"));
    // A leaf file at this level is valid under the unchanged traversal rules.
    CHECK(depth_workspace.write_atomic("one/two/result", "depth boundary").status == WorkspaceMutationStatus::Succeeded);
    CHECK(depth_workspace.search(".", "depth boundary").at("matches").as_array().size() == 1);
    depth.max_depth = 1;
    Workspace shallow(shallow_fixture.path, WorkspaceAccess::ReadWrite, depth);
    no_effect(shallow.mkdir("too-deep"), ErrorCode::Quota);
    CHECK(shallow.write_atomic("root-file", "valid leaf").status == WorkspaceMutationStatus::Succeeded);
}

void directory_sync_failures_retain_unknown_effects() {
    for (const auto failed_sync : {1U, 2U}) {
        test::TempDir fixture;
        Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
        mkdir_fault::Scope fault(fixture.path);
        mkdir_fault::state.fail_sync = failed_sync;
        const auto result = workspace.mkdir("uncertain");
        uncertain(result);
        CHECK(result.evidence.at("created") == true);
        CHECK(result.evidence.at("child_synced") == Json(failed_sync == 2));
        CHECK(result.evidence.at("parent_synced") == false);
        CHECK(mkdir_fault::state.mkdir_calls == 1 && mkdir_fault::state.directory_syncs == failed_sync);
        CHECK(std::filesystem::is_directory(fixture.path / "uncertain"));
        CHECK(std::filesystem::is_empty(fixture.path / "uncertain"));
        CHECK((observed(fixture.path / "uncertain").st_mode & 0777) == 0700);
    }
}

void raced_existing_target_is_unknown_without_adoption() {
    test::TempDir fixture;
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    mkdir_fault::Scope fault(fixture.path);
    bool raced = false;
    ino_t competing_inode{};
    mkdir_fault::state.before_mkdir = [&](int descriptor, const char* path) {
        CHECK(::syscall(SYS_mkdirat, descriptor, path, 0700) == 0);
        competing_inode = observed(fixture.path / "raced").st_ino;
        raced = true;
    };
    const auto result = workspace.mkdir("raced");
    CHECK(raced && !mkdir_fault::state.callback_failed);
    uncertain(result);
    CHECK(result.evidence.at("created") == false && result.evidence.at("phase") == "mkdir");
    CHECK(mkdir_fault::state.mkdir_calls == 1 && mkdir_fault::state.directory_syncs == 0);
    CHECK(observed(fixture.path / "raced").st_ino == competing_inode);
}

void open_failure_after_creation_is_unknown() {
    CHECK(::geteuid() != 0); // This permission fault must be tested as an ordinary user.
    test::TempDir fixture;
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    mkdir_fault::Scope fault(fixture.path);
    bool changed = false;
    mkdir_fault::state.after_mkdir = [&](int, const char*) {
        CHECK(::chmod((fixture.path / "closed").c_str(), 0000) == 0);
        changed = true;
    };
    const auto result = workspace.mkdir("closed");
    CHECK(::chmod((fixture.path / "closed").c_str(), 0700) == 0);
    CHECK(changed && !mkdir_fault::state.callback_failed);
    uncertain(result);
    CHECK(result.evidence.at("created") == true && result.evidence.at("child_synced") == false);
    CHECK(std::filesystem::is_directory(fixture.path / "closed"));
}

void replaced_root_parent_or_child_never_proves_success() {
    for (const auto which : {0, 1, 2}) {
        test::TempDir fixture;
        const auto root = fixture.path / "root";
        std::filesystem::create_directory(root);
        std::filesystem::create_directory(root / "parent");
        Workspace workspace(root, WorkspaceAccess::ReadWrite);
        mkdir_fault::Scope fault(root / "parent");
        bool replaced = false;
        mkdir_fault::state.after_child_sync = [&] {
            if (which == 0) {
                std::filesystem::rename(root, fixture.path / "original-root");
                std::filesystem::create_directory(root);
            } else if (which == 1) {
                std::filesystem::rename(root / "parent", root / "original-parent");
                std::filesystem::create_directory(root / "parent");
            } else {
                std::filesystem::rename(root / "parent" / "new", root / "parent" / "original-child");
                std::filesystem::create_directory(root / "parent" / "new");
                host_write(root / "parent" / "new" / "foreign", "retain replacement");
            }
            replaced = true;
        };
        const auto result = workspace.mkdir("parent/new");
        CHECK(replaced && !mkdir_fault::state.callback_failed);
        uncertain(result);
        CHECK(result.evidence.at("created") == true);
        CHECK(result.evidence.at("child_synced") == true && result.evidence.at("parent_synced") == true);
        const auto original = which == 0 ? fixture.path / "original-root" / "parent" / "new" :
            which == 1 ? root / "original-parent" / "new" : root / "parent" / "original-child";
        CHECK(std::filesystem::is_directory(original) && std::filesystem::is_empty(original));
        if (which == 2) CHECK(host_read(root / "parent" / "new" / "foreign") == "retain replacement");
        else CHECK(std::filesystem::is_empty(which == 0 ? root : root / "parent"));
    }
}

void fixture_sql(const std::filesystem::path& database, const std::string& sql) {
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open(database.c_str(), &raw) == SQLITE_OK);
    const std::unique_ptr<sqlite3, int(*)(sqlite3*)> connection(raw, sqlite3_close);
    CHECK(sqlite3_busy_timeout(connection.get(), 1000) == SQLITE_OK);
    char* error = nullptr;
    const int status = sqlite3_exec(connection.get(), sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : "";
    sqlite3_free(error);
    if (status != SQLITE_OK) throw std::runtime_error("fixture SQL failed: " + message);
}
AdmissionInput admission() {
    AdmissionInput result;
    result.tenant_id = "mkdir-test"; result.project_id = "private-fixtures"; result.root_id = "test-root";
    result.function_id = frozen->function_id(); result.idempotency_key = new_id("key");
    result.original_text = "测试有界目录创建；保留原始需求；不要调用外部服务。";
    result.config_snapshot = frozen->json(); result.constraint_catalog = constraints;
    const auto grants = frozen->worker_tools();
    result.tool_allowlist.assign(grants.begin(), grants.end());
    return result;
}
std::vector<FakeModelStep> steps(std::optional<ModelToolCall> worker_call = std::nullopt) {
    auto result = fake_model_steps_from_json(initial_script);
    if (worker_call) result.at(2).tool_calls = {*worker_call};
    return result;
}
struct Session {
    test::TempDir directory;
    std::filesystem::path database{directory.path / "runtime.sqlite"};
    std::filesystem::path work{directory.path / "work"};
    std::unique_ptr<Store> store;
    std::unique_ptr<WorkerTools> tools;
    std::unique_ptr<OfflineRuntime> runtime;
    std::string request;
    explicit Session(std::optional<AdmissionInput> input = std::nullopt) {
        std::filesystem::create_directory(work);
        store = std::make_unique<Store>(database, base_sql, migration_sql, StoreOpenMode::Takeover, tools_sql);
        request = text(store->admit(input ? *input : admission()).at("request_id"));
        tools = std::make_unique<WorkerTools>(*store, work);
        runtime = std::make_unique<OfflineRuntime>(*store, *schemas, tools.get());
        CHECK(runtime->run(request, steps()).at("calls").as_array().size() == 3);
    }
    std::string task() const { return text(store->tasks(request).front().at("task_id")); }
    ExecutionBinding binding() const { return store->binding(task()); }
    StoreActor worker() const { return store->host_actor(Role::Worker, request, binding().team_id); }
    std::int64_t version() const { return number(store->task(task()).at("state_version")); }
    Json execute(const std::string& name, const Json& args, const std::string& key = {}, const std::string& command = {}) {
        return tools->execute(worker(), binding(), version(), key.empty() ? new_id("key") : key,
            name, args, command.empty() ? new_id("cmd") : command);
    }
    void amend() {
        (void)store->amend(store->host_actor(Role::Root, request), number(store->request(request).at("state_version")),
            "变更目录要求；保留原始需求；不要调用外部服务。", constraints, new_id("amend"));
    }
};
std::string operation_id(const Json& result) { return text(result.at("operation").at("op_id")); }
void succeeded(const Json& result) {
    CHECK(result.at("operation").at("status") == "SUCCEEDED");
    CHECK(result.at("operation").at("side_effect") == "workspace");
    CHECK(result.at("execution_result").at("observed_outcome") == "SUCCEEDED");
    CHECK(result.at("execution_result").at("accepted") == 1);
}
bool declares(const std::vector<ContextTool>& declarations, std::string_view name) {
    return std::any_of(declarations.begin(), declarations.end(), [&](const auto& tool) { return tool.name == name; });
}

void frozen_grants_schema_and_diagnostic_boundary() {
    Session session;
    const auto& definition = worker_tool_definition("fs_mkdir");
    CHECK(definition.version == "wls-fs-mkdir-v1" && definition.category == ToolClass::Business);
    CHECK(definition.side_effect == "workspace");
    CHECK(declares(session.tools->declarations(session.worker()), "fs_mkdir"));
    const auto task = session.store->task(session.task());
    const auto events = session.store->events(session.request);
    for (const Json& invalid : {Json(Object{}), Json(Object{{"path", ""}}), Json(Object{{"path", 7}}),
                              Json(Object{{"path", std::string(4097, 'a')}}), Json(Array{})})
        expect(ErrorCode::InvalidInput, [&] { (void)session.execute("fs_mkdir", invalid); });
    for (const auto* field : {"parents", "exist_ok", "mode", "side_effect", "category", "role", "workspace", "request_id"}) {
        Json args = Object{{"path", "forged"}};
        args.as_object()[field] = true;
        expect(ErrorCode::InvalidInput, [&] { (void)session.execute("fs_mkdir", args); });
    }
    expect(ErrorCode::Unauthorized, [&] {
        (void)session.tools->diagnostic(session.worker(), session.binding(), "fs_mkdir", Object{{"path", "diagnostic"}});
    });
    const auto leader = session.store->host_actor(Role::TeamLead, session.request, session.binding().team_id);
    expect(ErrorCode::Unauthorized, [&] {
        (void)session.tools->execute(leader, session.binding(), session.version(), "lead-key", "fs_mkdir", Object{{"path", "leader"}}, "lead-command");
    });
    CHECK(session.store->task(session.task()) == task && session.store->events(session.request) == events);
    CHECK(std::filesystem::is_empty(session.work));

    auto restricted = admission();
    std::erase(restricted.tool_allowlist, "fs_mkdir");
    Session without_grant(restricted);
    CHECK(!declares(without_grant.tools->declarations(without_grant.worker()), "fs_mkdir"));
    expect(ErrorCode::Unauthorized, [&] { (void)without_grant.execute("fs_mkdir", Object{{"path", "denied"}}); });
    CHECK(std::filesystem::is_empty(without_grant.work));
}

void successful_fact_replays_without_creating_again() {
    Session session;
    const Json args = Object{{"path", "output"}};
    const auto first = session.execute("fs_mkdir", args, "mkdir-key", "mkdir-command");
    succeeded(first);
    CHECK(first.at("executed_now") == true && first.at("replayed") == false);
    const auto fact = session.store->operation_result(operation_id(first));
    CHECK(fact.at("original_binding") == to_json(session.binding()));
    succeeded(session.execute("fs_write", Object{{"path", "output/result.txt"}, {"content", "stored after mkdir"}}));
    const auto task = session.store->task(session.task());
    const auto budget = session.store->budget(session.request);
    std::filesystem::rename(session.work / "output", session.work / "moved-output");
    mkdir_fault::Scope fault(session.work);
    const auto replay = session.execute("fs_mkdir", args, "mkdir-key", "different-command");
    CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
    CHECK(operation_id(replay) == operation_id(first) && replay.at("execution_result") == fact);
    CHECK(mkdir_fault::state.mkdir_calls == 0 && !std::filesystem::exists(session.work / "output"));
    CHECK(host_read(session.work / "moved-output" / "result.txt") == "stored after mkdir");
    CHECK(session.store->task(session.task()) == task && session.store->budget(session.request) == budget);
    expect(ErrorCode::Conflict, [&] { (void)session.execute("fs_mkdir", Object{{"path", "different"}}, "mkdir-key"); });
    expect(ErrorCode::Conflict, [&] { (void)session.execute("note", Object{{"text", "wrong operation"}}, "mkdir-key"); });
    CHECK(!std::filesystem::exists(session.work / "different"));
}

void concurrent_exact_calls_claim_only_one_mkdir() {
    Session session;
    WorkerTools other(*session.store, session.work);
    other.bind(session.worker());
    const auto actor = session.worker(); const auto binding = session.binding(); const auto version = session.version();
    const Json args = Object{{"path", "once"}};
    std::barrier ready(2);
    std::array<Json, 2> results;
    std::array<std::exception_ptr, 2> errors;
    std::array<unsigned int, 2> mkdir_calls{};
    const auto invoke = [&](std::size_t index, WorkerTools& executor) {
        ready.arrive_and_wait();
        try {
            mkdir_fault::Scope fault(session.work);
            results[index] = executor.execute(actor, binding, version, "same-key", "fs_mkdir", args, "same-command");
            mkdir_calls[index] = mkdir_fault::state.mkdir_calls;
        } catch (...) { errors[index] = std::current_exception(); }
    };
    std::jthread left([&] { invoke(0, *session.tools); });
    std::jthread right([&] { invoke(1, other); });
    left.join(); right.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    CHECK(mkdir_calls[0] + mkdir_calls[1] == 1);
    CHECK(static_cast<int>(results[0].at("executed_now").as_bool()) + static_cast<int>(results[1].at("executed_now").as_bool()) == 1);
    CHECK(operation_id(results[0]) == operation_id(results[1]));
    CHECK(session.store->operation(operation_id(results[0])).at("status") == "SUCCEEDED");
    CHECK(session.store->operation_result(operation_id(results[0])).at("evidence").at("durable") == true);
    CHECK(std::filesystem::is_empty(session.work / "once"));
}

void real_unknown_blocks_new_business_and_replays_original_fact() {
    Session session;
    const auto captured = session.binding();
    const Json args = Object{{"path", "uncertain"}};
    Json first;
    {
        mkdir_fault::Scope fault(session.work);
        mkdir_fault::state.fail_sync = 2;
        first = session.execute("fs_mkdir", args, "unknown-key", "unknown-command");
        CHECK(mkdir_fault::state.directory_syncs == 2 && mkdir_fault::state.mkdir_calls == 1);
    }
    CHECK(first.at("operation").at("status") == "UNKNOWN");
    const auto fact = session.store->operation_result(operation_id(first));
    CHECK(fact.at("observed_outcome") == "UNKNOWN" && fact.at("original_binding") == to_json(captured));
    CHECK(fact.at("evidence").at("created") == true && fact.at("evidence").at("parent_synced") == false);
    CHECK(std::filesystem::is_directory(session.work / "uncertain"));
    // Current filesystem presence/absence cannot silently settle or retry it.
    std::filesystem::rename(session.work / "uncertain", session.work / "retained-uncertain");
    mkdir_fault::Scope fault(session.work);
    const auto replay = session.execute("fs_mkdir", args, "unknown-key", "unknown-replay");
    CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
    CHECK(replay.at("execution_result") == fact && replay.at("operation").at("status") == "UNKNOWN");
    expect(ErrorCode::UnknownOutcome, [&] { (void)session.execute("fs_mkdir", Object{{"path", "blocked"}}); });
    expect(ErrorCode::UnknownOutcome, [&] { (void)session.execute("fs_write", Object{{"path", "blocked-file"}, {"content", "x"}}); });
    CHECK(mkdir_fault::state.mkdir_calls == 0);
    CHECK(!std::filesystem::exists(session.work / "uncertain") && !std::filesystem::exists(session.work / "blocked"));
    const auto listing = session.tools->diagnostic(session.worker(), session.binding(), "fs_list", Object{});
    CHECK(listing.at("entries").as_array().size() == 1);
    CHECK(session.store->operation_result(operation_id(first)) == fact);
}

void amend_rejects_late_model_mkdir_and_preserves_old_result() {
    Session session;
    const Json args = Object{{"path", "original"}};
    const auto first = session.execute("fs_mkdir", args, "before-amend");
    const auto fact = session.store->operation_result(operation_id(first));
    const FakeModel fake(steps(ModelToolCall{"late-mkdir", "fs_mkdir", Object{{"path", "late"}}}));
    const auto call = session.runtime->reserve_call(fake, session.request, Role::Worker, session.task());
    session.amend();
    const auto task = session.store->task(session.task());
    const auto spent = number(session.store->budget(session.request).at("spent_input"));
    expect(ErrorCode::Stale, [&] { (void)session.runtime->accept_response(call, fake.complete(call.request)); });
    CHECK(session.store->attempt(call.request.attempt_id).at("accepted") == 0);
    CHECK(number(session.store->budget(session.request).at("spent_input")) == spent + 100);
    CHECK(session.store->task(session.task()) == task && !std::filesystem::exists(session.work / "late"));
    expect(ErrorCode::NotReady, [&] { (void)session.execute("fs_mkdir", Object{{"path", "blocked"}}); });
    const auto replay = session.execute("fs_mkdir", args, "before-amend");
    CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
    CHECK(replay.at("execution_result") == fact);
    CHECK(fact.at("original_binding").at("requirements_revision") == 1);
    CHECK(session.store->task(session.task()) == task && !std::filesystem::exists(session.work / "blocked"));
}

void mkdir_consumes_existing_business_quota() {
    test::TempDir config;
    auto system = frozen->json().at("system");
    system.at("limits").at("worker_max_tool_calls_per_order") = 2;
    const auto system_path = config.path / "system.yaml";
    host_write(system_path, canonical_json(system));
    // Freeze the real fixture bytes so parsed limits, retained dependencies and
    // their hashes agree; production snapshot verification remains intact.
    const auto limited = load_config_catalog(system_path,
        source / "config/examples/functions", source / "prompts").freeze("general");
    auto input = admission();
    input.config_snapshot = limited.json();
    Session session(input); // The initialization note consumes one business call.
    const auto first = session.execute("fs_mkdir", Object{{"path", "last-slot"}}, "last-slot-key");
    succeeded(first);
    expect(ErrorCode::Quota, [&] { (void)session.execute("fs_mkdir", Object{{"path", "no-slot"}}); });
    CHECK(!std::filesystem::exists(session.work / "no-slot"));
    const auto replay = session.execute("fs_mkdir", Object{{"path", "last-slot"}}, "last-slot-key");
    CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
    CHECK(operation_id(replay) == operation_id(first));
}

void settlement_failure_fences_an_already_prepared_mkdir() {
    Session session;
    WorkerTools second(*session.store, session.work);
    second.bind(session.worker());
    const auto actor = session.worker(); const auto binding = session.binding(); const auto version = session.version();
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool reached{false}, released{false};
        Json prepared;
        void release() { const std::lock_guard lock(mutex); released = true; changed.notify_all(); }
    } gate;
    CHECK(!prepared_observer);
    prepared_observer = [&](std::string_view key, const Json& prepared) {
        if (key != "waiting-key") return;
        std::unique_lock lock(gate.mutex);
        gate.prepared = prepared; gate.reached = true; gate.changed.notify_all();
        if (!gate.changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate.released; }))
            throw std::runtime_error("mkdir prepared gate timed out");
    };
    ResetObserver reset;
    std::exception_ptr waiting_error;
    std::jthread waiting([&] {
        try { (void)second.execute(actor, binding, version, "waiting-key", "fs_mkdir", Object{{"path", "waiting"}}, "waiting-command"); }
        catch (...) { waiting_error = std::current_exception(); }
    });
    struct Release { Gate& gate; ~Release() { gate.release(); } } release{gate};
    Json prepared;
    {
        std::unique_lock lock(gate.mutex);
        CHECK(gate.changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate.reached; }));
        prepared = gate.prepared;
    }
    CHECK(prepared.at("status") == "PREPARED" && prepared.at("started_at").is_null());
    fixture_sql(session.database,
        "CREATE TRIGGER fail_mkdir_settlements BEFORE INSERT ON events "
        "WHEN NEW.type IN ('record_op_result','finish_op') "
        "BEGIN SELECT RAISE(ABORT,'injected mkdir settlement failure'); END");
    const Json args = Object{{"path", "unsettled"}};
    expect(ErrorCode::Storage, [&] { (void)session.execute("fs_mkdir", args, "unsettled-key", "unsettled-command"); });
    CHECK(std::filesystem::is_directory(session.work / "unsettled"));
    const auto unsettled = session.store->operation_for_key(actor, binding, "unsettled-key", "fs_mkdir", args);
    CHECK(unsettled.at("status") == "RUNNING");
    CHECK(session.store->operation_result(text(unsettled.at("op_id"))).is_null());
    fixture_sql(session.database, "DROP TRIGGER fail_mkdir_settlements");
    gate.release(); waiting.join();
    CHECK(waiting_error != nullptr);
    expect(ErrorCode::NotReady, [&] { std::rethrow_exception(waiting_error); });
    const auto after = session.store->operation(text(prepared.at("op_id")));
    CHECK(after.at("status") == "PREPARED" && after.at("started_at").is_null());
    CHECK(after.at("state_version") == prepared.at("state_version"));
    CHECK(!std::filesystem::exists(session.work / "waiting"));
    WorkerTools fresh(*session.store, session.work);
    fresh.bind(actor);
    expect(ErrorCode::NotReady, [&] {
        (void)fresh.execute(actor, binding, session.version(), "fresh-key", "fs_mkdir", Object{{"path", "fresh"}}, "fresh-command");
    });
    CHECK(!std::filesystem::exists(session.work / "fresh"));
    const auto replay = fresh.execute(actor, binding, session.version(), "waiting-key", "fs_mkdir", Object{{"path", "waiting"}}, "waiting-replay");
    CHECK(replay.at("replayed") == true && replay.at("executed_now") == false);
    CHECK(replay.at("operation").at("status") == "PREPARED" && replay.at("execution_result").is_null());
}
}

int main() {
    schemas = std::make_unique<SchemaRegistry>(source / "schemas");
    frozen = std::make_unique<FrozenRequestConfig>(load_config_catalog(source / "config/examples/system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general"));
    base_sql = read_runtime_file(source / "db/schema.sql");
    migration_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    tools_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    initial_script = parse_json_strict(read_runtime_file(source / "tests/fixtures/s1_fake_initial.json"));
    constraints = parse_json_strict(read_runtime_file(source / "tests/fixtures/s1_constraints.json"));
    return test::run({
        {"mkdir is private under ordinary umasks and supports following file tools", private_directory_and_following_write},
        {"existing directory or file and missing parent have no creation effect", existing_targets_and_missing_parent},
        {"relative path and read-only boundaries reject before mkdir", paths_and_read_only_have_no_effect},
        {"links, special files and writable parents cannot be adopted", links_and_untrusted_parent_are_not_followed},
        {"projected entry and directory depth limits leave the tree traversable", projected_entries_and_directory_depth_are_bounded},
        {"child and parent directory fsync failures retain UNKNOWN directories", directory_sync_failures_retain_unknown_effects},
        {"racing EEXIST after mkdir dispatch stays UNKNOWN without adoption", raced_existing_target_is_unknown_without_adoption},
        {"open failure after successful mkdir retains UNKNOWN effect", open_failure_after_creation_is_unknown},
        {"post-creation root parent and target replacements cannot prove success", replaced_root_parent_or_child_never_proves_success},
        {"frozen grants strict arguments Worker role and diagnostic boundary", frozen_grants_schema_and_diagnostic_boundary},
        {"successful mkdir replay returns its original fact without another effect", successful_fact_replays_without_creating_again},
        {"concurrent exact commands have one actual mkdir dispatch", concurrent_exact_calls_claim_only_one_mkdir},
        {"real uncertain mkdir preserves original evidence and blocks new business", real_unknown_blocks_new_business_and_replays_original_fact},
        {"AMEND fences a late mkdir response and preserves old result replay", amend_rejects_late_model_mkdir_and_preserves_old_result},
        {"mkdir consumes ordinary business quota while exact replay remains a read", mkdir_consumes_existing_business_quota},
        {"failed result persistence blocks an already prepared and a fresh mkdir", settlement_failure_fences_an_already_prepared_mkdir}
    });
}
