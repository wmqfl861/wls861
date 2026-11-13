#pragma once

#include "wls/store.hpp"
#include "wls/provider_profile.hpp"

#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>

struct sqlite3;

namespace wls::s1 {
class FrozenRequestConfig;

// Internal declarations only: all bodies keep a single translation-unit owner.
namespace store_detail {
using Row = boost::json::object;
using Array = boost::json::array;
struct AttemptPricing : TokenPricing { bool reserve_fallback{false}; };

void require(bool condition, ErrorCode code, const std::string& message);
std::string str(const Row& row, std::string_view key);
std::int64_t number(const Row& row, std::string_view key);
std::int64_t next(std::int64_t n);
std::string stamp();
Row payload(const StoreActor& actor, std::string_view type, Row fields = {});
std::string json_text(const Json& value, std::size_t maximum = 1024 * 1024);
void object_fields(const Json& value, std::initializer_list<std::string_view> allowed,
                   std::initializer_list<std::string_view> required = {});
std::string workflow_text(const Json& value, std::string_view key, std::size_t maximum,
                          bool optional = false);
} // namespace store_detail

class Store::Impl {
public:
    using Row = store_detail::Row;
    using Array = store_detail::Array;
    using AttemptPricing = store_detail::AttemptPricing;
    Impl(const std::filesystem::path& database_path, std::string_view base_sql,
         std::string_view migration_sql, StoreOpenMode selected_mode,
         std::string_view tools_migration_sql, std::string_view events_migration_sql,
         std::string_view workflow_migration_sql, std::string_view acceptance_migration_sql,
         std::string_view cleanup_migration_sql, StoreRecoveryScope selected_scope,
         std::string_view allocation_migration_sql, std::string_view live_migration_sql,
         std::string_view reconciliation_migration_sql);
    ~Impl();
    void close() noexcept;
    void exec(std::string_view sql);
    void rollback() noexcept;
    void execute(std::string_view sql, const Array& args = {});
    std::optional<Row> optional(std::string_view sql, const Array& args = {}) const;
    Row one(std::string_view sql, const Array& args = {}) const;
    std::vector<Row> all(std::string_view sql, const Array& args = {}) const;
    void cas(std::string_view sql, const Array& args);
    class Transaction {
    public:
        explicit Transaction(Impl& owner, bool admission = false);
        ~Transaction();
        void commit();
    private:
        Impl& owner_;
        bool committed_{false};
    };

    void check_request_scope(const std::string& id) const;
    Row scoped_row(Row row) const;
    Row request_row(const std::string& id) const;
    Row task_row(const std::string& id) const;
    Row team_row(const std::string& id) const;
    Row op_row(const std::string& id) const;
    void require_tools_schema() const;
    void require_events_schema() const;
    void require_workflow_schema() const;
    void require_acceptance_schema() const;
    Json acceptance_info(const Row& row) const;
    std::optional<Row> current_acceptance(const std::string& task_id) const;
    Json experience_info(const std::string& id) const;
    Json validate_experience_target(const StoreActor& actor, const ExecutionBinding& captured,
                                   const std::string& id, std::int64_t expected, const std::string& source_hash) const;
    void invalidate_acceptance(const std::string& request_id, const std::string& task_id = {});
    void require_settled(const std::string& request_id, const std::string& team_id = {}) const;
    Row delivery_row(const std::string& id) const;
    Json delivery_info(const std::string& id) const;
    void delivery_current(const Row& delivery) const;
    void verify_publication(const Row& record, const Json& publication) const;
    void retain_closeout(const Row& request, const std::string& delivery_path = {}, const std::string& manifest_hash = {});
    Row question_row(const std::string& id) const;
    Json question_info(const std::string& id) const;
    Row submission_row(const std::string& id) const;
    Json submission_info(const std::string& id) const;
    bool has_blocking_question(const std::string& task_id) const;
    void workflow_subject(const StoreActor& actor, const ExecutionBinding& captured,
                          const Row& subject, bool current_policy_control = false) const;
    Json rejected_info(const std::string& task_id) const;
    Json rejection_subject(const StoreActor& actor, const ExecutionBinding& captured,
                           std::int64_t task_version, const std::string& submission_id,
                           std::int64_t submission_version) const;
    void supersede_workflow(const std::string& request_id, const std::string& task_id = {});
    Json wait_info(const std::string& task_id) const;
    Json feedback_info(const std::string& attempt_id) const;
    Json workspace_info(const std::string& team_id) const;
    Json result_info(const std::string& op_id) const;
    Row authorize(const StoreActor& actor, std::initializer_list<Role> roles, bool current_epochs = true) const;
    ExecutionBinding request_binding(const std::string& request_id) const;
    ExecutionBinding binding(const std::string& task_id) const;
    void matches(const ExecutionBinding& b, bool policy = true) const;
    void actor_binding(const StoreActor& actor, const ExecutionBinding& b) const;
    void tool_grant(const Row& r, const Row& team, std::string_view tool_id,
                    const FrozenRequestConfig* reservation_config = nullptr) const;
    Json control_authorization(const StoreActor& actor, const ExecutionBinding& b,
                               const std::string& tool_id, bool diagnostic) const;
    void ready(const StoreActor& actor, const ExecutionBinding& b, std::string_view tool_id = {},
               const FrozenRequestConfig* reservation_config = nullptr) const;
    std::optional<Json> replay(const std::string& request_id, const std::string& command_id, const Json& command) const;
    // Additive bookkeeping in the existing immutable event log. No SQL asset
    // changes and no deletion of commands/results needed for exact replay.
    static std::string capacity_control_sql();
    static bool capacity_control(const Row& command);
    static bool capacity_maintenance(std::string_view type);
    static bool capacity_continuation(const Row& command);
    struct CapacityEventState {
        Json saved{Row{}};
        std::int64_t seq{0};
        std::int64_t window{0};
        std::int64_t business{0};
        std::int64_t control{0};
        bool held{false};
        bool warned{false};
    };
    mutable std::map<std::string, CapacityEventState> capacity_event_cache;
    mutable std::map<std::string, Json> capacity_limit_cache;
    mutable std::int64_t capacity_data_version{-1};
    void refresh_capacity_cache() const;
    Json capacity_limits(const std::string& id) const;
    CapacityEventState capacity_events(const std::string& id) const;
    void capacity_event_inserted(const std::string& id, const Row& command);
    Row capacity_totals(const std::string& id) const;
    Row capacity_state(const std::string& id) const;
    void capacity_raw_event(const std::string& id, const std::string& key, const Row& command, const Json& result);
    void observe_capacity(const std::string& id);
    void guard_capacity_event(const std::string& id, const Row& command) const;
    void capacity_available(const std::string& id, const std::string& dimension, const std::string& task_id = {}) const;
    Row checkpoint_intent(const StoreActor& root, std::int64_t expected, const std::string& evidence) const;
    Row checkpoint_state(const std::string& id, std::int64_t expected) const;

    std::int64_t observe_runtime_controls(const std::string& id);

    void event(const std::string& request_id, const std::string& command_id,
               const Row& command, const Json& result, std::string_view actor = "runtime",
               std::size_t payload_limit = 1024U * 1024U);
    void release_claims(const std::string& request_id);
    void supersede_waits(const std::string& request_id, const std::string& task_id = {});
    void route_event(const std::string& request_id, const std::string& type,
                     const Row& command, const Json& result, std::int64_t seq);
    std::optional<Json> check_reconciliation(const StoreActor& root, const std::string& id,
            std::int64_t expected, const std::string& command_id, const Row& command) const;
    void fence_ops(const std::string& request_id, const std::string& team_id = {});
    void unknown_attempts(const std::string& request_id);
    Row current_instruction(const Row& t) const;
    void recover_selected_request(const Row& r);
    void recover(const Row& r);
    void catalog(const std::string& request_id, std::int64_t revision, const Json& value);
    void brief_constraints(const Row& r, const Json& brief) const;
    std::int64_t configured_limit(const Row& r, std::string_view name) const;

    std::map<std::string, Row> independent_nodes(const Row& request, const Json& body) const;
    void check_dispositions(const Json& dispositions, const std::map<std::string, Row>& nodes) const;
    void check_independent_identities(const std::vector<Row>& existing, const std::map<std::string, Row>& nodes) const;
    void check_captured_amendment_tasks(const Row& request, const std::vector<Row>& existing,
                                        const Json& captured, const Json& dispositions,
                                        bool publishing_plan) const;
    Json allocate_independent(const Row& request, std::int64_t revision, const std::string& node_id, const Row& node);
    Json apply_independent_task(const Row& request, const Row& task, std::int64_t revision,
                               const Row& node, const Json& decision);

    Json prepare_operation(const StoreActor& actor, const ExecutionBinding& frozen,
                           std::int64_t expected, const std::string& business_key,
                           const std::string& tool_id, const Json& arguments,
                           const std::string& side_effect, const std::string& command_id,
                           bool registered);

    sqlite3* db{};
    int lease_fd{-1};
    StoreOpenMode mode;
    StoreRecoveryScope recovery_scope;
    bool tools_schema{false};
    bool events_schema{false};
    bool workflow_schema{false};
    bool acceptance_schema{false};
    bool cleanup_schema{false};
    bool allocation_schema{false};
    bool live_schema{false};
    bool reconciliation_schema{false};
    bool has_unsettled_model(const std::string& id, const std::string& team_id = {}) const;
    void live_accounting_ready(const std::string& request_id, const std::string& excluding = {}) const;
    void initialize_usage_projection(const Row& original);
    std::optional<AttemptPricing> attempt_pricing(const Row& original) const;
    bool reservation_accounting(const Row& original) const;
    std::optional<bool> append_priced_accounting_ready(const Row& delta) const;
    Json pricing_accounting_scan(const std::string& id, const std::string& excluding = {},
        const std::string& team_id = {}) const;
    Json attempt_budget_view(const Row& original) const;
    Json pricing_budget_summary(const std::string& id) const;
    Json settle_model_attempt(const std::string& attempt_id, const ExecutionBinding& frozen,
        const std::string& config_hash, const std::string& outcome, const Json& usage, const Json& response,
        bool existing_transaction = false);
    const std::string writer_instance{new_id("writer")};
    Json owned_info(const Row& row) const;
    Row allocation_authorize(const StoreActor& actor, const std::string& id, std::int64_t expected) const;
    Row abandonment_intent(const StoreActor& actor, const std::string& id, std::int64_t expected,
                           const std::string& reason) const;
    Row abandonment_target(const StoreActor& actor, const std::string& id, std::int64_t expected) const;
    Array cleanup_blockers(const std::string& id) const;
    Json cleanup_status(const std::string& id) const;
    void cleanup_authorize(const StoreActor& actor, const Json& claim) const;
    void cleanup_publication(const std::string& id, const Json& publication) const;
    Json cleanup_process_facts(const std::string& id) const;
    std::string database_file_path;
    mutable std::mutex mutex;
};

} // namespace wls::s1
