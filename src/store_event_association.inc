// Included inside wls::s1, after Store::Impl and the original transaction
// steps. No callback, file operation or model wait occurs in this protocol.
namespace {
constexpr std::int64_t n5_receipt_reserve = 2 * 65536 + 1024;
std::int64_t n5_encoded_bytes(std::string_view bytes) {
    // Count the canonical body, repeated scalar/index identities and framing
    // conservatively. This counter is never reset by a capacity checkpoint.
    return static_cast<std::int64_t>(bytes.size()) * 2 + 1024;
}
Array n5_ids(const std::vector<std::int64_t>& input) {
    require(!input.empty() && input.size() <= 64, ErrorCode::InvalidInput, "event selection must contain 1..64 IDs");
    std::set<std::int64_t> unique;
    for (const auto id : input)
        require(id > 0 && unique.insert(id).second, ErrorCode::InvalidInput, "duplicate or invalid event selection ID");
    Array result;
    for (const auto id : unique) result.emplace_back(id);
    return result;
}
bool n5_has_id(const Json& ids, std::int64_t id) {
    return std::any_of(ids.as_array().begin(), ids.as_array().end(), [id](const Json& value) { return value.as_int64() == id; });
}
} // namespace

bool Store::Impl::event_driver_enabled(const std::string& id) const {
    check_request_scope(id);
    return event_association_schema && optional("SELECT request_id FROM n5_event_requests WHERE request_id=?", {id}).has_value();
}
void Store::Impl::require_legacy_event_path(const std::string& id) const {
    check_request_scope(id);
    require(!event_driver_enabled(id), ErrorCode::Unsupported, "request requires the persistent event-call proof protocol");
}
Json Store::Impl::event_fact(const Row& row, std::size_t maximum) const {
    const auto bytes = str(row, "fact");
    require(sha256_hex(bytes) == str(row, "fact_sha256") && number(row, "encoded_bytes") == n5_encoded_bytes(bytes),
            ErrorCode::Corrupt, "persistent event fact fingerprint or byte count changed");
    const auto value = parse_json_strict(bytes, maximum);
    require(value.is_object() && value.at("format") == "n5-event-link-v1", ErrorCode::Corrupt, "unknown persistent event fact format");
    return value;
}
Row Store::Impl::event_source(std::int64_t id) const {
    const auto row = one("SELECT i.*,e.event_id,e.type,e.payload,e.result,e.command_sha256 FROM event_inbox i JOIN events e ON e.seq=i.event_seq WHERE i.inbox_id=?", {id});
    require(sha256_hex(str(row, "payload")) == row.at("command_sha256"), ErrorCode::Corrupt, "event source command bytes changed");
    return Row{{"inbox_id", id}, {"event_seq", row.at("event_seq")}, {"event_id", row.at("event_id")},
        {"type", row.at("type")}, {"request_id", row.at("request_id")}, {"role", row.at("role")},
        {"actor_task_id", row.at("actor_task_id")}, {"subject_task_id", row.at("subject_task_id")},
        {"event_requirements_revision", row.at("event_requirements_revision")},
        {"command_sha256", row.at("command_sha256")}, {"payload_sha256", sha256_hex(str(row, "payload"))},
        {"result_sha256", sha256_hex(str(row, "result"))}};
}
Row Store::Impl::event_claim_snapshot(const std::string& id) const {
    require(event_association_schema, ErrorCode::Unsupported, "persistent event protocol requires Store 411");
    auto row = one("SELECT * FROM n5_event_claims WHERE claim_id=?", {id});
    check_request_scope(str(row, "request_id"));
    const auto fact = event_fact(row, 65536);
    require(fact.at("claim_id") == row.at("claim_id") && fact.at("request_id") == row.at("request_id") &&
            fact.at("generation") == row.at("generation") && fact.at("actor").at("role") == row.at("role") &&
            fact.at("actor").at("task_id") == row.at("actor_task_id"), ErrorCode::Corrupt, "claim snapshot identity mismatch");
    std::int64_t previous = 0;
    require(fact.at("items").is_array() && !fact.at("items").as_array().empty() && fact.at("items").as_array().size() <= 64,
            ErrorCode::Corrupt, "invalid captured event set");
    for (const auto& item : fact.at("items").as_array()) {
        const auto inbox_id = number(item.as_object(), "inbox_id");
        require(inbox_id > previous && event_source(inbox_id) == item.as_object(), ErrorCode::Corrupt, "captured event source changed");
        previous = inbox_id;
    }
    return row;
}
void Store::Impl::event_claim_current(const StoreActor& actor, const Row& snapshot) const {
    const auto request = authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    const auto fact = event_fact(snapshot, 65536);
    require(fact.at("actor") == actor_json(actor) && str(snapshot, "request_id") == actor.request_id &&
            number(fact.as_object(), "requirements_revision") == number(request, "requirements_revision"),
            ErrorCode::Stale, "persistent claim belongs to an obsolete original actor");
    const auto current = one("SELECT * FROM event_actor_claims WHERE request_id=? AND role=? AND actor_task_id=?",
        {actor.request_id, role_name(actor.role), actor.task_id});
    require(current.at("claim_id") == snapshot.at("claim_id") && current.at("generation") == snapshot.at("generation") &&
            str(current, "status") == "ACTIVE" && current.at("principal_id") == actor.principal_id &&
            current.at("team_id") == actor.team_id && number(current, "dispatcher_epoch") == actor.dispatcher_epoch &&
            number(current, "execution_epoch") == actor.execution_epoch &&
            number(current, "requirements_revision") == number(fact.as_object(), "requirements_revision"),
            ErrorCode::Stale, "persistent original claim is no longer active");
    Array captured;
    for (const auto& item : fact.at("items").as_array()) {
        captured.push_back(item.at("inbox_id"));
        const auto inbox = one("SELECT status,claim_id FROM event_inbox WHERE inbox_id=?", {item.at("inbox_id")});
        require(str(inbox, "status") == "CLAIMED" && inbox.at("claim_id") == snapshot.at("claim_id"),
                ErrorCode::Stale, "original inbox claim membership changed");
    }
    require(parse_json_strict(str(current, "captured_ids")) == captured, ErrorCode::Corrupt, "original complete claim set changed");
}
Row Store::Impl::event_link_row(const std::string& id) const {
    require(event_association_schema, ErrorCode::Unsupported, "persistent event protocol requires Store 411");
    auto row = one("SELECT * FROM n5_event_links WHERE link_id=?", {id});
    check_request_scope(str(row, "request_id"));
    return row;
}
Json Store::Impl::event_receipt(const std::string& id, const std::string& kind) const {
    const auto row = optional("SELECT * FROM n5_event_receipts WHERE link_id=? AND kind=?", {id, kind});
    if (!row) return nullptr;
    const auto value = event_fact(*row, 65536);
    require(value.at("link_id") == id && value.at("kind") == kind, ErrorCode::Corrupt, "event receipt identity mismatch");
    return value;
}
void Store::Impl::verify_event_link(const Row& link) const {
    const auto fact = event_fact(link);
    const auto id = str(link, "link_id");
    require(event_driver_enabled(str(link, "request_id")) && fact.at("link_id") == id && fact.at("request_id") == link.at("request_id") &&
            fact.at("claim_id") == link.at("claim_id") && fact.at("attempt_id") == link.at("attempt_id") &&
            fact.at("call_id") == link.at("call_id") && fact.at("action") == link.at("action"), ErrorCode::Corrupt, "event link identity mismatch");
    const auto claim = event_claim_snapshot(str(link, "claim_id"));
    const auto captured = event_fact(claim, 65536);
    require(fact.at("claim_sha256") == claim.at("fact_sha256") && fact.at("actor") == captured.at("actor") &&
            fact.at("generation") == claim.at("generation"), ErrorCode::Corrupt, "event link original claim changed");
    const auto attempt = one("SELECT * FROM call_attempts WHERE attempt_id=?", {link.at("attempt_id")});
    const auto envelope = str(attempt, "request_envelope");
    const auto request = model_request_from_json(parse_json_strict(envelope, 4U * 1024U * 1024U));
    require(attempt.at("request_id") == link.at("request_id") && attempt.at("call_id") == link.at("call_id") &&
            fact.at("envelope_sha256") == sha256_hex(envelope) && fact.at("binding") == to_json(request.binding) &&
            fact.at("reservation_sha256") == attempt.at("reservation_sha256") && fact.at("role_call_index") == request.role_call_index &&
            fact.at("config_snapshot_sha256") == attempt.at("config_snapshot_sha256") && fact.at("model_ref") == attempt.at("model_ref") &&
            fact.at("actor").at("role") == attempt.at("role") && fact.at("actor").at("principal_id") == attempt.at("principal_id") &&
            parse_json_strict(str(attempt, "frozen_binding")) == fact.at("binding") &&
            ((request.binding.task_id.empty() && attempt.at("task_id").is_null()) || attempt.at("task_id") == request.binding.task_id) &&
            ((request.binding.team_id.empty() && attempt.at("team_id").is_null()) || attempt.at("team_id") == request.binding.team_id),
            ErrorCode::Corrupt, "event association differs from original attempt bytes");
    const auto original_request = request_row(str(link, "request_id"));
    const auto frozen = FrozenRequestConfig::restore(str(original_request, "config_snapshot"), str(original_request, "config_snapshot_sha256"));
    require(fact.at("profile") == frozen.model_profile(request.role), ErrorCode::Corrupt, "event frozen model profile changed");
    const auto reserve_event = one("SELECT payload,command_sha256 FROM events WHERE request_id=? AND command_id=?",
        {link.at("request_id"), "reserve:" + str(link, "attempt_id")});
    require(sha256_hex(str(reserve_event, "payload")) == attempt.at("reservation_sha256") &&
            reserve_event.at("command_sha256") == attempt.at("reservation_sha256"), ErrorCode::Corrupt, "event original reservation fingerprint changed");
    require(sha256_hex(canonical_json(fact.at("target"))) == fact.at("target_sha256"), ErrorCode::Corrupt, "event target fingerprint mismatch");
    const auto contract = model_contract(request);
    const auto* action = contract.as_object().if_contains("workflow_action");
    require(fact.at("target").at("workflow_action") == (action ? *action : Json(nullptr)) &&
            fact.at("target").at("expected_state_version") == contract.at("captured_state_version"),
            ErrorCode::Corrupt, "event association changed original model target");
    const auto members = all("SELECT * FROM n5_event_items WHERE link_id=? ORDER BY inbox_id", {id});
    require(members.size() == fact.at("selected_ids").as_array().size() && !members.empty() && members.size() <= 64,
            ErrorCode::Corrupt, "event selected membership is incomplete");
    const bool closed = !event_receipt(id, "CLOSE").is_null();
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto& member = members[index]; const auto source = event_fact(member, 65536);
        const auto inbox_id = number(member, "inbox_id");
        require(fact.at("selected_ids").as_array()[index] == inbox_id && source.at("link_id") == id &&
                source.at("source") == event_source(inbox_id), ErrorCode::Corrupt, "event member source changed");
        const auto original = std::find_if(captured.at("items").as_array().begin(), captured.at("items").as_array().end(),
            [inbox_id](const Json& item) { return item.at("inbox_id") == inbox_id; });
        require(original != captured.at("items").as_array().end() && *original == source.at("source"),
                ErrorCode::Corrupt, "event selected ID escaped its original capture");
        const auto open = optional("SELECT link_id FROM n5_event_open_items WHERE inbox_id=?", {inbox_id});
        if (!closed) require(open && open->at("link_id") == id, ErrorCode::Corrupt, "unresolved event ownership projection is missing");
        else require(!open || open->at("link_id") != id, ErrorCode::Corrupt, "closed event retained an open projection");
    }
    const auto reserved = event_receipt(id, "RESERVED");
    require(!reserved.is_null() && reserved.at("body").at("link_sha256") == link.at("fact_sha256") &&
            reserved.at("body").at("reservation_sha256") == attempt.at("reservation_sha256") &&
            number(link, "business_tail") == (str(link, "action") == "note" ? 3 : 2) &&
            number(link, "control_tail") == (str(link, "action") == "note" ? 3 : 2), ErrorCode::Corrupt, "event reserved tail fact changed");
    const auto dispatch = event_receipt(id, "DISPATCH");
    const auto dispatch_event = optional("SELECT seq,event_id,payload,result FROM events WHERE request_id=? AND command_id=?",
        {link.at("request_id"), "offline-dispatch:" + str(link, "attempt_id")});
    require(dispatch.is_null() == !dispatch_event.has_value(), ErrorCode::Corrupt, "event dispatch receipt and original authority disagree");
    if (!dispatch.is_null()) require(dispatch.at("body").at("event_seq") == dispatch_event->at("seq") &&
        dispatch.at("body").at("event_id") == dispatch_event->at("event_id") &&
        dispatch.at("body").at("payload_sha256") == sha256_hex(str(*dispatch_event, "payload")) &&
        dispatch.at("body").at("result_sha256") == sha256_hex(str(*dispatch_event, "result")), ErrorCode::Corrupt, "event dispatch source changed");
    const auto settlement = event_receipt(id, "SETTLEMENT");
    require(settlement.is_null() == attempt.at("settlement_sha256").is_null(), ErrorCode::Corrupt, "event settlement and receipt disagree");
    if (!settlement.is_null()) require(settlement.at("body").at("settlement_sha256") == attempt.at("settlement_sha256") &&
        settlement.at("body").at("response_sha256") == sha256_hex(str(attempt, "response")) &&
        settlement.at("body").at("usage_sha256") == sha256_hex(str(attempt, "usage")), ErrorCode::Corrupt, "event original observation bytes changed");
    const auto close = event_receipt(id, "CLOSE"); const auto error = event_receipt(id, "ERROR");
    require(close.is_null() || error.is_null(), ErrorCode::Corrupt, "event has conflicting close and error facts");
    if (!close.is_null() || !error.is_null()) {
        const auto feedback = feedback_info(str(link, "attempt_id"));
        const auto& body = (!close.is_null() ? close : error).at("body");
        require(!feedback.is_null() && feedback.at("record_sha256") == body.at("feedback_sha256") &&
            feedback.at("original_binding") == fact.at("binding") && canonical_json(feedback.at("response")) == str(attempt, "response"),
            ErrorCode::Corrupt, "event closeout lacks original immutable feedback");
    }
    if (!close.is_null()) {
        const auto& body = close.at("body");
        require(body.at("settlement_sha256") == attempt.at("settlement_sha256") &&
            body.at("dispositions").as_array().size() == captured.at("items").as_array().size(), ErrorCode::Corrupt,
            "event close lacks complete individual original dispositions");
        Array completed;
        for (std::size_t index = 0; index < captured.at("items").as_array().size(); ++index) {
            const auto& source = captured.at("items").as_array()[index]; const auto& proof = body.at("dispositions").as_array()[index];
            require(proof.at("inbox_id") == source.at("inbox_id") && proof.at("source_sha256") == sha256_hex(canonical_json(source)),
                ErrorCode::Corrupt, "event close individual source changed");
            const auto disposition = str(proof.as_object(), "disposition");
            require(disposition == "HANDLED" || disposition == "NOT_SELECTED" || disposition == "DEFERRED_NO_EFFECT", ErrorCode::Corrupt,
                "event close has an unknown disposition");
            if (disposition == "HANDLED") {
                require(n5_has_id(fact.at("selected_ids"), source.at("inbox_id").as_int64()) &&
                    one("SELECT status FROM event_inbox WHERE inbox_id=?", {source.at("inbox_id")}).at("status") == "DONE",
                    ErrorCode::Corrupt, "event handled ID lacks original completion");
                completed.push_back(source.at("inbox_id"));
                if (str(link, "action") == "amendment") {
                    const auto changed = std::find_if(body.at("result").at("tasks").as_array().begin(), body.at("result").at("tasks").as_array().end(),
                        [&](const Json& task) { return task.at("task_id") == source.at("subject_task_id"); });
                    require(changed != body.at("result").at("tasks").as_array().end() && proof.at("task_id") == source.at("subject_task_id") &&
                        proof.at("node_id") == changed->at("node_id") && proof.at("transition_sha256") == sha256_hex(canonical_json(*changed)),
                        ErrorCode::Corrupt, "AMEND close lacks its individual original transition");
                }
            }
        }
        require(completed == body.at("completed_ids"), ErrorCode::Corrupt, "event close completed set changed");
        for (const auto& effect : body.at("effects").as_array()) {
            const auto event = one("SELECT event_id,type,command_id,payload,result FROM events WHERE request_id=? AND seq=?",
                {link.at("request_id"), effect.at("event_seq")});
            require(event.at("event_id") == effect.at("event_id") && event.at("command_id") == effect.at("command_id") &&
                event.at("type") == effect.at("type") && sha256_hex(str(event, "payload")) == effect.at("payload_sha256") &&
                sha256_hex(str(event, "result")) == effect.at("result_sha256"), ErrorCode::Corrupt, "event original effect source changed");
        }
    }
}
void Store::Impl::event_link_current(const StoreActor& actor, const Row& link) const {
    verify_event_link(link);
    const auto fact = event_fact(link);
    require(fact.at("actor") == actor_json(actor), ErrorCode::Unauthorized, "event call belongs to another original actor");
    event_claim_current(actor, event_claim_snapshot(str(link, "claim_id")));
    const auto binding = execution_binding_from_json(fact.at("binding"));
    actor_binding(actor, binding); matches(binding);
    const auto request = request_row(actor.request_id);
    require(!terminal_request(str(request, "status")), ErrorCode::NotReady, "persistent event request has terminated");
    const auto subject = actor.task_id.empty() ? request : task_row(actor.task_id);
    require(number(subject, "state_version") == number(fact.at("target").as_object(), "expected_state_version") &&
            sha256_hex(canonical_json(subject)) == fact.at("target").at("subject_sha256"), ErrorCode::Stale,
            "persistent event original target CAS or source changed");
}
void Store::Impl::event_amendment_targets_current(const Json& fact) const {
    if (fact.at("action") != "amendment") return;
    const auto& captured = fact.at("target").at("workflow_action").at("target").at("tasks");
    require(captured.is_array() && !captured.as_array().empty() && captured.as_array().size() <= 20,
            ErrorCode::InvalidInput, "event AMEND requires bounded original task targets");
    std::set<std::string> seen_tasks, seen_nodes;
    for (const auto& value : captured.as_array()) {
        require(value.is_object(), ErrorCode::InvalidInput, "event AMEND task target must be an object");
        const auto& target = value.as_object();
        for (const auto* key : {"task_id", "node_id", "team_id", "state_version", "execution_epoch"})
            require(target.contains(key), ErrorCode::InvalidInput, "event AMEND task target lacks its original fields");
        const auto task_id = str(target, "task_id"), node_id = str(target, "node_id");
        require(seen_tasks.insert(task_id).second && seen_nodes.insert(node_id).second, ErrorCode::InvalidInput,
                "event AMEND original task targets are duplicated");
        const auto current = task_row(task_id);
        require(current.at("request_id") == fact.at("request_id") && current.at("node_id") == node_id,
                ErrorCode::Unauthorized, "event AMEND target task identity or request ownership changed");
        require(number(current, "state_version") == number(target, "state_version"), ErrorCode::Conflict,
                "event AMEND original task CAS changed before reservation or dispatch");
        const auto team = optional("SELECT * FROM teams WHERE task_id=? AND status IN ('STARTING','ACTIVE','DRAINING')", {task_id});
        require(team ? team->at("request_id") == fact.at("request_id") && target.at("team_id") == team->at("team_id") &&
                target.at("execution_epoch") == team->at("execution_epoch") :
                target.at("team_id").is_null() && target.at("execution_epoch").is_null(), ErrorCode::Stale,
                "event AMEND original team identity or execution epoch changed");
    }
}
std::int64_t Store::Impl::event_metadata_bytes(const std::string& id) const {
    const auto row = one("SELECT (SELECT COALESCE(SUM(encoded_bytes),0) FROM n5_event_requests WHERE request_id=?) + "
        "(SELECT COALESCE(SUM(encoded_bytes),0) FROM n5_event_claims WHERE request_id=?) + "
        "(SELECT COALESCE(SUM(encoded_bytes),0) FROM n5_event_links WHERE request_id=?) + "
        "(SELECT COALESCE(SUM(i.encoded_bytes),0) FROM n5_event_items i JOIN n5_event_links l USING(link_id) WHERE l.request_id=?) + "
        "(SELECT COALESCE(SUM(r.encoded_bytes),0) FROM n5_event_receipts r JOIN n5_event_links l USING(link_id) WHERE l.request_id=?) AS n",
        {id, id, id, id, id});
    auto total = number(row, "n");
    for (const auto& link : all("SELECT link_id FROM n5_event_links l WHERE request_id=? AND NOT EXISTS(SELECT 1 FROM n5_event_receipts r WHERE r.link_id=l.link_id AND r.kind='CLOSE')", {id})) {
        const auto count = number(one("SELECT COUNT(*) AS n FROM n5_event_receipts WHERE link_id=?", {link.at("link_id")}), "n");
        require(count <= 8, ErrorCode::Corrupt, "event receipt bound changed");
        total = add(total, (8 - count) * n5_receipt_reserve);
    }
    return total;
}
void Store::Impl::guard_event_metadata(const std::string& id, std::int64_t extra) const {
    const auto policy = one("SELECT * FROM n5_event_requests WHERE request_id=?", {id});
    (void)event_fact(policy);
    require(add(event_metadata_bytes(id), extra) <= number(policy, "max_metadata_bytes"), ErrorCode::Quota,
            "persistent event lifetime metadata and reserved proof capacity exhausted");
}
void Store::Impl::append_event_receipt(const Row& link, const std::string& kind, const Json& body) {
    Row fact{{"format", "n5-event-link-v1"}, {"kind", kind}, {"link_id", link.at("link_id")}, {"body", body}};
    const auto encoded = json_text(fact, 65536);
    const auto id = str(link, "link_id");
    if (const auto old = event_receipt(id, kind); !old.is_null()) {
        require(old == fact, ErrorCode::Conflict, "persistent event receipt replay changed original facts"); return;
    }
    const auto ordinal = number(one("SELECT COUNT(*) AS n FROM n5_event_receipts WHERE link_id=?", {id}), "n");
    require(ordinal < 8, ErrorCode::Quota, "persistent event receipt capacity exhausted");
    execute("INSERT INTO n5_event_receipts VALUES (?,?,?,?,?,?,?)", {id, kind, ordinal, encoded, sha256_hex(encoded), n5_encoded_bytes(encoded), stamp()});
    guard_event_metadata(str(link, "request_id"));
}

void Store::Impl::guard_event_protocol(const std::string& id, const std::string&, const Row& command) const {
    if (!event_driver_enabled(id) || !event_protocol_link.empty()) return;
    const auto type = str(command, "type");
    require(type == "ack" || type == "amend" || type == "cancel_request" || type == "attempts_unknown" ||
            capacity_maintenance(type), ErrorCode::Unsupported, "enabled request cannot bypass persistent event proof gates");
}
bool Store::Impl::event_protocol_continuation(const std::string& id, const std::string& key, const Row& command) const {
    if (!event_driver_enabled(id) || event_protocol_link.empty()) return false;
    const auto link = optional("SELECT * FROM n5_event_links WHERE link_id=? AND request_id=?", {event_protocol_link, id});
    if (!link) return false;
    const auto attempt = str(*link, "attempt_id"); const auto type = str(command, "type");
    return (type == "claim_offline_call" && key == "offline-dispatch:" + attempt) ||
        (type == "settle_attempt" && key == "settle:" + attempt) ||
        (type == "tool_feedback" && key == "feedback:" + attempt) ||
        ((type == "independent_plan" || type == "independent_dispositions" || type == "instruction" ||
          type == "prepare_op" || type == "start_op" || type == "finish_op") && key.starts_with(attempt + ":"));
}
void Store::Impl::guard_event_tail_capacity(const std::string& id, const std::string& key, const Row* command) const {
    if (!event_driver_enabled(id)) return;
    const auto state = capacity_state(id);
    auto business = number(state.at("counts").as_object(), "business_events") + number(state, "reserved_dispatch");
    auto control = number(state.at("counts").as_object(), "control_events") + number(state, "reserved_control");
    business = add(business, event_admission_business);
    // T1's pending call already reserves four controls in the original pool.
    control = add(control, std::max<std::int64_t>(0, event_admission_control - 4));
    for (const auto& link : all("SELECT l.*,c.status AS attempt_status,c.unknown_input,c.unknown_output,c.unknown_cost "
            "FROM n5_event_links l JOIN call_attempts c USING(attempt_id) WHERE l.request_id=?", {id})) {
        const auto link_id = str(link, "link_id");
        if (!event_receipt(link_id, "CLOSE").is_null() || !event_receipt(link_id, "ERROR").is_null()) continue;
        const auto attempt = str(link, "attempt_id"); const auto prefix = attempt + ":";
        const auto consumed = one("SELECT COALESCE(SUM(CASE WHEN " + capacity_control_sql() + " THEN 0 ELSE 1 END),0) AS b, "
            "COALESCE(SUM(CASE WHEN " + capacity_control_sql() + " THEN 1 ELSE 0 END),0) AS c FROM events WHERE request_id=? "
            "AND (command_id=? OR command_id=? OR command_id=? OR substr(command_id,1,?)=?)",
            {id, "offline-dispatch:" + attempt, "settle:" + attempt, "feedback:" + attempt, static_cast<std::int64_t>(prefix.size()), prefix});
        auto remaining_business = number(link, "business_tail") - number(consumed, "b");
        auto remaining_control = number(link, "control_tail") - number(consumed, "c");
        if (command && link_id == event_protocol_link && event_protocol_continuation(id, key, *command)) {
            if (capacity_control(*command)) --remaining_control; else --remaining_business;
        }
        require(remaining_business >= 0 && remaining_control >= 0, ErrorCode::Corrupt, "event tail consumed outside its original bound");
        const auto prepared = number(one("SELECT COUNT(*) AS n FROM ops WHERE request_id=? AND business_idempotency_key=? AND status='PREPARED'", {id, attempt}), "n");
        const auto pending_ops = number(one("SELECT COUNT(*) AS n FROM ops WHERE request_id=? AND business_idempotency_key=? AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN')", {id, attempt}), "n");
        const bool pending_call = str(link, "attempt_status") == "PENDING" || str(link, "attempt_status") == "UNKNOWN" ||
            number(link, "unknown_input") != 0 || number(link, "unknown_output") != 0 || number(link, "unknown_cost") != 0;
        business = add(business, std::max<std::int64_t>(0, remaining_business - prepared));
        control = add(control, std::max<std::int64_t>(0, remaining_control - 4 * (pending_ops + (pending_call ? 1 : 0))));
    }
    if (command && !capacity_maintenance(str(*command, "type"))) {
        if (capacity_control(*command)) control = add(control, 1); else business = add(business, 1);
    }
    require(business <= number(state.at("limits").as_object(), "business_events") &&
            control <= number(state.at("limits").as_object(), "control_events"), ErrorCode::NotReady,
            "persistent event original business/control tail capacity unavailable");
}

bool Store::Impl::event_item_targets(const Row& source, const Json& fact) const {
    const auto binding = execution_binding_from_json(fact.at("binding"));
    if (source.at("request_id") != fact.at("request_id") || source.at("role") != fact.at("actor").at("role") ||
            source.at("actor_task_id") != fact.at("actor").at("task_id") ||
            number(source, "event_requirements_revision") != binding.requirements_revision) return false;
    const auto action = str(fact.as_object(), "action"); const auto type = str(source, "type");
    const auto subject = str(source, "subject_task_id");
    if (action == "plan") return subject.empty() && (type == "admit" || type == "amend" || type == "recover" || type == "event_bootstrap");
    if (action == "amendment") {
        if (type != "amend" && type != "recover" && type != "event_bootstrap") return false;
        const auto& target = fact.at("target").at("workflow_action").at("target");
        const auto& tasks = target.at("tasks").as_array();
        return std::any_of(tasks.begin(), tasks.end(), [&](const Json& task) {
            return task.at("task_id") == subject && contains(target.at("pending_node_ids"), str(task.as_object(), "node_id"));
        });
    }
    if (subject != binding.task_id) return false;
    if (action == "instruction") return type == "independent_plan" || type == "independent_dispositions" || type == "create_task" ||
        type == "disposition" || type == "rebind_task" || type == "recover" || type == "event_bootstrap" || type == "change_task";
    if (action != "note") return false;
    if (type == "recover" || type == "event_bootstrap" || type == "wake_worker") return true;
    if (type != "instruction" && type != "ack" && type != "tool_feedback") return false;
    const auto event = one("SELECT payload,result FROM events WHERE seq=?", {source.at("event_seq")});
    const auto result = parse_json_strict(str(event, "result"));
    const auto payload = parse_json_strict(str(event, "payload"));
    const Json* tuple = nullptr;
    if (const auto* value = result.as_object().if_contains("binding")) tuple = value;
    else if (const auto* value = payload.as_object().if_contains("binding")) tuple = value;
    return tuple && *tuple == to_json(binding);
}

Json Store::enable_event_driver(const StoreActor& root, const EventDriverLimits& limits, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    require(impl_->event_association_schema, ErrorCode::Unsupported, "persistent event driver requires Store 411");
    text_field(command_id, "persistent event enable command");
    require(limits.max_links > 0 && limits.max_links <= 64 && limits.max_claim_snapshots > 0 && limits.max_claim_snapshots <= 128 &&
            limits.max_metadata_bytes > 0 && limits.max_metadata_bytes <= 16 * 1024 * 1024, ErrorCode::InvalidInput,
            "persistent event limits exceed the frozen protocol ceilings");
    Impl::Transaction tx(*impl_);
    const auto request = impl_->authorize(root, {Role::Root}, false);
    const Row requested{{"max_links", limits.max_links}, {"max_claim_snapshots", limits.max_claim_snapshots}, {"max_metadata_bytes", limits.max_metadata_bytes}};
    if (const auto old = impl_->optional("SELECT * FROM n5_event_requests WHERE request_id=?", {root.request_id})) {
        const auto fact = impl_->event_fact(*old);
        require(old->at("command_id") == command_id && fact.at("requested_limits") == requested, ErrorCode::Conflict,
                "persistent event policy is already frozen with a different command or limits");
        tx.commit(); return fact;
    }
    (void)impl_->authorize(root, {Role::Root});
    require(!terminal_request(str(request, "status")) && number(request, "requirements_revision") == 1 && number(request, "plan_revision") == 0 &&
            !impl_->optional("SELECT attempt_id FROM call_attempts WHERE request_id=? LIMIT 1", {root.request_id}) &&
            !impl_->optional("SELECT op_id FROM ops WHERE request_id=? LIMIT 1", {root.request_id}) &&
            !impl_->optional("SELECT task_id FROM tasks WHERE request_id=? LIMIT 1", {root.request_id}) &&
            !impl_->optional("SELECT claim_id FROM event_actor_claims WHERE request_id=? LIMIT 1", {root.request_id}),
            ErrorCode::NotReady, "persistent events can only be enabled on a new request before attempts, operations or claims");
    Array sources;
    for (const auto& row : impl_->all("SELECT i.inbox_id FROM event_inbox i JOIN events e ON e.seq=i.event_seq WHERE i.request_id=? AND i.role='dispatcher' AND i.actor_task_id='' AND i.subject_task_id='' AND i.status='PENDING' AND e.type='admit' ORDER BY i.inbox_id", {root.request_id}))
        sources.emplace_back(impl_->event_source(number(row, "inbox_id")));
    require(sources.size() == 1, ErrorCode::Corrupt, "new request requires its exact original admission wake");
    const auto migration_barrier = number(impl_->one("SELECT activation_barrier_seq FROM event_association_metadata WHERE singleton=1"), "activation_barrier_seq");
    require(number(sources.front().as_object(), "event_seq") > migration_barrier, ErrorCode::NotReady,
            "a pre-411 admission remains legacy; persistent events require a new request");
    const auto maximum = number(impl_->one("SELECT max_attempts FROM request_budgets WHERE request_id=?", {root.request_id}), "max_attempts");
    const auto max_links = std::min(limits.max_links, maximum);
    const Row frozen{{"max_links", max_links}, {"max_claim_snapshots", limits.max_claim_snapshots}, {"max_metadata_bytes", limits.max_metadata_bytes}};
    const Row policy{{"protocol", "n5-persistent-events-v1"}, {"selection_predicate", "n5-event-target-v1"}, {"limits", frozen}};
    const Row fact{{"format", "n5-event-link-v1"}, {"state", "ENABLED"}, {"request_id", root.request_id}, {"actor", actor_json(root)},
        {"command_id", command_id}, {"policy", policy}, {"policy_sha256", sha256_hex(canonical_json(policy))},
        {"config_snapshot_sha256", request.at("config_snapshot_sha256")}, {"requested_limits", requested},
        {"migration_activation_barrier_seq", migration_barrier},
        {"activation_barrier_seq", impl_->one("SELECT COALESCE(MAX(seq),0) AS n FROM events WHERE request_id=?", {root.request_id}).at("n")},
        {"admission_sources", sources}};
    const auto encoded = json_text(fact, 65536);
    impl_->execute("INSERT INTO n5_event_requests VALUES (?,?,?,?,?,?,?,?,?)", {root.request_id, command_id, max_links,
        limits.max_claim_snapshots, limits.max_metadata_bytes, encoded, sha256_hex(encoded), n5_encoded_bytes(encoded), stamp()});
    impl_->guard_event_metadata(root.request_id);
    tx.commit(); return fact;
}
bool Store::persistent_events_enabled(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex);
    (void)impl_->request_row(id);
    return impl_->event_driver_enabled(id);
}
Json Store::claim_event_batch(const StoreActor& actor, std::int64_t maximum) {
    const std::lock_guard lock(impl_->mutex);
    require(impl_->event_driver_enabled(actor.request_id), ErrorCode::Unsupported, "request did not enable persistent events");
    require(maximum > 0 && maximum <= 64, ErrorCode::InvalidInput, "persistent event claim limit must be 1..64");
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    for (const auto& link : impl_->all("SELECT * FROM n5_event_links WHERE request_id=?", {actor.request_id})) impl_->verify_event_link(link);
    if (impl_->optional("SELECT link_id FROM n5_event_links l WHERE request_id=? AND json_extract(fact,'$.actor.role')=? "
            "AND json_extract(fact,'$.actor.task_id')=? AND NOT EXISTS(SELECT 1 FROM n5_event_receipts r WHERE r.link_id=l.link_id AND r.kind='CLOSE') LIMIT 1",
            {actor.request_id, role_name(actor.role), actor.task_id})) {
        tx.commit(); return Row{{"claim_id", ""}, {"generation", 0}, {"items", Array{}}, {"hold_reason", "HOLD_UNRESOLVED_ORIGINAL_CALL"}};
    }
    auto result = impl_->claim_events_in_transaction(actor, maximum);
    if (str(result.as_object(), "claim_id").empty()) { tx.commit(); return result; }
    const auto policy = impl_->one("SELECT * FROM n5_event_requests WHERE request_id=?", {actor.request_id});
    require(number(impl_->one("SELECT COUNT(*) AS n FROM n5_event_claims WHERE request_id=?", {actor.request_id}), "n") < number(policy, "max_claim_snapshots"),
            ErrorCode::Quota, "persistent event lifetime claim capacity exhausted");
    Array items;
    for (const auto& item : result.at("items").as_array()) items.emplace_back(impl_->event_source(number(item.as_object(), "inbox_id")));
    const auto request = impl_->request_row(actor.request_id);
    const Row fact{{"format", "n5-event-link-v1"}, {"claim_id", result.at("claim_id")}, {"generation", result.at("generation")},
        {"request_id", actor.request_id}, {"tenant_id", request.at("tenant_id")}, {"project_id", request.at("project_id")},
        {"actor", actor_json(actor)}, {"requirements_revision", result.at("requirements_revision")}, {"items", items}};
    const auto encoded = json_text(fact, 65536);
    impl_->execute("INSERT INTO n5_event_claims VALUES (?,?,?,?,?,?,?,?,?)", {result.at("claim_id"), actor.request_id, role_name(actor.role), actor.task_id,
        result.at("generation"), encoded, sha256_hex(encoded), n5_encoded_bytes(encoded), stamp()});
    impl_->guard_event_metadata(actor.request_id);
    tx.commit(); return result;
}
Json Store::release_event_batch(const StoreActor& actor, const std::string& id, std::int64_t generation) {
    const std::lock_guard lock(impl_->mutex);
    Impl::Transaction tx(*impl_);
    const auto snapshot = impl_->event_claim_snapshot(id);
    require(snapshot.at("request_id") == actor.request_id && number(snapshot, "generation") == generation,
            ErrorCode::Unauthorized, "event release does not name the original claim");
    require(!impl_->optional("SELECT link_id FROM n5_event_links WHERE claim_id=?", {id}), ErrorCode::NotReady,
            "an associated claim requires its original completion proof");
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    require(impl_->event_fact(snapshot).at("actor") == actor_json(actor), ErrorCode::Unauthorized, "event release original actor changed");
    auto result = impl_->finish_events_in_transaction(actor, id, generation, {});
    tx.commit(); return result;
}

Json Store::reserve_event_call(const StoreActor& actor, const ExecutionBinding& binding,
                               const AttemptReservation& reservation, const EventCallAssociation& association) {
    const std::lock_guard lock(impl_->mutex);
    require(impl_->event_driver_enabled(actor.request_id), ErrorCode::Unsupported, "request did not enable persistent events");
    require(reservation.adapter_kind == "fake" && reservation.experience_job_id.empty(), ErrorCode::Unsupported,
            "persistent event slice supports only original offline business calls");
    const auto selected = n5_ids(association.selected_ids);
    const auto request_envelope = model_request_from_json(reservation.request_envelope);
    const auto contract = model_contract(request_envelope);
    require(contract.is_object() && contract.as_object().contains("captured_state_version") &&
            number(contract.as_object(), "captured_state_version") == association.expected_state_version, ErrorCode::Conflict,
            "persistent event target differs from the original captured CAS");
    const auto* original_action = contract.as_object().if_contains("workflow_action");
    require((original_action ? *original_action : Json(nullptr)) == association.workflow_action, ErrorCode::Conflict,
            "persistent event target differs from the original captured action");
    std::string action = actor.role == Role::Dispatcher ? "plan" : actor.role == Role::TeamLead ? "instruction" : "note";
    if (!association.workflow_action.is_null()) {
        require(actor.role == Role::Dispatcher && str(association.workflow_action.as_object(), "kind") == "amendment",
                ErrorCode::Unsupported, "workflow action is outside the persistent event slice");
        action = "amendment";
    }
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker}, false);
    const auto snapshot = impl_->event_claim_snapshot(association.claim_id);
    require(snapshot.at("request_id") == actor.request_id && number(snapshot, "generation") == association.generation &&
            impl_->event_fact(snapshot).at("actor") == actor_json(actor), ErrorCode::Unauthorized, "persistent event original claim tuple changed");
    if (const auto old = impl_->optional("SELECT * FROM n5_event_links WHERE attempt_id=? OR call_id=? OR claim_id=?",
            {reservation.attempt_id, reservation.call_id, association.claim_id})) {
        const auto fact = impl_->event_fact(*old);
        require(old->at("attempt_id") == reservation.attempt_id && old->at("call_id") == reservation.call_id &&
                old->at("claim_id") == association.claim_id && fact.at("generation") == association.generation && fact.at("selected_ids") == selected &&
                fact.at("binding") == to_json(binding) && fact.at("actor") == actor_json(actor) && fact.at("action") == action &&
                fact.at("target").at("expected_state_version") == association.expected_state_version &&
                fact.at("target").at("workflow_action") == association.workflow_action, ErrorCode::Conflict,
                "persistent event replay changed its immutable association");
        (void)impl_->reserve_attempt_in_transaction(actor, binding, reservation);
        const auto result = impl_->event_link_info(*old); tx.commit(); return result;
    }
    impl_->event_claim_current(actor, snapshot);
    require(!impl_->event_application_halted, ErrorCode::NotReady, "persistent event Store retained a failed application; halt new business");
    require(!impl_->optional("SELECT link_id FROM n5_event_links l WHERE request_id=? AND json_extract(fact,'$.actor.role')=? "
        "AND json_extract(fact,'$.actor.task_id')=? AND NOT EXISTS(SELECT 1 FROM n5_event_receipts r WHERE r.link_id=l.link_id AND r.kind='CLOSE') LIMIT 1",
        {actor.request_id, role_name(actor.role), actor.task_id}), ErrorCode::UnknownOutcome,
        "original actor retains an unresolved event call; a new wake cannot authorize a retry");
    require(!impl_->optional("SELECT attempt_id FROM call_attempts WHERE attempt_id=? OR call_id=?", {reservation.attempt_id, reservation.call_id}),
            ErrorCode::Conflict, "an existing attempt without this original event link cannot acquire a synthetic association");
    impl_->actor_binding(actor, binding); impl_->matches(binding);
    const auto subject = actor.task_id.empty() ? impl_->request_row(actor.request_id) : impl_->task_row(actor.task_id);
    require(number(subject, "state_version") == association.expected_state_version, ErrorCode::Stale, "persistent event captured target CAS changed");
    const auto policy = impl_->one("SELECT * FROM n5_event_requests WHERE request_id=?", {actor.request_id});
    require(number(impl_->one("SELECT COUNT(*) AS n FROM n5_event_links WHERE request_id=?", {actor.request_id}), "n") < number(policy, "max_links"),
            ErrorCode::Quota, "persistent event lifetime link capacity exhausted");
    const auto frozen = FrozenRequestConfig::restore(str(impl_->request_row(actor.request_id), "config_snapshot"), reservation.config_snapshot_sha256);
    const Row target{{"expected_state_version", association.expected_state_version}, {"workflow_action", association.workflow_action},
        {"subject_sha256", sha256_hex(canonical_json(subject))}, {"binding", to_json(binding)}};
    const auto target_bytes = json_text(target, 1024 * 1024);
    const auto id = new_id("eventlink");
    Row fact{{"format", "n5-event-link-v1"}, {"link_id", id}, {"request_id", actor.request_id}, {"actor", actor_json(actor)},
        {"claim_id", association.claim_id}, {"generation", association.generation}, {"claim_sha256", snapshot.at("fact_sha256")},
        {"binding", to_json(binding)}, {"call_id", reservation.call_id}, {"attempt_id", reservation.attempt_id}, {"action", action},
        {"role_call_index", request_envelope.role_call_index}, {"config_snapshot_sha256", reservation.config_snapshot_sha256},
        {"model_ref", reservation.model_ref}, {"profile", frozen.model_profile(actor.role)}, {"selected_ids", selected},
        {"selection_predicate", "n5-event-target-v1"}, {"target", target}, {"target_sha256", sha256_hex(target_bytes)}};
    impl_->event_amendment_targets_current(fact);
    const auto captured = impl_->event_fact(snapshot).at("items").as_array();
    for (const auto& value : selected) {
        const auto found = std::find_if(captured.begin(), captured.end(), [&](const Json& item) { return item.at("inbox_id") == value; });
        require(found != captured.end(), ErrorCode::Unauthorized, "event selection escaped the complete original capture");
        require(impl_->event_item_targets(found->as_object(), fact), ErrorCode::Unauthorized, "selected event source does not match the original target");
        require(!impl_->optional("SELECT link_id FROM n5_event_open_items WHERE inbox_id=?", {value}), ErrorCode::Conflict, "event ID already has an unresolved call owner");
    }
    const auto business_tail = action == "note" ? 3 : 2; const auto control_tail = action == "note" ? 3 : 2;
    if (action == "note") {
        const auto state = impl_->capacity_state(actor.request_id);
        const auto pending = number(impl_->one("SELECT COUNT(*) AS n FROM n5_event_links l WHERE l.request_id=? AND action='note' "
            "AND NOT EXISTS(SELECT 1 FROM ops o WHERE o.request_id=l.request_id AND o.business_idempotency_key=l.attempt_id) "
            "AND NOT EXISTS(SELECT 1 FROM n5_event_receipts r WHERE r.link_id=l.link_id AND r.kind IN ('CLOSE','ERROR'))", {actor.request_id}), "n");
        require(number(state.at("counts").as_object(), "operations") + pending + 1 <= number(state.at("limits").as_object(), "operations"),
                ErrorCode::NotReady, "persistent note operation tail capacity unavailable");
    }
    Impl::EventProtocolScope step(*impl_, id, business_tail, control_tail);
    (void)impl_->reserve_attempt_in_transaction(actor, binding, reservation);
    const auto original = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {reservation.attempt_id});
    fact["envelope_sha256"] = sha256_hex(str(original, "request_envelope")); fact["reservation_sha256"] = original.at("reservation_sha256");
    const auto encoded = json_text(fact, 2 * 1024 * 1024);
    impl_->execute("INSERT INTO n5_event_links VALUES (?,?,?,?,?,?,?,?,?,?,?,?)", {id, actor.request_id, association.claim_id,
        reservation.attempt_id, reservation.call_id, action, business_tail, control_tail, encoded, sha256_hex(encoded), n5_encoded_bytes(encoded), stamp()});
    impl_->event_admission_business = 0; impl_->event_admission_control = 0;
    for (const auto& value : selected) {
        const Row member{{"format", "n5-event-link-v1"}, {"link_id", id}, {"source", impl_->event_source(value.as_int64())}};
        const auto bytes = json_text(member, 65536);
        impl_->execute("INSERT INTO n5_event_items VALUES (?,?,?,?,?)", {id, value, bytes, sha256_hex(bytes), n5_encoded_bytes(bytes)});
        impl_->execute("INSERT INTO n5_event_open_items VALUES (?,?)", {value, id});
    }
    const auto link = impl_->event_link_row(id);
    impl_->append_event_receipt(link, "RESERVED", Row{{"claim_sha256", snapshot.at("fact_sha256")}, {"link_sha256", sha256_hex(encoded)},
        {"reservation_sha256", original.at("reservation_sha256")}, {"business_tail", business_tail}, {"control_tail", control_tail}});
    impl_->guard_event_tail_capacity(actor.request_id);
    const auto result = impl_->event_link_info(link); tx.commit(); return result;
}

Json Store::claim_event_dispatch(const StoreActor& actor, const std::string& id) {
    const std::lock_guard lock(impl_->mutex);
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker}, false);
    const auto link = impl_->event_link_row(id); impl_->verify_event_link(link);
    require(impl_->event_fact(link).at("actor") == actor_json(actor), ErrorCode::Unauthorized, "dispatch original actor changed");
    Impl::EventProtocolScope step(*impl_, id);
    if (!impl_->event_receipt(id, "DISPATCH").is_null()) {
        const auto replay = impl_->claim_offline_call_in_transaction(actor, str(link, "attempt_id"));
        require(replay.at("dispatch_claimed") == false, ErrorCode::Corrupt, "original event dispatch authority disappeared");
        auto result = impl_->event_link_info(link).as_object(); result["dispatch_claimed"] = false;
        tx.commit(); return result;
    }
    require(!impl_->optional("SELECT seq FROM events WHERE request_id=? AND command_id=?", {link.at("request_id"), "offline-dispatch:" + str(link, "attempt_id")}),
            ErrorCode::Corrupt, "dispatch event exists without its atomic association receipt");
    impl_->event_link_current(actor, link);
    require(!impl_->event_application_halted, ErrorCode::NotReady, "persistent event Store retained a failed application; halt new dispatch");
    impl_->event_amendment_targets_current(impl_->event_fact(link));
    require(impl_->event_receipt(id, "SETTLEMENT").is_null() && impl_->event_receipt(id, "ERROR").is_null(), ErrorCode::NotReady,
            "settled or held event calls cannot dispatch");
    const auto claimed = impl_->claim_offline_call_in_transaction(actor, str(link, "attempt_id"));
    require(claimed.at("dispatch_claimed") == true, ErrorCode::Corrupt, "new event call did not obtain original unique dispatch");
    const auto event = impl_->one("SELECT seq,event_id,payload,result FROM events WHERE request_id=? AND command_id=?",
        {link.at("request_id"), "offline-dispatch:" + str(link, "attempt_id")});
    impl_->append_event_receipt(link, "DISPATCH", Row{{"event_seq", event.at("seq")}, {"event_id", event.at("event_id")},
        {"payload_sha256", sha256_hex(str(event, "payload"))}, {"result_sha256", sha256_hex(str(event, "result"))},
        {"reservation_sha256", impl_->event_fact(link).at("reservation_sha256")}});
    auto result = impl_->event_link_info(link).as_object(); result["dispatch_claimed"] = true;
    tx.commit(); return result;
}

Json Store::settle_event_observation(const std::string& id, const ModelResponse& response) {
    const std::lock_guard lock(impl_->mutex);
    Impl::Transaction tx(*impl_);
    const auto link = impl_->event_link_row(id); impl_->verify_event_link(link);
    require(!impl_->event_receipt(id, "DISPATCH").is_null(), ErrorCode::NotReady, "event observation lacks its original dispatch authority");
    const auto original = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {link.at("attempt_id")});
    const auto request = model_request_from_json(parse_json_strict(str(original, "request_envelope"), 4U * 1024U * 1024U));
    require(response.call_id == request.call_id && response.attempt_id == request.attempt_id && response.role == request.role &&
            response.role_call_index == request.role_call_index && response.binding == request.binding &&
            response.config_snapshot_id == request.config_snapshot_id, ErrorCode::Stale, "event response changed its complete original call identity");
    const auto raw = to_json(response);
    const auto outcome = response.status == ModelResultStatus::Completed ? "COMPLETED" :
        response.status == ModelResultStatus::Interrupted ? "INTERRUPTED" : "ERROR";
    Impl::EventProtocolScope step(*impl_, id);
    const auto settled = impl_->settle_model_attempt(request.attempt_id, response.binding, response.config_snapshot_id,
        outcome, to_json(response.usage), raw, true);
    impl_->append_event_receipt(link, "SETTLEMENT", Row{{"settlement_sha256", settled.at("settlement_sha256")},
        {"response_sha256", sha256_hex(str(impl_->one("SELECT response FROM call_attempts WHERE attempt_id=?", {request.attempt_id}), "response"))},
        {"usage_sha256", sha256_hex(str(impl_->one("SELECT usage FROM call_attempts WHERE attempt_id=?", {request.attempt_id}), "usage"))}});
    const auto result = impl_->event_link_info(link); tx.commit(); return result;
}

Json Store::commit_event_action_and_finish(const StoreActor& actor, const std::string& id, const SchemaRegistry& schemas) {
    const std::lock_guard lock(impl_->mutex);
    // Authentication precedes every replay and precedes the error writer: a
    // foreign or old-session actor cannot poison another owner's association.
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker}, false);
    const auto link = impl_->event_link_row(id); impl_->verify_event_link(link);
    const auto fact = impl_->event_fact(link);
    require(fact.at("actor") == actor_json(actor), ErrorCode::Unauthorized, "application original actor changed");
    if (!impl_->event_receipt(id, "CLOSE").is_null()) return impl_->event_link_info(link);
    require(impl_->event_receipt(id, "ERROR").is_null(), ErrorCode::NotReady, "original event application is held; only evidence may be retried");
    require(!impl_->event_receipt(id, "SETTLEMENT").is_null(), ErrorCode::NotReady, "event application requires its original settlement receipt");
    require(!impl_->event_application_halted, ErrorCode::NotReady, "persistent event Store retained a failed application; only observations may be flushed");
    require(impl_->event_application_started.insert(id).second, ErrorCode::NotReady, "original event application was already attempted; retain evidence without reapplication");
    try {
        Impl::Transaction tx(*impl_);
        Impl::EventProtocolScope step(*impl_, id);
        impl_->event_link_current(actor, link);
        const auto original = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {link.at("attempt_id")});
        require(!original.at("response").is_null() && number(original, "accepted") == 1 &&
                number(original, "unknown_input") == 0 && number(original, "unknown_output") == 0 && number(original, "unknown_cost") == 0,
                ErrorCode::UnknownOutcome, "event application requires complete original response and known original accounting");
        const auto request = model_request_from_json(parse_json_strict(str(original, "request_envelope"), 4U * 1024U * 1024U));
        const auto raw_response = parse_json_strict(str(original, "response"), 4U * 1024U * 1024U);
        const auto response = model_response_from_json(raw_response);
        require_complete_response(request, response);
        require(response.tool_calls.size() == 1, ErrorCode::InvalidInput, "persistent event requires exactly one declared action");
        const auto& tool = response.tool_calls.front(); const auto command = request.attempt_id + ":" + tool.call_id;
        const auto expected = number(fact.at("target").as_object(), "expected_state_version");
        const auto action = str(link, "action");
        std::map<std::string, Row> before_tasks;
        if (action == "amendment") for (const auto& task : impl_->all("SELECT * FROM tasks WHERE request_id=?", {actor.request_id}))
            before_tasks.emplace(str(task, "task_id"), task);
        Json result;
        Json decisions = Row{};
        if (action == "plan" || action == "amendment") {
            const auto validate_plan = [&](Json plan) {
                schemas.validate("plan", plan, true);
                plan.as_object()["plan_revision"] = next(request.binding.plan_revision);
                plan.as_object()["based_on_requirements_revision"] = request.binding.requirements_revision;
                schemas.validate("plan", plan);
                for (const auto& node : plan.at("nodes").as_array()) schemas.validate("task_brief", node.at("brief"));
                return plan;
            };
            if (action == "plan") {
                require(tool.name == "submit_plan", ErrorCode::Unauthorized, "persistent planning requires its declared action");
                const auto plan = validate_plan(tool.arguments);
                result = impl_->apply_independent_plan_in_transaction(actor, expected, request.binding.requirements_revision,
                    plan, Row{}, command + ":independent-plan", nullptr);
            } else {
                const auto& target = fact.at("target").at("workflow_action").at("target");
                const bool publish = target.at("plan_required").as_bool();
                require(tool.name == (publish ? "submit_amendment_plan" : "resolve_amendment"), ErrorCode::Unauthorized, "AMEND response changed its original phase");
                schemas.validate(tool.name, tool.arguments, true); decisions = tool.arguments.at("dispositions");
                for (const auto& decision : decisions.as_object())
                    require(contains(target.at("pending_node_ids"), std::string(decision.key())), ErrorCode::Unauthorized, "AMEND decision escaped its original pending targets");
                Array guards;
                for (const auto& task : target.at("tasks").as_array()) {
                    if (!publish && !decisions.as_object().contains(str(task.as_object(), "node_id"))) continue;
                    guards.emplace_back(Row{{"node_id", task.at("node_id")}, {"task_id", task.at("task_id")}, {"state_version", task.at("state_version")},
                        {"team_id", task.at("team_id")}, {"execution_epoch", task.at("execution_epoch")}});
                }
                if (publish) result = impl_->apply_independent_plan_in_transaction(actor, expected, request.binding.requirements_revision,
                    validate_plan(tool.arguments.at("plan")), decisions, command + ":amendment-plan", guards);
                else result = impl_->apply_independent_dispositions_in_transaction(actor, expected, request.binding.requirements_revision,
                    request.binding.plan_revision, decisions, command + ":amendment", guards);
            }
        } else if (action == "instruction") {
            require(tool.name == "issue_work_order", ErrorCode::Unauthorized, "persistent instruction requires its declared work order");
            schemas.validate("work_order", tool.arguments, true);
            auto next_binding = request.binding; next_binding.instruction_revision = next(next_binding.instruction_revision);
            auto order = tool.arguments; const auto envelope = to_json(next_binding);
            for (const auto& field : envelope.as_object()) order.as_object()[field.key()] = field.value();
            schemas.validate("work_order", order);
            result = impl_->issue_instruction_in_transaction(actor, request.binding, expected, order, command);
        } else {
            require(action == "note" && tool.name == "note", ErrorCode::Unsupported, "persistent Worker slice supports only a database note");
            object_fields(tool.arguments, {"text"}, {"text"});
            (void)workflow_text(tool.arguments, "text", 4000);
            const auto prepared = impl_->prepare_operation_in_transaction(actor, request.binding, expected, request.attempt_id,
                "note", tool.arguments, "none", command + ":prepare", false);
            const auto op_id = str(prepared.as_object(), "op_id");
            const auto started = impl_->start_operation_in_transaction(actor, op_id, number(prepared.as_object(), "state_version"), command + ":start");
            result = impl_->finish_operation_in_transaction(actor, op_id, request.binding, number(started.as_object(), "state_version"),
                "SUCCEEDED", "sqlite:operation_intents/" + op_id + "#sha256=" + sha256_hex(canonical_json(tool.arguments)), command + ":finish");
            const auto op = impl_->op_row(op_id);
            require(str(op, "status") == "SUCCEEDED" && op.at("business_idempotency_key") == request.attempt_id &&
                    op.at("args_sha256") == sha256_hex(canonical_json(tool.arguments)), ErrorCode::Corrupt, "note lacks its original successful operation proof");
        }
        const auto feedback = impl_->record_tool_feedback_in_transaction(request.attempt_id, request.binding, raw_response, result);
        Array effects;
        const auto prefix = command;
        for (const auto& event : impl_->all("SELECT seq,event_id,type,command_id,payload,result FROM events WHERE request_id=? AND substr(command_id,1,?)=? ORDER BY seq",
                {actor.request_id, static_cast<std::int64_t>(prefix.size()), prefix}))
            effects.emplace_back(Row{{"event_seq", event.at("seq")}, {"event_id", event.at("event_id")}, {"type", event.at("type")},
                {"command_id", event.at("command_id")}, {"payload_sha256", sha256_hex(str(event, "payload"))}, {"result_sha256", sha256_hex(str(event, "result"))}});
        require(!effects.empty(), ErrorCode::Corrupt, "persistent action lacks original business event proof");
        Array dispositions; std::vector<std::int64_t> completed;
        const auto captured = impl_->event_fact(impl_->event_claim_snapshot(str(link, "claim_id"))).at("items").as_array();
        for (const auto& value : captured) {
            const auto& source = value.as_object(); const auto inbox_id = number(source, "inbox_id");
            std::string disposition = "NOT_SELECTED";
            Row proof{{"inbox_id", inbox_id}, {"source_sha256", sha256_hex(canonical_json(value))}};
            if (n5_has_id(fact.at("selected_ids"), inbox_id)) {
                require(impl_->event_item_targets(source, fact), ErrorCode::Corrupt, "selected ID lost its individual target proof");
                disposition = "HANDLED";
                if (action == "amendment") {
                    const auto subject = str(source, "subject_task_id"); const auto& before = before_tasks.at(subject);
                    const auto node = str(before, "node_id");
                    if (!decisions.as_object().contains(node)) {
                        const auto after = impl_->task_row(subject);
                        require(after == before && number(after, "based_on_requirements_revision") < request.binding.requirements_revision &&
                            std::any_of(result.at("pending_tasks").as_array().begin(), result.at("pending_tasks").as_array().end(),
                                [&](const Json& item) { return item.at("task_id") == subject; }), ErrorCode::Corrupt,
                            "omitted AMEND target lacks an atomic unchanged pending proof");
                        disposition = "DEFERRED_NO_EFFECT"; proof["unchanged_task_sha256"] = sha256_hex(canonical_json(before));
                        proof["node_id"] = node; proof["decisions_sha256"] = sha256_hex(canonical_json(decisions));
                    } else {
                        const auto changed = std::find_if(result.at("tasks").as_array().begin(), result.at("tasks").as_array().end(),
                            [&](const Json& item) { return item.at("task_id") == subject; });
                        require(changed != result.at("tasks").as_array().end() && changed->at("node_id") == node, ErrorCode::Corrupt,
                            "AMEND subject has no individual committed transition");
                        proof["task_id"] = subject; proof["node_id"] = node;
                        proof["transition_sha256"] = sha256_hex(canonical_json(*changed));
                        proof["post_state_version"] = changed->at("state_version");
                    }
                } else if (action == "note") {
                    const auto task = impl_->task_row(request.binding.task_id);
                    require(number(task, "applied_task_revision") == request.binding.task_revision &&
                        number(task, "applied_plan_revision") == request.binding.plan_revision &&
                        number(task, "applied_instruction_revision") == request.binding.instruction_revision,
                        ErrorCode::Corrupt, "note proof lacks the original applied instruction tuple");
                    proof["op_id"] = result.at("op_id"); proof["arguments_sha256"] = sha256_hex(canonical_json(tool.arguments));
                    proof["binding"] = to_json(request.binding);
                } else if (action == "instruction") proof["instruction_binding"] = result.at("binding");
                else proof["plan_revision"] = result.at("plan_revision");
                if (disposition == "HANDLED") completed.push_back(inbox_id);
            }
            proof["disposition"] = disposition; dispositions.emplace_back(std::move(proof));
        }
        const auto finished = impl_->finish_events_in_transaction(actor, str(link, "claim_id"), number(fact.as_object(), "generation"), completed);
        for (const auto& value : fact.at("selected_ids").as_array())
            impl_->cas("DELETE FROM n5_event_open_items WHERE inbox_id=? AND link_id=?", {value, id});
        impl_->append_event_receipt(link, "CLOSE", Row{{"settlement_sha256", original.at("settlement_sha256")},
            {"feedback_sha256", feedback.at("record_sha256")}, {"response_sha256", sha256_hex(str(original, "response"))},
            {"effects", effects}, {"dispositions", dispositions}, {"completed_ids", finished.at("completed_ids")},
            {"released_ids", finished.at("released_ids")}, {"result", result}});
        impl_->guard_event_tail_capacity(actor.request_id);
        const auto closed = impl_->event_link_info(link); tx.commit(); impl_->event_application_started.erase(id); return closed;
    } catch (const Error& error) {
        // SQLite/storage corruption retains the original response and the
        // caller's result slot. This path cannot turn an I/O failure into an
        // application retry or an invented completion receipt.
        if (error.code() == ErrorCode::Storage || error.code() == ErrorCode::Corrupt) {
            impl_->event_application_halted = true; throw;
        }
        impl_->event_application_halted = true;
        Impl::Transaction tx(*impl_);
        Impl::EventProtocolScope step(*impl_, id);
        const auto original = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {link.at("attempt_id")});
        const auto response = parse_json_strict(str(original, "response"), 4U * 1024U * 1024U);
        const Row failure{{"status", "ERROR"}, {"runtime_error", true}, {"error_code", error_name(error.code())},
            {"message", error.what()}, {"observation_complete", false}};
        const auto feedback = impl_->record_tool_feedback_in_transaction(str(link, "attempt_id"), execution_binding_from_json(fact.at("binding")), response, failure);
        const auto current_claim = impl_->optional("SELECT * FROM event_actor_claims WHERE claim_id=? AND status='ACTIVE'", {link.at("claim_id")});
        const auto request = impl_->request_row(actor.request_id);
        if (current_claim && number(*current_claim, "requirements_revision") == number(request, "requirements_revision") &&
                number(request, "dispatcher_epoch") == actor.dispatcher_epoch &&
                (actor.team_id.empty() || number(impl_->team_row(actor.team_id), "execution_epoch") == actor.execution_epoch))
            (void)impl_->finish_events_in_transaction(actor, str(link, "claim_id"), number(fact.as_object(), "generation"), {});
        impl_->append_event_receipt(link, "ERROR", Row{{"error_code", error_name(error.code())}, {"message", error.what()},
            {"feedback_sha256", feedback.at("record_sha256")}, {"business_applied", false}, {"completed_ids", Array{}}, {"hold", true}});
        tx.commit(); impl_->event_application_started.erase(id); impl_->event_application_halted = false; throw;
    }
}

Json Store::Impl::event_link_info(const Row& link) const {
    verify_event_link(link);
    const auto fact = event_fact(link); const auto id = str(link, "link_id");
    const auto attempt = one("SELECT * FROM call_attempts WHERE attempt_id=?", {link.at("attempt_id")});
    std::string state = "RESERVED", hold = "HOLD_MISSING_DISPATCH";
    if (!event_receipt(id, "DISPATCH").is_null()) { state = "DISPATCHED"; hold = "HOLD_MISSING_RESPONSE"; }
    if (!event_receipt(id, "SETTLEMENT").is_null()) { state = "SETTLED"; hold = "HOLD_MISSING_FEEDBACK_PROOF"; }
    if (str(attempt, "status") == "UNKNOWN" || (!attempt.at("settlement_sha256").is_null() &&
        (number(attempt, "unknown_input") != 0 || number(attempt, "unknown_output") != 0 || number(attempt, "unknown_cost") != 0))) state = hold = "HOLD_UNKNOWN";
    const auto failure = event_receipt(id, "ERROR");
    if (!failure.is_null() && state != "HOLD_UNKNOWN") { state = "HOLD_ERROR"; hold = str(failure.at("body").as_object(), "error_code"); }
    const auto close = event_receipt(id, "CLOSE");
    if (!close.is_null()) { state = "CLOSED"; hold.clear(); }
    Array captured_ids, items, receipts;
    const auto captured = event_fact(event_claim_snapshot(str(link, "claim_id"))).at("items").as_array();
    for (const auto& source : captured) {
        const auto inbox_id = source.at("inbox_id"); captured_ids.push_back(inbox_id);
        auto item = one("SELECT inbox_id,status,claim_id FROM event_inbox WHERE inbox_id=?", {inbox_id});
        const auto open = optional("SELECT link_id FROM n5_event_open_items WHERE inbox_id=?", {inbox_id});
        item["open"] = open && open->at("link_id") == id; item["selected"] = n5_has_id(fact.at("selected_ids"), inbox_id.as_int64());
        items.emplace_back(std::move(item));
    }
    for (const auto& receipt : all("SELECT * FROM n5_event_receipts WHERE link_id=? ORDER BY ordinal", {id})) receipts.emplace_back(event_fact(receipt, 65536));
    Row result{{"link_id", id}, {"attempt_id", link.at("attempt_id")}, {"call_id", link.at("call_id")},
        {"request_id", link.at("request_id")}, {"claim_id", link.at("claim_id")}, {"generation", fact.at("generation")},
        {"state", state}, {"hold_reason", hold}, {"binding", fact.at("binding")}, {"target", fact.at("target")},
        {"selected_ids", fact.at("selected_ids")}, {"captured_ids", captured_ids}, {"items", items}, {"receipts", receipts},
        {"metadata_bytes_and_reserved", event_metadata_bytes(str(link, "request_id"))}};
    if (!close.is_null()) {
        const auto& body = close.at("body");
        result["result"] = body.at("result"); result["dispositions"] = body.at("dispositions");
        result["completed_ids"] = body.at("completed_ids"); result["released_ids"] = body.at("released_ids");
    } else { result["result"] = failure.is_null() ? Json(nullptr) : failure.at("body"); result["dispositions"] = Array{}; result["completed_ids"] = Array{}; }
    return result;
}
Json Store::event_link_status(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex);
    // One SQLite snapshot covers facts, projection and current inbox status.
    impl_->exec("BEGIN");
    try { auto result = impl_->event_link_info(impl_->event_link_row(id)); impl_->exec("COMMIT"); return result; }
    catch (...) { impl_->rollback(); throw; }
}
std::vector<Json> Store::request_event_links(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex);
    (void)impl_->request_row(id);
    require(impl_->event_association_schema, ErrorCode::Unsupported, "persistent event protocol requires Store 411");
    impl_->exec("BEGIN");
    try {
        std::vector<Json> result;
        for (const auto& link : impl_->all("SELECT * FROM n5_event_links WHERE request_id=? ORDER BY rowid", {id})) result.push_back(impl_->event_link_info(link));
        impl_->exec("COMMIT"); return result;
    } catch (...) { impl_->rollback(); throw; }
}
