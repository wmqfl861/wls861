#pragma once

#include "wls/structured_context.hpp"
#include "wls/store.hpp"

namespace wls::s1 {

// History access is a frozen, Worker-only control capability, not a workspace
// tool grant. These reads never execute historical tools or clear readiness.
bool is_history_control(std::string_view name) noexcept;
std::vector<ContextTool> history_control_declarations();
Json execute_history_control(Store& store, const StoreActor& actor,
    const ExecutionBinding& binding, std::string_view name, const Json& arguments);

// Pure projection of a scoped, host-verified tail. Large individual turns become
// explicit hash-pinned references; originals stay in SQLite. The current task
// contract is never an input to this projection and therefore cannot be pruned.
std::vector<StructuredContextMessage> worker_history_messages(const Json& tail,
    const Json& frozen_policy);

} // namespace wls::s1
