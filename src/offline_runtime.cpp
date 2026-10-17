#include "wls/offline_runtime.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <set>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

void require(bool valid, ErrorCode code, const std::string& message) {
    if (!valid) throw Error(code, message);
}
std::string string_at(const Json& value, std::string_view key) {
    require(value.is_object() && value.as_object().contains(key) && value.at(key).is_string(),
            ErrorCode::InvalidInput, "string field missing: " + std::string(key));
    return std::string(value.at(key).as_string());
}
std::int64_t integer_at(const Json& value, std::string_view key) {
    require(value.is_object() && value.as_object().contains(key), ErrorCode::InvalidInput, "integer field missing: " + std::string(key));
    const auto& number = value.at(key);
    if (number.is_int64()) return number.as_int64();
    if (number.is_uint64() && number.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return static_cast<std::int64_t>(number.as_uint64());
    throw Error(ErrorCode::InvalidInput, "invalid integer field: " + std::string(key));
}
std::int64_t increment(std::int64_t value) {
    require(value < std::numeric_limits<std::int64_t>::max(), ErrorCode::Quota, "revision exhausted");
    return value + 1;
}

const ConfigDependency& dependency(const FrozenRequestConfig& config, std::string_view path) {
    const auto& dependencies = config.dependencies();
    const auto found = std::find_if(dependencies.begin(), dependencies.end(), [&](const auto& item) { return item.path == path; });
    require(found != dependencies.end(), ErrorCode::Corrupt, "frozen dependency missing: " + std::string(path));
    return *found;
}

Json note_schema() {
    const Object text_field{{"type", "string"}, {"minLength", 1}, {"maxLength", 4000}};
    return Object{{"type", "object"}, {"additionalProperties", false}, {"required", Array{"text"}},
        {"properties", Object{{"text", text_field}}}};
}

std::vector<ContextTool> tools_for(const SchemaRegistry& schemas, Role role) {
    if (role == Role::Dispatcher) return {{"submit_plan", "v0.4.1", schemas.project_model_input("plan")}};
    if (role == Role::TeamLead) return {{"issue_work_order", "v0.4.1", schemas.project_model_input("work_order")}};
    if (role == Role::Worker) return {{"note", "wls-offline-note-v1", note_schema()}};
    throw Error(ErrorCode::Unsupported, "offline admission is host-owned; no Root model runner yet");
}

ContextInput make_context(const FrozenRequestConfig& config, Role role,
                          const std::string& template_id,
                          const std::vector<ContextTool>& tools, const Json& contract) {
    ContextInput input;
    input.role = role;
    input.model = config.resolve(role);
    const auto profile = config.model_profile(role);
    input.adapter_version = string_at(profile, "adapter_version");
    input.protocol_version = string_at(profile, "protocol");
    input.tools = tools;
    input.dependencies.push_back({"model_profiles/" + input.model.model,
                                  input.adapter_version, sha256_hex(canonical_json(profile))});
    const auto role_id = std::string(role_name(role));
    const auto prefix = "functions/" + config.function_id() + "/";
    const auto& function = config.json().at("function");
    const auto& role_config = function.at("roles").at(role_id);
    const auto& common = dependency(config, "prompts/common.md");
    const auto& role_prompt = dependency(config, "prompts/" + role_id + ".md");
    input.system_prompt = common.content;
    input.role_prompt = role_prompt.content;
    input.dependencies.push_back({common.path, "content", common.sha256});
    input.dependencies.push_back({role_prompt.path, "content", role_prompt.sha256});
    if (role_config.as_object().contains("prompt_append")) {
        const auto& function_prompt = dependency(config, prefix + string_at(role_config, "prompt_append"));
        input.function_prompt = function_prompt.content;
        input.dependencies.push_back({function_prompt.path, "content", function_prompt.sha256});
    }
    if (input.function_prompt.empty()) {
        input.function_prompt = string_at(function, "description");
        input.dependencies.push_back({prefix + "description", std::to_string(integer_at(function, "version")),
                                      sha256_hex(input.function_prompt)});
    }
    const auto& selected_template = function.at("team_templates").at(template_id);
    const auto& environment = dependency(config, prefix + string_at(selected_template, "env"));
    input.environment_card = canonical_json(Object{
        {"adapter", "fake"}, {"checks_executed", false}, {"business_executor", "durable_note_only"},
        {"template", environment.content},
        {"template_sha256", environment.sha256}});
    input.dependencies.push_back({environment.path, "content", environment.sha256});
    input.dependencies.push_back({prefix + "selected_template/" + template_id,
        std::to_string(integer_at(function, "version")), sha256_hex(canonical_json(selected_template))});
    input.approved_rules = "This offline host only records a note. Environment checks have not run. "
        "Do not claim file creation, shell execution, sandbox isolation, delivery, or real provider usage.";
    input.project_snapshot = "No project files loaded by this offline increment.";
    input.task_contract = canonical_json(contract);
    return input;
}

void check_plan_supported(const Json& plan, const FrozenRequestConfig& config) {
    const auto& nodes = plan.at("nodes").as_array();
    require(nodes.size() == 1, ErrorCode::Unsupported, "offline runner requires exactly one task; DAG runners remain a later card");
    const auto& node = nodes.front();
    require(string_at(node, "kind") == "team_task" && node.at("depends_on").as_array().empty() &&
        !node.as_object().contains("review"), ErrorCode::Unsupported, "offline runner needs an independent team_task without an automatic review");
    const auto template_id = node.as_object().contains("template") ? string_at(node, "template") : "default";
    require(config.worker_tools(template_id).contains("note"), ErrorCode::Unauthorized, "selected Worker template does not grant note");
}
std::string outcome(ModelResultStatus status) {
    switch (status) {
    case ModelResultStatus::Completed: return "COMPLETED";
    case ModelResultStatus::Error: return "ERROR";
    case ModelResultStatus::Interrupted: return "INTERRUPTED";
    }
    throw Error(ErrorCode::InvalidInput, "invalid model result status");
}
} // namespace

std::string read_runtime_file(const std::filesystem::path& path, std::size_t max_bytes) {
    require(!path.empty() && path.string().find('\0') == std::string::npos, ErrorCode::InvalidInput, "invalid file path");
    std::filesystem::path absolute;
    try { absolute = std::filesystem::absolute(path); }
    catch (const std::filesystem::filesystem_error&) {
        throw Error(ErrorCode::InvalidInput, "cannot resolve explicit input path");
    }
    struct Close { int fd; ~Close() { if (fd >= 0) ::close(fd); } };
    Close directory{::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
    require(directory.fd >= 0, ErrorCode::Storage, "cannot open filesystem root for input path");
    // Walk the original components instead of lexically collapsing '..': a
    // symbolic-link component must reject even when followed by a parent step.
    for (const auto& component : absolute.parent_path().relative_path()) {
        const int next = ::openat(directory.fd, component.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        require(next >= 0, ErrorCode::InvalidInput,
                "input parent is missing, inaccessible, or a symbolic link: " + path.string());
        ::close(directory.fd);
        directory.fd = next;
    }
    const int fd = ::openat(directory.fd, absolute.filename().c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    require(fd >= 0, ErrorCode::InvalidInput, "cannot open regular input file: " + path.string());
    Close close{fd};
    struct stat info {};
    require(::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
        static_cast<std::uint64_t>(info.st_size) <= max_bytes, ErrorCode::InvalidInput, "input is not a bounded regular file: " + path.string());
    std::string result;
    char buffer[8192];
    while (true) {
        const auto count = ::read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, ErrorCode::Storage, "input file read failed");
        if (count == 0) break;
        require(static_cast<std::size_t>(count) <= max_bytes - result.size(), ErrorCode::InvalidInput, "input grew past byte limit");
        result.append(buffer, static_cast<std::size_t>(count));
    }
    return result;
}

FrozenRequestConfig restore_request_config(const Json& record) {
    return FrozenRequestConfig::restore(canonical_json(record.at("config_snapshot")),
                                        string_at(record, "config_snapshot_sha256"));
}

OfflineRuntime::OfflineRuntime(Store& store, const SchemaRegistry& schemas) : store_(store), schemas_(schemas) {}

CapturedModelCall OfflineRuntime::reserve_call(const ModelAdapter& adapter,
        const std::string& request_id, Role role, const std::string& task_id, std::int64_t index) {
    require(!adapter.capabilities().real_calls, ErrorCode::Unsupported, "offline runner refuses real adapters");
    require((role == Role::Dispatcher && task_id.empty()) ||
        ((role == Role::TeamLead || role == Role::Worker) && !task_id.empty()), ErrorCode::InvalidInput, "role and task scope disagree");
    const auto record = store_.request(request_id);
    const auto config = restore_request_config(record);
    const auto task = task_id.empty() ? Json(nullptr) : store_.task(task_id);
    const auto binding = task_id.empty() ? store_.request_binding(request_id) : store_.binding(task_id);
    require(binding.request_id == request_id, ErrorCode::Unauthorized, "task belongs to another request");
    const auto actor = store_.host_actor(role, request_id, binding.team_id);
    const auto template_id = task_id.empty() ? "default" : string_at(task, "template_id");
    if (role == Role::Worker)
        require(config.worker_tools(template_id).contains("note"), ErrorCode::Unauthorized, "frozen Worker template does not grant note");
    Json contract = Object{{"request", store_.request_snapshot(request_id)}, {"task", task},
                           {"binding", to_json(binding)}};
    if (!task_id.empty()) contract.as_object()["instruction"] = store_.instruction(task_id);
    ModelRequest request;
    request.call_id = new_id("call"); request.attempt_id = new_id("attempt");
    request.role = role; request.role_call_index = index; request.binding = binding;
    request.config_snapshot_id = config.sha256(); request.model = config.resolve(role);
    request.context = ContextCompiler().compile(make_context(config, role, template_id, tools_for(schemas_, role), contract));
    const auto profile = config.model_profile(role);
    require(string_at(profile.at("capabilities"), "tool_calling") == "supported",
            ErrorCode::Unsupported, "frozen model profile does not support required tool calls");
    request.max_output_tokens = std::min<std::int64_t>(8192, integer_at(profile.at("capabilities"), "max_output_tokens"));
    // A conservative byte-based reservation is not a claim about provider
    // tokenization. Fake usage is explicit fixture data or remains unknown.
    require(request.context.prompt().size() < static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()), ErrorCode::Quota, "input reservation overflow");
    request.reserved_input_tokens = static_cast<std::int64_t>(request.context.prompt().size());
    request.reserved_output_tokens = request.max_output_tokens;
    request.reserved_cost_micro_usd = 0; // this adapter performs zero network calls
    adapter.validate(request);
    AttemptReservation reservation;
    reservation.call_id = request.call_id; reservation.attempt_id = request.attempt_id;
    reservation.config_snapshot_sha256 = request.config_snapshot_id; reservation.model_ref = request.model.model;
    reservation.reserved_input_tokens = request.reserved_input_tokens;
    reservation.reserved_output_tokens = request.reserved_output_tokens;
    reservation.reserved_cost_micro_usd = request.reserved_cost_micro_usd;
    reservation.max_output_tokens = request.max_output_tokens;
    reservation.request_envelope = to_json(request);
    static_cast<void>(store_.reserve_attempt(actor, binding, reservation));
    return {std::move(request), actor, integer_at(task_id.empty() ? record : task, "state_version")};
}

Json OfflineRuntime::accept_response(const CapturedModelCall& call, const ModelResponse& response,
                                    const std::string& disposition) {
    const auto& captured = call.request;
    // No current-row metadata is copied onto a response. A corrupted envelope
    // cannot impersonate another pending attempt; its reservation remains held.
    require(response.call_id == captured.call_id && response.attempt_id == captured.attempt_id,
            ErrorCode::Stale, "response call identity differs from captured attempt");
    const auto settlement = store_.settle_attempt(captured.attempt_id, response.binding,
        response.config_snapshot_id, outcome(response.status), to_json(response.usage), to_json(response));
    require_complete_response(captured, response);
    const auto& accepted = settlement.at("accepted");
    require((accepted.is_bool() && accepted.as_bool()) || (accepted.is_int64() && accepted.as_int64() == 1),
            ErrorCode::Stale, "response was recorded but cannot change current business state");
    require(response.tool_calls.size() == 1, ErrorCode::InvalidInput, "offline wake requires exactly one declared tool call");
    const auto& tool = response.tool_calls.front();
    const auto command = captured.attempt_id + ":" + tool.call_id;
    if (captured.role == Role::Dispatcher) {
        schemas_.validate("plan", tool.arguments, true);
        const auto config = restore_request_config(store_.request(captured.binding.request_id));
        check_plan_supported(tool.arguments, config);
        const auto existing = store_.tasks(captured.binding.request_id);
        require(existing.size() <= 1, ErrorCode::Unsupported, "offline runner cannot replace an existing multi-task plan");
        if (!existing.empty()) require(disposition == "update" || disposition == "keep" || disposition == "cancel",
            ErrorCode::InvalidInput, "existing AMEND task needs explicit update, keep, or cancel");
        Json plan = tool.arguments;
        plan.as_object()["plan_revision"] = increment(captured.binding.plan_revision);
        plan.as_object()["based_on_requirements_revision"] = captured.binding.requirements_revision;
        schemas_.validate("plan", plan);
        const auto& node = plan.at("nodes").as_array().front();
        // Validate the independent brief before committing the new plan.
        schemas_.validate("task_brief", node.at("brief"));
        store_.validate_brief(captured.binding.request_id, node.at("brief"));
        if (!existing.empty()) require(string_at(existing.front(), "node_id") == string_at(node, "node_id"),
            ErrorCode::Unsupported, "offline AMEND retains the independent node identity");
        if (!existing.empty()) {
            const auto selected = node.as_object().contains("template") ? string_at(node, "template") : "default";
            require(selected == string_at(existing.front(), "template_id"), ErrorCode::Unsupported,
                    "offline AMEND cannot change an allocated team template");
        }
        const auto result = store_.publish_plan(call.actor, call.expected_state_version,
            captured.binding.requirements_revision, plan, command + ":plan");
        if (existing.empty()) {
            const auto template_id = node.as_object().contains("template") ? string_at(node, "template") : "default";
            return store_.create_task(call.actor, integer_at(result, "state_version"), captured.binding.requirements_revision,
                integer_at(result, "plan_revision"), string_at(node, "node_id"), node.at("brief"), command + ":task", template_id);
        }
        return store_.set_amendment_disposition(call.actor, string_at(existing.front(), "task_id"),
            integer_at(existing.front(), "state_version"), captured.binding.requirements_revision,
            integer_at(result, "plan_revision"), disposition, "Explicit host-selected AMEND disposition for this offline invocation",
            node.at("brief"), command + ":disposition");
    }
    if (captured.role == Role::TeamLead) {
        schemas_.validate("work_order", tool.arguments, true);
        auto next = captured.binding;
        next.instruction_revision = increment(next.instruction_revision);
        Json order = tool.arguments;
        const auto envelope = to_json(next);
        for (const auto& field : envelope.as_object()) order.as_object()[field.key()] = field.value();
        schemas_.validate("work_order", order);
        return store_.issue_instruction(call.actor, captured.binding, call.expected_state_version, order, command);
    }
    require(captured.role == Role::Worker && tool.name == "note", ErrorCode::Unauthorized, "unsupported Worker tool");
    require(tool.arguments.is_object() && tool.arguments.as_object().size() == 1, ErrorCode::InvalidInput, "note accepts only text");
    const auto text = string_at(tool.arguments, "text");
    require(!text.empty() && text.size() <= 4000 && text.find('\0') == std::string::npos, ErrorCode::InvalidInput, "note text must contain 1 to 4000 bytes without NUL");
    const auto prepared = store_.prepare_operation(call.actor, captured.binding, call.expected_state_version,
        captured.attempt_id, "note", tool.arguments, "none", command + ":prepare");
    const auto id = string_at(prepared, "op_id");
    const auto started = store_.start_operation(call.actor, id, integer_at(prepared, "state_version"), command + ":start");
    return store_.finish_operation(call.actor, id, captured.binding, integer_at(started, "state_version"),
        "SUCCEEDED", "sqlite:operation_intents/" + id + "#sha256=" + sha256_hex(canonical_json(tool.arguments)), command + ":finish");
}

Json OfflineRuntime::run(const std::string& id, const std::vector<FakeModelStep>& steps, const std::string& disposition) {
    auto tasks = store_.tasks(id);
    require(tasks.size() <= 1, ErrorCode::Unsupported, "offline runner supports one task per request");
    auto record = store_.request(id);
    const auto config = restore_request_config(record);
    if (!tasks.empty() && integer_at(tasks.front(), "based_on_requirements_revision") == integer_at(record, "requirements_revision") &&
        integer_at(tasks.front(), "plan_revision") != integer_at(record, "plan_revision")) {
        // A published plan is already an authoritative durable decision. A
        // crash between publishing it and binding its task does not authorize
        // another interpretation of the original task; resume that decision.
        const auto stored_plan = store_.plan(id);
        const auto& body = stored_plan.at("body");
        schemas_.validate("plan", body);
        check_plan_supported(body, config);
        const auto& node = body.at("nodes").as_array().front();
        const auto selected = node.as_object().contains("template") ? string_at(node, "template") : "default";
        require(string_at(node, "node_id") == string_at(tasks.front(), "node_id") &&
                selected == string_at(tasks.front(), "template_id"), ErrorCode::Unsupported,
                "published plan changes task identity; explicit host migration is required");
        static_cast<void>(store_.rebind_task(store_.host_actor(Role::Dispatcher, id), string_at(tasks.front(), "task_id"),
            integer_at(tasks.front(), "state_version"), integer_at(record, "requirements_revision"), integer_at(record, "plan_revision"),
            node.at("brief"), new_id("resume")));
        tasks = store_.tasks(id);
    }
    const auto invoke = [&](Role role, const std::string& task_id) {
        const FakeModel fake(steps, config.resolve(role).model);
        const auto call = reserve_call(fake, id, role, task_id);
        const auto response = fake.complete(call.request);
        const auto result = accept_response(call, response, disposition);
        return Json(Object{{"attempt_id", call.request.attempt_id}, {"role", role_name(role)},
            {"config_snapshot_sha256", call.request.config_snapshot_id},
            {"prefix_sha256", call.request.context.prefix_sha256}, {"result", result}});
    };
    Array calls;
    if (tasks.empty() || integer_at(tasks.front(), "based_on_requirements_revision") != integer_at(record, "requirements_revision")) {
        if (!tasks.empty()) require(disposition == "update" || disposition == "keep" || disposition == "cancel",
            ErrorCode::InvalidInput, "AMEND requires --disposition update, keep, or cancel before another model call");
        calls.push_back(invoke(Role::Dispatcher, {}));
        tasks = store_.tasks(id);
    } else require(disposition.empty(), ErrorCode::InvalidInput, "disposition supplied without a pending AMEND");
    require(tasks.size() == 1, ErrorCode::NotReady, "plan has no executable task");
    const auto task_id = string_at(tasks.front(), "task_id");
    if (string_at(tasks.front(), "status") != "CANCELLED") {
        calls.push_back(invoke(Role::TeamLead, task_id));
        const auto binding = store_.binding(task_id);
        const auto worker = store_.host_actor(Role::Worker, id, binding.team_id);
        static_cast<void>(store_.acknowledge(worker, binding, integer_at(store_.task(task_id), "state_version"), new_id("ack")));
        calls.push_back(invoke(Role::Worker, task_id));
    }
    return Object{{"mode", "offline_fake"}, {"request_id", id}, {"task_id", task_id},
        {"config_snapshot_sha256", config.sha256()}, {"calls", calls}, {"budget", store_.budget(id)},
        {"request_status", store_.request(id).at("status")}, {"task_status", store_.task(task_id).at("status")},
        {"business_delivered", false}, {"environment_checks_executed", false}, {"real_model_calls", 0}};
}

} // namespace wls::s1
