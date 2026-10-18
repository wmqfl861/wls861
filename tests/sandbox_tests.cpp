#include "wls/sandbox.hpp"
#include "test_support.hpp"

// Exercise exactly the private process supervision implementation using this
// test binary's fixed fixtures. No raw execution function is linked into the
// production API, and these tests never report strong isolation as available.
#define WLS_SANDBOX_SUPERVISOR_TEST_ONLY
#include "../src/sandbox_posix.cpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace wls::s1;

class Temp {
public:
    Temp() {
        std::array<char, 64> path{};
        const std::string pattern = "/tmp/wls-s12-sandbox-test-XXXXXX";
        std::copy(pattern.begin(), pattern.end(), path.begin());
        const auto* made = ::mkdtemp(path.data());
        if (made == nullptr) throw std::runtime_error("mkdtemp failed");
        root = made;
    }
    ~Temp() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::filesystem::path root;
};

void put(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    CHECK(output.good());
}

// Fixed fixture implementation, reached only by this test executable. None of
// these programs opens a network connection or consumes user workspace data.
int fixture(int argc, char** argv) {
    if (argc < 3) return 90;
    const std::string mode = argv[2];
    if (mode == "echo") {
        if (argc != 4) return 91;
        std::cout << argv[3] << '\n';
        std::cout << "explicit=" << (::getenv("ONLY") == nullptr ? "missing" : ::getenv("ONLY")) << '\n';
        std::cout << "canary=" << (::getenv("WLS_SANDBOX_TEST_CANARY") == nullptr ? "absent" : "LEAKED") << '\n';
        std::cerr << "fixture stderr\n";
        return 0;
    }
    if (mode == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return 0;
    }
    if (mode == "flood") {
        const std::string chunk(4096, 'x');
        for (int i = 0; i < 4096; ++i) {
            write_all(i % 2 == 0 ? 1 : 2, chunk);
        }
        return 0;
    }
    if (mode == "exit7") return 7;
    if (mode == "signal") { ::raise(SIGTERM); return 92; }
    if (mode == "fds") {
        int found = 0;
        for (int fd = 3; fd < 1024; ++fd) {
            if (::fcntl(fd, F_GETFD) >= 0) { std::cout << fd << '\n'; ++found; }
        }
        return found == 0 ? 0 : 93;
    }
    if (mode == "binary") {
        write_all(1, std::string("ok\0\xff\xf0", 5));
        write_all(2, "中文");
        return 0;
    }
    if (mode == "gated") {
        write_all(4, "{\"child-pid\":" + std::to_string(::getpid()) + "}\n");
        ::close(4);
        char permit = 0;
        const auto received = ::read(5, &permit, 1);
        ::close(5);
        if (received != 1 || permit != 'x') return 89;
        std::cout << "fixture action started\n";
        return 0;
    }
    if (mode == "escape") {
        int ready[2];
        if (::pipe2(ready, O_CLOEXEC) != 0) return 94;
        const auto child = ::fork();
        if (child < 0) return 95;
        if (child == 0) {
            ::close(ready[0]);
            if (::setsid() < 0) _exit(96);
            if (::write(ready[1], "x", 1) != 1) _exit(97);
            ::close(ready[1]);
            // Independently bounded even if the assertion process is killed.
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            _exit(0);
        }
        ::close(ready[1]);
        char byte = 0;
        if (::read(ready[0], &byte, 1) != 1) return 98;
        ::close(ready[0]);
        std::cout << child << '\n' << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return 0;
    }
    return 99;
}

struct FixtureSpec {
    Fd executable{::open("/proc/self/exe", O_RDONLY | O_CLOEXEC)};
    ChildSpec child;
    FixtureSpec(std::string mode) {
        CHECK(executable.get() >= 0);
        child.executable_fd = executable.get();
        child.argv = {"sandbox-fixture", "--process-fixture", std::move(mode)};
        child.env = {"ONLY=explicit"};
        child.timeout = Milliseconds(1500);
        child.cleanup_timeout = Milliseconds(300);
    }
    SupervisionResult run(std::stop_token stop = {}) {
        return supervise(child, {[](pid_t pid) { if (::kill(-pid, SIGKILL) != 0 && errno != ESRCH) system_error("kill fixture group"); }, {}, false}, stop);
    }
};

void explicit_argv_env_and_dual_output() {
    CHECK(::setenv("WLS_SANDBOX_TEST_CANARY", "not-for-child", 1) == 0);
    FixtureSpec fixture_spec("echo");
    fixture_spec.child.argv.push_back("literal $(no-command) ; * ' ");
    const auto result = fixture_spec.run().process;
    CHECK(::unsetenv("WLS_SANDBOX_TEST_CANARY") == 0);
    CHECK(result.spawned && result.launcher_exec_confirmed);
    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_text == "literal $(no-command) ; * ' \nexplicit=explicit\ncanary=absent\n");
    CHECK(result.stderr_text == "fixture stderr\n");
    CHECK(!result.strong_isolation && !result.all_descendants_exited);
    CHECK(result.evidence.as_object().at("supervision") == "trusted-fixture-only");
}

void no_inherited_descriptors() {
    Temp temp;
    put(temp.root / "private", "private test content");
    Fd secret(::open((temp.root / "private").c_str(), O_RDONLY));
    CHECK(secret.get() >= 0);
    CHECK(::fcntl(secret.get(), F_SETFD, 0) == 0);
    const auto result = FixtureSpec("fds").run().process;
    CHECK(result.status == ProcessStatus::Exited && result.exit_code == 0);
    CHECK(result.stdout_text.empty());
    CHECK(::fcntl(secret.get(), F_GETFD) >= 0);
}

void exit_and_signal() {
    const auto failed = FixtureSpec("exit7").run().process;
    CHECK(failed.status == ProcessStatus::Exited && failed.exit_code == 7);
    const auto signal = FixtureSpec("signal").run().process;
    CHECK(signal.status == ProcessStatus::Signalled && signal.signal == SIGTERM);
    CHECK(signal.evidence.as_object().at("side_effects_undone") == false);
}

void timeout_and_cancellation() {
    FixtureSpec slow("sleep");
    slow.child.timeout = Milliseconds(70);
    auto began = Clock::now();
    const auto timeout = slow.run().process;
    CHECK(timeout.status == ProcessStatus::TimedOut);
    CHECK(timeout.signal == SIGKILL);
    CHECK(Clock::now() - began < std::chrono::seconds(2));
    std::stop_source stopped;
    stopped.request_stop();
    const auto pre = slow.run(stopped.get_token()).process;
    CHECK(pre.status == ProcessStatus::Cancelled && !pre.spawned);
    std::stop_source active;
    std::jthread cancel([&] { std::this_thread::sleep_for(Milliseconds(50)); active.request_stop(); });
    slow.child.timeout = Milliseconds(1500);
    began = Clock::now();
    const auto cancelled = slow.run(active.get_token()).process;
    CHECK(cancelled.status == ProcessStatus::Cancelled);
    CHECK(cancelled.spawned && cancelled.signal == SIGKILL);
    CHECK(Clock::now() - began < std::chrono::seconds(2));
}

void bounded_output() {
    FixtureSpec flood("flood");
    flood.child.output_limit = 1234;
    const auto result = flood.run().process;
    CHECK(result.status == ProcessStatus::OutputLimit);
    CHECK(result.output_truncated);
    CHECK(result.stdout_text.size() + result.stderr_text.size() == 1234);
    CHECK(result.evidence.as_object().at("kill_sent") == true);
    CHECK(!result.all_descendants_exited);
}

void start_gate_requires_host_release() {
    const auto exercise = [](int mode) {
        FixtureSpec fixture_spec("gated");
        if (mode == 3) fixture_spec.child.timeout = Milliseconds(200);
        auto status_pipe = make_pipe();
        auto gate_pipe = make_start_pipe();
        std::stop_source stop;
        CHECK((::fcntl(gate_pipe[0].get(), F_GETFL) & O_NONBLOCK) == 0);
        fixture_spec.child.mappings = {{status_pipe[1].get(), 4}, {gate_pipe[0].get(), 5}};
        bool considered = false;
        const auto gate = [&](std::string_view status, Clock::time_point deadline) {
            CHECK(!stop.stop_requested());
            CHECK(Clock::now() < deadline);
            if (considered || status.find('\n') == std::string_view::npos) return;
            CHECK(parse_json_strict(status).as_object().contains("child-pid"));
            considered = true;
            if (mode == 1) throw Error(ErrorCode::Unauthorized, "fixed fixture attestation rejected");
            if (mode == 2) stop.request_stop();
            if (mode == 3) std::this_thread::sleep_for(Milliseconds(225));
            // A cancellation/deadline reached during attestation cannot
            // release the action even though the control record is valid.
            if (stop.stop_requested() || Clock::now() >= deadline) return;
            CHECK(::write(gate_pipe[1].get(), "x", 1) == 1);
        };
        const auto result = supervise(fixture_spec.child,
            {[](pid_t pid) { if (::kill(-pid, SIGKILL) != 0 && errno != ESRCH) system_error("kill fixture group"); }, {}, false},
            stop.get_token(), status_pipe[0].get(), status_pipe[1].release(), gate).process;
        CHECK(considered);
        CHECK(!result.strong_isolation && !result.all_descendants_exited);
        return result;
    };
    const auto allowed = exercise(0);
    if (allowed.status != ProcessStatus::Exited || allowed.exit_code != 0)
        throw std::runtime_error("gated fixture: " + canonical_json(to_json(allowed)));
    CHECK(allowed.status == ProcessStatus::Exited && allowed.exit_code == 0);
    CHECK(allowed.stdout_text == "fixture action started\n");
    const auto denied = exercise(1);
    CHECK(denied.status == ProcessStatus::SpawnFailed);
    CHECK(denied.stdout_text.empty());
    CHECK(denied.error.find("attestation rejected") != std::string::npos);
    const auto cancelled = exercise(2);
    CHECK(cancelled.status == ProcessStatus::Cancelled && cancelled.stdout_text.empty());
    const auto expired = exercise(3);
    CHECK(expired.status == ProcessStatus::TimedOut && expired.stdout_text.empty());
}

void stale_descendant_query_cannot_confirm_cleanup() {
    FixtureSpec fixture_spec("sleep");
    fixture_spec.child.cleanup_timeout = Milliseconds(80);
    std::size_t queries = 0;
    const auto result = supervise(fixture_spec.child,
        {[](pid_t pid) { if (::kill(-pid, SIGKILL) != 0 && errno != ESRCH) system_error("kill fixture group"); },
         [&]() -> bool {
             if (++queries == 1) return true;
             throw Error(ErrorCode::Storage, "injected final descendant query failure");
         }, true}, {}).process;
    CHECK(queries >= 2);
    CHECK(result.status == ProcessStatus::CleanupUnknown);
    CHECK(!result.all_descendants_exited && !result.strong_isolation);
    CHECK(result.evidence.as_object().at("descendants_proof") == "unconfirmed");
    CHECK(result.error.find("query failure") != std::string::npos);
}

void workspace_mountinfo_rejects_escape_mounts() {
    const std::string top = "10 1 0:9 / / ro,nosuid,nodev - tmpfs tmpfs rw\n";
    const std::string work = "20 10 8:1 /owned /work rw,nosuid,nodev - ext4 /dev/test rw\n";
    verify_workspace_mounts(top + work);
    verify_workspace_mounts(top + work + "22 10 8:1 /other /work-other rw - ext4 /dev/test rw\n");
    for (const auto* nested : {"/work/child", "/work/a\\040b", "/work/a\\134b"}) {
        const auto line = std::string("21 20 8:1 /outside ") + nested + " rw - ext4 /dev/test rw\n";
        test::throws<Error>([&] { verify_workspace_mounts(top + work + line); });
    }
    for (const auto* propagation : {"shared:5", "master:5", "propagate_from:5"}) {
        test::throws<Error>([&] {
            verify_workspace_mounts(top + "20 10 8:1 /owned /work rw,nosuid,nodev " + propagation + " - ext4 /dev/test rw\n");
        });
    }
    test::throws<Error>([&] { verify_workspace_mounts(top); });
    test::throws<Error>([&] { verify_workspace_mounts(top + work + work); });
    test::throws<Error>([&] { verify_workspace_mounts(top + "20 10 8:1 /owned /work rw - ext4 /dev/test rw\n"); });
    test::throws<Error>([&] { verify_workspace_mounts(top + "20 10 8:1 /owned /work rw,nosuid,nodev\n"); });
    test::throws<Error>([&] { verify_workspace_mounts(top + "20 10 8:1 /owned /work/a\\0 rw - ext4 /dev/test rw\n"); });
    test::throws<Error>([&] { verify_workspace_mounts(std::string(262145, 'x')); });
}

void workspace_tree_rejects_unsafe_inodes() {
    Temp temp;
    const auto workspace = temp.root / "work";
    std::filesystem::create_directory(workspace);
    std::filesystem::create_directory(workspace / "dir");
    put(workspace / "dir" / "safe", "safe fixture bytes");
    Fd root(::open(workspace.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    CHECK(root.get() >= 0);
    CHECK(verify_workspace_tree(root.get()) == 2);
    put(temp.root / "outside", "outside fixture bytes");
    const auto unsafe = workspace / "unsafe";
    const auto rejected = [&] {
        test::throws<Error>([&] { (void)verify_workspace_tree(root.get()); });
        std::filesystem::remove(unsafe);
    };
    std::filesystem::create_hard_link(temp.root / "outside", unsafe);
    rejected();
    std::filesystem::create_symlink(temp.root / "outside", unsafe);
    rejected();
    CHECK(::mkfifo(unsafe.c_str(), 0600) == 0);
    rejected();
    // A filesystem socket inode suffices to verify the boundary; this test
    // creates no listener, sends no bytes and performs no network operation.
    CHECK(::mknod(unsafe.c_str(), S_IFSOCK | 0600, 0) == 0);
    rejected();
    std::filesystem::create_directory(unsafe);
    CHECK(::chmod(unsafe.c_str(), 0777) == 0);
    rejected();
    put(unsafe, "unsafe writable fixture");
    CHECK(::chmod(unsafe.c_str(), 0666) == 0);
    rejected();
    auto deep = workspace / "deep";
    for (int i = 0; i < 33; ++i) {
        std::filesystem::create_directory(deep);
        deep /= "d";
    }
    test::throws<Error>([&] { (void)verify_workspace_tree(root.get()); });
    std::filesystem::remove_all(workspace / "deep");
    CHECK(verify_workspace_tree(root.get()) == 2);
    const ProcessRunner runner(workspace, SandboxPolicy{});
    std::filesystem::create_symlink(temp.root / "outside", unsafe);
    ProcessRequest request;
    request.argv = {"/usr/bin/true"};
    const auto refused = runner.run(request);
    CHECK(refused.status == ProcessStatus::Refused && !refused.spawned);
    CHECK(refused.error.find("workspace tree") != std::string::npos);
    std::filesystem::remove(unsafe);
    std::filesystem::remove_all(workspace / "dir");
    for (std::size_t i = 0; i < 20001; ++i) {
        const auto name = std::to_string(i);
        Fd file(::openat(root.get(), name.c_str(), O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0600));
        CHECK(file.get() >= 0);
    }
    test::throws<Error>([&] { (void)verify_workspace_tree(root.get()); });
}

void spawn_and_preexec_failure() {
    Temp temp;
    put(temp.root / "not-executable", "not an ELF or script\n");
    FixtureSpec invalid("echo");
    Fd executable(::open((temp.root / "not-executable").c_str(), O_RDONLY | O_CLOEXEC));
    invalid.child.executable_fd = executable.get();
    const auto bad_exec = invalid.run().process;
    CHECK(bad_exec.status == ProcessStatus::SpawnFailed);
    CHECK(bad_exec.spawned && !bad_exec.launcher_exec_confirmed);
    CHECK(bad_exec.stdout_text.empty());
    CHECK(bad_exec.error.find("stage 11") != std::string::npos);
    FixtureSpec preexec("echo");
    preexec.child.cgroup_procs_fd = executable.get(); // deliberately read-only fixture fd
    const auto bad_join = preexec.run().process;
    CHECK(bad_join.status == ProcessStatus::SpawnFailed);
    CHECK(!bad_join.launcher_exec_confirmed && bad_join.stdout_text.empty());
    CHECK(bad_join.error.find("stage 7") != std::string::npos);
}

void escaped_descendant_remains_unknown() {
    int old = 0;
    CHECK(::prctl(PR_GET_CHILD_SUBREAPER, &old, 0, 0, 0) == 0);
    CHECK(::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0);
    FixtureSpec escaping("escape");
    escaping.child.timeout = Milliseconds(80);
    escaping.child.cleanup_timeout = Milliseconds(90);
    const auto began = Clock::now();
    const auto result = escaping.run().process;
    const auto pid = static_cast<pid_t>(std::stoi(result.stdout_text));
    Fd child_pidfd(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
    CHECK(child_pidfd.get() >= 0);
    CHECK(::syscall(SYS_pidfd_send_signal, child_pidfd.get(), SIGKILL, nullptr, 0) == 0);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(::prctl(PR_SET_CHILD_SUBREAPER, old, 0, 0, 0) == 0);
    CHECK(result.status == ProcessStatus::CleanupUnknown);
    CHECK(!result.all_descendants_exited && !result.strong_isolation);
    CHECK(result.evidence.as_object().at("termination_reason") == "TIMED_OUT");
    CHECK(Clock::now() - began < std::chrono::seconds(2));
}

void binary_output_is_lossless_json() {
    const auto result = FixtureSpec("binary").run().process;
    CHECK(result.stdout_text == std::string("ok\0\xff\xf0", 5));
    const auto json = to_json(result);
    CHECK(json.as_object().at("stdout_base64") == "b2sA//A=");
    CHECK(json.as_object().at("stderr_text") == "中文");
    CHECK(parse_json_strict(canonical_json(json)).is_object());
}

void policy_and_paths_are_bounded() {
    Temp temp;
    std::filesystem::create_directory(temp.root / "work");
    std::filesystem::create_directory(temp.root / "outside");
    std::filesystem::create_directory_symlink(temp.root / "outside", temp.root / "work" / "link");
    SandboxPolicy policy;
    ProcessRunner runner(temp.root / "work", policy);
    ProcessRequest request;
    request.argv = {"/usr/bin/true"};
    for (const auto* path : {"../outside", "/tmp", "link", "link/../.", "a//b"}) {
        request.cwd = path;
        const auto result = runner.run(request);
        CHECK(result.status == ProcessStatus::Refused && !result.spawned);
    }
    request.cwd = ".";
    request.argv = {"/usr/bin/true", std::string("bad\0arg", 7)};
    CHECK(runner.run(request).status == ProcessStatus::Refused);
    request.argv = {"relative-program"};
    CHECK(runner.run(request).status == ProcessStatus::Refused);
    request.argv = {"/usr/bin/true"};
    request.env = {{"LD_PRELOAD", "/work/untrusted.so"}};
    CHECK(runner.run(request).status == ProcessStatus::Refused);
    request.env = {{"HOME", "/host"}};
    CHECK(runner.run(request).status == ProcessStatus::Refused);
    request.env.clear();
    request.timeout = policy.max_timeout + Milliseconds(1);
    CHECK(runner.run(request).status == ProcessStatus::Refused);
    request.timeout = Milliseconds(10);
    request.output_limit_bytes = policy.max_output_bytes + 1;
    CHECK(runner.run(request).status == ProcessStatus::Refused);
    test::throws<Error>([&] { ProcessRunner invalid(temp.root / "work" / "link", policy); });
    test::throws<Error>([&] { ProcessRunner invalid(std::filesystem::path("/"), policy); });
    SandboxPolicy dangerous;
    dangerous.runtime_files.push_back({"/etc/passwd", "/work/replaced", ""});
    test::throws<Error>([&] { ProcessRunner invalid(temp.root, dangerous); });
}

void doctor_and_production_refuse_without_capabilities() {
    Temp temp;
    SandboxPolicy policy;
    policy.bwrap_path = temp.root / "missing-bwrap";
    const auto before = std::distance(std::filesystem::directory_iterator(temp.root), std::filesystem::directory_iterator{});
    const auto doctor = sandbox_doctor(policy);
    CHECK(!doctor.bwrap_available && !doctor.strong_prerequisites);
    CHECK(!doctor.issues.empty());
    CHECK(doctor.evidence.as_object().at("programs_executed") == 0);
    CHECK(doctor.evidence.as_object().at("namespace_execution_tested") == false);
    CHECK(std::distance(std::filesystem::directory_iterator(temp.root), std::filesystem::directory_iterator{}) == before);
    ProcessRunner runner(temp.root, policy);
    ProcessRequest request;
    request.argv = {"/usr/bin/true"};
    const auto result = runner.run(request);
    CHECK(result.status == ProcessStatus::Refused && !result.spawned);
    CHECK(!result.strong_isolation && !result.all_descendants_exited);
    CHECK(result.error.find("strong execution requires") != std::string::npos);
    CHECK(parse_json_strict(canonical_json(to_json(doctor))).is_object());
    // A regular directory with cgroup-shaped text files is not cgroup v2.
    policy.cgroup_root = temp.root;
    put(temp.root / "cgroup.controllers", "cpu memory pids\n");
    put(temp.root / "cgroup.subtree_control", "cpu memory pids\n");
    put(temp.root / "cgroup.type", "domain\n");
    const auto forged = sandbox_doctor(policy);
    CHECK(!forged.cgroup_v2 && !forged.delegated_root_ready && !forged.strong_prerequisites);
    put(temp.root / "runtime", "trusted runtime");
    policy.runtime_files.push_back({temp.root / "runtime", "/usr/bin/fixture", ""});
    ProcessRunner unpinned(temp.root, policy);
    const auto absent_hash = unpinned.run(request);
    CHECK(!absent_hash.spawned && absent_hash.status == ProcessStatus::Refused);
    CHECK(absent_hash.error.find("frozen SHA-256") != std::string::npos);
    policy.expected_bwrap_sha256 = std::string(64, '0');
    ProcessRunner unpinned_runtime(temp.root, policy);
    CHECK(unpinned_runtime.run(request).error.find("frozen SHA-256") != std::string::npos);
}

void runtime_files_are_frozen_and_scoped() {
    Temp temp;
    const auto original = temp.root / "runtime";
    put(original, "original runtime bytes");
    const auto digest = sha256_hex("original runtime bytes");
    auto frozen = pin_file(original, digest, false, true);
    CHECK(frozen.sha256 == digest);
    std::filesystem::rename(original, temp.root / "renamed");
    put(original, "replacement");
    put(temp.root / "renamed", "changed inode bytes");
    std::array<char, 64> output{};
    const auto count = ::read(frozen.fd.get(), output.data(), output.size());
    CHECK(count == 22);
    CHECK(std::string(output.data(), static_cast<std::size_t>(count)) == "original runtime bytes");
    CHECK(::write(frozen.fd.get(), "x", 1) < 0 && errno == EPERM);
    test::throws<Error>([&] { (void)pin_file(original, digest, false, true); });
    std::filesystem::create_symlink(original, temp.root / "symlink");
    test::throws<Error>([&] { (void)pin_file(temp.root / "symlink", "", false, true); });
    test::throws<Error>([&] { (void)pin_file(temp.root, "", false, true); });
    test::throws<Error>([&] { (void)pin_file(original, "", true, false); });
    for (const auto* path : {"/", "/home/private", "/proc/self/mem", "/etc/shadow", "/work/library", "/usr/bin/../secret"})
        test::throws<Error>([&] { validate_destination(path); });
    validate_destination("/usr/bin/runtime");
    validate_destination("/lib/x86_64-linux-gnu/libc.so.6");
}

void cooperative_workspace_lock() {
    Temp temp;
    Fd directory(::open(temp.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    CHECK(directory.get() >= 0);
    CHECK(::flock(directory.get(), LOCK_EX | LOCK_NB) == 0);
    ProcessRunner runner(temp.root, SandboxPolicy{});
    ProcessRequest request;
    request.argv = {"/usr/bin/true"};
    const auto busy = runner.run(request);
    CHECK(busy.status == ProcessStatus::Refused && !busy.spawned);
    CHECK(busy.error.find("workspace is busy") != std::string::npos);
    CHECK(::flock(directory.get(), LOCK_UN) == 0);
    const auto unavailable = runner.run(request);
    CHECK(unavailable.status == ProcessStatus::Refused && !unavailable.spawned);
    CHECK(::flock(directory.get(), LOCK_EX | LOCK_NB) == 0);
    CHECK(::flock(directory.get(), LOCK_UN) == 0);
}

void doctor_real_environment_is_observation_only() {
    const auto doctor = sandbox_doctor(SandboxPolicy{});
    CHECK(doctor.linux_supported);
    CHECK(!doctor.strong_prerequisites); // no manifest/delegated root configured
    CHECK(doctor.evidence.as_object().at("programs_executed") == 0);
    std::cout << "[OBSERVED] " << canonical_json(to_json(doctor)) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--process-fixture") return fixture(argc, argv);
    return test::run({
        {"trusted fixture preserves literal argv, explicit env and both output streams", explicit_argv_env_and_dual_output},
        {"exec closes inherited host descriptors", no_inherited_descriptors},
        {"nonzero exit and signals retain distinct outcome evidence", exit_and_signal},
        {"timeout and stop_token cancellation are bounded", timeout_and_cancellation},
        {"combined output cap stops a continuously writing child", bounded_output},
        {"blocked fixture action starts only after the host releases its gate", start_gate_requires_host_release},
        {"exec and pre-exec containment failures cannot run fixture code", spawn_and_preexec_failure},
        {"setsid descendant escape stays UNKNOWN in supervisor-only fixture", escaped_descendant_remains_unknown},
        {"failed descendant query cannot reuse an earlier empty observation", stale_descendant_query_cannot_confirm_cleanup},
        {"mountinfo rejects nested and propagating workspace mounts", workspace_mountinfo_rejects_escape_mounts},
        {"workspace audit rejects unsafe inodes, modes and traversal overflow", workspace_tree_rejects_unsafe_inodes},
        {"binary output retains exact bytes and valid JSON", binary_output_is_lossless_json},
        {"production policy, arguments and cwd reject unsafe boundaries", policy_and_paths_are_bounded},
        {"doctor is read-only and production refuses absent or forged isolation", doctor_and_production_refuse_without_capabilities},
        {"runtime hash pins sealed bytes and restricts mount destinations", runtime_files_are_frozen_and_scoped},
        {"workspace flock serializes file and process ownership", cooperative_workspace_lock},
        {"actual environment doctor does not execute programs or claim strong test", doctor_real_environment_is_observation_only}
    });
}
