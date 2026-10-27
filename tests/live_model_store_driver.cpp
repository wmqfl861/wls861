// Test-only original Store runner: fixed loopback destination, synthetic bearer,
// disposable database and no business tool dispatch or credential environment.
#include "live_test_support.hpp"

#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>

using namespace wls::s1;
using wls::Role;
namespace fixture = wls::test::live;
using fixture::Object;
using fixture::text;
using fixture::number;
using namespace std::chrono_literals;

namespace {
constexpr std::string_view bearer = "WLS_SYNTHETIC_STORE_BEARER_8E42";
std::string optional_text(const Json& value, std::string_view key, const std::string& fallback = {}) {
    const auto* item = value.as_object().if_contains(key);
    return item ? std::string(item->as_string()) : fallback;
}
bool flag(const Json& value, std::string_view key) {
    const auto* item = value.as_object().if_contains(key); return item && item->as_bool();
}
void wait_marker(const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!std::filesystem::exists(path)) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("fixture marker deadline");
        std::this_thread::sleep_for(2ms);
    }
}
void marker(const std::filesystem::path& path) { std::ofstream output(path); output << "ready\n"; }
void pause(const Json& config, const std::string& request_id, const std::string& stage) {
    std::cout << canonical_json(Object{{"ready", stage}, {"request_id", request_id}}) << std::endl;
    wait_marker(text(config, "continue_marker"));
}
ModelRequest request_for(Store& store, const std::string& request_id) {
    const auto row = store.request(request_id);
    const auto frozen = FrozenRequestConfig::restore(canonical_json(row.at("config_snapshot")), text(row, "config_snapshot_sha256"));
    const auto profile = frozen.model_profile(Role::Dispatcher);
    ContextInput context;
    context.role = Role::Dispatcher; context.model = frozen.resolve(Role::Dispatcher);
    context.adapter_version = text(profile, "adapter_version"); context.protocol_version = text(profile, "protocol");
    context.system_prompt = "Local TLS Store fixture only.";
    context.role_prompt = "Keep the original captured Store identity.";
    context.function_prompt = "Return a bounded note.";
    context.task_contract = canonical_json(store.request_snapshot(request_id));
    context.tools = wls::test::protocol::context().tools;
    ModelRequest request;
    request.call_id = "call-fixture"; request.attempt_id = "attempt-fixture"; request.role = Role::Dispatcher;
    request.role_call_index = 1; request.binding = store.request_binding(request_id);
    request.config_snapshot_id = text(row, "config_snapshot_sha256"); request.model = context.model;
    request.context = compile_structured_context(context, {});
    request.reserved_input_tokens = 1000; request.reserved_output_tokens = 100; request.reserved_cost_micro_usd = 100;
    request.max_output_tokens = 100; request.timeout = 3s;
    return request;
}
AttemptReservation reservation_for(const ModelRequest& request) {
    AttemptReservation reservation;
    reservation.call_id = request.call_id; reservation.attempt_id = request.attempt_id;
    reservation.config_snapshot_sha256 = request.config_snapshot_id; reservation.model_ref = request.model.model;
    reservation.adapter_kind = "openai-responses"; reservation.reserved_input_tokens = request.reserved_input_tokens;
    reservation.reserved_output_tokens = request.reserved_output_tokens;
    reservation.reserved_cost_micro_usd = request.reserved_cost_micro_usd;
    reservation.max_output_tokens = request.max_output_tokens; reservation.request_envelope = to_json(request);
    return reservation;
}
Json settle(Store& store, const ModelRequest& request, const ModelResponse& response) {
    const auto outcome = response.status == ModelResultStatus::Completed ? "COMPLETED" :
        response.status == ModelResultStatus::Interrupted ? "INTERRUPTED" : "ERROR";
    return store.settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
                                outcome, to_json(response.usage), to_json(response));
}
Object summary(Store& store, const std::string& request_id) {
    return Object{{"request_id", request_id}, {"request", store.request(request_id)},
        {"attempt", store.attempt("attempt-fixture")}, {"safe", store.live_attempt("attempt-fixture")},
        {"private", store.live_attempt("attempt-fixture", true)}, {"budget", store.budget(request_id)},
        {"ledger", store.verify_usage_ledger(request_id)}};
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const auto config = parse_json_strict(fixture::read(argv[1]), 16384);
        const fixture::Assets assets(argv[2]);
        const std::filesystem::path database = text(config, "database");
        const auto action = optional_text(config, "action", "run");
        if (action == "recover") {
            const auto request_id = fixture::sql_text(database, "SELECT request_id FROM call_attempts WHERE attempt_id='attempt-fixture'");
            auto store = assets.open(database, StoreRecoveryScope::request(request_id));
            const auto original = store->attempt("attempt-fixture");
            const auto request = model_request_from_json(original.at("request_envelope"));
            const auto root = store->host_actor(Role::Root, request_id);
            const auto before = store->live_attempt(request.attempt_id, true);
            Json settled;
            if (before.at("transport").is_null()) {
                const auto reason = before.at("send_claim").is_null() ? "not_dispatched_after_recovery" : "unobserved_after_recovery";
                settled = store->settle_unobserved_live(root, request.attempt_id, number(original, "state_version"), reason, "recover-fixture");
            } else settled = settle(*store, request, model_response_from_json(before.at("private_observation").at("response")));
            const auto frozen = FrozenRequestConfig::restore(canonical_json(store->request(request_id).at("config_snapshot")), request.config_snapshot_id);
            const auto settings = live_model_settings_from_json(frozen.model_profile(Role::Dispatcher), request.model.model);
            StoreActor old_actor{Role::Dispatcher, text(original, "principal_id"), request_id, {}, {}, request.binding.dispatcher_epoch, 0};
            bool send_forbidden = false;
            try {
                const auto claimed = store->claim_live_send(old_actor, request.attempt_id, 1,
                    encode_responses_request(settings.provider, request, settings.stream), settings.transport, "send:" + request.attempt_id);
                send_forbidden = !claimed.at("new_send_authority").as_bool();
            } catch (const Error&) { send_forbidden = true; }
            CHECK(send_forbidden);
            if (flag(config, "reconcile")) {
                auto usage = fixture::complete_usage();
                std::string source = "loopback_fixture";
                if (before.at("send_claim").is_null()) {
                    usage.input_tokens = 0; usage.output_tokens = 0; usage.cache_read_tokens = 0;
                    usage.cache_write_tokens = 0; usage.cost_micro_usd = 0; source = "host_no_dispatch";
                }
                (void)store->reconcile_usage(root, fixture::evidence(settled, usage, "recovery-proof", source), {"recovery", "recovery", 1});
                CHECK(store->attempt(request.attempt_id) == settled);
            }
            auto result = summary(*store, request_id); result["send_forbidden"] = send_forbidden;
            result["recovered_before"] = before; std::cout << canonical_json(result) << '\n'; return 0;
        }
        const auto port = number(config, "port");
        CHECK(port > 0 && port <= 65535);
        auto live = fixture::settings(fixture::read(text(config, "ca_path")), static_cast<int>(port));
        live.stream = flag(config, "stream");
        auto store = assets.open(database);
        const auto request_id = text(store->admit(fixture::admission(assets, live, "tls-store", !flag(config, "disabled"))), "request_id");
        auto request = request_for(*store, request_id);
        if (const auto* timeout = config.as_object().if_contains("timeout_ms")) request.timeout = std::chrono::milliseconds(timeout->as_int64());
        const auto actor = store->host_actor(Role::Dispatcher, request_id);
        if (flag(config, "admission_fault")) fixture::sql(database,
            "CREATE TRIGGER fail_live_admission BEFORE INSERT ON live_attempt_execution BEGIN SELECT RAISE(ABORT,'injected admission failure'); END;");
        if (action == "run") {
            try {
                const auto executed = execute_live_attempt(*store, actor, request, bearer, {true, false});
                auto result = summary(*store, request_id);
                const auto replay = execute_live_attempt(*store, actor, request, bearer, {true, false});
                result["first_sent"] = executed.sent; result["replay_sent"] = replay.sent;
                result["replay_new_send_authority"] = replay.send_claim.at("new_send_authority");
                if (flag(config, "reconcile")) {
                    (void)store->reconcile_usage(store->host_actor(Role::Root, request_id),
                        fixture::evidence(executed.settlement, fixture::complete_usage(), "tls-response-usage"), {"tls-usage", "tls-usage", 1});
                    result["after_reconcile"] = summary(*store, request_id);
                }
                std::cout << canonical_json(result) << '\n'; return 0;
            } catch (const Error& error) {
                std::cout << canonical_json(Object{{"threw", true}, {"error", error.what()}, {"budget", store->budget(request_id)}}) << '\n'; return 0;
            }
        }
        (void)store->reserve_attempt(actor, request.binding, reservation_for(request));
        if (action == "pause-reservation") pause(config, request_id, "reservation");
        const auto encoded = encode_responses_request(live.provider, request, live.stream);
        const auto claim = store->claim_live_send(actor, request.attempt_id, 1, encoded, live.transport, "send:" + request.attempt_id);
        CHECK(claim.at("new_send_authority").as_bool());
        if (action == "pause-claim") pause(config, request_id, "claim");
        HttpsTransportResult transport;
        if (action == "late") {
            auto pending = std::async(std::launch::async, [&] { return perform_responses_https(live.provider, request, encoded, live.transport, bearer); });
            wait_marker(text(config, "received_marker"));
            const auto mutation = text(config, "mutation");
            const auto root = store->host_actor(Role::Root, request_id);
            if (mutation == "cancel") (void)store->cancel_request(root, number(store->request(request_id), "state_version"), "cancel-fixture");
            else if (mutation == "amend") (void)store->amend(root, number(store->request(request_id), "state_version"),
                "Add an explicit synthetic requirement.", fixture::Array{Object{{"constraint_id", "c1"},
                    {"text", "Preserve the synthetic original requirement."}, {"source_revision", 1}},
                    Object{{"constraint_id", "c2"}, {"text", "Add an explicit synthetic requirement."}, {"source_revision", 2}}}, "amend-fixture");
            else if (mutation == "policy") fixture::sql(database, "UPDATE runtime_policy SET policy_epoch=policy_epoch+1,state_version=state_version+1 WHERE singleton=1;");
            else if (mutation == "recover") { store.reset(); store = assets.open(database, StoreRecoveryScope::request(request_id)); }
            else throw std::runtime_error("unsupported fixture mutation");
            marker(text(config, "release_marker")); transport = pending.get();
        } else transport = perform_responses_https(live.provider, request, encoded, live.transport, bearer);
        if (action == "pause-transport") pause(config, request_id, "transport");
        (void)store->record_live_transport(request.attempt_id, text(claim, "claim_id"), transport);
        if (action == "pause-observation") pause(config, request_id, "observation");
        const auto original = settle(*store, request, transport.response);
        auto result = summary(*store, request_id);
        result["transport_summary"] = https_transport_summary(transport);
        if (flag(config, "reconcile")) {
            (void)store->reconcile_usage(store->host_actor(Role::Root, request_id),
                fixture::evidence(original, fixture::complete_usage(), "late-original-usage"), {"late-usage", "late-usage", 1});
            CHECK(store->attempt(request.attempt_id) == original);
            result["after_reconcile"] = summary(*store, request_id);
        }
        std::cout << canonical_json(result) << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
