#pragma once

#include "wls/structured_context.hpp"
#include "wls/store.hpp"

namespace wls::s1 {

// History access is a frozen, Worker-only control capability, not a workspace
// tool grant. These reads never execute historical tools or clear readiness.
bool is_history_control(std::string_view name) noexcept;
std::vector<ContextTool> history_control_declarations(const Json& policy = nullptr);
Json execute_history_control(Store& store, const StoreActor& actor,
    const ExecutionBinding& binding, std::string_view name, const Json& arguments);

// Pure projection of a scoped, host-verified tail. Large individual turns become
// explicit hash-pinned references; originals stay in SQLite. The current task
// contract is never an input to this projection and therefore cannot be pruned.
std::vector<StructuredContextMessage> worker_history_messages(const Json& tail,
    const Json& frozen_policy);

bool append_fold_policy(const Json& policy);
// Flat original turns and readback entries; no compiled predecessor is embedded.
Json append_fold_history(const Json& previous, const Json& delta, bool fold = false);
std::vector<StructuredContextMessage> append_fold_messages(const Json& projection);

} // namespace wls::s1
