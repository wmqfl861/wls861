// Included within wls::s1 after Store::Impl and the original history helpers.
namespace {
template<class Implementation>
std::vector<Json> single_agent_original_history(Implementation& impl, const std::string& request_id,
                                               const FrozenRequestConfig& config) {
    impl.require_events_schema();
    validate_single_agent_config(config);
    const auto policy = config.logical_agent_policy();
    const auto count = number(impl.one("SELECT COUNT(*) AS n FROM call_attempts WHERE request_id=?", {request_id}), "n");
    require(count <= number(policy.as_object(), "max_attempts"), ErrorCode::Quota,
        "complete single-Agent history exceeds its frozen attempt capacity");
    // Bound original blobs before copying any response/feedback into memory.
    // The exact canonical aggregate is checked again by the chain builder.
    const auto bytes = number(impl.one(
        "SELECT COALESCE(SUM(COALESCE(length(CAST(a.response AS BLOB)),0)*2+"
        "length(CAST(a.frozen_binding AS BLOB))*2+COALESCE(length(CAST(a.usage AS BLOB)),0)+"
        "COALESCE(length(CAST(f.result AS BLOB)),0)),0) AS n "
        "FROM call_attempts a LEFT JOIN model_tool_feedback f USING(attempt_id) WHERE a.request_id=?",
        {request_id}), "n");
    require(bytes <= number(policy.as_object(), "max_history_bytes"), ErrorCode::Quota,
        "complete single-Agent original blobs exceed their frozen byte capacity");
    Statement query(impl.db,
        "SELECT attempt_id,call_id,request_id,task_id,team_id,role,principal_id,config_snapshot_sha256,model_ref,"
        "frozen_binding,json_extract(request_envelope,'$.role_call_index') AS role_call_index,"
        "reservation_sha256,status,accepted,response,usage,settlement_sha256,"
        "json_extract(json_extract(request_envelope,'$.context.input.task_contract'),'$.logical_agent_execution') "
        "AS logical_agent_execution FROM call_attempts WHERE request_id=? ORDER BY rowid", {request_id});
    std::vector<Json> result;
    while (query.step()) {
        auto row = query.row();
        auto record = exposed(row);
        require(!row.at("logical_agent_execution").is_null(), ErrorCode::Corrupt,
            "existing attempt does not belong to the original single-Agent history");
        record.as_object()["logical_agent_execution"] = parse_json_strict(str(row, "logical_agent_execution"));
        record.as_object()["history_sequence"] = result.size() + 1;
        const auto feedback = impl.feedback_info(str(row, "attempt_id"));
        record.as_object()["feedback"] = feedback;
        record.as_object()["feedback_missing"] = feedback.is_null();
        result.push_back(std::move(record));
    }
    return result;
}
} // namespace

std::vector<Json> Store::logical_agent_history(const StoreActor& actor) const {
    const std::lock_guard lock(impl_->mutex);
    const auto request = impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    const auto config = FrozenRequestConfig::restore(str(request, "config_snapshot"), str(request, "config_snapshot_sha256"));
    auto history = single_agent_original_history(*impl_, actor.request_id, config);
    // Complete original fingerprints, ordering and pairing are verified even
    // for a read. Nothing from another request or an unclosed turn is adopted.
    static_cast<void>(single_agent_transcript(actor.request_id, config, history));
    return history;
}
