#include "wls/offline_runtime.hpp"

#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <set>

namespace {
using namespace wls;
using namespace wls::s1;
using Object = boost::json::object;
using Array = boost::json::array;

class Options {
public:
    Options(int argc, char** argv, int start) {
        for (int i = start; i < argc; ++i) {
            std::string key(argv[i]);
            if (!key.starts_with("--") || key.size() <= 2 || values_.contains(key))
                throw Error(ErrorCode::InvalidInput, "invalid or duplicate option: " + key);
            if (key == "--model-input") { values_[key] = "true"; continue; }
            if (i + 1 >= argc || std::string_view(argv[i + 1]).starts_with("--"))
                throw Error(ErrorCode::InvalidInput, "option needs a value: " + key);
            values_[key] = argv[++i];
        }
    }
    std::string take(const std::string& key, const std::string& fallback = {}) {
        const auto found = values_.find(key);
        if (found == values_.end()) return fallback;
        auto result = found->second; values_.erase(found); return result;
    }
    std::string required(const std::string& key) {
        const auto value = take(key);
        if (value.empty()) throw Error(ErrorCode::InvalidInput, "required option: " + key);
        return value;
    }
    std::int64_t number(const std::string& key, std::int64_t fallback, std::int64_t minimum = 0) {
        const auto text = take(key);
        if (text.empty()) return fallback;
        std::int64_t number{};
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (ec != std::errc{} || end != text.data() + text.size() || number < minimum)
            throw Error(ErrorCode::InvalidInput, "invalid integer: " + key);
        return number;
    }
    void finish() const {
        if (!values_.empty()) throw Error(ErrorCode::InvalidInput, "unknown option for command: " + values_.begin()->first);
    }
private:
    std::map<std::string, std::string> values_;
};

std::int64_t integer(const Json& value) {
    if (value.is_int64()) return value.as_int64();
    if (value.is_uint64() && value.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return static_cast<std::int64_t>(value.as_uint64());
    throw Error(ErrorCode::InvalidInput, "expected a bounded integer");
}

std::string text(const Json& value) {
    if (!value.is_string()) throw Error(ErrorCode::InvalidInput, "expected text");
    return std::string(value.as_string());
}

void output(const Json& value) { std::cout << canonical_json(value) << '\n'; }

FrozenRequestConfig live_config(Options& options, const std::filesystem::path& source) {
    const std::filesystem::path system = options.take("--config", (source / "config/examples/system.yaml").string());
    const std::filesystem::path functions = options.take("--functions-dir", (system.parent_path() / "functions").string());
    const std::filesystem::path prompts = options.take("--prompts-dir", (source / "prompts").string());
    const auto function = options.take("--function", "general");
    const auto capability = options.take("--capability");
    return load_config_catalog(system, functions, prompts).freeze(function, capability);
}

Json array_of(const std::vector<Json>& items) {
    Array result;
    for (const auto& item : items) result.push_back(item);
    return result;
}

Json request_view(Store& store, const std::string& id) {
    const auto record = store.request(id);
    Object visible;
    for (const auto key : {"request_id", "tenant_id", "project_id", "root_id", "function_id", "status", "delivery_status",
                           "resource_status", "requirements_revision", "plan_revision", "state_version", "dispatcher_epoch",
                           "config_snapshot_sha256", "created_at", "updated_at"}) {
        if (record.as_object().contains(key)) visible[key] = record.at(key);
    }
    return Object{{"request", visible}, {"snapshot", store.request_snapshot(id)}, {"tasks", array_of(store.tasks(id))},
                  {"budget", store.budget(id)}};
}

void help() {
    std::cout << R"(wls 0.2.0-s1.1 — persistent offline S1 foundation

Resource paths default to the source checkout in the current directory.
Use --source-root PATH from another directory. Outputs are JSON; errors go to stderr.

  wls config check [--config FILE] [--function ID]
  wls config explain [--config FILE] [--function ID] [--role worker]
  wls schema check --schema NAME --input FILE [--model-input]
  wls init --db FILE
  wls submit --db FILE --key KEY --text-file FILE [--constraints-file FILE]
             [--tenant local] [--project local] [--root local-root]
             [--function general] [--config FILE] [--max-attempts 64]
             [--input-budget N] [--output-budget N] [--cost-budget-micro-usd N]
  wls show --db FILE --request ID
  wls events --db FILE --request ID [--after N] [--limit N]
  wls budget --db FILE --request ID
  wls config-snapshot --db FILE --request ID [--role worker]
  wls run-fake --db FILE --request ID --script FILE [--disposition update|keep|cancel]
  wls amend --db FILE --request ID --text-file FILE --constraints-file FILE
  wls cancel --db FILE --request ID
  wls pending --db FILE

submit freezes original text, explicit constraints, configuration and budgets.
An omitted initial catalog is empty; this CLI does not extract or certify requirements.
amend takes the entire effective constraint catalog; every existing task then needs
an explicit disposition. run-fake supports one independent team and durable note
operations. It does not execute file/shell tools or produce a business delivery.
show/events/budget/config-snapshot/pending use read-only database connections.
Opening a writer fences interrupted executions and keeps their unknown budgets.
No command starts a daemon, runs environment checks or calls a real provider.
)";
}

int run_cli(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "help") { help(); return 0; }
    const std::string command(argv[1]);
    if (command == "--version" || command == "version") {
        if (argc != 2) throw Error(ErrorCode::InvalidInput, "version takes no options");
        output(Object{{"version", "0.2.0-s1.1"}, {"blueprint", "0.4.1"}, {"mode", "offline_fake"}}); return 0;
    }
    int start = 2;
    std::string action;
    if (command == "config" || command == "schema") {
        if (argc < 3) throw Error(ErrorCode::InvalidInput, command + " needs a subcommand");
        action = argv[2]; start = 3;
    }
    Options options(argc, argv, start);
    const std::filesystem::path source = options.take("--source-root", ".");
    if (command == "config") {
        if (action != "check" && action != "explain") throw Error(ErrorCode::InvalidInput, "config needs check or explain");
        const auto frozen = live_config(options, source);
        const auto role = action == "explain" ? role_from_string(options.take("--role", "worker")) : Role::Root;
        options.finish();
        if (action == "explain") output(frozen.explain(role));
        else output(Object{{"valid", true}, {"function_id", frozen.function_id()}, {"capability_id", frozen.capability_id()},
            {"snapshot_sha256", frozen.sha256()}, {"dependency_count", frozen.dependencies().size()},
            {"snapshot_bytes", frozen.serialize().size()}, {"real_calls_enabled", false}, {"environment_checks_executed", false}});
        return 0;
    }
    if (command == "schema") {
        if (action != "check") throw Error(ErrorCode::InvalidInput, "schema needs check");
        const auto name = options.required("--schema");
        const auto input = options.required("--input");
        const auto model_input = options.take("--model-input") == "true";
        options.finish();
        const SchemaRegistry schemas(source / "schemas");
        schemas.validate(name, parse_json_strict(read_runtime_file(input)), model_input);
        output(Object{{"valid", true}, {"schema", name}, {"model_input", model_input}, {"semantic_acceptance", "not_evaluated"}});
        return 0;
    }
    const std::set<std::string> commands{"init", "submit", "show", "events", "budget", "config-snapshot", "run-fake", "amend", "cancel", "pending"};
    if (!commands.contains(command)) throw Error(ErrorCode::InvalidInput, "unknown command: " + command);
    const auto database = options.required("--db");
    const auto schema_sql = read_runtime_file(source / "db/schema.sql");
    const auto migration_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    if (command == "init") {
        options.finish(); Store store(database, schema_sql, migration_sql); output(store.diagnostics()); return 0;
    }
    if (command == "submit") {
        const auto frozen = live_config(options, source);
        AdmissionInput admission;
        admission.tenant_id = options.take("--tenant", "local");
        admission.project_id = options.take("--project", "local");
        admission.root_id = options.take("--root", "local-root");
        admission.function_id = frozen.function_id();
        admission.idempotency_key = options.required("--key");
        admission.original_text = read_runtime_file(options.required("--text-file"), 65536);
        const auto constraints = options.take("--constraints-file");
        if (!constraints.empty()) admission.constraint_catalog = parse_json_strict(read_runtime_file(constraints));
        admission.config_snapshot = frozen.json();
        const auto grants = frozen.worker_tools();
        admission.tool_allowlist = std::vector<std::string>(grants.begin(), grants.end());
        const auto& defaults = frozen.json().at("system").at("budget_defaults");
        admission.budget.max_attempts = options.number("--max-attempts", 64, 1);
        admission.budget.input_tokens = options.number("--input-budget", defaults.at("max_input_tokens").is_null() ? 1000000 : integer(defaults.at("max_input_tokens")));
        admission.budget.output_tokens = options.number("--output-budget", defaults.at("max_output_tokens").is_null() ? 100000 : integer(defaults.at("max_output_tokens")));
        std::int64_t cost_limit{};
        if (!defaults.at("max_cost_usd").is_null()) {
            const auto& cost = defaults.at("max_cost_usd");
            const double dollars = cost.is_double() ? cost.as_double() : static_cast<double>(integer(cost));
            const double micro = std::floor(dollars * 1000000.0);
            if (!std::isfinite(micro) || micro < 0 || micro >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                throw Error(ErrorCode::InvalidInput, "cost budget overflow");
            cost_limit = static_cast<std::int64_t>(micro);
        }
        admission.budget.cost_micro_usd = options.number("--cost-budget-micro-usd", cost_limit);
        options.finish();
        Store store(database, schema_sql, migration_sql);
        const auto admitted = store.admit(admission);
        output(request_view(store, text(admitted.at("request_id")))); return 0;
    }
    if (command == "pending") {
        options.finish(); Store store(database, schema_sql, migration_sql, StoreOpenMode::ReadOnly);
        output(store.pending_recovery()); return 0;
    }
    const auto request = options.required("--request");
    if (command == "show" || command == "events" || command == "budget" || command == "config-snapshot") {
        const auto after = command == "events" ? options.number("--after", 0) : 0;
        const auto limit = command == "events" ? options.number("--limit", 100, 1) : 100;
        const auto role = command == "config-snapshot" ? role_from_string(options.take("--role", "worker")) : Role::Root;
        options.finish();
        Store store(database, schema_sql, migration_sql, StoreOpenMode::ReadOnly);
        if (command == "show") output(request_view(store, request));
        if (command == "events") output(array_of(store.events(request, after, limit)));
        if (command == "budget") output(store.budget(request));
        if (command == "config-snapshot") output(restore_request_config(store.request(request)).explain(role));
        return 0;
    }
    if (command == "amend") {
        const auto amendment = read_runtime_file(options.required("--text-file"), 65536);
        const auto catalog = parse_json_strict(read_runtime_file(options.required("--constraints-file")));
        options.finish();
        Store store(database, schema_sql, migration_sql);
        const auto actor = store.host_actor(Role::Root, request);
        output(store.amend(actor, integer(store.request(request).at("state_version")), amendment, catalog, new_id("amend")));
        return 0;
    }
    if (command == "cancel") {
        options.finish(); Store store(database, schema_sql, migration_sql);
        output(store.cancel_request(store.host_actor(Role::Root, request), integer(store.request(request).at("state_version")), new_id("cancel")));
        return 0;
    }
    const auto script = fake_model_steps_from_json(parse_json_strict(read_runtime_file(options.required("--script"))));
    const auto disposition = options.take("--disposition");
    options.finish();
    const SchemaRegistry schemas(source / "schemas");
    Store store(database, schema_sql, migration_sql);
    output(OfflineRuntime(store, schemas).run(request, script, disposition));
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try { return run_cli(argc, argv); }
    catch (const Error& error) {
        std::cerr << canonical_json(Object{{"error", error_name(error.code())}, {"message", error.what()}}) << '\n';
        return error.code() == ErrorCode::Stale || error.code() == ErrorCode::Conflict ? 3 : 2;
    } catch (const std::exception& error) {
        std::cerr << canonical_json(Object{{"error", "runtime_error"}, {"message", error.what()}}) << '\n';
        return 1;
    }
}
