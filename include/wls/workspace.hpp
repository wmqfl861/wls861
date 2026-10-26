#pragma once

#include "wls/runtime_types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace wls::s1 {

enum class WorkspaceAccess { ReadOnly, ReadWrite };
enum class WorkspaceMutationStatus { Succeeded, EffectNotStarted, OutcomeUnknown };

struct WorkspaceLimits {
    std::uint64_t max_file_bytes{16U * 1024U * 1024U};
    std::uint64_t max_total_bytes{256U * 1024U * 1024U};
    std::size_t max_read_bytes{1024U * 1024U};
    std::size_t max_write_bytes{16U * 1024U * 1024U};
    std::size_t max_entries{20000};
    std::size_t max_list_entries{1000};
    std::size_t max_search_files{1000};
    std::uint64_t max_search_bytes{16U * 1024U * 1024U};
    std::size_t max_search_matches{1000};
    std::size_t max_path_bytes{4096};
    std::size_t max_depth{32};
};

struct WorkspaceMutationResult {
    WorkspaceMutationStatus status{WorkspaceMutationStatus::EffectNotStarted};
    std::optional<ErrorCode> error_code;
    Json evidence{boost::json::object{}};
    Json result{boost::json::object{}};
};

std::string_view workspace_mutation_status_name(WorkspaceMutationStatus status) noexcept;

// Linux-only, descriptor-anchored workspace. Construction requires openat2 and
// the BENEATH/NO_SYMLINKS/NO_MAGICLINKS/NO_XDEV resolution guarantees. There is no
// fallback to a weaker resolver. Callers must obtain Store authorization before
// invoking this low-level host API and persist the returned outcome afterwards.
// flock coordinates cooperating Workspace instances/processes, not arbitrary
// same-UID host code that can rename directories or ignore the lock.
class Workspace {
public:
    Workspace(const std::filesystem::path& root, WorkspaceAccess access,
              WorkspaceLimits limits = {});
    ~Workspace();
    Workspace(Workspace&&) noexcept;
    Workspace& operator=(Workspace&&) noexcept;
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    // {"path": canonical host path, "device": uint64, "inode": uint64}.
    Json root_identity() const;
    // The caller owns the returned descriptor. FD_CLOEXEC is always set.
    int dup_root_fd() const;

    // Bytes are represented losslessly by content_base64. Valid UTF-8 also has
    // a content field; otherwise content is null. sha256 identifies the complete
    // bounded file, even when only an offset/page is returned.
    Json read(std::string_view path, std::uint64_t offset = 0,
              std::size_t max_bytes = 65536) const;
    // Sorted name cursor pagination; pages are separate live observations, not
    // a stable multi-call snapshot. Unsafe entries make the operation fail.
    Json list(std::string_view path = ".", std::string_view after_name = "",
              std::size_t limit = 100) const;
    // Literal byte search. Offset and line are byte offset / one-based line.
    // Bounds fail explicitly rather than silently skipping unsearched files.
    Json search(std::string_view path, std::string_view literal,
                std::size_t max_matches = 100) const;

    // Create one empty directory with mode 0700. The parent must exist and the
    // destination must be absent. Any failure after the mkdir attempt is UNKNOWN;
    // this invocation never removes a possibly published directory on failure.
    WorkspaceMutationResult mkdir(std::string_view path);

    // Missing expected_sha256 authorizes creation only. Existing destinations
    // require the full old SHA-256. Parent directories must already exist.
    WorkspaceMutationResult write_atomic(
        std::string_view path, std::string_view bytes,
        std::optional<std::string> expected_sha256 = std::nullopt);
    // expected_bytes must be nonempty and occur exactly once in the whole old
    // file, at byte_offset. Overlapping matches also make a patch ambiguous.
    WorkspaceMutationResult patch(
        std::string_view path, std::string_view expected_sha256,
        std::uint64_t byte_offset, std::string_view expected_bytes,
        std::string_view replacement);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wls::s1
