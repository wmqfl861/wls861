#pragma once
#include "readonly_internal.hpp"
#include <windows.h>

namespace wls::native::system {

// Internal, handle-only mechanism. It does not open paths or grant any Store,
// process, filesystem-write or cleanup authority.
class WindowsReadOnlyChecks final {
public:
    explicit WindowsReadOnlyChecks(const WindowsReadOnlyPolicy& policy);
    ~WindowsReadOnlyChecks();
    WindowsReadOnlyChecks(const WindowsReadOnlyChecks&) = delete;
    WindowsReadOnlyChecks& operator=(const WindowsReadOnlyChecks&) = delete;
    void require_current_token() const;
    void require_volume_root(HANDLE handle) const;
    WindowsObjectObservation observe(HANDLE handle, bool directory,
                                     std::wstring_view expected_component) const;
    void require_unchanged(HANDLE handle, const WindowsObjectObservation& original) const;
    const std::string& policy_digest() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A separate internal ACL evaluator permits native synthetic-descriptor
// rejection tests without pretending an elevated Server token is Win11.
// Runtime calls it only with the security descriptor from the same live HANDLE.
struct WindowsAclObservation {
    std::string owner_sid;
    std::string descriptor_sha256;
};
WindowsAclObservation inspect_windows_acl(PSECURITY_DESCRIPTOR descriptor,
                                         const WindowsReadOnlyPolicy& policy);
WindowsObjectIdentity inspect_windows_identity(HANDLE handle);

} // namespace wls::native::system
