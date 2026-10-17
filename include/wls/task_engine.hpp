#pragma once

#include "wls/config.hpp"
#include "wls/identity.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace wls {
using Revision = std::uint64_t;
enum class ErrorCode {
    InvalidInput, NotFound, Conflict, Unauthorized, InvalidState,
    StaleEpoch, StaleRequirements, QuotaExceeded, UnknownOutcome, Persistence
};
class KernelError : public std::runtime_error {
public:
    KernelError(ErrorCode error_code, const std::string& message)
        : std::runtime_error(message), code(error_code) {}
    ErrorCode code;
};

enum class RequestState {
    Queued, Running, Adjusting, Submitted, Verifying, Accepted,
    HandoffPending, Completed, Cancelled, Superseded, Failed
};
enum class ResourceState { Starting, Active, Draining, Stopped, Cleaning, Closed, CleanupFailed };
enum class OperationState { Prepared, Running, Succeeded, Failed, Unknown, Cancelled };

std::string to_string(RequestState state);
std::string to_string(ResourceState state);
std::string to_string(OperationState state);

struct RequestInput {
    std::string tenant_id;
    std::string project_id;
    std::string root_id;
    std::string idempotency_key;
    std::string goal;
    ModelSelection root_defaults;
    ToolSet allowed_tools;
    bool operator==(const RequestInput&) const = default;
};

struct Command {
    Actor actor;
    std::string command_id;
    Revision expected_state_version{};
    Revision requirements_revision{};
    Revision lease_epoch{};
};

struct TeamSnapshot {
    std::string team_id;
    std::string task_id;
    std::string lead_id;
    std::string worker_id;
    bool review_team{false};
    Revision lease_epoch{};
    Revision applied_revision{};
    Revision applied_plan_revision{};
    Revision instructions_revision{};
    Revision instructions_plan_revision{};
    Revision instructions_version{};
    Revision applied_instructions_version{};
    std::string instructions;
};

struct OperationSnapshot {
    std::string operation_id;
    std::string team_id;
    std::string idempotency_key;
    std::string tool_id;
    std::string payload;
    Revision requirements_revision{};
    Revision plan_revision{};
    Revision lease_epoch{};
    OperationState state{OperationState::Prepared};
    std::string evidence_ref;
};

struct Candidate {
    std::string artifact_id;
    Revision artifact_version{};
    std::string content_hash;
    std::string producer_team_id;
    Revision requirements_revision{};
    bool operator==(const Candidate&) const = default;
};

struct ReviewRecord {
    Candidate candidate;
    std::string reviewer_team_id;
    bool passed{false};
    std::string evidence_ref;
    Revision instructions_version{};
};

struct RequestSnapshot {
    std::string request_id;
    std::string task_id;
    std::string dispatcher_id;
    RequestInput original_input;
    std::string goal;
    Revision requirements_revision{1};
    Revision plan_revision{0};
    Revision plan_requirements_revision{0};
    Revision state_version{1};
    Revision lease_epoch{1};
    RequestState state{RequestState::Queued};
    ResourceState resources{ResourceState::Starting};
    std::string plan;
    std::string replaces;
    std::string replaced_by;
    std::map<std::string, TeamSnapshot> teams;
    std::map<std::string, OperationSnapshot> operations;
    std::optional<Candidate> candidate;
    // Request-scoped immutable artifact version high-water marks survive AMEND.
    std::map<std::string, Revision> artifact_versions;
    std::vector<ReviewRecord> reviews;
    bool accepted{false};
    std::string package_ref;
    std::string receipt_ref;
    // Successful command fingerprints for exact retries, scoped to this request.
    std::map<std::string, std::string> command_receipts;
};

struct EventEnvelope {
    Revision sequence{};
    std::string event_id;
    std::string request_id;
    std::string task_id;
    std::string team_id;
    std::string command_id;
    std::string actor_id;
    std::string kind;
    Revision requirements_revision{};
    Revision plan_revision{};
    Revision state_version{};
    Revision lease_epoch{};
    std::string detail;
};

struct EngineLimits {
    std::size_t max_requests{128};
    std::size_t max_teams_per_request{16};
    std::size_t max_operations_per_request{256};
    // Ordinary state-change budget; necessary settlement/lifecycle events
    // remain writable. Diagnostic rejection events have a separate same-size cap.
    std::size_t max_events{10000};
    std::size_t max_transaction_bytes{16U * 1024U * 1024U};
};

// Single-host, trusted in-process API. This kernel never executes a model,
// shell command, external tool, real delivery, or data deletion.
class TaskEngine final {
public:
    TaskEngine(const std::filesystem::path& journal_path, EngineLimits limits,
               ToolSet system_tools);
    ~TaskEngine();
    TaskEngine(const TaskEngine&) = delete;
    TaskEngine& operator=(const TaskEngine&) = delete;

    RequestSnapshot submit(const Actor& root, const RequestInput& input);
    RequestSnapshot amend(const std::string& request_id, const Command&, const std::string& goal);
    RequestSnapshot replace(const std::string& request_id, const Command&, const RequestInput& replacement);
    RequestSnapshot cancel(const std::string& request_id, const Command&);
    RequestSnapshot plan(const std::string& request_id, const Command&, const std::string& plan);
    RequestSnapshot create_team(const std::string& request_id, const Command&,
                                const std::string& team_id, bool review_team = false);
    RequestSnapshot instruct_team(const std::string& request_id, const Command&,
                                  const std::string& instructions);
    RequestSnapshot acknowledge_applied(const std::string& request_id, const Command&);

    RequestSnapshot prepare_operation(const std::string& request_id, const Command&,
                                      const std::string& operation_key,
                                      const std::string& tool_id, const std::string& payload);
    RequestSnapshot start_operation(const std::string& request_id, const Command&,
                                    const std::string& operation_id);
    RequestSnapshot finish_operation(const std::string& request_id, const Command&,
                                     const std::string& operation_id, OperationState outcome,
                                     const std::string& evidence_ref);

    RequestSnapshot submit_candidate(const std::string& request_id, const Command&, Candidate candidate);
    RequestSnapshot record_review(const std::string& request_id, const Command&,
                                  const Candidate& candidate, bool passed,
                                  const std::string& evidence_ref);
    RequestSnapshot accept(const std::string& request_id, const Command&);
    RequestSnapshot prepare_handoff(const std::string& request_id, const Command&,
                                    const std::string& package_ref);
    RequestSnapshot confirm_delivery(const std::string& request_id, const Command&,
                                     const Candidate& candidate, const std::string& package_ref,
                                     const std::string& synthetic_receipt_ref);

    // Host-only lifecycle APIs: never expose as agent tools.
    RequestSnapshot recover(const std::string& request_id, Revision expected_state_version,
                            const std::string& command_id);
    RequestSnapshot reconcile_operation(const std::string& request_id,
        Revision expected_state_version, const std::string& command_id,
        const std::string& operation_id, OperationState outcome, const std::string& evidence_ref);
    RequestSnapshot advance_resources(const std::string& request_id,
        Revision expected_state_version, const std::string& command_id, ResourceState target);

    // Trusted runtime diagnostics. Future external views must apply visibility policy.
    RequestSnapshot get(const std::string& request_id) const;
    std::vector<RequestSnapshot> requests() const;
    std::vector<EventEnvelope> events(const std::string& request_id = "",
                                      Revision after_sequence = 0) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace wls
