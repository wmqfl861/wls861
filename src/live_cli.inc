// Trusted local-owner CLI controls. The authorization document is an explicit
// host instruction, not a model tool, an authentication service or a provider
// billing attestation. No credential value is accepted in this document.
struct LiveCliAuthorization {
    std::string request_id;
    std::string config_snapshot_sha256;
    std::string execution_kind;
    std::map<std::string, std::string> models;
    LiveWorkflowOptions workflow;
};

bool live_cli_digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

LiveCliAuthorization live_cli_authorization(const Json& value) {
    static const std::set<std::string> fields{"schema_version", "kind", "request_id",
        "config_snapshot_sha256", "execution_kind", "network_enabled", "provider_enabled", "models",
        "max_calls", "max_worker_calls_per_task", "max_batch_duration_ms", "reserved_cost_micro_usd"};
    if (!value.is_object() || value.as_object().size() != fields.size())
        throw Error(ErrorCode::InvalidInput, "live authorization requires the complete v1 host envelope");
    for (const auto& item : value.as_object())
        if (!fields.contains(std::string(item.key())))
            throw Error(ErrorCode::InvalidInput, "live authorization contains an unsupported field");
    if (integer(value.at("schema_version")) != 1 ||
            text(value.at("kind")) != "wls-live-workflow-authorization-v1")
        throw Error(ErrorCode::Unsupported, "unsupported live authorization version");
    LiveCliAuthorization result;
    result.request_id = text(value.at("request_id"));
    result.config_snapshot_sha256 = text(value.at("config_snapshot_sha256"));
    result.execution_kind = text(value.at("execution_kind"));
    if (!is_typed_id(result.request_id, "req") || !live_cli_digest(result.config_snapshot_sha256) ||
            (result.execution_kind != "local_tls" && result.execution_kind != "provider"))
        throw Error(ErrorCode::InvalidInput, "live authorization has an invalid request, snapshot or execution kind");
    if (!value.at("network_enabled").is_bool() || !value.at("provider_enabled").is_bool())
        throw Error(ErrorCode::InvalidInput, "live authorization switches must be booleans");
    result.workflow.authorization.network_enabled = value.at("network_enabled").as_bool();
    result.workflow.authorization.provider_enabled = value.at("provider_enabled").as_bool();
    if (!result.workflow.authorization.network_enabled ||
            (result.execution_kind == "provider" && !result.workflow.authorization.provider_enabled))
        throw Error(ErrorCode::Unauthorized, "live execution requires explicit host network and provider authorization");
    if (result.execution_kind == "local_tls" && result.workflow.authorization.provider_enabled)
        throw Error(ErrorCode::InvalidInput, "local TLS authorization must not grant provider execution");
    const auto& models = value.at("models");
    if (!models.is_object() || models.as_object().empty() || models.as_object().size() > 32)
        throw Error(ErrorCode::InvalidInput, "live authorization requires a bounded model profile allowlist");
    for (const auto& item : models.as_object()) {
        const std::string name(item.key());
        const auto digest = text(item.value());
        if (name.empty() || name.size() > 128 || name.find('\0') != std::string::npos || !live_cli_digest(digest))
            throw Error(ErrorCode::InvalidInput, "invalid authorized model profile identity");
        result.models.emplace(name, digest);
    }
    result.workflow.limits.max_calls = integer(value.at("max_calls"));
    result.workflow.limits.max_worker_calls_per_task = integer(value.at("max_worker_calls_per_task"));
    const auto duration = integer(value.at("max_batch_duration_ms"));
    result.workflow.reserved_cost_micro_usd = integer(value.at("reserved_cost_micro_usd"));
    if (result.workflow.limits.max_calls < 1 || result.workflow.limits.max_calls > 64 ||
            result.workflow.limits.max_worker_calls_per_task < 1 ||
            result.workflow.limits.max_worker_calls_per_task > 16 || duration < 1 || duration > 3600000 ||
            result.workflow.reserved_cost_micro_usd <= 0)
        throw Error(ErrorCode::InvalidInput, "live authorization limits are outside the bounded execution envelope");
    result.workflow.max_batch_duration = std::chrono::milliseconds(duration);
    result.workflow.credentials = [](std::string_view reference) {
        // Invoked by the runtime only after complete host/config/budget checks.
        // A missing or invalid value is described without echoing any bytes.
        const std::string name(reference);
        const char* raw = std::getenv(name.c_str());
        if (!raw) throw Error(ErrorCode::Unauthorized, "the authorized frozen credential reference is unavailable");
        const auto size = ::strnlen(raw, 8193);
        if (size == 0 || size > 8192 || !std::all_of(raw, raw + size, [](char c) {
                const auto byte = static_cast<unsigned char>(c); return byte >= 33U && byte <= 126U;
            }))
            throw Error(ErrorCode::InvalidInput, "the authorized frozen credential has an invalid bounded format");
        return std::string(raw, size);
    };
    return result;
}

void validate_live_cli_authorization(Store& store, const std::string& request_id,
                                     const LiveCliAuthorization& authorization) {
    if (authorization.request_id != request_id)
        throw Error(ErrorCode::Unauthorized, "live authorization belongs to another request");
    const auto frozen = restore_request_config(store.request(request_id));
    if (frozen.sha256() != authorization.config_snapshot_sha256)
        throw Error(ErrorCode::Conflict, "live authorization differs from the request's original frozen configuration");
    std::set<std::string> selected;
    for (const auto role : {Role::Dispatcher, Role::TeamLead, Role::Worker}) {
        const auto model = frozen.resolve(role).model;
        const auto profile = frozen.model_profile(role);
        const auto found = authorization.models.find(model);
        if (found == authorization.models.end() || found->second != sha256_hex(canonical_json(profile)))
            throw Error(ErrorCode::Unauthorized, "a frozen role's model profile is outside the explicit authorization");
        const auto settings = live_model_settings_from_json(profile, model);
        if (settings.execution_kind != authorization.execution_kind)
            throw Error(ErrorCode::Unauthorized, "frozen execution kind differs from the explicit authorization");
        selected.insert(model);
    }
    if (selected.size() != authorization.models.size())
        throw Error(ErrorCode::InvalidInput, "live authorization must name exactly the frozen workflow model profiles");
    validate_live_workflow_config(frozen, authorization.workflow);
    const auto budget = store.budget(request_id);
    const auto cost_limit = integer(budget.at("cost_limit"));
    const auto spent = integer(budget.at("spent_cost"));
    const auto reserved = integer(budget.at("reserved_cost"));
    if (cost_limit < 0 || spent < 0 || reserved < 0)
        throw Error(ErrorCode::Corrupt, "the original request budget contains invalid negative balances");
    if (authorization.workflow.reserved_cost_micro_usd > cost_limit)
        throw Error(ErrorCode::Quota, "the authorized per-attempt reservation exceeds the original request cost limit");
    // Existing pending/unknown work is not a reason to bypass scoped takeover.
    // The live pump reports the accounting stop after recovery, and checks all
    // remaining balances again before resolving a credential or reserving.
}

Json live_cli_status(Store& store, const std::string& request_id) {
    const auto frozen = restore_request_config(store.request(request_id));
    Object models;
    Array roles;
    std::string kind;
    for (const auto role : {Role::Dispatcher, Role::TeamLead, Role::Worker}) {
        const auto model = frozen.resolve(role).model;
        const auto profile = frozen.model_profile(role);
        const auto settings = live_model_settings_from_json(profile, model);
        if (!kind.empty() && kind != settings.execution_kind)
            throw Error(ErrorCode::Unsupported, "one live batch cannot mix provider and local TLS execution kinds");
        kind = settings.execution_kind;
        const auto digest = sha256_hex(canonical_json(profile));
        models[model] = digest;
        roles.push_back(Object{{"role", role_name(role)}, {"model_ref", model}, {"profile_sha256", digest},
            {"execution_kind", settings.execution_kind}});
    }
    return Object{{"request_id", request_id}, {"config_snapshot_sha256", frozen.sha256()},
        {"roles", std::move(roles)}, {"accounting", store.live_accounting_status(request_id)},
        {"budget", store.budget(request_id)}, {"credentials_resolved", false},
        {"authorization_template", Object{{"schema_version", 1},
            {"kind", "wls-live-workflow-authorization-v1"}, {"request_id", request_id},
            {"config_snapshot_sha256", frozen.sha256()}, {"execution_kind", kind},
            {"network_enabled", false}, {"provider_enabled", false}, {"models", std::move(models)},
            {"max_calls", 1}, {"max_worker_calls_per_task", 1}, {"max_batch_duration_ms", 30000},
            {"reserved_cost_micro_usd", 0}}}};
}
