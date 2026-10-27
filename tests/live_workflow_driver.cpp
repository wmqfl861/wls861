// Synthetic loopback workflow host. No external endpoint, environment credential,
// provider spending, weak sandbox or fabricated production billing is used.
#include "live_test_support.hpp"
#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace wls::s1;
using wls::Role;
namespace fixture = wls::test::live;
using fixture::Array;
using fixture::Object;
using fixture::number;
using fixture::text;
using namespace std::chrono_literals;

namespace {
constexpr std::string_view bearer = "WLS_SYNTHETIC_WORKFLOW_BEARER_7C91";
bool flag(const Json& value, std::string_view key) {
    const auto* item = value.as_object().if_contains(key); return item && item->as_bool();
}
std::string string_option(const Json& value, std::string_view key, std::string fallback = {}) {
    const auto* item = value.as_object().if_contains(key); return item ? std::string(item->as_string()) : std::move(fallback);
}
std::int64_t number_option(const Json& value, std::string_view key, std::int64_t fallback) {
    const auto* item = value.as_object().if_contains(key); return item ? item->as_int64() : fallback;
}
Json accounting_cases(const fixture::Assets& assets) {
    fixture::Fixture value(assets);
    Object result{{"empty", value.store->live_accounting_status(value.request_id)}};
    const auto request = value.request();
    (void)value.reserve(request);
    result["pending"] = value.store->live_accounting_status(value.request_id);
    const auto send = value.claim(request);
    const auto response = value.cancelled(request);
    (void)value.store->record_live_transport(request.attempt_id, text(send, "claim_id"), response);
    const auto settled = value.settle(request, response);
    result["unknown"] = value.store->live_accounting_status(value.request_id);
    ModelUsage zeros; zeros.input_tokens = 0; zeros.output_tokens = 0; zeros.cost_micro_usd = 0;
    (void)value.store->reconcile_usage(value.actor(Role::Root), fixture::evidence(settled, zeros, "PRIVATE_FIXTURE_SOURCE"), {"partial", "partial", 1});
    result["cache_unknown"] = value.store->live_accounting_status(value.request_id);
    result["zero_budget"] = value.store->budget(value.request_id);
    zeros.cache_read_tokens = 0; zeros.cache_write_tokens = 0;
    (void)value.store->reconcile_usage(value.actor(Role::Root), fixture::evidence(settled, zeros, "complete"), {"complete", "complete", 2});
    result["complete"] = value.store->live_accounting_status(value.request_id);
    auto conflict = zeros; conflict.input_tokens = 1;
    (void)value.store->reconcile_usage(value.actor(Role::Root), fixture::evidence(settled, conflict, "conflict"), {"conflict", "conflict", 3});
    result["disputed"] = value.store->live_accounting_status(value.request_id);
    CHECK(value.store->attempt(request.attempt_id) == settled);
    CHECK(value.store->verify_usage_ledger(value.request_id).at("consistent").as_bool());
    fixture::Fixture excess(assets);
    const auto other = excess.request(); const auto original = excess.interrupted(other);
    (void)excess.store->reconcile_usage(excess.actor(Role::Root), fixture::evidence(original, fixture::complete_usage(1200, 150, 12000)), {"excess", "excess", 1});
    result["over_reserved"] = excess.store->live_accounting_status(excess.request_id);
    CHECK(excess.store->verify_usage_ledger(excess.request_id).at("consistent").as_bool());
    return result;
}
VerifiedUsageEvidence receipt(const Json& config, const LiveModelSettings& live,
                             const ModelRequest& request, const ModelResponse& response, const Json& settlement) {
    CHECK(response.tool_calls.size() == 1);
    const auto key = response.tool_calls.front().call_id;
    CHECK(!key.empty() && key.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos);
    const auto independent = parse_json_strict(read_runtime_file(
        std::filesystem::path(text(config, "receipts")) / (key + ".json"), 16384));
    // The separate loopback server persisted this receipt before sending its
    // response. The hook validates its observed request bytes and response key;
    // it does not reinterpret missing protocol usage as zero or a price guess.
    CHECK(independent.at("fixture") == "loopback-workflow-billing-v1");
    CHECK(text(independent, "call_id") == key);
    CHECK(text(independent, "request_body_sha256") == encode_responses_request(live.provider, request, live.stream).body_sha256);
    auto usage = model_usage_from_json(independent.at("usage"));
    if (flag(config, "partial_evidence")) usage.cache_write_tokens.reset();
    Object statement{{"independent_receipt", independent},
        {"attempt_id", request.attempt_id}, {"settlement_sha256", settlement.at("settlement_sha256")},
        {"usage", to_json(usage)}};
    if (flag(config, "wrong_evidence_attempt")) statement["attempt_id"] = "attempt-other-original";
    if (flag(config, "wrong_evidence_settlement")) statement["settlement_sha256"] = std::string(64, '0');
    const auto proof = canonical_json(statement);
    return verify_usage_evidence("loopback_fixture", "private-server-receipt:" + key, proof,
        [](std::string_view bytes) {
            const auto value = parse_json_strict(bytes);
            return UsageEvidenceStatement{text(value, "attempt_id"), text(value, "settlement_sha256"), model_usage_from_json(value.at("usage"))};
        });
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || ::geteuid() == 0) return 2;
    try {
        const auto config = parse_json_strict(read_runtime_file(argv[1], 65536));
        const fixture::Assets assets(argv[2]);
        if (string_option(config, "action") == "accounting") {
            std::cout << canonical_json(accounting_cases(assets)) << '\n'; return 0;
        }
        const auto port = number(config, "port"); CHECK(port > 0 && port <= 65535);
        auto live = fixture::settings(fixture::read(text(config, "ca_path")), static_cast<int>(port));
        auto store = assets.open(text(config, "database"));
        auto input = fixture::admission(assets, live, "workflow", !flag(config, "disabled"));
        input.original_text = string_option(config, "material", "Complete a synthetic loopback workflow.");
        input.constraint_catalog = Array{};
        const auto frozen = FrozenRequestConfig::restore(canonical_json(input.config_snapshot), sha256_hex(canonical_json(input.config_snapshot)));
        const auto grants = frozen.worker_tools(); input.tool_allowlist.assign(grants.begin(), grants.end());
        input.budget = BudgetLimits{64, 4000000, 1000000, 100000};
        const auto id = text(store->admit(input), "request_id");
        SchemaRegistry schemas(assets.source / "schemas");
        std::map<std::string, std::unique_ptr<WorkerTools>> owned;
        std::map<std::string, WorkerTools*> mapping;
        for (const auto& node : {"alpha", "beta"}) {
            const auto path = std::filesystem::path(text(config, "workspaces")) / node;
            std::filesystem::create_directory(path); CHECK(::chmod(path.c_str(), 0700) == 0);
            owned[node] = std::make_unique<WorkerTools>(*store, path);
            mapping[node] = owned.at(node).get();
        }
        EventRuntime runtime(*store, schemas, mapping);
        std::int64_t credentials = 0, evidence_calls = 0;
        LiveWorkflowOptions options;
        options.authorization = {!flag(config, "unauthorized"), false};
        options.reserved_cost_micro_usd = number_option(config, "reserved_cost", 100);
        // This is the synthetic host's explicit batch budget, not a production
        // default. Sanitizers need time for the same original capture/ledger
        // work; correctness scenarios must not accidentally become deadlines.
        options.max_batch_duration = std::chrono::milliseconds(number_option(config, "timeout_ms", 60000));
        options.limits.max_calls = number_option(config, "max_calls", 32);
        options.limits.max_worker_calls_per_task = number_option(config, "worker_calls", 16);
        options.credentials = [&](std::string_view name) {
            ++credentials; CHECK(name == live.provider.api_key_env);
            if (flag(config, "resolver_failure")) throw std::runtime_error("PRIVATE_RESOLVER_FAILURE_7C91");
            return std::string(bearer);
        };
        if (flag(config, "reconcile")) options.usage_evidence = [&](const ModelRequest& request, const ModelResponse& response, const Json& settlement) {
            ++evidence_calls; return std::optional<VerifiedUsageEvidence>(receipt(config, live, request, response, settlement));
        };
        if (flag(config, "host_dispositions")) options.limits.amendment_dispositions = Object{{"alpha", Object{{"disposition", "keep"}, {"rationale", "fixture refusal"}}}};
        Object result{{"request_id", id}, {"initial_accounting", store->live_accounting_status(id)}};
        try {
            const auto action = string_option(config, "action", "pump");
            if (action == "replay" || action == "settled_without_feedback") {
                OfflineRuntime original(*store, schemas);
                const auto call = original.capture_call(id, Role::Dispatcher, {}, 1, {}, false, {100, 3s});
                const auto observed = observe_live_attempt(*store, call.actor, call.request, bearer, {true, false});
                CHECK(observed.response.has_value());
                std::int64_t hook_calls = 0;
                const auto hook = [&](const Json& settlement) {
                    ++hook_calls;
                    (void)store->reconcile_usage(store->host_actor(Role::Root, id), receipt(config, live, call.request, *observed.response, settlement), {"original", "original", 1});
                };
                if (action == "settled_without_feedback") {
                    (void)store->settle_attempt(call.request.attempt_id, call.request.binding, call.request.config_snapshot_id,
                        "COMPLETED", to_json(observed.response->usage), to_json(*observed.response));
                    bool refused = false;
                    try { (void)original.accept_live_response(call, *observed.response, hook); }
                    catch (const Error& error) { refused = error.code() == ErrorCode::NotReady; }
                    CHECK(refused); result["refused_missing_feedback"] = refused;
                } else {
                    const auto first = original.accept_live_response(call, *observed.response, hook);
                    const auto feedback = store->tool_feedback(call.request.attempt_id);
                    const auto count = fixture::sql_text(text(config, "database"), "SELECT count(*) FROM events");
                    const auto repeated = original.accept_live_response(call, *observed.response, hook);
                    CHECK(first == repeated && store->tool_feedback(call.request.attempt_id) == feedback);
                    CHECK(count == fixture::sql_text(text(config, "database"), "SELECT count(*) FROM events"));
                    result["replayed_original_feedback"] = true;
                }
                const auto repeated_send = observe_live_attempt(*store, call.actor, call.request, bearer, {true, false});
                CHECK(!repeated_send.sent && !repeated_send.response.has_value());
                result["hook_calls"] = hook_calls; result["replay_sent"] = repeated_send.sent;
            } else {
                std::stop_source stopped; if (flag(config, "pre_cancel")) stopped.request_stop();
                bool cancellation_marker_observed = false, cancellation_marker_error = false;
                std::jthread cancellation;
                const auto cancel_marker = string_option(config, "cancel_marker");
                if (!cancel_marker.empty()) {
                    cancellation = std::jthread([&, cancel_marker](std::stop_token watcher_stop) {
                        const auto deadline = std::chrono::steady_clock::now() + 30s;
                        while (!watcher_stop.stop_requested()) {
                            std::error_code error;
                            if (std::filesystem::exists(cancel_marker, error)) {
                                cancellation_marker_observed = true;
                                stopped.request_stop(); return;
                            }
                            if (error || std::chrono::steady_clock::now() >= deadline) {
                                cancellation_marker_error = true;
                                stopped.request_stop(); return;
                            }
                            std::this_thread::sleep_for(2ms);
                        }
                    });
                }
                Array reports;
                const auto* phases = config.as_object().if_contains("phases");
                const auto schedule = phases ? *phases : Json(Array{Object{}});
                for (const auto& phase : schedule.as_array()) {
                    if (flag(phase, "amend")) (void)store->amend(store->host_actor(Role::Root, id), number(store->request(id), "state_version"),
                        "Apply the explicit synthetic AMEND requirement.", Array{}, "owner-amend");
                    options.limits.max_calls = number_option(phase, "max_calls", number_option(config, "max_calls", 32));
                    reports.push_back(runtime.pump_live(id, options, stopped.get_token()));
                    if (flag(phase, "host_submit_alpha")) {
                        // A legitimate trusted-host submission/check is
                        // deliberately not a model call. This constructs the
                        // cross-task provenance counterexample without
                        // deleting attempts or changing any ledger history.
                        std::string task_id;
                        for (const auto& task : store->tasks(id))
                            if (task.at("node_id") == "alpha") task_id = text(task, "task_id");
                        CHECK(!task_id.empty());
                        const auto binding = store->binding(task_id);
                        const auto worker = store->host_actor(Role::Worker, id, binding.team_id);
                        const auto bytes = owned.at("alpha")->diagnostic(worker, binding, "fs_read", Object{{"path", "result.txt"}});
                        CHECK(bytes.at("eof").as_bool() && bytes.at("bytes_returned") == bytes.at("size_bytes"));
                        const Array artifact{Object{{"path", "/work/result.txt"}, {"sha256", bytes.at("sha256")},
                            {"size_bytes", bytes.at("size_bytes")}, {"content_base64", bytes.at("content_base64")}}};
                        const auto submission = store->submit_work(worker, binding, number(store->task(task_id), "state_version"),
                            config.at("host_submission"), artifact, "host-alpha-submit");
                        (void)store->check_submission(store->host_actor(Role::TeamLead, id, binding.team_id), binding,
                            number(store->task(task_id), "state_version"), text(submission, "submission_id"),
                            number(submission, "state_version"), config.at("host_check"), nullptr, "host-alpha-check");
                        result["host_submission_id"] = submission.at("submission_id");
                    }
                }
                if (cancellation.joinable()) {
                    cancellation.request_stop(); cancellation.join();
                    result["cancellation_marker_observed"] = cancellation_marker_observed;
                    result["cancellation_marker_error"] = cancellation_marker_error;
                }
                result["reports"] = std::move(reports);
                if (flag(config, "repeat_batch")) result["repeat"] = runtime.pump_live(id, options);
                if (flag(config, "amend_after")) (void)store->amend(store->host_actor(Role::Root, id),
                    number(store->request(id), "state_version"), "An explicit later requirement fences the original model facts.",
                    Array{}, "owner-later-amend");
            }
        } catch (const Error& error) {
            result["threw"] = true; result["error"] = error.what();
        }
        result["credential_reads"] = credentials; result["evidence_reads"] = evidence_calls;
        result["accounting"] = store->live_accounting_status(id);
        result["budget"] = store->budget(id); result["ledger"] = store->verify_usage_ledger(id);
        result["task_count"] = store->tasks(id).size();
        std::cout << canonical_json(result) << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
