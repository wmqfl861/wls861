#pragma once

#include "wls/runtime_types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace wls::s1 {

enum class DeliveryMutationStatus { Succeeded, EffectNotStarted, OutcomeUnknown };

struct DeliveryLimits {
    std::size_t max_files{64};
    std::uint64_t max_file_bytes{2U * 1024U * 1024U};
    // Includes manifest.json and all decoded file bytes, not their base64 text.
    std::uint64_t max_total_bytes{4U * 1024U * 1024U};
    std::size_t max_manifest_bytes{1024U * 1024U};
};

struct DeliveryMutationResult {
    DeliveryMutationStatus status{DeliveryMutationStatus::EffectNotStarted};
    std::optional<ErrorCode> error_code;
    Json evidence = boost::json::object{};
    Json publication = nullptr;
};

std::string_view delivery_mutation_status_name(DeliveryMutationStatus status) noexcept;

// Trusted-host Linux API; this is not a Worker or management model tool. The
// caller persists the exact plan before publish and records its result later.
// It must separately authorize acceptance, current versions and any receipt.
// The delivery root must be outside all business workspaces and the control DB.
// This class pins one existing root using Workspace's strong openat2 boundary.
// Its cooperative lock does not isolate arbitrary same-UID host programs.
class DeliveryDirectory {
public:
    explicit DeliveryDirectory(const std::filesystem::path& root,
                               DeliveryLimits limits = {});
    ~DeliveryDirectory();
    DeliveryDirectory(DeliveryDirectory&&) noexcept;
    DeliveryDirectory& operator=(DeliveryDirectory&&) noexcept;
    DeliveryDirectory(const DeliveryDirectory&) = delete;
    DeliveryDirectory& operator=(const DeliveryDirectory&) = delete;

    // {path: canonical absolute path, device: uint64, inode: uint64}.
    Json root_identity() const;

    // Exact plan keys: delivery_id, root_identity, manifest, manifest_sha256,
    // files. Each file is {name, sha256, size_bytes, content_base64}; names are
    // safe flat basenames, sorted and unique. The final absolute package path
    // is bounded to 4096 bytes. manifest.files is exactly their
    // metadata without content_base64, and manifest.delivery_id matches the
    // plan. Other manifest fields are opaque host evidence. The digest covers
    // the complete internal canonical_json(manifest), without an added newline.
    //
    // The full bundle is staged, synced, published with RENAME_NOREPLACE, and
    // synced again. An existing matching bundle is verified and synced anew;
    // it is never overwritten. Failure after attempted rename is UNKNOWN.
    // Only positively identified files created by this call may be removed on
    // failure. Orphan stages from another call are retained for reconciliation.
    DeliveryMutationResult publish(const Json& frozen_plan);

    // Pure read: verifies the complete package and rejects missing/extra files,
    // unsafe entries or changed bytes. It does not fsync, mutate state, or sign
    // a receipt. publication includes verified=true, durable=false. publish's
    // success includes durable=true. Neither result proves business acceptance.
    Json inspect(const Json& frozen_plan) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wls::s1
