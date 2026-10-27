// Local-owner controls for an explicitly trusted billing adapter. Captured
// policy bytes are separate from network authorization and model credentials.
struct BillingCliPolicy {
    BillingTrustPolicy policy;
    std::filesystem::path path;
    std::string bytes_sha256;
};

std::string billing_cli_control_bytes(Store& store, const std::filesystem::path& path,
        std::size_t limit = 65536) {
    store.validate_control_file_path(std::filesystem::absolute(path).lexically_normal());
    return read_runtime_file(path, limit);
}

std::optional<BillingCliPolicy> billing_cli_policy(Store& store, const std::string& path) {
    if (path.empty()) return std::nullopt;
    const auto bytes = billing_cli_control_bytes(store, path);
    return BillingCliPolicy{billing_trust_policy_from_json(parse_json_strict(bytes, 65536)),
        std::filesystem::absolute(path).lexically_normal(), sha256_hex(bytes)};
}

void validate_cli_model_topology(Store& store, const std::string& request_id, bool single_agent) {
    const auto frozen = restore_request_config(store.request(request_id));
    if (single_agent) {
        validate_single_agent_config(frozen);
    } else if (!frozen.logical_agent_policy().is_null()) {
        throw Error(ErrorCode::Unsupported,
            "this frozen logical Agent requires the explicit pump-single entry point");
    }
}

void validate_billing_cli_policy(Store& store, const std::string& request_id,
        const BillingCliPolicy& captured) {
    store.validate_control_file_path(captured.path);
    validate_billing_trust_policy(store, request_id, captured.policy);
}

void validate_billing_cli_authorization(Store& store, const std::string& request_id,
        const LiveCliAuthorization& authorization, const std::optional<BillingCliPolicy>& captured) {
    if (!captured) {
        if (!authorization.billing_policy_sha256.empty())
            throw Error(ErrorCode::InvalidInput, "billing authorization requires its explicit trust-policy file");
        return;
    }
    if (authorization.billing_policy_sha256 != captured->bytes_sha256)
        throw Error(ErrorCode::Unauthorized,
            "live authorization does not bind the captured billing-policy bytes");
    validate_billing_cli_policy(store, request_id, *captured);
}

Json billing_cli_status(Store& store, const std::string& request_id,
        const std::optional<BillingCliPolicy>& captured) {
    auto result = live_cli_status(store, request_id);
    const auto frozen = restore_request_config(store.request(request_id));
    const bool single_agent = !frozen.logical_agent_policy().is_null();
    if (single_agent) validate_single_agent_config(frozen);
    result.as_object()["execution_mode"] = single_agent ? "single_agent" : "role_isolated";
    result.as_object()["pump_command"] = single_agent ? "pump-single-live" : "pump-live";
    if (captured) {
        validate_billing_cli_policy(store, request_id, *captured);
        auto& authorization = result.at("authorization_template").as_object();
        authorization["schema_version"] = 2;
        authorization["kind"] = "wls-live-workflow-authorization-v2";
        authorization["billing_policy_sha256"] = captured->bytes_sha256;
        result.as_object()["billing_policy_sha256"] = captured->bytes_sha256;
        result.as_object()["billing_policy_canonical_sha256"] = captured->policy.sha256();
    }
    return result;
}

Json billing_cli_result(const std::string& request_id, const std::string& attempt_id,
        const BillingCliPolicy& captured, const std::string& receipt_sha256,
        const Json& reconciliation, bool replayed) {
    return Object{{"mode", "billing_reconciliation"}, {"request_id", request_id},
        {"attempt_id", attempt_id}, {"billing_policy_sha256", captured.bytes_sha256},
        {"billing_policy_canonical_sha256", captured.policy.sha256()},
        {"receipt_sha256", receipt_sha256}, {"replayed", replayed},
        {"result", reconciliation}, {"business_authority_restored", false},
        {"credentials_resolved", false}, {"model_calls", 0}, {"network_calls", 0}};
}
