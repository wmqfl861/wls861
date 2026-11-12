// R1 read-only evidence access. Included after Store::attempt_history so this
// narrow implementation can use the existing private Store transaction/actor
// helpers without widening the trusted-host API or changing SQL assets.
namespace {
template<class Implementation>
Json previous_context_projection(Implementation& impl, const StoreActor& actor,
                                 const FrozenRequestConfig& config) {
    const bool shared = !config.logical_agent_policy().is_null();
    const Array scope = shared ? Array{actor.request_id} : Array{
        actor.request_id, role_name(actor.role), actor.principal_id, actor.task_id, actor.team_id};
    const auto filter = shared ? std::string("request_id=?") : std::string(
        "request_id=? AND role=? AND principal_id=? AND COALESCE(task_id,'')=? AND COALESCE(team_id,'')=?");
    const auto row = impl.optional("SELECT attempt_id,call_id,role,principal_id,frozen_binding,reservation_sha256,"
        "CASE WHEN length(CAST(request_envelope AS BLOB))<=2097152 "
        "THEN request_envelope ELSE NULL END AS request_envelope FROM call_attempts WHERE " + filter +
        " ORDER BY rowid DESC LIMIT 1", scope);
    if (!row) return nullptr;
    require(!row->at("request_envelope").is_null(), ErrorCode::Quota, "prior v3 envelope exceeds bounded recovery limit");
    const auto envelope = parse_json_strict(str(*row, "request_envelope"), 2U * 1024U * 1024U);
    const auto original = model_request_from_json(envelope); // Entire envelope and logical hashes.
    require(envelope.at("format") == "wls-model-request-v3" && original.attempt_id == str(*row, "attempt_id") &&
        original.call_id == str(*row, "call_id") && to_json(original.binding) == parse_json_strict(str(*row, "frozen_binding")) &&
        original.binding.request_id == actor.request_id && original.config_snapshot_id == config.sha256() &&
        (shared || (original.role == actor.role && original.binding.task_id == actor.task_id && original.binding.team_id == actor.team_id)),
        ErrorCode::Corrupt, "prior v3 envelope changed its original scope or frozen configuration");
    StoreActor source{original.role, str(*row, "principal_id"), actor.request_id, original.binding.task_id,
        original.binding.team_id, original.binding.dispatcher_epoch, original.binding.execution_epoch};
    auto command = payload(source, "reserve_attempt", Row{{"binding", to_json(original.binding)},
        {"call_id", original.call_id}, {"attempt_id", original.attempt_id}, {"config_snapshot_sha256", config.sha256()},
        {"model_ref", original.model.model}, {"adapter_kind", config.model_profile(original.role).at("adapter")},
        {"reserved_input_tokens", original.reserved_input_tokens}, {"reserved_output_tokens", original.reserved_output_tokens},
        {"reserved_cost_micro_usd", original.reserved_cost_micro_usd}, {"max_output_tokens", original.max_output_tokens},
        {"request_envelope", envelope}});
    if (impl.acceptance_schema) if (const auto experience = impl.optional(
        "SELECT * FROM experience_attempts WHERE attempt_id=?", {original.attempt_id})) {
        command["experience_job_id"] = experience->at("experience_id");
        command["experience_job_version"] = experience->at("job_version");
        command["experience_source_sha256"] = experience->at("source_sha256");
    }
    require(sha256_hex(canonical_json(command)) == str(*row, "reservation_sha256"), ErrorCode::Corrupt,
        "prior v3 envelope differs from its original reservation hash");
    return Row{{"attempt_id", original.attempt_id}, {"input", original.context.logical_input}};
}

std::string history_accounting_attention(bool reconciliation, const std::string& alias) {
    const auto original = "(" + alias + ".unknown_input=1 OR " + alias +
        ".unknown_output=1 OR " + alias + ".unknown_cost=1)";
    if (!reconciliation) return original;
    const auto live = "SELECT 1 FROM live_attempt_execution l WHERE l.attempt_id=" + alias + ".attempt_id";
    const auto projection = "SELECT 1 FROM live_usage_projection p WHERE p.attempt_id=" + alias + ".attempt_id";
    return "((NOT EXISTS(" + live + ") AND " + original + ") OR (EXISTS(" + live +
        ") AND (NOT EXISTS(" + projection + ") OR EXISTS(" + projection +
        " AND (p.disputed=1 OR p.over_reserved=1 OR p.input_tokens IS NULL OR p.output_tokens IS NULL OR "
        "p.cache_read_tokens IS NULL OR p.cache_write_tokens IS NULL OR p.cost_micro_usd IS NULL)))))";
}


template <typename Implementation>
Json history_unresolved_fact(Implementation& impl, const Row& row) {
    auto fact = exposed(row);
    if (impl.reconciliation_schema && impl.optional("SELECT attempt_id FROM live_attempt_execution WHERE attempt_id=?",
            {row.at("attempt_id")})) {
        const auto projection = impl.optional("SELECT * FROM live_usage_projection WHERE attempt_id=?",
            {row.at("attempt_id")});
        fact.as_object()["live_usage_projection"] = projection ? Json(*projection) : Json(nullptr);
        fact.as_object()["original_usage_immutable"] = true;
    }
    return fact;
}
} // namespace

Json Store::previous_context(const StoreActor& actor) const {
    const std::lock_guard lock(impl_->mutex);
    const auto request = impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    const auto config = FrozenRequestConfig::restore(str(request, "config_snapshot"), str(request, "config_snapshot_sha256"));
    require(config.context_request_format() == "structured-v3", ErrorCode::Unauthorized, "v3 recovery requires frozen v3 policy");
    return previous_context_projection(*impl_, actor, config);
}

Json Store::attempt_history_page(const StoreActor& actor, std::int64_t after,
        std::int64_t through, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema();
    require(after >= 0 && through >= 0 && limit > 0 && limit <= 64,
            ErrorCode::InvalidInput, "history page requires nonnegative cursors and limit 1..64");
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    const Array scope{actor.request_id, role_name(actor.role), actor.principal_id,
                      actor.task_id, actor.team_id};
    const std::string filter = "request_id=? AND role=? AND principal_id=? AND "
        "COALESCE(task_id,'')=? AND COALESCE(team_id,'')=?";
    // A caller may never use another actor's cursor as a proxy for ownership.
    const auto check_cursor = [&](std::int64_t cursor) {
        if (cursor == 0) return;
        auto parameters = scope; parameters.push_back(cursor);
        require(impl_->optional("SELECT rowid FROM call_attempts WHERE " + filter + " AND rowid=?",
                                parameters).has_value(), ErrorCode::Unauthorized,
                "history cursor does not belong to this actor scope");
    };
    check_cursor(after); check_cursor(through);
    if (through == 0) through = number(impl_->one(
        "SELECT COALESCE(MAX(rowid),0) AS maximum FROM call_attempts WHERE " + filter, scope), "maximum");
    require(after <= through, ErrorCode::InvalidInput, "history cursor exceeds its captured high watermark");
    auto parameters = scope; parameters.push_back(after); parameters.push_back(through);
    parameters.push_back(limit + 1);
    // In particular, do not SELECT request_envelope, response or feedback body.
    // Fixed high water bounds membership even if another connection appends.
    auto rows = impl_->all("SELECT a.rowid AS sequence,a.attempt_id,a.call_id,a.role,"
        "a.request_id,a.task_id,a.team_id,a.principal_id,a.config_snapshot_sha256,"
        "a.frozen_binding,a.status,a.accepted,a.settlement_sha256,"
        "(SELECT record_sha256 FROM model_tool_feedback f WHERE f.attempt_id=a.attempt_id) AS record_sha256 "
        "FROM call_attempts a WHERE " + filter + " AND a.rowid>? AND a.rowid<=? ORDER BY a.rowid LIMIT ?",
        parameters);
    const bool more = rows.size() > static_cast<std::size_t>(limit);
    if (more) rows.pop_back();
    Array items;
    for (const auto& row : rows) {
        auto value = exposed(row);
        value.as_object()["feedback_missing"] = row.at("record_sha256").is_null();
        // Preserve original binding and distinguish eligibility from success.
        value.as_object()["accepted_is_business_success"] = false;
        items.push_back(std::move(value));
    }
    Json next_after = nullptr;
    if (more) next_after = number(rows.back(), "sequence");
    return Row{{"format", "wls-history-index-v1"}, {"request_id", actor.request_id},
        {"task_id", actor.task_id}, {"team_id", actor.team_id}, {"role", role_name(actor.role)},
        {"principal_id", actor.principal_id}, {"after_sequence", after},
        {"through_sequence", through}, {"items", std::move(items)},
        {"has_more", more}, {"next_after_sequence", next_after},
        {"snapshot_kind", "insertion_membership_only"}};
}

Json Store::read_history_feedback(const StoreActor& actor, const std::string& attempt_id,
        const std::string& expected_hash, const std::string& section,
        std::int64_t offset, std::int64_t max_bytes) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema();
    text_field(attempt_id, "history attempt id");
    require(expected_hash.size() == 64 && std::all_of(expected_hash.begin(), expected_hash.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
        ErrorCode::InvalidInput, "history read requires a lowercase SHA-256 record fingerprint");
    require((section == "response" || section == "result") && offset >= 0 &&
            max_bytes > 0 && max_bytes <= 16384, ErrorCode::InvalidInput,
            "history read requires response/result, nonnegative offset and max_bytes 1..16384");
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    const auto attempt = impl_->optional("SELECT frozen_binding,response FROM call_attempts WHERE "
        "attempt_id=? AND request_id=? AND role=? AND principal_id=? AND COALESCE(task_id,'')=? "
        "AND COALESCE(team_id,'')=?", {attempt_id, actor.request_id, role_name(actor.role),
                                    actor.principal_id, actor.task_id, actor.team_id});
    require(attempt.has_value(), ErrorCode::Unauthorized, "history attempt does not belong to this actor scope");
    const auto feedback = impl_->feedback_info(attempt_id); // Verifies complete immutable fact hash.
    require(!feedback.is_null(), ErrorCode::NotReady, "original feedback was not recorded; history cannot invent it");
    require(str(feedback.as_object(), "record_sha256") == expected_hash, ErrorCode::Conflict,
            "history record fingerprint differs from the requested original");
    require(feedback.at("original_binding") == parse_json_strict(str(*attempt, "frozen_binding")) &&
            !attempt->at("response").is_null() &&
            feedback.at("response") == parse_json_strict(str(*attempt, "response"), 4U * 1024U * 1024U),
            ErrorCode::Corrupt, "history feedback differs from its settled original attempt");
    const auto bytes = json_text(feedback.at(section), 4U * 1024U * 1024U);
    require(static_cast<std::uint64_t>(offset) <= bytes.size(), ErrorCode::InvalidInput,
            "history byte offset exceeds the immutable section");
    const auto start = static_cast<std::size_t>(offset);
    const auto count = std::min(static_cast<std::size_t>(max_bytes), bytes.size() - start);
    const auto end = start + count;
    return Row{{"format", "wls-history-bytes-v1"}, {"attempt_id", attempt_id},
        {"record_sha256", expected_hash}, {"original_binding", feedback.at("original_binding")},
        {"section", section}, {"section_sha256", sha256_hex(bytes)},
        {"encoding", "base64-canonical-json-utf8"}, {"size_bytes", bytes.size()},
        {"offset", offset}, {"bytes_returned", count},
        {"content_base64", base64_bytes(std::string_view(bytes).substr(start, count))},
        {"eof", end == bytes.size()}, {"next_offset", end == bytes.size() ? Json(nullptr) : Json(end)}};
}


Json Store::attempt_history_tail(const StoreActor& actor, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema();
    require(limit >= 1 && limit <= 8, ErrorCode::InvalidInput, "history tail limit must be 1..8");
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    const Array scope{actor.request_id, role_name(actor.role), actor.principal_id, actor.task_id, actor.team_id};
    const std::string filter = "request_id=? AND role=? AND principal_id=? AND COALESCE(task_id,'')=? AND COALESCE(team_id,'')=?";
    const auto counts = impl_->one("SELECT COUNT(*) AS n,COALESCE(MAX(rowid),0) AS high FROM call_attempts WHERE " + filter, scope);
    // A read-only Store can coexist with the exclusive writer. Keep the tail
    // within the same insertion membership captured by COUNT/MAX, even when
    // a new attempt commits between these two statements. Settlement state
    // and separately pinned unresolved facts intentionally remain live.
    auto parameters = scope;
    parameters.push_back(counts.at("high")); parameters.push_back(limit);
    auto rows = impl_->all("SELECT rowid AS sequence,attempt_id,call_id,frozen_binding,status,accepted,response "
        "FROM call_attempts WHERE " + filter + " AND rowid<=? ORDER BY rowid DESC LIMIT ?", parameters);
    Array items;
    for (auto entry = rows.rbegin(); entry != rows.rend(); ++entry) {
        auto record = exposed(*entry);
        const auto feedback = impl_->feedback_info(str(*entry, "attempt_id"));
        if (!feedback.is_null())
            require(feedback.at("original_binding") == record.at("frozen_binding") &&
                feedback.at("response") == record.at("response"), ErrorCode::Corrupt,
                "recent history feedback differs from its original settled attempt");
        record.as_object()["feedback"] = feedback;
        record.as_object()["feedback_missing"] = feedback.is_null();
        // Bound each original before the runtime projects it, independently of
        // total lifetime rows. At most eight such records are ever loaded.
        (void)json_text(record, 8U * 1024U * 1024U);
        items.push_back(std::move(record));
    }
    auto unresolved = impl_->all("SELECT * FROM ops WHERE request_id=? AND task_id=? AND team_id=? "
        "AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN') ORDER BY rowid LIMIT 65",
        {actor.request_id, actor.task_id, actor.team_id});
    require(unresolved.size() <= 64, ErrorCode::Quota, "unresolved operation facts cannot fit; reconcile before another model call");
    Array operations;
    for (const auto& operation : unresolved) operations.push_back(exposed(operation));
    const auto pending_calls = impl_->all("SELECT a.attempt_id,a.call_id,a.frozen_binding,a.status,a.accepted,"
        "a.unknown_input,a.unknown_output,a.unknown_cost FROM call_attempts a WHERE " + filter +
        " AND (a.status IN ('PENDING','UNKNOWN') OR " + history_accounting_attention(impl_->reconciliation_schema, "a") +
        " OR NOT EXISTS(SELECT 1 FROM model_tool_feedback f WHERE f.attempt_id=a.attempt_id)) ORDER BY a.rowid LIMIT 65", scope);
    require(pending_calls.size() <= 64, ErrorCode::Quota,
        "unresolved attempt facts cannot fit; reconcile missing feedback/accounting before another model call");
    Array unsettled;
    for (const auto& call : pending_calls) unsettled.push_back(history_unresolved_fact(*impl_, call));
    const auto n = number(counts, "n");
    return Row{{"format", "wls-history-tail-v1"}, {"request_id", actor.request_id},
        {"task_id", actor.task_id}, {"team_id", actor.team_id}, {"principal_id", actor.principal_id},
        {"through_sequence", counts.at("high")}, {"total_count", n},
        {"omitted_count", n - static_cast<std::int64_t>(items.size())},
        {"snapshot_kind", "insertion_membership_only"}, {"unresolved_operations", operations},
        {"unresolved_attempts", unsettled}, {"items", items}};
}

std::vector<std::string> Store::authorized_history_tools(const StoreActor& actor,
        const ExecutionBinding& current) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema();
    const auto r = impl_->authorize(actor, {Role::Worker});
    impl_->actor_binding(actor, current); impl_->matches(current);
    const auto frozen = FrozenRequestConfig::restore(str(r, "config_snapshot"), str(r, "config_snapshot_sha256"));
    if (frozen.worker_history_policy().is_null()) return {};
    const auto denied = parse_json_strict(str(impl_->one("SELECT denied_tools FROM runtime_policy WHERE singleton=1"), "denied_tools"));
    std::vector<std::string> result;
    for (const auto* name : {"history_page", "history_read"})
        if (!contains(denied, name)) result.emplace_back(name);
    return result;
}

void Store::authorize_history_control(const StoreActor& actor, const ExecutionBinding& current,
        const std::string& name) const {
    const auto available = authorized_history_tools(actor, current);
    require(std::find(available.begin(), available.end(), name) != available.end(), ErrorCode::Unauthorized,
        "history control is not enabled by the frozen/current policy");
    const std::lock_guard lock(impl_->mutex);
    const auto r = impl_->authorize(actor, {Role::Worker});
    impl_->actor_binding(actor, current); impl_->matches(current);
    const auto frozen = FrozenRequestConfig::restore(str(r, "config_snapshot"), str(r, "config_snapshot_sha256"));
    const auto limit = number(frozen.json().at("system").at("control").as_object(), "max_queries_per_minute");
    // The current response is already settled before dispatch. Counting all
    // attempts, including rejected reads, closes restart and failure loopholes.
    const auto used = number(impl_->one("SELECT COUNT(*) AS n FROM call_attempts WHERE request_id=? AND task_id=? "
        "AND team_id=? AND principal_id=? AND role='worker' AND settled_at>=strftime('%Y-%m-%dT%H:%M:%SZ','now','-60 seconds') "
        "AND json_extract(response,'$.tool_calls[0].name') IN ('history_page','history_read')",
        {actor.request_id, actor.task_id, actor.team_id, actor.principal_id}), "n");
    require(used <= limit, ErrorCode::Quota, "persistent history control rate limit exhausted");
}

namespace {
// All R1.4 reads hold one SQLite read snapshot. A read-only connection may
// coexist with the writer; its old membership must never be filled with later
// rows between COUNT/MAX, current facts and the recent tail.
class ManagementReadSnapshot {
public:
    explicit ManagementReadSnapshot(sqlite3* database) : db_(database), owned_(sqlite3_get_autocommit(db_) != 0) {
        if (owned_) sqlite_failure(db_, sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr), "management snapshot");
    }
    ~ManagementReadSnapshot() { if (owned_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void finish() {
        if (owned_) {
            sqlite_failure(db_, sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr), "management snapshot end");
            owned_ = false;
        }
    }
    ManagementReadSnapshot(const ManagementReadSnapshot&) = delete;
    ManagementReadSnapshot& operator=(const ManagementReadSnapshot&) = delete;
private:
    sqlite3* db_;
    bool owned_;
};

Json management_scope(const StoreActor& actor) {
    return Row{{"request_id", actor.request_id}, {"role", role_name(actor.role)},
        {"principal_id", actor.principal_id}, {"task_id", actor.task_id}, {"team_id", actor.team_id}};
}

template<class Implementation>
void bounded_history_feedback(Implementation& impl, const std::string& id, std::int64_t maximum) {
    const auto sizes = impl.optional("SELECT length(CAST(original_binding AS BLOB)) AS binding_size,"
        "length(CAST(response AS BLOB)) AS response_size,length(CAST(result AS BLOB)) AS result_size FROM model_tool_feedback WHERE attempt_id=?", {id});
    if (sizes) require(number(*sizes, "binding_size") <= 65536 && number(*sizes, "response_size") <= maximum &&
        number(*sizes, "result_size") <= maximum, ErrorCode::Quota, "original feedback exceeds bounded history read capacity");
}

template<class Implementation>
Json append_history_delta(Implementation& impl, const StoreActor& actor, const Json& previous) {
    const auto after = previous.is_null() ? 0 : number(previous.at("input").at("worker_history").as_object(), "last_sequence");
    const Array scope{actor.request_id, role_name(actor.role), actor.principal_id, actor.task_id, actor.team_id};
    const std::string filter = "request_id=? AND role=? AND principal_id=? AND COALESCE(task_id,'')=? AND COALESCE(team_id,'')=?";
    const auto high = number(impl.one("SELECT COALESCE(MAX(rowid),0) AS high FROM call_attempts WHERE " + filter, scope), "high");
    require(after >= 0 && after <= high, ErrorCode::Conflict, "append-fold consumed watermark is stale");
    auto parameters = scope; parameters.emplace_back(after); parameters.emplace_back(high);
    const auto rows = impl.all("SELECT rowid AS sequence,attempt_id,call_id,frozen_binding,status,accepted,"
        "CASE WHEN length(CAST(response AS BLOB))<=1048576 THEN response ELSE NULL END AS response,length(CAST(response AS BLOB)) AS response_size," +
        history_accounting_attention(impl.reconciliation_schema, "call_attempts") + " AS accounting_attention FROM call_attempts WHERE " +
        filter + " AND rowid>? AND rowid<=? ORDER BY rowid LIMIT 65", parameters);
    require(rows.size() <= 64, ErrorCode::Quota, "append-fold delta exceeds 64 original turns");
    Array items;
    for (const auto& row : rows) {
        require(row.at("response_size").is_null() || number(row, "response_size") <= 1048576, ErrorCode::Quota,
            "append-fold original response exceeds bounded delta capacity");
        bounded_history_feedback(impl, str(row, "attempt_id"), 1048576);
        auto item = exposed(row); const auto feedback = impl.feedback_info(str(row, "attempt_id"));
        if (const auto ready = impl.append_priced_accounting_ready(row))
            item.as_object()["accounting_attention"] = *ready ? 0 : 1;
        // Never consume an unfinished rowid: late original feedback/usage then
        // remains in the next bounded delta instead of needing lifetime refresh.
        require(str(row, "status") != "PENDING" && str(row, "status") != "UNKNOWN" &&
            number(item.as_object(), "accounting_attention") == 0 && !feedback.is_null(), ErrorCode::NotReady,
            "append-fold continuation awaits original feedback and complete accounting");
        if (!feedback.is_null()) require(feedback.at("original_binding") == item.at("frozen_binding") &&
            feedback.at("response") == item.at("response"), ErrorCode::Corrupt, "delta feedback differs from original attempt");
        item.as_object()["feedback"] = feedback;
        items.push_back(std::move(item));
        (void)json_text(items, 1024U * 1024U); // Aggregate bound; no lifetime envelope reads.
    }
    return Row{{"scope", management_scope(actor)}, {"through_sequence", high}, {"items", items}};
}

template <typename Implementation>
FrozenRequestConfig management_authorize(Implementation& impl, const StoreActor& actor,
        const ExecutionBinding& current, const std::string& tool = {}) {
    impl.require_events_schema(); impl.require_workflow_schema();
    const auto request = impl.authorize(actor, {Role::Dispatcher, Role::TeamLead});
    impl.actor_binding(actor, current); impl.matches(current);
    const auto config = FrozenRequestConfig::restore(str(request, "config_snapshot"), str(request, "config_snapshot_sha256"));
    require(!config.management_history_policy().is_null(), ErrorCode::Unauthorized,
            "management history is not enabled by the frozen request");
    if (!tool.empty()) {
        require(tool == "management_history_page" || tool == "management_history_read", ErrorCode::Unauthorized,
                "unknown management history control");
        const auto denied = parse_json_strict(str(impl.one("SELECT denied_tools FROM runtime_policy WHERE singleton=1"), "denied_tools"));
        require(!contains(denied, tool), ErrorCode::Unauthorized, "management history control was revoked by current policy");
    }
    return config;
}

struct ManagementSource {
    std::string kind;
    std::string table;
    std::string id;
    std::string filter;
    std::string columns;
    Array parameters;
};


ManagementSource management_source(const StoreActor& actor, const std::string& kind, bool reconciliation = false) {
    require(kind == "question" || kind == "submission" || kind == "attempt", ErrorCode::InvalidInput,
            "management history kind must be question/submission/attempt");
    ManagementSource source{kind, {}, {}, "q.request_id=?", {}, {actor.request_id}};
    if (kind == "attempt") {
        source.table = "call_attempts q"; source.id = "attempt_id";
        source.filter += " AND q.role=? AND q.principal_id=? AND COALESCE(q.task_id,'')=? AND COALESCE(q.team_id,'')=?";
        source.parameters.emplace_back(role_name(actor.role)); source.parameters.emplace_back(actor.principal_id);
        source.parameters.emplace_back(actor.task_id); source.parameters.emplace_back(actor.team_id);
        source.columns = "q.rowid AS sequence,q.attempt_id AS record_id,1 AS record_version,"
            "q.reservation_sha256 AS original_sha256,q.frozen_binding AS original_binding,q.status,"
            "q.unknown_input,q.unknown_output,q.unknown_cost,"
            "(SELECT record_sha256 FROM model_tool_feedback f WHERE f.attempt_id=q.attempt_id) AS feedback_sha256," +
            history_accounting_attention(reconciliation, "q") + " AS accounting_attention";
    } else {
        if (actor.role == Role::TeamLead) {
            source.filter += " AND q.task_id=? AND q.team_id=?";
            source.parameters.emplace_back(actor.task_id); source.parameters.emplace_back(actor.team_id);
        }
        if (kind == "question") {
            source.table = "questions q JOIN question_runtime x USING(question_id)"; source.id = "question_id";
            source.columns = "q.rowid AS sequence,q.question_id AS record_id,q.state_version AS record_version,"
                "x.original_sha256,x.original_binding,x.status";
        } else {
            source.table = "workflow_submissions q"; source.id = "submission_id";
            source.columns = "q.rowid AS sequence,q.submission_id AS record_id,q.state_version AS record_version,"
                "q.record_sha256 AS original_sha256,q.original_binding,q.status";
        }
    }
    return source;
}

bool management_completed(const ManagementSource& source, const Row& row) {
    const auto status = str(row, "status");
    if (source.kind == "question") return status != "OPEN";
    if (source.kind == "submission") return status == "RETURNED_FOR_REWORK" || status == "SUPERSEDED";
    return status != "PENDING" && status != "UNKNOWN" && number(row, "accounting_attention") == 0 &&
        !row.at("feedback_sha256").is_null();
}

template <typename Implementation>
Json management_original(Implementation& impl, const StoreActor& actor,
        const ManagementSource& source, const Row& row) {
    const auto id = str(row, "record_id");
    Json original;
    if (source.kind == "question") original = impl.question_info(id);
    else if (source.kind == "submission") {
        original = impl.submission_info(id);
        // These are live projections, not part of this original submission.
        // They must not change the fingerprint of a closed historical record.
        for (const auto* key : {"team_rework_count", "business_accepted", "business_delivered", "acceptance"})
            original.as_object().erase(key);
    } else {
        const auto feedback = impl.feedback_info(id);
        if (feedback.is_null()) return nullptr;
        const auto attempt = impl.one("SELECT frozen_binding,response FROM call_attempts WHERE attempt_id=?", {id});
        require(!attempt.at("response").is_null() && feedback.at("original_binding") ==
                parse_json_strict(str(attempt, "frozen_binding")) && feedback.at("response") ==
                parse_json_strict(str(attempt, "response"), 4U * 1024U * 1024U), ErrorCode::Corrupt,
                "management feedback differs from its original settled attempt");
        original = feedback;
    }
    const Json record = Row{{"format", "wls-management-original-v1"}, {"scope", management_scope(actor)},
        {"kind", source.kind}, {"record_id", id}, {"record_version", row.at("record_version")},
        {"original", original}};
    (void)json_text(record, 8U * 1024U * 1024U);
    return record;
}

template <typename Implementation>
Json management_catalog(Implementation& impl, const StoreActor& actor, const ManagementSource& source,
        std::int64_t through = 0) {
    if (through == 0) through = number(impl.one("SELECT COALESCE(MAX(q.rowid),0) AS n FROM " +
        source.table + " WHERE " + source.filter, source.parameters), "n");
    auto parameters = source.parameters; parameters.push_back(through);
    // Constant-memory scan of metadata only. The digest fixes insertion
    // membership and immutable original fingerprints; mutable completion state
    // is explicitly not represented as an old captured authorization.
    Statement query(impl.db, "SELECT " + source.columns + " FROM " + source.table + " WHERE " +
        source.filter + " AND q.rowid<=? ORDER BY q.rowid", parameters);
    auto hash = sha256_hex(canonical_json(Row{{"format", "wls-management-membership-v1"},
        {"scope", management_scope(actor)}, {"kind", source.kind}, {"through_sequence", through}}));
    std::int64_t count = 0, completed = 0;
    while (query.step()) {
        const auto row = query.row();
        hash = sha256_hex(hash + canonical_json(Row{{"sequence", row.at("sequence")},
            {"record_id", row.at("record_id")}, {"original_sha256", row.at("original_sha256")}}));
        ++count;
        if (management_completed(source, row)) ++completed;
    }
    return Row{{"kind", source.kind}, {"through_sequence", through}, {"total_count", count},
        {"completed_count", completed}, {"catalog_sha256", hash},
        {"snapshot_kind", "insertion_membership_only"}, {"not_execution_authority", true},
        {"retrieval", Row{{"tool", "management_history_page"}, {"arguments", Row{
            {"kind", source.kind}, {"after_sequence", 0}, {"through_sequence", through}, {"limit", 4}}}}}};
}

template <typename Implementation>
void management_cursor(Implementation& impl, const ManagementSource& source, std::int64_t cursor) {
    if (cursor == 0) return;
    auto parameters = source.parameters; parameters.push_back(cursor);
    require(impl.optional("SELECT q.rowid FROM " + source.table + " WHERE " + source.filter +
            " AND q.rowid=?", parameters).has_value(), ErrorCode::Unauthorized,
            "management history cursor is outside the current role/request/target scope");
}
} // namespace

Json Store::append_history_capture(const StoreActor& actor) const {
    const std::lock_guard lock(impl_->mutex);
    ManagementReadSnapshot snapshot(impl_->db);
    const auto request = impl_->authorize(actor, {Role::Worker});
    const auto config = FrozenRequestConfig::restore(str(request, "config_snapshot"), str(request, "config_snapshot_sha256"));
    require(append_fold_policy(config.worker_history_policy()), ErrorCode::Unauthorized, "append-fold history is not frozen");
    const auto previous = previous_context_projection(*impl_, actor, config);
    const Json result = Row{{"previous", previous}, {"delta", append_history_delta(*impl_, actor, previous)}};
    snapshot.finish();
    return result;
}

Json Store::read_history_sequence(const StoreActor& actor, std::int64_t sequence,
        const std::string& section, std::int64_t offset, std::int64_t max_bytes) const {
    const std::lock_guard lock(impl_->mutex);
    ManagementReadSnapshot snapshot(impl_->db);
    const auto request = impl_->authorize(actor, {Role::Worker});
    const auto config = FrozenRequestConfig::restore(str(request, "config_snapshot"), str(request, "config_snapshot_sha256"));
    const auto policy = config.worker_history_policy();
    require(append_fold_policy(policy), ErrorCode::Unauthorized, "text history requires frozen append-fold policy");
    require(sequence > 0 && offset >= 0 && max_bytes > 0 && max_bytes <= number(policy.as_object(), "read_max_bytes") &&
        (section == "response" || section == "result" || section == "result_data"), ErrorCode::InvalidInput, "invalid text history page bounds or section");
    const auto row = impl_->optional("SELECT attempt_id,frozen_binding,accepted,length(CAST(response AS BLOB)) AS response_size,"
        "CASE WHEN length(CAST(response AS BLOB))<=4194304 THEN response ELSE NULL END AS response FROM call_attempts WHERE rowid=? AND "
        "request_id=? AND role=? AND principal_id=? AND COALESCE(task_id,'')=? AND COALESCE(team_id,'')=?",
        {sequence, actor.request_id, role_name(actor.role), actor.principal_id, actor.task_id, actor.team_id});
    require(row.has_value(), ErrorCode::Unauthorized, "history sequence is outside the current actor scope");
    require(row->at("response_size").is_null() || number(*row, "response_size") <= 4194304, ErrorCode::Quota,
        "original history response exceeds 4 MiB");
    const auto id = str(*row, "attempt_id"); bounded_history_feedback(*impl_, id, 4194304);
    const auto feedback = impl_->feedback_info(id);
    require(!feedback.is_null(), ErrorCode::NotReady, "original feedback is missing");
    require(feedback.at("original_binding") == parse_json_strict(str(*row, "frozen_binding")) &&
        !row->at("response").is_null() && feedback.at("response") == parse_json_strict(str(*row, "response"), 4U * 1024U * 1024U),
        ErrorCode::Corrupt, "text history differs from settled original binding/response");
    std::string bytes;
    if (section == "result_data") {
        const auto response = model_response_from_json(feedback.at("response"));
        require(number(*row, "accepted") == 1 && response.status == ModelResultStatus::Completed &&
            response.tool_calls.size() == 1 && response.tool_calls.front().name == "fs_read", ErrorCode::InvalidInput,
            "result_data requires a completed original fs_read");
        const auto& page = feedback.at("result");
        object_fields(page, {"path", "offset", "bytes_returned", "size_bytes", "sha256", "content", "content_base64", "eof", "next_offset"},
            {"path", "offset", "bytes_returned", "size_bytes", "sha256", "content", "content_base64", "eof", "next_offset"});
        bytes = decoded_base64(str(page.as_object(), "content_base64"));
        const auto begin = number(page.as_object(), "offset"), size = number(page.as_object(), "size_bytes");
        const auto& args = response.tool_calls.front().arguments;
        const auto& hash = str(page.as_object(), "sha256");
        require(page.at("path") == args.at("path") && begin == (args.as_object().contains("offset") ? number(args.as_object(), "offset") : 0) &&
            bytes.size() <= static_cast<std::uint64_t>(args.as_object().contains("max_bytes") ? number(args.as_object(), "max_bytes") : 65536) &&
            hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }),
            ErrorCode::Corrupt, "result_data differs from original fs_read range or file identity");
        require(begin >= 0 && size >= begin && number(page.as_object(), "bytes_returned") == static_cast<std::int64_t>(bytes.size()) &&
            bytes.size() <= static_cast<std::uint64_t>(size - begin) && page.at("eof") == (begin + static_cast<std::int64_t>(bytes.size()) == size) &&
            (page.at("content").is_null() || page.at("content") == Json(bytes)) &&
            (page.at("eof") == true ? page.at("next_offset").is_null() : page.at("next_offset") == begin + static_cast<std::int64_t>(bytes.size())),
            ErrorCode::Corrupt, "invalid original fs_read payload");
        if (begin == 0 && static_cast<std::int64_t>(bytes.size()) == size)
            require(sha256_hex(bytes) == str(page.as_object(), "sha256"), ErrorCode::Corrupt, "original fs_read full payload hash differs");
    } else bytes = json_text(feedback.at(section), 4U * 1024U * 1024U);
    bool utf8 = true;
    try { (void)canonical_json(Json(bytes)); } catch (const Error&) { utf8 = false; }
    require(static_cast<std::uint64_t>(offset) <= bytes.size(), ErrorCode::InvalidInput, "history offset exceeds original payload");
    const auto start = static_cast<std::size_t>(offset);
    auto end = start + std::min(static_cast<std::size_t>(max_bytes), bytes.size() - start);
    const auto continuation = [&](std::size_t pos) { return pos < bytes.size() && (static_cast<unsigned char>(bytes[pos]) & 0xc0U) == 0x80U; };
    if (utf8) {
        require(!continuation(start), ErrorCode::InvalidInput, "history offset splits a UTF-8 codepoint");
        while (end > start && continuation(end)) --end;
        require(end > start || start == bytes.size(), ErrorCode::InvalidInput, "history page cannot fit one UTF-8 codepoint");
    }
    const auto page = std::string_view(bytes).substr(start, end - start);
    Json result = Row{{"format", "wls-history-text-v1"}, {"sequence", sequence}, {"attempt_id", id},
        {"record_sha256", feedback.at("record_sha256")}, {"original_binding", feedback.at("original_binding")},
        {"section", section}, {"payload_sha256", sha256_hex(bytes)}, {"size_bytes", bytes.size()}, {"offset", offset},
        {"bytes_returned", page.size()}, {"encoding", utf8 ? "utf-8" : "base64"}, {"eof", end == bytes.size()},
        {"next_offset", end == bytes.size() ? Json(nullptr) : Json(end)}};
    result.as_object()[utf8 ? "content" : "content_base64"] = utf8 ? std::string(page) : base64_bytes(page);
    require(canonical_json(result).size() <= static_cast<std::uint64_t>(number(policy.as_object(), "read_token_budget")),
        ErrorCode::Quota, "serialized history page exceeds frozen conservative token budget");
    snapshot.finish();
    (void)impl_->authorize(actor, {Role::Worker});
    return result;
}

std::vector<std::string> Store::authorized_management_tools(const StoreActor& actor,
        const ExecutionBinding& current) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_events_schema(); impl_->require_workflow_schema();
    const auto request = impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead});
    impl_->actor_binding(actor, current); impl_->matches(current);
    const auto config = FrozenRequestConfig::restore(str(request, "config_snapshot"), str(request, "config_snapshot_sha256"));
    if (config.management_history_policy().is_null()) return {};
    const auto denied = parse_json_strict(str(impl_->one("SELECT denied_tools FROM runtime_policy WHERE singleton=1"), "denied_tools"));
    std::vector<std::string> result;
    for (const auto* name : {"management_history_page", "management_history_read"})
        if (!contains(denied, name)) result.emplace_back(name);
    return result;
}

void Store::authorize_management_control(const StoreActor& actor, const ExecutionBinding& current,
        const std::string& name) const {
    const std::lock_guard lock(impl_->mutex);
    const auto config = management_authorize(*impl_, actor, current, name);
    const auto maximum = number(config.json().at("system").at("control").as_object(), "max_queries_per_minute");
    const auto used = number(impl_->one("SELECT COUNT(*) AS n FROM call_attempts WHERE request_id=? AND role=? "
        "AND principal_id=? AND COALESCE(task_id,'')=? AND COALESCE(team_id,'')=? "
        "AND settled_at>=strftime('%Y-%m-%dT%H:%M:%SZ','now','-60 seconds') "
        "AND json_extract(response,'$.tool_calls[0].name') IN ('management_history_page','management_history_read')",
        {actor.request_id, role_name(actor.role), actor.principal_id, actor.task_id, actor.team_id}), "n");
    require(used <= maximum, ErrorCode::Quota, "persistent management history query limit exhausted");
}

Json Store::management_view(const StoreActor& actor, const ExecutionBinding& current) const {
    const std::lock_guard lock(impl_->mutex);
    ManagementReadSnapshot snapshot(impl_->db);
    const auto config = management_authorize(*impl_, actor, current);
    const auto policy = config.management_history_policy();
    const auto question_source = management_source(actor, "question");
    const auto submission_source = management_source(actor, "submission");
    const auto attempt_source = management_source(actor, "attempt", impl_->reconciliation_schema);
    Json catalogs = Row{{"question", management_catalog(*impl_, actor, question_source)},
        {"submission", management_catalog(*impl_, actor, submission_source)},
        {"attempt", management_catalog(*impl_, actor, attempt_source)}};
    const auto open = impl_->all("SELECT q.question_id FROM " + question_source.table + " WHERE " +
        question_source.filter + " AND x.status='OPEN' ORDER BY q.rowid LIMIT 65", question_source.parameters);
    require(open.size() <= 64, ErrorCode::Quota,
        "management context capacity: section=questions OPEN count exceeds 64; resolve outstanding questions before reservation");
    Array questions;
    for (const auto& row : open) questions.push_back(impl_->question_info(str(row, "question_id")));
    const auto descendant = actor.role == Role::Dispatcher ? std::string("request_id=?") :
        std::string("request_id=? AND task_id=? AND team_id=?");
    const Array scope = actor.role == Role::Dispatcher ? Array{actor.request_id} :
        Array{actor.request_id, actor.task_id, actor.team_id};
    const auto operations = impl_->all("SELECT * FROM ops WHERE " + descendant +
        " AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN') ORDER BY rowid LIMIT 65", scope);
    require(operations.size() <= 64, ErrorCode::Quota,
        "management context capacity: section=unresolved_operations count exceeds 64; reconcile original outcomes before reservation");
    Array unresolved_operations;
    for (const auto& operation : operations) unresolved_operations.push_back(exposed(operation));
    const auto unsettled = impl_->all("SELECT attempt_id,call_id,role,principal_id,task_id,team_id,frozen_binding,status,"
        "accepted,unknown_input,unknown_output,unknown_cost,input_tokens,output_tokens,cost_micro_usd,usage,response "
        "FROM call_attempts WHERE " + descendant + " AND (status IN ('PENDING','UNKNOWN') OR " +
        history_accounting_attention(impl_->reconciliation_schema, "call_attempts") +
        " OR NOT EXISTS(SELECT 1 FROM model_tool_feedback f "
        "WHERE f.attempt_id=call_attempts.attempt_id)) ORDER BY rowid LIMIT 65", scope);
    require(unsettled.size() <= 64, ErrorCode::Quota,
        "management context capacity: section=unresolved_attempts count exceeds 64; reconcile usage and missing feedback before reservation");
    Array unresolved_attempts;
    for (const auto& attempt : unsettled) unresolved_attempts.push_back(history_unresolved_fact(*impl_, attempt));
    auto parameters = attempt_source.parameters;
    parameters.push_back(catalogs.at("attempt").at("through_sequence"));
    parameters.push_back(policy.at("recent_attempts"));
    auto recent = impl_->all("SELECT " + attempt_source.columns + " FROM " + attempt_source.table + " WHERE " +
        attempt_source.filter + " AND q.rowid<=? ORDER BY q.rowid DESC LIMIT ?", parameters);
    Array attempts;
    std::int64_t visible_completed = 0;
    for (auto entry = recent.rbegin(); entry != recent.rend(); ++entry) {
        const auto id = str(*entry, "record_id");
        const auto row = impl_->one("SELECT attempt_id,call_id,frozen_binding,status,accepted,response FROM call_attempts WHERE attempt_id=?", {id});
        auto item = exposed(row);
        const auto original = management_original(*impl_, actor, attempt_source, *entry);
        const auto feedback = impl_->feedback_info(id);
        item.as_object()["sequence"] = entry->at("sequence");
        item.as_object()["feedback"] = feedback;
        item.as_object()["feedback_missing"] = feedback.is_null();
        item.as_object()["management_record_sha256"] = original.is_null() ? Json(nullptr) : Json(sha256_hex(canonical_json(original)));
        (void)json_text(item, 8U * 1024U * 1024U);
        attempts.push_back(std::move(item));
        if (management_completed(attempt_source, *entry)) ++visible_completed;
    }
    // Inserting into a boost::json::object may invalidate references to its
    // existing values. Read scalar counts before any insertion, rather than
    // assigning from an aliased value in the same catalog object.
    const auto completed_questions = number(catalogs.at("question").as_object(), "completed_count");
    const auto completed_submissions = number(catalogs.at("submission").as_object(), "completed_count");
    const auto omitted_attempts = number(catalogs.at("attempt").as_object(), "completed_count") - visible_completed;
    catalogs.at("question").as_object()["omitted_completed_count"] = completed_questions;
    catalogs.at("submission").as_object()["omitted_completed_count"] = completed_submissions;
    catalogs.at("attempt").as_object()["omitted_completed_count"] = omitted_attempts;
    const Json view = Row{{"format", "wls-management-view-v1"}, {"scope", management_scope(actor)},
        {"current_binding", to_json(current)}, {"history_catalogs", catalogs}, {"questions", questions},
        {"unresolved_operations", unresolved_operations}, {"unresolved_attempts", unresolved_attempts},
        {"recent_attempts", attempts}, {"not_execution_authority", true}};
    snapshot.finish();
    (void)management_authorize(*impl_, actor, current);
    return view;
}

Json Store::management_history_page(const StoreActor& actor, const ExecutionBinding& current,
        const std::string& kind, std::int64_t after, std::int64_t through, std::int64_t limit) const {
    const std::lock_guard lock(impl_->mutex);
    require(after >= 0 && through >= 0 && limit >= 1 && limit <= 4, ErrorCode::InvalidInput,
        "management history requires nonnegative cursors and page limit 1..4");
    ManagementReadSnapshot snapshot(impl_->db);
    (void)management_authorize(*impl_, actor, current, "management_history_page");
    const auto source = management_source(actor, kind, impl_->reconciliation_schema);
    management_cursor(*impl_, source, after); management_cursor(*impl_, source, through);
    const auto catalog = management_catalog(*impl_, actor, source, through);
    through = number(catalog.as_object(), "through_sequence");
    require(after <= through, ErrorCode::InvalidInput, "management history cursor exceeds its high watermark");
    auto parameters = source.parameters;
    parameters.push_back(after); parameters.push_back(through); parameters.push_back(limit + 1);
    auto rows = impl_->all("SELECT " + source.columns + " FROM " + source.table + " WHERE " + source.filter +
        " AND q.rowid>? AND q.rowid<=? ORDER BY q.rowid LIMIT ?", parameters);
    const bool more = rows.size() > static_cast<std::size_t>(limit);
    if (more) rows.pop_back();
    Array items;
    for (const auto& row : rows) {
        const auto original = management_original(*impl_, actor, source, row);
        items.push_back(Row{{"sequence", row.at("sequence")}, {"kind", kind}, {"record_id", row.at("record_id")},
            {"record_version", row.at("record_version")}, {"original_binding", parse_json_strict(str(row, "original_binding"))},
            {"status", row.at("status")}, {"completed", management_completed(source, row)},
            {"record_sha256", original.is_null() ? Json(nullptr) : Json(sha256_hex(canonical_json(original)))},
            {"size_bytes", original.is_null() ? Json(nullptr) : Json(canonical_json(original).size())},
            {"original_missing", original.is_null()}, {"not_execution_authority", true}});
    }
    Json next = nullptr;
    if (more) next = rows.back().at("sequence");
    const Json result = Row{{"format", "wls-management-history-index-v1"}, {"scope", management_scope(actor)},
        {"kind", kind}, {"after_sequence", after}, {"through_sequence", through}, {"catalog", catalog},
        {"items", items}, {"has_more", more}, {"next_after_sequence", next}, {"not_execution_authority", true}};
    snapshot.finish();
    (void)management_authorize(*impl_, actor, current, "management_history_page");
    return result;
}

Json Store::read_management_history(const StoreActor& actor, const ExecutionBinding& current,
        const std::string& kind, const std::string& id, const std::string& expected_hash,
        std::int64_t expected_version, std::int64_t offset, std::int64_t max_bytes) const {
    const std::lock_guard lock(impl_->mutex);
    text_field(id, "management history record id");
    require(expected_hash.size() == 64 && std::all_of(expected_hash.begin(), expected_hash.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), ErrorCode::InvalidInput,
        "management history requires a lowercase original SHA-256");
    require(expected_version > 0 && offset >= 0 && max_bytes >= 1 && max_bytes <= 4096, ErrorCode::InvalidInput,
        "management history requires a positive original version and max_bytes 1..4096");
    ManagementReadSnapshot snapshot(impl_->db);
    (void)management_authorize(*impl_, actor, current, "management_history_read");
    const auto source = management_source(actor, kind, impl_->reconciliation_schema);
    auto parameters = source.parameters; parameters.emplace_back(id);
    const auto row = impl_->optional("SELECT " + source.columns + " FROM " + source.table + " WHERE " +
        source.filter + " AND q." + source.id + "=?", parameters);
    require(row.has_value(), ErrorCode::Unauthorized,
        "management original is outside the current role/request/target scope");
    require(number(*row, "record_version") == expected_version, ErrorCode::Stale,
        "management original version changed; an old reference cannot be refreshed silently");
    const auto original = management_original(*impl_, actor, source, *row);
    require(!original.is_null(), ErrorCode::NotReady, "original management feedback is missing; no history can be invented");
    const auto bytes = canonical_json(original);
    require(sha256_hex(bytes) == expected_hash, ErrorCode::Conflict,
        "management original hash or reader scope differs from its captured reference");
    require(static_cast<std::uint64_t>(offset) <= bytes.size(), ErrorCode::InvalidInput,
        "management history offset exceeds the original record");
    const auto start = static_cast<std::size_t>(offset);
    const auto count = std::min(static_cast<std::size_t>(max_bytes), bytes.size() - start);
    const Json result = Row{{"format", "wls-management-history-bytes-v1"}, {"scope", management_scope(actor)},
        {"kind", kind}, {"record_id", id}, {"record_version", expected_version}, {"record_sha256", expected_hash},
        {"section_sha256", expected_hash}, {"original_binding", parse_json_strict(str(*row, "original_binding"))},
        {"encoding", "base64-canonical-json-utf8"}, {"size_bytes", bytes.size()}, {"offset", offset},
        {"bytes_returned", count}, {"content_base64", base64_bytes(std::string_view(bytes).substr(start, count))},
        {"eof", start + count == bytes.size()},
        {"next_offset", start + count == bytes.size() ? Json(nullptr) : Json(start + count)},
        {"not_execution_authority", true}};
    snapshot.finish();
    (void)management_authorize(*impl_, actor, current, "management_history_read");
    return result;
}
