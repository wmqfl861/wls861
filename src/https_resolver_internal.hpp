#pragma once

// Fixed wire protocol and parent-side lifecycle shared by the pinned
// wls861-resolve-ipv4 helper and its only caller (the HTTPS transport).
// Linux-only by design; the helper links nothing but the system resolver
// and the C library, and the parent never takes a PATH/cwd/configuration
// hint for helper discovery.

#include "wls/runtime_types.hpp"

#include <chrono>
#include <stop_token>
#include <string>
#include <vector>

namespace wls::s1::resolver {

inline constexpr char magic[] = "WLS-RESOLVE-1";
inline constexpr std::size_t max_addresses = 16;
// "WLS-RESOLVE-1\n" + status line + 16 dotted quads with newlines, with room
// for the count line; anything larger is a protocol violation, never parsed.
inline constexpr std::size_t max_output_bytes = 512;

struct Observation {
    bool started{false};
    bool ok{false};
    // A child that could not be reaped inside the bounded cleanup window.
    // Its pid/pidfd stay in the process-local slot and block later resolves.
    bool cleanup_pending{false};
    // Private evidence only: executable-relative identity of the pinned
    // helper, never a credential, workspace or provider diagnostic.
    std::string helper_identity;
    std::vector<std::string> addresses;
    std::chrono::microseconds elapsed{};
};

// Resolves one already-frozen hostname/service pair over the pinned helper.
// Throws Error before any child is spawned when the helper cannot be pinned
// or a previous child is still unreaped. Returns started=true with ok=false
// for bounded lookup/protocol/cancel/deadline failures after the spawn.
Observation resolve_ipv4(const std::string& hostname, const std::string& service,
                         std::chrono::steady_clock::time_point deadline,
                         std::stop_token stop);

// Reaps any still-pending child inside a bounded window; true when a child
// remains, in which case live transport in this process must stay refused.
bool cleanup_pending() noexcept;

} // namespace wls::s1::resolver
