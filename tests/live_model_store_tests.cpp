#include "../src/controls_detail.hpp"
#include "../src/correction_detail.hpp"
#include "live_test_support.hpp"
#include "wls/event_runtime.hpp"
#include "wls/single_agent.hpp"
#include "wls/worker_tools.hpp"

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
    // D_FIXTURE repair (FINAL v24 repair record): the wait binds the actual
    // admission's persisted immutable L, and the typed-stop assertion below
    // rejects generic, unrelated or missing errors. Bounded plan against the
    // unchanged 90-second suite bound: setup < 5s, one real wait <= 61.5s in
    // 250ms slices, assertions < 5s.
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
    auto store = assets->open(directory.path / "d-wall.sqlite");
    const std::string d_id = text(store->admit(d_input), "request_id");
    // Bind the persisted marker of the ACTUAL normal admit: its immutable boot
    // identity and the absolute boot limit L this very request froze. A
    // pre-admit independent sample computed an earlier local limit and cannot
    // prove the real request deadline expired.
    const auto marker = store->controls_admission(d_id);
    CHECK(marker.is_object());
    CHECK(text(marker, "format") == "wls-request-controls-v1");
    CHECK(text(marker, "config_sha256") == text(store->request(d_id), "config_snapshot_sha256"));
    const std::string boot_id = text(marker, "boot_id");
    CHECK(!boot_id.empty());
    cd::clock_time persisted_limit{};
    persisted_limit.seconds = number(marker.at("boot_limit").as_object(), "seconds");
    persisted_limit.nanoseconds = static_cast<std::int32_t>(
        number(marker.at("boot_limit").as_object(), "nanoseconds"));
    CHECK(persisted_limit.nanoseconds >= 0 && persisted_limit.nanoseconds <= 999'999'999);
    // One real wait against that same immutable L and boot identity (bounded
    // 250ms slices with a 75s hard cap).
    bool expired = false;
    for (int slice = 0; slice < 300 && !expired; ++slice) {
        const auto sample = cd::sample_host_clock();
        CHECK(sample.status == cd::host_sample_status::ok);
        CHECK(std::string(sample.sample.boot_id_after) == boot_id);
        expired = cd::compare_time(sample.sample.boot_after, persisted_limit) >= 0;
        if (!expired) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    CHECK(expired);
    // First new legal reservation after the same actual L expired: it must be
    // refused by the identifiable typed Controls deadline stop — the existing
    // ErrorCode plus the exact gate classification. A generic/unrelated Error
    // or a missing exception fails this input; attempts stay untouched.
    const auto request = admitted_request(*store, d_id, "d-red-after-expiry");
    const auto actor = store->host_actor(Role::Dispatcher, d_id);
    bool refused = false;
    try {
        (void)store->reserve_attempt(actor, request.binding, fake_reservation(request, 100));
    } catch (const Error& error) {
        refused = true;
        CHECK(error.code() == ErrorCode::NotReady);
        const std::string explanation(error.what());
        CHECK(explanation.find("controls business gate: deadline:") != std::string::npos);
    }
    CHECK(refused);
    CHECK(number(store->budget(d_id), "attempts") == 0);
    CHECK(store->verify_usage_ledger(d_id).at("consistent").as_bool());
}

void f2_present_marker_corruption_is_a_typed_closed_stop() {
    // F2 regression input (FINAL v24 repair record): a PRESENT controls_
    // admission marker that is unparseable, JSON null, a non-object, wrong
    // format, identity-mismatched or range/anchor-invalid is corrupt — a typed
    // closed stop at the direct Store reserve boundary — never silently legacy
    // and never an open gate. Only a truly absent marker row keeps the frozen
    // legacy semantics (covered by the legacy compatibility suites). Every
    // fault below is injected into this test's own throwaway fixture only.
    struct Variant {
        const char* name;
        const char* raw;                 // when set, replaces the stored bytes
        void (*mutate)(Json& marker);    // otherwise mutates the parsed marker
    };
    const Variant variants[] = {
        {"stored bytes are not strict json", "{\"format\":", nullptr},
        {"stored json is null", "null", nullptr},
        {"stored json is not an object", "[1,2,3]", nullptr},
        {"wrong format field", nullptr, [](Json& marker) {
            marker.as_object()["format"] = "wls-request-controls-v2"; }},
        {"missing boot limit", nullptr, [](Json& marker) {
            marker.as_object().erase("boot_limit"); }},
        {"missing boot identity", nullptr, [](Json& marker) {
            marker.as_object().erase("boot_id"); }},
        {"nanoseconds out of range", nullptr, [](Json& marker) {
            marker.at("boot_limit").as_object()["nanoseconds"] = 1'000'000'000; }},
        {"boot limit moved off the anchor", nullptr, [](Json& marker) {
            marker.at("boot_limit").as_object()["seconds"] =
                marker.at("boot_limit").at("seconds").as_int64() + 1; }},
        {"config identity mismatch", nullptr, [](Json& marker) {
            marker.as_object()["config_sha256"] = std::string(64, '0'); }},
        {"stall threshold out of range", nullptr, [](Json& marker) {
            marker.as_object()["stall_same_error_repeats"] = 0; }},
    };
    for (const auto& variant : variants) {
        fixture::Fixture f(*assets, fixture::settings(), false, 410, true);
        auto marker = f.store->controls_admission(f.request_id);
        CHECK(marker.is_object());
        std::string stored;
        if (variant.raw) stored = variant.raw;
        else {
            variant.mutate(marker);
            stored = canonical_json(marker);
        }
        fixture::sql(f.path, "UPDATE events SET result='" + stored +
            "' WHERE request_id='" + f.request_id + "' AND type='controls_admission'");
        const auto gate = f.store->controls_business_gate(f.request_id);
        CHECK(text(gate, "status") == "stop");
        CHECK(text(gate, "stop_class") == "controls_marker_corrupt");
        const auto request = admitted_request(*f.store, f.request_id, "f2-corrupt-marker");
        const auto actor = f.store->host_actor(Role::Dispatcher, f.request_id);
        error(ErrorCode::Corrupt, [&] {
            (void)f.store->reserve_attempt(actor, request.binding, fake_reservation(request, 100));
        });
        CHECK(number(f.store->budget(f.request_id), "attempts") == 0);
        CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
    }
}

void f2_duplicate_marker_rows_are_a_typed_closed_stop() {
    // F2 regression input (FINAL v24 repair02 permanent matrix; NOT_RUN in
    // this source-only stage): a SECOND controls_admission marker row —
    // inserted through the ACTUAL events table columns (event_id, request_id,
    // type, actor, valid JSON payload/result and created_at; command identity
    // columns legally NULL), never a blind partial insert — is a present
    // conflicting identity. The gate must reject it as corrupt instead of
    // silently choosing one row, and the direct Store reserve boundary
    // refuses with the typed Corrupt code while attempts stay untouched.
    fixture::Fixture f(*assets, fixture::settings(), false, 410, true);
    const auto marker = f.store->controls_admission(f.request_id);
    CHECK(marker.is_object());
    const auto stored = canonical_json(marker);
    CHECK(stored.find('\'') == std::string::npos);  // the exact original bytes stay SQL-safe
    fixture::sql(f.path, "INSERT INTO events(event_id,request_id,type,actor,payload,result,created_at) "
        "VALUES ('evt-fixture-second-marker','" + f.request_id +
        "','controls_admission','runtime','{}','" + stored + "','2026-11-10T00:00:00Z')");
    const auto gate = f.store->controls_business_gate(f.request_id);
    CHECK(text(gate, "status") == "stop");
    CHECK(text(gate, "stop_class") == "controls_marker_corrupt");
    const auto request = admitted_request(*f.store, f.request_id, "f2-duplicate-marker");
    const auto actor = f.store->host_actor(Role::Dispatcher, f.request_id);
    error(ErrorCode::Corrupt, [&] {
        (void)f.store->reserve_attempt(actor, request.binding, fake_reservation(request, 100));
    });
    CHECK(number(f.store->budget(f.request_id), "attempts") == 0);
    CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
}

// --- R03_FOLD permanent fixtures (F3_B.1-4 / F3_A.4, all NOT_RUN here) ---
//
// Every case drives the REAL production fold through reserve -> settle ->
// record_tool_feedback -> controls_business_gate on this file's own disposable
// fixtures. SQL is used only to seed faults (a removed/duplicated feedback
// event, a corrupted original binding) on those same throwaway databases.

Json fold_response(const ModelRequest& request, const std::string& body) {
    ModelResponse response;
    response.call_id = request.call_id; response.attempt_id = request.attempt_id;
    response.role_call_index = request.role_call_index; response.role = request.role;
    response.binding = request.binding; response.config_snapshot_id = request.config_snapshot_id;
    response.status = ModelResultStatus::Completed;
    response.text = body;
    response.usage = fixture::complete_usage();
    return to_json(response);
}
// Settle one accepted COMPLETED attempt and record its original trusted
// feedback. accepted==1 here is exactly the R03 defect scenario: accepted is
// the response settlement fact, never host-tool success.
void fold_attempt(fixture::Fixture& f, const std::string& id, const Json& result) {
    const auto request = f.request(id);
    (void)f.reserve(request);
    const auto response = fold_response(request, "fold observation " + id);
    (void)f.store->settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
        "COMPLETED", to_json(fixture::complete_usage()), response);
    (void)f.store->record_tool_feedback(request.attempt_id, request.binding, response, result);
}
Json fold_eligible(const char* reason) {
    const auto message = std::string("fold eligible failure ") + reason;
    return correction_detail::ordinary_failure_feedback(reason, "model_schema",
        message.c_str());
}
Json fold_stagnation(fixture::Fixture& f) {
    const auto gate = f.store->controls_business_gate(f.request_id);
    if (!gate.is_object()) return Json(nullptr);
    const auto* found = gate.as_object().if_contains("stagnation");
    return found && found->is_object() ? *found : Json(nullptr);
}
bool fold_tail(fixture::Fixture& f, const char* key) {
    const auto stagnation = fold_stagnation(f);
    return stagnation.is_object() && stagnation.at(key) == Json(true);
}

void f3_b_consecutive_key_semantics_and_frozen_bound() {
    // F3_B.2: A,A -> 2 (still open, failure tail); the frozen bound N only
    // stops on the Nth CONSECUTIVE same-key failure. A,B,A -> the current key
    // restarts at one, never cumulative historical A. Distinct call/attempt
    // ids never create a new semantic allowance.
    fixture::Fixture f(*assets, fixture::settings(), false, 410, true);
    const auto marker = f.store->controls_admission(f.request_id);
    const auto n = number(marker, "stall_same_error_repeats");
    CHECK(n >= 3);
    for (std::int64_t i = 0; i < n - 1; ++i)
        fold_attempt(f, "fold-a-" + std::to_string(i), fold_eligible("missing_tool_call"));
    {
        const auto gate = f.store->controls_business_gate(f.request_id);
        CHECK(text(gate, "status") == "open");
        CHECK(fold_tail(f, "prior_failure_tail"));
        CHECK(!fold_tail(f, "incomplete_tail"));
    }
    fold_attempt(f, "fold-a-nth", fold_eligible("missing_tool_call"));
    {
        const auto gate = f.store->controls_business_gate(f.request_id);
        CHECK(text(gate, "status") == "stop");
        CHECK(text(gate, "stop_class") == "stagnation");
        CHECK(number(gate.at("stagnation"), "consecutive_eligible_failures") == n);
    }
    // The frozen bound refuses the next reserve/claim with the typed stop.
    {
        const auto request = f.request("fold-after-stagnation");
        bool refused = false;
        try { (void)f.reserve(request); }
        catch (const Error& error) {
            refused = true;
            CHECK(error.code() == ErrorCode::NotReady);
            const std::string explanation(error.what());
            CHECK(explanation.find("stagnation") != std::string::npos);
        }
        CHECK(refused);
        // F2.3 (controls variant): while new business is refused by the
        // stagnation gate, the exact feedback replay of an already-recorded
        // original still returns the identical durable record under its
        // original guard — the stop never blocks read/exact-replay paths.
        {
            const auto replay_request = f.request("fold-a-0");
            const auto gate_before = f.store->controls_business_gate(f.request_id);
            (void)gate_before;
            const auto replayed = f.store->record_tool_feedback(replay_request.attempt_id,
                replay_request.binding, fold_response(replay_request, "fold observation fold-a-0"),
                fold_eligible("missing_tool_call"));
            CHECK(replayed.is_object());
            CHECK(text(replayed, "attempt_id") == replay_request.attempt_id);
            CHECK(text(replayed, "record_sha256") ==
                  text(f.store->tool_feedback(replay_request.attempt_id), "record_sha256"));
        }
    }
    // A,B,A in one frozen scope: the different key restarts the streak, so the
    // same total number of attempts never reaches the bound again.
    fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
    const auto m = number(g.store->controls_admission(g.request_id), "stall_same_error_repeats");
    for (std::int64_t i = 0; i < m - 1; ++i)
        fold_attempt(g, "fold-cum-" + std::to_string(i), fold_eligible("invalid_arguments"));
    fold_attempt(g, "fold-key-change", fold_eligible("missing_tool_call"));
    fold_attempt(g, "fold-cum-back", fold_eligible("invalid_arguments"));
    {
        const auto gate = g.store->controls_business_gate(g.request_id);
        // cumulative historical invalid_arguments would be m; the current key
        // restarted at one, so the gate stays open with a real failure tail.
        CHECK(text(gate, "status") == "open");
        CHECK(fold_tail(g, "prior_failure_tail"));
        CHECK(gate.at("stagnation").at("consecutive_eligible_failures") == 1);
    }
}

struct FoldWorker {
    fixture::Fixture f{*assets, fixture::settings(), false, 410, true};
    SchemaRegistry schemas{assets->source / "schemas"};
    test::TempDir workspace;
    std::unique_ptr<WorkerTools> tools;
    std::string task_id;
    std::int64_t sequence = 0;
    struct Turn { CapturedModelCall call; ModelResponse response; Json feedback; };

    FoldWorker() {
        auto input = fixture::admission(*assets, f.live, "fold-worker-real", false, true);
        input.original_text = fixture::read(assets->source / "tests/fixtures/s13_request.txt");
        input.constraint_catalog = parse_json_strict(fixture::read(assets->source / "tests/fixtures/s13_constraints.json"));
        const auto frozen = FrozenRequestConfig::restore(canonical_json(input.config_snapshot), sha256_hex(canonical_json(input.config_snapshot)));
        const auto grants = frozen.worker_tools();
        input.tool_allowlist.assign(grants.begin(), grants.end());
        f.request_id = text(f.store->admit(input), "request_id");
        tools = std::make_unique<WorkerTools>(*f.store, workspace.path);
        auto setup = event_fake_script_from_json(parse_json_strict(
            fixture::read(assets->source / "tests/fixtures/s15_fake_complete.json")));
        setup.default_steps.resize(1);
        setup.by_node.at("alpha").resize(1);
        EventRuntime runtime(*f.store, schemas, {{"alpha", tools.get()}});
        const auto report = runtime.pump(f.request_id, setup);
        CHECK(report.at("calls").as_array().size() == 2);
        task_id = text(f.store->tasks(f.request_id).front(), "task_id");
        const auto actor = f.store->host_actor(Role::Worker, f.request_id, f.store->binding(task_id).team_id);
        tools->bind(actor);
        CHECK(!fold_tail(f, "incomplete_tail"));
    }
    Turn turn(std::string tool, Json arguments = Object{}, bool text_only = false) {
        FakeModelStep step;
        step.role = Role::Worker; step.role_call_index = 1;
        step.status = ModelResultStatus::Completed; step.usage = fixture::complete_usage();
        if (text_only) step.text = "A real completed Worker response with no tool call.";
        else step.tool_calls.push_back({"fold-worker-" + std::to_string(++sequence), std::move(tool), std::move(arguments)});
        const auto row = f.store->request(f.request_id);
        const auto frozen = FrozenRequestConfig::restore(canonical_json(row.at("config_snapshot")), text(row, "config_snapshot_sha256"));
        FakeModel model({step}, frozen.resolve(Role::Worker).model);
        OfflineRuntime runtime(*f.store, schemas, tools.get());
        auto call = runtime.reserve_call(model, f.request_id, Role::Worker, task_id);
        auto response = model.complete(call.request);
        auto feedback = runtime.accept_response(call, response);
        return {std::move(call), std::move(response), std::move(feedback)};
    }
};

void f3_b_same_scope_success_resets_and_replay_does_not() {
    FoldWorker worker;
    const auto n = number(worker.f.store->controls_admission(worker.f.request_id), "stall_same_error_repeats");
    CHECK(n >= 3);
    for (std::int64_t i = 0; i < n - 1; ++i) (void)worker.turn("", Object{}, true);
    CHECK(fold_tail(worker.f, "prior_failure_tail"));
    const auto success = worker.turn("fs_write", Object{{"path", "fold-success.txt"}, {"content", "actual retained file effect"}});
    CHECK(success.feedback.at("executed_now") == true);
    CHECK(success.feedback.at("replayed") == false);
    CHECK(success.feedback.at("execution_result").at("accepted") == 1);
    CHECK(text(success.feedback.at("operation"), "status") == "SUCCEEDED");
    CHECK(fixture::read(worker.workspace.path / "fold-success.txt") == "actual retained file effect");
    CHECK(!fold_tail(worker.f, "prior_failure_tail"));
    CHECK(!fold_tail(worker.f, "incomplete_tail"));
    (void)worker.turn("", Object{}, true);
    const auto before = fold_stagnation(worker.f);
    // A genuine diagnostic returns an old successful operation without any
    // replay flag; its original DECLARED op_status category still forbids reset.
    const auto diagnostic = worker.turn("op_status", Object{{"op_id", success.feedback.at("operation").at("op_id")}});
    CHECK(diagnostic.feedback.at("operation").at("op_id") == success.feedback.at("operation").at("op_id"));
    CHECK(fold_tail(worker.f, "prior_failure_tail"));
    CHECK(fold_stagnation(worker.f).at("success_witness") == before.at("success_witness"));
    // Exact command/attempt replay returns its durable feedback; no new
    // operation, success witness or allowance is manufactured.
    const auto attempts_before = number(worker.f.store->budget(worker.f.request_id), "attempts");
    const auto replay = OfflineRuntime(*worker.f.store, worker.schemas, worker.tools.get()).accept_response(success.call, success.response);
    CHECK(replay == success.feedback);
    CHECK(number(worker.f.store->budget(worker.f.request_id), "attempts") == attempts_before);
    CHECK(fold_tail(worker.f, "prior_failure_tail"));
    CHECK(fold_stagnation(worker.f).at("success_witness") == before.at("success_witness"));
    for (std::int64_t i = 1; i < n - 1; ++i) (void)worker.turn("", Object{}, true);
    CHECK(text(worker.f.store->controls_business_gate(worker.f.request_id), "status") == "open");
    (void)worker.turn("", Object{}, true);
    CHECK(text(worker.f.store->controls_business_gate(worker.f.request_id), "status") == "stop");
    CHECK(number(fold_stagnation(worker.f), "consecutive_eligible_failures") ==
          number(worker.f.store->controls_admission(worker.f.request_id), "stall_same_error_repeats"));
}

void f3_b_committed_event_order_and_association_faults() {
    // F3_B.3: the fold follows the immutable committed feedback EVENT order,
    // never the reservation rowid; a cross-request event gap (a second
    // request's admission committed between the feedback events) does not
    // fabricate missing history.
    fixture::Fixture f(*assets, fixture::settings(), false, 410, true);
    const auto n = number(f.store->controls_admission(f.request_id), "stall_same_error_repeats");
    // Reserve a1 then a2 (rowid order a1 < a2), but commit a2's feedback
    // event FIRST; the fold must see a2's reason as the opening key.
    const auto a1 = f.request("fold-order-a1");
    const auto a2 = f.request("fold-order-a2");
    (void)f.reserve(a1); (void)f.reserve(a2);
    const auto r1 = fold_response(a1, "order a1");
    const auto r2 = fold_response(a2, "order a2");
    (void)f.store->settle_attempt(a2.attempt_id, a2.binding, a2.config_snapshot_id,
        "COMPLETED", to_json(fixture::complete_usage()), r2);
    (void)f.store->record_tool_feedback(a2.attempt_id, a2.binding, r2, fold_eligible("missing_tool_call"));
    // A second request's admission commits between the two feedback events:
    // a legal cross-request seq gap.
    (void)f.store->admit(fixture::admission(f.assets, f.live, "fold-gap-request"));
    (void)f.store->settle_attempt(a1.attempt_id, a1.binding, a1.config_snapshot_id,
        "COMPLETED", to_json(fixture::complete_usage()), r1);
    (void)f.store->record_tool_feedback(a1.attempt_id, a1.binding, r1, fold_eligible("invalid_arguments"));
    for (std::int64_t i = 0; i + 1 < n; ++i)
        fold_attempt(f, "fold-order-b" + std::to_string(i), fold_eligible("invalid_arguments"));
    // Event order: A(a2) opens, then B(a1) plus n-1 later B reach n
    // consecutive. Rowid order would fold B(a1),A(a2),then n-1 B — only n-1
    // consecutive B — and stay open, so the stop is decisive evidence for the
    // event-seq fold.
    CHECK(text(f.store->controls_business_gate(f.request_id), "status") == "stop");
    CHECK(!fold_tail(f, "incomplete_tail"));
    CHECK(number(f.store->controls_business_gate(f.request_id).at("stagnation"),
                 "consecutive_eligible_failures") == n);

    // F3_A.4: a settled attempt whose original feedback row is missing, whose
    // matching feedback event was removed, whose event association is
    // duplicated, or whose stored original binding is malformed, is incomplete
    // evidence that conservatively closes new business — not a silently
    // skipped row and never a validated fold.
    {
        fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
        const auto request = g.request("fold-missing-feedback");
        (void)g.reserve(request);
        (void)g.store->settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
            "COMPLETED", to_json(fixture::complete_usage()), fold_response(request, "no feedback row"));
        CHECK(fold_tail(g, "incomplete_tail"));
    }
    {
        fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
        fold_attempt(g, "fold-event-removed", fold_eligible("missing_tool_call"));
        CHECK(!fold_tail(g, "incomplete_tail"));
        fixture::sql(g.path, "DELETE FROM events WHERE request_id='" + g.request_id +
            "' AND type='tool_feedback' AND json_extract(payload,'$.attempt_id')='fold-event-removed'");
        CHECK(fold_tail(g, "incomplete_tail"));
    }
    {
        fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
        fold_attempt(g, "fold-event-duplicated", fold_eligible("missing_tool_call"));
        fixture::sql(g.path, "INSERT INTO events(event_id,request_id,type,actor,payload,result,created_at) "
            "VALUES ('evt-fixture-dup-feedback','" + g.request_id + "','tool_feedback','runtime',"
            "'{\"type\":\"tool_feedback\",\"attempt_id\":\"fold-event-duplicated\",\"record_sha256\":\"" +
            std::string(64, '0') + "\"}','{}','2026-11-10T00:00:00Z')");
        CHECK(fixture::sql_text(g.path, "SELECT count(*) FROM events WHERE event_id='evt-fixture-dup-feedback'") == "1");
        CHECK(fold_tail(g, "incomplete_tail"));
    }
    {
        // F3_B.4: the attempt's own immutable original binding is the scope
        // authority; a malformed stored binding is incomplete evidence.
        fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
        fold_attempt(g, "fold-bad-binding", fold_eligible("missing_tool_call"));
        fixture::sql(g.path, "INSERT OR REPLACE INTO model_tool_feedback SELECT attempt_id,'{\"malformed\":true}',"
            "response,result,record_sha256,recorded_at FROM model_tool_feedback WHERE attempt_id='fold-bad-binding'");
        CHECK(fixture::sql_text(g.path, "SELECT original_binding FROM model_tool_feedback WHERE attempt_id='fold-bad-binding'") ==
              "{\"malformed\":true}");
        CHECK(fold_tail(g, "incomplete_tail"));
    }
}


std::string fixture_sql_quote(std::string_view value) {
    std::string encoded = "'";
    for (const char byte : value) { encoded.push_back(byte); if (byte == '\'') encoded.push_back(byte); }
    return encoded + "'";
}
void f3_single_original_association_faults_close_new_business() {
    for (const auto* fault : {"feedback_hash", "feedback_binding", "feedback_result", "feedback_command",
            "settlement_hash", "settlement_binding", "settlement_result", "missing_settlement", "bad_recordhash"}) {
        fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
        fold_attempt(g, "single-original", fold_eligible("missing_tool_call"));
        CHECK(!fold_tail(g, "incomplete_tail"));
        const bool feedback = std::string(fault).starts_with("feedback");
        const auto type = feedback ? "tool_feedback" : "settle_attempt";
        if (std::string(fault) == "bad_recordhash")
            fixture::sql(g.path, "INSERT OR REPLACE INTO model_tool_feedback SELECT attempt_id,original_binding,response,result,'" +
                std::string(64, '0') + "',recorded_at FROM model_tool_feedback WHERE attempt_id='single-original'");
        else if (std::string(fault) == "missing_settlement")
            fixture::sql(g.path, "DELETE FROM events WHERE type='settle_attempt' AND json_extract(payload,'$.attempt_id')='single-original'");
        else {
            const auto condition = " WHERE type=" + fixture_sql_quote(type) + " AND json_extract(payload,'$.attempt_id')='single-original'";
            auto payload = parse_json_strict(fixture::sql_text(g.path, "SELECT payload FROM events" + condition));
            auto result = parse_json_strict(fixture::sql_text(g.path, "SELECT result FROM events" + condition));
            if (std::string(fault) == "feedback_hash") payload.at("record_sha256") = std::string(64, '0');
            if (std::string(fault).ends_with("binding")) payload.at("binding").at("requirements_revision") = 2;
            if (std::string(fault) == "settlement_hash") payload.at("usage").at("cost_micro_usd") = 99;
            if (std::string(fault).ends_with("result")) result.at("attempt_id") = "foreign-original";
            auto command_hash = sha256_hex(canonical_json(payload));
            if (std::string(fault) == "feedback_command") command_hash = std::string(64, '0');
            fixture::sql(g.path, "UPDATE events SET payload=" + fixture_sql_quote(canonical_json(payload)) +
                ",result=" + fixture_sql_quote(canonical_json(result)) + ",command_sha256=" + fixture_sql_quote(command_hash) + condition);
            CHECK(fixture::sql_text(g.path, "SELECT count(*) FROM events" + condition) == "1");
        }
        CHECK(fold_tail(g, "incomplete_tail"));
        const auto gate = g.store->controls_business_gate(g.request_id);
        CHECK(gate.at("status") == "stop" && gate.at("stop_class") == "incomplete_tail");
        const auto used = number(g.store->budget(g.request_id), "attempts");
        error(ErrorCode::NotReady, [&] { (void)g.reserve(g.request("refused-after-single-fault")); });
        CHECK(number(g.store->budget(g.request_id), "attempts") == used);
        g.reopen(StoreRecoveryScope::request(g.request_id));
        CHECK(fold_tail(g, "incomplete_tail"));
        error(ErrorCode::NotReady, [&] { (void)g.reserve(g.request("refused-after-reopen")); });
        CHECK(number(g.store->budget(g.request_id), "attempts") == used);
    }
}

void f3_original_worker_scope_survives_mutable_projection_and_non_effects() {
    FoldWorker worker;
    (void)worker.turn("", Object{}, true);
    const auto original = fold_stagnation(worker.f);
    CHECK(original.at("consecutive_eligible_failures") == 1);
    fixture::sql(worker.f.path, "UPDATE tasks SET node_id='renamed-projection',state_version=state_version+1 WHERE task_id=" +
        fixture_sql_quote(worker.task_id));
    const auto projected = fold_stagnation(worker.f);
    CHECK(projected.at("consecutive_eligible_failures") == 1);
    CHECK(projected.at("success_witness") == original.at("success_witness"));
    CHECK(projected.at("incomplete_tail") == false);
    // Distinct valid flat/nested feedback and replay/non-effect shapes are
    // original durable records, with no current task join used by the fold.
    for (const bool nested : {false, true}) {
        fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
        auto failure = fold_eligible("missing_tool_call");
        fold_attempt(g, "flat-nested-a", nested ? Json(Object{{"correction_feedback", failure}}) : failure);
        fold_attempt(g, "flat-nested-b", nested ? Json(Object{{"correction_feedback", failure}}) : failure);
        CHECK(fold_stagnation(g).at("consecutive_eligible_failures") == 2);
        const auto before = fold_stagnation(g).at("success_witness");
        for (const Json& result : {Json(nullptr), Json(Object{}), Json(Object{{"status", "COMPLETED"}, {"notification", true}}),
                Json(Object{{"executed_now", false}}), Json(Object{{"replayed", true}})})
            fold_attempt(g, new_id("observation"), result);
        CHECK(fold_tail(g, "prior_failure_tail"));
        CHECK(fold_stagnation(g).at("consecutive_eligible_failures") == 2);
        CHECK(fold_stagnation(g).at("success_witness") == before);
    }
}

void f3_original_semantic_revisions_survive_each_current_projection() {
    for (const auto* field : {"requirements_revision", "plan_revision", "task_revision", "instruction_revision"}) {
        FoldWorker worker;
        const auto turn = worker.turn("", Object{}, true);
        const auto before = fold_stagnation(worker.f);
        CHECK(before.at("consecutive_eligible_failures") == 1 && before.at("incomplete_tail") == false);
        const auto original = worker.f.store->attempt(turn.call.request.attempt_id);
        const auto table = std::string_view(field) == "requirements_revision" ? "requests" : "tasks";
        const auto condition = std::string_view(field) == "requirements_revision"
            ? "request_id=" + fixture_sql_quote(worker.f.request_id)
            : "task_id=" + fixture_sql_quote(worker.task_id);
        fixture::sql(worker.f.path, "UPDATE " + std::string(table) + " SET " + field + "=" + field +
            "+1,state_version=state_version+1 WHERE " + condition);
        const auto after = fold_stagnation(worker.f);
        CHECK(after.at("consecutive_eligible_failures") == 1 && after.at("incomplete_tail") == false);
        CHECK(after.at("success_witness") == before.at("success_witness"));
        CHECK(worker.f.store->attempt(turn.call.request.attempt_id) == original);
    }
}

void f3_frozen_stagnation_survives_current_epoch_and_version_changes() {
    for (const auto* mutation : {"dispatcher_epoch", "state_version", "policy_epoch"}) {
        fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
        const auto n = number(g.store->controls_admission(g.request_id), "stall_same_error_repeats");
        for (std::int64_t index = 0; index < n; ++index)
            fold_attempt(g, "epoch-failure-" + std::to_string(index), fold_eligible("invalid_arguments"));
        CHECK(g.store->controls_business_gate(g.request_id).at("stop_class") == "stagnation");
        const auto before = g.store->budget(g.request_id);
        if (std::string_view(mutation) == "policy_epoch")
            fixture::sql(g.path, "UPDATE runtime_policy SET policy_epoch=policy_epoch+1,state_version=state_version+1 WHERE singleton=1");
        else fixture::sql(g.path, "UPDATE requests SET " + std::string(mutation) + "=" + mutation +
            "+1 WHERE request_id=" + fixture_sql_quote(g.request_id));
        CHECK(fold_stagnation(g).at("consecutive_eligible_failures") == n);
        CHECK(g.store->controls_business_gate(g.request_id).at("stop_class") == "stagnation");
        error(ErrorCode::NotReady, [&] { (void)g.reserve(g.request("no-new-semantic-allowance")); });
        CHECK(g.store->budget(g.request_id) == before);
    }
}

void f3_settlement_observer_exempts_only_exact_current_original() {
    fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
    const auto request = g.request("completion-original");
    (void)g.reserve(request);
    const auto response = fold_response(request, "completion observation");
    const auto settled = g.store->settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
        "COMPLETED", to_json(fixture::complete_usage()), response);
    CHECK(g.store->controls_business_gate(g.request_id).at("stop_class") == "incomplete_tail");
    CHECK(g.store->controls_settlement_gate(g.actor(), to_json(request), settled).at("status") == "open");
    {
        auto wrong = request;
        wrong.binding.request_id = "req-foreign";
        test::throws<Error>([&] { (void)g.store->controls_settlement_gate(g.actor(), to_json(wrong), settled); });
        auto stale = settled;
        stale.at("state_version") = stale.at("state_version").as_int64() + 1;
        error(ErrorCode::Conflict, [&] { (void)g.store->controls_settlement_gate(g.actor(), to_json(request), stale); });
        auto wrong_actor = g.actor();
        ++wrong_actor.dispatcher_epoch;
        error(ErrorCode::Stale, [&] { (void)g.store->controls_settlement_gate(wrong_actor, to_json(request), settled); });
    }
    // Make a second settled original without feedback before checking the
    // first one. The observer excludes only its own attempt, so the sibling
    // missing original remains a closed history gate.
    fixture::Fixture h(*assets, fixture::settings(), false, 410, true);
    const auto one = h.request("one-completion"), two = h.request("two-completion");
    (void)h.reserve(one); (void)h.reserve(two);
    const auto settled_one = h.store->settle_attempt(one.attempt_id, one.binding, one.config_snapshot_id,
        "COMPLETED", to_json(fixture::complete_usage()), fold_response(one, "one"));
    (void)h.store->settle_attempt(two.attempt_id, two.binding, two.config_snapshot_id,
        "COMPLETED", to_json(fixture::complete_usage()), fold_response(two, "two"));
    CHECK(h.store->controls_settlement_gate(h.actor(), to_json(one), settled_one).at("stop_class") == "incomplete_tail");
    (void)g.store->record_tool_feedback(request.attempt_id, request.binding, response, fold_eligible("missing_tool_call"));
    CHECK(g.store->controls_business_gate(g.request_id).at("status") == "open");
}

void f2_legacy_absence_keeps_store_and_explicit_pump() {
    fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
    fixture::sql(g.path, "DELETE FROM events WHERE type='controls_admission' AND request_id=" + fixture_sql_quote(g.request_id));
    CHECK(g.store->controls_admission(g.request_id).is_null());
    CHECK(fixture::sql_text(g.path, "SELECT count(*) FROM events WHERE type='controls_admission' AND request_id=" +
        fixture_sql_quote(g.request_id)) == "0");
    const auto before = g.store->budget(g.request_id);
    CHECK(number(before, "attempts") == 0);
    CHECK(g.store->verify_usage_ledger(g.request_id).at("consistent") == true);
    fixture::controls_regression_evidence("legacy-before", Object{{"true_absence", true},
        {"request", g.store->request(g.request_id)}, {"budget", before}}, g.path);
    try {
        CHECK(g.store->controls_business_gate(g.request_id).at("status") == "legacy");
        const auto request = g.request("legacy-explicit-original");
        (void)g.reserve(request);
        CHECK(number(g.store->budget(g.request_id), "attempts") == 1);
        SchemaRegistry schemas(assets->source / "schemas");
        const auto report = EventRuntime(*g.store, schemas).pump(g.request_id, EventFakeScript{});
        CHECK(report.at("calls").as_array().empty());
        CHECK(g.store->controls_business_gate(g.request_id).at("status") == "legacy");
        fixture::controls_regression_evidence("legacy-after", Object{{"gate", g.store->controls_business_gate(g.request_id)},
            {"budget", g.store->budget(g.request_id)}, {"explicit_pump", report}}, g.path);
    } catch (const std::exception& failure) {
        fixture::controls_regression_evidence("legacy-exception", Object{{"exception", failure.what()},
            {"budget_before", before}, {"budget_after", g.store->budget(g.request_id)},
            {"marker", g.store->controls_admission(g.request_id)}}, g.path);
        throw;
    }
}

void f3_settlement_query_requires_exact_original_worker_actor() {
    FoldWorker worker;
    FakeModelStep step;
    step.role = Role::Worker; step.role_call_index = 1;
    step.status = ModelResultStatus::Completed; step.usage = fixture::complete_usage();
    step.text = "Original Worker settlement; no tool execution.";
    const auto request_row = worker.f.store->request(worker.f.request_id);
    const auto frozen = FrozenRequestConfig::restore(canonical_json(request_row.at("config_snapshot")),
        text(request_row, "config_snapshot_sha256"));
    FakeModel model({step}, frozen.resolve(Role::Worker).model);
    OfflineRuntime runtime(*worker.f.store, worker.schemas, worker.tools.get());
    const auto call = runtime.reserve_call(model, worker.f.request_id, Role::Worker, worker.task_id);
    const auto response = model.complete(call.request);
    const auto settled = worker.f.store->settle_attempt(call.request.attempt_id, call.request.binding,
        call.request.config_snapshot_id, "COMPLETED", to_json(response.usage), to_json(response));
    const auto original = worker.f.store->attempt(call.request.attempt_id);
    CHECK(original == settled);
    CHECK(original.at("role") == "worker" && original.at("principal_id") == Json(call.actor.principal_id));
    CHECK(original.at("request_envelope") == to_json(call.request));
    const auto lead = worker.f.store->host_actor(Role::TeamLead, worker.f.request_id, call.actor.team_id);
    CHECK(lead.role == Role::TeamLead && call.actor.role == Role::Worker);
    CHECK(lead.principal_id != call.actor.principal_id);
    CHECK(lead.request_id == call.actor.request_id && lead.task_id == call.actor.task_id &&
        lead.team_id == call.actor.team_id && lead.dispatcher_epoch == call.actor.dispatcher_epoch &&
        lead.execution_epoch == call.actor.execution_epoch);
    CHECK(worker.f.store->controls_settlement_gate(call.actor, to_json(call.request), settled).at("status") == "open");
    auto wrong_principal = call.actor;
    wrong_principal.principal_id += "-not-the-original-worker";
    error(ErrorCode::Unauthorized, [&] {
        (void)worker.f.store->controls_settlement_gate(wrong_principal, to_json(call.request), settled);
    });
    const auto before = worker.f.store->budget(worker.f.request_id);
    const auto sends = fixture::sql_text(worker.f.path, "SELECT count(*) FROM live_send_claims");
    const auto effects = fixture::sql_text(worker.f.path, "SELECT count(*) FROM operations");
    fixture::controls_regression_evidence("actor-before", Object{{"captured_request", to_json(call.request)},
        {"original_settlement", settled}, {"worker_principal", call.actor.principal_id},
        {"authorized_same_team_lead_principal", lead.principal_id}, {"budget", before},
        {"send_claims", sends}, {"operations", effects}}, worker.f.path);
    bool refused = false;
    Json wrong_actor_snapshot = nullptr;
    try { wrong_actor_snapshot = worker.f.store->controls_settlement_gate(lead, to_json(call.request), settled); }
    catch (const Error& failure) { CHECK(failure.code() == ErrorCode::Conflict); refused = true; }
    CHECK(worker.f.store->attempt(call.request.attempt_id) == original);
    CHECK(worker.f.store->budget(worker.f.request_id) == before);
    CHECK(fixture::sql_text(worker.f.path, "SELECT count(*) FROM live_send_claims") == sends);
    CHECK(fixture::sql_text(worker.f.path, "SELECT count(*) FROM operations") == effects);
    fixture::controls_regression_evidence("actor-after", Object{{"refused_exact_actor_mismatch", refused},
        {"wrong_actor_snapshot", wrong_actor_snapshot}, {"budget", before}, {"send_claims", sends},
        {"operations", effects}, {"original_settlement", worker.f.store->attempt(call.request.attempt_id)}}, worker.f.path);
    CHECK(refused);
}

void f2_no_marker_single_agent_preserves_explicit_legacy_pump() {
    fixture::Fixture g(*assets, fixture::settings(), false, 410, true);
    test::TempDir config;
    std::filesystem::copy(assets->source / "config/examples", config.path / "config",
        std::filesystem::copy_options::recursive);
    const auto system_path = config.path / "config/system.yaml";
    auto catalog = load_config_catalog(system_path, config.path / "config/functions", assets->source / "prompts");
    auto system = catalog.freeze("general").json().at("system");
    system.at("context").as_object()["request_format"] = "structured-v2";
    system.at("context").as_object()["logical_agent"] = Object{{"mode", single_agent_contract},
        {"max_attempts", 64}, {"max_history_bytes", 524288}};
    { std::ofstream output(system_path); output << canonical_json(system); CHECK(output.good()); }
    catalog = load_config_catalog(system_path, config.path / "config/functions", assets->source / "prompts");
    auto input = fixture::admission(*assets, g.live, "legacy-single-agent", false, true);
    input.config_snapshot = catalog.freeze("general").json();
    g.request_id = text(g.store->admit(input), "request_id");
    const auto original_config = g.store->request(g.request_id).at("config_snapshot");
    fixture::sql(g.path, "DELETE FROM events WHERE type='controls_admission' AND request_id=" + fixture_sql_quote(g.request_id));
    CHECK(g.store->controls_admission(g.request_id).is_null());
    CHECK(g.store->controls_business_gate(g.request_id).at("status") == "legacy");
    SchemaRegistry schemas(assets->source / "schemas");
    const auto report = SingleAgentRuntime(*g.store, schemas).pump(g.request_id, EventFakeScript{});
    CHECK(report.at("mode") == "single_agent_fake" && report.at("logical_agent_count") == 1);
    CHECK(report.at("calls").as_array().empty());
    CHECK(number(g.store->budget(g.request_id), "attempts") == 0);
    CHECK(g.store->request(g.request_id).at("config_snapshot") == original_config);
    CHECK(g.store->controls_admission(g.request_id).is_null());
    CHECK(g.store->controls_business_gate(g.request_id).at("status") == "legacy");
}

void f2_incomplete_original_rejects_dispatch_but_preserves_cancel() {
    for (const bool registered : {false, true}) {
        FoldWorker worker;
        const auto failure = worker.turn("", Object{}, true);
        const auto binding = worker.f.store->binding(worker.task_id);
        const auto actor = worker.f.store->host_actor(Role::Worker, worker.f.request_id, binding.team_id);
        const auto version = number(worker.f.store->task(worker.task_id), "state_version");
        const auto op = registered
            ? worker.f.store->prepare_tool_operation(actor, binding, version, "unrelated-business", "fs_write",
                Object{{"path", "must-not-be-written.txt"}, {"content", "no effect"}}, "prepare-controlled-dispatch")
            : worker.f.store->prepare_operation(actor, binding, version, "unrelated-note", "note",
                Object{{"text", "no effect"}}, "none", "prepare-controlled-dispatch");
        fixture::sql(worker.f.path, "UPDATE events SET command_sha256='" + std::string(64, '0') +
            "' WHERE type='tool_feedback' AND json_extract(payload,'$.attempt_id')=" + fixture_sql_quote(failure.call.request.attempt_id));
        CHECK(fold_tail(worker.f, "incomplete_tail"));
        const auto op_id = text(op, "op_id");
        error(ErrorCode::NotReady, [&] {
            if (registered) (void)worker.f.store->start_tool_operation(actor, op_id, number(op, "state_version"), "refused-controlled-start");
            else (void)worker.f.store->start_operation(actor, op_id, number(op, "state_version"), "refused-controlled-start");
        });
        CHECK(worker.f.store->operation(op_id).at("status") == "PREPARED");
        CHECK(!std::filesystem::exists(worker.workspace.path / "must-not-be-written.txt"));
        const auto cancelled = worker.f.store->cancel_operation(actor, op_id, number(op, "state_version"), "restore-controlled-cancel");
        CHECK(cancelled.at("status") == "CANCELLED");
        CHECK(worker.f.store->cancel_operation(actor, op_id, number(op, "state_version"), "restore-controlled-cancel") == cancelled);
    }
}

void f2_incomplete_history_refuses_fresh_claim_preserves_original_replays() {
    fixture::Fixture g(*assets);
    const auto original = g.request("live-original"), pending = g.request("live-pending");
    (void)g.reserve(original); (void)g.reserve(pending);
    const auto claim = g.claim(original);
    const auto response = fold_response(original, "original completed live observation");
    fixture::sql(g.path, "INSERT INTO live_transport_observations VALUES ('" + original.attempt_id + "','" +
        text(claim, "claim_id") + "','" + sha256_hex("synthetic original complete observation") +
        "','{\"network_started\":true}'," + fixture_sql_quote(canonical_json(response)) + ",'','','{}','2026-11-10T00:00:00Z')");
    const auto settled = g.store->settle_attempt(original.attempt_id, original.binding, original.config_snapshot_id,
        "COMPLETED", to_json(fixture::complete_usage()), response);
    const auto feedback = g.store->record_tool_feedback(original.attempt_id, original.binding, response, fold_eligible("missing_tool_call"));
    fixture::sql(g.path, "UPDATE events SET command_sha256='" + std::string(64, '0') +
        "' WHERE type='tool_feedback' AND json_extract(payload,'$.attempt_id')='live-original'");
    error(ErrorCode::NotReady, [&] { (void)g.claim(pending); });
    CHECK(g.store->live_attempt(pending.attempt_id).at("send_claim").is_null());
    CHECK(g.claim(original).at("new_send_authority") == false);
    CHECK(g.store->settle_attempt(original.attempt_id, original.binding, original.config_snapshot_id,
        "COMPLETED", to_json(fixture::complete_usage()), response) == settled);
    CHECK(g.store->record_tool_feedback(original.attempt_id, original.binding, response, fold_eligible("missing_tool_call")) == feedback);
    CHECK(number(g.store->budget(g.request_id), "attempts") == 2);
    CHECK(g.store->cancel_request(g.actor(Role::Root), number(g.store->request(g.request_id), "state_version"), "restore-request-cancel").at("status") == "CANCELLED");
}

void f2_incomplete_history_preserves_usage_reconcile_and_request_recovery() {
    fixture::Fixture g(*assets);
    const auto pending = g.request("recovery-pending-original");
    (void)g.reserve(pending);
    const auto original_request = g.request("reconcile-interrupted-original");
    const auto original = g.interrupted(original_request);
    const auto original_bytes = canonical_json(original);
    CHECK(g.store->controls_business_gate(g.request_id).at("stop_class") == "incomplete_tail");
    const auto before = number(g.store->budget(g.request_id), "attempts");
    CHECK(before == 2);
    const auto reconciled = g.store->reconcile_usage(g.actor(Role::Root),
        fixture::evidence(original, fixture::complete_usage(), "closed-gate-restoring-reconcile"),
        {"closed-gate-restoring-reconcile", "closed-gate-restoring-reconcile", 1});
    CHECK(!reconciled.is_null());
    CHECK(g.store->usage_projection(original_request.attempt_id).at("accounting_complete") == true);
    CHECK(canonical_json(g.store->attempt(original_request.attempt_id)) == original_bytes);
    CHECK(number(g.store->budget(g.request_id), "attempts") == before);
    // Accounting completion restores no business authority or feedback.
    CHECK(g.store->controls_business_gate(g.request_id).at("stop_class") == "incomplete_tail");
    const auto stale_actor = g.actor();
    const auto recovered = g.store->recover_request(g.request_id, number(g.store->request(g.request_id), "state_version"));
    CHECK(number(recovered, "dispatcher_epoch") > stale_actor.dispatcher_epoch);
    CHECK(g.store->attempt(pending.attempt_id).at("status") == "UNKNOWN");
    CHECK(canonical_json(g.store->attempt(original_request.attempt_id)) == original_bytes);
    CHECK(number(g.store->budget(g.request_id), "attempts") == before);
    error(ErrorCode::Stale, [&] {
        (void)g.store->controls_settlement_gate(stale_actor, to_json(original_request), original);
    });
    CHECK(g.store->controls_business_gate(g.request_id).at("stop_class") == "incomplete_tail");
    CHECK(fixture::sql_text(g.path, "SELECT count(*) FROM live_send_claims") == "1");
}

LiveModelSettings pricing_settings() {
    auto live = fixture::settings();
    live.provider.pricing.emplace();
    live.provider.pricing->input_micro_usd_per_million_tokens = 1500000;
    live.provider.pricing->output_micro_usd_per_million_tokens = 4000000;
    live.provider.pricing->cache_read_applicable = true;
    live.provider.pricing->cache_read_micro_usd_per_million_tokens = 250000;
    return live;  // cache write stays frozen not-applicable
}
ModelUsage priced_usage() {
    ModelUsage usage; usage.input_tokens = 11; usage.output_tokens = 7; usage.cache_read_tokens = 3;
    return usage;
}
// Reservation above ceil(1.5*1000 + 4*100)=1900 so the priced minimum holds.
ModelRequest priced_request(fixture::Fixture& f, std::string id = "attempt-priced") {
    auto request = f.request(std::move(id));
    request.reserved_cost_micro_usd = 2000;
    return request;
}
// Test-only completed observation inside this fixture's own disposable
// database; the response tuple is the exact one admitted for this attempt.
Json settle_completed(fixture::Fixture& f, const ModelRequest& request, const ModelUsage& usage) {
    (void)f.reserve(request);
    const auto claim = f.claim(request);
    ModelResponse response;
    response.call_id = request.call_id; response.attempt_id = request.attempt_id;
    response.role_call_index = request.role_call_index; response.role = request.role;
    response.binding = request.binding; response.config_snapshot_id = request.config_snapshot_id;
    response.status = ModelResultStatus::Completed;
    response.text = "synthetic priced completion";
    response.usage = usage;
    const auto body = canonical_json(to_json(response));
    fixture::sql(f.path, "INSERT INTO live_transport_observations VALUES ('" + request.attempt_id + "','" +
        text(claim, "claim_id") + "','" + sha256_hex("synthetic-priced-observation") + "','{}','" + body +
        "','','', '{}','2026-01-01T00:00:00Z')");
    return f.store->settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
        "COMPLETED", to_json(usage), to_json(response));
}

void priced_status_observes_one_budget_snapshot_across_late_receipt() {
    fixture::Fixture f(*assets, pricing_settings());
    const auto request = priced_request(f, "interleaved-priced");
    const auto settled = settle_completed(f, request, priced_usage());
    BudgetReadInterleave interleave;
    auto reader = assets->open(f.path, {}, 410, StoreOpenMode::ReadOnly);
    bool committed = false;
    auto late = priced_usage(); late.cost_micro_usd = 60;
    after_budget_read = [&] {
        (void)f.store->reconcile_usage(f.actor(Role::Root),
            fixture::evidence(settled, late, "late"), {"late", "late", 1});
        committed = true;
    };
    const auto observed = reader->live_accounting_status(f.request_id);
    if (budget_read_failure) std::rethrow_exception(budget_read_failure);
    CHECK(committed);
    // One snapshot: the reported summary and its reported basis never mix.
    const auto estimated = number(observed, "estimated_cost_micro_usd");
    const auto reported = number(observed, "reported_cost_micro_usd");
    CHECK(estimated + reported == 41 || estimated + reported == 60);
    CHECK(number(f.store->budget(f.request_id), "spent_cost") == 60);
    const auto refreshed = reader->live_accounting_status(f.request_id);
    CHECK(number(refreshed, "reported_cost_micro_usd") == 60);
    CHECK(number(refreshed, "estimated_cost_micro_usd") == 0);
    CHECK(reader->verify_usage_ledger(f.request_id).at("consistent").as_bool());
}

void estimated_budget_accounting_and_late_actual_evidence() {
    const auto priced = priced_usage();  // 11/7/3 reported, no cache write: estimate 41
    {   // The deterministic estimate completes budget accounting and releases
        // settlement capacity while every original row keeps its null cost.
        fixture::Fixture f(*assets, pricing_settings());
        const auto request = priced_request(f);
        const auto settled = settle_completed(f, request, priced);
        CHECK(settled.at("cost_micro_usd").is_null() && settled.at("cache_write_tokens").is_null());
        CHECK(number(settled, "input_tokens") == 11 && number(settled, "cache_read_tokens") == 3);
        auto budget = f.store->budget(f.request_id);
        CHECK(number(budget, "spent_cost") == 41 && number(budget, "reserved_cost") == 0 &&
              number(budget, "unknown_cost") == 0);
        CHECK(budget.at("accounting_format") == "wls-estimated-budget-v1");
        CHECK(number(budget.at("cost_breakdown").as_object(), "estimated_micro_usd") == 41);
        const auto status = f.store->live_accounting_status(f.request_id);
        CHECK(status.at("ready").as_bool() && number(status, "blocked_count") == 0);
        CHECK(number(status, "estimated_cost_micro_usd") == 41);
        CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 16);
        const auto projection = f.store->usage_projection(request.attempt_id);
        CHECK(!projection.at("accounting_complete").as_bool());
        CHECK(projection.at("budget_accounting_complete").as_bool());
        CHECK(projection.at("estimated_cost").at("amount_micro_usd") == 41);
        const auto& frozen_budget = f.store->budget(f.request_id);
        CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
        f.reopen();  // a reopened or read-only observer cannot change the frozen price
        CHECK(f.store->budget(f.request_id) == frozen_budget);
        auto reader = assets->open(f.path, {}, 410, StoreOpenMode::ReadOnly);
        CHECK(reader->live_accounting_status(f.request_id).at("ready").as_bool());
        CHECK(reader->budget(f.request_id) == frozen_budget);
        CHECK(reader->verify_usage_ledger(f.request_id).at("consistent").as_bool());
        fixture::sql(f.path, "DROP TRIGGER live_execution_no_update;");
        fixture::sql(f.path, "UPDATE live_attempt_execution SET settings=replace(settings,'1500000','1500001');");
        fixture::sql(f.path, "CREATE TRIGGER live_execution_no_update BEFORE UPDATE ON live_attempt_execution "
                             "BEGIN SELECT RAISE(ABORT,'immutable live attempt identity'); END;");
        error(ErrorCode::Corrupt, [&] { (void)f.store->live_accounting_status(f.request_id); });
        error(ErrorCode::Corrupt, [&] { (void)f.store->verify_usage_ledger(f.request_id); });
    }
    for (const auto& [actual, expected, ready] : {std::tuple<std::int64_t, std::int64_t, bool>{41, 41, true},
            {60, 60, true}, {20, 20, true}}) {
        fixture::Fixture f(*assets, pricing_settings());
        const auto request = priced_request(f, "attempt-actual");
        const auto settled = settle_completed(f, request, priced);
        auto confirmed = priced; confirmed.cost_micro_usd = actual;
        const auto result = f.store->reconcile_usage(f.actor(Role::Root),
            fixture::evidence(settled, confirmed, "actual"), {"actual", "actual", 1});
        CHECK(result.at("budget_delta").at("cost").at("spent") == actual - 41);
        CHECK(number(f.store->budget(f.request_id), "spent_cost") == expected);
        CHECK(number(f.store->budget(f.request_id), "unknown_cost") == 0);
        CHECK(f.store->live_accounting_status(f.request_id).at("ready").as_bool() == ready);
        CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
        CHECK(f.store->attempt(request.attempt_id) == settled);
    }
    {   // An actual above the original reservation blocks new paid business.
        fixture::Fixture f(*assets, pricing_settings());
        const auto request = priced_request(f, "attempt-excess");
        const auto settled = settle_completed(f, request, priced);
        auto excess = priced; excess.cost_micro_usd = 2500;
        (void)f.store->reconcile_usage(f.actor(Role::Root), fixture::evidence(settled, excess, "excess"),
            {"excess", "excess", 1});
        const auto status = f.store->live_accounting_status(f.request_id);
        CHECK(!status.at("ready").as_bool() && number(status, "over_reserved_count") == 1);
        error(ErrorCode::UnknownOutcome, [&] { (void)f.reserve(priced_request(f, "excess-blocked")); });
        CHECK(number(f.store->capacity_status(f.request_id), "reserved_control") == 16);
        CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
    }
    {   // A cancelled response never gains an estimate; its reservation stays.
        fixture::Fixture f(*assets, pricing_settings());
        (void)f.interrupted(priced_request(f, "attempt-cancelled"));
        CHECK(number(f.store->budget(f.request_id), "spent_cost") == 0);
        CHECK(number(f.store->budget(f.request_id), "reserved_cost") == 2000);
        CHECK(number(f.store->budget(f.request_id), "unknown_cost") == 2000);
        CHECK(!f.store->live_accounting_status(f.request_id).at("ready").as_bool());
        error(ErrorCode::UnknownOutcome, [&] { (void)f.reserve(priced_request(f, "cancel-blocked")); });
        CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
    }
    {   // A frozen not-applicable count the response reported is a mismatch.
        fixture::Fixture f(*assets, pricing_settings());
        auto mismatched = priced; mismatched.cache_write_tokens = 1;
        const auto settled = settle_completed(f, priced_request(f, "attempt-mismatch"), mismatched);
        CHECK(number(settled, "cache_write_tokens") == 1);  // original bytes stay
        CHECK(number(f.store->budget(f.request_id), "spent_cost") == 0);
        const auto status = f.store->live_accounting_status(f.request_id);
        CHECK(!status.at("ready").as_bool());
        bool mismatch = false;
        for (const auto& item : status.at("blocked_attempts").as_array())
            mismatch = mismatch || (item.at("reason") == "policy_mismatch");
        CHECK(mismatch);
        error(ErrorCode::UnknownOutcome, [&] { (void)f.reserve(priced_request(f, "mismatch-blocked")); });
        CHECK(f.store->verify_usage_ledger(f.request_id).at("consistent").as_bool());
    }
}
} // namespace

int main(int argc, char** argv) {
    std::string regression;
    try {
        const bool source = argc > 1 && std::string_view(argv[1]).substr(0, 2) != "--";
        regression = fixture::controls_regression_options(argc, argv, source ? 2 : 1);
        assets = std::make_unique<fixture::Assets>(source ? argv[1] : WLS_SOURCE_DIR);
        if (!regression.empty() && regression != "legacy_absence" && regression != "settlement_actor")
            throw std::runtime_error("unknown Controls regression selector");
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    if (regression == "legacy_absence") return test::run({{"legacy_absence", f2_legacy_absence_keeps_store_and_explicit_pump}});
    if (regression == "settlement_actor") return test::run({{"settlement_actor", [] {
        f3_settlement_observer_exempts_only_exact_current_original();
        f3_settlement_query_requires_exact_original_worker_actor();
    }}});
    return test::run({
        {"read-only ledger reconstruction uses one snapshot across a separate writer commit", readonly_ledger_observes_one_snapshot},
        {"priced accounting status observes one budget snapshot across a late receipt", priced_status_observes_one_budget_snapshot_across_late_receipt},
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
        {"D-RED input: expired one-minute wall deadline typed-stops the first reservation", d_red_expired_wall_deadline_still_reserves},
        {"F2 input: present corrupt admission marker is a typed closed stop before reservation", f2_present_marker_corruption_is_a_typed_closed_stop},
        {"F2 input: duplicate admission marker rows are a typed closed stop", f2_duplicate_marker_rows_are_a_typed_closed_stop},
        {"F3_B input: consecutive same-key failures reach the frozen bound while A,B,A restarts the current key", f3_b_consecutive_key_semantics_and_frozen_bound},
        {"F3_B input: same-scope trusted host success resets the streak while a replay neither resets nor consumes it", f3_b_same_scope_success_resets_and_replay_does_not},
        {"F3_B/F3_A input: the fold follows committed feedback event order and marks association faults incomplete", f3_b_committed_event_order_and_association_faults},
        {"F3: single original association faults close new business", f3_single_original_association_faults_close_new_business},
        {"F3: original Worker scope survives current projection and non-effects", f3_original_worker_scope_survives_mutable_projection_and_non_effects},
        {"F3: original semantic revisions survive each mutable projection", f3_original_semantic_revisions_survive_each_current_projection},
        {"F3: frozen stagnation survives current epochs and versions", f3_frozen_stagnation_survives_current_epoch_and_version_changes},
        {"F3: settlement observer exempts only exact current original", f3_settlement_observer_exempts_only_exact_current_original},
        {"F3: settlement query requires exact original Worker actor", f3_settlement_query_requires_exact_original_worker_actor},
        {"F2: legacy absence keeps Store and explicit pump", f2_legacy_absence_keeps_store_and_explicit_pump},
        {"F2: no-marker SingleAgent preserves explicit legacy pump", f2_no_marker_single_agent_preserves_explicit_legacy_pump},
        {"F2: incomplete original rejects dispatch but preserves cancel", f2_incomplete_original_rejects_dispatch_but_preserves_cancel},
        {"F2: incomplete history rejects new claim and preserves original replay", f2_incomplete_history_refuses_fresh_claim_preserves_original_replays},
        {"F2: incomplete history preserves reconciliation and recovery", f2_incomplete_history_preserves_usage_reconcile_and_request_recovery},
        {"host verifier, original settlement, Root role and evidence kind remain mandatory", source_verifier_and_role_are_mandatory},
        {"estimated budget accounting completes without actual cost and late evidence adjusts only the difference", estimated_budget_accounting_and_late_actual_evidence}
    });
}
