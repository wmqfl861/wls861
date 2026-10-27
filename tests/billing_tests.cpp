#include "wls/billing.hpp"
#include "live_test_support.hpp"

#include <openssl/evp.h>

#include <array>
#include <memory>

using namespace wls::s1;
using wls::Role;
namespace fixture = wls::test::live;
using fixture::Object;
using fixture::text;
using fixture::number;
using fixture::error;

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
std::unique_ptr<fixture::Assets> assets;

std::string hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    for (const auto c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        value.push_back(digits[byte >> 4U]); value.push_back(digits[byte & 15U]);
    }
    return value;
}
struct Issuer {
    // Synthetic test-only seed. This is not a provider key, fixture deployment
    // key or production credential, and is never included in installed assets.
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
    std::string public_key;
    explicit Issuer(unsigned char offset = 0) {
        std::array<unsigned char, 32> seed{};
        for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<unsigned char>(i + offset);
        key.reset(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
        CHECK(key != nullptr);
        std::array<unsigned char, 32> public_bytes{};
        std::size_t size = public_bytes.size();
        CHECK(EVP_PKEY_get_raw_public_key(key.get(), public_bytes.data(), &size) == 1 && size == public_bytes.size());
        public_key = hex(std::string_view(reinterpret_cast<const char*>(public_bytes.data()), public_bytes.size()));
    }
    std::string sign(const Json& payload) const {
        // Independent issuer implementation of the published signing frame;
        // it intentionally does not call the production formatting helper.
        const auto message = std::string("wls-signed-billing-receipt-v1\n") + canonical_json(payload);
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        CHECK(context && EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1);
        std::array<unsigned char, 64> signature{};
        std::size_t length = signature.size();
        CHECK(EVP_DigestSign(context.get(), signature.data(), &length,
            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1 && length == signature.size());
        return canonical_json(Object{{"schema_version", 1}, {"kind", "wls-signed-billing-receipt-v1"},
            {"payload", payload}, {"signature_hex", hex(std::string_view(reinterpret_cast<const char*>(signature.data()), length))}});
    }
};

LiveModelSettings settings() {
    auto live = fixture::settings(); live.provider.billing_challenge = "wls-billing-challenge-v1"; return live;
}
Json policy_json(const fixture::Fixture& f, const Issuer& issuer) {
    const auto row = f.store->request(f.request_id);
    return Object{{"schema_version", 1}, {"kind", "wls-billing-trust-policy-v1"},
        {"trust_domain", f.live.execution_kind == "local_tls" ? "local_tls_fixture" : "provider_billing"},
        {"request_id", f.request_id}, {"config_snapshot_sha256", row.at("config_snapshot_sha256")},
        {"issuer", "synthetic-issuer"}, {"account_id", "synthetic-account"},
        {"public_keys", Object{{"key-one", issuer.public_key}}},
        {"models", Object{{f.live.provider.model_ref, sha256_hex(canonical_json(fixture::model_profile(f.live)))}}}};
}
Json receipt_payload(const fixture::Fixture& f, const ModelRequest& request, std::string id = "receipt-one",
                     ModelUsage usage = fixture::complete_usage()) {
    const auto encoded = encode_responses_request(f.live.provider, request, f.live.stream);
    return Object{{"trust_domain", f.live.execution_kind == "local_tls" ? "local_tls_fixture" : "provider_billing"},
        {"issuer", "synthetic-issuer"}, {"account_id", "synthetic-account"}, {"key_id", "key-one"},
        {"receipt_id", std::move(id)}, {"revision", 1}, {"billing_challenge", encoded.billing_challenge},
        {"request_body_sha256", encoded.body_sha256}, {"response_body_sha256", sha256_hex("synthetic unobserved issuer body")},
        {"provider_response_id", "resp-synthetic"}, {"provider_model", f.live.provider.model},
        {"token_unit", "tokens"}, {"cost_unit", "micro_usd"}, {"usage", to_json(usage)}};
}
Json recover(fixture::Fixture& f, const ModelRequest& request, bool claim = true) {
    (void)f.reserve(request);
    if (claim) (void)f.claim(request);
    f.reopen();
    return f.store->settle_unobserved_live(f.actor(Role::Root), request.attempt_id,
        number(f.store->attempt(request.attempt_id), "state_version"),
        claim ? "unobserved_after_recovery" : "not_dispatched_after_recovery", "recover-" + request.attempt_id);
}
UsageReconciliation command(const VerifiedUsageEvidence& evidence, std::int64_t version = 1) {
    return {evidence.required_reconciliation_id(), "signed:" + evidence.sha256(), version};
}

void legacy_profile_and_unique_challenge() {
    const auto legacy = fixture::settings();
    const auto legacy_json = to_json(legacy.provider);
    CHECK(!legacy_json.as_object().contains("billing_challenge"));
    CHECK(to_json(provider_profile_from_json(legacy_json)) == legacy_json);
    const auto& p = legacy.provider;
    const ProviderProfile old_aggregate{p.profile_id, p.adapter_version, p.protocol, p.base_url,
        p.responses_path, p.model_ref, p.model, p.api_key_env, p.streaming, p.tool_calling,
        p.parallel_tool_calls, p.reasoning_provider_default, p.reasoning_off, p.reasoning_efforts,
        p.context_window, p.max_output_tokens, p.limits};
    CHECK(to_json(old_aggregate) == legacy_json);
    fixture::Fixture f(*assets, settings());
    const auto first = f.request("attempt-first");
    auto second = first; second.call_id = "call-second"; second.attempt_id = "attempt-second";
    const auto one = encode_responses_request(f.live.provider, first);
    const auto two = encode_responses_request(f.live.provider, second);
    CHECK(one.body == two.body); CHECK(one.billing_challenge != two.billing_challenge);
    CHECK(one.billing_challenge.size() == 64);
    CHECK(to_json(one).at("format") == "wls-encoded-responses-request-v2-billing");
    auto old_profile = f.live.provider; old_profile.billing_challenge.clear();
    const auto old = encode_responses_request(old_profile, first);
    const EncodedResponsesRequest old_encoded_aggregate{old.method, old.url, old.body, old.body_sha256,
        old.profile_sha256, old.logical_request_sha256, old.stream, old.evidence, old.record_sha256};
    CHECK(to_json(old_encoded_aggregate) == to_json(old));
    CHECK(old.billing_challenge.empty()); CHECK(!to_json(old).as_object().contains("billing_challenge"));
    CHECK(to_json(old).at("format") == "wls-encoded-responses-request-v1"); CHECK(old.body == one.body);
    auto tampered = one; tampered.billing_challenge = two.billing_challenge;
    error(ErrorCode::Corrupt, [&] { (void)to_json(tampered); });
}

void bounded_exact_policy_and_frozen_profiles() {
    fixture::Fixture f(*assets, settings()); Issuer issuer;
    const auto original = policy_json(f, issuer);
    validate_billing_trust_policy(*f.store, f.request_id, billing_trust_policy_from_json(original));
    for (const auto* key : {"kind", "issuer", "public_keys", "models"}) {
        auto changed = original; changed.as_object().erase(key);
        error(ErrorCode::InvalidInput, [&] { (void)billing_trust_policy_from_json(changed); });
    }
    auto changed = original; changed.as_object()["verified"] = true;
    error(ErrorCode::InvalidInput, [&] { (void)billing_trust_policy_from_json(changed); });
    changed = original; changed.at("public_keys").at("key-one") = std::string(63, '0');
    error(ErrorCode::InvalidInput, [&] { (void)billing_trust_policy_from_json(changed); });
    changed = original; changed.at("models").at(f.live.provider.model_ref) = sha256_hex("other profile");
    error(ErrorCode::Unauthorized, [&] { validate_billing_trust_policy(*f.store, f.request_id, billing_trust_policy_from_json(changed)); });
    changed = original; changed.at("config_snapshot_sha256") = sha256_hex("other config");
    error(ErrorCode::Conflict, [&] { validate_billing_trust_policy(*f.store, f.request_id, billing_trust_policy_from_json(changed)); });
    changed = original; changed.at("models").as_object()["unused"] = sha256_hex("unused");
    error(ErrorCode::InvalidInput, [&] { validate_billing_trust_policy(*f.store, f.request_id, billing_trust_policy_from_json(changed)); });
    fixture::Fixture old(*assets);
    error(ErrorCode::Unsupported, [&] { validate_billing_trust_policy(*old.store, old.request_id, billing_trust_policy_from_json(policy_json(old, issuer))); });
}

void signed_late_receipt_preserves_original_missing_response() {
    fixture::Fixture f(*assets, settings()); Issuer issuer;
    const auto request = f.request(); const auto original = recover(f, request);
    const auto policy = billing_trust_policy_from_json(policy_json(f, issuer));
    const auto bytes = issuer.sign(receipt_payload(f, request));
    const auto proof = verify_signed_billing_receipt(*f.store, policy, request.attempt_id, bytes);
    CHECK(proof.source_kind() == "loopback_fixture"); CHECK(proof.required_reconciliation_id().starts_with("billing:"));
    CHECK(proof.statement().attempt_id == request.attempt_id);
    CHECK(proof.statement().settlement_sha256 == text(original, "settlement_sha256"));
    const auto row = f.store->reconcile_usage(f.actor(Role::Root), proof, command(proof));
    CHECK(row.at("status") == "APPLIED"); CHECK(!row.at("business_authority_restored").as_bool());
    CHECK(f.store->attempt(request.attempt_id) == original);
    CHECK(f.store->live_attempt(request.attempt_id).at("transport").is_null());
    CHECK(f.store->live_accounting_status(f.request_id).at("ready").as_bool());
    CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
    CHECK(fixture::sql_text(f.path, "SELECT count(*) FROM model_tool_feedback") == "0");
    const auto stored = parse_json_strict(proof.bytes());
    CHECK(stored.at("original_mapping").at("observation").at("status") == "unobserved");
    CHECK(stored.at("receipt_original_bytes_hex").as_string() == hex(bytes));
}

void forged_signature_key_and_payload_are_rejected() {
    fixture::Fixture f(*assets, settings()); Issuer issuer, other(32);
    const auto request = f.request(); (void)recover(f, request);
    const auto policy = billing_trust_policy_from_json(policy_json(f, issuer));
    const auto payload = receipt_payload(f, request);
    error(ErrorCode::Unauthorized, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, other.sign(payload)); });
    auto receipt = parse_json_strict(issuer.sign(payload)); receipt.at("payload").at("usage").at("cost_micro_usd") = 0;
    error(ErrorCode::Unauthorized, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, canonical_json(receipt)); });
    receipt = parse_json_strict(issuer.sign(payload)); receipt.at("signature_hex") = std::string(128, '0');
    error(ErrorCode::Unauthorized, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, canonical_json(receipt)); });
    auto changed = payload; changed.at("key_id") = "untrusted-key";
    error(ErrorCode::Unauthorized, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(changed)); });
    CHECK(fixture::sql_text(f.path, "SELECT count(*) FROM usage_reconciliations") == "0");
}

void issuer_domain_units_and_shape_are_exact() {
    fixture::Fixture f(*assets, settings()); Issuer issuer;
    const auto request = f.request(); (void)recover(f, request);
    const auto policy = billing_trust_policy_from_json(policy_json(f, issuer));
    const auto payload = receipt_payload(f, request);
    for (const auto* key : {"issuer", "account_id"}) {
        auto altered = payload; altered.at(key) = "different";
        error(ErrorCode::Unauthorized, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(altered)); });
    }
    auto altered = payload; altered.at("trust_domain") = "provider_billing";
    error(ErrorCode::Unauthorized, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(altered)); });
    for (const auto* key : {"token_unit", "cost_unit"}) {
        altered = payload; altered.at(key) = "estimated";
        error(ErrorCode::InvalidInput, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(altered)); });
    }
    altered = payload; altered.at("usage").at("cost_micro_usd") = -1;
    error(ErrorCode::InvalidInput, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(altered)); });
    altered = payload; altered.at("usage").as_object().erase("cache_write_tokens");
    error(ErrorCode::InvalidInput, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(altered)); });
    altered = payload; altered.as_object()["verified"] = true;
    error(ErrorCode::InvalidInput, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(altered)); });
}

void unique_challenge_and_wire_identity_are_mandatory() {
    fixture::Fixture f(*assets, settings()); Issuer issuer;
    const auto request = f.request(); (void)recover(f, request);
    const auto policy = billing_trust_policy_from_json(policy_json(f, issuer));
    const auto payload = receipt_payload(f, request);
    for (const auto* key : {"billing_challenge", "request_body_sha256"}) {
        auto altered = payload; altered.at(key) = sha256_hex("another original request");
        error(ErrorCode::Conflict, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(altered)); });
    }
    auto altered = payload; altered.at("provider_model") = "another-model";
    error(ErrorCode::Conflict, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(altered)); });
}

void source_identity_survives_key_rotation_and_recovery() {
    fixture::Fixture f(*assets, settings()); Issuer issuer, rotated(32);
    const auto first = f.request("attempt-first"); (void)recover(f, first);
    auto definition = policy_json(f, issuer); definition.at("public_keys").as_object()["key-two"] = rotated.public_key;
    const auto policy = billing_trust_policy_from_json(definition);
    const auto initial = verify_signed_billing_receipt(*f.store, policy, first.attempt_id, issuer.sign(receipt_payload(f, first)));
    (void)f.store->reconcile_usage(f.actor(Role::Root), initial, command(initial));
    const auto second = f.request("attempt-second"); (void)recover(f, second);
    auto newer = receipt_payload(f, second); newer.at("key_id") = "key-two"; newer.at("revision") = 2;
    const auto proof = verify_signed_billing_receipt(*f.store, policy, second.attempt_id, rotated.sign(newer));
    CHECK(proof.source_reference() == initial.source_reference());
    error(ErrorCode::Conflict, [&] { (void)f.store->reconcile_usage(f.actor(Role::Root), proof, command(proof)); });
    CHECK(fixture::sql_text(f.path, "SELECT count(*) FROM usage_reconciliations") == "1");
    CHECK(f.store->usage_projection(second.attempt_id).at("projection").at("cost_micro_usd").is_null());
}

void source_identity_is_global_across_requests() {
    fixture::Fixture f(*assets, settings()); Issuer issuer;
    const auto first_id = f.request_id; const auto first = f.request("attempt-first"); (void)recover(f, first);
    const auto policy = billing_trust_policy_from_json(policy_json(f, issuer));
    const auto initial = verify_signed_billing_receipt(*f.store, policy, first.attempt_id, issuer.sign(receipt_payload(f, first)));
    (void)f.store->reconcile_usage(f.actor(Role::Root), initial, command(initial));
    const auto first_budget = f.store->budget(first_id);
    f.request_id = text(f.store->admit(fixture::admission(*assets, f.live, "sibling")), "request_id");
    const auto sibling = f.request("attempt-sibling"); (void)recover(f, sibling);
    const auto sibling_policy = billing_trust_policy_from_json(policy_json(f, issuer));
    const auto proof = verify_signed_billing_receipt(*f.store, sibling_policy, sibling.attempt_id,
        issuer.sign(receipt_payload(f, sibling)));
    error(ErrorCode::Conflict, [&] { (void)f.store->reconcile_usage(f.actor(Role::Root), proof, command(proof)); });
    CHECK(f.store->budget(first_id) == first_budget);
    error(ErrorCode::Unauthorized, [&] { (void)verify_signed_billing_receipt(*f.store, sibling_policy, first.attempt_id,
        issuer.sign(receipt_payload(f, sibling))); });
}

void same_receipt_revision_can_fill_partial_usage_but_never_overwrite() {
    fixture::Fixture f(*assets, settings()); Issuer issuer;
    const auto request = f.request(); const auto original = recover(f, request);
    const auto policy = billing_trust_policy_from_json(policy_json(f, issuer));
    auto partial = fixture::complete_usage(); partial.cache_read_tokens.reset(); partial.cache_write_tokens.reset(); partial.cost_micro_usd.reset();
    auto payload = receipt_payload(f, request, "receipt-one", partial);
    const auto first = verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(payload));
    (void)f.store->reconcile_usage(f.actor(Role::Root), first, command(first));
    CHECK(!f.store->live_accounting_status(f.request_id).at("ready").as_bool());
    payload.at("revision") = 2; payload.at("usage") = to_json(fixture::complete_usage());
    const auto complete = verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(payload));
    CHECK(first.source_reference() == complete.source_reference()); CHECK(first.required_reconciliation_id() != complete.required_reconciliation_id());
    (void)f.store->reconcile_usage(f.actor(Role::Root), complete, command(complete, 2));
    CHECK(f.store->live_accounting_status(f.request_id).at("ready").as_bool());
    payload.at("revision") = 3; payload.at("usage").at("cost_micro_usd") = 18;
    const auto contradiction = verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(payload));
    const auto result = f.store->reconcile_usage(f.actor(Role::Root), contradiction, command(contradiction, 3));
    CHECK(result.at("status") == "CONFLICT"); CHECK(result.at("after").at("cost_micro_usd") == 17);
    CHECK(!f.store->live_accounting_status(f.request_id).at("ready").as_bool());
    CHECK(f.store->attempt(request.attempt_id) == original);
    CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
}

void exclusive_receipt_claim_and_budget_rollback_together() {
    fixture::Fixture f(*assets, settings()); Issuer issuer;
    const auto request = f.request(); (void)recover(f, request);
    const auto policy = billing_trust_policy_from_json(policy_json(f, issuer));
    const auto proof = verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(receipt_payload(f, request)));
    const auto before = f.store->budget(f.request_id);
    fixture::sql(f.path, "CREATE TRIGGER fixture_billing_fault BEFORE INSERT ON events WHEN NEW.type='reconcile_usage' BEGIN SELECT RAISE(ABORT,'synthetic billing commit fault'); END;");
    test::throws<Error>([&] { (void)f.store->reconcile_usage(f.actor(Role::Root), proof, command(proof)); });
    CHECK(f.store->budget(f.request_id) == before); CHECK(fixture::sql_text(f.path, "SELECT count(*) FROM usage_reconciliations") == "0");
    fixture::sql(f.path, "DROP TRIGGER fixture_billing_fault;");
    CHECK(f.store->reconcile_usage(f.actor(Role::Root), proof, command(proof)).at("status") == "APPLIED");
    auto invalid = command(proof); invalid.reconciliation_id = "ordinary-id";
    error(ErrorCode::InvalidInput, [&] { (void)f.store->reconcile_usage(f.actor(Role::Root), proof, invalid); });
}

void exact_readonly_replay_survives_writer_epoch_without_new_authority() {
    fixture::Fixture f(*assets, settings()); Issuer issuer;
    const auto request = f.request(); (void)recover(f, request);
    const auto policy = billing_trust_policy_from_json(policy_json(f, issuer));
    const auto proof = verify_signed_billing_receipt(*f.store, policy, request.attempt_id, issuer.sign(receipt_payload(f, request)));
    const auto intent = command(proof);
    CHECK(f.store->replay_usage_reconciliation(f.actor(Role::Root), proof, intent).is_null());
    const auto result = f.store->reconcile_usage(f.actor(Role::Root), proof, intent);
    f.reopen();
    const auto budget = f.store->budget(f.request_id);
    const auto current_request = f.store->request(f.request_id);
    const auto reader = assets->open(f.path, {}, 410, StoreOpenMode::ReadOnly);
    CHECK(reader->replay_usage_reconciliation(reader->host_actor(Role::Root, f.request_id), proof, intent) == result);
    CHECK(f.store->request(f.request_id) == current_request && f.store->budget(f.request_id) == budget);
    auto different = intent; different.expected_projection_version = 2;
    error(ErrorCode::Conflict, [&] { (void)reader->replay_usage_reconciliation(reader->host_actor(Role::Root, f.request_id), proof, different); });
    different = intent; different.idempotency_key = "different";
    error(ErrorCode::Conflict, [&] { (void)reader->replay_usage_reconciliation(reader->host_actor(Role::Root, f.request_id), proof, different); });
    error(ErrorCode::Unauthorized, [&] { (void)reader->replay_usage_reconciliation(reader->host_actor(Role::Dispatcher, f.request_id), proof, intent); });
}

void no_claim_or_proven_no_network_cannot_consume_issuer_receipt() {
    fixture::Fixture f(*assets, settings()); Issuer issuer;
    const auto request = f.request(); (void)recover(f, request, false);
    const auto policy = billing_trust_policy_from_json(policy_json(f, issuer));
    error(ErrorCode::NotReady, [&] { (void)verify_signed_billing_receipt(*f.store, policy, request.attempt_id,
        issuer.sign(receipt_payload(f, request))); });
    fixture::Fixture cancelled(*assets, settings()); const auto other = cancelled.request();
    const auto original = cancelled.interrupted(other);
    const auto other_policy = billing_trust_policy_from_json(policy_json(cancelled, issuer));
    CHECK(!verify_observed_billing_receipt(*cancelled.store, other_policy, other,
        model_response_from_json(original.at("response")), original).has_value());
    error(ErrorCode::Conflict, [&] { (void)verify_signed_billing_receipt(*cancelled.store, other_policy, other.attempt_id,
        issuer.sign(receipt_payload(cancelled, other))); });
}
} // namespace

int main(int argc, char** argv) {
    try { assets = std::make_unique<fixture::Assets>(argc > 1 ? argv[1] : WLS_SOURCE_DIR); }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
    return test::run({
        {"legacy profile bytes and explicit unique wire challenges", legacy_profile_and_unique_challenge},
        {"bounded exact policy and original full profile binding", bounded_exact_policy_and_frozen_profiles},
        {"signed late receipt keeps original missing response and null settlement immutable", signed_late_receipt_preserves_original_missing_response},
        {"forged signatures, wrong keys and modified payloads reject", forged_signature_key_and_payload_are_rejected},
        {"issuer, trust domain, units and payload shape are exact", issuer_domain_units_and_shape_are_exact},
        {"unique attempt challenge and original wire request binding", unique_challenge_and_wire_identity_are_mandatory},
        {"receipt ownership survives key rotation and recovery", source_identity_survives_key_rotation_and_recovery},
        {"receipt ownership is global across sibling requests", source_identity_is_global_across_requests},
        {"same receipt partial revision and contradictions preserve known facts", same_receipt_revision_can_fill_partial_usage_but_never_overwrite},
        {"receipt ownership and accounting commit atomically", exclusive_receipt_claim_and_budget_rollback_together},
        {"read-only exact replay survives writer epoch without effects", exact_readonly_replay_survives_writer_epoch_without_new_authority},
        {"no claim or proven no network cannot consume signed charge", no_claim_or_proven_no_network_cannot_consume_issuer_receipt}
    });
}
