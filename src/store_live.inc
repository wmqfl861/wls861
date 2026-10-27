// Included in store.cpp after Store::Impl. All writes use the original Store
// mutex, writer scope and SQLite transaction; network and source verification
// are deliberately outside these methods.
namespace {
constexpr std::array<std::string_view, 5> usage_fields{
    "input_tokens", "output_tokens", "cache_read_tokens", "cache_write_tokens", "cost_micro_usd"};
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> ledger_dimensions{{
    {"input", "input_tokens"}, {"output", "output_tokens"}, {"cost", "cost_micro_usd"}}};

std::string live_hex(std::string_view bytes) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string encoded; encoded.reserve(bytes.size() * 2U);
    for (const auto character : bytes) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(alphabet[byte >> 4U]); encoded.push_back(alphabet[byte & 15U]);
    }
    return encoded;
}
std::string live_unhex(std::string_view encoded) {
    require(encoded.size() % 2U == 0, ErrorCode::Corrupt, "invalid private evidence encoding");
    const auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        fail(ErrorCode::Corrupt, "invalid private evidence encoding");
    };
    std::string bytes; bytes.reserve(encoded.size() / 2U);
    for (std::size_t i = 0; i < encoded.size(); i += 2U)
        bytes.push_back(static_cast<char>((digit(encoded[i]) << 4U) | digit(encoded[i + 1U])));
    return bytes;
}
Row projection_usage(const Row& row) {
    Row values;
    for (const auto field : usage_fields) values[field] = row.at(field);
    return values;
}
bool exceeds_reservation(const Row& original, const Row& usage) {
    for (const auto& [dimension, field] : ledger_dimensions)
        if (!usage.at(field).is_null() && number(usage, field) > number(original, "reserved_" + std::string(dimension)))
            return true;
    return false;
}
Row initial_projection(const Row& original) {
    Row projection{{"attempt_id", original.at("attempt_id")},
        {"settlement_sha256", original.at("settlement_sha256")}, {"state_version", 1},
        {"disputed", 0}, {"over_reserved", exceeds_reservation(original, original) ? 1 : 0}};
    for (const auto field : usage_fields) projection[field] = original.at(field);
    const Row fingerprint{{"format", "wls-original-usage-projection-v1"}, {"projection", projection}};
    projection["chain_sha256"] = sha256_hex(canonical_json(fingerprint));
    return projection;
}
Row original_usage_identity(const Row& original) {
    Row result;
    for (const auto* key : {"attempt_id", "call_id", "request_id", "task_id", "team_id", "role", "model_ref",
            "config_snapshot_sha256", "reservation_sha256", "reserved_input", "reserved_output", "reserved_cost",
            "status", "accepted", "state_version", "settlement_sha256", "created_at", "settled_at"})
        result[key] = original.at(key);
    result["frozen_binding"] = parse_json_strict(str(original, "frozen_binding"));
    result["usage"] = projection_usage(original);
    return result;
}
Json projection_status(const Row& original, const std::optional<Row>& projection) {
    Array unknown;
    const auto& current = projection ? *projection : original;
    for (const auto field : usage_fields) if (current.at(field).is_null()) unknown.emplace_back(field);
    const bool settled = !original.at("settlement_sha256").is_null();
    const bool disputed = projection && number(*projection, "disputed") != 0;
    return Row{{"original_settlement", original_usage_identity(original)},
        {"projection", projection ? Json(*projection) : Json(nullptr)}, {"unknown_dimensions", unknown},
        {"accounting_complete", settled && projection.has_value() && unknown.empty() && !disputed},
        {"business_authority_restored", false}};
}
struct ReconciledProjection {
    Row after;
    Row delta;
    Array conflicts;
};
ReconciledProjection reconcile_projection(const Row& original, const Row& before, const Row& proposed,
                                         const std::string& fingerprint) {
    ReconciledProjection value{before, {}, {}};
    for (const auto field : usage_fields) {
        if (proposed.at(field).is_null()) continue;
        if (!before.at(field).is_null() && before.at(field) != proposed.at(field)) value.conflicts.emplace_back(field);
        else if (before.at(field).is_null()) value.after[field] = proposed.at(field);
    }
    for (const auto& [dimension, field] : ledger_dimensions) {
        const bool filled = before.at(field).is_null() && !value.after.at(field).is_null();
        const auto reservation = number(original, "reserved_" + std::string(dimension));
        value.delta[dimension] = Row{{"reserved", filled ? -reservation : 0},
            {"spent", filled ? number(value.after, field) : 0}, {"unknown", filled ? -reservation : 0}};
    }
    value.after["disputed"] = number(before, "disputed") != 0 || !value.conflicts.empty() ? 1 : 0;
    value.after["over_reserved"] = number(before, "over_reserved") != 0 || exceeds_reservation(original, value.after) ? 1 : 0;
    value.after["state_version"] = next(number(before, "state_version"));
    value.after.erase("chain_sha256");
    value.after["chain_sha256"] = sha256_hex(canonical_json(Row{{"format", "wls-usage-reconciliation-chain-v1"},
        {"previous_chain_sha256", before.at("chain_sha256")}, {"record_sha256", fingerprint}, {"projection", value.after}}));
    return value;
}
std::int64_t apply_ledger_delta(std::int64_t value, std::int64_t delta) {
    if (delta >= 0) return add(value, delta);
    require(delta != INT64_MIN && value >= -delta, ErrorCode::Corrupt, "usage reconciliation would lose a reservation");
    return value + delta;
}
Row live_reconciliation_command(const StoreActor& actor, const VerifiedUsageEvidence& evidence,
                                const UsageReconciliation& reconciliation) {
    const auto& statement = evidence.statement();
    Row fields{{"format", "wls-usage-reconciliation-v1"},
        {"reconciliation_id", reconciliation.reconciliation_id}, {"idempotency_key", reconciliation.idempotency_key},
        {"expected_projection_version", reconciliation.expected_projection_version},
        {"source_kind", evidence.source_kind()}, {"source_reference_sha256", sha256_hex(evidence.source_reference())},
        {"evidence_sha256", evidence.sha256()}, {"statement", Row{{"attempt_id", statement.attempt_id},
            {"settlement_sha256", statement.settlement_sha256}, {"usage", to_json(statement.usage)}}}};
    if (!evidence.required_reconciliation_id().empty())
        fields["exclusive_source"] = Row{{"kind", "wls-signed-billing-source-v1"},
            {"identity_sha256", evidence.source_reference().substr(std::string_view("wls-signed-billing-v1:").size())}};
    return payload(actor, "reconcile_usage", std::move(fields));
}
std::string validate_usage_reconciliation_input(const VerifiedUsageEvidence& evidence,
                                                const UsageReconciliation& reconciliation) {
    text_field(reconciliation.reconciliation_id, "reconciliation_id");
    text_field(reconciliation.idempotency_key, "usage idempotency key");
    require(reconciliation.expected_projection_version > 0 &&
            !evidence.bytes().empty() && evidence.bytes().size() <= 256U * 1024U &&
            sha256_hex(evidence.bytes()) == evidence.sha256() &&
            (evidence.source_kind() == "loopback_fixture" || evidence.source_kind() == "host_verified_provider" ||
             evidence.source_kind() == "host_no_dispatch"),
            ErrorCode::InvalidInput, "invalid verified usage evidence or projection version");
    if (evidence.required_reconciliation_id().empty()) return {};
    constexpr std::string_view source_prefix = "wls-signed-billing-v1:";
    require(evidence.source_reference().starts_with(source_prefix), ErrorCode::InvalidInput,
            "signed billing evidence lost its exclusive source identity");
    const auto identity = evidence.source_reference().substr(source_prefix.size());
    require(identity.size() == 64 && std::all_of(identity.begin(), identity.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }), ErrorCode::InvalidInput, "invalid exclusive billing source fingerprint");
    const auto prefix = "billing:" + identity + ":";
    require(evidence.required_reconciliation_id() == prefix + evidence.sha256() &&
            reconciliation.reconciliation_id == evidence.required_reconciliation_id() &&
            evidence.source_kind() != "host_no_dispatch", ErrorCode::InvalidInput,
            "signed billing reconciliation must preserve its indexed source identity");
    return prefix;
}
} // namespace

bool Store::Impl::has_unsettled_model(const std::string& id, const std::string& team_id) const {
    const Array args = team_id.empty() ? Array{id} : Array{id, team_id};
    const auto team_filter = team_id.empty() ? std::string{} : " AND c.team_id=?";
    if (!reconciliation_schema)
        return optional("SELECT c.attempt_id FROM call_attempts c WHERE c.request_id=? AND "
            "(c.status IN ('PENDING','UNKNOWN') OR c.unknown_input=1 OR c.unknown_output=1 OR c.unknown_cost=1)" +
            team_filter + " LIMIT 1", args).has_value();
    return optional("SELECT c.attempt_id FROM call_attempts c LEFT JOIN live_attempt_execution l USING(attempt_id) "
        "LEFT JOIN live_usage_projection p USING(attempt_id) WHERE c.request_id=? AND "
        "(c.status IN ('PENDING','UNKNOWN') OR "
        "(l.attempt_id IS NULL AND (c.unknown_input=1 OR c.unknown_output=1 OR c.unknown_cost=1)) OR "
        "(l.attempt_id IS NOT NULL AND (p.attempt_id IS NULL OR p.disputed=1 OR p.input_tokens IS NULL OR "
        "p.output_tokens IS NULL OR p.cache_read_tokens IS NULL OR p.cache_write_tokens IS NULL OR p.cost_micro_usd IS NULL)))" +
        team_filter + " LIMIT 1", args).has_value();
}

void Store::Impl::live_accounting_ready(const std::string& id, const std::string& excluding) const {
    require(reconciliation_schema, ErrorCode::Unsupported, "live accounting requires the 410 migration");
    require(!optional("SELECT c.attempt_id FROM call_attempts c JOIN live_attempt_execution l USING(attempt_id) "
        "LEFT JOIN live_usage_projection p USING(attempt_id) WHERE c.request_id=? AND c.attempt_id<>? AND "
        "(c.settlement_sha256 IS NULL OR p.attempt_id IS NULL OR p.disputed=1 OR p.over_reserved=1 OR "
        "p.input_tokens IS NULL OR p.output_tokens IS NULL OR p.cache_read_tokens IS NULL OR "
        "p.cache_write_tokens IS NULL OR p.cost_micro_usd IS NULL) LIMIT 1", {id, excluding}),
        ErrorCode::UnknownOutcome, "unresolved or over-reservation live accounting blocks new paid execution");
    const auto budget = one("SELECT * FROM request_budgets WHERE request_id=?", {id});
    for (const auto& [dimension, unused] : ledger_dimensions) {
        (void)unused;
        require(add(number(budget, "spent_" + std::string(dimension)), number(budget, "reserved_" + std::string(dimension))) <=
                number(budget, std::string(dimension) + "_limit"), ErrorCode::Quota, "original request budget is overspent");
    }
}

void Store::Impl::initialize_usage_projection(const Row& original) {
    require(reconciliation_schema && !original.at("settlement_sha256").is_null(),
            ErrorCode::Corrupt, "live settlement requires an original usage projection");
    const auto projection = initial_projection(original);
    execute("INSERT INTO live_usage_projection VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        {projection.at("attempt_id"), projection.at("settlement_sha256"), projection.at("input_tokens"),
         projection.at("output_tokens"), projection.at("cache_read_tokens"), projection.at("cache_write_tokens"),
         projection.at("cost_micro_usd"), projection.at("disputed"), projection.at("over_reserved"),
         projection.at("state_version"), projection.at("chain_sha256")});
}

bool Store::live_model_enabled() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->live_schema && impl_->reconciliation_schema;
}

void Store::validate_control_file_path(const std::filesystem::path& path) const {
    require(path.is_absolute() && path == path.lexically_normal(), ErrorCode::InvalidInput,
            "control file placement requires an absolute normalized path");
    const auto normalized = path.string();
    require(!normalized.empty() && normalized.find('\0') == std::string::npos, ErrorCode::InvalidInput,
            "invalid control file path");
    const std::lock_guard lock(impl_->mutex);
    require(!impl_->optional("SELECT 1 AS present FROM workspace_bindings WHERE canonical_path=? OR "
            "substr(?,1,length(canonical_path)+1)=canonical_path||'/' LIMIT 1", {normalized, normalized}),
            ErrorCode::Unauthorized, "host control files must be outside every bound business workspace");
}

Json Store::claim_live_send(const StoreActor& actor, const std::string& attempt_id,
        std::int64_t expected_attempt_version, const EncodedResponsesRequest& encoded,
        const HttpsTransportOptions& transport, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    require(impl_->live_schema && impl_->reconciliation_schema, ErrorCode::Unsupported, "live send requires the complete 410 schema");
    text_field(command_id, "live send command_id");
    require(expected_attempt_version > 0, ErrorCode::InvalidInput, "live send requires the original attempt CAS");
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker}, false);
    const auto original = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {attempt_id});
    impl_->check_request_scope(str(original, "request_id"));
    require(str(original, "request_id") == actor.request_id && str(original, "role") == role_name(actor.role) &&
            str(original, "principal_id") == actor.principal_id, ErrorCode::Unauthorized, "live send belongs to the original actor");
    const auto request = model_request_from_json(parse_json_strict(str(original, "request_envelope"), 4U * 1024U * 1024U));
    impl_->actor_binding(actor, request.binding);
    const auto live = impl_->one("SELECT * FROM live_attempt_execution WHERE attempt_id=?", {attempt_id});
    const auto current_request = impl_->request_row(actor.request_id);
    const auto frozen = FrozenRequestConfig::restore(str(current_request, "config_snapshot"), str(current_request, "config_snapshot_sha256"));
    const auto settings = live_model_settings_from_json(frozen.model_profile(actor.role), request.model.model);
    const auto required_encoding = encode_responses_request(settings.provider, request, settings.stream);
    require(to_json(encoded) == to_json(required_encoding) &&
            encoded.record_sha256 == str(live, "encoded_record_sha256") &&
            live_transport_options_sha256(transport) == str(live, "transport_options_sha256") &&
            canonical_json(live_model_settings_json(settings)) == str(live, "settings"),
            ErrorCode::Conflict, "send bytes or transport settings differ from original admission");
    const auto command = payload(actor, "claim_live_send", Row{{"attempt_id", attempt_id},
        {"reservation_sha256", original.at("reservation_sha256")}, {"encoded_record_sha256", encoded.record_sha256},
        {"transport_options_sha256", live_transport_options_sha256(transport)}});
    const auto fingerprint = sha256_hex(canonical_json(command));
    if (const auto existing = impl_->optional("SELECT * FROM live_send_claims WHERE attempt_id=?", {attempt_id})) {
        require(str(*existing, "command_sha256") == fingerprint && str(*existing, "command_id") == command_id,
                ErrorCode::Conflict, "live send claim cannot be replaced or reused with different intent");
        const Row result{{"attempt_id", attempt_id}, {"claim_id", existing->at("claim_id")},
            {"original_attempt_version", existing->at("original_attempt_version")}, {"new_send_authority", false}};
        tx.commit(); return result;
    }
    (void)impl_->authorize(actor, {Role::Dispatcher, Role::TeamLead, Role::Worker});
    impl_->matches(request.binding);
    require(str(original, "status") == "PENDING" && original.at("settlement_sha256").is_null() &&
            number(original, "state_version") == expected_attempt_version, ErrorCode::Conflict,
            "original live attempt is not pending at the captured CAS");
    require(frozen.json().at("system").at("model_calls").at("real_calls_enabled").as_bool(),
            ErrorCode::Unauthorized, "original request did not enable live calls");
    if (actor.role == Role::Worker) impl_->ready(actor, request.binding);
    bool experience = false;
    if (impl_->acceptance_schema) if (const auto job = impl_->optional("SELECT * FROM experience_attempts WHERE attempt_id=?", {attempt_id})) {
        (void)impl_->validate_experience_target(actor, request.binding, str(*job, "experience_id"),
                                               number(*job, "job_version"), str(*job, "source_sha256"));
        experience = true;
    }
    require(experience || !terminal_request(str(current_request, "status")), ErrorCode::NotReady,
            "live send cannot restart terminated business");
    if (!experience && actor.role == Role::TeamLead) {
        const auto task = impl_->task_row(actor.task_id);
        require(!terminal_task(str(task, "status")), ErrorCode::NotReady, "live send task is no longer active");
    }
    impl_->live_accounting_ready(actor.request_id, attempt_id);
    const auto claim_id = new_id("send");
    impl_->execute("INSERT INTO live_send_claims VALUES (?,?,?,?,?,?,?,?,?)",
        {attempt_id, claim_id, command_id, fingerprint, expected_attempt_version,
         canonical_json(to_json(request.binding)), canonical_json(actor_json(actor)), impl_->writer_instance, stamp()});
    const Row recorded{{"attempt_id", attempt_id}, {"claim_id", claim_id},
        {"original_attempt_version", expected_attempt_version}};
    impl_->event(actor.request_id, "send:" + attempt_id, command, recorded, role_name(actor.role));
    tx.commit();
    auto result = recorded; result["new_send_authority"] = true; return result;
}

Json Store::record_live_transport(const std::string& attempt_id, const std::string& claim_id,
                                  const HttpsTransportResult& result) {
    // Validate hashes while no database transaction is held. The full report
    // may contain echoed secrets; only its explicitly safe summary is an event.
    const auto summary = https_transport_summary(result);
    const auto response_bytes = json_text(to_json(result.response), 4U * 1024U * 1024U);
    const auto protocol_bytes = json_text(result.protocol_evidence, 4U * 1024U * 1024U);
    require(result.raw_headers.size() <= 1024U * 1024U && result.raw_body.size() <= 4U * 1024U * 1024U,
            ErrorCode::Quota, "Store private transport observation exceeds its durable bound");
    const auto headers_hex = live_hex(result.raw_headers);
    const auto body_hex = live_hex(result.raw_body);
    require(headers_hex.size() + body_hex.size() + response_bytes.size() + protocol_bytes.size() <= 12U * 1024U * 1024U,
            ErrorCode::Quota, "Store combined transport observation exceeds its durable bound");
    const std::lock_guard lock(impl_->mutex);
    require(impl_->live_schema, ErrorCode::Unsupported, "live observation requires the 409 migration");
    Impl::Transaction tx(*impl_);
    const auto original = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {attempt_id});
    impl_->check_request_scope(str(original, "request_id"));
    const auto claim = impl_->one("SELECT * FROM live_send_claims WHERE attempt_id=?", {attempt_id});
    require(str(claim, "claim_id") == claim_id, ErrorCode::Unauthorized, "transport observation changed its original send claim");
    const auto live = impl_->one("SELECT * FROM live_attempt_execution WHERE attempt_id=?", {attempt_id});
    require(result.encoded_record_sha256 == str(live, "encoded_record_sha256") &&
            result.request_body_sha256 == str(live, "request_body_sha256") &&
            result.profile_sha256 == str(live, "profile_sha256") &&
            result.logical_request_sha256 == str(live, "logical_request_sha256") &&
            result.transport_options_sha256 == str(live, "transport_options_sha256"), ErrorCode::Conflict,
            "transport observation differs from original admitted bytes, profile or options");
    const auto request = model_request_from_json(parse_json_strict(str(original, "request_envelope"), 4U * 1024U * 1024U));
    validate_response(request, result.response);
    const auto settings = parse_json_strict(str(live, "settings"), 2U * 1024U * 1024U);
    require(result.ca_bundle_sha256 == sha256_hex(std::string(settings.at("transport").at("ca_bundle_pem").as_string())),
            ErrorCode::Conflict, "transport observation changed the explicit trust material");
    const Row answer{{"attempt_id", attempt_id}, {"claim_id", claim_id}, {"transport", summary}};
    if (const auto previous = impl_->optional("SELECT record_sha256 FROM live_transport_observations WHERE attempt_id=?", {attempt_id})) {
        require(str(*previous, "record_sha256") == result.record_sha256, ErrorCode::Conflict,
                "original transport observation is immutable");
        tx.commit(); return answer;
    }
    require(original.at("settlement_sha256").is_null() ||
            impl_->optional("SELECT attempt_id FROM live_unobserved_settlements WHERE attempt_id=?", {attempt_id}).has_value(),
            ErrorCode::Conflict, "transport observation cannot follow an unrelated settlement");
    impl_->execute("INSERT INTO live_transport_observations VALUES (?,?,?,?,?,?,?,?,?)",
        {attempt_id, claim_id, result.record_sha256, json_text(summary), response_bytes,
         headers_hex, body_hex, protocol_bytes, stamp()});
    impl_->event(str(original, "request_id"), "transport:" + attempt_id,
        Row{{"type", "record_live_transport"}, {"attempt_id", attempt_id}, {"claim_id", claim_id},
            {"record_sha256", result.record_sha256}}, answer);
    tx.commit(); return answer;
}

Json Store::live_attempt(const std::string& attempt_id, bool include_private) const {
    const std::lock_guard lock(impl_->mutex);
    require(impl_->live_schema, ErrorCode::Unsupported, "live attempt query requires the 409 migration");
    const auto original = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {attempt_id});
    impl_->check_request_scope(str(original, "request_id"));
    const auto live = impl_->one("SELECT * FROM live_attempt_execution WHERE attempt_id=?", {attempt_id});
    Row identity = live; identity.erase("settings");
    Row answer{{"original_attempt", original_usage_identity(original)}, {"execution", identity},
        {"send_claim", nullptr}, {"transport", nullptr}};
    if (const auto claim = impl_->optional("SELECT claim_id,original_attempt_version,created_at FROM live_send_claims WHERE attempt_id=?", {attempt_id}))
        answer["send_claim"] = *claim;
    answer["host_unobserved_settlement"] = nullptr;
    if (const auto host = impl_->optional("SELECT claim_id,command_sha256,original_attempt_version,reason,created_at FROM live_unobserved_settlements WHERE attempt_id=?", {attempt_id}))
        answer["host_unobserved_settlement"] = *host;
    if (const auto observation = impl_->optional("SELECT * FROM live_transport_observations WHERE attempt_id=?", {attempt_id})) {
        answer["transport"] = parse_json_strict(str(*observation, "safe_summary"));
        if (include_private) {
            Row private_record = *observation;
            private_record["response"] = parse_json_strict(str(*observation, "response"), 4U * 1024U * 1024U);
            private_record["protocol_evidence"] = parse_json_strict(str(*observation, "protocol_evidence"), 4U * 1024U * 1024U);
            answer["private_observation"] = private_record;
        }
    }
    if (impl_->reconciliation_schema)
        answer["accounting"] = projection_status(original,
            impl_->optional("SELECT * FROM live_usage_projection WHERE attempt_id=?", {attempt_id}));
    if (include_private) {
        answer["original_attempt"] = exposed(original);
        answer["frozen_live_settings"] = parse_json_strict(str(live, "settings"), 2U * 1024U * 1024U);
    }
    return answer;
}

Json Store::usage_projection(const std::string& attempt_id) const {
    const std::lock_guard lock(impl_->mutex);
    require(impl_->reconciliation_schema, ErrorCode::Unsupported, "usage query requires the 410 migration");
    const auto original = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {attempt_id});
    impl_->check_request_scope(str(original, "request_id"));
    (void)impl_->one("SELECT attempt_id FROM live_attempt_execution WHERE attempt_id=?", {attempt_id});
    auto answer = projection_status(original,
        impl_->optional("SELECT * FROM live_usage_projection WHERE attempt_id=?", {attempt_id}));
    Array events;
    for (const auto& event : impl_->all("SELECT result FROM usage_reconciliations WHERE attempt_id=? ORDER BY expected_projection_version", {attempt_id}))
        events.emplace_back(parse_json_strict(str(event, "result")));
    answer.as_object()["reconciliations"] = std::move(events);
    return answer;
}

Json Store::settle_unobserved_live(const StoreActor& root, const std::string& attempt_id,
        std::int64_t expected_attempt_version, const std::string& reason, const std::string& idempotency_key) {
    require((reason == "unobserved_after_recovery" || reason == "not_dispatched_after_recovery") && expected_attempt_version > 0,
            ErrorCode::InvalidInput, "host missing-response settlement requires an explicit recovery reason and CAS");
    text_field(idempotency_key, "host missing-response idempotency key");
    const auto command = payload(root, "settle_unobserved_live", Row{{"attempt_id", attempt_id},
        {"expected_attempt_version", expected_attempt_version}, {"reason", reason}, {"idempotency_key", idempotency_key}});
    const auto fingerprint = sha256_hex(canonical_json(command));
    const std::lock_guard lock(impl_->mutex);
    require(impl_->reconciliation_schema, ErrorCode::Unsupported, "host missing-response settlement requires the 410 migration");
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(root, {Role::Root}, false);
    const auto original = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {attempt_id});
    require(str(original, "request_id") == root.request_id, ErrorCode::Unauthorized, "host recovery observation belongs to another request");
    const auto execution = impl_->one("SELECT * FROM live_attempt_execution WHERE attempt_id=?", {attempt_id});
    const auto claim = impl_->optional("SELECT * FROM live_send_claims WHERE attempt_id=?", {attempt_id});
    if (const auto prior = impl_->optional("SELECT * FROM live_unobserved_settlements WHERE attempt_id=?", {attempt_id})) {
        require(str(*prior, "command_sha256") == fingerprint && str(*prior, "idempotency_key") == idempotency_key,
                ErrorCode::Conflict, "host missing-response observation is immutable");
        require(!original.at("settlement_sha256").is_null(), ErrorCode::Corrupt, "host recovery observation lacks its atomic original settlement");
        const auto answer = exposed(original); tx.commit(); return answer;
    }
    (void)impl_->authorize(root, {Role::Root});
    require(str(execution, "writer_instance") != impl_->writer_instance &&
            (!claim || str(*claim, "writer_instance") != impl_->writer_instance) && str(original, "status") == "UNKNOWN" &&
            number(original, "state_version") == expected_attempt_version && original.at("settlement_sha256").is_null(),
            ErrorCode::NotReady, "only an original attempt recovered from a previous writer can settle as unobserved");
    require((reason == "not_dispatched_after_recovery") == !claim.has_value(),
            ErrorCode::Conflict, "host recovery reason must match the original durable send authority");
    require(!impl_->optional("SELECT attempt_id FROM live_transport_observations WHERE attempt_id=?", {attempt_id}),
            ErrorCode::Conflict, "a recorded original transport response cannot become unobserved");
    impl_->execute("INSERT INTO live_unobserved_settlements VALUES (?,?,?,?,?,?,?,?)",
        {attempt_id, claim ? claim->at("claim_id") : Json(nullptr), idempotency_key, fingerprint, expected_attempt_version,
         canonical_json(actor_json(root)), reason, stamp()});
    const auto request = model_request_from_json(parse_json_strict(str(original, "request_envelope"), 4U * 1024U * 1024U));
    const auto response = unobserved_live_response(request, reason);
    const auto answer = impl_->settle_model_attempt(attempt_id, request.binding, request.config_snapshot_id,
        "INTERRUPTED", to_json(response.usage), to_json(response), true);
    require(answer.at("accepted") == 0, ErrorCode::Corrupt, "host missing-response observation cannot restore business authority");
    tx.commit(); return answer;
}

Json Store::reconcile_usage(const StoreActor& root, const VerifiedUsageEvidence& evidence,
                            const UsageReconciliation& reconciliation) {
    const auto& statement = evidence.statement();
    const auto exclusive_prefix = validate_usage_reconciliation_input(evidence, reconciliation);
    const auto proposed = to_json(model_usage_from_json(to_json(statement.usage))).as_object();
    const auto command = live_reconciliation_command(root, evidence, reconciliation);
    const auto fingerprint = sha256_hex(canonical_json(command));
    const std::lock_guard lock(impl_->mutex);
    require(impl_->reconciliation_schema, ErrorCode::Unsupported, "usage reconciliation requires the 410 migration");
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(root, {Role::Root}, false);
    const auto original = impl_->one("SELECT * FROM call_attempts WHERE attempt_id=?", {statement.attempt_id});
    require(str(original, "request_id") == root.request_id, ErrorCode::Unauthorized, "usage evidence belongs to another request");
    impl_->check_request_scope(root.request_id);
    const auto live = impl_->one("SELECT * FROM live_attempt_execution WHERE attempt_id=?", {statement.attempt_id});
    require(evidence.source_kind() != "loopback_fixture" || str(live, "execution_kind") == "local_tls",
            ErrorCode::Unauthorized, "fixture evidence cannot verify provider usage");
    if (evidence.source_kind() == "host_no_dispatch") {
        const auto observation = impl_->optional("SELECT reason FROM live_unobserved_settlements WHERE attempt_id=?", {statement.attempt_id});
        require(observation && str(*observation, "reason") == "not_dispatched_after_recovery" &&
                !impl_->optional("SELECT attempt_id FROM live_send_claims WHERE attempt_id=?", {statement.attempt_id}),
                ErrorCode::Unauthorized, "host no-dispatch evidence requires the immutable recovered absence of send authority");
        for (const auto field : usage_fields)
            require(!proposed.at(field).is_null() && number(proposed, field) == 0, ErrorCode::InvalidInput,
                    "host no-dispatch evidence must confirm all five zero dimensions");
    }
    require(!original.at("settlement_sha256").is_null() && str(original, "settlement_sha256") == statement.settlement_sha256,
            ErrorCode::Conflict, "usage evidence changed the original settlement identity");
    if (!exclusive_prefix.empty()) {
        // The primary-key range is an indexed source-identity lookup. Receipt
        // revisions/key rotation use different evidence suffixes, but remain
        // owned by the first original attempt within this transaction.
        if (const auto owner = impl_->optional("SELECT attempt_id,source_reference,source_kind FROM usage_reconciliations "
                "WHERE reconciliation_id>=? AND reconciliation_id<? ORDER BY reconciliation_id LIMIT 1",
                {exclusive_prefix, exclusive_prefix + "~"})) {
            require(str(*owner, "source_reference") == evidence.source_reference() &&
                    str(*owner, "source_kind") == evidence.source_kind() &&
                    str(*owner, "attempt_id") == statement.attempt_id,
                    ErrorCode::Conflict, "signed billing source is already owned by a different original attempt");
        }
    }
    if (const auto previous = impl_->optional("SELECT * FROM usage_reconciliations WHERE reconciliation_id=? OR (attempt_id=? AND idempotency_key=?)",
            {reconciliation.reconciliation_id, statement.attempt_id, reconciliation.idempotency_key})) {
        require(str(*previous, "record_sha256") == fingerprint && str(*previous, "reconciliation_id") == reconciliation.reconciliation_id &&
                str(*previous, "attempt_id") == statement.attempt_id, ErrorCode::Conflict,
                "usage idempotency key or reconciliation ID was reused with different evidence");
        const auto answer = parse_json_strict(str(*previous, "result")); tx.commit(); return answer;
    }
    // Accounting is a control action. A current Root identity is required, but
    // terminal requests, AMEND and original stale response fences stay intact.
    (void)impl_->authorize(root, {Role::Root});
    const auto before = impl_->one("SELECT * FROM live_usage_projection WHERE attempt_id=?", {statement.attempt_id});
    require(number(before, "state_version") == reconciliation.expected_projection_version,
            ErrorCode::Conflict, "usage projection compare-and-swap changed");
    const auto updated = reconcile_projection(original, before, proposed, fingerprint);
    auto budget = impl_->one("SELECT * FROM request_budgets WHERE request_id=?", {root.request_id});
    for (const auto& [dimension, field] : ledger_dimensions) {
        (void)field;
        const auto& delta = updated.delta.at(dimension).as_object();
        for (const auto* balance : {"reserved", "spent", "unknown"}) {
            const auto key = std::string(balance) + "_" + std::string(dimension);
            budget[key] = apply_ledger_delta(number(budget, key), number(delta, balance));
        }
    }
    impl_->cas("UPDATE request_budgets SET reserved_input=?,reserved_output=?,reserved_cost=?,spent_input=?,spent_output=?,spent_cost=?,unknown_input=?,unknown_output=?,unknown_cost=?,state_version=state_version+1 WHERE request_id=? AND state_version=?",
        {budget.at("reserved_input"), budget.at("reserved_output"), budget.at("reserved_cost"),
         budget.at("spent_input"), budget.at("spent_output"), budget.at("spent_cost"),
         budget.at("unknown_input"), budget.at("unknown_output"), budget.at("unknown_cost"), root.request_id, number(budget, "state_version")});
    const auto& after = updated.after;
    impl_->cas("UPDATE live_usage_projection SET input_tokens=?,output_tokens=?,cache_read_tokens=?,cache_write_tokens=?,cost_micro_usd=?,disputed=?,over_reserved=?,state_version=state_version+1,chain_sha256=? WHERE attempt_id=? AND state_version=?",
        {after.at("input_tokens"), after.at("output_tokens"), after.at("cache_read_tokens"), after.at("cache_write_tokens"),
         after.at("cost_micro_usd"), after.at("disputed"), after.at("over_reserved"), after.at("chain_sha256"),
         statement.attempt_id, reconciliation.expected_projection_version});
    const Row answer{{"record_sha256", fingerprint}, {"command", command},
        {"status", updated.conflicts.empty() ? "APPLIED" : "CONFLICT"}, {"conflicting_dimensions", updated.conflicts},
        {"original_usage", projection_usage(original)}, {"before", before}, {"after", after},
        {"budget_delta", updated.delta}, {"business_authority_restored", false}};
    impl_->execute("INSERT INTO usage_reconciliations VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        {reconciliation.reconciliation_id, statement.attempt_id, reconciliation.idempotency_key,
         reconciliation.expected_projection_version, fingerprint, evidence.sha256(), evidence.source_kind(),
         evidence.source_reference(), live_hex(evidence.bytes()), canonical_json(command.at("statement")), json_text(answer), stamp()});
    impl_->event(root.request_id, "usage:" + reconciliation.reconciliation_id, command, answer, "root");
    tx.commit(); return answer;
}

Json Store::replay_usage_reconciliation(const StoreActor& root, const VerifiedUsageEvidence& evidence,
                                       const UsageReconciliation& reconciliation) const {
    (void)validate_usage_reconciliation_input(evidence, reconciliation);
    const auto& statement = evidence.statement();
    (void)model_usage_from_json(to_json(statement.usage));
    const std::lock_guard lock(impl_->mutex);
    require(impl_->reconciliation_schema, ErrorCode::Unsupported, "usage replay requires the 410 migration");
    (void)impl_->authorize(root, {Role::Root});
    impl_->check_request_scope(root.request_id);
    const auto original = impl_->one("SELECT request_id,settlement_sha256 FROM call_attempts WHERE attempt_id=?", {statement.attempt_id});
    require(str(original, "request_id") == root.request_id, ErrorCode::Unauthorized, "usage replay belongs to another request");
    require(!original.at("settlement_sha256").is_null() && str(original, "settlement_sha256") == statement.settlement_sha256,
            ErrorCode::Conflict, "usage replay changed the original settlement identity");
    const auto previous = impl_->optional("SELECT * FROM usage_reconciliations WHERE reconciliation_id=? OR (attempt_id=? AND idempotency_key=?)",
        {reconciliation.reconciliation_id, statement.attempt_id, reconciliation.idempotency_key});
    if (!previous) return nullptr;
    require(str(*previous, "reconciliation_id") == reconciliation.reconciliation_id &&
            str(*previous, "attempt_id") == statement.attempt_id &&
            str(*previous, "source_kind") == evidence.source_kind() &&
            str(*previous, "source_reference") == evidence.source_reference() &&
            str(*previous, "evidence_sha256") == evidence.sha256(), ErrorCode::Conflict,
            "usage replay identity or evidence differs from the original record");
    const auto result = parse_json_strict(str(*previous, "result"));
    const auto& recorded = result.at("command").as_object();
    const auto& actor = recorded.at("actor").as_object();
    const auto current = actor_json(root);
    for (const auto* key : {"role", "principal_id", "request_id", "task_id", "team_id", "execution_epoch"})
        require(actor.at(key) == current.at(key), ErrorCode::Unauthorized, "usage replay changed the original Root identity");
    StoreActor original_actor = root;
    original_actor.dispatcher_epoch = number(actor, "dispatcher_epoch");
    require(original_actor.dispatcher_epoch > 0, ErrorCode::Corrupt, "usage replay original Root epoch is invalid");
    const auto command = live_reconciliation_command(original_actor, evidence, reconciliation);
    const auto fingerprint = sha256_hex(canonical_json(command));
    require(command == recorded && fingerprint == str(*previous, "record_sha256") &&
            str(result.as_object(), "record_sha256") == fingerprint &&
            str(*previous, "statement") == canonical_json(command.at("statement")),
            ErrorCode::Conflict, "usage replay differs from the complete immutable command");
    return result;
}

Json Store::live_accounting_status(const std::string& request_id) const {
    const std::lock_guard lock(impl_->mutex);
    require(impl_->reconciliation_schema, ErrorCode::Unsupported,
            "live accounting status requires the 410 migration");
    (void)impl_->request_row(request_id);
    const std::string predicate = " FROM call_attempts c JOIN live_attempt_execution l USING(attempt_id) "
        "LEFT JOIN live_usage_projection p USING(attempt_id) WHERE c.request_id=? AND "
        "(c.settlement_sha256 IS NULL OR p.attempt_id IS NULL OR p.disputed=1 OR p.over_reserved=1 OR "
        "p.input_tokens IS NULL OR p.output_tokens IS NULL OR p.cache_read_tokens IS NULL OR "
        "p.cache_write_tokens IS NULL OR p.cost_micro_usd IS NULL)";
    // Count, bounded details and budget are observed by one SQLite statement,
    // including on a read-only connection alongside another process's writer.
    const auto rows = impl_->all("WITH blocked AS ("
        "SELECT c.attempt_id,c.status,(c.settlement_sha256 IS NULL) AS unsettled,"
        "(p.attempt_id IS NULL) AS missing_projection,COALESCE(p.disputed,0) AS disputed,"
        "COALESCE(p.over_reserved,0) AS over_reserved,(p.input_tokens IS NULL) AS unknown_input,"
        "(p.output_tokens IS NULL) AS unknown_output,(p.cache_read_tokens IS NULL) AS unknown_cache_read,"
        "(p.cache_write_tokens IS NULL) AS unknown_cache_write,(p.cost_micro_usd IS NULL) AS unknown_cost" +
        predicate + "), summary AS (SELECT COUNT(*) AS blocked_count FROM blocked) "
        "SELECT summary.blocked_count,b.spent_input,b.reserved_input,b.input_limit,"
        "b.spent_output,b.reserved_output,b.output_limit,b.spent_cost,b.reserved_cost,b.cost_limit,blocked.* "
        "FROM request_budgets b CROSS JOIN summary LEFT JOIN blocked ON 1 "
        "WHERE b.request_id=? ORDER BY blocked.attempt_id LIMIT 64", {request_id, request_id});
    require(!rows.empty(), ErrorCode::Corrupt, "request has no accounting budget");
    const auto& budget = rows.front();
    const auto count = number(budget, "blocked_count");
    Array blocked;
    for (const auto& item : rows) {
        if (item.at("attempt_id").is_null()) continue;
        Row safe;
        for (const auto* field : {"attempt_id", "status", "unsettled", "missing_projection", "disputed", "over_reserved",
                "unknown_input", "unknown_output", "unknown_cache_read", "unknown_cache_write", "unknown_cost"})
            safe[field] = item.at(field);
        blocked.push_back(std::move(safe));
    }
    bool within_limits = true;
    for (const auto& [dimension, unused] : ledger_dimensions) {
        (void)unused;
        if (add(number(budget, "spent_" + std::string(dimension)),
                number(budget, "reserved_" + std::string(dimension))) >
                number(budget, std::string(dimension) + "_limit")) within_limits = false;
    }
    return Row{{"request_id", request_id}, {"ready", count == 0 && within_limits},
        {"blocked_count", count}, {"blocked_attempts", std::move(blocked)},
        {"truncated", count > 64}, {"budget_within_limits", within_limits},
        {"reservation_authority", false}};
}

Json Store::verify_usage_ledger(const std::string& request_id) const {
    const std::lock_guard lock(impl_->mutex);
    require(impl_->reconciliation_schema, ErrorCode::Unsupported, "ledger reconstruction requires the 410 migration");
    (void)impl_->request_row(request_id);
    const auto actual = impl_->one("SELECT * FROM request_budgets WHERE request_id=?", {request_id});
    Row rebuilt{{"attempts", 0}, {"reserved_input", 0}, {"reserved_output", 0}, {"reserved_cost", 0},
        {"spent_input", 0}, {"spent_output", 0}, {"spent_cost", 0},
        {"unknown_input", 0}, {"unknown_output", 0}, {"unknown_cost", 0}};
    Array attempts;
    for (const auto& original : impl_->all("SELECT * FROM call_attempts WHERE request_id=? ORDER BY attempt_id", {request_id})) {
        rebuilt["attempts"] = next(number(rebuilt, "attempts"));
        const auto attempt_id = str(original, "attempt_id");
        const bool live = impl_->optional("SELECT attempt_id FROM live_attempt_execution WHERE attempt_id=?", {attempt_id}).has_value();
        const bool settled = !original.at("settlement_sha256").is_null();
        auto values = projection_usage(original);
        std::optional<Row> projection;
        if (live && settled) {
            auto expected = initial_projection(original);
            for (const auto& record : impl_->all("SELECT * FROM usage_reconciliations WHERE attempt_id=? ORDER BY expected_projection_version", {attempt_id})) {
                const auto result = parse_json_strict(str(record, "result"));
                const auto& command = result.at("command");
                const auto& proposed_statement = command.at("statement");
                require(number(record, "expected_projection_version") == number(expected, "state_version") &&
                        result.at("before") == expected && result.at("original_usage") == projection_usage(original) &&
                        sha256_hex(canonical_json(command)) == str(record, "record_sha256") &&
                        result.at("record_sha256") == record.at("record_sha256") &&
                        command.at("reconciliation_id") == record.at("reconciliation_id") &&
                        command.at("idempotency_key") == record.at("idempotency_key") &&
                        command.at("expected_projection_version") == record.at("expected_projection_version") &&
                        command.at("source_kind") == record.at("source_kind") &&
                        command.at("evidence_sha256") == record.at("evidence_sha256") &&
                        command.at("source_reference_sha256").as_string() == sha256_hex(str(record, "source_reference")) &&
                        sha256_hex(live_unhex(str(record, "source_bytes_hex"))) == str(record, "evidence_sha256") &&
                        canonical_json(proposed_statement) == str(record, "statement") &&
                        proposed_statement.at("attempt_id") == original.at("attempt_id") &&
                        proposed_statement.at("settlement_sha256") == original.at("settlement_sha256"),
                        ErrorCode::Corrupt, "usage reconciliation evidence or chain mismatch");
                const auto proposed = to_json(model_usage_from_json(proposed_statement.at("usage"))).as_object();
                if (str(record, "source_kind") == "host_no_dispatch") {
                    const auto observation = impl_->optional("SELECT reason FROM live_unobserved_settlements WHERE attempt_id=?", {attempt_id});
                    require(observation && str(*observation, "reason") == "not_dispatched_after_recovery" &&
                            !impl_->optional("SELECT attempt_id FROM live_send_claims WHERE attempt_id=?", {attempt_id}),
                            ErrorCode::Corrupt, "replayed no-dispatch evidence has no matching host observation");
                    for (const auto field : usage_fields)
                        require(!proposed.at(field).is_null() && number(proposed, field) == 0, ErrorCode::Corrupt,
                                "replayed no-dispatch evidence is not complete zero usage");
                }
                const auto reconstructed = reconcile_projection(original, expected, proposed, str(record, "record_sha256"));
                require(result.at("after") == reconstructed.after && result.at("budget_delta") == reconstructed.delta &&
                        result.at("conflicting_dimensions") == reconstructed.conflicts &&
                        result.at("status") == (reconstructed.conflicts.empty() ? "APPLIED" : "CONFLICT"),
                        ErrorCode::Corrupt, "usage reconciliation projection or dimension delta mismatch");
                expected = reconstructed.after;
            }
            projection = impl_->one("SELECT * FROM live_usage_projection WHERE attempt_id=?", {attempt_id});
            require(*projection == expected, ErrorCode::Corrupt, "current usage projection differs from original evidence replay");
            values = projection_usage(expected);
        } else if (live) require(!impl_->optional("SELECT attempt_id FROM live_usage_projection WHERE attempt_id=?", {attempt_id}),
                                 ErrorCode::Corrupt, "unsettled attempt has an invented usage projection");
        for (const auto& [dimension, field] : ledger_dimensions) {
            const auto suffix = std::string(dimension);
            if (settled && !values.at(field).is_null()) rebuilt["spent_" + suffix] = add(number(rebuilt, "spent_" + suffix), number(values, field));
            else {
                const auto reservation = number(original, "reserved_" + suffix);
                rebuilt["reserved_" + suffix] = add(number(rebuilt, "reserved_" + suffix), reservation);
                if (settled || number(original, "unknown_" + suffix) != 0)
                    rebuilt["unknown_" + suffix] = add(number(rebuilt, "unknown_" + suffix), reservation);
            }
        }
        attempts.emplace_back(Row{{"attempt_id", attempt_id}, {"original_settlement_sha256", original.at("settlement_sha256")},
            {"execution_kind", live ? "live" : "synthetic_fake"}, {"current_usage", values},
            {"projection_version", projection ? projection->at("state_version") : Json(nullptr)}});
    }
    for (const auto& field : rebuilt)
        require(field.value() == actual.at(field.key()), ErrorCode::Corrupt, "request budget differs from original and reconciled usage");
    return Row{{"request_id", request_id}, {"consistent", true}, {"reconstructed_budget", rebuilt},
        {"durable_budget", actual}, {"attempts", attempts}, {"unsettled_accounting", impl_->has_unsettled_model(request_id)}};
}
