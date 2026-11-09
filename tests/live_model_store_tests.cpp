#include "../src/controls_detail.hpp"
#include "live_test_support.hpp"

#include <barrier>
#include <chrono>
#include <future>
#include <thread>

using namespace wls::s1;
using wls::Role;
namespace fixture = wls::test::live;
using fixture::Object;
using fixture::Array;
using fixture::text;
using fixture::number;
using fixture::error;

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
std::unique_ptr<fixture::Assets> assets;

// Inject a separate writer's commit immediately after the observer finishes
// reading its budget. SQLite's documented per-connection trace hook fixes the
// interleaving without sleeps or a probabilistic reader/writer stress loop.
std::function<void()> after_budget_read;
std::exception_ptr budget_read_failure;
int interleave_after_budget(unsigned type, void*, void* statement, void*) noexcept {
    if (type != SQLITE_TRACE_PROFILE || !after_budget_read) return 0;
    const auto* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
    if (!sql || std::string_view(sql) != "SELECT * FROM request_budgets WHERE request_id=?") return 0;
    auto callback = std::move(after_budget_read);
    after_budget_read = {};
    try { callback(); } catch (...) { budget_read_failure = std::current_exception(); }
    return 0;
}
int install_budget_interleave(sqlite3* database, char**, const sqlite3_api_routines*) {
    return sqlite3_trace_v2(database, SQLITE_TRACE_PROFILE, interleave_after_budget, nullptr);
}
struct BudgetReadInterleave {
    BudgetReadInterleave() {
        CHECK(sqlite3_auto_extension(reinterpret_cast<void (*)()>(install_budget_interleave)) == SQLITE_OK);
    }
    ~BudgetReadInterleave() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(install_budget_interleave));
        after_budget_read = {}; budget_read_failure = {};
    }
};

void readonly_ledger_observes_one_snapshot() {
    fixture::Fixture f(*assets);
    const auto request = f.request("interleaved-reservation");
    BudgetReadInterleave interleave;
    auto reader = assets->open(f.path, {}, 410, StoreOpenMode::ReadOnly);
    const auto original_request = f.store->request(f.request_id);
    bool committed = false;
    after_budget_read = [&] { (void)f.reserve(request); committed = true; };
    const auto observed = reader->verify_usage_ledger(f.request_id);
    if (budget_read_failure) std::rethrow_exception(budget_read_failure);
    CHECK(committed);
    CHECK(observed.at("consistent").as_bool());
    CHECK(number(observed.at("durable_budget"), "attempts") == 0);
    CHECK(observed.at("attempts").as_array().empty());
    CHECK(!observed.at("unsettled_accounting").as_bool());
    CHECK(number(f.store->budget(f.request_id), "attempts") == 1);
    CHECK(reader->request(f.request_id) == original_request);
    const auto refreshed = reader->verify_usage_ledger(f.request_id);
    CHECK(refreshed.at("consistent").as_bool());
    CHECK(number(refreshed.at("durable_budget"), "attempts") == 1);
    CHECK(refreshed.at("attempts").as_array().size() == 1);
    CHECK(refreshed.at("unsettled_accounting").as_bool());
}

void reconciled_usage_releases_settlement_capacity() {
    fixture::Fixture f(*assets);
    CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 16);
    const auto request = f.request();
    const auto original = f.interrupted(request);
    const auto original_bytes = canonical_json(f.store->attempt(request.attempt_id));
    CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 20);
    auto partial = fixture::complete_usage(); partial.cache_write_tokens.reset();
    (void)f.store->reconcile_usage(f.actor(Role::Root), fixture::evidence(original, partial, "partial-capacity"),
        {"partial-capacity", "partial-capacity", 1});
    // A zero reserved balance cannot substitute for the missing cache fact.
    CHECK(number(f.store->budget(f.request_id), "reserved_cost") == 0);
    CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 20);
    (void)f.store->reconcile_usage(f.actor(Role::Root), fixture::evidence(original, fixture::complete_usage(), "complete-capacity"),
        {"complete-capacity", "complete-capacity", 2});
    CHECK(f.store->usage_projection(request.attempt_id).at("accounting_complete").as_bool());
    CHECK(!f.store->verify_usage_ledger(f.request_id).at("unsettled_accounting").as_bool());
    CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 16);
    CHECK(canonical_json(f.store->attempt(request.attempt_id)) == original_bytes);
}

void fake_and_legacy_capacity_keep_original_usage_rules() {
    for (const int version : {408, 410}) {
        fixture::Fixture f(*assets, fixture::settings(), false, version, true);
        const auto request = f.request("known-fake-usage");
        CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 16);
        (void)f.store->reserve_attempt(f.actor(), request.binding, f.reservation(request, true));
        CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 20);
        auto usage = fixture::complete_usage();
        usage.cache_read_tokens.reset(); usage.cache_write_tokens.reset();
        (void)f.store->settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
            "COMPLETED", to_json(usage), Object{{"synthetic", true}});
        CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 16);
        const auto pending = f.request("unknown-fake-cost");
        (void)f.store->reserve_attempt(f.actor(), pending.binding, f.reservation(pending, true));
        usage.cost_micro_usd.reset();
        (void)f.store->settle_attempt(pending.attempt_id, pending.binding, pending.config_snapshot_id,
            "INTERRUPTED", to_json(usage), Object{{"synthetic", true}});
        CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 20);
        if (version == 410) CHECK(f.store->verify_usage_ledger(f.request_id).at("unsettled_accounting").as_bool());
    }
}

void disabled_and_complete_envelope() {
    fixture::Fixture f(*assets, fixture::settings(), false);
    const auto request = f.request(); const auto budget = f.store->budget(f.request_id);
    error(ErrorCode::Unauthorized, [&] { (void)f.reserve(request); });
    error(ErrorCode::Unauthorized, [&] { (void)execute_live_attempt(*f.store, f.actor(), request, "SYNTHETIC"); });
    error(ErrorCode::Unauthorized, [&] { (void)f.store->reserve_attempt(f.actor(), request.binding, f.reservation(request, true)); });
    CHECK(f.store->budget(f.request_id) == budget);
    fixture::Fixture active(*assets);
    auto missing = active.request(); missing.reserved_cost_micro_usd = 0;
    error(ErrorCode::InvalidInput, [&] { (void)active.reserve(missing); });
    auto altered_request = active.request(); altered_request.binding.policy_epoch = 87;
    auto changed = active.reservation(altered_request);
    error(ErrorCode::Conflict, [&] { (void)active.store->reserve_attempt(active.actor(), active.store->request_binding(active.request_id), changed); });
    CHECK(number(active.store->budget(active.request_id), "attempts") == 0);
}

void live_profile_capacity_is_rejected_before_admission() {
    auto accepted = fixture::settings();
    CHECK(live_model_settings_from_json(fixture::model_profile(accepted), accepted.provider.model_ref).transport.max_total_response_bytes == 3U * 1024U * 1024U);
    auto too_wide = accepted; ++too_wide.transport.max_total_response_bytes;
    error(ErrorCode::InvalidInput, [&] { (void)live_model_settings_from_json(fixture::model_profile(too_wide), too_wide.provider.model_ref); });
    too_wide = accepted; ++too_wide.provider.limits.max_response_bytes;
    error(ErrorCode::InvalidInput, [&] { (void)live_model_settings_from_json(fixture::model_profile(too_wide), too_wide.provider.model_ref); });
    too_wide = accepted; too_wide.provider.limits.max_stream_bytes = 16U * 1024U * 1024U;
    error(ErrorCode::InvalidInput, [&] { (void)live_model_settings_from_json(fixture::model_profile(too_wide), too_wide.provider.model_ref); });
    too_wide = accepted; ++too_wide.provider.limits.max_text_bytes;
    error(ErrorCode::InvalidInput, [&] { (void)live_model_settings_from_json(fixture::model_profile(too_wide), too_wide.provider.model_ref); });
}

void live_profile_route_is_rejected_before_admission() {
    auto selected = fixture::settings();
    selected.execution_kind = "provider";
    const auto original = fixture::model_profile(selected);
    for (const auto* route : {"999.0.0.1", "127.1", "127.00.0.1", "127.0.0.1.", "..."}) {
        auto changed = original;
        changed.at("responses").at("transport").at("resolved_ipv4") = route;
        error(ErrorCode::InvalidInput, [&] {
            (void)live_model_settings_from_json(changed, selected.provider.model_ref);
        });
    }
    for (const auto* host : {"127.1", "2130706433", "0x7f000001", "127.00.0.1"}) {
        auto changed = original;
        changed.at("responses").at("provider_profile").at("base_url") = std::string("https://") + host;
        error(ErrorCode::InvalidInput, [&] {
            (void)live_model_settings_from_json(changed, selected.provider.model_ref);
        });
    }
    auto inconsistent = original;
    inconsistent.at("responses").at("provider_profile").at("base_url") = "https://127.0.0.2";
    error(ErrorCode::InvalidInput, [&] {
        (void)live_model_settings_from_json(inconsistent, selected.provider.model_ref);
    });
    // Valid DNS + explicit route, strict numeric destinations and the existing
    // c-ares-only runtime DNS path retain their original configuration bytes.
    for (const auto* host : {"localhost", "gateway.example.invalid", "127.0.0.1"}) {
        auto changed = original;
        changed.at("responses").at("provider_profile").at("base_url") = std::string("https://") + host;
        const auto parsed = live_model_settings_from_json(changed, selected.provider.model_ref);
        CHECK(live_model_settings_json(parsed) == changed.at("responses"));
    }
    auto dns = original;
    dns.at("responses").at("transport").at("resolved_ipv4") = "";
    CHECK(live_model_settings_json(live_model_settings_from_json(dns, selected.provider.model_ref)) == dns.at("responses"));
}

void frozen_identity_and_one_claim() {
    fixture::Fixture f(*assets); const auto request = f.request();
    const auto reservation = f.reserve(request); CHECK(f.reserve(request) == reservation);
    const auto original = f.store->live_attempt(request.attempt_id);
    CHECK(original.at("execution").at("execution_kind") == "local_tls");
    auto changed = encode_responses_request(f.live.provider, request, false); changed.body += " ";
    error(ErrorCode::Corrupt, [&] { (void)f.store->claim_live_send(f.actor(), request.attempt_id, 1, changed, f.live.transport, "bad"); });
    changed = encode_responses_request(f.live.provider, request, false);
    auto altered_options = f.live.transport; altered_options.max_header_bytes += 1;
    error(ErrorCode::Conflict, [&] { (void)f.store->claim_live_send(f.actor(), request.attempt_id, 1, changed, altered_options, "bad"); });
    error(ErrorCode::Conflict, [&] { (void)f.store->claim_live_send(f.actor(), request.attempt_id, 2, changed, f.live.transport, "bad"); });
    CHECK(f.store->live_attempt(request.attempt_id).at("send_claim").is_null());
    std::barrier start(3);
    auto contender = [&] { start.arrive_and_wait(); return f.claim(request); };
    auto one = std::async(std::launch::async, contender); auto two = std::async(std::launch::async, contender);
    start.arrive_and_wait(); const auto a = one.get(); const auto b = two.get();
    CHECK(a.at("claim_id") == b.at("claim_id"));
    CHECK(a.at("new_send_authority").as_bool() != b.at("new_send_authority").as_bool());
    CHECK(!f.claim(request).at("new_send_authority").as_bool());
    CHECK(number(f.store->budget(f.request_id), "attempts") == 1);
}

void reservation_and_claim_rollback() {
    fixture::Fixture f(*assets); const auto request = f.request(); const auto budget = f.store->budget(f.request_id);
    fixture::sql(f.path, "CREATE TRIGGER fixture_fail_live BEFORE INSERT ON live_attempt_execution BEGIN SELECT RAISE(ABORT,'synthetic admission fault'); END;");
    test::throws<Error>([&] { (void)f.reserve(request); });
    CHECK(f.store->budget(f.request_id) == budget);
    CHECK(fixture::sql_text(f.path, "SELECT count(*) FROM call_attempts") == "0");
    fixture::sql(f.path, "DROP TRIGGER fixture_fail_live;"); (void)f.reserve(request);
    fixture::sql(f.path, "CREATE TRIGGER fixture_fail_claim BEFORE INSERT ON events WHEN NEW.type='claim_live_send' BEGIN SELECT RAISE(ABORT,'synthetic claim fault'); END;");
    test::throws<Error>([&] { (void)f.claim(request); });
    CHECK(f.store->live_attempt(request.attempt_id).at("send_claim").is_null());
    fixture::sql(f.path, "DROP TRIGGER fixture_fail_claim;"); CHECK(f.claim(request).at("new_send_authority").as_bool());
}

void observation_and_original_settlement_rollback() {
    fixture::Fixture f(*assets); const auto request = f.request(); (void)f.reserve(request); const auto claim = f.claim(request);
    const auto transport = f.cancelled(request); CHECK(!transport.network_started);
    CHECK(transport.transport_options_sha256 == live_transport_options_sha256(f.live.transport));
    error(ErrorCode::UnknownOutcome, [&] { (void)f.settle(request, transport); });
    fixture::sql(f.path, "CREATE TRIGGER fixture_observation_fault BEFORE INSERT ON events WHEN NEW.type='record_live_transport' BEGIN SELECT RAISE(ABORT,'synthetic observation fault'); END;");
    test::throws<Error>([&] { (void)f.store->record_live_transport(request.attempt_id, text(claim, "claim_id"), transport); });
    CHECK(f.store->live_attempt(request.attempt_id).at("transport").is_null());
    fixture::sql(f.path, "DROP TRIGGER fixture_observation_fault;");
    const auto saved = f.store->record_live_transport(request.attempt_id, text(claim, "claim_id"), transport);
    CHECK(saved == f.store->record_live_transport(request.attempt_id, text(claim, "claim_id"), transport));
    auto corrupt = transport; corrupt.raw_body += "secret";
    error(ErrorCode::Corrupt, [&] { (void)f.store->record_live_transport(request.attempt_id, text(claim, "claim_id"), corrupt); });
    const auto original_budget = f.store->budget(f.request_id);
    fixture::sql(f.path, "CREATE TRIGGER fixture_settle_fault BEFORE INSERT ON live_usage_projection BEGIN SELECT RAISE(ABORT,'synthetic settlement fault'); END;");
    test::throws<Error>([&] { (void)f.settle(request, transport); });
    CHECK(f.store->attempt(request.attempt_id).at("settlement_sha256").is_null());
    CHECK(f.store->budget(f.request_id) == original_budget);
    fixture::sql(f.path, "DROP TRIGGER fixture_settle_fault;"); const auto settled = f.settle(request, transport);
    CHECK(f.settle(request, transport) == settled);
    CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
    auto invented_usage = to_json(transport.response.usage); invented_usage.at("cost_micro_usd") = 0;
    error(ErrorCode::Conflict, [&] { (void)f.store->settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
        "INTERRUPTED", invented_usage, to_json(transport.response)); });
}

void partial_usage_exact_replay_and_completion() {
    fixture::Fixture f(*assets); const auto request = f.request(); const auto original = f.interrupted(request);
    const auto original_bytes = canonical_json(f.store->attempt(request.attempt_id));
    ModelUsage partial; partial.input_tokens = 9; partial.cost_micro_usd = 17;
    const auto proof = fixture::evidence(original, partial);
    const UsageReconciliation first{"usage-first", "key-first", 1};
    const auto result = f.store->reconcile_usage(f.actor(Role::Root), proof, first);
    CHECK(f.store->reconcile_usage(f.actor(Role::Root), proof, first) == result);
    auto budget = f.store->budget(f.request_id);
    CHECK(number(budget, "spent_input") == 9); CHECK(number(budget, "spent_cost") == 17);
    CHECK(number(budget, "reserved_input") == 0); CHECK(number(budget, "reserved_output") == 100);
    CHECK(number(budget, "unknown_output") == 100); CHECK(number(budget, "reserved_cost") == 0);
    CHECK(!f.store->usage_projection(request.attempt_id).at("accounting_complete").as_bool());
    error(ErrorCode::UnknownOutcome, [&] { (void)f.reserve(f.request("blocked-paid")); });
    error(ErrorCode::Conflict, [&] { (void)f.store->reconcile_usage(f.actor(Role::Root), proof, {"usage-other", "key-first", 2}); });
    const auto complete = fixture::evidence(original, fixture::complete_usage(), "complete");
    error(ErrorCode::Conflict, [&] { (void)f.store->reconcile_usage(f.actor(Role::Root), complete, {"usage-complete", "key-complete", 1}); });
    const auto second = f.store->reconcile_usage(f.actor(Role::Root), complete, {"usage-complete", "key-complete", 2});
    CHECK(second.at("budget_delta").at("input").at("spent") == 0);
    CHECK(second.at("budget_delta").at("cost").at("spent") == 0);
    CHECK(f.store->usage_projection(request.attempt_id).at("accounting_complete").as_bool());
    CHECK(number(f.store->budget(f.request_id), "reserved_output") == 0);
    CHECK(canonical_json(f.store->attempt(request.attempt_id)) == original_bytes);
    CHECK(f.store->settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
        "INTERRUPTED", original.at("usage"), original.at("response")) == original);
    CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
}

void known_zero_does_not_clear_unknown_cache() {
    fixture::Fixture f(*assets); const auto request = f.request(); const auto settled = f.interrupted(request);
    ModelUsage zeros; zeros.input_tokens = 0; zeros.output_tokens = 0; zeros.cost_micro_usd = 0;
    (void)f.store->reconcile_usage(f.actor(Role::Root), fixture::evidence(settled, zeros), {"zeros", "zeros", 1});
    const auto budget = f.store->budget(f.request_id);
    CHECK(number(budget, "reserved_input") == 0 && number(budget, "reserved_output") == 0 && number(budget, "reserved_cost") == 0);
    const auto projection = f.store->usage_projection(request.attempt_id);
    CHECK(projection.at("unknown_dimensions").as_array().size() == 2);
    CHECK(!projection.at("accounting_complete").as_bool());
    CHECK(f.store->verify_usage_ledger(f.request_id).at("unsettled_accounting").as_bool());
    error(ErrorCode::UnknownOutcome, [&] { (void)f.reserve(f.request("unknown-cache-block")); });
}

void contradictory_evidence_and_over_reservation() {
    fixture::Fixture f(*assets); const auto request = f.request(); const auto settled = f.interrupted(request);
    (void)f.store->reconcile_usage(f.actor(Role::Root), fixture::evidence(settled, fixture::complete_usage()), {"one", "one", 1});
    const auto balance = f.store->budget(f.request_id);
    const auto disputed = f.store->reconcile_usage(f.actor(Role::Root), fixture::evidence(settled, fixture::complete_usage(10), "contradict"), {"two", "two", 2});
    CHECK(disputed.at("status") == "CONFLICT"); CHECK(disputed.at("after").at("input_tokens") == 9);
    CHECK(disputed.at("after").at("disputed") == 1);
    CHECK(number(f.store->budget(f.request_id), "spent_input") == number(balance, "spent_input"));
    CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 20);
    error(ErrorCode::UnknownOutcome, [&] { (void)f.reserve(f.request("disputed-block")); });
    CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
    fixture::Fixture excess(*assets); const auto other = excess.request(); const auto original = excess.interrupted(other);
    const auto result = excess.store->reconcile_usage(excess.actor(Role::Root),
        fixture::evidence(original, fixture::complete_usage(1200, 150, 12000)), {"excess", "excess", 1});
    CHECK(result.at("after").at("over_reserved") == 1);
    CHECK(number(excess.store->budget(excess.request_id), "spent_input") == 1200);
    CHECK(number(excess.store->budget(excess.request_id), "spent_cost") == 12000);
    CHECK(number(excess.store->budget(excess.request_id), "cost_limit") == 10000);
    // Complete excess usage blocks paid admission, but does not leave unknown
    // settlement work or permanently occupy its control reservation.
    CHECK(number(excess.store->capacity_status(excess.request_id), "reserved_control") == 16);
    error(ErrorCode::UnknownOutcome, [&] { (void)excess.reserve(excess.request("excess-block")); });
    CHECK(excess.store->verify_usage_ledger(excess.request_id).at("consistent").as_bool());
}

void reconcile_atomic_and_private_source() {
    fixture::Fixture f(*assets); const auto request = f.request(); const auto original = f.interrupted(request);
    const auto budget = f.store->budget(f.request_id); const auto projection = f.store->usage_projection(request.attempt_id);
    const auto proof = fixture::evidence(original, fixture::complete_usage(), "PRIVATE_SYNTHETIC_SOURCE");
    fixture::sql(f.path, "CREATE TRIGGER fixture_usage_fault BEFORE INSERT ON events WHEN NEW.type='reconcile_usage' BEGIN SELECT RAISE(ABORT,'synthetic reconciliation fault'); END;");
    test::throws<Error>([&] { (void)f.store->reconcile_usage(f.actor(Role::Root), proof, {"fault", "fault", 1}); });
    CHECK(f.store->budget(f.request_id) == budget); CHECK(f.store->usage_projection(request.attempt_id) == projection);
    CHECK(fixture::sql_text(f.path, "SELECT count(*) FROM usage_reconciliations") == "0");
    fixture::sql(f.path, "DROP TRIGGER fixture_usage_fault;");
    (void)f.store->reconcile_usage(f.actor(Role::Root), proof, {"fault", "fault", 1});
    CHECK(canonical_json(f.store->usage_projection(request.attempt_id)).find("PRIVATE_SYNTHETIC_SOURCE") == std::string::npos);
    CHECK(canonical_json(f.store->live_attempt(request.attempt_id)).find("PRIVATE_SYNTHETIC_SOURCE") == std::string::npos);
    CHECK(fixture::sql_text(f.path, "SELECT source_reference FROM usage_reconciliations").find("PRIVATE_SYNTHETIC_SOURCE") != std::string::npos);
    test::throws([&] { fixture::sql(f.path, "UPDATE usage_reconciliations SET source_reference='replacement';"); });
    test::throws([&] { fixture::sql(f.path, "UPDATE live_usage_projection SET input_tokens=99,state_version=state_version+1;"); });
    CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
}

void recovery_unobserved_is_atomic_and_never_resends() {
    fixture::Fixture f(*assets); const auto request = f.request(); const auto original_actor = f.actor();
    (void)f.reserve(request); const auto first_claim = f.claim(request);
    error(ErrorCode::NotReady, [&] { (void)f.store->settle_unobserved_live(f.actor(Role::Root), request.attempt_id, 1, "unobserved_after_recovery", "host"); });
    f.reopen(); CHECK(f.store->attempt(request.attempt_id).at("status") == "UNKNOWN");
    const auto replay = f.store->claim_live_send(original_actor, request.attempt_id, 1,
        encode_responses_request(f.live.provider, request, false), f.live.transport, "claim-" + request.attempt_id);
    CHECK(replay.at("claim_id") == first_claim.at("claim_id")); CHECK(!replay.at("new_send_authority").as_bool());
    const auto version = number(f.store->attempt(request.attempt_id), "state_version");
    const auto before = f.store->budget(f.request_id);
    fixture::sql(f.path, "CREATE TRIGGER fixture_host_fault BEFORE INSERT ON live_usage_projection BEGIN SELECT RAISE(ABORT,'synthetic host recovery fault'); END;");
    test::throws<Error>([&] { (void)f.store->settle_unobserved_live(f.actor(Role::Root), request.attempt_id, version, "unobserved_after_recovery", "host"); });
    CHECK(f.store->budget(f.request_id) == before);
    CHECK(fixture::sql_text(f.path, "SELECT count(*) FROM live_unobserved_settlements") == "0");
    fixture::sql(f.path, "DROP TRIGGER fixture_host_fault;");
    const auto settled = f.store->settle_unobserved_live(f.actor(Role::Root), request.attempt_id, version, "unobserved_after_recovery", "host");
    CHECK(settled.at("accepted") == 0); CHECK(settled.at("response").at("status") == "interrupted");
    CHECK(settled.at("response").at("error_code") == "unobserved_after_recovery");
    CHECK(settled.at("usage").at("cost_micro_usd").is_null());
    CHECK(f.store->settle_unobserved_live(f.actor(Role::Root), request.attempt_id, version, "unobserved_after_recovery", "host") == settled);
    const auto late = f.cancelled(request);
    (void)f.store->record_live_transport(request.attempt_id, text(first_claim, "claim_id"), late);
    error(ErrorCode::Conflict, [&] { (void)f.settle(request, late); });
    (void)f.store->reconcile_usage(f.actor(Role::Root), fixture::evidence(settled, fixture::complete_usage()), {"recovered", "recovered", 1});
    CHECK(f.store->attempt(request.attempt_id) == settled);
    CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
}

void recovered_reservation_without_send_has_explicit_host_proof() {
    auto provider = fixture::settings(); provider.execution_kind = "provider";
    fixture::Fixture f(*assets, provider); const auto request = f.request();
    (void)f.reserve(request);
    error(ErrorCode::NotReady, [&] { (void)f.store->settle_unobserved_live(f.actor(Role::Root), request.attempt_id, 1,
        "not_dispatched_after_recovery", "no-send"); });
    f.reopen();
    const auto version = number(f.store->attempt(request.attempt_id), "state_version");
    error(ErrorCode::Conflict, [&] { (void)f.store->settle_unobserved_live(f.actor(Role::Root), request.attempt_id, version,
        "unobserved_after_recovery", "wrong-reason"); });
    const auto settled = f.store->settle_unobserved_live(f.actor(Role::Root), request.attempt_id, version,
        "not_dispatched_after_recovery", "no-send");
    CHECK(settled.at("usage").at("cost_micro_usd").is_null());
    ModelUsage zero; zero.input_tokens = 0; zero.output_tokens = 0; zero.cache_read_tokens = 0;
    zero.cache_write_tokens = 0; zero.cost_micro_usd = 0;
    auto nonzero = zero; nonzero.input_tokens = 1;
    error(ErrorCode::InvalidInput, [&] { (void)f.store->reconcile_usage(f.actor(Role::Root),
        fixture::evidence(settled, nonzero, "not-zero", "host_no_dispatch"), {"bad", "bad", 1}); });
    (void)f.store->reconcile_usage(f.actor(Role::Root), fixture::evidence(settled, zero, "no-send-proof", "host_no_dispatch"),
        {"no-send-proof", "no-send-proof", 1});
    CHECK(f.store->attempt(request.attempt_id) == settled);
    CHECK(f.store->usage_projection(request.attempt_id).at("accounting_complete").as_bool());
    CHECK(f.store->live_attempt(request.attempt_id).at("send_claim").is_null());
    CHECK(number(f.store->budget(f.request_id), "spent_cost") == 0);
    CHECK(number(f.store->budget(f.request_id), "reserved_cost") == 0);
    CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
    (void)f.reserve(f.request("after-no-send"));
    fixture::Fixture sent(*assets); const auto sent_request = sent.request();
    const auto sent_settlement = sent.interrupted(sent_request);
    error(ErrorCode::Unauthorized, [&] { (void)sent.store->reconcile_usage(sent.actor(Role::Root),
        fixture::evidence(sent_settlement, zero, "false-no-send", "host_no_dispatch"), {"forbidden", "forbidden", 1}); });
}

void terminal_scoped_reconciliation_and_sibling_isolation() {
    fixture::Fixture f(*assets); const auto request = f.request(); const auto original = f.interrupted(request);
    const auto sibling_id = text(f.store->admit(fixture::admission(*assets, f.live, "sibling")), "request_id");
    const auto sibling = f.store->request(sibling_id); const auto sibling_budget = f.store->budget(sibling_id);
    (void)f.store->cancel_request(f.actor(Role::Root), number(f.store->request(f.request_id), "state_version"), "cancel");
    const auto terminal = f.store->request(f.request_id);
    f.reopen(StoreRecoveryScope::request(f.request_id));
    const auto root = f.actor(Role::Root);
    const auto evidence = fixture::evidence(original, fixture::complete_usage());
    (void)f.store->reconcile_usage(root, evidence, {"terminal", "terminal", 1});
    CHECK(f.store->request(f.request_id).at("status") == terminal.at("status"));
    CHECK(f.store->attempt(request.attempt_id) == original);
    error(ErrorCode::Unauthorized, [&] { (void)f.store->request(sibling_id); });
    f.store.reset();
    auto reader = assets->open(f.path, {}, 410, StoreOpenMode::ReadOnly);
    CHECK(reader->request(sibling_id) == sibling); CHECK(reader->budget(sibling_id) == sibling_budget);
    CHECK(reader->verify_usage_ledger(f.request_id).at("consistent").as_bool());
    error(ErrorCode::Unauthorized, [&] { (void)reader->reconcile_usage(root, evidence, {"terminal", "terminal", 1}); });
}

void scoped_upgrade_and_fake_bytes_stay_original() {
    fixture::Fixture f(*assets, fixture::settings(), false, 408, true);
    const auto request = f.request(); const auto original_actor = f.actor();
    (void)f.store->reserve_attempt(original_actor, request.binding, f.reservation(request, true));
    const auto settled = f.store->settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
        "COMPLETED", to_json(fixture::complete_usage()), Object{{"synthetic", true}});
    const auto snapshot = f.store->request(f.request_id).at("config_snapshot");
    const auto budget = f.store->budget(f.request_id); f.store.reset();
    error(ErrorCode::Unsupported, [&] { auto rejected = assets->open(f.path, StoreRecoveryScope::request(f.request_id)); });
    CHECK(fixture::sql_text(f.path, "PRAGMA user_version") == "408");
    f.store = assets->open(f.path);
    CHECK(number(f.store->diagnostics(), "user_version") == 410);
    CHECK(f.store->attempt(request.attempt_id) == settled);
    CHECK(f.store->request(f.request_id).at("config_snapshot") == snapshot);
    CHECK(f.store->budget(f.request_id) == budget);
    CHECK(fixture::sql_text(f.path, "SELECT count(*) FROM live_attempt_execution") == "0");
    CHECK(f.store->settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
        "COMPLETED", to_json(fixture::complete_usage()), Object{{"synthetic", true}}) == settled);
    CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
    f.store.reset();
    error(ErrorCode::Unsupported, [&] { auto old = assets->open(f.path, {}, 408, StoreOpenMode::ReadOnly); });
    auto reader = assets->open(f.path, {}, 410, StoreOpenMode::ReadOnly);
    CHECK(reader->attempt(request.attempt_id) == settled);
}

void policy_and_identity_fences_before_claim() {
    fixture::Fixture f(*assets); const auto request = f.request(); (void)f.reserve(request);
    fixture::sql(f.path, "UPDATE runtime_policy SET policy_epoch=policy_epoch+1,state_version=state_version+1 WHERE singleton=1;");
    error(ErrorCode::Stale, [&] { (void)f.claim(request); });
    CHECK(f.store->live_attempt(request.attempt_id).at("send_claim").is_null());
    const auto sibling = text(f.store->admit(fixture::admission(*assets, f.live, "other")), "request_id");
    const auto intruder = f.store->host_actor(Role::Dispatcher, sibling);
    error(ErrorCode::Unauthorized, [&] { (void)f.store->claim_live_send(intruder, request.attempt_id, 1,
        encode_responses_request(f.live.provider, request, false), f.live.transport, "intrude"); });
}

void source_verifier_and_role_are_mandatory() {
    fixture::Fixture f(*assets); const auto request = f.request(); const auto original = f.interrupted(request);
    error(ErrorCode::InvalidInput, [&] { (void)verify_usage_evidence("loopback_fixture", "source", "body", {}); });
    const auto valid = fixture::evidence(original, fixture::complete_usage());
    error(ErrorCode::Unauthorized, [&] { (void)f.store->reconcile_usage(f.actor(), valid, {"unauthorized", "unauthorized", 1}); });
    auto other = original; other.at("settlement_sha256") = sha256_hex("wrong original");
    error(ErrorCode::Conflict, [&] { (void)f.store->reconcile_usage(f.actor(Role::Root), fixture::evidence(other, fixture::complete_usage()), {"wrong", "wrong", 1}); });
    auto provider = fixture::settings(); provider.execution_kind = "provider";
    fixture::Fixture p(*assets, provider); const auto q = p.request(); const auto settled = p.interrupted(q);
    error(ErrorCode::Unauthorized, [&] { (void)p.store->reconcile_usage(p.actor(Role::Root), fixture::evidence(settled, fixture::complete_usage()), {"fixture-for-provider", "fixture-for-provider", 1}); });
    error(ErrorCode::Unauthorized, [&] { (void)execute_live_attempt(*p.store, p.actor(), p.request("new-provider"), "SYNTHETIC", {true, false}); });
    CHECK(number(p.store->budget(p.request_id), "attempts") == 1);
}

// --- W/D RED inputs prepared by the v16 test-only source stage ---
// Prepared, not executed in that stage. They reuse only the existing public
// Store 410 surface, the existing fixture assets and the S0 seam header; no
// new support framework and no production change.

namespace cd = wls::controls::detail;

ModelRequest admitted_request(Store& store, const std::string& request_id, std::string attempt_id) {
    const auto row = store.request(request_id);
    const auto frozen = FrozenRequestConfig::restore(canonical_json(row.at("config_snapshot")),
                                                     text(row, "config_snapshot_sha256"));
    const auto profile = frozen.model_profile(Role::Dispatcher);
    ContextInput context;
    context.role = Role::Dispatcher; context.model = frozen.resolve(Role::Dispatcher);
    context.adapter_version = text(profile, "adapter_version"); context.protocol_version = text(profile, "protocol");
    context.system_prompt = "Synthetic Store fixture only."; context.role_prompt = "Preserve original captured identity.";
    context.function_prompt = "Return a bounded note."; context.task_contract = canonical_json(store.request_snapshot(request_id));
    ModelRequest output;
    output.call_id = "call-" + attempt_id; output.attempt_id = std::move(attempt_id); output.role = Role::Dispatcher;
    output.binding = store.request_binding(request_id); output.config_snapshot_id = text(row, "config_snapshot_sha256");
    output.model = context.model;
    output.context = frozen.context_request_format() == "structured-v2" ? compile_structured_context(context, {}) : ContextCompiler().compile(context);
    output.reserved_input_tokens = 1000; output.reserved_output_tokens = 100; output.reserved_cost_micro_usd = 100;
    output.max_output_tokens = 100; output.timeout = std::chrono::seconds(3);
    return output;
}

AttemptReservation fake_reservation(const ModelRequest& request, std::int64_t reserved_cost_micro_usd) {
    AttemptReservation result;
    result.call_id = request.call_id; result.attempt_id = request.attempt_id;
    result.config_snapshot_sha256 = request.config_snapshot_id; result.model_ref = request.model.model;
    result.adapter_kind = "fake";
    result.reserved_input_tokens = request.reserved_input_tokens; result.reserved_output_tokens = request.reserved_output_tokens;
    result.reserved_cost_micro_usd = reserved_cost_micro_usd; result.max_output_tokens = request.max_output_tokens;
    result.request_envelope = to_json(request); return result;
}

std::vector<Json> budget_warning_events(Store& store, const std::string& request_id) {
    std::vector<Json> found;
    std::int64_t offset = 0;
    while (true) {
        const auto page = store.events(request_id, offset, 64);
        if (page.empty()) break;
        offset += static_cast<std::int64_t>(page.size());
        for (const auto& event : page)
            if (event.at("type").as_string() == "budget_warning") found.push_back(event);
    }
    return found;
}

void w_red_budget_warning_missing_after_exact_crossing() {
    // W-RED input: a legal Fake reservation moves E from 0 to exactly 100
    // against the B = 125 micro-USD budget frozen at public admit; 100/125
    // == 4/5 crosses the config-declared warn_at_fraction 0.8 exactly. The
    // original permission/binding/ledger/capacity guards hold first. The
    // business target is exactly one durable budget_warning event carrying
    // the fixed request/config/B/unit/p/q identity, observed over the
    // complete paginated event history. The old source admits the
    // reservation with no warning at all: that and only that is the RED.
    fixture::Fixture f(*assets, fixture::settings(), false, 410, true);
    auto w_input = fixture::admission(*assets, f.live, "w-red-budget-125", false, true);
    w_input.budget = BudgetLimits{8, 1000000, 100000, 125};
    const std::string w_id = text(f.store->admit(w_input), "request_id");
    const auto config_sha = text(f.store->request(w_id), "config_snapshot_sha256");
    const auto actor = f.store->host_actor(Role::Dispatcher, w_id);
    const auto request = admitted_request(*f.store, w_id, "w-red-crossing");
    (void)f.store->reserve_attempt(actor, request.binding, fake_reservation(request, 100));
    CHECK(f.store->verify_usage_ledger(w_id).at("consistent").as_bool());
    CHECK(number(f.store->budget(w_id), "reserved_cost") == 100);
    CHECK(number(f.store->budget(w_id), "cost_limit") == 125);
    CHECK(number(f.store->capacity_status(w_id), "reserved_control") == 20);
    CHECK(5 * 100 >= 4 * 125);  // exact integer crossing oracle for p/q = 4/5
    const auto warnings = budget_warning_events(*f.store, w_id);
    CHECK(warnings.size() == 1);
    const auto& payload = warnings.front().at("payload");
    CHECK(text(payload, "request_id") == w_id);
    CHECK(text(payload, "config_snapshot_sha256") == config_sha);
    CHECK(number(payload, "budget_micro_usd") == 125);
    CHECK(text(payload, "unit") == "micro_usd");
    CHECK(number(payload, "warn_numerator") == 4);
    CHECK(number(payload, "warn_denominator") == 5);
}

void d_red_expired_wall_deadline_still_reserves() {
    // D-RED input: a legal max_wall_minutes = 1 wall deadline is frozen
    // through normal config loading and the public admit (the sample already
    // declares budget_defaults.max_wall_minutes; the rewrite only narrows the
    // value). After a REAL CLOCK_BOOTTIME wait past the one-minute bound the
    // first new legal reservation must not create an attempt. No SQL time
    // edit, no system-clock change; the sampler is only read for evidence.
    // The old source has no request wallclock gate and still reserves:
    // exactly that is the RED. Bounded plan against the unchanged 90-second
    // suite bound: setup < 5s, one real wait <= 61.5s in 250ms slices,
    // assertions < 5s.
    ::test::TempDir directory;
    std::filesystem::copy(assets->source / "config/examples", directory.path / "config",
                          std::filesystem::copy_options::recursive);
    std::filesystem::copy(assets->source / "prompts", directory.path / "prompts",
                          std::filesystem::copy_options::recursive);
    const auto system_path = directory.path / "config/system.yaml";
    auto system = fixture::read(system_path);
    const std::string existing = "max_wall_minutes: 120";
    const auto position = system.find(existing);
    CHECK(position != std::string::npos);
    system.replace(position, existing.size(), "max_wall_minutes: 1");
    { std::ofstream output(system_path); output << system; }
    auto d_input = fixture::admission(*assets, fixture::settings(), "d-red-wall-1", false, true);
    d_input.config_snapshot = load_config_catalog(system_path, directory.path / "config/functions",
                                                  directory.path / "prompts").freeze("general").json();
    const auto anchor = cd::sample_host_clock();
    CHECK(anchor.status == cd::host_sample_status::ok);
    const auto admission = cd::compute_admission_deadline(anchor.sample, 1);
    CHECK(admission.status == cd::deadline_status::ok);
    auto store = assets->open(directory.path / "d-wall.sqlite");
    const std::string d_id = text(store->admit(d_input), "request_id");
    // One real wait: poll the sampler until the admission boot limit has
    // really passed (bounded 250ms slices with a 75s hard cap).
    bool expired = false;
    for (int slice = 0; slice < 300 && !expired; ++slice) {
        const auto sample = cd::sample_host_clock();
        CHECK(sample.status == cd::host_sample_status::ok);
        expired = cd::compare_time(sample.sample.boot_after, admission.value.boot_limit) >= 0;
        if (!expired) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    CHECK(expired);
    // First new legal reservation after expiry: the future typed deadline
    // stop must leave the attempt count untouched; the old source reserves,
    // so the count check below is the D business RED.
    const auto request = admitted_request(*store, d_id, "d-red-after-expiry");
    const auto actor = store->host_actor(Role::Dispatcher, d_id);
    try {
        (void)store->reserve_attempt(actor, request.binding, fake_reservation(request, 100));
    } catch (const Error&) {
        // future Controls: typed request-wallclock stop before the reservation
    }
    CHECK(number(store->budget(d_id), "attempts") == 0);
    CHECK(store->verify_usage_ledger(d_id).at("consistent").as_bool());
}
} // namespace

int main(int argc, char** argv) {
    try { assets = std::make_unique<fixture::Assets>(argc > 1 ? argv[1] : WLS_SOURCE_DIR); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return test::run({
        {"read-only ledger reconstruction uses one snapshot across a separate writer commit", readonly_ledger_observes_one_snapshot},
        {"completed usage reconciliation releases settlement capacity while missing cache stays reserved", reconciled_usage_releases_settlement_capacity},
        {"Fake and legacy calls retain three-dimensional settlement capacity rules", fake_and_legacy_capacity_keep_original_usage_rules},
        {"disabled, credential authorization and full frozen envelope reject before reservation", disabled_and_complete_envelope},
        {"live profile capacity rejects impossible durable evidence before admission", live_profile_capacity_is_rejected_before_admission},
        {"invalid live routes reject before admission and valid DNS/numeric routes remain exact", live_profile_route_is_rejected_before_admission},
        {"original encoding/options CAS and concurrent one-time durable send claim", frozen_identity_and_one_claim},
        {"reservation and send-claim faults roll back original budget and authority", reservation_and_claim_rollback},
        {"private observation and immutable original settlement faults are atomic", observation_and_original_settlement_rollback},
        {"partial usage, exact replay, version conflict and dimension completion", partial_usage_exact_replay_and_completion},
        {"known zero balances retain explicit unknown cache dimensions", known_zero_does_not_clear_unknown_cache},
        {"contradiction preserves known facts and actual excess cost stops paid admission", contradictory_evidence_and_over_reservation},
        {"reconciliation faults roll back all projections and source bytes stay private", reconcile_atomic_and_private_source},
        {"recovery-only missing response settlement keeps original identity and never resends", recovery_unobserved_is_atomic_and_never_resends},
        {"reservation-only recovery requires explicit no-dispatch proof and original null settlement", recovered_reservation_without_send_has_explicit_host_proof},
        {"terminal scoped reconciliation cannot alter sibling or restore business", terminal_scoped_reconciliation_and_sibling_isolation},
        {"explicit migration preserves original fake snapshots, attempts, settlement and budget", scoped_upgrade_and_fake_bytes_stay_original},
        {"policy and cross-request identity are rechecked before first send", policy_and_identity_fences_before_claim},
        {"W-RED input: exact 4/5 crossing of B=125 leaves no durable budget warning", w_red_budget_warning_missing_after_exact_crossing},
        {"D-RED input: expired one-minute wall deadline still creates a reservation", d_red_expired_wall_deadline_still_reserves},
        {"host verifier, original settlement, Root role and evidence kind remain mandatory", source_verifier_and_role_are_mandatory}
    });
}
