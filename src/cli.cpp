#include "wls/offline_runtime.hpp"
#include "wls/event_runtime.hpp"
#include "wls/worker_tools.hpp"
#include "wls/delivery.hpp"
#include "wls/cleanup_runtime.hpp"
#include "wls/protocol_codec.hpp"
#include "wls/sse_replay.hpp"
#include "wls/runtime_resources.hpp"
#include "wls/migrations.hpp"
#include "wls/billing.hpp"
#include "wls/single_agent.hpp"
#include "amendment_disposition.hpp"
#include "controls_detail.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>

#include <openssl/evp.h>

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
    bool has(const std::string& key) const { return values_.contains(key); }
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

void output(const Json& value) {
    std::cout << canonical_json(value) << '\n';
    std::cout.flush();
    if (!std::cout) throw Error(ErrorCode::Storage, "delivery_of_report_failed: stdout write or flush failed");
}

void output_error(std::string_view code, std::string_view message) noexcept {
    // Error text can come from POSIX byte paths or third-party exceptions.
    // Never let reporting a rejected input throw another uncaught exception.
    try {
        constexpr std::size_t maximum = 4096;
        const auto prefix = message.substr(0, maximum);
        Object report{{"error", code}, {"message", prefix}};
        if (message.size() > maximum) {
            report["message_truncated"] = true;
            report["message_bytes"] = message.size();
        }
        std::string encoded;
        try { encoded = canonical_json(report); }
        catch (const Error&) {
            constexpr char digits[] = "0123456789abcdef";
            std::string hex;
            hex.reserve(prefix.size() * 2);
            for (const char character : prefix) {
                const auto byte = static_cast<unsigned char>(character);
                hex.push_back(digits[byte >> 4]);
                hex.push_back(digits[byte & 15]);
            }
            report["message"] = "diagnostic text is encoded in message_hex";
            report["message_encoding"] = "hex";
            report["message_hex"] = std::move(hex);
            report["message_bytes"] = message.size();
            report["message_truncated"] = message.size() > maximum;
            encoded = canonical_json(report);
        }
        std::cerr << encoded << '\n';
    } catch (...) {
        std::fputs("{\"error\":\"runtime_error\",\"message\":\"cannot serialize CLI error\"}\n", stderr);
    }
}

Json input_path_details(const std::filesystem::path& path, std::string_view origin) {
    const auto input = path.string();
    if (input.empty() || input.size() > 4096 || input.find('\0') != std::string::npos)
        throw Error(ErrorCode::InvalidInput, "diagnostic input path must be bounded nonempty text");
    std::filesystem::path absolute;
    try { absolute = std::filesystem::absolute(path).lexically_normal(); }
    catch (const std::filesystem::filesystem_error&) {
        throw Error(ErrorCode::InvalidInput, "cannot interpret diagnostic input path");
    }
    if (absolute.string().size() > 4096)
        throw Error(ErrorCode::InvalidInput, "absolute diagnostic input path exceeds capacity");
    Json result = Object{{"origin", origin}, {"input_path", input},
        {"absolute_lexical_path", absolute.string()}, {"path_kind", "lexical_interpretation"}};
    // Paths are POSIX byte strings, but this interface promises UTF-8 JSON.
    // Validate before a read can embed an unrepresentable path in an error.
    static_cast<void>(canonical_json(result));
    return result;
}

FrozenRequestConfig live_config(Options& options, const RuntimeResources& resources,
                               Json* provenance = nullptr) {
    const auto& source = resources.root();
    const bool system_supplied = options.has("--config");
    const bool functions_supplied = options.has("--functions-dir");
    const bool prompts_supplied = options.has("--prompts-dir");
    const bool function_supplied = options.has("--function");
    const bool capability_supplied = options.has("--capability");
    if (!resources.source_tree() && (!system_supplied || !functions_supplied))
        throw Error(ErrorCode::InvalidInput,
            "installed runtime requires explicit --config and --functions-dir");
    const std::filesystem::path system = options.take("--config", (source / "config/examples/system.yaml").string());
    const std::filesystem::path functions = options.take("--functions-dir", (system.parent_path() / "functions").string());
    const std::filesystem::path prompts = options.take("--prompts-dir", (source / "prompts").string());
    const auto function = options.take("--function", "general");
    const auto capability = options.take("--capability");
    if (provenance) {
        *provenance = Object{
            {"system", input_path_details(system, system_supplied ? "supplied" : "default")},
            {"functions", input_path_details(functions, functions_supplied ? "supplied" : "default")},
            {"prompts", input_path_details(prompts, prompts_supplied ? "supplied" : "default")}};
    }
    auto frozen = load_config_catalog(system, functions, prompts).freeze(function, capability);
    if (!prompts_supplied) resources.validate_frozen_prompts(frozen);
    if (provenance) {
        auto& inputs = provenance->as_object();
        inputs["function"] = Object{{"origin", function_supplied ? "supplied" : "default"}, {"value", frozen.function_id()}};
        inputs["capability"] = Object{{"origin", capability_supplied ? "supplied" : "default"}, {"value", frozen.capability_id()}};
        inputs["snapshot_sha256"] = frozen.sha256();
        inputs["snapshot_scope"] = "current_explicit_configuration";
    }
    return frozen;
}

#include "protocol_cli.inc"

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

void check_delivery_root(Store& store, const std::string& request_id,
        const std::filesystem::path& database, const Json& identity) {
    const std::filesystem::path root = text(identity.at("path"));
    const auto control = std::filesystem::weakly_canonical(std::filesystem::absolute(database));
    if (path_contains(root, control))
        throw Error(ErrorCode::Unauthorized, "delivery directory must not contain the control database");
    for (const auto& task : store.tasks(request_id)) {
        const auto workspace = store.workspace(text(task.at("team_id")));
        if (workspace.is_null()) continue;
        const std::filesystem::path path = text(workspace.at("canonical_path"));
        if (path_contains(root, path) || path_contains(path, root) ||
            (identity.at("device") == workspace.at("device") && identity.at("inode") == workspace.at("inode")))
            throw Error(ErrorCode::Unauthorized, "delivery directory must be separate from business workspaces");
    }
}

void check_delivery_scope(const Json& delivery, const std::string& request_id,
                          const Json& root_identity) {
    if (text(delivery.at("request_id")) != request_id)
        throw Error(ErrorCode::Unauthorized, "delivery belongs to another request");
    if (delivery.at("frozen_plan").at("root_identity") != root_identity)
        throw Error(ErrorCode::Conflict, "delivery directory identity differs from the frozen publication intent");
}

Json publish_bundle(DeliveryDirectory& directory, const Json& delivery) {
    const auto result = directory.publish(delivery.at("frozen_plan"));
    if (result.status != DeliveryMutationStatus::Succeeded) {
        const auto code = result.status == DeliveryMutationStatus::OutcomeUnknown
            ? ErrorCode::UnknownOutcome : result.error_code.value_or(ErrorCode::Storage);
        throw Error(code, "delivery publication " + std::string(delivery_mutation_status_name(result.status)) +
            "; retained intent " + text(delivery.at("delivery_id")) + "; " + canonical_json(result.evidence));
    }
    return result.publication;
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
        if (!is_amendment_disposition(kind) || rationale.empty() ||
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
                  {"workflow_runtime_enabled", store.workflow_runtime_enabled()},
                  {"acceptance_runtime_enabled", store.acceptance_runtime_enabled()}, {"budget", store.budget(id)}};
}

#include "live_cli.inc"
#include "billing_cli.inc"

void help() {
    std::cout << R"(wls 0.11.1-r3.3c-audit — signed billing and a continuous single Agent

Resources are discovered relative to the executable, never the current directory.
Use --resource-root PATH for an installed bundle or --source-root PATH for a checkout.
Installed configuration commands require explicit --config and --functions-dir.
Outputs are JSON; errors go to stderr. Network execution is disabled by default.
Request writes recover only their target; submit only admits/replays.
init is explicit all-request recovery and migration maintenance.

  wls config check [--config FILE] [--function ID]
  wls config explain [--config FILE] [--function ID] [--role worker]
  wls schema check --schema NAME --input FILE [--model-input]
  wls doctor [--config FILE] [--template default] [--sandbox-manifest FILE]
  wls resources [--resource-root PATH | --source-root PATH]
  wls encode-model --profile FILE --request FILE [--stream true|false]
  wls replay-model --profile FILE --request FILE --http-response FILE [--http-status 200]
  wls replay-model --profile FILE --request FILE --sse-stream FILE [--chunk-bytes 4096]
  wls replay-model --profile FILE --request FILE --replay FILE
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
  wls run-live --db FILE --request ID --authorization-file FILE [--workspaces-file FILE]
               [--sandbox-manifest FILE] [--billing-policy-file FILE]
  wls run-fake --db FILE --request ID (--script FILE | --scripts-file FILE)
               [--max-calls 1..64] [--worker-calls 1..16]
               [--workspaces-file FILE] [--sandbox-manifest FILE]
  wls pump-fake --db FILE --request ID --script FILE [--workspaces-file FILE]
                [--dispositions-file FILE] [--max-calls 1..64] [--worker-calls 1..16]
                [--sandbox-manifest FILE]
  wls live-status --db FILE --request ID [--billing-policy-file FILE]
  wls pump-live --db FILE --request ID --authorization-file FILE
                [--billing-policy-file FILE] [--workspaces-file FILE] [--sandbox-manifest FILE]
  wls pump-single-live --db FILE --request ID --authorization-file FILE
                [--billing-policy-file FILE] [--workspaces-file FILE] [--sandbox-manifest FILE]
  wls pump-single-fake --db FILE --request ID --script FILE [--workspaces-file FILE]
                [--max-calls N] [--worker-calls N] [--sandbox-manifest FILE]
  wls reconcile-billing --db FILE --request ID --attempt ID --billing-policy-file FILE
                --receipt-file FILE --expected-version N --key KEY
  wls inbox --db FILE --request ID [--role dispatcher] [--task ID] [--limit N]
  wls history --db FILE --request ID --role ROLE [--task ID] [--limit N]
  wls history-page --db FILE --request ID --role ROLE [--task ID] [--after N] [--through N] [--limit N]
  wls history-read --db FILE --request ID --role ROLE [--task ID] --attempt ID --record-sha256 SHA
      --section response|result [--offset N] [--max-bytes N]
  wls questions --db FILE --request ID [--task ID] [--limit N]
  wls submissions --db FILE --request ID --task ID [--limit N]
  wls answer --db FILE --request ID --question ID --expected-version N
             [--text-file FILE] [--selected-option ID]
  wls deliveries --db FILE --request ID [--limit 1..64]
  wls experience --db FILE --request ID [--task ID] [--limit 1..128]
  wls closeout --db FILE --request ID
  wls publish-delivery --db FILE --request ID --delivery-dir DIRECTORY [--delivery ID]
  wls verify-delivery --db FILE --request ID --delivery ID --delivery-dir DIRECTORY
  wls ack-delivery --db FILE --request ID --delivery ID --delivery-dir DIRECTORY
                  --manifest-sha256 HASH --requirements-revision N --plan-revision N
                  --recipient ID --key KEY [--method durable_local|explicit_ack]
  wls wait --db FILE --request ID --task ID --reason TEXT
  wls wake --db FILE --request ID --task ID --wait-generation N
  wls tool-query --db FILE --request ID --task ID --workspace DIRECTORY
                 --tool NAME --input FILE [--sandbox-manifest FILE]
  wls amend --db FILE --request ID --text-file FILE --constraints-file FILE
  wls cancel --db FILE --request ID
  wls pending --db FILE
  wls reconcile-op --db FILE --request ID --op ID --expected-version N
                   --outcome SUCCEEDED|FAILED|CANCELLED|TIMED_OUT --evidence-file FILE --key KEY
  wls allocate-workspace --db FILE --request ID --task ID --parent DIRECTORY --key KEY
  wls abandon-workspace --db FILE --request ID --resource ID
                        --expected-state-version N --key KEY --reason TEXT
  wls owned-workspaces --db FILE --request ID
  wls cleanup-workspaces --db FILE --request ID --confirm-request ID [--max-unlinks N]

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
Dispatcher accept_candidate evaluates one captured current candidate separately.
publish-delivery assembles only previously accepted original bytes and publishes
an immutable package; --delivery resumes its exact saved intent. It does not sign
a receipt. ack-delivery verifies and syncs that whole package again, then records
the exact recipient/method/version/hash intent. durable_local means a persisted
local package, not proof that a person read it. deliveries, experience, closeout
and verify-delivery are read-only and never acknowledge delivery. Experience
jobs remain pending until an explicit TeamLead report. Owned cleanup is disabled
by default. With cleanup enabled in the frozen request configuration, allocate-workspace
creates a new owned work directory using a persisted staging receipt and no-overwrite
publication. Exact retries verify the original directory generations. abandon-workspace
only retires an unbound PREPARED allocation record; its paths and evidence remain retained.
The original key returns that record; a fresh key may allocate a new resource. Abandonment
does not delete anything, clear UNKNOWN, or acknowledge cleanup. Populated older databases
require explicit init maintenance before a request-scoped write can upgrade them.
cleanup-workspaces requires the exact request confirmation, a durable success receipt,
completed experience/accounting and original process exit facts. It freezes the inventory
before bounded deletion and resumes only that inventory. It never recreates a lost delivery.
The ownership envelope, private database records and delivery package remain retained;
workspace completion does not mark the global cleanup job DONE.
reconcile-op is a trusted-local-host Root control: it records a definite host
observation for one UNKNOWN operation with explicit CAS and an idempotency key.
Evidence does not rewrite original output or prove descendant exit for cleanup.
It neither runs a command nor automatically resumes business.
capacity is read-only. capacity-checkpoint requires Root, --expected-window,
--evidence-file and --key, and refuses unsettled effects or unknown accounting.
It advances a storage admission window without deleting audit or resetting budgets.
wait/wake are local host controls;
a wake leaves instruction, AMEND and UNKNOWN gates in force. An explicit
workspace enables bounded file tools and process execution with verified strong
isolation. A process requires a pinned host manifest and delegated cgroup v2;
missing prerequisites refuse execution. Worker tools do not establish delivery.
tool-query accepts only registered diagnostics. Read commands do not fence a writer.
Opening a writer fences interrupted executions and keeps their unknown budgets.
doctor observes prerequisites without changing host policy or running template checks.
encode-model and replay-model only read local fixtures, never resolve credentials,
open Store or send requests. Replay returns 4 with JSON for Error/Interrupted results.
live-status reads frozen identities and accounting without resolving credentials.
pump-live requires an explicit request/config/profile-bound host authorization;
only the frozen credential environment reference is resolved after preflight.
Unknown usage stops further paid attempts. Signed billing requires an explicitly
frozen challenge adapter, external public-key policy and v2 authorization binding
the exact captured policy bytes. No arbitrary JSON value can verify billing.
reconcile-billing authenticates a late receipt against an original settled attempt;
it performs no network call, model call or business response application.
pump-single-live/fake require the original frozen single-agent-role-broker-v1 policy
and continue one request-wide model history through the original role permissions.
pump-live never automatically retries a paid attempt or publishes delivery.
No command starts a daemon.
)";
}

// G1-6 Controls v1 (FINAL v24 R): the bounded multi-batch run driver shared by
// run-live and run-fake --scripts-file. Strong STOP first, then complete
// business delivery or the model milestone, then ordinary caps together with
// real progress and an accurate runnable target; every decision consumes the
// complete latest durable state reread in the quiet window after any local
// write (including the warning observer). Reports stay bounded by construction.
Json controls_run_driver(Store& store, const std::string& request, std::int64_t batch_limit,
                         bool fake_execution, const Json& identity_fields,
                         const std::function<Json(std::int64_t)>& run_batch,
                         const std::function<Json(const Json&, std::int64_t)>& next_target = {}) {
    if (batch_limit < 1 || batch_limit > 64)
        throw Error(ErrorCode::Quota, "run-* requires at least one remaining original attempt");
    const auto jstr = [](const Json& value, const char* key) -> std::string {
        if (!value.is_object()) return {};
        const auto* found = value.as_object().if_contains(key);
        return found && found->is_string() ? std::string(found->as_string()) : std::string();
    };
    const auto jget = [](const Json& value, const char* key) -> Json {
        if (!value.is_object()) return nullptr;
        const auto* found = value.as_object().if_contains(key);
        return found ? *found : Json(nullptr);
    };
    const auto marker = store.controls_admission(request);
    // F2 repair: a present-but-unreadable marker row (including stored JSON
    // null) is NOT legacy. True absence is exactly the gate's "legacy" status;
    // anything else present is validated by the gate, which reports a corrupt
    // present marker as the typed stop below instead of degrading it.
    if (jstr(store.controls_business_gate(request), "status") == "legacy")
        throw Error(ErrorCode::InvalidInput,
                    "controls_legacy_unavailable: this request was admitted before Controls v1; "
                    "the original explicit pump commands keep their legacy semantics");
    Array summaries, stop_classes, raw_stop_codes;
    summaries.reserve(64);
    Json primary_stop = nullptr;
    bool report_complete = true, model_milestone = false, business_delivered = false;
    bool history_complete = false, delivery_receipt_verified = false;
    std::int64_t started = 0, completed = 0;
    // LIVE completeness (FINAL v24 repair02): the network/provider totals are
    // the conjunction of verified coverage over the actually observed batches.
    // A missing, partial or throwing batch makes the affected totals unknown
    // and they stay unknown; a later complete batch never hides that missing
    // prefix. Known Fake zero needs the Fake execution type with every started
    // Fake batch actually returned.
    // R03_REPORT/F4_COUNTS: one production checked-totals accumulator drives
    // every report total; overflow or a malformed observation makes the
    // affected totals unknown without corrupting the known prefix.
    controls::detail::run_totals totals;
    std::int64_t event_cursor = 0;
    std::string warning_status = "not_observed";
    const auto add_class = [&](const std::string& code) {
        if (code.empty()) return;
        for (const auto& existing : stop_classes)
            if (existing == Json(code)) return;
        stop_classes.push_back(Json(code));
    };
    const auto add_raw = [&](const std::string& code) {
        if (code.empty()) return;
        for (const auto& existing : raw_stop_codes)
            if (existing == Json(code)) return;
        raw_stop_codes.push_back(Json(code));
    };
    // F4 repair (FINAL v24 repair record): stop_classes uses exactly the fixed
    // wls-controls-run-v1 vocabulary; raw cap/stop codes stay in bounded
    // separate detail (blocked_codes, batch_stop_reason, raw_stop_codes). An
    // unmapped raw code is handled as a conservative typed STOP, never silent
    // continuation.
    const auto fixed_stop_class = [](const std::string& raw) -> std::string {
        if (raw == "cancelled" || raw == "deadline" || raw == "needs_accounting" ||
            raw == "correction_limit" || raw == "stagnation" || raw == "budget" ||
            raw == "context" || raw == "capacity" || raw == "storage" ||
            raw == "permission" || raw == "protocol" || raw == "transport" ||
            raw == "failure_tail" || raw == "input" || raw == "host_limit" ||
            raw == "unclassified")
            return raw;
        if (raw == "controls_clock_unavailable" || raw == "manager_limit" ||
            raw == "worker_limit" || raw == "call_limit" || raw == "run_batch_limit" ||
            raw == "workspace_missing")
            return "host_limit";
        if (raw == "no_new_progress") return "stagnation";
        if (raw == "prior_failure_tail" || raw == "operation_incomplete") return "failure_tail";
        if (raw == "incomplete_tail") return "needs_accounting";
        if (raw == "controls_marker_corrupt" || raw == "amendment_pending" ||
            raw == "waiting" || raw == "rework_blocked" || raw == "submission_pending" ||
            raw == "acceptance_pending" || raw == "script_exhausted")
            return "input";
        if (raw == "capacity_waiting_input" || raw == "report_capacity" || raw == "report_incomplete")
            return "capacity";
        if (raw == "error") return "unclassified";
        return {};
    };
    // R03_REPORT: all checked report-total arithmetic now goes through the
    // shared production run_totals helper (controls_detail.hpp), which never
    // mutates a published known-prefix accumulator on overflow.
    // B2 repair (FINAL v24 repair02) superseded by the shared production
    // streaming helper in controls_detail.hpp (R03_REPORT): the digest and the
    // byte count now come from one bounded traversal of the raw pump JSON.
    // First terminal scope wins primary reporting; the fixed class and the raw
    // code are both retained even when another reason already won primary.
    const auto stop_run = [&](const std::string& raw) {
        if (primary_stop.is_null()) {
            const auto fixed = fixed_stop_class(raw);
            primary_stop = Json(fixed.empty() ? std::string("unclassified") : fixed);
        }
        add_raw(raw);
        const auto fixed = fixed_stop_class(raw);
        add_class(fixed.empty() ? std::string("unclassified") : fixed);
    };
    // run-* observes the budget once before the first batch; the observer may
    // itself write, which is exactly why every later decision rereads fully.
    warning_status = jstr(store.controls_observe_budget(request), "warning_status");
    const auto& stagnation_entry = [&]() -> Json {
        // R03_DECISION: a stopped gate (deadline/corrupt) carries no stagnation
        // member; reading it must yield that gate's own typed stop, never an
        // unhandled at() exception.
        const auto gate = store.controls_business_gate(request);
        if (const auto* found = gate.is_object() ? gate.as_object().if_contains("stagnation") : nullptr;
                found && found->is_object())
            return *found;
        return nullptr;
    }();
    if (jget(stagnation_entry, "prior_failure_tail") == true) {
        stop_run("prior_failure_tail");
    } else if (jget(stagnation_entry, "incomplete_tail") == true || jget(stagnation_entry, "active_tail") == true) {
        stop_run("incomplete_tail");
    } else {
        const auto gate = store.controls_business_gate(request);
        if (jstr(gate, "status") == "stop") stop_run(jstr(gate, "stop_class"));
    }
    const auto attempts_used = [&](const Json& budget) {
        const auto value = jget(budget, "attempts");
        try { const auto used = integer(value); return used >= 0 ? used : -1; }
        catch (...) { return std::int64_t{-1}; }
    };
    // Prepare the typed failure before a batch can produce durable effects.
    const Error report_delivery_failed(ErrorCode::Storage,
        "delivery_of_report_failed: bounded batch summary unavailable");
    const auto summary_fits = [](const Json& summary) {
        return controls::detail::canonical_sha256_stream(summary,
            controls::detail::canonical_stream_chunk_default, 8191).ok;
    };
    const auto stop_incomplete_report = [&] {
        if (primary_stop == "model_milestone" || primary_stop == "business_delivered")
            primary_stop = nullptr;
        stop_run("report_incomplete");
    };
    for (std::int64_t index = 1; primary_stop.is_null() && index <= batch_limit; ++index) {
        const auto gate = store.controls_business_gate(request);
        if (jstr(gate, "status") == "stop") {
            stop_run(jstr(gate, "stop_class"));
            break;
        }
        // F3 repair (v12 §2/§5): the safe-tail gate is re-checked before EVERY
        // batch on the stable post-write snapshot, not only at driver entry. A
        // prior eligible failure tail or an incomplete/unsettled/missing-
        // feedback tail forbids ordinary-cap continuation and preserves its
        // actual reason; no fresh quota is minted for any role.
        if (const auto* stagnation = gate.is_object() ? gate.as_object().if_contains("stagnation") : nullptr;
                stagnation && stagnation->is_object()) {
            if (jget(*stagnation, "prior_failure_tail") == true) { stop_run("prior_failure_tail"); break; }
            if (jget(*stagnation, "incomplete_tail") == true) { stop_run("incomplete_tail"); break; }
            if (jget(*stagnation, "active_tail") == true) { stop_run("incomplete_tail"); break; }
        }
        const Json budget_before = store.budget(request);
        if (attempts_used(budget_before) < 0) { report_complete = false; stop_run("report_incomplete"); break; }
        // B2 repair: capture the durable event high-water immediately before
        // the real invocation; the first batch's interval starts here and
        // never swallows pre-run history.
        std::int64_t events_highwater = event_cursor;
        bool highwater_complete = true;
        {
            std::int64_t scan = event_cursor;
            for (int pages = 0; pages < 128; ++pages) {
                const auto page = store.events(request, scan, 1000);
                if (page.empty()) break;
                for (const auto& event : page) {
                    const auto seq = jget(event, "seq");
                    if (seq.is_int64()) events_highwater = seq.as_int64();
                    else highwater_complete = false;
                }
                scan = events_highwater;
                if (static_cast<std::int64_t>(page.size()) < 1000) break;
                if (pages == 127) highwater_complete = false;
            }
            event_cursor = events_highwater;
        }
        // R03_DECISION: incomplete event high-water evidence stops before the
        // first/next invocation; the boundary is never guessed.
        if (!highwater_complete) {
            report_complete = false;
            stop_run("report_incomplete");
            break;
        }
        Json pump_report, first_terminal_scope;
        controls::detail::canonical_digest_result canonical_digest;
        // Allocate the bounded emergency shape before invoking a batch.
        Object emergency;
        try {
            emergency = Object{{"index", index}, {"completed", false}, {"report_sha256", nullptr},
                {"report_bytes", nullptr}, {"new_attempts", nullptr}, {"budget_before", budget_before},
                {"budget_after", nullptr}, {"event_seq_first", nullptr}, {"event_seq_last", nullptr},
                {"attempt_set_sha256", nullptr}, {"network_started_calls", nullptr},
                {"network_count_complete", false}, {"continued", false}};
        } catch (...) {
            throw report_delivery_failed;
        }
        bool emergency_has_raw_identity = false;
        const auto retain_raw_identity = [&] {
            emergency["report_sha256"] = canonical_digest.sha256_hex_value;
            emergency["report_bytes"] = canonical_digest.bytes;
            emergency_has_raw_identity = true;
        };
        bool returned = false, summary_truncated = false;
        const auto run_and_summarize = [&] {
        // R03_REPORT: batches_started is incremented immediately BEFORE the
        // actual invocation; completed only when it returns.
        ++started;
        try {
            pump_report = run_batch(index);
            returned = true;
            ++completed;
        } catch (const std::exception& error) {
            // F4 repair: a throwing batch was started but never completed; its
            // durable partial effects stay visible through the Store evidence.
            // LIVE completeness: a throwing batch leaves its observation
            // coverage incomplete; the affected totals stay unknown. An
            // unknown report length/hash is null, never zero.
            report_complete = false;
            const auto message = std::string(error.what());
            stop_run(message.rfind("script_exhausted", 0) == 0 ? std::string("script_exhausted")
                                                               : std::string("error"));
        } catch (...) {
            report_complete = false;
            stop_run("error");
        }
        // Success, throw and invalid/oversized reports all observe the SAME
        // durable originals before any exit; raw report availability is not
        // a precondition for observing reservations, events or network facts.
        const Json budget_after = store.budget(request);
        emergency["completed"] = returned;
        emergency["budget_after"] = budget_after;
        bool observed_counts = true, observed_network = true;
        const auto observed_delta = controls::detail::run_totals::checked_delta(
            attempts_used(budget_after), attempts_used(budget_before), observed_counts);
        std::set<std::string> observed_ids;
        std::int64_t observed_first = 0, observed_last = events_highwater, observed_sent = 0, observed_reservations = 0;
        for (int pages = 0; pages < 128; ++pages) {
            const auto page = store.events(request, observed_last, 1000);
            for (const auto& event : page) {
                const auto seq = jget(event, "seq");
                if (!seq.is_int64() || seq.as_int64() <= observed_last) { observed_counts = observed_network = false; continue; }
                if (!observed_first) observed_first = seq.as_int64();
                observed_last = seq.as_int64();
                const auto type = jstr(event, "type");
                if (type == "reserve_attempt") ++observed_reservations;
                if (type == "reserve_attempt" || type == "claim_live_send")
                    observed_ids.insert(jstr(jget(event, "payload"), "attempt_id"));
            }
            if (page.size() < 1000) break;
            if (pages == 127) observed_counts = observed_network = false;
        }
        Array originals;
        if (observed_reservations != observed_delta) observed_counts = observed_network = false;
        for (const auto& id : observed_ids) {
            if (id.empty()) { observed_counts = observed_network = false; continue; }
            originals.push_back(Json(id));
            (void)store.attempt(id);
            if (!fake_execution) {
                const auto original = store.live_attempt(id);
                const auto sent = jget(jget(original, "transport"), "network_started");
                if (sent.is_bool()) { if (sent.as_bool()) ++observed_sent; }
                else if (!jget(original, "send_claim").is_null()) observed_network = false;
            }
        }
        emergency["event_seq_first"] = observed_first;
        emergency["event_seq_last"] = observed_last;
        emergency["attempt_set_sha256"] = sha256_hex(canonical_json(originals));
        emergency["new_attempts"] = observed_counts ? Json(observed_delta) : Json(nullptr);
        emergency["network_started_calls"] = observed_network ? Json(observed_sent) : Json(nullptr);
        emergency["network_count_complete"] = observed_network;
        totals.add_attempts(observed_delta);
        totals.add_reservation(observed_delta);
        if (!observed_counts) totals.counts_complete = false;
        totals.observe_network(observed_sent, observed_network, !fake_execution);
        // R03_REPORT: the canonical raw-report digest and byte count come from
        // ONE bounded streaming traversal of the actual pump JSON — the same
        // canonical semantics as the protected reference (sorted keys, array
        // order, escaping, finite numbers, depth limit) — without ever
        // materializing a second full canonical string.
        if (returned)
            canonical_digest = controls::detail::canonical_sha256_stream(
                pump_report, controls::detail::canonical_stream_chunk_default, 4U * 1024U * 1024U);
        // A missing/invalid raw report still keeps the durable observations
        // above, with a null hash/length and no permission for a next batch.
        if (!canonical_digest.ok) return false;
        retain_raw_identity();
        const auto& canonical_sha = canonical_digest.sha256_hex_value;
        const auto canonical_bytes = canonical_digest.bytes;
        Array attempt_ids, blocked_codes;
        if (jget(pump_report, "calls").is_array())
            for (const auto& call : pump_report.at("calls").as_array()) {
                const auto attempt = jget(call, "attempt_id");
                if (!attempt.is_null()) attempt_ids.push_back(attempt);
            }
        if (jget(pump_report, "blocked").is_array())
            for (const auto& entry : pump_report.at("blocked").as_array()) {
                const auto code = jstr(entry, "code");
                if (!code.empty()) {
                    blocked_codes.push_back(Json(code));
                    if (first_terminal_scope.is_null())
                        first_terminal_scope = Object{{"role", jget(entry, "role")},
                                                      {"node_id", jget(entry, "node_id")},
                                                      {"code", code}};
                }
            }
        const auto batch_stop = [&] {
            const auto value = jget(pump_report, "stop_reason");
            return value.is_string() ? std::string(value.as_string()) : std::string("quiescent");
        }();
        // F1/F4 repair: raw codes are always retained as bounded detail and
        // mapped onto the fixed vocabulary, even when the run continues.
        add_raw(batch_stop);
        if (const auto fixed = fixed_stop_class(batch_stop); !fixed.empty()) add_class(fixed);
        else if (batch_stop != "quiescent") add_class("unclassified");
        for (const auto& code : blocked_codes) {
            const auto raw = std::string(code.as_string());
            add_raw(raw);
            const auto fixed = fixed_stop_class(raw);
            add_class(fixed.empty() ? std::string("unclassified") : fixed);
        }
        // Event-sequence interval of THIS batch only: from the pre-invocation
        // high-water captured above; a bounded scan keeps the summary inside
        // its bound and pre-run history never leaks into the first interval.
        std::int64_t event_first = 0, event_last = events_highwater;
        bool event_interval_complete = highwater_complete;
        {
            std::int64_t scan = events_highwater;
            for (int pages = 0; pages < 128; ++pages) {
                const auto page = store.events(request, scan, 1000);
                if (page.empty()) break;
                for (const auto& event : page) {
                    const auto seq = jget(event, "seq");
                    if (!seq.is_int64()) continue;
                    if (event_first == 0) event_first = seq.as_int64();
                    event_last = seq.as_int64();
                }
                scan = event_last;
                if (static_cast<std::int64_t>(page.size()) < 1000) break;
                if (pages == 127) event_interval_complete = false;
            }
            if (!event_interval_complete) report_complete = false;
            event_cursor = event_last;
        }
        Object batch_class_counts;
        const auto count_class = [&](const std::string& raw) {
            const auto fixed = fixed_stop_class(raw);
            if (fixed.empty()) return;
            if (const auto* found = batch_class_counts.if_contains(fixed))
                batch_class_counts[fixed] = found->as_int64() + 1;
            else
                batch_class_counts[fixed] = 1;
        };
        count_class(batch_stop);
        for (const auto& code : blocked_codes) count_class(std::string(code.as_string()));
        const auto budget_view = [&](const Json& budget) {
            const auto spent = jget(budget, "spent_cost"), reserved = jget(budget, "reserved_cost");
            std::int64_t estimated = 0;
            const bool e_ok = spent.is_int64() && reserved.is_int64() && spent.as_int64() >= 0 && reserved.as_int64() >= 0 &&
                !__builtin_add_overflow(spent.as_int64(), reserved.as_int64(), &estimated);
            if (!e_ok || attempts_used(budget) < 0 || !jget(budget, "state_version").is_int64() ||
                    integer(jget(budget, "state_version")) < 1 || !jget(budget, "max_attempts").is_int64() ||
                    integer(jget(budget, "max_attempts")) < 0) report_complete = false;
            return Object{{"attempts", jget(budget, "attempts")},
                          {"max_attempts", jget(budget, "max_attempts")},
                          {"spent_cost", spent},
                          {"reserved_cost", reserved},
                          {"e_spent_plus_reserved", e_ok ? Json(estimated) : Json(nullptr)},
                          {"state_version", jget(budget, "state_version")}};
        };
        // Any local write (including this observation) invalidates earlier
        // reads; the complete reread below happens after it in the quiet window
        // between pumps, with no callback or concurrent writer active, and the
        // continuation decision consumes only this latest stable snapshot.
        warning_status = jstr(store.controls_observe_budget(request), "warning_status");
        (void)store.tasks(request);
        (void)store.experience_jobs(request);
        (void)store.request_inbox(request);
        const auto current_request = store.request(request);
        const auto budget_after_reread = store.budget(request);
        const auto gate_after = store.controls_business_gate(request);
        // B1 repair: ONE shared factual predicate drives both the summary
        // semantic_progress and the outer continuation — at least one NEW
        // original attempt AND at least one NEW successfully committed
        // semantic transition (a settled completed accepted attempt or a
        // passed candidate acceptance) proved by original attempt/feedback/
        // event/binding facts committed inside this batch. A budget delta,
        // counters, epochs, state versions, notifications, hash growth,
        // replay or a successful sibling with an unsafe tail is never
        // semantic progress.
        bool attempts_delta_ok = true;
        const auto batch_new_attempts = controls::detail::run_totals::checked_delta(
            attempts_used(budget_after_reread), attempts_used(budget_before), attempts_delta_ok);
        if (!attempts_delta_ok) totals.counts_complete = false;
        const bool new_attempt = attempts_delta_ok && batch_new_attempts > 0;
        std::int64_t transition_max_seq = 0;
        std::string transition_witness_identity;
        bool transition_witness_valid = true;
        {
            const auto witness = jget(jget(gate_after, "stagnation"), "success_witness");
            const auto max_seq = jget(witness, "max_event_seq");
            const auto witness_complete = jget(witness, "witness_complete");
            if (max_seq.is_int64()) transition_max_seq = max_seq.as_int64();
            else transition_witness_valid = false;
            if (!(witness_complete.is_bool() && witness_complete.as_bool()))
                transition_witness_valid = false;
            // R03_DECISION: the bounded witness identity hash resolves back to
            // the exact original attempts/events; a bare seq number is not a
            // substitute for the original identity.
            transition_witness_identity = jstr(witness, "identity_sha256");
            if (transition_witness_identity.empty()) transition_witness_valid = false;
        }
        const bool new_semantic_transition =
            transition_witness_valid && transition_max_seq > events_highwater;
        const bool progressed = new_attempt && new_semantic_transition;
        const auto calls = jget(pump_report, "calls");
        std::set<std::string> reported_ids;
        std::int64_t worker_calls = 0;
        bool calls_complete = calls.is_array() && calls.as_array().size() <= static_cast<std::size_t>(INT64_MAX);
        if (calls_complete) for (const auto& call : calls.as_array()) {
            const auto role = jstr(call, "role");
            const auto id = jstr(call, "attempt_id");
            if (id.empty() || !observed_ids.contains(id) || !reported_ids.insert(id).second)
                calls_complete = false;
            else if (jstr(store.attempt(id), "role") != role) calls_complete = false;
            if (role == "worker") ++worker_calls;
            else if (role != "dispatcher" && role != "team_lead") calls_complete = false;
        }
        if (reported_ids != observed_ids) calls_complete = false;
        const auto managers = jget(pump_report, "manager_calls");
        if (!calls_complete || !managers.is_int64() || managers.as_int64() < 0 ||
                managers.as_int64() != static_cast<std::int64_t>(calls.as_array().size()) - worker_calls)
            report_complete = false;
        Object summary{{"index", index},
            {"report_sha256", canonical_sha},
            {"report_bytes", canonical_bytes},
            {"calls", calls_complete ? Json(static_cast<std::int64_t>(calls.as_array().size())) : Json(nullptr)},
            {"worker_calls", calls_complete ? Json(worker_calls) : Json(nullptr)},
            {"manager_calls", jget(pump_report, "manager_calls")},
            {"network_started_calls", jget(pump_report, "network_started_calls")},
            {"budget_before", budget_view(budget_before)},
            {"budget_after", budget_view(budget_after)},
            {"attempt_set_sha256", sha256_hex(canonical_json(attempt_ids))},
            {"blocked_codes", blocked_codes},
            {"batch_stop_reason", batch_stop},
            {"stop_class_counts", std::move(batch_class_counts)},
            {"event_seq_first", event_first},
            {"event_seq_last", event_last},
            {"event_interval_complete", event_interval_complete},
            {"first_terminal_scope", first_terminal_scope},
            {"new_attempts", attempts_delta_ok ? Json(batch_new_attempts) : Json(nullptr)},
            {"semantic_progress", progressed},
            {"progress_witness", Object{{"new_attempt", new_attempt},
                                        {"transition_max_event_seq", transition_max_seq},
                                        {"batch_highwater_before", events_highwater},
                                        {"witness_identity_sha256", transition_witness_identity},
                                        {"witness_complete", transition_witness_valid}}},
            {"completed", true}};
        // R03_FACTS: model milestone and delivery/receipt facts derive from the
        // Store's re-verified durable originals on this stable post-write
        // snapshot — current acceptance plus the exact original-source
        // experience job for milestone, and complete current original receipt
        // verification (including the durable command fingerprint and retained
        // publication) for delivery. Status strings alone prove neither.
        const auto milestone_facts = store.controls_model_milestone(request);
        model_milestone = jget(milestone_facts, "milestone") == true;
        const auto receipt_facts = store.controls_delivery_receipt_facts(request);
        delivery_receipt_verified = jget(receipt_facts, "verified") == true;
        business_delivered = jstr(current_request, "status") == "DELIVERED" &&
            jstr(current_request, "delivery_status") == "CONFIRMED" && delivery_receipt_verified;
        // F1 repair (v12 §2): any blocked correction_limit stops the whole
        // outer run immediately — even with sibling success, a Fake-quiescent
        // main stop or a stronger selected primary reason — and never mints a
        // fresh correction quota for another pump.
        const bool correction_limit_hit = batch_stop == "correction_limit" ||
            std::any_of(blocked_codes.begin(), blocked_codes.end(),
                        [](const Json& code) { return code.as_string() == "correction_limit"; });
        Json continuation_target = nullptr;
        // R03_DECISION: every raw blocked code is inspected on this stable
        // post-write snapshot BEFORE delivery/milestone selection. Any code
        // that is neither an ordinary cap nor a pending code — including
        // unmapped/unclassified raw codes and stale/waiting/nonrunnable
        // states — conservatively stops even when a success status or an
        // ordinary cap coexists.
        const auto ordinary_cap = [](const std::string& code) {
            return code == "manager_limit" || code == "worker_limit" || code == "call_limit";
        };
        const auto pending_code = [](const std::string& code) {
            return code == "submission_pending" || code == "acceptance_pending";
        };
        bool unclassified_code = false;
        std::string unclassified_raw;
        std::set<std::string> pending_codes;
        bool ordinary_capped = batch_stop == "call_limit";
        {
            // The raw cap/failure reason and the blocked entry that carried it
            // (role/node) are retained as bounded detail.
            Json cap_entry;
            for (const auto& entry : jget(pump_report, "blocked").is_array()
                     ? pump_report.at("blocked").as_array() : Array{}) {
                const auto raw = jstr(entry, "code");
                if (raw.empty()) continue;
                if (ordinary_cap(raw)) {
                    ordinary_capped = true;
                    cap_entry = entry;
                } else if (pending_code(raw)) pending_codes.insert(raw);
                else if (!unclassified_code) {
                    unclassified_code = true;
                    unclassified_raw = raw;
                }
            }
        }
        if (batch_stop != "quiescent" && !ordinary_cap(batch_stop) && !pending_code(batch_stop)) {
            stop_run(batch_stop);
        } else if (batch_stop == "cancelled" || batch_stop == "deadline" ||
            batch_stop == "needs_accounting" || batch_stop == "error" ||
            batch_stop == "correction_limit") {
            stop_run(batch_stop);
        } else if (correction_limit_hit) {
            stop_run("correction_limit");
        } else if (unclassified_code) {
            // Unknown/unclassified raw codes conservatively stop; the raw code
            // and its fixed class are both retained even under a stronger
            // primary or an ordinary cap.
            stop_run(unclassified_raw);
        } else if (jstr(gate_after, "status") == "stop") {
            // B1 repair: every strong stop and every unsafe tail on this
            // stable post-write snapshot outranks milestone/continuation; a
            // success, delivery status or milestone never masks a tail.
            stop_run(jstr(gate_after, "stop_class"));
        } else if (const auto* stagnation_after = gate_after.is_object()
                ? gate_after.as_object().if_contains("stagnation") : nullptr;
                   stagnation_after && stagnation_after->is_object() &&
                   (jget(*stagnation_after, "prior_failure_tail") == true ||
                    jget(*stagnation_after, "active_tail") == true ||
                    jget(*stagnation_after, "incomplete_tail") == true)) {
            stop_run(jget(*stagnation_after, "prior_failure_tail") == true
                         ? std::string("prior_failure_tail") : std::string("incomplete_tail"));
        } else if (business_delivered) {
            primary_stop = Json(std::string("business_delivered"));
        } else if (model_milestone) {
            primary_stop = Json(std::string("model_milestone"));
        } else {
            // F3/B1 repair: only ACTUAL ordinary call/worker/manager caps may
            // chain another batch, and only together with the shared progress
            // predicate and an exact runnable captured target resolved from
            // Store facts on this stable snapshot. submission_pending and
            // acceptance_pending are not caps; EVERY observed pending code
            // must additionally prove its exact captured next-role target,
            // not only the first. An empty actor_task_id, an unrelated ready
            // task, waiting/stale/missing-script/workspace or unclassified
            // states never prove a target and are never probed with another
            // batch; a legitimately runnable RUNNING Worker of the capped
            // role's own captured task is not rejected merely for RUNNING.
            ordinary_capped = false;
            bool pending_targets_valid = true;
            Array exact_pending_targets;
            for (const auto& entry : jget(pump_report, "blocked").is_array()
                     ? pump_report.at("blocked").as_array() : Array{}) {
                const auto code = jstr(entry, "code");
                if (!ordinary_cap(code) && !pending_code(code)) continue;
                const auto target = next_target ? next_target(entry, index + 1) : Json(nullptr);
                const bool exact = !target.is_null() && target == jget(entry, "controls_target");
                if (ordinary_cap(code) && exact) {
                    ordinary_capped = true;
                    if (continuation_target.is_null()) continuation_target = target;
                } else if (pending_code(code)) {
                    pending_targets_valid = pending_targets_valid && exact;
                    if (exact) exact_pending_targets.push_back(target);
                }
            }
            ordinary_capped = ordinary_capped && pending_targets_valid;
            summary["pending_targets"] = std::move(exact_pending_targets);
            if (!ordinary_capped) {
                stop_run(pending_codes.empty() ? batch_stop : *pending_codes.begin());
            } else if (!progressed) {
                stop_run("no_new_progress");
            } else if (continuation_target.is_null()) {
                stop_run(batch_stop);  // no exact target: never probe
            } else if (index == batch_limit) {
                stop_run("run_batch_limit");
            }
        }
        summary["continued"] = primary_stop.is_null();
        if (primary_stop.is_null()) {
            summary["continuation_target"] = continuation_target;
        }
        // R03_REPORT/F4_COUNTS: checked totals accumulation — every new
        // attempt holds exactly one new reservation in this Store, so the two
        // checked columns share the same per-batch delta; an unknown delta
        // observation makes the totals unknown without corrupting the prefix.
        if (!attempts_delta_ok) totals.counts_complete = false;
        {
            const auto reported_network = jget(pump_report, "network_started_calls");
            const auto reported_net_complete = jget(pump_report, "network_count_complete");
            if (!fake_execution && (!reported_network.is_int64() || reported_network.as_int64() < 0 ||
                    reported_network.as_int64() != observed_sent || reported_net_complete != true))
                totals.network_count_complete = false;
        }
        // R03_REPORT: any relevant total/count/completeness failure prevents
        // continued=true and another batch; finish the summary before its
        // capacity check so every final flag is included in the bound.
        const bool incomplete = !totals.counts_complete || !totals.network_count_complete || !report_complete;
        if (incomplete) {
            summary["continued"] = false;
            summary["stopped_after_incomplete_totals"] = true;
        }
        Json finalized(std::move(summary));
        if (!summary_fits(finalized)) {
            summary_truncated = true;
            return false;
        }
        if (incomplete) stop_incomplete_report();
        summaries.push_back(std::move(finalized));
        return true;
        };

        bool recovering_from_exception = false;
        try {
            if (run_and_summarize()) continue;
        } catch (...) {
            recovering_from_exception = true;
        }
        const auto append_emergency = [&] {
            report_complete = false;
            if (recovering_from_exception) {
                totals.counts_complete = false;
                // Fake network zero is proven by the execution type.
                if (!fake_execution) totals.network_count_complete = false;
            } else {
                if (summary_truncated) emergency["summary_truncated"] = true;
                else emergency["raw_batch_report_oversize"] = canonical_digest.oversize;
                stop_run("report_capacity");
            }
            if (canonical_digest.ok && !emergency_has_raw_identity) retain_raw_identity();
            if (!first_terminal_scope.is_null()) {
                const auto scope = controls::detail::canonical_sha256_stream(first_terminal_scope);
                if (!scope.ok) return false;
                emergency["first_terminal_scope_sha256"] = scope.sha256_hex_value;
            }
            emergency["completed"] = returned;
            emergency["stopped_after_incomplete_totals"] = true;
            stop_incomplete_report();
            Json fallback(std::move(emergency));
            if (!summary_fits(fallback)) return false;
            summaries.push_back(std::move(fallback));
            return true;
        };
        try {
            if (append_emergency()) break;
        } catch (...) {
            // Any failure to finish the emergency report is terminal.
        }
        throw report_delivery_failed;
    }
    if (primary_stop.is_null()) primary_stop = Json(std::string("run_batch_limit"));
    // F4 repair: history and receipt facts are derived from complete original
    // evidence, never hardcoded. history_complete follows the complete original
    // attempt/feedback coverage on the final gate snapshot; the receipt is
    // read and verified against the retained delivery identity before
    // receipt_verified can become true. Status strings alone prove neither.
    try {
        const auto final_gate = store.controls_business_gate(request);
        const auto* stagnation = final_gate.is_object()
            ? final_gate.as_object().if_contains("stagnation") : nullptr;
        history_complete = jstr(final_gate, "status") != "legacy" &&
            stagnation && stagnation->is_object() && jget(*stagnation, "incomplete_tail") != true &&
            jget(*stagnation, "active_tail") != true;
    } catch (...) {
        report_complete = false;
        primary_stop = nullptr;
        stop_run("report_incomplete");
    }
    // R03_FACTS: the delivery/receipt facts were derived per batch from the
    // Store's complete original receipt verification (including the durable
    // command fingerprint, retained publication and acceptance coverage);
    // business_delivered already requires that verification, and the weaker
    // status-only scan is intentionally NOT repeated here.
    Object controls_profile{{"format", jget(marker, "format")},
        {"config_sha256", jget(marker, "config_sha256")},
        {"max_wall_minutes", jget(marker, "max_wall_minutes")},
        {"warn_fraction", jget(marker, "warn_fraction")},
        {"stall_same_error_repeats", jget(marker, "stall_same_error_repeats")}};
    Object report{{"format", "wls-controls-run-v1"}, {"request_id", request}};
    for (const auto& field : identity_fields.as_object()) report[field.key()] = field.value();
    report["controls_profile"] = std::move(controls_profile);
    report["batch_limit"] = batch_limit;
    report["batches_started"] = started;
    report["batches_completed"] = completed;
    // B2 repair: checked actual aggregate totals. Unknown, missing or
    // overflowed totals are null with counts_complete false; every new
    // attempt holds exactly one new reservation in this Store, so the two
    // checked columns share the same per-batch delta. The retained safe
    // prefix is emitted only as an explicitly labelled lower bound.
    report["attempts_total"] = totals.counts_complete ? Json(totals.attempts) : Json(nullptr);
    if (!totals.counts_complete) report["attempts_total_lower_bound"] = totals.attempts;
    report["new_reservations_total"] = totals.counts_complete ? Json(totals.new_reservations) : Json(nullptr);
    if (!totals.counts_complete)
        report["new_reservations_total_lower_bound"] = totals.new_reservations;
    report["counts_complete"] = totals.counts_complete;
    // LIVE completeness: the totals carry a value only for the verified
    // conjunction of coverage over the actually observed batches; a partial
    // observation keeps the exact total null and the known prefix only as an
    // explicitly labelled lower bound, never as the total. Fake network zero
    // is proven by the Fake execution type; a live provider call is exactly a
    // started network model call under complete observation.
    report["network_started_calls"] = totals.network_count_complete
        ? Json(totals.network_started_calls) : Json(nullptr);
    if (!totals.network_count_complete)
        report["network_started_calls_lower_bound"] = totals.network_started_calls;
    report["network_count_complete"] = totals.network_count_complete;
    report["provider_calls"] = fake_execution ? Json(std::int64_t{0})
        : (totals.network_count_complete ? Json(totals.network_started_calls) : Json(nullptr));
    report["primary_stop"] = primary_stop;
    report["stop_classes"] = std::move(stop_classes);
    report["raw_stop_codes"] = std::move(raw_stop_codes);
    report["model_milestone"] = model_milestone;
    report["business_delivered"] = business_delivered;
    report["delivery_receipt_verified"] = delivery_receipt_verified;
    report["warning_status"] = warning_status;
    report["history_complete"] = history_complete;
    report["report_complete"] = report_complete;
    // B2 repair: the terminal part (everything except the batch summaries)
    // carries its own 65536-byte bound, checked before the summaries are
    // attached; exceeding it marks the report incomplete instead of silently
    // truncating facts.
    const auto terminal_digest = controls::detail::canonical_sha256_stream(report,
        controls::detail::canonical_stream_chunk_default, 65535);
    if (!terminal_digest.ok) {
        const auto identity_digest = controls::detail::canonical_sha256_stream(identity_fields);
        for (const auto& field : identity_fields.as_object()) report.erase(field.key());
        report["identity_fields_sha256"] = identity_digest.ok ? Json(identity_digest.sha256_hex_value) : Json(nullptr);
        const auto raw_identity = controls::detail::canonical_sha256_stream(Array{report.at("stop_classes"), report.at("raw_stop_codes")});
        report["stop_identity_sha256"] = raw_identity.ok ? Json(raw_identity.sha256_hex_value) : Json(nullptr);
        report["stop_classes"] = Array{"capacity"};
        report["raw_stop_codes"] = Array{"report_capacity"};
        report["report_complete"] = false;
        report["primary_stop"] = "capacity";
    }
    if (!controls::detail::canonical_sha256_stream(report,
            controls::detail::canonical_stream_chunk_default, 65535).ok)
        throw Error(ErrorCode::Storage, "delivery_of_report_failed: bounded terminal report unavailable");
    report["batch_summaries"] = std::move(summaries);
    if (!terminal_digest.ok || canonical_json(report).size() + 1 > 589824) {
        // Replace raw detail with bounded identities; every batch remains
        // represented and the aggregate digest includes every original entry.
        const auto digest = controls::detail::canonical_sha256_stream(report.at("batch_summaries"));
        Array identities;
        for (const auto& summary : report.at("batch_summaries").as_array())
            identities.push_back(Object{{"index", jget(summary, "index")},
                {"report_sha256", jget(summary, "report_sha256")},
                {"report_bytes", jget(summary, "report_bytes")},
                {"attempt_set_sha256", jget(summary, "attempt_set_sha256")},
                {"new_attempts", jget(summary, "new_attempts")}, {"completed", jget(summary, "completed")}});
        report["batch_summaries"] = std::move(identities);
        report["batch_identity_sha256"] = digest.ok ? Json(digest.sha256_hex_value) : Json(nullptr);
        report["report_complete"] = false;
        report["report_truncated"] = true;
        report["primary_stop"] = "capacity";
    }
    if (canonical_json(report).size() + 1 > 589824)
        throw Error(ErrorCode::Storage, "delivery_of_report_failed: bounded emergency report unavailable");
    return report;
}

int run_cli(int argc, char** argv) {
    // Every CLI branch speaks UTF-8 JSON, including its error path. Validate
    // before option parsing, filesystem reads or a writer can persist input.
    for (int index = 1; index < argc; ++index)
        static_cast<void>(canonical_json(Json(argv[index])));
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "help") { help(); return 0; }
    const std::string command(argv[1]);
    if (command == "--version" || command == "version") {
        if (argc != 2) throw Error(ErrorCode::InvalidInput, "version takes no options");
        output(Object{{"version", "0.11.1-r3.3c-audit"}, {"blueprint", "0.4.1"},
            {"mode", "offline_fake_and_controlled_live"}, {"default_network_authorized", false}}); return 0;
    }
    int start = 2;
    std::string action;
    if (command == "config" || command == "schema") {
        if (argc < 3) throw Error(ErrorCode::InvalidInput, command + " needs a subcommand");
        action = argv[2]; start = 3;
    }
    if (command == "doctor" || command == "encode-model" || command == "replay-model") {
        for (int index = start; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument.size() > 4096)
                throw Error(ErrorCode::InvalidInput, "diagnostic and replay arguments exceed 4096 bytes");
        }
    }
    Options options(argc, argv, start);
    if (command == "encode-model" || command == "replay-model")
        return run_protocol_command(command, options);
    const bool source_supplied = options.has("--source-root");
    const bool resource_supplied = options.has("--resource-root");
    if (source_supplied && resource_supplied)
        throw Error(ErrorCode::InvalidInput, "choose either --source-root or --resource-root");
    const std::filesystem::path source_argument = options.take("--source-root");
    const std::filesystem::path resource_argument = options.take("--resource-root");
    const auto resources = source_supplied ? RuntimeResources::from_source_tree(source_argument) :
        (resource_supplied ? RuntimeResources::from_bundle(resource_argument) : RuntimeResources::discover());
    const auto& source = resources.root();
    if (command == "resources") {
        options.finish();
        output(Object{{"resources", resources.diagnostics()}, {"migrations", MigrationRegistry(resources).diagnostics()}});
        return 0;
    }
    if (command == "config") {
        if (action != "check" && action != "explain") throw Error(ErrorCode::InvalidInput, "config needs check or explain");
        const auto frozen = live_config(options, resources);
        const auto role = action == "explain" ? role_from_string(options.take("--role", "worker")) : Role::Root;
        options.finish();
        if (action == "explain") output(frozen.explain(role));
        else output(Object{{"valid", true}, {"function_id", frozen.function_id()}, {"capability_id", frozen.capability_id()},
            {"snapshot_sha256", frozen.sha256()}, {"dependency_count", frozen.dependencies().size()},
            {"snapshot_bytes", frozen.serialize().size()},
            {"real_calls_enabled", frozen.json().at("system").at("model_calls").at("real_calls_enabled")},
            {"host_network_authorized", false}, {"credentials_resolved", false}, {"environment_checks_executed", false},
            {"runtime_support", frozen.runtime_support()}});
        return 0;
    }
    if (command == "schema") {
        if (action != "check") throw Error(ErrorCode::InvalidInput, "schema needs check");
        const auto name = options.required("--schema");
        const auto input = options.required("--input");
        const auto model_input = options.take("--model-input") == "true";
        options.finish();
        const SchemaRegistry schemas(resources.schema_documents());
        schemas.validate(name, parse_json_strict(read_runtime_file(input)), model_input);
        output(Object{{"valid", true}, {"schema", name}, {"model_input", model_input}, {"semantic_acceptance", "not_evaluated"}});
        return 0;
    }
    if (command == "doctor") {
        const auto source_input = input_path_details(source_supplied ? source_argument :
            (resource_supplied ? resource_argument : source), source_supplied || resource_supplied ? "supplied" : "default");
        Json config_inputs;
        const auto frozen = live_config(options, resources, &config_inputs);
        const bool template_supplied = options.has("--template");
        const auto template_id = options.take("--template", "default");
        const auto manifest_path = options.take("--sandbox-manifest");
        options.finish();
        const auto manifest_input = manifest_path.empty() ? Json(nullptr) : input_path_details(manifest_path, "supplied");
        const auto manifest_bytes = manifest_path.empty() ? std::string{} : read_runtime_file(manifest_path, 262144);
        const auto manifest = manifest_path.empty() ? Json(Object{}) : parse_json_strict(manifest_bytes, 262144);
        const auto policy = worker_sandbox_policy(frozen, template_id, manifest);
        auto result = to_json(sandbox_doctor(policy));
        result.as_object()["config_snapshot_sha256"] = frozen.sha256();
        result.as_object()["template_id"] = template_id;
        result.as_object()["environment_checks_executed"] = false;
        result.as_object()["process_isolation_verified"] = false;
        result.as_object()["execution_policy_sha256"] = sha256_hex(canonical_json(worker_execution_policy(frozen, template_id, policy)));
        config_inputs.as_object()["source_root"] = source_input;
        config_inputs.as_object()["runtime_resources"] = resources.diagnostics();
        config_inputs.as_object()["template"] = Object{{"origin", template_supplied ? "supplied" : "default"}, {"value", template_id}};
        auto& inputs = result.at("inputs").as_object();
        inputs["config"] = std::move(config_inputs);
        if (manifest_path.empty()) {
            inputs["manifest"] = Object{{"origin", "not_configured"}, {"input_path", nullptr},
                {"absolute_lexical_path", nullptr}, {"raw_sha256", nullptr}, {"bytes", 0}, {"schema_version", nullptr}};
        } else {
            auto info = manifest_input;
            info.as_object()["raw_sha256"] = sha256_hex(manifest_bytes);
            info.as_object()["bytes"] = manifest_bytes.size();
            info.as_object()["schema_version"] = manifest.as_object().contains("schema_version") ? manifest.at("schema_version") : Json(nullptr);
            inputs["manifest"] = std::move(info);
        }
        for (const auto* name : {"launcher", "cgroup", "runtime_files"}) {
            if (auto* item = inputs.if_contains(name); item && item->is_object()) {
                const std::string field = std::string(name) == "launcher" ? "bwrap_path" :
                    (std::string(name) == "cgroup" ? "cgroup_root" : "runtime_files");
                item->as_object()["origin"] = manifest.as_object().contains(field) ? "supplied" :
                    (std::string(name) == "launcher" ? "default" : "not_configured");
            }
        }
        bound_sandbox_doctor_report(result);
        output(result); return 0;
    }
    const std::set<std::string> commands{"init", "submit", "show", "events", "budget", "config-snapshot", "run-fake", "pump-fake", "pump-live", "pump-single-fake", "pump-single-live", "run-live", "live-status", "reconcile-billing", "inbox", "history", "history-page", "history-read", "questions", "submissions", "answer", "wait", "wake", "tool-query", "amend", "cancel", "pending", "deliveries", "experience", "closeout", "publish-delivery", "verify-delivery", "ack-delivery", "allocate-workspace", "abandon-workspace", "owned-workspaces", "cleanup-workspaces", "reconcile-op", "capacity", "capacity-checkpoint"};
    if (!commands.contains(command)) throw Error(ErrorCode::InvalidInput, "unknown command: " + command);
    const auto database = options.required("--db");
    const MigrationRegistry migrations(resources);
    if (command == "capacity" || command == "capacity-checkpoint") {
        const auto request = options.required("--request");
        if (command == "capacity") {
            options.finish();
            Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            output(inspect.capacity_status(request)); return 0;
        }
        const auto expected = options.number("--expected-window", -1, 0);
        const auto evidence = read_runtime_file(options.required("--evidence-file"), 4096);
        const auto key = options.required("--key"); options.finish();
        {
            Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            inspect.validate_capacity_checkpoint(inspect.host_actor(Role::Root, request), expected, evidence, key);
        }
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        output(store.checkpoint_capacity(store.host_actor(Role::Root, request), expected, evidence, key)); return 0;
    }
    if (command == "reconcile-op") {
        const auto request = options.required("--request");
        const auto op = options.required("--op");
        const auto expected = options.number("--expected-version", 0, 1);
        const auto outcome = options.required("--outcome");
        const auto evidence = read_runtime_file(options.required("--evidence-file"), 4096);
        const auto key = options.required("--key");
        options.finish();
        // Invalid evidence/CAS/ownership cannot take over the writer or fence
        // unrelated requests. Repeat the identical guard after acquiring it.
        {
            Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            inspect.validate_reconciliation(inspect.host_actor(Role::Root, request), op, expected, outcome, evidence, key);
        }
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        output(store.reconcile_operation(store.host_actor(Role::Root, request), op, expected, outcome, evidence, key));
        return 0;
    }
    if (command == "init") {
        options.finish(); Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope{}); output(store.diagnostics()); return 0;
    }
    if (command == "submit") {
        const auto frozen = live_config(options, resources);
        AdmissionInput admission;
        admission.tenant_id = options.take("--tenant", "local");
        admission.project_id = options.take("--project", "local");
        admission.root_id = options.take("--root", "local-root");
        admission.function_id = frozen.function_id();
        admission.idempotency_key = options.required("--key");
        admission.original_text = read_runtime_file(options.required("--text-file"), 65536);
        const auto constraints = options.take("--constraints-file");
        if (!constraints.empty()) admission.constraint_catalog = parse_json_strict(read_runtime_file(constraints));
        validate_constraint_catalog_shape(admission.constraint_catalog);
        admission.config_snapshot = frozen.json();
        const auto grants = frozen.worker_tools();
        admission.tool_allowlist = std::vector<std::string>(grants.begin(), grants.end());
        const auto& defaults = frozen.json().at("system").at("budget_defaults");
        admission.budget.max_attempts = options.number("--max-attempts", 64, 1);
        admission.budget.input_tokens = options.number("--input-budget", defaults.at("max_input_tokens").is_null() ? 1000000 : integer(defaults.at("max_input_tokens")));
        admission.budget.output_tokens = options.number("--output-budget", defaults.at("max_output_tokens").is_null() ? 100000 : integer(defaults.at("max_output_tokens")));
        const auto& cost = defaults.at("max_cost_usd");
        const auto cost_limit = cost.is_null() ? 0 : cost_limit_micro(cost);
        admission.budget.cost_micro_usd = options.number("--cost-budget-micro-usd", cost_limit);
        options.finish();
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::admission());
        const auto admitted = store.admit(admission);
        output(request_view(store, text(admitted.at("request_id")))); return 0;
    }
    if (command == "pending") {
        options.finish(); Store store(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
        output(store.pending_recovery()); return 0;
    }
    const auto request = options.required("--request");
    if (command == "deliveries" || command == "experience" || command == "closeout") {
        const auto task = command == "experience" ? options.take("--task") : std::string{};
        const auto limit = command == "closeout" ? 0 : options.number("--limit", command == "experience" ? 128 : 64, 1);
        if (limit > (command == "experience" ? 128 : 64))
            throw Error(ErrorCode::InvalidInput, "delivery/experience query limit exceeds its bound");
        options.finish();
        Store store(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
        (void)store.request(request);
        if (!task.empty() && text(store.task(task).at("request_id")) != request)
            throw Error(ErrorCode::Unauthorized, "task belongs to another request");
        if (command == "deliveries") output(Object{{"request_id", request}, {"deliveries", array_of(store.deliveries(request, limit))}});
        else if (command == "experience") output(Object{{"request_id", request}, {"experience_jobs", array_of(store.experience_jobs(request, task, limit))}});
        else output(store.closeout(request));
        return 0;
    }
    if (command == "publish-delivery" || command == "verify-delivery" || command == "ack-delivery") {
        const auto directory_path = options.required("--delivery-dir");
        const auto delivery_id = command == "publish-delivery" ? options.take("--delivery") : options.required("--delivery");
        if (!delivery_id.empty() && !is_typed_id(delivery_id, "dlv"))
            throw Error(ErrorCode::InvalidInput, "invalid delivery ID");
        std::string manifest_hash, recipient, receipt_key, method;
        std::int64_t requirements_revision{}, plan_revision{};
        if (command == "ack-delivery") {
            manifest_hash = options.required("--manifest-sha256");
            if (manifest_hash.size() != 64 || !std::all_of(manifest_hash.begin(), manifest_hash.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                throw Error(ErrorCode::InvalidInput, "manifest SHA-256 must be 64 lowercase hexadecimal characters");
            requirements_revision = options.number("--requirements-revision", 0, 1);
            plan_revision = options.number("--plan-revision", 0, 1);
            if (requirements_revision == 0 || plan_revision == 0)
                throw Error(ErrorCode::InvalidInput, "ack-delivery requires explicit requirements and plan revisions");
            recipient = options.required("--recipient");
            receipt_key = options.required("--key");
            method = options.take("--method", "durable_local");
            for (const auto& value : {recipient, receipt_key})
                if (value.size() > 256 || value.find('\0') != std::string::npos ||
                    value.find_first_not_of(" \t\r\n") == std::string::npos)
                    throw Error(ErrorCode::InvalidInput, "receipt recipient and key must be bounded nonblank strings");
            if (method != "durable_local" && method != "explicit_ack")
                throw Error(ErrorCode::InvalidInput, "receipt method must be durable_local or explicit_ack");
        }
        options.finish();
        DeliveryDirectory directory(directory_path);
        const auto root_identity = directory.root_identity();
        // Invalid UI tuples, foreign delivery IDs and changed directories fail
        // before taking over a writer. The Store repeats version/ownership checks
        // at the final transaction, after durable filesystem verification.
        {
            Store reader(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            const auto record = reader.request(request);
            check_delivery_root(reader, request, database, root_identity);
            if (!delivery_id.empty()) {
                const auto delivery = reader.delivery(delivery_id);
                check_delivery_scope(delivery, request, root_identity);
                if (command != "verify-delivery") {
                    if (delivery.at("requirements_revision") != record.at("requirements_revision") ||
                        delivery.at("plan_revision") != record.at("plan_revision"))
                        throw Error(ErrorCode::Stale, "delivery versions no longer match the current request");
                    if (text(delivery.at("status")) == "SUPERSEDED")
                        throw Error(ErrorCode::Stale, "delivery intent has been superseded");
                }
                if (command == "ack-delivery") {
                    if (text(delivery.at("manifest_sha256")) != manifest_hash ||
                        integer(delivery.at("requirements_revision")) != requirements_revision ||
                        integer(delivery.at("plan_revision")) != plan_revision)
                        throw Error(ErrorCode::Conflict, "receipt hash or versions differ from the exact delivery");
                    if (text(delivery.at("status")) != "STORED" && text(delivery.at("status")) != "CONFIRMED")
                        throw Error(ErrorCode::NotReady, "receipt requires a recorded durable publication");
                }
                if (command != "publish-delivery") {
                    const auto verified = directory.inspect(delivery.at("frozen_plan"));
                    if (command == "verify-delivery") {
                        output(Object{{"request_id", request}, {"delivery_id", delivery_id}, {"inspection", verified},
                                      {"receipt_created", false}});
                        return 0;
                    }
                }
            }
        }
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        const auto root = store.host_actor(Role::Root, request);
        const auto binding = store.request_binding(request);
        auto delivery = delivery_id.empty()
            ? store.prepare_delivery(root, binding, integer(store.request(request).at("state_version")),
                                     root_identity, new_id("delivery"))
            : store.delivery(delivery_id);
        check_delivery_scope(delivery, request, root_identity);
        // A concurrent writer may have changed acceptance after the read-only
        // preflight and before this lease was acquired. Authorize the exact
        // retained intent again before any filesystem mutation.
        delivery = store.authorize_delivery_publication(root, binding, text(delivery.at("delivery_id")));
        const auto publication = publish_bundle(directory, delivery);
        if (command == "ack-delivery") {
            output(store.acknowledge_delivery(root, binding, delivery_id, manifest_hash, requirements_revision,
                plan_revision, recipient, method, receipt_key, publication));
        } else {
            delivery = store.record_delivery_publication(root, binding, text(delivery.at("delivery_id")),
                integer(delivery.at("state_version")), publication, new_id("publish"));
            output(Object{{"delivery", delivery}, {"publication", publication}, {"receipt_created", false}});
        }
        return 0;
    }
    if (command == "owned-workspaces") {
        options.finish();
        Store store(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
        output(Object{{"request_id", request}, {"resources", array_of(store.owned_workspaces(request))}}); return 0;
    }
    if (command == "abandon-workspace") {
        const auto resource_id = options.required("--resource");
        const auto expected = options.number("--expected-state-version", 0, 1);
        const auto key = options.required("--key");
        const auto reason = options.required("--reason"); options.finish();
        if (!is_typed_id(resource_id, "res") || expected == 0)
            throw Error(ErrorCode::InvalidInput, "abandon-workspace requires a resource ID and explicit positive state version");
        {
            Store reader(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            reader.validate_abandon_owned_workspace(reader.host_actor(Role::Root, request), resource_id, expected, key, reason);
        }
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        output(store.abandon_owned_workspace(store.host_actor(Role::Root, request), resource_id, expected, key, reason));
        return 0;
    }
    if (command == "allocate-workspace") {
        const auto task_id = options.required("--task"); const auto parent = options.required("--parent");
        const auto key = options.required("--key"); options.finish();
        {
            Store reader(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            if (text(reader.task(task_id).at("request_id")) != request)
                throw Error(ErrorCode::Unauthorized, "task belongs to another request");
        }
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        output(allocate_owned_workspace(store, request, task_id, parent, key)); return 0;
    }
    if (command == "cleanup-workspaces") {
        const auto confirm = options.required("--confirm-request");
        const auto max_unlinks = options.number("--max-unlinks", 1024, 1); options.finish();
        if (confirm != request || max_unlinks > 4096)
            throw Error(ErrorCode::InvalidInput, "cleanup requires exact --confirm-request and max-unlinks 1..4096");
        {
            Store reader(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            if (!reader.workspace_cleanup_status(request).at("workspace_eligible").as_bool())
                throw Error(ErrorCode::NotReady, "workspace cleanup blocked; inspect closeout");
        }
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        output(cleanup_owned_workspaces(store, request, static_cast<std::size_t>(max_unlinks))); return 0;
    }
    if (command == "questions" || command == "submissions") {
        const auto task = command == "submissions" ? options.required("--task") : options.take("--task");
        const auto limit = options.number("--limit", command == "questions" ? 128 : 64, 1);
        const auto maximum = command == "questions" ? 128 : 64;
        if (limit > maximum) throw Error(ErrorCode::InvalidInput,
            command == "questions" ? "questions limit must be 1..128" : "submissions limit must be 1..64");
        options.finish();
        Store store(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
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
        const SchemaRegistry schemas(resources.schema_documents());
        schemas.validate("answer_question", answer, true);
        // Reject stale UI actions, foreign objects and invalid selections before
        // acquiring a writer. Store repeats the guards atomically after takeover.
        {
            Store reader(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
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
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        output(store.answer_question(store.host_actor(Role::Root, request), store.request_binding(request),
            question_id, expected_version, answer, new_id("answer")));
        return 0;
    }
    if (command == "history-page" || command == "history-read") {
        const auto role = role_from_string(options.required("--role"));
        const auto task = options.take("--task");
        std::int64_t after = 0, through = 0, limit = 32;
        std::string attempt, hash, section;
        std::int64_t offset = 0, max_bytes = 16384;
        if (command == "history-read") {
            attempt = options.required("--attempt"); hash = options.required("--record-sha256");
            section = options.required("--section");
            offset = options.number("--offset", 0, 0); max_bytes = options.number("--max-bytes", 16384, 1);
        } else {
            after = options.number("--after", 0, 0); through = options.number("--through", 0, 0);
            limit = options.number("--limit", 32, 1);
        }
        options.finish();
        Store store(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
        const auto actor = scoped_actor(store, request, role, task);
        output(command == "history-page" ? store.attempt_history_page(actor, after, through, limit)
            : store.read_history_feedback(actor, attempt, hash, section, offset, max_bytes));
        return 0;
    }
    if (command == "inbox" || command == "history") {
        const auto role = role_from_string(options.take("--role", "dispatcher"));
        const auto task = options.take("--task");
        const auto limit = options.number("--limit", 256, 1);
        if (limit > 256) throw Error(ErrorCode::InvalidInput, "inbox/history limit must be 1..256");
        options.finish();
        Store store(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
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
        {
            Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            (void)scoped_actor(inspect, request, Role::Worker, task);
            const auto record = inspect.task(task);
            const auto request_record = inspect.request(request);
            const auto terminal = [](const std::string& status) {
                return status == "ACCEPTED" || status == "DELIVERED" || status == "FAILED" ||
                    status == "CANCELLED" || status == "SKIPPED" || status == "SUPERSEDED";
            };
            if (terminal(text(record.at("status"))) || terminal(text(request_record.at("status"))))
                throw Error(ErrorCode::NotReady, "terminal work cannot wait or wake");
            for (const auto& question : inspect.questions(request, task)) {
                if (text(question.at("status")) == "OPEN" && integer(question.at("blocking")) != 0)
                    throw Error(ErrorCode::NotReady, "answer the owning blocking question before changing this Worker wait");
            }
            const bool current_plan = integer(record.at("based_on_requirements_revision")) == integer(request_record.at("requirements_revision")) &&
                integer(record.at("plan_revision")) == integer(request_record.at("plan_revision"));
            if (command == "wait" && (!current_plan || text(record.at("status")) == "SUBMITTED" ||
                text(record.at("status")) == "REVIEWING" || text(record.at("status")) == "BLOCKED"))
                throw Error(ErrorCode::NotReady, "Worker wait requires a current disposition without a submission hold");
            const auto waiting = inspect.worker_wait(task);
            if (command == "wait" && !waiting.is_null() && text(waiting.at("status")) == "WAITING")
                throw Error(ErrorCode::NotReady, "Worker already has an active wait");
            if (command == "wake" && (waiting.is_null() || text(waiting.at("status")) != "WAITING" ||
                integer(waiting.at("generation")) != generation))
                throw Error(ErrorCode::Stale, "wait generation is absent or no longer waiting");
            if (command == "wake") {
                auto original = execution_binding_from_json(waiting.at("original_binding"));
                const auto current = inspect.binding(task);
                original.dispatcher_epoch = current.dispatcher_epoch;
                original.execution_epoch = current.execution_epoch;
                original.policy_epoch = current.policy_epoch;
                if (!current_plan || original != current)
                    throw Error(ErrorCode::Stale, "wait target changed semantically");
            }
        }
        // This is a preflight, not authorization. Store repeats all guards with
        // the current binding after acquiring the exclusive writer lease.
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        const auto actor = scoped_actor(store, request, Role::Worker, task);
        const auto binding = store.binding(task);
        output(command == "wait"
            ? store.wait_worker(actor, binding, integer(store.task(task).at("state_version")), reason, new_id("wait"))
            : store.wake_worker(actor, binding, generation, new_id("wake")));
        return 0;
    }
    if (command == "live-status") {
        const auto billing_policy_path = options.take("--billing-policy-file");
        options.finish();
        Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
        const auto billing_policy = billing_cli_policy(inspect, billing_policy_path);
        output(billing_cli_status(inspect, request, billing_policy));
        return 0;
    }
    if (command == "reconcile-billing") {
        const auto attempt_id = options.required("--attempt");
        const auto billing_policy_path = options.required("--billing-policy-file");
        const std::filesystem::path receipt_path = options.required("--receipt-file");
        const auto expected = options.number("--expected-version", 0, 1);
        const auto key = options.required("--key");
        options.finish();
        if (expected <= 0 || attempt_id.size() > 256 || key.size() > 256)
            throw Error(ErrorCode::InvalidInput, "billing reconciliation needs bounded identities and an explicit positive version");
        const auto receipt_absolute = std::filesystem::absolute(receipt_path).lexically_normal();
        std::optional<BillingCliPolicy> policy;
        std::string receipt_bytes;
        std::string receipt_sha256;
        {
            Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            inspect.validate_control_file_path(receipt_absolute);
            policy = billing_cli_policy(inspect, billing_policy_path);
            validate_billing_cli_policy(inspect, request, *policy);
            receipt_bytes = billing_cli_control_bytes(inspect, receipt_path);
            receipt_sha256 = sha256_hex(receipt_bytes);
            const auto evidence = verify_signed_billing_receipt(inspect, policy->policy, attempt_id, receipt_bytes);
            const UsageReconciliation reconciliation{evidence.required_reconciliation_id(), key, expected};
            const auto replay = inspect.replay_usage_reconciliation(
                inspect.host_actor(Role::Root, request), evidence, reconciliation);
            if (!replay.is_null()) {
                output(billing_cli_result(request, attempt_id, *policy, receipt_sha256, replay, true));
                return 0;
            }
            const auto projection = inspect.usage_projection(attempt_id).at("projection");
            if (!projection.is_object() || integer(projection.at("state_version")) != expected)
                throw Error(ErrorCode::Conflict, "billing projection changed before writer takeover");
        }
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        validate_billing_cli_policy(store, request, *policy);
        store.validate_control_file_path(receipt_absolute);
        const auto evidence = verify_signed_billing_receipt(store, policy->policy, attempt_id, receipt_bytes);
        const UsageReconciliation reconciliation{evidence.required_reconciliation_id(), key, expected};
        const auto root = store.host_actor(Role::Root, request);
        const auto replay = store.replay_usage_reconciliation(root, evidence, reconciliation);
        const auto result = replay.is_null() ? store.reconcile_usage(root, evidence, reconciliation) : replay;
        output(billing_cli_result(request, attempt_id, *policy, receipt_sha256, result, !replay.is_null()));
        return 0;
    }
    if (command == "pump-live" || command == "pump-single-live") {
        const bool single_agent = command == "pump-single-live";
        const std::filesystem::path authorization_path = options.required("--authorization-file");
        const auto billing_policy_path = options.take("--billing-policy-file");
        const auto workspaces_file = options.take("--workspaces-file");
        const auto paths = workspace_map(workspaces_file.empty() ? Json(Object{}) :
            parse_json_strict(read_runtime_file(workspaces_file, 262144)), database);
        const auto manifest_path = options.take("--sandbox-manifest");
        if (!manifest_path.empty() && paths.empty())
            throw Error(ErrorCode::InvalidInput, "sandbox manifest requires explicit task workspaces");
        const auto manifest = manifest_path.empty() ? Json(Object{}) :
            parse_json_strict(read_runtime_file(manifest_path, 262144));
        options.finish();
        const auto authorization_absolute = std::filesystem::absolute(authorization_path).lexically_normal();
        const auto billing_absolute = billing_policy_path.empty() ? std::filesystem::path{} :
            std::filesystem::absolute(billing_policy_path).lexically_normal();
        for (const auto& [node, path] : paths) {
            (void)node;
            if (path_contains(path, authorization_absolute))
                throw Error(ErrorCode::Unauthorized, "host authorization must be outside business workspaces");
            if (!billing_absolute.empty() && path_contains(path, billing_absolute))
                throw Error(ErrorCode::Unauthorized, "billing trust policy must be outside business workspaces");
        }
        const SchemaRegistry schemas(resources.schema_documents());
        std::optional<BillingCliPolicy> billing_policy;
        LiveCliAuthorization authorization;
        std::string authorization_bytes;
        {
            Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            inspect.validate_control_file_path(authorization_absolute);
            if (!billing_absolute.empty()) inspect.validate_control_file_path(billing_absolute);
            authorization_bytes = billing_cli_control_bytes(inspect, authorization_path);
            authorization = live_cli_authorization(parse_json_strict(authorization_bytes, 65536));
            billing_policy = billing_cli_policy(inspect, billing_policy_path);
            validate_cli_model_topology(inspect, request, single_agent);
            validate_live_cli_authorization(inspect, request, authorization);
            validate_billing_cli_authorization(inspect, request, authorization, billing_policy);
            const auto frozen = restore_request_config(inspect.request(request));
            for (const auto& entry : frozen.json().at("function").at("team_templates").as_object())
                (void)worker_sandbox_policy(frozen, std::string(entry.key()), manifest);
            for (const auto& task : inspect.tasks(request)) {
                const auto workspace = inspect.workspace(text(task.at("team_id")));
                if (workspace.is_object() &&
                        path_contains(text(workspace.at("canonical_path")), authorization_absolute))
                    throw Error(ErrorCode::Unauthorized, "host authorization must be outside bound business workspaces");
            }
        }
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        store.validate_control_file_path(authorization_absolute);
        validate_cli_model_topology(store, request, single_agent);
        validate_live_cli_authorization(store, request, authorization);
        validate_billing_cli_authorization(store, request, authorization, billing_policy);
        if (billing_policy) {
            authorization.workflow.usage_evidence = [&store, policy = billing_policy->policy](
                    const ModelRequest& call, const ModelResponse& response, const Json& settlement) {
                return verify_observed_billing_receipt(store, policy, call, response, settlement);
            };
        }
        std::vector<std::unique_ptr<WorkerTools>> owners;
        std::map<std::string, WorkerTools*> tools_by_node;
        for (const auto& [node, path] : paths) {
            owners.push_back(std::make_unique<WorkerTools>(store, path, manifest));
            tools_by_node.emplace(node, owners.back().get());
        }
        auto result = single_agent
            ? SingleAgentRuntime(store, schemas, std::move(tools_by_node)).pump_live(request, authorization.workflow)
            : EventRuntime(store, schemas, std::move(tools_by_node)).pump_live(request, authorization.workflow);
        result.as_object()["host_authorization_sha256"] = sha256_hex(authorization_bytes);
        if (billing_policy) {
            result.as_object()["billing_policy_sha256"] = billing_policy->bytes_sha256;
            result.as_object()["billing_policy_canonical_sha256"] = billing_policy->policy.sha256();
        }
        output(result);
        return 0;
    }
    if (command == "pump-fake" || command == "pump-single-fake") {
        const bool single_agent = command == "pump-single-fake";
        const auto script = event_fake_script_from_json(parse_json_strict(read_runtime_file(options.required("--script"))));
        const auto workspaces_file = options.take("--workspaces-file");
        const auto paths = workspace_map(workspaces_file.empty() ? Json(Object{}) :
            parse_json_strict(read_runtime_file(workspaces_file, 262144)), database);
        const auto dispositions_file = options.take("--dispositions-file");
        EventPumpOptions pump_options;
        if (!dispositions_file.empty())
            pump_options.amendment_dispositions = parse_json_strict(read_runtime_file(dispositions_file, 262144));
        validate_dispositions(pump_options.amendment_dispositions);
        if (single_agent && !pump_options.amendment_dispositions.as_object().empty())
            throw Error(ErrorCode::Unauthorized, "single-Agent AMEND decisions must come from original model calls");
        pump_options.max_calls = options.number("--max-calls", 12, 1);
        pump_options.max_worker_calls_per_task = options.number("--worker-calls", 1, 1);
        if (pump_options.max_calls > 64 || pump_options.max_worker_calls_per_task > 16)
            throw Error(ErrorCode::InvalidInput, "pump limit is 1..64 calls and 1..16 Worker calls per task");
        const auto manifest_path = options.take("--sandbox-manifest");
        if (!manifest_path.empty() && paths.empty())
            throw Error(ErrorCode::InvalidInput, "sandbox manifest requires explicit task workspaces");
        const auto manifest = manifest_path.empty() ? Json(Object{}) : parse_json_strict(read_runtime_file(manifest_path, 262144));
        options.finish();
        const SchemaRegistry schemas(resources.schema_documents());
        {
            Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            validate_cli_model_topology(inspect, request, single_agent);
        }
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        validate_cli_model_topology(store, request, single_agent);
        std::vector<std::unique_ptr<WorkerTools>> owners;
        std::map<std::string, WorkerTools*> tools_by_node;
        for (const auto& [node, path] : paths) {
            owners.push_back(std::make_unique<WorkerTools>(store, path, manifest));
            tools_by_node.emplace(node, owners.back().get());
        }
        output(single_agent
            ? SingleAgentRuntime(store, schemas, std::move(tools_by_node)).pump(request, script, pump_options)
            : EventRuntime(store, schemas, std::move(tools_by_node)).pump(request, script, pump_options));
        return 0;
    }
    if (command == "run-live") {
        const std::filesystem::path authorization_path = options.required("--authorization-file");
        const auto billing_policy_path = options.take("--billing-policy-file");
        const auto workspaces_file = options.take("--workspaces-file");
        const auto paths = workspace_map(workspaces_file.empty() ? Json(Object{}) :
            parse_json_strict(read_runtime_file(workspaces_file, 262144)), database);
        const auto manifest_path = options.take("--sandbox-manifest");
        if (!manifest_path.empty() && paths.empty())
            throw Error(ErrorCode::InvalidInput, "sandbox manifest requires explicit task workspaces");
        const auto manifest = manifest_path.empty() ? Json(Object{}) :
            parse_json_strict(read_runtime_file(manifest_path, 262144));
        options.finish();
        const auto authorization_absolute = std::filesystem::absolute(authorization_path).lexically_normal();
        const auto billing_absolute = billing_policy_path.empty() ? std::filesystem::path{} :
            std::filesystem::absolute(billing_policy_path).lexically_normal();
        for (const auto& [node, path] : paths) {
            (void)node;
            if (path_contains(path, authorization_absolute))
                throw Error(ErrorCode::Unauthorized, "host authorization must be outside business workspaces");
            if (!billing_absolute.empty() && path_contains(path, billing_absolute))
                throw Error(ErrorCode::Unauthorized, "billing trust policy must be outside business workspaces");
        }
        const SchemaRegistry schemas(resources.schema_documents());
        std::optional<BillingCliPolicy> billing_policy;
        LiveCliAuthorization authorization;
        std::string authorization_bytes;
        {
            Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            inspect.validate_control_file_path(authorization_absolute);
            if (!billing_absolute.empty()) inspect.validate_control_file_path(billing_absolute);
            authorization_bytes = billing_cli_control_bytes(inspect, authorization_path);
            authorization = live_cli_authorization(parse_json_strict(authorization_bytes, 65536));
            billing_policy = billing_cli_policy(inspect, billing_policy_path);
            validate_cli_model_topology(inspect, request, false);
            validate_live_cli_authorization(inspect, request, authorization);
            validate_billing_cli_authorization(inspect, request, authorization, billing_policy);
            const auto frozen = restore_request_config(inspect.request(request));
            for (const auto& entry : frozen.json().at("function").at("team_templates").as_object())
                (void)worker_sandbox_policy(frozen, std::string(entry.key()), manifest);
        }
        // One request-scoped Takeover Store and one WorkerTools set serve the
        // whole command; the authorization bytes are hashed once and never
        // reread to obtain new quota.
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        store.validate_control_file_path(authorization_absolute);
        validate_cli_model_topology(store, request, false);
        validate_live_cli_authorization(store, request, authorization);
        validate_billing_cli_authorization(store, request, authorization, billing_policy);
        if (billing_policy) {
            authorization.workflow.usage_evidence = [&store, policy = billing_policy->policy](
                    const ModelRequest& call, const ModelResponse& response, const Json& settlement) {
                return verify_observed_billing_receipt(store, policy, call, response, settlement);
            };
        }
        std::vector<std::unique_ptr<WorkerTools>> owners;
        std::map<std::string, WorkerTools*> tools_by_node;
        for (const auto& [node, path] : paths) {
            owners.push_back(std::make_unique<WorkerTools>(store, path, manifest));
            tools_by_node.emplace(node, owners.back().get());
        }
        EventRuntime runtime(store, schemas, std::move(tools_by_node));
        const auto budget = store.budget(request);
        const auto batch_limit = std::min<std::int64_t>(
            integer(budget.at("max_attempts")) - integer(budget.at("attempts")), 64);
        Object identity{{"host_authorization_sha256", sha256_hex(authorization_bytes)},
            {"script_source_sha256", nullptr}};
        if (billing_policy) {
            identity["billing_policy_sha256"] = billing_policy->bytes_sha256;
            identity["billing_policy_canonical_sha256"] = billing_policy->policy.sha256();
        }
        output(controls_run_driver(store, request, batch_limit, false, identity,
            [&](std::int64_t) { return runtime.pump_live(request, authorization.workflow); },
            [&](const Json& entry, std::int64_t) { return runtime.controls_next_target(request, entry); }));
        return 0;
    }
    if (command == "run-fake" && options.has("--scripts-file")) {
        if (options.has("--script"))
            throw Error(ErrorCode::InvalidInput, "--script and --scripts-file are mutually exclusive");
        // Whole-file pre-parse before any Takeover or business effect; the
        // original bytes are hash-frozen and never reread during execution.
        const auto scripts = controls_scripts_document_from_bytes(
            read_runtime_file(options.required("--scripts-file"), 1048576));
        const auto workspaces_file = options.take("--workspaces-file");
        const auto paths = workspace_map(workspaces_file.empty() ? Json(Object{}) :
            parse_json_strict(read_runtime_file(workspaces_file, 262144)), database);
        EventPumpOptions pump_options;
        const auto dispositions_file = options.take("--dispositions-file");
        if (!dispositions_file.empty())
            pump_options.amendment_dispositions = parse_json_strict(read_runtime_file(dispositions_file, 262144));
        validate_dispositions(pump_options.amendment_dispositions);
        pump_options.max_calls = options.number("--max-calls", 12, 1);
        pump_options.max_worker_calls_per_task = options.number("--worker-calls", 1, 1);
        if (pump_options.max_calls > 64 || pump_options.max_worker_calls_per_task > 16)
            throw Error(ErrorCode::InvalidInput, "run limit is 1..64 calls and 1..16 Worker calls per task");
        const auto manifest_path = options.take("--sandbox-manifest");
        if (!manifest_path.empty() && paths.empty())
            throw Error(ErrorCode::InvalidInput, "sandbox manifest requires explicit task workspaces");
        const auto manifest = manifest_path.empty() ? Json(Object{}) :
            parse_json_strict(read_runtime_file(manifest_path, 262144));
        options.finish();
        const SchemaRegistry schemas(resources.schema_documents());
        {
            // Input qualification (batch budget vs the original remaining
            // attempts) completes read-only, before any Takeover effect.
            Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
            validate_cli_model_topology(inspect, request, false);
            const auto budget = inspect.budget(request);
            const auto batch_limit = std::min<std::int64_t>(
                integer(budget.at("max_attempts")) - integer(budget.at("attempts")), 64);
            if (batch_limit < 1)
                throw Error(ErrorCode::Quota, "run-fake requires at least one remaining original attempt");
            if (static_cast<std::int64_t>(scripts.batches.size()) > batch_limit)
                throw Error(ErrorCode::InvalidInput,
                            "run-fake scripts exceed the bounded batch budget; input qualification failed");
        }
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        validate_cli_model_topology(store, request, false);
        std::vector<std::unique_ptr<WorkerTools>> owners;
        std::map<std::string, WorkerTools*> tools_by_node;
        for (const auto& [node, path] : paths) {
            owners.push_back(std::make_unique<WorkerTools>(store, path, manifest));
            tools_by_node.emplace(node, owners.back().get());
        }
        EventRuntime runtime(store, schemas, std::move(tools_by_node));
        const auto budget = store.budget(request);
        const auto batch_limit = std::min<std::int64_t>(
            integer(budget.at("max_attempts")) - integer(budget.at("attempts")), 64);
        if (batch_limit < 1)
            throw Error(ErrorCode::Quota, "run-fake requires at least one remaining original attempt");
        const auto batch_count = scripts.batches.size();
        Object identity{{"host_authorization_sha256", nullptr},
            {"script_source_sha256", scripts.source_sha256},
            {"script_batches", static_cast<std::int64_t>(batch_count)},
            {"script_total_steps", scripts.total_steps}};
        output(controls_run_driver(store, request, batch_limit, true, identity, [&](std::int64_t index) {
            // SCRIPT[i-1] is consumed only when batch i actually runs; a
            // missing next script with remaining work reports script_exhausted,
            // and an unused tail is counted, never run to clear notifications.
            if (static_cast<std::size_t>(index - 1) >= scripts.batches.size())
                throw Error(ErrorCode::InvalidInput, "script_exhausted: no script remains for this batch");
            return runtime.pump(request, scripts.batches[static_cast<std::size_t>(index - 1)], pump_options);
        }, [&](const Json& entry, std::int64_t next) -> Json {
            if (next < 1 || static_cast<std::size_t>(next) > scripts.batches.size()) return nullptr;
            return runtime.controls_next_target(request, entry, &scripts.batches[static_cast<std::size_t>(next - 1)]);
        }));
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
        Store store(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
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
        Store store(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
        if (command == "show") output(request_view(store, request));
        if (command == "events") output(array_of(store.events(request, after, limit)));
        if (command == "budget") output(store.budget(request));
        if (command == "config-snapshot") output(restore_request_config(store.request(request)).explain(role));
        return 0;
    }
    if (command == "amend") {
        const auto amendment = read_runtime_file(options.required("--text-file"), 65536);
        const auto catalog = parse_json_strict(read_runtime_file(options.required("--constraints-file")));
        validate_constraint_catalog_shape(catalog);
        options.finish();
        Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        const auto actor = store.host_actor(Role::Root, request);
        output(store.amend(actor, integer(store.request(request).at("state_version")), amendment, catalog, new_id("amend")));
        return 0;
    }
    if (command == "cancel") {
        options.finish(); Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
        output(store.cancel_request(store.host_actor(Role::Root, request), integer(store.request(request).at("state_version")), new_id("cancel")));
        return 0;
    }
    const auto script = fake_model_steps_from_json(parse_json_strict(read_runtime_file(options.required("--script"))));
    const auto disposition = options.take("--disposition");
    // Validate the bounded disposition vocabulary before any Takeover Store or
    // scripted effect, then preflight any supplied disposition through a
    // read-only Store requiring a pending AMEND. Applicability to the current
    // task state is still rechecked by the runtime; an absent database keeps
    // its existing open semantics and is never created by the read-only open.
    if (!(disposition.empty() || is_amendment_disposition(disposition)))
        throw Error(ErrorCode::InvalidInput, "disposition must be empty or one of update, keep, or cancel");
    const auto workspace = options.take("--workspace");
    const auto manifest_path = options.take("--sandbox-manifest");
    const auto worker_calls = options.number("--worker-calls", 1, 1);
    if (worker_calls > 16 || (workspace.empty() && (worker_calls != 1 || !manifest_path.empty())))
        throw Error(ErrorCode::InvalidInput, "multiple Worker calls and sandbox manifests require --workspace; limit is 16");
    const auto manifest = manifest_path.empty() ? Json(Object{}) : parse_json_strict(read_runtime_file(manifest_path, 262144));
    options.finish();
    const SchemaRegistry schemas(resources.schema_documents());
    if (!disposition.empty()) {
        Store inspect(database, migrations, StoreOpenMode::ReadOnly, StoreRecoveryScope{});
        const auto tasks = inspect.tasks(request);
        if (tasks.size() > 1)
            throw Error(ErrorCode::Unsupported, "offline runner supports one task per request");
        const auto record = inspect.request(request);
        const bool pending_amend = has_pending_amendment(tasks, record);
        if (!pending_amend)
            throw Error(ErrorCode::InvalidInput, "disposition supplied without a pending AMEND");
    }
    Store store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::request(request));
    std::unique_ptr<WorkerTools> worker_tools;
    if (!workspace.empty()) worker_tools = std::make_unique<WorkerTools>(store, workspace, manifest);
    output(OfflineRuntime(store, schemas, worker_tools.get()).run(request, script, disposition, worker_calls));
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try { return run_cli(argc, argv); }
    catch (const Error& error) {
        output_error(error_name(error.code()), error.what());
        return error.code() == ErrorCode::Stale || error.code() == ErrorCode::Conflict ? 3 : 2;
    } catch (const std::exception& error) {
        output_error("runtime_error", error.what());
        return 1;
    }
}
