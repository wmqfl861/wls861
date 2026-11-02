#pragma once

#include "wls/native_foundation.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace wls::native {

// Explicit trusted-host input. There is no default build or privileged SID.
// Supplying this structure does not constitute coordinator policy acceptance.
// The mechanism always requires native x64 Windows 11, a normal non-admin
// primary token, no impersonation, and local NTFS with persistent ACLs.
struct WindowsReadOnlyPolicy {
    std::string approval_reference;
    std::vector<std::uint32_t> approved_builds;
    std::vector<std::string> trusted_owner_and_writer_sids;
};

// An observation from a live Windows HANDLE, never a POSIX device/inode or
// permission to reopen an object. File ID bytes retain FILE_ID_128 order;
// volume_serial is the full FILE_ID_INFO unsigned 64-bit value.
struct WindowsObjectIdentity {
    std::uint64_t volume_serial{};
    std::array<std::uint8_t, 16> file_id{};
    bool operator==(const WindowsObjectIdentity&) const = default;
};

struct WindowsObjectObservation {
    WindowsObjectIdentity identity;
    std::uint32_t attributes{};
    std::uint32_t reparse_tag{};
    std::uint32_t links{};
    std::uint64_t size{};
    std::uint64_t creation_time{};
    std::uint64_t last_write_time{};
    // These host details are private evidence; do not emit them to public logs.
    std::string owner_sid;
    std::string security_descriptor_sha256;
    bool operator==(const WindowsObjectObservation&) const = default;
};

class ReadOnlyPublicCapture final {
public:
    const std::string& bytes(std::string_view registered_relative) const;
    std::size_t count() const noexcept { return assets_.size(); }
    std::size_t total_bytes() const noexcept { return total_bytes_; }
    const std::string& manifest_digest() const noexcept { return manifest_digest_; }
    const std::string& policy_digest() const noexcept { return policy_digest_; }
    const WindowsObjectObservation& root_observation() const noexcept { return root_; }
    const std::map<std::string, WindowsObjectObservation, std::less<>>& observations() const noexcept {
        return observations_;
    }
private:
    friend class ReadOnlyRoot;
    std::map<std::string, std::string, std::less<>> assets_;
    std::map<std::string, WindowsObjectObservation, std::less<>> observations_;
    std::size_t total_bytes_{};
    std::string manifest_digest_;
    std::string policy_digest_;
    WindowsObjectObservation root_;
};

// Owns the root and all ancestors; handles are neither borrowed nor serialized.
// No arbitrary read/list/write/execute API is exposed. A complete capture uses
// only the existing compiled manifest/catalog and returns immutable bytes.
class ReadOnlyRoot final {
public:
    static ReadOnlyRoot open(const std::filesystem::path& absolute_root,
                             const WindowsReadOnlyPolicy& policy);
    ~ReadOnlyRoot();
    ReadOnlyRoot(ReadOnlyRoot&&) noexcept;
    ReadOnlyRoot& operator=(ReadOnlyRoot&&) noexcept;
    ReadOnlyRoot(const ReadOnlyRoot&) = delete;
    ReadOnlyRoot& operator=(const ReadOnlyRoot&) = delete;
    const WindowsObjectObservation& identity() const;
    ReadOnlyPublicCapture capture_registered() const;
private:
    struct Impl;
    explicit ReadOnlyRoot(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace wls::native
