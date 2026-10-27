#pragma once

#include "wls/structured_context.hpp"
#include "wls/store.hpp"

namespace wls::s1 {

// Frozen management controls are observation-only. Every model use still
// reserves, settles and records feedback through the ordinary runner.
bool is_management_history_control(std::string_view name) noexcept;
std::vector<ContextTool> management_history_declarations();
Json execute_management_history_control(Store& store, const StoreActor& actor,
    const ExecutionBinding& current, std::string_view name, const Json& arguments);

// Window requests expose every OPEN question. Legacy requests retain the
// original complete-history API and its original capacity behavior.
std::vector<Json> visible_management_questions(Store& store, Role role,
    const std::string& request_id, const std::string& task_id = {});

// A view contains scoped, verified originals and bounded lifetime catalogs.
// Only complete old turns can become references; pinned unresolved facts and
// the caller's authoritative contract are outside this projection.
std::vector<StructuredContextMessage> management_history_messages(
    const Json& view, const Json& frozen_policy);

// Measures the complete logical input before any reservation. Capacity errors
// contain section sizes and an actionable explanation, never clipped content.
CompiledContext compile_management_context(const ContextInput& input,
    const std::vector<StructuredContextMessage>& messages, const Json& frozen_policy);

} // namespace wls::s1
