// Internal read-only diagnostic primitives shared by the real checks and
// focused fixed-input tests. No execution, Store or host-mutation API lives here.
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wls::s1::sandbox_detail {

inline constexpr std::size_t doctor_max_checks = 512;
inline constexpr std::size_t doctor_max_reasons = 8;
inline constexpr std::size_t doctor_max_error_bytes = 512;
inline constexpr std::size_t doctor_max_report_bytes = 2097152;
inline constexpr std::uint64_t runtime_max_file_bytes = 67108864;
inline constexpr std::uint64_t runtime_max_total_bytes = 268435456;

class DiagnosticError : public Error {
public:
    DiagnosticError(ErrorCode code, std::string reason_value, std::string stage_value,
                    std::string message, std::optional<int> number_value = std::nullopt)
        : Error(code, message), reason(std::move(reason_value)), stage(std::move(stage_value)), number(number_value) {}
    std::string reason;
    std::string stage;
    std::optional<int> number;
};

[[noreturn]] inline void diagnostic_error(ErrorCode code, std::string_view reason,
        std::string_view stage, const std::string& message,
        std::optional<int> number = std::nullopt) {
    throw DiagnosticError(code, std::string(reason), std::string(stage), message, number);
}

inline std::string_view errno_reason(int number) {
    if (number == ENOENT) return "path_missing";
    if (number == EACCES || number == EPERM) return "path_unreadable";
    if (number == ELOOP) return "symlink_refused";
    if (number == ENOTDIR || number == EISDIR) return "file_type_refused";
    return "probe_io_failed";
}

inline std::string_view next_step(std::string_view code) {
    if (code == "expected_hash_missing" || code == "hash_mismatch" || code == "hash_invalid")
        return "Verify the intended file and its separately approved frozen SHA-256.";
    if (code == "source_changed") return "Inspect the changing source and repeat this read-only check after it is stable.";
    if (code == "runtime_manifest_empty" || code == "manifest_not_configured")
        return "Provide an explicit trusted host manifest with the required runtime files and hashes.";
    if (code == "cgroup_not_configured") return "Select an already delegated dedicated cgroup v2 subtree.";
    if (code == "controller_missing") return "Ask the deployment owner to verify the listed controllers in the selected subtree.";
    if (code == "not_domain") return "Verify that the selected delegated cgroup has domain type.";
    if (code == "not_cgroup_v2") return "Verify the filesystem of the explicitly selected cgroup root.";
    if (code == "mount_read_only" || code == "delegation_access_unavailable")
        return "Ask the deployment owner to verify writable delegation for the current runtime identity.";
    if (code == "read_budget_exceeded" || code == "report_budget_exceeded")
        return "Reduce the explicit input to the documented bounds before repeating the check.";
    if (code == "dependency_not_checked") return "Resolve the referenced prerequisite before checking this dependent object.";
    if (code == "operation_not_created") return "Verify these controls during a separately authorized real isolated operation.";
    if (code == "destination_refused" || code == "destination_duplicate")
        return "Correct the explicit runtime destination while retaining the fixed runtime whitelist.";
    if (code == "policy_unsupported" || code == "path_refused")
        return "Correct the explicit input to the existing executor policy and path rules.";
    return "Inspect the named source, its ownership and permissions; this command does not repair them.";
}

inline std::string bounded_message(std::string_view text) {
    std::size_t length = std::min(text.size(), doctor_max_error_bytes);
    // Messages generated here are ASCII; preserve complete UTF-8 boundaries
    // if a system message was translated by the host locale.
    if (length < text.size()) {
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xc0U) == 0x80U) --length;
    }
    return std::string(text.substr(0, length));
}

inline boost::json::object reason(std::string_view code, std::string_view message,
                                 std::optional<int> number = std::nullopt) {
    return {{"code", code}, {"errno", number ? Json(*number) : Json(nullptr)},
            {"message", bounded_message(message)}, {"message_truncated", message.size() > doctor_max_error_bytes},
            {"next_step", next_step(code)}};
}

// The budget counts bytes actually read, including reads of a source which
// later fails its hash/identity check. A too-large declared file is rejected
// before reading any of its content. Tests may use small budgets directly.
class RuntimeReadBudget {
public:
    explicit RuntimeReadBudget(std::uint64_t maximum) : maximum_(maximum) {}
    void preflight(std::uint64_t size) const {
        if (size > remaining())
            diagnostic_error(ErrorCode::Quota, "read_budget_exceeded", "read_budget",
                             "runtime file exceeds the remaining shared read budget");
    }
    void charge(std::uint64_t count) {
        preflight(count);
        consumed_ += count;
    }
    std::uint64_t consumed() const noexcept { return consumed_; }
    std::uint64_t remaining() const noexcept { return maximum_ - consumed_; }
private:
    std::uint64_t maximum_;
    std::uint64_t consumed_{0};
};

inline std::vector<std::string> missing_controllers(std::string_view text) {
    std::vector<std::string> words;
    std::istringstream input{std::string(text)};
    for (std::string word; input >> word;) words.push_back(std::move(word));
    std::vector<std::string> missing;
    for (const auto* required : {"cpu", "memory", "pids"})
        if (std::find(words.begin(), words.end(), required) == words.end()) missing.emplace_back(required);
    return missing;
}

inline bool cgroup_is_domain(std::string_view text) { return text == "domain\n"; }

inline void require_cgroup_v2(bool observed) {
    if (!observed)
        diagnostic_error(ErrorCode::Unsupported, "not_cgroup_v2", "filesystem", "configured root is not cgroup v2");
}

inline void require_cgroup_writable_mount(bool read_only) {
    if (read_only)
        diagnostic_error(ErrorCode::Unsupported, "mount_read_only", "mount", "cgroup root mount is read-only");
}

inline void require_cgroup_access(int result, int error_number) {
    if (result != 0)
        diagnostic_error(ErrorCode::Unsupported, "delegation_access_unavailable", "access",
            "cgroup access is not delegated to the current runtime identity", error_number);
}

} // namespace wls::s1::sandbox_detail
