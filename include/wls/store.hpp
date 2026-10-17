#pragma once

#include "wls/identity.hpp"
#include "wls/runtime_types.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wls::s1 {

// Trusted-host context, never constructed from a model tool argument. A host
// authenticates first; Store checks its persisted owner and fencing epochs.
struct StoreActor {
    Role role{Role::Root};
    std::string principal_id;
    std::string request_id;
    std::string task_id;
    std::string team_id;
    std::int64_t dispatcher_epoch{};
    std::int64_t execution_epoch{};
};

struct BudgetLimits {
    std::int64_t max_attempts{64};
    std::int64_t input_tokens{1000000};
    std::int64_t output_tokens{100000};
    std::int64_t cost_micro_usd{};
};

struct AdmissionInput {
    std::string tenant_id;
    std::string project_id;
    std::string root_id;
    std::string function_id;
    std::string idempotency_key;
    std::string original_text;
    Json config_snapshot;
    std::vector<std::string> tool_allowlist;
    Json constraint_catalog = boost::json::array{};
    BudgetLimits budget;
};

struct AttemptReservation {
    std::string call_id;
    std::string attempt_id;
    std::string config_snapshot_sha256;
    std::string model_ref;
    std::string adapter_kind{"fake"};
    std::int64_t reserved_input_tokens{};
    std::int64_t reserved_output_tokens{};
    std::int64_t reserved_cost_micro_usd{};
    std::int64_t max_output_tokens{};
    // Every attempt supplies the complete to_json(ModelRequest); reserve
    // rejects a missing envelope before allocating budget or writing events.
    Json request_envelope = nullptr;
};

enum class StoreOpenMode { Takeover, ReadOnly };

class Store {
public:
    // SQL is a trusted, program-distributed asset. Existing unknown databases
    // fail closed; original M1 Journals are never interpreted or rewritten.
    // The unique writer lease is held for this Store's lifetime. Opening fences
    // live requests before actors can be issued and makes pending calls UNKNOWN.
    Store(const std::filesystem::path& database_path,
          std::string_view base_schema_sql, std::string_view migration_sql,
          StoreOpenMode mode = StoreOpenMode::Takeover);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Json diagnostics() const;
    Json admit(const AdmissionInput& input);
    Json request(const std::string& request_id) const;
    Json request_snapshot(const std::string& request_id) const;
    Json plan(const std::string& request_id, std::int64_t revision = 0) const;
    Json task(const std::string& task_id) const;
    Json instruction(const std::string& task_id) const;
    void validate_brief(const std::string& request_id, const Json& brief) const;
    Json operation(const std::string& op_id) const;
    Json budget(const std::string& request_id) const;
    Json attempt(const std::string& attempt_id) const;
    std::vector<Json> events(const std::string& request_id,
                             std::int64_t after_seq = 0,
                             std::int64_t limit = 100) const;
    std::vector<Json> tasks(const std::string& request_id) const;
    Json pending_recovery() const;

    StoreActor host_actor(Role role, const std::string& request_id,
                          const std::string& team_id = {}) const;
    ExecutionBinding binding(const std::string& task_id) const;
    ExecutionBinding request_binding(const std::string& request_id) const;

    Json publish_plan(const StoreActor& actor, std::int64_t expected_request_version,
                      std::int64_t based_on_requirements_revision, const Json& body,
                      const std::string& command_id);
    Json create_task(const StoreActor& actor, std::int64_t expected_request_version,
                     std::int64_t based_on_requirements_revision,
                     std::int64_t target_plan_revision, const std::string& node_id,
                     const Json& brief, const std::string& command_id,
                     const std::string& template_id = "default");
    Json amend(const StoreActor& root, std::int64_t expected_request_version,
               const std::string& text, const Json& effective_constraint_catalog,
               const std::string& command_id);
    Json set_amendment_disposition(const StoreActor& actor,
                                  const std::string& task_id,
                                  std::int64_t expected_task_version,
                                  std::int64_t based_on_requirements_revision,
                                  std::int64_t target_plan_revision,
                                  const std::string& disposition,
                                  const std::string& rationale,
                                  const Json& brief,
                                  const std::string& command_id);
    // Explicit rebind for a new plan under the same requirement revision.
    Json rebind_task(const StoreActor& actor, const std::string& task_id,
                     std::int64_t expected_task_version,
                     std::int64_t based_on_requirements_revision,
                     std::int64_t target_plan_revision, const Json& brief,
                     const std::string& command_id);
    Json issue_instruction(const StoreActor& actor,
                           const ExecutionBinding& frozen_binding,
                           std::int64_t expected_task_version, const Json& body,
                           const std::string& command_id);
    Json acknowledge(const StoreActor& actor, const ExecutionBinding& frozen_binding,
                     std::int64_t expected_task_version,
                     const std::string& command_id);

    Json prepare_operation(const StoreActor& actor,
                           const ExecutionBinding& frozen_binding,
                           std::int64_t expected_task_version,
                           const std::string& business_key,
                           const std::string& tool_id, const Json& arguments,
                           const std::string& side_effect,
                           const std::string& command_id);
    Json start_operation(const StoreActor& actor, const std::string& op_id,
                         std::int64_t expected_op_version,
                         const std::string& command_id);
    Json finish_operation(const StoreActor& actor, const std::string& op_id,
                          const ExecutionBinding& original_binding,
                          std::int64_t expected_op_version,
                          const std::string& outcome,
                          const std::string& evidence_ref,
                          const std::string& command_id);
    Json cancel_operation(const StoreActor& actor, const std::string& op_id,
                          std::int64_t expected_op_version,
                          const std::string& command_id);
    Json reconcile_operation(const std::string& op_id,
                             std::int64_t expected_op_version,
                             const std::string& outcome,
                             const std::string& host_evidence,
                             const std::string& command_id);

    Json reserve_attempt(const StoreActor& actor,
                         const ExecutionBinding& frozen_binding,
                         const AttemptReservation& reservation);
    // Host-only recording of the original adapter response and usage. A stale
    // result is persisted for accounting, returned as accepted=false, and never
    // applied to a task. Usage fields may be null; their reservations remain.
    Json settle_attempt(const std::string& attempt_id,
                        const ExecutionBinding& original_binding,
                        const std::string& config_snapshot_sha256,
                        const std::string& outcome,
                        const Json& usage, const Json& response);

    Json recover_request(const std::string& request_id,
                         std::int64_t expected_request_version);
    Json cancel_request(const StoreActor& root,
                        std::int64_t expected_request_version,
                        const std::string& command_id);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wls::s1
