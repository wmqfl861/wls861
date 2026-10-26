#include "wls/sandbox.hpp"
#include "test_support.hpp"
#include "../src/sandbox_doctor.inc"

#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <map>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// Fixed fault injection affects only a recorded inode in this test's private
// temporary directory. The production library has no injection hooks.
namespace doctor_fault {
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> device{0};
std::atomic<std::uint64_t> inode{0};
std::atomic<std::uint64_t> calls{0};
std::atomic<std::uint64_t> bytes{0};
std::atomic<int> action{0}; // 1: grow after read; 2: fail read with EIO
std::atomic<int> writer{-1};
std::atomic<std::int64_t> grown_size{0};
std::atomic<int> mutation_error{0};
}

extern "C" ssize_t read(int fd, void* buffer, std::size_t length) {
    bool watched = false;
    if (doctor_fault::enabled.load(std::memory_order_relaxed)) {
        struct stat info{};
        if (::syscall(SYS_fstat, fd, &info) == 0)
            watched = static_cast<std::uint64_t>(info.st_dev) == doctor_fault::device.load() &&
                      static_cast<std::uint64_t>(info.st_ino) == doctor_fault::inode.load();
    }
    const int action = watched ? doctor_fault::action.exchange(0) : 0;
    if (watched) ++doctor_fault::calls;
    if (action == 2) { errno = EIO; return -1; }
    const auto result = static_cast<ssize_t>(::syscall(SYS_read, fd, buffer, length));
    if (watched && result > 0) {
        doctor_fault::bytes.fetch_add(static_cast<std::uint64_t>(result));
        if (action == 1 && ::syscall(SYS_ftruncate, doctor_fault::writer.load(), doctor_fault::grown_size.load()) != 0)
            doctor_fault::mutation_error = errno;
    }
    return result;
}

namespace {
using namespace wls::s1;
using Object = boost::json::object;
using Array = boost::json::array;

class Watch {
public:
    explicit Watch(const std::filesystem::path& path, int action = 0) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        CHECK(fd >= 0);
        struct stat info{};
        CHECK(::fstat(fd, &info) == 0);
        doctor_fault::device = static_cast<std::uint64_t>(info.st_dev);
        doctor_fault::inode = static_cast<std::uint64_t>(info.st_ino);
        doctor_fault::calls = 0;
        doctor_fault::bytes = 0;
        doctor_fault::mutation_error = 0;
        doctor_fault::writer = fd;
        doctor_fault::grown_size = static_cast<std::int64_t>(info.st_size) + 1;
        doctor_fault::action = action;
        doctor_fault::enabled = true;
    }
    ~Watch() { doctor_fault::enabled = false; ::close(doctor_fault::writer.exchange(-1)); }
    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
};

void put(const std::filesystem::path& path, std::string_view contents, mode_t mode = 0600) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
    CHECK(fd >= 0);
    std::size_t at = 0;
    while (at < contents.size()) {
        const auto count = ::write(fd, contents.data() + at, contents.size() - at);
        CHECK(count > 0);
        at += static_cast<std::size_t>(count);
    }
    CHECK(::fchmod(fd, mode) == 0);
    CHECK(::close(fd) == 0);
}

std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

const Json& check(const SandboxDoctor& report, std::string_view id) {
    for (const auto& item : report.checks.as_array())
        if (std::string(item.at("id").as_string()) == id) return item;
    throw std::runtime_error("missing diagnostic check: " + std::string(id));
}

const Json& first_reason(const SandboxDoctor& report, std::string_view id) {
    const auto& reasons = check(report, id).at("reasons").as_array();
    CHECK(!reasons.empty());
    return reasons.front();
}

SandboxPolicy policy_for(const test::TempDir& temp) {
    SandboxPolicy policy;
    policy.bwrap_path = temp.path / "absent-launcher";
    return policy;
}

void add_runtime(SandboxPolicy& policy, const std::filesystem::path& source,
                 std::string destination, std::string_view bytes) {
    policy.runtime_files.push_back({source, std::move(destination), sha256_hex(bytes)});
}

void defaults_are_provenance_and_not_execution() {
    const auto report = sandbox_doctor(SandboxPolicy{});
    const auto json = to_json(report);
    CHECK(json.at("report_format") == "wls-doctor-report-v1");
    CHECK(json.at("inputs").at("launcher").at("path") == "/usr/bin/bwrap");
    CHECK(json.at("inputs").at("launcher").at("origin") == "policy");
    CHECK(json.at("inputs").at("cgroup").at("path").is_null());
    CHECK(json.at("evidence").at("programs_executed") == 0);
    CHECK(json.at("evidence").at("namespace_execution_tested") == false);
    CHECK(!report.strong_prerequisites && !report.delegated_root_ready);
    CHECK(first_reason(report, "runtime.manifest").at("code") == "runtime_manifest_empty");
    CHECK(first_reason(report, "cgroup.root").at("code") == "cgroup_not_configured");
    CHECK(!report.limitations.as_array().empty());
    CHECK(parse_json_strict(canonical_json(json), 2097152).is_object());
}

void independent_files_keep_their_results() {
    test::TempDir temp;
    put(temp.path / "good", "safe bytes");
    auto policy = policy_for(temp);
    add_runtime(policy, temp.path / "missing", "/usr/bin/missing", "missing");
    add_runtime(policy, temp.path / "good", "/usr/bin/good", "safe bytes");
    const auto report = sandbox_doctor(policy);
    CHECK(first_reason(report, "runtime.0").at("code") == "path_missing");
    CHECK(first_reason(report, "runtime.0").at("errno") == ENOENT);
    const auto& good = check(report, "runtime.1");
    CHECK(good.at("status") == "observed_ok");
    CHECK(std::string(good.at("path").as_string()) == (temp.path / "good").string());
    CHECK(good.at("destination") == "/usr/bin/good");
    CHECK(std::string(good.at("observed").at("sha256").as_string()) == sha256_hex("safe bytes"));
    CHECK(good.at("observed").at("bytes_read") == 10);
    CHECK(report.evidence.at("runtime_files").as_array().size() == 1);
    CHECK(!report.runtime_files_ready);
    CHECK(canonical_json(to_json(report)).find("safe bytes") == std::string::npos);
}

void hash_conflict_preserves_actual_observation() {
    test::TempDir temp;
    put(temp.path / "runtime", "original");
    auto policy = policy_for(temp);
    add_runtime(policy, temp.path / "runtime", "/usr/bin/runtime", "different");
    const auto report = sandbox_doctor(policy);
    const auto& item = check(report, "runtime.0");
    CHECK(item.at("status") == "blocked");
    CHECK(item.at("stage") == "hash");
    CHECK(std::string(item.at("expected").at("sha256").as_string()) == sha256_hex("different"));
    CHECK(std::string(item.at("observed").at("sha256").as_string()) == sha256_hex("original"));
    CHECK(item.at("observed").at("identity_verified") == true);
    CHECK(first_reason(report, "runtime.0").at("code") == "hash_mismatch");
    CHECK(first_reason(report, "runtime.0").at("errno").is_null());
}

void symlinks_are_identified_without_following() {
    test::TempDir temp;
    std::filesystem::create_directory(temp.path / "directory");
    CHECK(::chmod((temp.path / "directory").c_str(), 0700) == 0);
    put(temp.path / "directory" / "runtime", "bytes");
    std::filesystem::create_symlink(temp.path / "directory" / "runtime", temp.path / "leaf-link");
    std::filesystem::create_directory_symlink(temp.path / "directory", temp.path / "parent-link");
    auto policy = policy_for(temp);
    add_runtime(policy, temp.path / "leaf-link", "/usr/bin/leaf", "bytes");
    add_runtime(policy, temp.path / "parent-link" / "runtime", "/usr/bin/parent", "bytes");
    const auto report = sandbox_doctor(policy);
    CHECK(first_reason(report, "runtime.0").at("code") == "symlink_refused");
    CHECK(first_reason(report, "runtime.1").at("code") == "symlink_refused");
    CHECK(report.evidence.at("runtime_bytes_read") == 0);
}

void unsafe_permissions_are_observed_and_preserved() {
    test::TempDir temp;
    const auto path = temp.path / "shared";
    put(path, "unsafe", 0666);
    struct stat before{};
    CHECK(::stat(path.c_str(), &before) == 0 && (before.st_mode & 0022) == 0022);
    auto policy = policy_for(temp);
    add_runtime(policy, path, "/usr/bin/shared", "unsafe");
    const auto report = sandbox_doctor(policy);
    CHECK(first_reason(report, "runtime.0").at("code") == "owner_mode_refused");
    CHECK(check(report, "runtime.0").at("observed").at("bytes_read") == 0);
    struct stat after{};
    CHECK(::stat(path.c_str(), &after) == 0 && after.st_mode == before.st_mode);
    CHECK(contents(path) == "unsafe");
}

void directories_and_fifos_cannot_be_runtime_files() {
    test::TempDir temp;
    CHECK(::mkfifo((temp.path / "fifo").c_str(), 0600) == 0);
    auto policy = policy_for(temp);
    add_runtime(policy, temp.path, "/usr/bin/directory", "");
    add_runtime(policy, temp.path / "fifo", "/usr/bin/fifo", "");
    const auto report = sandbox_doctor(policy);
    CHECK(first_reason(report, "runtime.0").at("code") == "file_type_refused");
    CHECK(first_reason(report, "runtime.1").at("code") == "file_type_refused");
    CHECK(report.evidence.at("runtime_bytes_read") == 0);
}

void special_sources_are_rejected_without_open_side_effects() {
    test::TempDir temp;
    const auto fifo = temp.path / "watched-fifo";
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    const int events = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(events >= 0);
    CHECK(::inotify_add_watch(events, fifo.c_str(), IN_OPEN) >= 0);
    auto policy = policy_for(temp);
    policy.bwrap_path = fifo;
    add_runtime(policy, fifo, "/usr/bin/fifo", "");
    const auto report = sandbox_doctor(policy);
    std::array<char, 4096> observed{};
    const auto count = ::read(events, observed.data(), observed.size());
    const auto saved_errno = errno;
    CHECK(::close(events) == 0);
    CHECK(first_reason(report, "launcher.file").at("code") == "file_type_refused");
    CHECK(first_reason(report, "runtime.0").at("code") == "file_type_refused");
    CHECK(report.evidence.at("runtime_bytes_read") == 0);
    // O_RDONLY | O_NONBLOCK still opens a FIFO, allowing a pending writer to
    // proceed. O_PATH inspection must reject it without an IN_OPEN event.
    CHECK(count < 0 && saved_errno == EAGAIN);
}

void declared_zero_size_is_not_proof_of_empty_content() {
    // This fixed kernel observation is regular and size zero according to
    // fstat, but reading it returns the OS type. No user files are sampled.
    const std::filesystem::path source = "/proc/sys/kernel/ostype";
    struct stat observed{};
    CHECK(::stat(source.c_str(), &observed) == 0 && S_ISREG(observed.st_mode));
    CHECK(observed.st_size == 0 && !contents(source).empty());
    test::TempDir temp;
    auto policy = policy_for(temp);
    add_runtime(policy, source, "/usr/lib/ostype", "");
    const auto report = sandbox_doctor(policy);
    CHECK(!report.runtime_files_ready);
    CHECK(first_reason(report, "runtime.0").at("code") == "source_changed");
    CHECK(check(report, "runtime.0").at("observed").at("identity_verified") == false);
    CHECK(check(report, "runtime.0").at("observed").at("bytes_read") == 1);
    CHECK(report.evidence.at("runtime_bytes_read") == 1);
}

void forbidden_destination_does_not_open_source() {
    test::TempDir temp;
    put(temp.path / "runtime", "bytes");
    auto policy = policy_for(temp);
    add_runtime(policy, temp.path / "runtime", "/work/replaced", "bytes");
    Watch watch(temp.path / "runtime");
    const auto report = sandbox_doctor(policy);
    CHECK(first_reason(report, "runtime.0").at("code") == "destination_refused");
    CHECK(doctor_fault::calls == 0);
}

void duplicate_destination_does_not_hide_later_independent_file() {
    test::TempDir temp;
    put(temp.path / "runtime", "x");
    auto policy = policy_for(temp);
    add_runtime(policy, temp.path / "runtime", "/usr/bin/same", "x");
    add_runtime(policy, temp.path / "runtime", "/usr/bin/same", "x");
    add_runtime(policy, temp.path / "runtime", "/usr/bin/other", "x");
    const auto report = sandbox_doctor(policy);
    CHECK(first_reason(report, "runtime.1").at("code") == "destination_duplicate");
    CHECK(check(report, "runtime.2").at("status") == "observed_ok");
    CHECK(report.evidence.at("runtime_bytes_read") == 2);
}

void missing_and_invalid_hashes_do_not_become_approved() {
    test::TempDir temp;
    put(temp.path / "runtime", "x");
    auto policy = policy_for(temp);
    policy.runtime_files.push_back({temp.path / "runtime", "/usr/bin/unpinned", ""});
    const auto unpinned = sandbox_doctor(policy);
    CHECK(unpinned.runtime_files_ready && !unpinned.strong_prerequisites);
    CHECK(first_reason(unpinned, "runtime.0").at("code") == "expected_hash_missing");
    CHECK(std::string(check(unpinned, "runtime.0").at("observed").at("sha256").as_string()) == sha256_hex("x"));
    policy.runtime_files.front().expected_sha256 = "wrong-shape";
    const auto malformed = sandbox_doctor(policy);
    CHECK(first_reason(malformed, "runtime.0").at("code") == "hash_invalid");
    CHECK(malformed.evidence.at("runtime_bytes_read") == 0);
}

void malformed_paths_have_explicit_codes() {
    test::TempDir temp;
    auto policy = policy_for(temp);
    add_runtime(policy, "relative/path", "/usr/bin/relative", "");
    add_runtime(policy, temp.path / ".." / "outside", "/usr/bin/parent", "");
    add_runtime(policy, temp.path / "missing", "/usr/bin/../private", "");
    const auto report = sandbox_doctor(policy);
    for (const auto* id : {"runtime.0", "runtime.1", "runtime.2"})
        CHECK(first_reason(report, id).at("code") == "path_refused");
}

void oversized_source_is_rejected_before_any_read() {
    test::TempDir temp;
    const auto path = temp.path / "sparse";
    put(path, "");
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(::ftruncate(fd, static_cast<off_t>(sandbox_detail::runtime_max_file_bytes + 1)) == 0);
    CHECK(::close(fd) == 0);
    auto policy = policy_for(temp);
    add_runtime(policy, path, "/usr/bin/oversized", "");
    Watch watch(path);
    const auto report = sandbox_doctor(policy);
    CHECK(first_reason(report, "runtime.0").at("code") == "read_budget_exceeded");
    CHECK(check(report, "runtime.0").at("status") == "not_checked");
    CHECK(check(report, "runtime.0").at("observed").at("bytes_read") == 0);
    CHECK(doctor_fault::calls == 0 && doctor_fault::bytes == 0);
    CHECK(!report.complete && !report.strong_prerequisites);
}

void shared_budget_rejects_before_consumption_and_never_wraps() {
    // Fixed small budget exercises the production budget class; this is not
    // a claim that 256 MiB of real runtime files were read in this test.
    sandbox_detail::RuntimeReadBudget budget(7);
    budget.preflight(7);
    budget.charge(3);
    CHECK(budget.consumed() == 3 && budget.remaining() == 4);
    try { budget.preflight(5); CHECK(false); }
    catch (const sandbox_detail::DiagnosticError& error) {
        CHECK(error.reason == "read_budget_exceeded" && error.stage == "read_budget");
    }
    CHECK(budget.consumed() == 3 && budget.remaining() == 4);
    budget.charge(4);
    budget.preflight(0);
    test::throws<sandbox_detail::DiagnosticError>([&] { budget.charge(1); });
    CHECK(budget.consumed() == 7 && budget.remaining() == 0);
}

void changed_inode_bytes_are_rejected_after_actual_read() {
    test::TempDir temp;
    const auto path = temp.path / "changing";
    const std::string data(1024, 'a');
    put(path, data);
    auto policy = policy_for(temp);
    add_runtime(policy, path, "/usr/bin/changing", data);
    Watch watch(path, 1);
    const auto report = sandbox_doctor(policy);
    CHECK(doctor_fault::action == 0 && doctor_fault::mutation_error == 0);
    CHECK(std::filesystem::file_size(path) == 1025);
    CHECK(doctor_fault::bytes == 1024);
    CHECK(first_reason(report, "runtime.0").at("code") == "source_changed");
    CHECK(first_reason(report, "runtime.0").at("errno").is_null());
    CHECK(check(report, "runtime.0").at("observed").at("identity_verified") == false);
    CHECK(!report.runtime_files_ready);
}

void actual_read_failure_preserves_errno_and_continues() {
    test::TempDir temp;
    put(temp.path / "failing", "bad");
    put(temp.path / "good", "ok");
    auto policy = policy_for(temp);
    add_runtime(policy, temp.path / "failing", "/usr/bin/failing", "bad");
    add_runtime(policy, temp.path / "good", "/usr/bin/good", "ok");
    Watch watch(temp.path / "failing", 2);
    const auto report = sandbox_doctor(policy);
    CHECK(doctor_fault::action == 0 && doctor_fault::calls == 1);
    CHECK(first_reason(report, "runtime.0").at("code") == "probe_io_failed");
    CHECK(first_reason(report, "runtime.0").at("errno") == EIO);
    CHECK(check(report, "runtime.0").at("status") == "unavailable");
    CHECK(check(report, "runtime.1").at("status") == "observed_ok");
    CHECK(report.evidence.at("runtime_bytes_read") == 2);
}

void runtime_count_bound_is_explicit_and_bounded() {
    test::TempDir temp;
    put(temp.path / "runtime", "x");
    auto policy = policy_for(temp);
    for (std::size_t i = 0; i < 128; ++i)
        add_runtime(policy, temp.path / "runtime", "/usr/bin/file-" + std::to_string(i), "x");
    const auto at_limit = sandbox_doctor(policy);
    CHECK(at_limit.runtime_files_ready);
    CHECK(at_limit.inputs.at("runtime_files").at("items").as_array().size() == 128);
    CHECK(at_limit.evidence.at("runtime_bytes_read") == 128);
    add_runtime(policy, temp.path / "runtime", "/usr/bin/file-128", "x");
    const auto excess = sandbox_doctor(policy);
    CHECK(!excess.runtime_files_ready && !excess.complete);
    CHECK(excess.inputs.at("runtime_files").at("provided_count") == 129);
    CHECK(excess.inputs.at("runtime_files").at("omitted_count") == 1);
    CHECK(excess.inputs.at("runtime_files").at("items").as_array().size() == 128);
    CHECK(first_reason(excess, "runtime.manifest").at("code") == "policy_unsupported");
}

void unsupported_global_policy_preserves_independent_observation() {
    test::TempDir temp;
    put(temp.path / "runtime", "x");
    auto policy = policy_for(temp);
    policy.memory_max_bytes = 0;
    add_runtime(policy, temp.path / "runtime", "/usr/bin/runtime", "x");
    const auto report = sandbox_doctor(policy);
    CHECK(first_reason(report, "policy.limits").at("code") == "policy_unsupported");
    CHECK(check(report, "runtime.0").at("status") == "observed_ok");
    CHECK(!report.strong_prerequisites);
}

void fake_cgroup_is_not_a_delegated_filesystem() {
    test::TempDir temp;
    put(temp.path / "cgroup.subtree_control", "cpu memory pids\n");
    put(temp.path / "cgroup.type", "domain\n");
    put(temp.path / "cgroup.procs", "original\n");
    auto policy = policy_for(temp);
    policy.cgroup_root = temp.path;
    const auto before = std::distance(std::filesystem::directory_iterator(temp.path), std::filesystem::directory_iterator{});
    const auto report = sandbox_doctor(policy);
    CHECK(first_reason(report, "cgroup.root").at("code") == "not_cgroup_v2");
    CHECK(first_reason(report, "cgroup.cgroup.procs").at("code") == "dependency_not_checked");
    CHECK(std::string(check(report, "cgroup.cgroup.procs").at("path").as_string()) == (temp.path / "cgroup.procs").string());
    CHECK(!report.cgroup_v2 && !report.delegated_root_ready && !report.strong_prerequisites);
    CHECK(contents(temp.path / "cgroup.procs") == "original\n");
    CHECK(std::distance(std::filesystem::directory_iterator(temp.path), std::filesystem::directory_iterator{}) == before);
}

void hierarchy_root_is_not_a_dedicated_delegation() {
    test::TempDir temp;
    for (const auto* path : {"/", "/sys/fs/cgroup"}) {
        auto policy = policy_for(temp);
        policy.cgroup_root = path;
        const auto report = sandbox_doctor(policy);
        CHECK(first_reason(report, "cgroup.root").at("code") == "path_refused");
        CHECK(!report.delegated_root_ready);
    }
}

void pure_cgroup_control_classification_is_not_live_acceptance() {
    // Explicit fixed-input branch checks shared by Cgroup and doctor. No fake
    // directory is promoted to a successful cgroup v2 deployment.
    CHECK(sandbox_detail::missing_controllers("cpu memory pids\n").empty());
    CHECK(sandbox_detail::missing_controllers("memory\n") == std::vector<std::string>({"cpu", "pids"}));
    CHECK(sandbox_detail::missing_controllers("notcpu memoryx pidss\n").size() == 3);
    CHECK(sandbox_detail::cgroup_is_domain("domain\n"));
    CHECK(!sandbox_detail::cgroup_is_domain("domain threaded\n"));
    CHECK(!sandbox_detail::cgroup_is_domain("domain"));
}

void fixed_cgroup_filesystem_mount_and_access_causes_are_exact() {
    // These are injected observations of the same classification branches;
    // no host mount or cgroup delegation is created, repaired or claimed.
    sandbox_detail::require_cgroup_v2(true);
    sandbox_detail::require_cgroup_writable_mount(false);
    sandbox_detail::require_cgroup_access(0, EACCES); // stale errno is ignored
    try { sandbox_detail::require_cgroup_v2(false); CHECK(false); }
    catch (const sandbox_detail::DiagnosticError& error) {
        CHECK(error.reason == "not_cgroup_v2" && !error.number.has_value());
    }
    try { sandbox_detail::require_cgroup_writable_mount(true); CHECK(false); }
    catch (const sandbox_detail::DiagnosticError& error) {
        CHECK(error.reason == "mount_read_only" && error.stage == "mount");
    }
    try { sandbox_detail::require_cgroup_access(-1, EACCES); CHECK(false); }
    catch (const sandbox_detail::DiagnosticError& error) {
        CHECK(error.reason == "delegation_access_unavailable" && error.number == EACCES);
    }
}

void future_operation_paths_are_patterns_only() {
    test::TempDir temp;
    auto policy = policy_for(temp);
    policy.cgroup_root = temp.path;
    const auto report = sandbox_doctor(policy);
    std::size_t operation_count = 0;
    for (const auto& item : report.checks.as_array()) {
        if (item.at("kind") != "operation_control") continue;
        ++operation_count;
        CHECK(item.at("path_kind") == "operation_child_pattern");
        CHECK(item.at("status") == "not_checked");
        CHECK(item.at("required_for_prerequisites") == false);
        CHECK(item.at("observed").as_object().empty());
        CHECK(std::string(item.at("path").as_string()).find("wls-op-<sha256(operation_id)[0:32]>") != std::string::npos);
    }
    CHECK(operation_count == 8);
    CHECK(std::filesystem::is_empty(temp.path));
}

void root_owned_elf_is_observed_without_execution() {
    test::TempDir temp;
    const std::filesystem::path binary = "/usr/bin/true";
    struct stat info{};
    CHECK(::lstat(binary.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == 0);
    auto policy = policy_for(temp);
    policy.bwrap_path = binary;
    policy.expected_bwrap_sha256 = sha256_hex(contents(binary));
    const auto report = sandbox_doctor(policy);
    CHECK(report.bwrap_available);
    CHECK(check(report, "launcher.file").at("status") == "observed_ok");
    CHECK(report.evidence.at("bwrap_version") == "not-executed");
    CHECK(report.evidence.at("programs_executed") == 0);
    CHECK(!report.strong_prerequisites && std::filesystem::is_empty(temp.path));
    policy.expected_bwrap_sha256 = std::string(64, '0');
    const auto mismatch = sandbox_doctor(policy);
    CHECK(!mismatch.bwrap_available);
    CHECK(first_reason(mismatch, "launcher.file").at("code") == "hash_mismatch");
    CHECK(std::string(check(mismatch, "launcher.file").at("observed").at("sha256").as_string()) == sha256_hex(contents(binary)));
}

void launcher_mode_and_format_have_specific_causes() {
    test::TempDir temp;
    auto policy = policy_for(temp);
    // Fixed distribution inputs are read/inspected, never executed or changed.
    policy.bwrap_path = "/usr/include/stdio.h";
    const auto non_executable = sandbox_doctor(policy);
    CHECK(first_reason(non_executable, "launcher.file").at("code") == "launcher_not_executable");
    policy.bwrap_path = "/usr/bin/ldd";
    const auto script = sandbox_doctor(policy);
    CHECK(first_reason(script, "launcher.file").at("code") == "launcher_not_elf");
    CHECK(script.evidence.at("programs_executed") == 0);
}

void marked_launcher_is_not_executed_or_repaired() {
    test::TempDir temp;
    const auto marker = temp.path / "must-not-exist";
    const auto launcher = temp.path / "launcher";
    const auto script = "#!/bin/sh\nprintf executed > '" + marker.string() + "'\n";
    put(launcher, script, 0700);
    auto policy = policy_for(temp);
    policy.bwrap_path = launcher;
    const auto report = sandbox_doctor(policy);
    CHECK(first_reason(report, "launcher.file").at("code") == "owner_mode_refused");
    CHECK(!std::filesystem::exists(marker));
    CHECK(contents(launcher) == script);
    struct stat info{};
    CHECK(::stat(launcher.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
    CHECK(report.evidence.at("programs_executed") == 0);
}

Json synthetic_report() {
    // Synthetic report shape tests output bounds, not observed prerequisites.
    SandboxDoctor report;
    report.linux_supported = true;
    report.strong_prerequisites = true;
    report.evidence = Object{{"programs_executed", 0}, {"namespace_execution_tested", false}};
    return to_json(report);
}

Json synthetic_check(std::size_t id) {
    return Object{{"id", "synthetic." + std::to_string(id)}, {"kind", "synthetic"}, {"path", ""},
        {"stage", "fixed_input"}, {"status", "observed_ok"}, {"required_for_prerequisites", true},
        {"expected", Object{}}, {"observed", Object{}}, {"reasons", Array{}}};
}

void report_check_count_limit_is_fail_closed() {
    auto report = synthetic_report();
    for (std::size_t i = 0; i < 514; ++i) report.at("checks").as_array().push_back(synthetic_check(i));
    bound_sandbox_doctor_report(report);
    CHECK(report.at("checks").as_array().size() == 512);
    CHECK(report.at("report_truncated") == true);
    CHECK(report.at("omitted_check_count") == 2);
    CHECK(report.at("not_checked_count").as_uint64() >= 2);
    CHECK(report.at("complete") == false && report.at("strong_prerequisites") == false);
    CHECK(report.at("report_budget_reason").at("code") == "report_budget_exceeded");
}

void report_reason_and_error_limits_are_explicit() {
    auto report = synthetic_report();
    auto item = synthetic_check(0);
    for (std::size_t i = 0; i < 10; ++i)
        item.at("reasons").as_array().push_back(Object{{"code", "synthetic"}, {"errno", nullptr},
            {"message", std::string(700, 'm')}, {"next_step", "fixed-input test"}});
    report.at("checks").as_array().push_back(std::move(item));
    bound_sandbox_doctor_report(report);
    const auto& check_value = report.at("checks").as_array().front();
    CHECK(check_value.at("reasons").as_array().size() == 8);
    CHECK(check_value.at("omitted_reason_count") == 2);
    for (const auto& explanation : check_value.at("reasons").as_array()) {
        CHECK(explanation.at("message").as_string().size() == 512);
        CHECK(explanation.at("message_truncated") == true);
    }
    CHECK(report.at("report_truncated") == true && report.at("complete") == false);
}

void oversized_cli_provenance_cannot_exceed_report_budget() {
    auto report = synthetic_report();
    report.at("inputs").as_object()["oversized_synthetic_input"] = std::string(2200000, 'x');
    report.at("checks").as_array().push_back(synthetic_check(0));
    bound_sandbox_doctor_report(report);
    CHECK(canonical_json(report).size() <= sandbox_detail::doctor_max_report_bytes);
    CHECK(report.at("inputs").at("omitted_due_to_report_budget") == true);
    CHECK(report.at("evidence").at("programs_executed") == 0);
    CHECK(report.at("report_truncated") == true && report.at("strong_prerequisites") == false);
}

void core_report_truncation_survives_serialization_round_trip() {
    test::TempDir temp;
    auto policy = policy_for(temp);
    std::string tail;
    for (int i = 0; i < 16; ++i) tail += std::string(245, 'a') + "/";
    for (std::size_t i = 0; i < 128; ++i)
        add_runtime(policy, temp.path / "absent" / (tail + "file"), "/usr/bin/" + tail + "file" + std::to_string(i), "");
    const auto result = sandbox_doctor(policy);
    CHECK(result.report_truncated && !result.complete && !result.strong_prerequisites);
    CHECK(result.omitted_check_count > 0);
    const auto report = to_json(result);
    CHECK(report.at("report_truncated") == true);
    CHECK(report.at("report_budget_reason").at("code") == "report_budget_exceeded");
    CHECK(canonical_json(report).size() <= sandbox_detail::doctor_max_report_bytes);
}

void invalid_utf8_expansion_and_long_paths_have_exact_display_flags() {
    test::TempDir temp;
    auto policy = policy_for(temp);
    const auto invalid = temp.path.native() + "/" + std::string(2000, '\xff');
    const auto oversized = temp.path.native() + "/" + std::string(10000, 'a');
    add_runtime(policy, invalid, "/usr/bin/invalid", "");
    add_runtime(policy, oversized, "/usr/bin/oversized-path", "");
    const auto report = sandbox_doctor(policy);
    CHECK(check(report, "runtime.0").at("path_truncated") == true);
    CHECK(check(report, "runtime.0").at("path_display_transformed") == true);
    CHECK(check(report, "runtime.1").at("path_truncated") == true);
    const auto& items = report.inputs.at("runtime_files").at("items").as_array();
    CHECK(items[0].at("source_truncated") == true && items[0].at("source_display_transformed") == true);
    CHECK(items[1].at("source_truncated") == true);
    for (const auto& item : items) CHECK(item.at("source").as_string().size() <= 4096);
    CHECK(parse_json_strict(canonical_json(to_json(report)), 2097152).is_object());
}

void repeated_budget_enforcement_never_restores_truncated_readiness() {
    auto report = synthetic_report();
    report.as_object()["report_truncated"] = true;
    report.as_object()["complete"] = true;
    report.as_object()["strong_prerequisites"] = true;
    bound_sandbox_doctor_report(report);
    CHECK(report.at("report_truncated") == true);
    CHECK(report.at("complete") == false && report.at("strong_prerequisites") == false);
    CHECK(report.at("report_budget_reason").at("code") == "report_budget_exceeded");
}

} // namespace

int main() {
    // Preserve the caller's actual umask; each fixture requests its own mode.
    const auto mask = ::umask(0);
    ::umask(mask);
    std::cout << "[ENV] uid=" << ::geteuid() << " umask=" << std::oct << mask << std::dec << '\n';
    if (::geteuid() == 0) { std::cerr << "doctor regressions require an ordinary user\n"; return 2; }
    const std::vector<test::Case> cases{
        {"default report identifies inputs without execution proof", defaults_are_provenance_and_not_execution},
        {"failed runtime preserves later independent observations", independent_files_keep_their_results},
        {"hash conflict retains expected and observed bytes separately", hash_conflict_preserves_actual_observation},
        {"leaf and ancestor symlinks are rejected with explicit causes", symlinks_are_identified_without_following},
        {"unsafe owner modes remain unchanged and unread", unsafe_permissions_are_observed_and_preserved},
        {"directories and nonblocking FIFOs are not runtime files", directories_and_fifos_cannot_be_runtime_files},
        {"special runtime sources rejected without opening them", special_sources_are_rejected_without_open_side_effects},
        {"zero stat size cannot fabricate an empty runtime digest", declared_zero_size_is_not_proof_of_empty_content},
        {"forbidden destination refuses before source reads", forbidden_destination_does_not_open_source},
        {"duplicate destination does not hide a later independent entry", duplicate_destination_does_not_hide_later_independent_file},
        {"missing or invalid frozen hashes never become approval", missing_and_invalid_hashes_do_not_become_approved},
        {"relative parent and malformed destination paths have stable causes", malformed_paths_have_explicit_codes},
        {"sparse oversized source is rejected before content read", oversized_source_is_rejected_before_any_read},
        {"fixed small shared-budget boundary never consumes rejected bytes", shared_budget_rejects_before_consumption_and_never_wraps},
        {"actual source growth during read fails original identity check", changed_inode_bytes_are_rejected_after_actual_read},
        {"actual injected read failure preserves errno and later files", actual_read_failure_preserves_errno_and_continues},
        {"128-entry bound and omitted entries are explicit", runtime_count_bound_is_explicit_and_bounded},
        {"unsupported global policy retains independent observations", unsupported_global_policy_preserves_independent_observation},
        {"cgroup-shaped ordinary directory cannot prove delegation", fake_cgroup_is_not_a_delegated_filesystem},
        {"filesystem and cgroup hierarchy roots are refused", hierarchy_root_is_not_a_dedicated_delegation},
        {"fixed controller and domain classification is labeled synthetic", pure_cgroup_control_classification_is_not_live_acceptance},
        {"fixed cgroup filesystem mount and access causes are explicit", fixed_cgroup_filesystem_mount_and_access_causes_are_exact},
        {"operation control paths remain uncreated patterns", future_operation_paths_are_patterns_only},
        {"root-owned ELF and hash mismatch are observations only", root_owned_elf_is_observed_without_execution},
        {"launcher mode and ELF failures retain specific reasons", launcher_mode_and_format_have_specific_causes},
        {"marked fake launcher is neither executed nor repaired", marked_launcher_is_not_executed_or_repaired},
        {"report check-count overflow cannot claim completeness", report_check_count_limit_is_fail_closed},
        {"reason and error-text bounds expose truncation", report_reason_and_error_limits_are_explicit},
        {"CLI provenance cannot overflow the final report bound", oversized_cli_provenance_cannot_exceed_report_budget},
        {"core report budget facts survive repeated serialization", core_report_truncation_survives_serialization_round_trip},
        {"invalid UTF8 and oversized paths expose actual display truncation", invalid_utf8_expansion_and_long_paths_have_exact_display_flags},
        {"repeated bounds cannot restore truncated prerequisite readiness", repeated_budget_enforcement_never_restores_truncated_readiness}
    };
    std::size_t failed = 0;
    for (const auto& [name, function] : cases) {
        try { function(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << "RESULT " << cases.size() - failed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
