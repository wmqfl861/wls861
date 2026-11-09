// Test-only original Store runner: fixed loopback destination, synthetic bearer,
// disposable database and no business tool dispatch or credential environment.
#include "live_test_support.hpp"
#include "wls/offline_runtime.hpp"

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
ModelRequest request_for(Store& store, const std::string& request_id,
                          const std::vector<StructuredContextMessage>& messages = {}) {
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
    request.context = compile_structured_context(context, messages);
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
        // Compatibility profile is applied through the public JSON boundary
        // only; the frozen admission then carries the same original profile.
        if (const auto* compat = config.as_object().if_contains("compat_profile")) {
            auto provider_document = to_json(live.provider);
            provider_document.as_object()["model"] = "o3";
            provider_document.as_object()["responses_compatibility"] =
                parse_json_strict(fixture::read(std::string(compat->as_string())));
            live.provider = provider_profile_from_json(provider_document);
        }
        auto store = assets.open(database);
        // An optional frozen utf8-token-v1 readback selection rides the same
        // public admission boundary; it changes no other live behavior here.
        Json readback_selection = nullptr;
        if (flag(config, "readback"))
            readback_selection = Object{{"readback", Object{{"mode", "utf8-token-v1"},
                {"token_estimator", "utf8-byte-v1"}, {"max_page_tokens", 24576}}},
                {"worker_history", Object{{"mode", "window-v1"}, {"recent_attempts", 3},
                    {"inline_bytes", 65536}}}};
        const auto request_id = text(store->admit(fixture::admission(assets, live, "tls-store",
            !flag(config, "disabled"), false, readback_selection)), "request_id");
        auto request = request_for(*store, request_id);
        if (const auto* timeout = config.as_object().if_contains("timeout_ms")) request.timeout = std::chrono::milliseconds(timeout->as_int64());
        const auto actor = store->host_actor(Role::Dispatcher, request_id);
        if (flag(config, "admission_fault")) fixture::sql(database,
            "CREATE TRIGGER fail_live_admission BEFORE INSERT ON live_attempt_execution BEGIN SELECT RAISE(ABORT,'injected admission failure'); END;");
        if (action == "compat-two-round") {
            const auto trace = [&config](const char* stage) {
                if (config.as_object().contains("trace"))
                    std::cerr << "compat-two-round:" << stage << '\n';
            };
            // Round 1: reasoning + tool over loopback TLS, settled with the
            // original opaque output; feedback makes it a native round.
            trace("begin");
            const auto first = execute_live_attempt(*store, actor, request, bearer, {true, false});
            trace("round1");
            CHECK(first.sent && first.response_accepted);
            auto record = store->attempt(request.attempt_id);
            trace("attempt");
            (void)store->record_tool_feedback(request.attempt_id, request.binding,
                record.at("response"), Object{{"ok", true}, {"round", 1}});
            trace("feedback");
            if (flag(config, "reconcile")) {
                // Unknown cost blocks the next paid round by design; resolve it
                // with the independent loopback usage verifier before round 2.
                (void)store->reconcile_usage(store->host_actor(Role::Root, request_id),
                    fixture::evidence(first.settlement, fixture::complete_usage(11, 7), "compat-round-1"),
                    {"compat-round-1", "compat-round-1", 1});
                trace("reconcile");
            }
            record.as_object()["feedback"] = store->tool_feedback(request.attempt_id);
            const auto history = structured_attempt_history({record});
            trace("history");
            auto second = request_for(*store, request_id, history);
            trace("request2");
            second.call_id = "call-fixture-2"; second.attempt_id = "attempt-fixture-2"; second.role_call_index = 2;
            const auto before_reopen = encode_responses_request(live.provider, second, false);
            trace("encode2");
            if (flag(config, "reopen")) {
                store.reset();
                store = assets.open(database);
                trace("reopen");
                auto reopened = store->attempt(request.attempt_id);
                reopened.as_object()["feedback"] = store->tool_feedback(request.attempt_id);
                auto reopened_history = structured_attempt_history({reopened});
                auto rebuilt = request_for(*store, request_id, reopened_history);
                rebuilt.call_id = second.call_id; rebuilt.attempt_id = second.attempt_id;
                rebuilt.role_call_index = second.role_call_index;
                const auto after_reopen = encode_responses_request(live.provider, rebuilt, false);
                trace("reencode2");
                CHECK(after_reopen.body == before_reopen.body);
                // A takeover reopen advances the dispatch epoch; continue with
                // the reopened store's own current binding, not a stale copy.
                second = rebuilt;
            }
            const auto second_actor = store->host_actor(Role::Dispatcher, request_id);
            trace("actor2");
            const auto executed = execute_live_attempt(*store, second_actor, second, bearer, {true, false});
            trace("round2");
            CHECK(executed.sent);
            Object result{{"request_id", request_id}, {"first_settlement", first.settlement},
                {"second_settlement", executed.settlement},
                {"first_response", store->attempt(request.attempt_id).at("response")},
                {"second_response", store->attempt(second.attempt_id).at("response")},
                {"second_request_body", before_reopen.body}};
            std::cout << canonical_json(result) << '\n'; return 0;
        }
        if (action == "compat-native-two-reopen-third") {
            // Two distinct accepted native tool rounds (A then B) persisted
            // through the real loopback TLS path and immutable feedback, one
            // Store close/reopen, then a real third request whose captured
            // wire body must carry both opaque originals in original order.
            const auto snapshot = [](Store& open, const std::string& id) {
                return Object{{"attempt", open.attempt(id)}, {"feedback", open.tool_feedback(id)}};
            };
            const auto first = execute_live_attempt(*store, actor, request, bearer, {true, false});
            CHECK(first.sent && first.response_accepted);
            auto first_record = store->attempt(request.attempt_id);
            (void)store->record_tool_feedback(request.attempt_id, request.binding,
                first_record.at("response"), Object{{"ok", true}, {"round", 1}, {"result", "tool-result-A"}});
            // Unknown cost blocks the next paid round by design; resolve each
            // round with the independent loopback usage verifier, not a waiver.
            (void)store->reconcile_usage(store->host_actor(Role::Root, request_id),
                fixture::evidence(first.settlement, fixture::complete_usage(11, 7), "compat-native-a"),
                {"compat-native-a", "compat-native-a", 1});
            first_record.as_object()["feedback"] = store->tool_feedback(request.attempt_id);
            auto second = request_for(*store, request_id, structured_attempt_history({first_record}));
            second.call_id = "call-fixture-2"; second.attempt_id = "attempt-fixture-2"; second.role_call_index = 2;
            const auto executed = execute_live_attempt(*store, store->host_actor(Role::Dispatcher, request_id),
                                                       second, bearer, {true, false});
            CHECK(executed.sent && executed.response_accepted);
            auto second_record = store->attempt(second.attempt_id);
            (void)store->record_tool_feedback(second.attempt_id, second.binding,
                second_record.at("response"), Object{{"ok", true}, {"round", 2}, {"result", "tool-result-B"}});
            (void)store->reconcile_usage(store->host_actor(Role::Root, request_id),
                fixture::evidence(executed.settlement, fixture::complete_usage(11, 7), "compat-native-b"),
                {"compat-native-b", "compat-native-b", 1});
            const Object before_reopen{{"first", snapshot(*store, request.attempt_id)},
                {"second", snapshot(*store, second.attempt_id)}};
            store.reset();
            store = assets.open(database);
            const Object after_reopen{{"first", snapshot(*store, request.attempt_id)},
                {"second", snapshot(*store, second.attempt_id)}};
            CHECK(after_reopen == before_reopen);
            // Continue only with the reopened store's own current binding.
            auto first_reopened = store->attempt(request.attempt_id);
            first_reopened.as_object()["feedback"] = store->tool_feedback(request.attempt_id);
            auto second_reopened = store->attempt(second.attempt_id);
            second_reopened.as_object()["feedback"] = store->tool_feedback(second.attempt_id);
            auto third = request_for(*store, request_id,
                structured_attempt_history({first_reopened, second_reopened}));
            third.call_id = "call-fixture-3"; third.attempt_id = "attempt-fixture-3"; third.role_call_index = 3;
            const auto third_encoded = encode_responses_request(live.provider, third, false);
            const auto closing = execute_live_attempt(*store, store->host_actor(Role::Dispatcher, request_id),
                                                       third, bearer, {true, false});
            CHECK(closing.sent && closing.response_accepted);
            Object result{{"request_id", request_id}, {"first_settlement", first.settlement},
                {"second_settlement", executed.settlement}, {"third_settlement", closing.settlement},
                {"before_reopen", before_reopen}, {"after_reopen", after_reopen},
                {"first_response", store->attempt(request.attempt_id).at("response")},
                {"second_response", store->attempt(second.attempt_id).at("response")},
                {"third_response", store->attempt(third.attempt_id).at("response")},
                {"third_request_body", third_encoded.body}};
            std::cout << canonical_json(result) << '\n'; return 0;
        }
        if (action == "readback-opaque") {
            // Frozen utf8-token-v1 readback + worker window + responses
            // compatibility in one admission. The opaque continuation request
            // is produced through the production capture/reservation/feedback
            // path over real loopback TLS; reservations are the measured
            // capture values, never hand-built constants.
            const Json selection = Object{{"readback", Object{{"mode", "utf8-token-v1"},
                {"token_estimator", "utf8-byte-v1"}, {"max_page_tokens", 24576}}},
                {"worker_history", Object{{"mode", "window-v1"}, {"recent_attempts", 3},
                    {"inline_bytes", 65536}}}};
            const auto input_budget = config.as_object().contains("input_budget")
                ? number(config, "input_budget") : std::int64_t(-1);
            const auto output_budget = config.as_object().contains("output_budget")
                ? number(config, "output_budget") : std::int64_t(-1);
            auto admitted = fixture::admission(assets, live, "readback-opaque",
                !flag(config, "disabled"), false, selection, input_budget, output_budget);
            admitted.original_text = "Readback opaque continuation fixture; preserve originals.";
            admitted.constraint_catalog = fixture::Array{Object{{"constraint_id", "c1"},
                {"text", admitted.original_text}, {"source_revision", 1}}};
            const auto readback_request = text(store->admit(admitted), "request_id");
            SchemaRegistry schemas(assets.source / "schemas");
            // The worker window and its readback controls need a durable task.
            const Json brief = Object{{"goal", admitted.original_text}, {"constraint_refs", fixture::Array{"c1"}},
                {"constraints_verbatim", fixture::Array{admitted.original_text}}};
            (void)store->apply_independent_plan(store->host_actor(Role::Dispatcher, readback_request),
                number(store->request(readback_request), "state_version"), 1,
                Object{{"nodes", fixture::Array{Object{{"node_id", "opaque-readback"}, {"kind", "team_task"},
                    {"required", true}, {"depends_on", fixture::Array{}}, {"brief", brief}}}}},
                Object{}, "readback-opaque-plan");
            const auto task_id = text(store->tasks(readback_request).front(), "task_id");
            const auto team = store->binding(task_id).team_id;
            (void)store->issue_instruction(store->host_actor(Role::TeamLead, readback_request, team),
                store->binding(task_id), number(store->task(task_id), "state_version"),
                brief, "readback-opaque-order");
            (void)store->acknowledge(store->host_actor(Role::Worker, readback_request, team),
                store->binding(task_id), number(store->task(task_id), "state_version"), "readback-opaque-ack");
            OfflineRuntime runtime(*store, schemas);
            Object report{{"request_id", readback_request}};
            if (flag(config, "quota")) {
                // Deterministic no-progress Quota: the frozen input budget
                // cannot carry even the first envelope, so nothing may be
                // attempted, reserved, claimed or sent.
                bool quota = false;
                try {
                    const auto call = runtime.capture_call(readback_request, Role::Worker, task_id, 1);
                    const auto executed = execute_live_attempt(*store, call.actor, call.request, bearer, {true, false});
                    CHECK(executed.sent);  // a send here contradicts the frozen budget
                } catch (const Error& error) { quota = error.code() == ErrorCode::Quota; }
                CHECK(quota);
                report["quota"] = quota;
                report["budget"] = store->budget(readback_request);
                report["send_claims"] = fixture::sql_text(database, "SELECT count(*) FROM live_send_claims");
                std::cout << canonical_json(report) << '\n';
                return 0;
            }
            // Round 1: one native reasoning+tool round over loopback TLS,
            // settled and fed back so it becomes a native window round.
            const auto first = runtime.capture_call(readback_request, Role::Worker, task_id, 1);
            report["first_envelope_bytes"] = static_cast<std::int64_t>(first.request.context.prompt().size());
            const auto executed = execute_live_attempt(*store, first.actor, first.request, bearer, {true, false});
            CHECK(executed.sent && executed.response_accepted);
            auto first_record = store->attempt(first.request.attempt_id);
            (void)store->record_tool_feedback(first.request.attempt_id, first.request.binding,
                first_record.at("response"), Object{{"ok", true}, {"round", 1}});
            (void)store->reconcile_usage(store->host_actor(Role::Root, readback_request),
                fixture::evidence(executed.settlement, fixture::complete_usage(11, 7), "readback-opaque-1"),
                {"readback-opaque-1", "readback-opaque-1", 1});
            // Round 2 through the same production capture path: the window now
            // carries the native round with its complete opaque original, and
            // the full next request bytes are the encode of that capture.
            const auto second = runtime.capture_call(readback_request, Role::Worker, task_id, 2);
            report["second_envelope_bytes"] = static_cast<std::int64_t>(second.request.context.prompt().size());
            report["second_logical_messages"] = second.request.context.logical_input.at("messages");
            report["second_request_body"] = encode_responses_request(live.provider, second.request, false).body;
            const auto closing = execute_live_attempt(*store, second.actor, second.request, bearer, {true, false});
            CHECK(closing.sent && closing.response_accepted);
            report["first_settlement"] = executed.settlement;
            report["second_settlement"] = closing.settlement;
            report["first_response"] = store->attempt(first.request.attempt_id).at("response");
            report["second_response"] = store->attempt(second.request.attempt_id).at("response");
            report["budget"] = store->budget(readback_request);
            std::cout << canonical_json(report) << '\n';
            return 0;
        }
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
