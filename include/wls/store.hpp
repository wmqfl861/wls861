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
    // Host-selected post-acceptance job. This exceptional management attempt
    // can declare only report_experience; it never authorizes business tools.
    std::string experience_job_id;
    std::int64_t experience_job_version{};
    std::string experience_source_sha256;
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
          StoreOpenMode mode = StoreOpenMode::Takeover,
          std::string_view tools_migration_sql = {},
          std::string_view events_migration_sql = {},
          std::string_view workflow_migration_sql = {},
          std::string_view acceptance_migration_sql = {},
          std::string_view cleanup_migration_sql = {});
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

    // S1.6 trusted-host APIs. A persisted intent precedes physical creation;
    // only that new private work directory may become a deletion target.
    Json prepare_owned_workspace(const StoreActor& root, const std::string& task_id,
                                 const Json& parent_identity, const std::string& key);
    Json register_owned_workspace(const StoreActor& root, const std::string& resource_id,
                                  const Json& observed_identity);
    std::vector<Json> owned_workspaces(const std::string& request_id) const;
    Json workspace_cleanup_status(const std::string& request_id) const;
    Json begin_workspace_cleanup(const StoreActor& root, const Json& fresh_publication,
                                 const Json& inventories);
    void authorize_workspace_cleanup(const StoreActor& root, const Json& claim) const;
    Json record_workspace_removal(const StoreActor& root, const Json& claim,
                                  const std::string& resource_id, const Json& proof);
    Json finish_workspace_cleanup(const StoreActor& root, const Json& claim,
                                  const Json& fresh_publication, const Json& fresh_proofs);


    // S1.3 durable inbox/role-loop support requires the distributed 006 asset.
    // Dispatcher is request-scoped; subject_task_id on individual inbox items
    // keeps separate AMEND decisions distinct without inventing task actors.
    bool event_runtime_enabled() const;
    // Trusted-host inspection of every remaining actor/subject, including
    // retired teams. A smaller requested capacity fails instead of hiding work.
    std::vector<Json> request_inbox(const std::string& request_id,
                                    std::int64_t limit = 4096) const;
    std::vector<Json> inbox(const StoreActor& actor, std::int64_t limit = 256) const;
    Json claim_events(const StoreActor& actor, std::int64_t max_items = 64);
    // Completes exactly the captured IDs supplied here. Omitted captured items
    // return to PENDING; items that arrived during the claim remain untouched.
    Json finish_events(const StoreActor& actor, const std::string& claim_id,
                       std::int64_t generation,
                       const std::vector<std::int64_t>& completed_ids);

    Json worker_wait(const std::string& task_id) const;
    Json wait_worker(const StoreActor& actor, const ExecutionBinding& binding,
                     std::int64_t expected_task_version, const std::string& reason,
                     const std::string& command_id);
    // Epoch-only recovery may wake the same semantic target. An old wait may
    // never wake changed requirements, plan, task or instruction revisions.
    Json wake_worker(const StoreActor& actor, const ExecutionBinding& binding,
                     std::int64_t expected_wait_generation,
                     const std::string& command_id);

    // Complete feedback is immutable and paired with the already persisted
    // original attempt/response, including rejected or diagnostic results.
    Json record_tool_feedback(const std::string& attempt_id,
                              const ExecutionBinding& original_binding,
                              const Json& response, const Json& result);
    Json tool_feedback(const std::string& attempt_id) const;
    // Chronological, actor-scoped original facts. request_envelope is omitted
    // here to prevent recursive context embedding; attempt() retains it intact.
    // More than limit records fails explicitly rather than dropping history.
    std::vector<Json> attempt_history(const StoreActor& actor,
                                      std::int64_t limit = 256) const;

    // S1.4 question/answer and local submission checking require the known 007
    // asset. These bounded records retain their original semantic bindings;
    // epoch-only recovery never relabels old observations as new content.
    bool workflow_runtime_enabled() const;
    Json question(const std::string& question_id) const;
    std::vector<Json> questions(const std::string& request_id,
                                const std::string& task_id = {},
                                std::int64_t limit = 128) const;
    Json submission(const std::string& submission_id) const;
    std::vector<Json> submissions(const std::string& task_id,
                                  std::int64_t limit = 64) const;
    Json current_submission(const std::string& task_id) const;
    Json ask_question(const StoreActor& actor, const ExecutionBinding& binding,
                      std::int64_t expected_task_version, const Json& body,
                      const std::string& command_id);
    // Question controls retain the producer's policy as history while checking
    // this action's captured/current policy. Unrelated policy changes cannot
    // strand an unanswered blocking question; stale captured actions still fail.
    Json forward_question(const StoreActor& actor,
                          const ExecutionBinding& captured_actor_binding,
                          const std::string& question_id,
                          std::int64_t expected_question_version,
                          const std::string& note, const std::string& command_id);
    Json answer_question(const StoreActor& actor,
                         const ExecutionBinding& captured_actor_binding,
                         const std::string& question_id,
                         std::int64_t expected_question_version,
                         const Json& answer, const std::string& command_id);
    // Host fs_read captures an array of exactly {path,sha256,size_bytes,
    // content_base64}; Store checks canonical base64, hashes, complete bytes,
    // path coverage and frozen grants before committing immutable snapshots.
    // At most 8 files, 64 KiB each and 256 KiB total; no delivery acceptance.
    Json submit_work(const StoreActor& actor, const ExecutionBinding& binding,
                     std::int64_t expected_task_version, const Json& body,
                     const Json& artifact_snapshot,
                     const std::string& command_id);
    // check_body is {passed,notes?,criteria_results?}. A failed check requires
    // a new WorkOrder and is bounded by the frozen team rework limit. A pass
    // marks CANDIDATE_READY only; it never accepts or delivers the business.
    // Positive checks require the original producer policy to remain current;
    // current-policy negative checks may return an obsolete-policy submission.
    Json check_submission(const StoreActor& actor,
                          const ExecutionBinding& captured_binding,
                          std::int64_t expected_task_version,
                          const std::string& submission_id,
                          std::int64_t expected_submission_version,
                          const Json& check_body, const Json& rework_order,
                          const std::string& command_id);

    // S1.5 current Dispatcher acceptance is separate from local TL checking.
    // False decisions may revoke an accepted candidate before delivery using
    // the bounded control path. All original evidence remains immutable.
    bool acceptance_runtime_enabled() const;
    Json accept_candidate(const StoreActor& dispatcher,
                          const ExecutionBinding& captured_request_binding,
                          std::int64_t expected_task_version,
                          const std::string& submission_id,
                          std::int64_t expected_submission_version,
                          const Json& decision, const std::string& command_id);
    Json experience_job(const std::string& experience_id) const;
    std::vector<Json> experience_jobs(const std::string& request_id,
                                     const std::string& task_id = {},
                                     std::int64_t limit = 128) const;
    Json report_experience(const StoreActor& team_lead,
                           const ExecutionBinding& captured_binding,
                           const std::string& experience_id,
                           std::int64_t expected_job_version,
                           const std::string& captured_source_sha256,
                           const Json& body, const std::string& command_id);
    // Technical host assembly from previously accepted immutable evidence.
    // No live workspace reads and no new Root business judgment occur here.
    Json prepare_delivery(const StoreActor& root,
                          const ExecutionBinding& captured_request_binding,
                          std::int64_t expected_request_version,
                          const Json& output_root_identity,
                          const std::string& command_id);
    Json delivery(const std::string& delivery_id) const;
    std::vector<Json> deliveries(const std::string& request_id,
                                std::int64_t limit = 64) const;
    Json authorize_delivery_publication(const StoreActor& root,
                                        const ExecutionBinding& captured_binding,
                                        const std::string& delivery_id) const;
    Json record_delivery_publication(const StoreActor& root,
                                     const ExecutionBinding& captured_binding,
                                     const std::string& delivery_id,
                                     std::int64_t expected_delivery_version,
                                     const Json& publication,
                                     const std::string& command_id);
    // Exact receipt intent is replayable across epochs; querying never acks.
    Json acknowledge_delivery(const StoreActor& root,
                              const ExecutionBinding& captured_binding,
                              const std::string& delivery_id,
                              const std::string& manifest_sha256,
                              std::int64_t requirements_revision,
                              std::int64_t plan_revision,
                              const std::string& recipient,
                              const std::string& method,
                              const std::string& idempotency_key,
                              const Json& fresh_publication);
    Json closeout(const std::string& request_id) const;

    StoreActor host_actor(Role role, const std::string& request_id,
                          const std::string& team_id = {}) const;
    ExecutionBinding binding(const std::string& task_id) const;
    ExecutionBinding request_binding(const std::string& request_id) const;

    Json publish_plan(const StoreActor& actor, std::int64_t expected_request_version,
                      std::int64_t based_on_requirements_revision, const Json& body,
                      const std::string& command_id);
    // Bounded independent team-task plan plus initial team allocation is one
    // transaction. Later plans preserve every existing node/template identity.
    // Dispositions are keyed by node_id; omitted AMEND tasks stay paused.
    Json apply_independent_plan(const StoreActor& actor,
                                std::int64_t expected_request_version,
                                std::int64_t based_on_requirements_revision,
                                const Json& body, const Json& dispositions,
                                const std::string& command_id);
    Json apply_independent_dispositions(const StoreActor& actor,
                                        std::int64_t expected_request_version,
                                        std::int64_t based_on_requirements_revision,
                                        std::int64_t target_plan_revision,
                                        const Json& dispositions,
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
    // Atomic execution claim for registered real tools. Only the transaction
    // that changes PREPARED to RUNNING returns dispatch_claimed=true. Retried
    // commands and already claimed/settled operations return false, never a
    // fresh permission to repeat an external effect.
    Json start_tool_operation(const StoreActor& actor, const std::string& op_id,
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

    // Local host control, never a registered Worker/model business tool.
    // Preflight is read-only; mutation repeats all checks under a transaction.
    void validate_reconciliation(const StoreActor& root, const std::string& op_id,
                                 std::int64_t expected_op_version, const std::string& outcome,
                                 const std::string& host_evidence, const std::string& command_id) const;
    Json reconcile_operation(const StoreActor& root, const std::string& op_id,
                             std::int64_t expected_op_version, const std::string& outcome,
                             const std::string& host_evidence, const std::string& command_id);

    // S1.2 APIs require the known 403 schema. Passing the additional 005 asset
    // migrates a populated 402 database without replacing its frozen records.
    // Existing 402-only callers continue to work with the original API.
    // Workspace identities are supplied by the trusted host after opening a
    // canonical directory; models never choose a host path or device/inode.
    Json bind_workspace(const std::string& team_id,
                        const std::string& canonical_path,
                        std::uint64_t device, std::uint64_t inode,
                        const Json& execution_policy = boost::json::object{});
    Json workspace(const std::string& team_id) const;
    Json prepare_tool_operation(const StoreActor& actor,
                                const ExecutionBinding& frozen_binding,
                                std::int64_t expected_task_version,
                                const std::string& business_key,
                                const std::string& tool_id,
                                const Json& arguments,
                                const std::string& command_id);
    // A current authorized Worker may retrieve its old intent by business key
    // without a new dispatch, even after an instruction change or restart.
    // Different arguments/tool/team remain a conflict. Missing keys return null.
    Json operation_for_key(const StoreActor& actor,
                           const ExecutionBinding& current_binding,
                           const std::string& business_key,
                           const std::string& tool_id,
                           const Json& arguments) const;
    // Bounded diagnostic authorization is separate from business readiness,
    // UNKNOWN and ordinary budgets. This method is read-only and never fences.
    // The executor applies returned max_queries_per_minute and path/byte caps.
    Json authorize_diagnostic(const StoreActor& actor,
                              const ExecutionBinding& current_binding,
                              const std::string& tool_id) const;
    Json authorize_control(const StoreActor& actor,
                           const ExecutionBinding& current_binding,
                           const std::string& tool_id) const;
    std::vector<std::string> authorized_worker_tools(
        const StoreActor& actor, const ExecutionBinding& current_binding) const;
    // The original host command (including its captured binding/version) is
    // the durable retry identity. An old successful ack/cancel may replay after
    // newer instructions or epochs, but a fresh stale command cannot mutate.
    // Cancel reads its current operation CAS inside the same transaction; that
    // changing CAS is never substituted into the original command fingerprint.
    Json execute_tool_control(const StoreActor& actor,
                              const ExecutionBinding& original_binding,
                              std::int64_t expected_task_version,
                              const std::string& tool_id, const Json& arguments,
                              const std::string& command_id);
    // Host-only durable execution facts. Captured original bindings are checked
    // against the intent, not replaced with the current binding. Stale/UNKNOWN
    // facts are retained with accepted=false; only explicit reconciliation can
    // settle an UNKNOWN operation. Exact recording retries are idempotent.
    Json record_operation_result(const std::string& op_id,
                                 const ExecutionBinding& original_binding,
                                 const std::string& outcome,
                                 const Json& result, const Json& evidence,
                                 const std::string& stdout_bytes = {},
                                 const std::string& stderr_bytes = {},
                                 bool stdout_truncated = false,
                                 bool stderr_truncated = false);
    Json operation_result(const std::string& op_id) const;
    // Returns base64-encoded bytes, offset/next_offset, retained byte count and
    // truncation metadata. The caller must enforce the diagnostic rate limit.
    Json operation_output(const StoreActor& actor,
                          const ExecutionBinding& current_binding,
                          const std::string& op_id, const std::string& stream,
                          std::int64_t offset = 0,
                          std::int64_t limit = 65536) const;

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
