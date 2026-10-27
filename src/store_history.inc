// R1 read-only evidence access. Included after Store::attempt_history so this
// narrow implementation can use the existing private Store transaction/actor
// helpers without widening the trusted-host API or changing SQL assets.
namespace {
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
    (void)impl_->authorize(actor, {Role::Worker});
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
