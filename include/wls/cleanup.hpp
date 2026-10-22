#pragma once

#include "wls/runtime_types.hpp"
#include <cstddef>
#include <cstdint>

namespace wls::s1 {

struct CleanupLimits {
    std::size_t max_entries{4096};
    std::size_t max_depth{32};
    std::uint64_t max_file_bytes{16U * 1024U * 1024U};
    std::uint64_t max_total_bytes{256U * 1024U * 1024U};
};

// Trusted-host APIs, never model tools. The Store must persist the creation
// intent before create and the exact inventory before erase. Only directories
// created by this API are eligible. An ownership envelope is outside /work;
// the Worker cannot alter the ownership receipt through its file interface.
// Minimal envelopes/receipts remain retained; arbitrary same-UID host mutation
// and secure media erasure are not isolation guarantees of this implementation.
Json create_owned_workspace(const Json& intent);
Json inspect_owned_workspace(const Json& resource);
Json inventory_owned_workspace(const Json& resource, CleanupLimits limits = {});

// Each bounded pass preflights ALL surviving entries before the first unlink.
// Only exact frozen files/directories may disappear; new entries, links,
// mounts, changed bytes or replaced identities fail closed. Missing entries
// are resumable only because the caller has already durably authorized this
// exact inventory. max_unlinks is 1..4096; work removal also consumes a slot.
// The envelope and owner.json are never deleted. Partial passes are durable
// checkpoints, not successful complete cleanup.
Json erase_owned_workspace(const Json& resource, const Json& inventory,
                           std::size_t max_unlinks = 1024,
                           CleanupLimits limits = {});
Json verify_owned_workspace_removed(const Json& resource, const Json& inventory);

} // namespace wls::s1
