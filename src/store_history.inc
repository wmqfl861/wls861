// R1 read-only evidence access. Included after Store::attempt_history so this
// narrow implementation can use the existing private Store transaction/actor
// helpers without widening the trusted-host API or changing SQL assets.
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
    auto parameters = scope; parameters.push_back(limit);
    auto rows = impl_->all("SELECT rowid AS sequence,attempt_id,call_id,frozen_binding,status,accepted,response "
        "FROM call_attempts WHERE " + filter + " ORDER BY rowid DESC LIMIT ?", parameters);
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
        " AND (a.status IN ('PENDING','UNKNOWN') OR a.unknown_input=1 OR a.unknown_output=1 OR a.unknown_cost=1 "
        "OR NOT EXISTS(SELECT 1 FROM model_tool_feedback f WHERE f.attempt_id=a.attempt_id)) ORDER BY a.rowid LIMIT 65", scope);
    require(pending_calls.size() <= 64, ErrorCode::Quota,
        "unresolved attempt facts cannot fit; reconcile missing feedback/accounting before another model call");
    Array unsettled;
    for (const auto& call : pending_calls) unsettled.push_back(exposed(call));
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
