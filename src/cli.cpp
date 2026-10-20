#include "wls/offline_runtime.hpp"
#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
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
        const auto found = values_.find(key);
        if (found == values_.end()) return fallback;
        const auto text = found->second;
        values_.erase(found);
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

bool path_contains(const std::filesystem::path& parent, const std::filesystem::path& child) {
    auto left = parent.begin();
    auto right = child.begin();
    for (; left != parent.end(); ++left, ++right) {
        if (right == child.end() || *left != *right) return false;
    }
    return true;
}

// Check all host-selected roots before opening a writer or allowing task A to
// run. Store and WorkerTools recheck actual identities at binding/dispatch.
std::map<std::string, std::filesystem::path> workspace_map(
        const Json& value, const std::filesystem::path& database) {
    if (!value.is_object() || value.as_object().size() > 64)
        throw Error(ErrorCode::InvalidInput, "workspaces must be a bounded node-to-directory object");
    std::map<std::string, std::filesystem::path> result;
    std::vector<Json> identities;
    std::filesystem::path control;
    try { control = std::filesystem::weakly_canonical(std::filesystem::absolute(database)); }
    catch (const std::filesystem::filesystem_error&) {
        throw Error(ErrorCode::InvalidInput, "cannot resolve control database path");
    }
    for (const auto& entry : value.as_object()) {
        const std::string node(entry.key());
        const auto node_char = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
        if (node.empty() || node.size() > 48 || !node_char(node.front()) ||
            !std::all_of(node.begin(), node.end(), [&](char c) { return node_char(c) || c == '-'; }))
            throw Error(ErrorCode::InvalidInput, "invalid workspace node ID");
        const std::filesystem::path directory = text(entry.value());
        if (!directory.is_absolute())
            throw Error(ErrorCode::InvalidInput, "workspace map directories must be absolute");
        const Workspace inspected(directory, WorkspaceAccess::ReadOnly);
        const auto identity = inspected.root_identity();
        const std::filesystem::path canonical = text(identity.at("path"));
        if (path_contains(canonical, control))
            throw Error(ErrorCode::Unauthorized, "control database must be outside every workspace");
        for (const auto& prior : identities) {
            const std::filesystem::path previous = text(prior.at("path"));
            if ((prior.at("device") == identity.at("device") && prior.at("inode") == identity.at("inode")) ||
                path_contains(previous, canonical) || path_contains(canonical, previous))
                throw Error(ErrorCode::Conflict, "task workspaces must have distinct, non-overlapping identities");
        }
        identities.push_back(identity);
        result.emplace(node, canonical);
    }
    return result;
}

void validate_dispositions(const Json& value) {
    if (!value.is_object() || value.as_object().size() > 64)
        throw Error(ErrorCode::InvalidInput, "dispositions must be a bounded node-to-decision object");
    for (const auto& entry : value.as_object()) {
        const auto& decision = entry.value();
        if (!decision.is_object() || decision.as_object().size() != 2 ||
            !decision.as_object().contains("disposition") || !decision.as_object().contains("rationale"))
            throw Error(ErrorCode::InvalidInput, "each disposition requires exactly disposition and rationale");
        const auto kind = text(decision.at("disposition"));
        const auto rationale = text(decision.at("rationale"));
        if ((kind != "update" && kind != "keep" && kind != "cancel") || rationale.empty() ||
            rationale.size() > 4096 || rationale.find('\0') != std::string::npos ||
            rationale.find_first_not_of(" \t\r\n") == std::string::npos)
            throw Error(ErrorCode::InvalidInput, "invalid disposition or rationale");
    }
}

StoreActor scoped_actor(Store& store, const std::string& request_id, Role role, const std::string& task_id) {
    if (role == Role::Dispatcher && task_id.empty()) return store.host_actor(role, request_id);
    if ((role != Role::Worker && role != Role::TeamLead) || task_id.empty())
        throw Error(ErrorCode::InvalidInput, "Dispatcher has request scope; Worker and TeamLead require --task");
    const auto binding = store.binding(task_id);
    if (binding.request_id != request_id) throw Error(ErrorCode::Unauthorized, "task belongs to another request");
    return store.host_actor(role, request_id, binding.team_id);
}

Json request_view(Store& store, const std::string& id) {
    const auto record = store.request(id);
    auto tasks = store.tasks(id);
    const bool events_enabled = store.event_runtime_enabled();
    if (events_enabled) {
        for (auto& task : tasks) task.as_object()["worker_wait"] = store.worker_wait(text(task.at("task_id")));
    }
    Object visible;
    for (const auto key : {"request_id", "tenant_id", "project_id", "root_id", "function_id", "status", "delivery_status",
                           "resource_status", "requirements_revision", "plan_revision", "state_version", "dispatcher_epoch",
                           "config_snapshot_sha256", "created_at", "updated_at"}) {
        if (record.as_object().contains(key)) visible[key] = record.at(key);
    }
    return Object{{"request", visible}, {"snapshot", store.request_snapshot(id)}, {"tasks", array_of(tasks)},
                  {"event_runtime_enabled", events_enabled},
                  {"workflow_runtime_enabled", store.workflow_runtime_enabled()}, {"budget", store.budget(id)}};
}

void help() {
    std::cout << R"(wls 0.5.0-s1.4 — durable questions and submission checks with offline models

Resource paths default to the source checkout in the current directory.
Use --source-root PATH from another directory. Outputs are JSON; errors go to stderr.

  wls config check [--config FILE] [--function ID]
  wls config explain [--config FILE] [--function ID] [--role worker]
  wls schema check --schema NAME --input FILE [--model-input]
  wls doctor [--config FILE] [--template default] [--sandbox-manifest FILE]
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
               [--workspace DIRECTORY] [--worker-calls 1..16] [--sandbox-manifest FILE]
  wls pump-fake --db FILE --request ID --script FILE [--workspaces-file FILE]
                [--dispositions-file FILE] [--max-calls 1..64] [--worker-calls 1..16]
                [--sandbox-manifest FILE]
  wls inbox --db FILE --request ID [--role dispatcher] [--task ID] [--limit N]
  wls history --db FILE --request ID --role ROLE [--task ID] [--limit N]
  wls questions --db FILE --request ID [--task ID] [--limit N]
  wls submissions --db FILE --request ID --task ID [--limit N]
  wls answer --db FILE --request ID --question ID --expected-version N
             [--text-file FILE] [--selected-option ID]
  wls wait --db FILE --request ID --task ID --reason TEXT
  wls wake --db FILE --request ID --task ID --wait-generation N
  wls tool-query --db FILE --request ID --task ID --workspace DIRECTORY
                 --tool NAME --input FILE [--sandbox-manifest FILE]
  wls amend --db FILE --request ID --text-file FILE --constraints-file FILE
  wls cancel --db FILE --request ID
  wls pending --db FILE

submit freezes original text, explicit constraints, configuration and budgets.
An omitted initial catalog is empty; this CLI does not extract or certify requirements.
amend takes the entire effective constraint catalog; every existing task then needs
an explicit disposition. run-fake supports one independent team. pump-fake consumes
durable events for bounded independent teams; workspaces-file maps node IDs to
absolute directories and dispositions-file maps each node to an object with
disposition (update/keep/cancel) and a nonempty rationale. Omitted AMEND tasks
remain paused. questions/submissions inspect original workflow records without
taking over the writer. answer requires the displayed question state_version and
at least text-file or selected-option; Root answers only questions forwarded to
the user. An answer preserves the question's original binding. Worker submit
captures bounded immutable file bytes for TeamLead checking and possible rework;
passing that check does not establish independent review or business acceptance.
wait/wake are local host controls;
a wake leaves instruction, AMEND and UNKNOWN gates in force. An explicit
workspace enables bounded file tools and process execution with verified strong
isolation. A process requires a pinned host manifest and delegated cgroup v2;
missing prerequisites refuse execution. Worker tools do not establish delivery.
tool-query accepts only registered diagnostics. Read commands do not fence a writer.
Opening a writer fences interrupted executions and keeps their unknown budgets.
doctor observes prerequisites without changing host policy or running template checks.
No command starts a daemon or calls a real model provider.
)";
}

int run_cli(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "help") { help(); return 0; }
    const std::string command(argv[1]);
    if (command == "--version" || command == "version") {
        if (argc != 2) throw Error(ErrorCode::InvalidInput, "version takes no options");
        output(Object{{"version", "0.5.0-s1.4"}, {"blueprint", "0.4.1"}, {"mode", "offline_fake"}}); return 0;
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
    if (command == "doctor") {
        const auto frozen = live_config(options, source);
        const auto template_id = options.take("--template", "default");
        const auto manifest_path = options.take("--sandbox-manifest");
        const auto manifest = manifest_path.empty() ? Json(Object{}) : parse_json_strict(read_runtime_file(manifest_path, 262144));
        options.finish();
        auto result = to_json(sandbox_doctor(worker_sandbox_policy(frozen, template_id, manifest)));
        result.as_object()["config_snapshot_sha256"] = frozen.sha256();
        result.as_object()["template_id"] = template_id;
        result.as_object()["environment_checks_executed"] = false;
        result.as_object()["process_isolation_verified"] = false;
        output(result); return 0;
    }
    const std::set<std::string> commands{"init", "submit", "show", "events", "budget", "config-snapshot", "run-fake", "pump-fake", "inbox", "history", "questions", "submissions", "answer", "wait", "wake", "tool-query", "amend", "cancel", "pending"};
    if (!commands.contains(command)) throw Error(ErrorCode::InvalidInput, "unknown command: " + command);
    const auto database = options.required("--db");
    const auto schema_sql = read_runtime_file(source / "db/schema.sql");
    const auto migration_sql = read_runtime_file(source / "db/migrations/004_s1_store.sql");
    const auto tools_migration_sql = read_runtime_file(source / "db/migrations/005_worker_tools.sql");
    const auto events_migration_sql = read_runtime_file(source / "db/migrations/006_event_runtime.sql");
    const auto workflow_migration_sql = read_runtime_file(source / "db/migrations/007_workflow_runtime.sql");
    if (command == "init") {
        options.finish(); Store store(database, schema_sql, migration_sql, StoreOpenMode::Takeover, tools_migration_sql, events_migration_sql, workflow_migration_sql); output(store.diagnostics()); return 0;
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
        Store store(database, schema_sql, migration_sql, StoreOpenMode::Takeover, tools_migration_sql, events_migration_sql, workflow_migration_sql);
        const auto admitted = store.admit(admission);
        output(request_view(store, text(admitted.at("request_id")))); return 0;
    }
    if (command == "pending") {
        options.finish(); Store store(database, schema_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql, events_migration_sql, workflow_migration_sql);
        output(store.pending_recovery()); return 0;
    }
    const auto request = options.required("--request");
    if (command == "questions" || command == "submissions") {
        const auto task = command == "submissions" ? options.required("--task") : options.take("--task");
        const auto limit = options.number("--limit", command == "questions" ? 128 : 64, 1);
        const auto maximum = command == "questions" ? 128 : 64;
        if (limit > maximum) throw Error(ErrorCode::InvalidInput,
            command == "questions" ? "questions limit must be 1..128" : "submissions limit must be 1..64");
        options.finish();
        Store store(database, schema_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql, events_migration_sql, workflow_migration_sql);
        (void)store.request(request);
        if (!task.empty() && text(store.task(task).at("request_id")) != request)
            throw Error(ErrorCode::Unauthorized, "task belongs to another request");
        if (command == "questions") output(Object{{"request_id", request},
            {"questions", array_of(store.questions(request, task, limit))}});
        else output(Object{{"request_id", request}, {"task_id", task},
            {"submissions", array_of(store.submissions(task, limit))}});
        return 0;
    }
    if (command == "answer") {
        const auto question_id = options.required("--question");
        if (!is_typed_id(question_id, "q")) throw Error(ErrorCode::InvalidInput, "invalid question ID");
        const auto expected_version = options.number("--expected-version", 0, 1);
        if (expected_version == 0) throw Error(ErrorCode::InvalidInput, "answer requires --expected-version");
        const auto answer_path = options.take("--text-file");
        const auto selected_option = options.take("--selected-option");
        Object answer;
        if (!answer_path.empty()) {
            const auto answer_text = read_runtime_file(answer_path, 16000);
            if (answer_text.empty() || answer_text.find('\0') != std::string::npos ||
                answer_text.find_first_not_of(" \t\r\n") == std::string::npos)
                throw Error(ErrorCode::InvalidInput, "answer text must be nonblank and contain no NUL");
            answer["text"] = answer_text;
        }
        if (!selected_option.empty()) answer["selected_option"] = selected_option;
        options.finish();
        const SchemaRegistry schemas(source / "schemas");
        schemas.validate("answer_question", answer, true);
        // Reject stale UI actions, foreign objects and invalid selections before
        // acquiring a writer. Store repeats the guards atomically after takeover.
        {
            Store reader(database, schema_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql, events_migration_sql, workflow_migration_sql);
            const auto record = reader.request(request);
            const auto question = reader.question(question_id);
            if (text(question.at("request_id")) != request)
                throw Error(ErrorCode::Unauthorized, "question belongs to another request");
            if (integer(question.at("state_version")) != expected_version)
                throw Error(ErrorCode::Stale, "question version changed; inspect it before answering");
            if (text(question.at("status")) != "OPEN" || text(question.at("holder")) != "user")
                throw Error(ErrorCode::NotReady, "Root can answer only open questions forwarded to the user");
            const auto status = text(record.at("status"));
            if (status == "CANCELLED" || status == "FAILED" || status == "SUPERSEDED" || status == "DELIVERED")
                throw Error(ErrorCode::NotReady, "cannot answer a terminal request");
            if (!selected_option.empty()) {
                const auto& body = question.at("body").as_object();
                const auto* choices = body.if_contains("options");
                if (choices == nullptr || !std::any_of(choices->as_array().begin(), choices->as_array().end(),
                        [&](const Json& item) { return text(item.at("id")) == selected_option; }))
                    throw Error(ErrorCode::InvalidInput, "selected option is absent from the original question");
            }
        }
        Store store(database, schema_sql, migration_sql, StoreOpenMode::Takeover, tools_migration_sql, events_migration_sql, workflow_migration_sql);
        output(store.answer_question(store.host_actor(Role::Root, request), store.request_binding(request),
            question_id, expected_version, answer, new_id("answer")));
        return 0;
    }
    if (command == "inbox" || command == "history") {
        const auto role = role_from_string(options.take("--role", "dispatcher"));
        const auto task = options.take("--task");
        const auto limit = options.number("--limit", 256, 1);
        if (limit > 256) throw Error(ErrorCode::InvalidInput, "inbox/history limit must be 1..256");
        options.finish();
        Store store(database, schema_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql, events_migration_sql, workflow_migration_sql);
        const auto actor = scoped_actor(store, request, role, task);
        output(array_of(command == "inbox" ? store.inbox(actor, limit) : store.attempt_history(actor, limit)));
        return 0;
    }
    if (command == "wait" || command == "wake") {
        const auto task = options.required("--task");
        const auto reason = command == "wait" ? options.required("--reason") : std::string{};
        if (reason.size() > 4096)
            throw Error(ErrorCode::InvalidInput, "Worker wait reason must contain at most 4096 bytes");
        std::int64_t generation{};
        if (command == "wake") {
            generation = options.number("--wait-generation", 0, 1);
            if (generation == 0) throw Error(ErrorCode::InvalidInput, "wake requires --wait-generation");
        }
        options.finish();
        Store store(database, schema_sql, migration_sql, StoreOpenMode::Takeover, tools_migration_sql, events_migration_sql, workflow_migration_sql);
        const auto actor = scoped_actor(store, request, Role::Worker, task);
        const auto binding = store.binding(task);
        output(command == "wait"
            ? store.wait_worker(actor, binding, integer(store.task(task).at("state_version")), reason, new_id("wait"))
            : store.wake_worker(actor, binding, generation, new_id("wake")));
        return 0;
    }
    if (command == "pump-fake") {
        const auto script = event_fake_script_from_json(parse_json_strict(read_runtime_file(options.required("--script"))));
        const auto workspaces_file = options.take("--workspaces-file");
        const auto paths = workspace_map(workspaces_file.empty() ? Json(Object{}) :
            parse_json_strict(read_runtime_file(workspaces_file, 262144)), database);
        const auto dispositions_file = options.take("--dispositions-file");
        EventPumpOptions pump_options;
        if (!dispositions_file.empty())
            pump_options.amendment_dispositions = parse_json_strict(read_runtime_file(dispositions_file, 262144));
        validate_dispositions(pump_options.amendment_dispositions);
        pump_options.max_calls = options.number("--max-calls", 12, 1);
        pump_options.max_worker_calls_per_task = options.number("--worker-calls", 1, 1);
        if (pump_options.max_calls > 64 || pump_options.max_worker_calls_per_task > 16)
            throw Error(ErrorCode::InvalidInput, "pump limit is 1..64 calls and 1..16 Worker calls per task");
        const auto manifest_path = options.take("--sandbox-manifest");
        if (!manifest_path.empty() && paths.empty())
            throw Error(ErrorCode::InvalidInput, "sandbox manifest requires explicit task workspaces");
        const auto manifest = manifest_path.empty() ? Json(Object{}) : parse_json_strict(read_runtime_file(manifest_path, 262144));
        options.finish();
        const SchemaRegistry schemas(source / "schemas");
        Store store(database, schema_sql, migration_sql, StoreOpenMode::Takeover, tools_migration_sql, events_migration_sql, workflow_migration_sql);
        std::vector<std::unique_ptr<WorkerTools>> owners;
        std::map<std::string, WorkerTools*> tools_by_node;
        for (const auto& [node, path] : paths) {
            owners.push_back(std::make_unique<WorkerTools>(store, path, manifest));
            tools_by_node.emplace(node, owners.back().get());
        }
        output(EventRuntime(store, schemas, std::move(tools_by_node)).pump(request, script, pump_options));
        return 0;
    }
    if (command == "tool-query") {
        const auto task = options.required("--task");
        const auto workspace = options.required("--workspace");
        const auto tool = options.required("--tool");
        const auto arguments = parse_json_strict(read_runtime_file(options.required("--input"), 262144));
        const auto manifest_path = options.take("--sandbox-manifest");
        const auto manifest = manifest_path.empty() ? Json(Object{}) : parse_json_strict(read_runtime_file(manifest_path, 262144));
        options.finish();
        Store store(database, schema_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql, events_migration_sql, workflow_migration_sql);
        const auto binding = store.binding(task);
        if (binding.request_id != request) throw Error(ErrorCode::Unauthorized, "task belongs to another request");
        const auto actor = store.host_actor(Role::Worker, request, binding.team_id);
        WorkerTools worker_tools(store, workspace, manifest, true);
        worker_tools.bind(actor);
        output(worker_tools.diagnostic(actor, binding, tool, arguments)); return 0;
    }
    if (command == "show" || command == "events" || command == "budget" || command == "config-snapshot") {
        const auto after = command == "events" ? options.number("--after", 0) : 0;
        const auto limit = command == "events" ? options.number("--limit", 100, 1) : 100;
        const auto role = command == "config-snapshot" ? role_from_string(options.take("--role", "worker")) : Role::Root;
        options.finish();
        Store store(database, schema_sql, migration_sql, StoreOpenMode::ReadOnly, tools_migration_sql, events_migration_sql, workflow_migration_sql);
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
        Store store(database, schema_sql, migration_sql, StoreOpenMode::Takeover, tools_migration_sql, events_migration_sql, workflow_migration_sql);
        const auto actor = store.host_actor(Role::Root, request);
        output(store.amend(actor, integer(store.request(request).at("state_version")), amendment, catalog, new_id("amend")));
        return 0;
    }
    if (command == "cancel") {
        options.finish(); Store store(database, schema_sql, migration_sql, StoreOpenMode::Takeover, tools_migration_sql, events_migration_sql, workflow_migration_sql);
        output(store.cancel_request(store.host_actor(Role::Root, request), integer(store.request(request).at("state_version")), new_id("cancel")));
        return 0;
    }
    const auto script = fake_model_steps_from_json(parse_json_strict(read_runtime_file(options.required("--script"))));
    const auto disposition = options.take("--disposition");
    const auto workspace = options.take("--workspace");
    const auto manifest_path = options.take("--sandbox-manifest");
    const auto worker_calls = options.number("--worker-calls", 1, 1);
    if (worker_calls > 16 || (workspace.empty() && (worker_calls != 1 || !manifest_path.empty())))
        throw Error(ErrorCode::InvalidInput, "multiple Worker calls and sandbox manifests require --workspace; limit is 16");
    const auto manifest = manifest_path.empty() ? Json(Object{}) : parse_json_strict(read_runtime_file(manifest_path, 262144));
    options.finish();
    const SchemaRegistry schemas(source / "schemas");
    Store store(database, schema_sql, migration_sql, StoreOpenMode::Takeover, tools_migration_sql, events_migration_sql, workflow_migration_sql);
    std::unique_ptr<WorkerTools> worker_tools;
    if (!workspace.empty()) worker_tools = std::make_unique<WorkerTools>(store, workspace, manifest);
    output(OfflineRuntime(store, schemas, worker_tools.get()).run(request, script, disposition, worker_calls));
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
