#pragma once

#include "wls/runtime_types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace wls::s1 {

// Trusted host configuration only. Each mount is one regular runtime file,
// never a host directory tree. Destinations are restricted to runtime paths.
struct SandboxRuntimeFile {
    std::filesystem::path source;
    std::string destination;
    // Required for production run; doctor may observe an unpinned file.
    std::string expected_sha256;
};

struct SandboxPolicy {
    std::filesystem::path bwrap_path{"/usr/bin/bwrap"};
    // Required for production run; never infer an empty hash means approved.
    std::string expected_bwrap_sha256;
    // An already delegated cgroup v2 directory with cpu/memory/pids enabled.
    // The runner never changes host delegation, installs packages or weakens
    // namespace policy. Empty means strong execution is unavailable.
    std::filesystem::path cgroup_root;
    std::vector<SandboxRuntimeFile> runtime_files;
    std::uint64_t memory_max_bytes{268435456};
    std::uint32_t pids_max{64};
    std::uint64_t cpu_quota_us{100000};
    std::uint64_t cpu_period_us{100000};
    std::chrono::milliseconds max_timeout{600000};
    std::chrono::milliseconds cleanup_timeout{2000};
    std::size_t max_output_bytes{1048576};
};

struct ProcessRequest {
    // Host-owned operation identity for deterministic recovery of its cgroup.
    std::string operation_id;
    // argv[0] is an absolute path inside the sandbox; never shell-parsed.
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;
    // Relative to /work. Empty and "." both name the workspace root.
    std::string cwd{"."};
    std::chrono::milliseconds timeout{120000};
    // Combined retained stdout+stderr bound; exceeding it stops the operation.
    std::size_t output_limit_bytes{65536};
};

enum class ProcessStatus {
    Refused, Exited, Signalled, TimedOut, Cancelled, OutputLimit,
    SpawnFailed, CleanupUnknown
};

std::string_view process_status_name(ProcessStatus status) noexcept;

struct ProcessResult {
    ProcessStatus status{ProcessStatus::Refused};
    // spawned means a host child existed, not that a user program started.
    bool spawned{false};
    bool launcher_exec_confirmed{false};
    bool strong_isolation{false};
    bool all_descendants_exited{false};
    bool output_truncated{false};
    std::optional<int> exit_code;
    std::optional<int> signal;
    // Raw bytes. to_json provides valid UTF-8 display fields and lossless
    // *_base64 fields; callers may store these original bytes as BLOBs.
    std::string stdout_text;
    std::string stderr_text;
    std::string error;
    std::string cgroup_name;
    Json evidence = boost::json::object{};
};

struct SandboxDoctor {
    // These are read-only prerequisite observations, not a successful run.
    bool linux_supported{false};
    bool bwrap_available{false};
    bool cgroup_v2{false};
    bool delegated_root_ready{false};
    bool runtime_files_ready{false};
    bool strong_prerequisites{false};
    std::vector<std::string> issues;
    Json evidence = boost::json::object{};
};

Json to_json(const ProcessResult& result);
Json to_json(const SandboxDoctor& result);

SandboxDoctor sandbox_doctor(const SandboxPolicy& policy);

// Both constructors pin and validate the workspace directory. The fd overload
// duplicates its argument; the caller keeps ownership. There is intentionally
// no public unsandboxed/development execution mode.
// run audits at most 20,000 owned entries and 32 path components, refusing
// symlinks, hardlinks, special files and mount boundaries. The final /work
// mount must be private and cannot contain submounts.
class ProcessRunner {
public:
    ProcessRunner(int workspace_root_fd, SandboxPolicy policy);
    ProcessRunner(const std::filesystem::path& workspace_root,
                  SandboxPolicy policy);
    ~ProcessRunner();
    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;
    ProcessRunner(ProcessRunner&&) noexcept;
    ProcessRunner& operator=(ProcessRunner&&) noexcept;

    SandboxDoctor doctor() const;
    ProcessResult run(const ProcessRequest& request,
                      std::stop_token stop = {}) const;

private:
    int workspace_fd_{-1};
    SandboxPolicy policy_;
    mutable std::mutex run_mutex_;
    mutable bool cleanup_unknown_{false};
};

} // namespace wls::s1
