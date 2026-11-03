#include "readonly_internal.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace wls::native {
namespace system {
void require_readonly_policy_shape(const WindowsReadOnlyPolicy& policy) {
    const auto& reference = policy.approval_reference;
    if (reference.empty() || reference.size() > 256 ||
        std::any_of(reference.begin(), reference.end(), [](unsigned char value) { return value < 32 || value == 127; }) ||
        policy.approved_builds.empty() || policy.approved_builds.size() > 32 ||
        policy.trusted_owner_and_writer_sids.empty() || policy.trusted_owner_and_writer_sids.size() > 32)
        throw Error(ErrorCode::Denied, "POLICY_PENDING: explicit bounded build and SID policy is required");
    const std::set<std::uint32_t> builds(policy.approved_builds.begin(), policy.approved_builds.end());
    const std::set<std::string> sids(policy.trusted_owner_and_writer_sids.begin(), policy.trusted_owner_and_writer_sids.end());
    if (builds.size() != policy.approved_builds.size() || *builds.begin() < 22000 ||
        sids.size() != policy.trusted_owner_and_writer_sids.size())
        throw Error(ErrorCode::Denied, "read-only policy contains unsupported or duplicate entries");
    (void)utf8_to_utf16(reference);
    for (const auto& sid : sids) {
        if (!sid.starts_with("S-1-") || sid.size() > 184 ||
            std::any_of(sid.begin() + 4, sid.end(), [](char value) {
                return (value < '0' || value > '9') && value != '-';
            }))
            throw Error(ErrorCode::Denied, "read-only policy requires explicit canonical SID strings");
        // The current token's administrator membership and the resource
        // owner/writer trust set are separate boundaries. This restricted
        // profile cannot trust the broad Administrators group, even when the
        // caller explicitly lists it. Native SID canonicalization still runs
        // before any descriptor is accepted.
        if (sid == "S-1-5-32-544")
            throw Error(ErrorCode::Denied, "Builtin Administrators cannot be a trusted owner/writer");
    }
}
#ifndef _WIN32
std::unique_ptr<WindowsPublicDirectory> open_windows_readonly_directory(
    const std::filesystem::path&, const WindowsReadOnlyPolicy&) {
    throw Error(ErrorCode::Unsupported, "Windows read-only identity requires native Windows; no fallback adapter");
}
#endif
} // namespace system

struct ReadOnlyRoot::Impl {
    std::unique_ptr<system::WindowsPublicDirectory> directory;
};
ReadOnlyRoot::ReadOnlyRoot(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ReadOnlyRoot::~ReadOnlyRoot() = default;
ReadOnlyRoot::ReadOnlyRoot(ReadOnlyRoot&&) noexcept = default;
ReadOnlyRoot& ReadOnlyRoot::operator=(ReadOnlyRoot&&) noexcept = default;

ReadOnlyRoot ReadOnlyRoot::open(const std::filesystem::path& root, const WindowsReadOnlyPolicy& policy) {
    system::require_readonly_policy_shape(policy);
    const auto spelling = path_to_utf8(root);
    if (!root.is_absolute() || spelling.empty() || spelling.size() > 4096 ||
        spelling.find('\0') != std::string::npos || root != root.lexically_normal())
        throw Error(ErrorCode::InvalidInput, "read-only root must be a bounded normalized absolute path");
    auto impl = std::make_unique<Impl>();
    impl->directory = system::open_windows_readonly_directory(root, policy);
    return ReadOnlyRoot(std::move(impl));
}

const WindowsObjectObservation& ReadOnlyRoot::identity() const {
    if (!impl_) throw Error(ErrorCode::Denied, "read-only root has been moved");
    return impl_->directory->root_observation();
}

ReadOnlyPublicCapture ReadOnlyRoot::capture_registered() const {
    if (!impl_) throw Error(ErrorCode::Denied, "read-only root has been moved");
    ReadOnlyPublicCapture result;
    auto bundle = system::capture_registered_public_bundle([&](std::string_view name, std::size_t maximum) {
        auto captured = impl_->directory->read_observed(name, maximum);
        if (!result.observations_.emplace(name, std::move(captured.observation)).second)
            throw Error(ErrorCode::Mismatch, "duplicate read-only resource observation");
        return std::move(captured.bytes);
    });
    result.assets_ = std::move(bundle.assets);
    result.total_bytes_ = bundle.total_bytes;
    result.manifest_digest_ = std::move(bundle.manifest_digest);
    result.policy_digest_ = impl_->directory->policy_digest();
    result.root_ = impl_->directory->root_observation();
    return result;
}

const std::string& ReadOnlyPublicCapture::bytes(std::string_view relative) const {
    const auto found = assets_.find(relative);
    if (found == assets_.end()) throw Error(ErrorCode::InvalidInput, "resource is not registered");
    return found->second;
}
} // namespace wls::native
