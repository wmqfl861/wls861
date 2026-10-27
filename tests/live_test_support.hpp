#pragma once

#include "wls/config_loader.hpp"
#include "wls/live_model.hpp"
#include "wls/store.hpp"
#include "protocol_test_support.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <array>
#include <fstream>
#include <iterator>
#include <memory>

namespace wls::test::live {
using namespace s1;
using Object = boost::json::object;
using Array = boost::json::array;

inline std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read synthetic Store fixture asset");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
inline std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
inline std::int64_t number(const Json& value, std::string_view key) { return value.at(key).as_int64(); }
inline void error(ErrorCode expected, const std::function<void()>& operation) {
    try { operation(); }
    catch (const Error& failure) {
        if (failure.code() != expected) throw std::runtime_error("unexpected Store error: " + std::string(failure.what()));
        return;
    }
    throw std::runtime_error("expected Store error was not thrown");
}
inline void sql(const std::filesystem::path& path, const std::string& statement) {
    sqlite3* database = nullptr;
    CHECK(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    char* message = nullptr;
    const int result = sqlite3_exec(database, statement.c_str(), nullptr, nullptr, &message);
    const std::string explanation = message ? message : "synthetic SQLite failure";
    sqlite3_free(message); sqlite3_close(database);
    if (result != SQLITE_OK) throw std::runtime_error(explanation);
}
inline std::string sql_text(const std::filesystem::path& path, const std::string& statement) {
    sqlite3* database = nullptr;
    CHECK(sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* query = nullptr;
    CHECK(sqlite3_prepare_v2(database, statement.c_str(), -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW);
    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(query, 0));
    const std::string result = bytes ? std::string(bytes, static_cast<std::size_t>(sqlite3_column_bytes(query, 0))) : "";
    sqlite3_finalize(query); sqlite3_close(database); return result;
}

struct Assets {
    std::filesystem::path source;
    std::string base;
    std::array<std::string, 9> migrations;
    explicit Assets(std::filesystem::path root) : source(std::move(root)), base(read(source / "db/schema.sql")) {
        constexpr std::array<std::string_view, 9> names{"004_s1_store.sql", "005_worker_tools.sql", "006_event_runtime.sql",
            "007_workflow_runtime.sql", "008_acceptance_runtime.sql", "009_owned_cleanup.sql",
            "010_owned_allocation_recovery.sql", "011_live_model.sql", "012_usage_reconciliation.sql"};
        for (std::size_t i = 0; i < names.size(); ++i) migrations[i] = read(source / "db/migrations" / names[i]);
    }
    std::unique_ptr<Store> open(const std::filesystem::path& path, StoreRecoveryScope scope = {},
                               int version = 410, StoreOpenMode mode = StoreOpenMode::Takeover) const {
        return std::make_unique<Store>(path, base, migrations[0], mode, migrations[1], migrations[2], migrations[3],
            migrations[4], migrations[5], std::move(scope), migrations[6],
            version >= 409 ? migrations[7] : std::string_view{}, version >= 410 ? migrations[8] : std::string_view{});
    }
};

inline LiveModelSettings settings(std::string ca = "synthetic pre-cancel trust material", int port = 443) {
    LiveModelSettings live;
    live.provider = protocol::profile();
    live.provider.profile_id = "local-store-fixture"; live.provider.model_ref = "local-model";
    live.provider.base_url = "https://localhost:" + std::to_string(port);
    live.provider.reasoning_efforts.clear();
    live.transport.ca_bundle_pem = std::move(ca); live.transport.resolved_ipv4 = "127.0.0.1";
    live.transport.max_total_response_bytes = 3U * 1024U * 1024U;
    live.execution_kind = "local_tls"; live.capability_evidence_source = "synthetic-loopback-fixture-v1";
    live.capability_evidence_sha256 = sha256_hex("synthetic loopback capabilities; no provider claim");
    return live;
}
inline Json model_profile(const LiveModelSettings& live) {
    return Object{{"adapter", "openai-responses"}, {"adapter_version", "1"}, {"protocol", "openai-responses-v1"},
        {"model", live.provider.model}, {"capabilities", Object{{"streaming", "supported"}, {"tool_calling", "supported"},
            {"parallel_tool_calls", "supported"}, {"reasoning_modes", Array{"provider_default", "off"}},
            {"context_window", live.provider.context_window}, {"max_output_tokens", live.provider.max_output_tokens}, {"cache_mode", "none"}}},
        {"responses", live_model_settings_json(live)}};
}
inline AdmissionInput admission(const Assets& assets, const LiveModelSettings& live,
                                std::string key = "fixture", bool enabled = true, bool fake = false) {
    ::test::TempDir directory;
    std::filesystem::copy(assets.source / "config/examples", directory.path / "config", std::filesystem::copy_options::recursive);
    std::filesystem::copy(assets.source / "prompts", directory.path / "prompts", std::filesystem::copy_options::recursive);
    const auto system_path = directory.path / "config/system.yaml";
    auto catalog = load_config_catalog(system_path, directory.path / "config/functions", directory.path / "prompts");
    if (!fake) {
        auto system = catalog.freeze("general").json().at("system");
        system.at("models").as_object()[live.provider.model_ref] = model_profile(live);
        system.at("defaults").at("model_ref") = live.provider.model_ref;
        system.at("model_calls").at("real_calls_enabled") = enabled;
        system.at("context").at("request_format") = "structured-v2";
        { std::ofstream output(system_path); output << canonical_json(system); }
        catalog = load_config_catalog(system_path, directory.path / "config/functions", directory.path / "prompts");
    }
    AdmissionInput input;
    input.tenant_id = "local-tenant"; input.project_id = "local-project"; input.root_id = "root-fixture";
    input.function_id = "general"; input.idempotency_key = std::move(key);
    input.original_text = "Preserve the synthetic original requirement.";
    input.config_snapshot = catalog.freeze("general").json();
    input.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", input.original_text}, {"source_revision", 1}}};
    input.tool_allowlist = {"note", "fs_read", "fs_write"};
    input.budget = BudgetLimits{64, 1000000, 100000, 10000};
    return input;
}

struct Fixture {
    const Assets& assets;
    ::test::TempDir directory;
    std::filesystem::path path{directory.path / "fixture.sqlite"};
    std::unique_ptr<Store> store;
    LiveModelSettings live;
    std::string request_id;
    Fixture(const Assets& input_assets, LiveModelSettings selected = settings(), bool enabled = true, int version = 410, bool fake = false,
            std::filesystem::path persistent_path = {})
        : assets(input_assets), path(persistent_path.empty() ? directory.path / "fixture.sqlite" : std::move(persistent_path)),
          store(assets.open(path, {}, version)), live(std::move(selected)) {
        request_id = text(store->admit(admission(assets, live, "first", enabled, fake)), "request_id");
    }
    StoreActor actor(Role role = Role::Dispatcher) const { return store->host_actor(role, request_id); }
    ModelRequest request(std::string id = "attempt-fixture") const {
        const auto row = store->request(request_id);
        const auto frozen = FrozenRequestConfig::restore(canonical_json(row.at("config_snapshot")), text(row, "config_snapshot_sha256"));
        const auto profile = frozen.model_profile(Role::Dispatcher);
        ContextInput context;
        context.role = Role::Dispatcher; context.model = frozen.resolve(Role::Dispatcher);
        context.adapter_version = text(profile, "adapter_version"); context.protocol_version = text(profile, "protocol");
        context.system_prompt = "Synthetic Store fixture only."; context.role_prompt = "Preserve original captured identity.";
        context.function_prompt = "Return a bounded note."; context.task_contract = canonical_json(store->request_snapshot(request_id));
        ModelRequest output;
        output.call_id = "call-" + id; output.attempt_id = std::move(id); output.role = Role::Dispatcher;
        output.binding = store->request_binding(request_id); output.config_snapshot_id = text(row, "config_snapshot_sha256");
        output.model = context.model;
        output.context = frozen.context_request_format() == "structured-v2" ? compile_structured_context(context, {}) : ContextCompiler().compile(context);
        output.reserved_input_tokens = 1000; output.reserved_output_tokens = 100; output.reserved_cost_micro_usd = 100;
        output.max_output_tokens = 100; output.timeout = std::chrono::seconds(3);
        return output;
    }
    AttemptReservation reservation(const ModelRequest& request, bool fake = false) const {
        AttemptReservation result;
        result.call_id = request.call_id; result.attempt_id = request.attempt_id;
        result.config_snapshot_sha256 = request.config_snapshot_id; result.model_ref = request.model.model;
        result.adapter_kind = fake ? "fake" : "openai-responses";
        result.reserved_input_tokens = request.reserved_input_tokens; result.reserved_output_tokens = request.reserved_output_tokens;
        result.reserved_cost_micro_usd = request.reserved_cost_micro_usd; result.max_output_tokens = request.max_output_tokens;
        result.request_envelope = to_json(request); return result;
    }
    Json reserve(const ModelRequest& request) { return store->reserve_attempt(actor(), request.binding, reservation(request)); }
    Json claim(const ModelRequest& request) {
        return store->claim_live_send(actor(), request.attempt_id, number(store->attempt(request.attempt_id), "state_version"),
            encode_responses_request(live.provider, request, live.stream), live.transport, "claim-" + request.attempt_id);
    }
    HttpsTransportResult cancelled(const ModelRequest& request) const {
        std::stop_source stopped; stopped.request_stop();
        return perform_responses_https(live.provider, request, encode_responses_request(live.provider, request, live.stream),
            live.transport, "WLS_SYNTHETIC_STORE_BEARER", stopped.get_token());
    }
    Json settle(const ModelRequest& request, const HttpsTransportResult& response) {
        const auto outcome = response.response.status == ModelResultStatus::Completed ? "COMPLETED" :
            response.response.status == ModelResultStatus::Interrupted ? "INTERRUPTED" : "ERROR";
        return store->settle_attempt(request.attempt_id, request.binding, request.config_snapshot_id,
                                     outcome, to_json(response.response.usage), to_json(response.response));
    }
    Json interrupted(const ModelRequest& request) {
        (void)reserve(request); const auto send = claim(request); const auto result = cancelled(request);
        (void)store->record_live_transport(request.attempt_id, text(send, "claim_id"), result);
        return settle(request, result);
    }
    void reopen(StoreRecoveryScope scope = {}) { store.reset(); store = assets.open(path, std::move(scope)); }
};

inline VerifiedUsageEvidence evidence(const Json& settlement, ModelUsage usage, std::string nonce = "proof",
                                     std::string source_kind = "loopback_fixture") {
    const auto bytes = canonical_json(Object{{"fixture", "synthetic-usage-proof-v1"}, {"nonce", nonce},
        {"attempt_id", settlement.at("attempt_id")}, {"settlement_sha256", settlement.at("settlement_sha256")},
        {"unit", "micro_usd"}, {"usage", to_json(usage)}});
    return verify_usage_evidence(std::move(source_kind), "local-source-" + nonce, bytes, [](std::string_view raw) {
        const auto parsed = parse_json_strict(raw);
        CHECK(parsed.at("fixture") == "synthetic-usage-proof-v1"); CHECK(parsed.at("unit") == "micro_usd");
        return UsageEvidenceStatement{text(parsed, "attempt_id"), text(parsed, "settlement_sha256"), model_usage_from_json(parsed.at("usage"))};
    });
}
inline ModelUsage complete_usage(std::int64_t input = 9, std::int64_t output = 3, std::int64_t cost = 17) {
    ModelUsage value; value.input_tokens = input; value.output_tokens = output;
    value.cache_read_tokens = 2; value.cache_write_tokens = 1; value.cost_micro_usd = cost; return value;
}
} // namespace wls::test::live
