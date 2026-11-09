// Dedicated resolver lifecycle suite for the pinned wls861-resolve-ipv4
// helper. Production discovery is executable-relative only, so every case
// that can reach a child runs inside a staged private copy of this driver
// placed next to the helper under test (controlled fixture or the real
// helper). The orchestrator compares the staged child's RESULT line with the
// required semantics; original-candidate defects are expected to fail here
// until the runtime fix session turns them green.
#define _GNU_SOURCE

#include "https_resolver_internal.hpp"
#include "test_support.hpp"

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using wls::s1::resolver::Observation;
using wls::s1::resolver::cleanup_pending;
using wls::s1::resolver::resolve_ipv4;
using Clock = std::chrono::steady_clock;

const fs::perms program_permissions = fs::perms::owner_read | fs::perms::owner_write |
    fs::perms::owner_exec | fs::perms::group_read | fs::perms::group_exec |
    fs::perms::others_read | fs::perms::others_exec;

fs::path self_executable() {
    char buffer[4096];
    const auto size = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    CHECK(size > 0);
    buffer[size] = '\0';
    return fs::path(buffer);
}

void report(const std::string& extra, bool threw, const Observation& observation) {
    const std::string first = observation.addresses.empty() ? std::string("-") :
        observation.addresses.front();
    bool loopback = false;
    for (const auto& address : observation.addresses)
        loopback = loopback || address == "127.0.0.1";
    std::printf("RESULT %sthrew=%d started=%d ok=%d pending=%d addr=%zu first=%s identity=%d "
                "loopback=%d elapsed_us=%lld\n",
        extra.c_str(), threw ? 1 : 0, observation.started ? 1 : 0, observation.ok ? 1 : 0,
        observation.cleanup_pending ? 1 : 0, observation.addresses.size(), first.c_str(),
        observation.helper_identity.empty() ? 0 : 1, loopback ? 1 : 0,
        static_cast<long long>(observation.elapsed.count()));
}

int run_staged(const std::string& scenario) {
    const auto budget = Clock::now() + std::chrono::seconds(5);
    bool threw = false;
    Observation observation;
    std::string extra;
    try {
        if (scenario == "precancel") {
            std::stop_source source;
            source.request_stop();
            observation = resolve_ipv4("localhost", "443", budget, source.get_token());
        } else if (scenario == "high-fd") {
            std::vector<int> held;
            int descriptor = -1;
            while ((descriptor = ::dup(STDOUT_FILENO)) != -1 && descriptor < 900)
                held.push_back(descriptor);
            const bool raised = descriptor >= 900;
            observation = resolve_ipv4("localhost", "443", budget, {});
            for (const int held_fd : held) ::close(held_fd);
            if (descriptor >= 900) ::close(descriptor);
            extra = raised ? "raised=1 " : "raised=0 ";
        } else if (scenario == "deadline-delay") {
            observation = resolve_ipv4("wls-fault-delay", "443",
                                       Clock::now() + std::chrono::milliseconds(300), {});
        } else if (scenario == "cancel-block") {
            std::stop_source source;
            std::thread stopper([&source] {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                source.request_stop();
            });
            observation = resolve_ipv4("wls-fault-block", "443", budget, source.get_token());
            stopper.join();
        } else if (scenario == "pending-block") {
            const Observation first = resolve_ipv4("wls-fault-block", "443",
                                                   Clock::now() + std::chrono::milliseconds(150), {});
            bool blocked_threw = false;
            try {
                (void)resolve_ipv4("localhost", "443",
                                   Clock::now() + std::chrono::seconds(1), {});
            } catch (const wls::s1::Error&) {
                blocked_threw = true;
            }
            const bool still_pending = cleanup_pending();
            bool recovered = false;
            try {
                const Observation after = resolve_ipv4("localhost", "443",
                                                       Clock::now() + std::chrono::seconds(2), {});
                recovered = after.ok;
            } catch (const wls::s1::Error&) {
                recovered = false;
            }
            std::printf("RESULT p1_pending=%d p2_threw=%d p3_still=%d p4_ok=%d\n",
                        first.cleanup_pending ? 1 : 0, blocked_threw ? 1 : 0,
                        still_pending ? 1 : 0, recovered ? 1 : 0);
            std::fflush(stdout);
            return 0;
        } else if (scenario == "concurrent") {
            std::stop_source cancel_first;
            std::atomic<bool> second_threw{false};
            std::thread blocker([&] {
                try {
                    (void)resolve_ipv4("wls-fault-block", "443", budget, cancel_first.get_token());
                } catch (const wls::s1::Error&) {}
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            try {
                (void)resolve_ipv4("localhost", "443",
                                   Clock::now() + std::chrono::seconds(1), {});
            } catch (const wls::s1::Error&) {
                second_threw = true;
            }
            cancel_first.request_stop();
            blocker.join();
            std::printf("RESULT b_threw=%d\n", second_threw.load() ? 1 : 0);
            std::fflush(stdout);
            return 0;
        } else if (scenario == "wls-probe-mask") {
            // The caller blocks SIGUSR2 around the resolve; the spawned child
            // must arrive with an empty signal mask, not this blocked set.
            sigset_t blocked;
            ::sigemptyset(&blocked);
            ::sigaddset(&blocked, SIGUSR2);
            ::sigprocmask(SIG_BLOCK, &blocked, nullptr);
            observation = resolve_ipv4("wls-probe-mask", "443", budget, {});
            ::sigprocmask(SIG_UNBLOCK, &blocked, nullptr);
        } else {
            // Every remaining scenario resolves its own hostname against the
            // staged helper (fixture scenarios and real-localhost alike).
            observation = resolve_ipv4(scenario, "443", budget, {});
        }
    } catch (const wls::s1::Error&) {
        threw = true;
    }
    report(extra, threw, observation);
    return 0;
}

int run_resolve_once(const std::string& hostname, const std::string& service) {
    bool threw = false;
    Observation observation;
    try {
        observation = resolve_ipv4(hostname, service,
                                   Clock::now() + std::chrono::seconds(5), {});
    } catch (const wls::s1::Error&) {
        threw = true;
    }
    report("", threw, observation);
    std::fflush(stdout);
    return (!threw && observation.ok && !observation.cleanup_pending) ? 0 : 1;
}

struct Capture {
    bool exited_zero = false;
    std::string output;
};

Capture run_process(const fs::path& executable, const std::vector<std::string>& arguments) {
    int channels[2];
    CHECK(::pipe(channels) == 0);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        if (::dup2(channels[1], STDOUT_FILENO) != 0) ::_exit(126);
        ::close(channels[0]);
        ::close(channels[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(channels[1]);
    std::string output;
    char buffer[4096];
    ssize_t size = 0;
    while ((size = ::read(channels[0], buffer, sizeof(buffer))) > 0)
        output.append(buffer, static_cast<std::size_t>(size));
    ::close(channels[0]);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    return {WIFEXITED(status) && WEXITSTATUS(status) == 0, std::move(output)};
}

struct Fields {
    bool exited_zero = false;
    std::map<std::string, std::string> values;
    int number(const std::string& key) const {
        const auto found = values.find(key);
        return found == values.end() ? -1 : std::stoi(found->second);
    }
    std::string text(const std::string& key) const {
        const auto found = values.find(key);
        return found == values.end() ? std::string() : found->second;
    }
};

Fields staged_case(const fs::path& root, const fs::path& helper_source,
                   const std::string& scenario, bool stage_helper,
                   const std::function<void(const fs::path&)>& mutate) {
    static int counter = 0;
    const fs::path directory = root / ("stage-" + std::to_string(++counter));
    fs::create_directory(directory);
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
    const fs::path staged_driver = directory / "driver";
    fs::copy_file(self_executable(), staged_driver);
    fs::permissions(staged_driver, program_permissions, fs::perm_options::replace);
    if (stage_helper) {
        const fs::path staged_helper = directory / "wls861-resolve-ipv4";
        fs::copy_file(helper_source, staged_helper);
        fs::permissions(staged_helper, program_permissions, fs::perm_options::replace);
    }
    if (mutate) mutate(directory);
    const Capture capture = run_process(staged_driver, {"--stage", scenario});
    Fields fields;
    fields.exited_zero = capture.exited_zero;
    std::istringstream stream(capture.output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.rfind("RESULT ", 0) != 0) continue;
        std::istringstream tokens(line.substr(7));
        std::string token;
        while (tokens >> token) {
            const auto equals = token.find('=');
            if (equals != std::string::npos)
                fields.values[token.substr(0, equals)] = token.substr(equals + 1);
        }
    }
    return fields;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--stage") return run_staged(argv[2]);
    if (argc == 4 && std::string(argv[1]) == "--resolve-once")
        return run_resolve_once(argv[2], argv[3]);
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s FIXTURE_HELPER REAL_HELPER | --stage SCENARIO | "
                     "--resolve-once HOSTNAME SERVICE\n", argv[0]);
        return 2;
    }
    const fs::path fixture = fs::absolute(argv[1]);
    const fs::path helper = fs::absolute(argv[2]);
    CHECK(fs::is_regular_file(fixture));
    CHECK(fs::is_regular_file(helper));
    test::TempDir scratch;
    const auto run = [&](const fs::path& helper_source, const std::string& scenario,
                         bool stage_helper = true,
                         const std::function<void(const fs::path&)>& mutate = {}) {
        return staged_case(scratch.path, helper_source, scenario, stage_helper, mutate);
    };
    std::vector<test::Case> cases;

    cases.emplace_back("input_rejects_non_frozen_pairs", [&] {
        const auto past = Clock::now() + std::chrono::seconds(1);
        const std::vector<std::pair<std::string, std::string>> invalid = {
            {"", "443"}, {std::string(254, 'a'), "443"}, {"space host", "443"},
            {"tab\thost", "443"}, {"line\nhost", "443"}, {"/etc/passwd", "443"},
            {"back\\slash", "443"}, {std::string("nul\0byte", 8), "443"},
            {"localhost", ""}, {"localhost", "https"}, {"localhost", "123456"},
        };
        for (const auto& [hostname, service] : invalid)
            test::throws<wls::s1::Error>([&] { (void)resolve_ipv4(hostname, service, past, {}); });
    });
    cases.emplace_back("input_cleanup_pending_idle", [&] { CHECK(!cleanup_pending()); });

    struct Protocol { const char* scenario; bool ok; };
    const std::vector<Protocol> protocols = {
        {"localhost", true},
        {"wls-fault-fail", false},          // A valid fail verdict is a lookup failure, never ok.
        {"wls-fault-fail-extra", false},
        {"wls-fault-malformed", false},
        {"wls-fault-partial", false},
        {"wls-fault-invalid", false},
        {"wls-fault-noncanonical", false},
        {"wls-fault-duplicate", false},
        {"wls-fault-overlimit", false},
        {"wls-fault-count-zero", false},
        {"wls-fault-count-empty", false},
        {"wls-fault-count-nonnum", false},
        {"wls-fault-count-transpose", false},
        {"wls-fault-count-sixtyone", false},
        {"wls-fault-count-leading-zero", false},
        {"wls-fault-extra", false},
        {"wls-fault-truncated", false},
        {"wls-fault-oversize", false},
        {"wls-fault-nonzero", false},
        {"wls-fault-close", false},
    };
    for (const Protocol& protocol : protocols) {
        cases.emplace_back(std::string("protocol_") + protocol.scenario, [&, protocol] {
            const Fields result = run(fixture, protocol.scenario);
            CHECK(result.exited_zero);
            CHECK(result.number("threw") == 0);
            CHECK(result.number("started") == 1);
            CHECK(result.number("ok") == (protocol.ok ? 1 : 0));
            CHECK(result.number("pending") == 0);
            CHECK(result.number("addr") == (protocol.ok ? 1 : 0));
        });
    }
    for (int count = 1; count <= 16; ++count) {
        cases.emplace_back("protocol_wls-count-" + std::to_string(count), [&, count] {
            const Fields result = run(fixture, "wls-count-" + std::to_string(count));
            CHECK(result.exited_zero);
            CHECK(result.number("threw") == 0);
            CHECK(result.number("ok") == 1);
            CHECK(static_cast<std::size_t>(result.number("addr")) ==
                  static_cast<std::size_t>(count));
            CHECK(result.text("first") == "127.0.0.1");
        });
    }

    for (const char* probe : {"wls-probe-argv", "wls-probe-env", "wls-probe-stdio",
                              "wls-probe-fds", "wls-probe-pdeathsig", "wls-probe-nnp",
                              "wls-probe-core", "wls-probe-mask"}) {
        cases.emplace_back(std::string("hygiene_") + probe, [&, probe] {
            const Fields result = run(fixture, probe);
            CHECK(result.exited_zero);
            CHECK(result.number("threw") == 0);
            CHECK(result.number("ok") == 1); // Any leak answers with a non-protocol marker.
        });
    }

    const std::function<void(const fs::path&)> no_mutation;
    cases.emplace_back("identity_helper_missing", [&] {
        const Fields result = run(fixture, "localhost", false, no_mutation);
        CHECK(result.exited_zero);
        CHECK(result.number("threw") == 1);
        CHECK(result.number("started") == 0);
        CHECK(result.number("identity") == 0);
    });
    cases.emplace_back("identity_helper_symlink", [&] {
        const Fields result = run(fixture, "localhost", false, [&](const fs::path& directory) {
            fs::create_symlink(fixture, directory / "wls861-resolve-ipv4");
        });
        CHECK(result.exited_zero);
        CHECK(result.number("threw") == 1);
        CHECK(result.number("started") == 0);
    });
    cases.emplace_back("identity_helper_fifo", [&] {
        const Fields result = run(fixture, "localhost", false, [&](const fs::path& directory) {
            CHECK(::mkfifo((directory / "wls861-resolve-ipv4").c_str(), 0755) == 0);
        });
        CHECK(result.exited_zero);
        CHECK(result.number("threw") == 1);
        CHECK(result.number("started") == 0);
    });
    cases.emplace_back("identity_helper_group_writable", [&] {
        const Fields result = run(fixture, "localhost", true, [&](const fs::path& directory) {
            fs::permissions(directory / "wls861-resolve-ipv4",
                fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                fs::perms::group_write, fs::perm_options::replace);
        });
        CHECK(result.exited_zero);
        CHECK(result.number("threw") == 1);
        CHECK(result.number("started") == 0);
    });
    cases.emplace_back("identity_helper_setuid", [&] {
        const Fields result = run(fixture, "localhost", true, [&](const fs::path& directory) {
            fs::permissions(directory / "wls861-resolve-ipv4",
                fs::perms::set_uid | program_permissions, fs::perm_options::replace);
        });
        CHECK(result.exited_zero);
        CHECK(result.number("threw") == 1);
        CHECK(result.number("started") == 0);
    });
    cases.emplace_back("identity_helper_not_elf", [&] {
        const Fields result = run(fixture, "localhost", true, [&](const fs::path& directory) {
            const fs::path staged_helper = directory / "wls861-resolve-ipv4";
            {
                std::ofstream stream(staged_helper, std::ios::binary | std::ios::trunc);
                stream << "#!/bin/sh\nexit 0\n";
                CHECK(stream.good());
            }
            fs::permissions(staged_helper, program_permissions, fs::perm_options::replace);
        });
        CHECK(result.exited_zero);
        CHECK(result.number("threw") == 1);
        CHECK(result.number("started") == 0);
    });
    cases.emplace_back("identity_staging_directory_loose", [&] {
        const Fields result = run(fixture, "localhost", true, [](const fs::path& directory) {
            fs::permissions(directory, fs::perms::owner_all | fs::perms::group_all |
                fs::perms::others_all, fs::perm_options::replace);
        });
        CHECK(result.exited_zero);
        CHECK(result.number("threw") == 1);
        CHECK(result.number("started") == 0);
    });

    cases.emplace_back("lifecycle_precancel_spawns_nothing", [&] {
        const Fields result = run(fixture, "precancel");
        CHECK(result.exited_zero);
        CHECK(result.number("threw") == 0);
        CHECK(result.number("started") == 0);
        CHECK(result.number("ok") == 0);
        CHECK(result.number("identity") == 0); // Cancelled before pin, open or spawn.
    });
    cases.emplace_back("lifecycle_deadline_never_waits_past_budget", [&] {
        const Fields result = run(fixture, "deadline-delay");
        CHECK(result.exited_zero);
        CHECK(result.number("started") == 1);
        CHECK(result.number("ok") == 0);
        CHECK(result.number("pending") == 0);
        CHECK(result.number("elapsed_us") < 1200000);
    });
    cases.emplace_back("lifecycle_cancel_blocked_child", [&] {
        const Fields result = run(fixture, "cancel-block");
        CHECK(result.exited_zero);
        CHECK(result.number("started") == 1);
        CHECK(result.number("ok") == 0);
    });
    cases.emplace_back("lifecycle_pending_blocks_then_recovers", [&] {
        // SIGKILL usually lands after the single nonblocking waitpid of the
        // already-expired cleanup window, so the bounded kill leaves the child
        // pending. A killable fixture child cannot prove the real unkillable
        // D-state, only this state machine; see the phase report.
        const Fields result = run(fixture, "pending-block");
        CHECK(result.exited_zero);
        CHECK(result.number("p1_pending") == 1);
        CHECK(result.number("p2_threw") == 1); // A pending child blocks new resolves.
        CHECK(result.number("p3_still") == 0); // Bounded cleanup reaps it afterwards.
        CHECK(result.number("p4_ok") == 1);    // The slot recovers without PID reuse.
    });
    cases.emplace_back("lifecycle_concurrent_second_refused", [&] {
        const Fields result = run(fixture, "concurrent");
        CHECK(result.exited_zero);
        CHECK(result.number("b_threw") == 1); // Bounded refusal, never DNS under a lock.
    });
    cases.emplace_back("lifecycle_high_descriptors", [&] {
        const Fields result = run(fixture, "high-fd");
        CHECK(result.exited_zero);
        CHECK(result.number("raised") == 1);
        CHECK(result.number("ok") == 1);
    });
    cases.emplace_back("real_helper_resolves_loopback", [&] {
        const Fields result = run(helper, "real-localhost");
        CHECK(result.exited_zero);
        CHECK(result.number("threw") == 0);
        CHECK(result.number("started") == 1);
        CHECK(result.number("ok") == 1);
        CHECK(result.number("identity") == 1);
        CHECK(result.number("loopback") == 1);
    });

    return test::run(cases);
}
