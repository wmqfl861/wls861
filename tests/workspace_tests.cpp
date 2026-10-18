#include "wls/workspace.hpp"
#include "test_support.hpp"

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <utility>

// Test-binary-only failure injection: successful calls still execute the real
// fsync syscall. This tests classification at the file/rename/directory boundary
// without claiming a physical power failure or modifying production interfaces.
namespace fault {
thread_local bool fail_file = false;
thread_local bool fail_directory = false;
thread_local std::function<void()> after_file_sync;
thread_local unsigned int file_syncs = 0;
thread_local unsigned int directory_syncs = 0;
void clear() {
    fail_file = false; fail_directory = false; after_file_sync = {};
    file_syncs = 0; directory_syncs = 0;
}
struct Reset { ~Reset() { clear(); } };
}
extern "C" int fsync(int descriptor) {
    struct stat info{};
    if (::fstat(descriptor, &info) != 0) return -1;
    if (S_ISDIR(info.st_mode)) {
        ++fault::directory_syncs;
        if (std::exchange(fault::fail_directory, false)) { errno = EIO; return -1; }
    }
    if (S_ISREG(info.st_mode)) {
        ++fault::file_syncs;
        if (std::exchange(fault::fail_file, false)) { errno = EIO; return -1; }
    }
    const int result = static_cast<int>(::syscall(SYS_fsync, descriptor));
    if (result == 0 && S_ISREG(info.st_mode) && fault::after_file_sync) {
        auto action = std::move(fault::after_file_sync);
        fault::after_file_sync = {};
        try { action(); } catch (...) { errno = EIO; return -1; }
    }
    return result;
}

namespace {
using namespace wls::s1;
void host_write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("test host file write failed");
}
std::string host_read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string string_at(const Json& value, std::string_view name) {
    return std::string(value.as_object().at(name).as_string());
}
std::uint64_t uint_at(const Json& value, std::string_view name) {
    return value.as_object().at(name).to_number<std::uint64_t>();
}
void rejected(const WorkspaceMutationResult& result, ErrorCode code) {
    CHECK(result.status == WorkspaceMutationStatus::EffectNotStarted);
    CHECK(result.error_code == code);
    CHECK(result.result.is_null());
}
std::size_t file_count(const std::filesystem::path& path) {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(path), std::filesystem::directory_iterator()));
}

void identity_and_creation() {
    test::TempDir fixture;
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    const auto identity = workspace.root_identity();
    CHECK(string_at(identity, "path") == std::filesystem::canonical(fixture.path).string());
    const int root_fd = workspace.dup_root_fd();
    CHECK((::fcntl(root_fd, F_GETFD) & FD_CLOEXEC) != 0);
    struct stat info{};
    CHECK(::fstat(root_fd, &info) == 0);
    CHECK(uint_at(identity, "device") == static_cast<std::uint64_t>(info.st_dev));
    CHECK(uint_at(identity, "inode") == static_cast<std::uint64_t>(info.st_ino));
    CHECK(::close(root_fd) == 0);
    fault::clear(); fault::Reset reset;
    const auto created = workspace.write_atomic("hello.txt", "hello\n");
    CHECK(created.status == WorkspaceMutationStatus::Succeeded);
    CHECK(!created.error_code);
    CHECK(created.result.as_object().at("created").as_bool());
    CHECK(string_at(created.result, "sha256") == sha256_hex("hello\n"));
    CHECK(created.evidence.as_object().at("durable").as_bool());
    CHECK(fault::file_syncs == 1 && fault::directory_syncs == 1);
    CHECK(host_read(fixture.path / "hello.txt") == "hello\n");
    CHECK(file_count(fixture.path) == 1);
    CHECK(::stat((fixture.path / "hello.txt").c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
}

void cas_and_atomic_replacement() {
    test::TempDir fixture;
    host_write(fixture.path / "target", "old");
    CHECK(::chmod((fixture.path / "target").c_str(), 0750) == 0);
    const int held = ::open((fixture.path / "target").c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(held >= 0);
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    rejected(workspace.write_atomic("target", "new"), ErrorCode::Conflict);
    rejected(workspace.write_atomic("target", "new", sha256_hex("wrong")), ErrorCode::Conflict);
    rejected(workspace.write_atomic("absent", "new", sha256_hex("")), ErrorCode::Conflict);
    rejected(workspace.write_atomic("target", "new", std::string(64, 'z')), ErrorCode::InvalidInput);
    const auto success = workspace.write_atomic("target", "new", sha256_hex("old"));
    CHECK(success.status == WorkspaceMutationStatus::Succeeded);
    CHECK(!success.result.as_object().at("created").as_bool());
    CHECK(string_at(success.result, "previous_sha256") == sha256_hex("old"));
    char previous[3]{};
    CHECK(::read(held, previous, sizeof(previous)) == 3);
    CHECK(std::string_view(previous, 3) == "old");
    struct stat old_info{}; struct stat now{};
    CHECK(::fstat(held, &old_info) == 0);
    CHECK(::stat((fixture.path / "target").c_str(), &now) == 0);
    CHECK(old_info.st_ino != now.st_ino);
    CHECK((now.st_mode & 0777) == 0750);
    CHECK(::close(held) == 0);
    CHECK(host_read(fixture.path / "target") == "new");
    CHECK(file_count(fixture.path) == 1);
}

void paged_and_binary_reads() {
    test::TempDir fixture;
    host_write(fixture.path / "text", "abcdef");
    const std::string binary = std::string("\0", 1) + static_cast<char>(0xff) + "a";
    host_write(fixture.path / "binary", binary);
    host_write(fixture.path / "unicode", "中文");
    Workspace workspace(fixture.path, WorkspaceAccess::ReadOnly);
    const auto first = workspace.read("text", 0, 2);
    CHECK(string_at(first, "content") == "ab");
    CHECK(string_at(first, "content_base64") == "YWI=");
    CHECK(uint_at(first, "next_offset") == 2);
    CHECK(string_at(first, "sha256") == sha256_hex("abcdef"));
    CHECK(string_at(workspace.read("text", 2, 2), "content") == "cd");
    const auto last = workspace.read("text", 4, 2);
    CHECK(last.as_object().at("eof").as_bool());
    CHECK(last.as_object().at("next_offset").is_null());
    CHECK(string_at(workspace.read("text", 6, 2), "content").empty());
    test::throws<Error>([&] { (void)workspace.read("text", 7, 2); });
    const auto raw = workspace.read("binary");
    CHECK(raw.as_object().at("content").is_null());
    CHECK(string_at(raw, "content_base64") == "AP9h");
    CHECK(string_at(raw, "sha256") == sha256_hex(binary));
    CHECK(workspace.read("unicode", 0, 1).as_object().at("content").is_null());
    CHECK(string_at(workspace.read("unicode"), "content") == "中文");
    CHECK(parse_json_strict(canonical_json(raw)) == raw);
}

void paths_and_read_only() {
    test::TempDir fixture;
    host_write(fixture.path / "file", "safe");
    std::filesystem::create_directory(fixture.path / "nested");
    Workspace writable(fixture.path, WorkspaceAccess::ReadWrite);
    Workspace input(fixture.path, WorkspaceAccess::ReadOnly);
    rejected(input.write_atomic("new", "x"), ErrorCode::Unauthorized);
    rejected(input.patch("file", sha256_hex("safe"), 0, "safe", "other"), ErrorCode::Unauthorized);
    for (const std::string path : {"../outside", "nested/../../outside", "/tmp/outside", "nested/../file",
                                  "./file", "nested//file", "nested/", "", ".wls-tmp-forged"}) {
        const auto result = writable.write_atomic(path, "x");
        CHECK(result.status == WorkspaceMutationStatus::EffectNotStarted);
        test::throws<Error>([&] { (void)input.read(path); });
    }
    const std::string nul_path = std::string("file\0extra", 10);
    CHECK(writable.write_atomic(nul_path, "x").status == WorkspaceMutationStatus::EffectNotStarted);
    const auto missing_parent = writable.write_atomic("missing/file", "x");
    CHECK(missing_parent.status == WorkspaceMutationStatus::EffectNotStarted);
    CHECK(!std::filesystem::exists(fixture.path / "missing"));
    CHECK(writable.write_atomic("nested/file", "ok").status == WorkspaceMutationStatus::Succeeded);
    CHECK(host_read(fixture.path / "file") == "safe");
}

void link_and_special_file_rejection() {
    test::TempDir fixture;
    test::TempDir external;
    host_write(external.path / "original", "external");
    std::filesystem::create_directory(fixture.path / "dir");
    std::filesystem::create_symlink(external.path / "original", fixture.path / "symlink");
    std::filesystem::create_directory_symlink(external.path, fixture.path / "redirect");
    CHECK(::link((external.path / "original").c_str(), (fixture.path / "hardlink").c_str()) == 0);
    CHECK(::mkfifo((fixture.path / "fifo").c_str(), 0600) == 0);
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    for (const std::string path : {"symlink", "redirect/original", "hardlink", "fifo", "dir"}) {
        test::throws<Error>([&] { (void)workspace.read(path); });
        CHECK(workspace.write_atomic(path, "attack", sha256_hex("external")).status == WorkspaceMutationStatus::EffectNotStarted);
    }
    test::throws<Error>([&] { (void)workspace.list(); });
    test::throws<Error>([&] { (void)workspace.search(".", "external"); });
    CHECK(workspace.write_atomic("new", "safe").status == WorkspaceMutationStatus::EffectNotStarted);
    CHECK(host_read(external.path / "original") == "external");
    CHECK(!std::filesystem::exists(fixture.path / "new"));
    std::filesystem::create_symlink("/proc/self/fd/0", fixture.path / "magic");
    test::throws<Error>([&] { (void)workspace.read("magic"); });
}

void roots_and_untrusted_permissions() {
    test::TempDir fixture;
    std::filesystem::create_directory(fixture.path / "actual");
    std::filesystem::create_directory(fixture.path / "actual" / "inner");
    std::filesystem::create_directory_symlink(fixture.path / "actual", fixture.path / "alias");
    test::throws<Error>([&] { Workspace rejected_root(fixture.path / "alias", WorkspaceAccess::ReadOnly); });
    test::throws<Error>([&] { Workspace rejected_root(fixture.path / "alias" / "inner", WorkspaceAccess::ReadOnly); });
    test::throws<Error>([&] { Workspace rejected_root(fixture.path / "actual" / ".." / "actual", WorkspaceAccess::ReadOnly); });
    CHECK(::chmod((fixture.path / "actual").c_str(), 0777) == 0);
    test::throws<Error>([&] { Workspace rejected_root(fixture.path / "actual", WorkspaceAccess::ReadWrite); });
    CHECK(::chmod((fixture.path / "actual").c_str(), 0700) == 0);
    host_write(fixture.path / "actual" / "file", "value");
    CHECK(::chmod((fixture.path / "actual" / "file").c_str(), 0666) == 0);
    Workspace workspace(fixture.path / "actual", WorkspaceAccess::ReadWrite);
    test::throws<Error>([&] { (void)workspace.read("file"); });
    CHECK(::chmod((fixture.path / "actual" / "inner").c_str(), 0777) == 0);
    CHECK(workspace.write_atomic("inner/new", "value").status == WorkspaceMutationStatus::EffectNotStarted);
}

void list_cursor_and_bounds() {
    test::TempDir fixture;
    for (const auto* name : {"d", "b", "a", "c"}) host_write(fixture.path / name, name);
    std::filesystem::create_directory(fixture.path / "nested");
    Workspace workspace(fixture.path, WorkspaceAccess::ReadOnly);
    const auto first = workspace.list(".", "", 2);
    CHECK(first.as_object().at("has_more").as_bool());
    CHECK(string_at(first, "next_after_name") == "b");
    CHECK(string_at(first.as_object().at("entries").as_array()[0], "name") == "a");
    const auto second = workspace.list(".", "b", 2);
    CHECK(string_at(second, "next_after_name") == "d");
    const auto last = workspace.list(".", "d", 2);
    CHECK(!last.as_object().at("has_more").as_bool());
    CHECK(last.as_object().at("next_after_name").is_null());
    CHECK(string_at(last.as_object().at("entries").as_array()[0], "type") == "directory");
    CHECK(workspace.list("nested").as_object().at("entries").as_array().empty());
    test::throws<Error>([&] { (void)workspace.list(".", "../bad", 2); });
    test::throws<Error>([&] { (void)workspace.list(".", "", 0); });
    WorkspaceLimits small;
    small.max_entries = 3;
    Workspace limited(fixture.path, WorkspaceAccess::ReadOnly, small);
    test::throws<Error>([&] { (void)limited.list(); });
}

void literal_search_and_explicit_truncation() {
    test::TempDir fixture;
    std::filesystem::create_directory(fixture.path / "nested");
    host_write(fixture.path / "a", "banana\nbanana");
    host_write(fixture.path / "nested" / "b", "other ana");
    Workspace workspace(fixture.path, WorkspaceAccess::ReadOnly);
    const auto all = workspace.search(".", "ana", 10);
    const auto& matches = all.as_object().at("matches").as_array();
    CHECK(matches.size() == 5);
    CHECK(all.as_object().at("complete").as_bool());
    CHECK(uint_at(matches[0], "byte_offset") == 1 && uint_at(matches[1], "byte_offset") == 3);
    CHECK(uint_at(matches[0], "line") == 1 && uint_at(matches[2], "line") == 2);
    CHECK(string_at(matches[4], "path") == "nested/b");
    const auto partial = workspace.search("a", "ana", 2);
    CHECK(partial.as_object().at("matches").as_array().size() == 2);
    CHECK(partial.as_object().at("truncated").as_bool());
    CHECK(!partial.as_object().at("complete").as_bool());
    CHECK(string_at(partial, "limit_reason") == "max_matches");
    CHECK(workspace.search("a", ".*", 10).as_object().at("matches").as_array().empty());
    test::throws<Error>([&] { (void)workspace.search(".", ""); });
    WorkspaceLimits small;
    small.max_search_bytes = 3;
    Workspace limited(fixture.path, WorkspaceAccess::ReadOnly, small);
    test::throws<Error>([&] { (void)limited.search(".", "ana"); });
    small.max_search_bytes = 1000; small.max_search_files = 1;
    Workspace one_file(fixture.path, WorkspaceAccess::ReadOnly, small);
    test::throws<Error>([&] { (void)one_file.search(".", "ana"); });
}

void byte_and_entry_capacity() {
    test::TempDir fixture;
    WorkspaceLimits small;
    small.max_file_bytes = 8; small.max_write_bytes = 8; small.max_read_bytes = 4;
    small.max_total_bytes = 10; small.max_entries = 2;
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite, small);
    CHECK(workspace.write_atomic("a", "12345678").status == WorkspaceMutationStatus::Succeeded);
    rejected(workspace.write_atomic("b", "xyz"), ErrorCode::Quota);
    rejected(workspace.write_atomic("a", "123456789", sha256_hex("12345678")), ErrorCode::Quota);
    CHECK(workspace.write_atomic("a", "12", sha256_hex("12345678")).status == WorkspaceMutationStatus::Succeeded);
    CHECK(workspace.write_atomic("b", "34567890").status == WorkspaceMutationStatus::Succeeded);
    rejected(workspace.write_atomic("c", ""), ErrorCode::Quota);
    test::throws<Error>([&] { (void)workspace.read("a", 0, 5); });
    CHECK(file_count(fixture.path) == 2);
    host_write(fixture.path / "b", "tool-independent-oversize");
    test::throws<Error>([&] { (void)workspace.read("b", 0, 4); });
    rejected(workspace.write_atomic("a", "13", sha256_hex("12")), ErrorCode::Quota);
}

void exact_patch_and_binary_ranges() {
    test::TempDir fixture;
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    CHECK(workspace.write_atomic("file", "foo baz").status == WorkspaceMutationStatus::Succeeded);
    rejected(workspace.patch("file", sha256_hex("foo baz"), 1, "foo", "bar"), ErrorCode::Conflict);
    rejected(workspace.patch("file", sha256_hex("stale"), 4, "baz", "bar"), ErrorCode::Conflict);
    CHECK(workspace.patch("file", sha256_hex("foo baz"), 4, "baz", "bar").status == WorkspaceMutationStatus::Succeeded);
    CHECK(host_read(fixture.path / "file") == "foo bar");
    CHECK(workspace.patch("file", sha256_hex("foo bar"), 3, " ", "! ").status == WorkspaceMutationStatus::Succeeded);
    CHECK(host_read(fixture.path / "file") == "foo! bar");
    rejected(workspace.patch("file", sha256_hex("foo! bar"), std::numeric_limits<std::uint64_t>::max(), "f", "x"), ErrorCode::Conflict);
    CHECK(workspace.patch("file", sha256_hex("foo! bar"), 0, "foo! ", "").status == WorkspaceMutationStatus::Succeeded);
    CHECK(host_read(fixture.path / "file") == "bar");
    const std::string binary("a\0b", 3);
    CHECK(workspace.write_atomic("binary", binary).status == WorkspaceMutationStatus::Succeeded);
    CHECK(workspace.patch("binary", sha256_hex(binary), 1, std::string_view("\0", 1), "z").status == WorkspaceMutationStatus::Succeeded);
    CHECK(host_read(fixture.path / "binary") == "azb");
}

void patch_rejects_nonunique_expected_bytes() {
    test::TempDir fixture;
    host_write(fixture.path / "repeated", "foo foo");
    host_write(fixture.path / "overlap", "aaa");
    const std::string binary("a\0b\0c", 5);
    host_write(fixture.path / "binary", binary);
    host_write(fixture.path / "empty", "");
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    fault::clear(); fault::Reset reset;
    for (const auto offset : {0U, 4U})
        rejected(workspace.patch("repeated", sha256_hex("foo foo"), offset, "foo", "bar"), ErrorCode::Conflict);
    for (const auto offset : {0U, 1U})
        rejected(workspace.patch("overlap", sha256_hex("aaa"), offset, "aa", "b"), ErrorCode::Conflict);
    for (const auto offset : {1U, 3U})
        rejected(workspace.patch("binary", sha256_hex(binary), offset, std::string_view("\0", 1), "z"), ErrorCode::Conflict);
    rejected(workspace.patch("repeated", sha256_hex("foo foo"), 0, "", "x"), ErrorCode::InvalidInput);
    rejected(workspace.patch("empty", sha256_hex(""), 0, "", "x"), ErrorCode::InvalidInput);
    CHECK(host_read(fixture.path / "repeated") == "foo foo");
    CHECK(host_read(fixture.path / "overlap") == "aaa");
    CHECK(host_read(fixture.path / "binary") == binary);
    CHECK(host_read(fixture.path / "empty").empty());
    CHECK(file_count(fixture.path) == 4);
    CHECK(fault::file_syncs == 0 && fault::directory_syncs == 0);
}

void file_sync_failure_is_no_target_effect() {
    test::TempDir fixture;
    host_write(fixture.path / "file", "old");
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    fault::clear(); fault::Reset reset;
    fault::fail_file = true;
    const auto failed = workspace.write_atomic("file", "new", sha256_hex("old"));
    rejected(failed, ErrorCode::Storage);
    CHECK(!failed.evidence.as_object().at("rename_attempted").as_bool());
    CHECK(failed.evidence.as_object().at("temporary_cleanup_complete").as_bool());
    CHECK(host_read(fixture.path / "file") == "old");
    CHECK(file_count(fixture.path) == 1);
    CHECK(fault::file_syncs == 1 && fault::directory_syncs == 1);
}

void directory_sync_failure_is_unknown() {
    test::TempDir fixture;
    host_write(fixture.path / "file", "old");
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    fault::clear(); fault::Reset reset;
    fault::fail_directory = true;
    const auto failed = workspace.write_atomic("file", "new", sha256_hex("old"));
    CHECK(failed.status == WorkspaceMutationStatus::OutcomeUnknown);
    CHECK(failed.error_code == ErrorCode::UnknownOutcome);
    CHECK(failed.evidence.as_object().at("renamed").as_bool());
    CHECK(!failed.evidence.as_object().at("durable").as_bool());
    CHECK(host_read(fixture.path / "file") == "new");
    CHECK(file_count(fixture.path) == 1);
    // The low-level API reports what needs reconciliation; it does not undo or
    // automatically replay the operation using this visible post-error file.
}

void uncertain_cleanup_is_unknown() {
    test::TempDir fixture;
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    fault::clear(); fault::Reset reset;
    fault::fail_file = true; fault::fail_directory = true;
    const auto failed = workspace.write_atomic("new", "value");
    CHECK(failed.status == WorkspaceMutationStatus::OutcomeUnknown);
    CHECK(!failed.evidence.as_object().at("rename_attempted").as_bool());
    CHECK(!failed.evidence.as_object().at("temporary_cleanup_complete").as_bool());
    CHECK(failed.evidence.as_object().contains("cleanup_error"));
    CHECK(!std::filesystem::exists(fixture.path / "new"));
}

void destination_and_parent_rechecked() {
    test::TempDir fixture;
    test::TempDir external;
    host_write(fixture.path / "target", "old");
    host_write(external.path / "safe", "external");
    Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
    fault::clear(); fault::Reset reset;
    fault::after_file_sync = [&] { host_write(fixture.path / "target", "host-change"); };
    rejected(workspace.write_atomic("target", "new", sha256_hex("old")), ErrorCode::Conflict);
    CHECK(host_read(fixture.path / "target") == "host-change");
    CHECK(file_count(fixture.path) == 1);
    fault::after_file_sync = [&] {
        std::filesystem::remove(fixture.path / "target");
        std::filesystem::create_symlink(external.path / "safe", fixture.path / "target");
    };
    const auto swapped = workspace.write_atomic("target", "attack", sha256_hex("host-change"));
    CHECK(swapped.status == WorkspaceMutationStatus::EffectNotStarted);
    CHECK(host_read(external.path / "safe") == "external");
    std::filesystem::remove(fixture.path / "target");
    std::filesystem::create_directory(fixture.path / "parent");
    fault::after_file_sync = [&] {
        std::filesystem::rename(fixture.path / "parent", fixture.path / "moved");
        std::filesystem::create_directory(fixture.path / "parent");
    };
    rejected(workspace.write_atomic("parent/new", "value"), ErrorCode::Conflict);
    CHECK(file_count(fixture.path / "parent") == 0);
    CHECK(file_count(fixture.path / "moved") == 0);
}

void concurrent_cas_between_instances() {
    test::TempDir fixture;
    host_write(fixture.path / "counter", "old");
    Workspace first(fixture.path, WorkspaceAccess::ReadWrite);
    Workspace second(fixture.path, WorkspaceAccess::ReadWrite);
    std::atomic<int> success{0}; std::atomic<int> conflict{0};
    auto call = [&](Workspace& workspace, const char* text) {
        const auto result = workspace.write_atomic("counter", text, sha256_hex("old"));
        if (result.status == WorkspaceMutationStatus::Succeeded) ++success;
        else if (result.status == WorkspaceMutationStatus::EffectNotStarted && result.error_code == ErrorCode::Conflict) ++conflict;
    };
    std::jthread left([&] { call(first, "first"); });
    std::jthread right([&] { call(second, "second"); });
    left.join(); right.join();
    CHECK(success == 1 && conflict == 1);
    const auto bytes = host_read(fixture.path / "counter");
    CHECK(bytes == "first" || bytes == "second");
    CHECK(file_count(fixture.path) == 1);
}

void cross_process_root_lock() {
    test::TempDir fixture;
    host_write(fixture.path / "target", "old");
    std::array<pid_t, 2> children{};
    for (std::size_t index = 0; index < children.size(); ++index) {
        children[index] = ::fork();
        CHECK(children[index] >= 0);
        if (children[index] == 0) {
            try {
                Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite);
                const auto result = workspace.write_atomic("target", std::to_string(index), sha256_hex("old"));
                if (result.status == WorkspaceMutationStatus::Succeeded) ::_exit(0);
                if (result.status == WorkspaceMutationStatus::EffectNotStarted && result.error_code == ErrorCode::Conflict) ::_exit(2);
                ::_exit(4);
            } catch (...) { ::_exit(5); }
        }
    }
    unsigned int success = 0; unsigned int conflicts = 0;
    for (const auto child : children) {
        int status = 0;
        CHECK(::waitpid(child, &status, 0) == child && WIFEXITED(status));
        if (WEXITSTATUS(status) == 0) ++success;
        if (WEXITSTATUS(status) == 2) ++conflicts;
    }
    CHECK(success == 1 && conflicts == 1);
    CHECK(file_count(fixture.path) == 1);
}

void renamed_root_is_rejected() {
    test::TempDir fixture;
    std::filesystem::create_directory(fixture.path / "root");
    Workspace workspace(fixture.path / "root", WorkspaceAccess::ReadWrite);
    std::filesystem::rename(fixture.path / "root", fixture.path / "moved");
    std::filesystem::create_directory(fixture.path / "root");
    rejected(workspace.write_atomic("new", "value"), ErrorCode::Conflict);
    test::throws<Error>([&] { (void)workspace.root_identity(); });
    CHECK(file_count(fixture.path / "root") == 0 && file_count(fixture.path / "moved") == 0);
}

void missing_openat2_fails_closed() {
    test::TempDir fixture;
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        // This child only adds a restriction. No namespace/mount capability or
        // system policy change is required; the parent retains its own filter.
        std::array<sock_filter, 4> filter{{
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned int>(offsetof(seccomp_data, nr))),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned int>(SYS_openat2), 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | static_cast<unsigned int>(ENOSYS)),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
        }};
        sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
        if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
            ::_exit(3);
        try { Workspace workspace(fixture.path, WorkspaceAccess::ReadWrite); ::_exit(4); }
        catch (const Error& error) { ::_exit(error.code() == ErrorCode::Unsupported ? 0 : 5); }
        catch (...) { ::_exit(6); }
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(file_count(fixture.path) == 0);
}
} // namespace

int main() {
    return test::run({
        {"workspace identity, CLOEXEC root descriptor and durable creation", identity_and_creation},
        {"CAS creation and inode-atomic replacement preserve previous open readers", cas_and_atomic_replacement},
        {"paged reads keep full hashes and encode arbitrary bytes losslessly", paged_and_binary_reads},
        {"read-only policy and relative path/parent boundaries", paths_and_read_only},
        {"symlinks, magic links, hard links and special files fail closed", link_and_special_file_rejection},
        {"root ancestors and directory/file ownership permissions are checked", roots_and_untrusted_permissions},
        {"directory cursor pages are sorted and bounded", list_cursor_and_bounds},
        {"literal search preserves byte offsets and reports truncation explicitly", literal_search_and_explicit_truncation},
        {"file, workspace, response and entry capacities reject before effects", byte_and_entry_capacity},
        {"patch binds complete digest and exact byte range including binary data", exact_patch_and_binary_ranges},
        {"patch rejects repeated and overlapping expected bytes before any staging", patch_rejects_nonunique_expected_bytes},
        {"injected file fsync failure cleans staging without target effects", file_sync_failure_is_no_target_effect},
        {"injected directory fsync failure after rename remains UNKNOWN", directory_sync_failure_is_unknown},
        {"injected unprovable staging cleanup remains UNKNOWN", uncertain_cleanup_is_unknown},
        {"destination and parent changes are rechecked before publish", destination_and_parent_rechecked},
        {"two workspace instances serialize a competing CAS", concurrent_cas_between_instances},
        {"cooperating processes share a directory inode advisory lock", cross_process_root_lock},
        {"renamed root is rejected instead of silently adopting its replacement", renamed_root_is_rejected},
        {"real child seccomp denial of openat2 has no weaker fallback", missing_openat2_fails_closed}
    });
}
