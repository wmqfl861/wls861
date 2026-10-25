#include "wls/context.hpp"
#include "wls/structured_context.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace wls::s1 {
namespace {

void require(bool valid, const std::string& message) {
    if (!valid) throw Error(ErrorCode::InvalidInput, message);
}
void capacity(bool valid, const std::string& message) {
    if (!valid) throw Error(ErrorCode::Quota, message);
}

std::string role_text(Role role) {
    switch (role) {
    case Role::Root: return "root";
    case Role::Dispatcher: return "dispatcher";
    case Role::TeamLead: return "team_lead";
    case Role::Worker: return "worker";
    }
    throw Error(ErrorCode::InvalidInput, "unknown context role");
}

Role parse_role(const std::string& role) {
    if (role == "root") return Role::Root;
    if (role == "dispatcher") return Role::Dispatcher;
    if (role == "team_lead") return Role::TeamLead;
    if (role == "worker") return Role::Worker;
    throw Error(ErrorCode::InvalidInput, "unknown context role");
}

std::string reasoning_text(ReasoningMode mode) {
    switch (mode) {
    case ReasoningMode::Disabled: return "off";
    case ReasoningMode::ProviderDefault: return "provider_default";
    case ReasoningMode::Explicit: return "effort";
    case ReasoningMode::Inherit: break;
    }
    throw Error(ErrorCode::InvalidInput, "compiled context needs resolved reasoning");
}

void validate_model(const ResolvedModel& model) {
    require(!model.model.empty(), "compiled context needs a model reference");
    (void)reasoning_text(model.reasoning.mode);
    require((model.reasoning.mode == ReasoningMode::Explicit) ==
            !model.reasoning.effort.empty(), "invalid compiled reasoning value");
}

bool valid_sha256(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void append_frame(std::string& destination, std::string_view name,
                  const std::string& content, std::size_t maximum) {
    const std::string header = std::string(name) + " " +
                               std::to_string(content.size()) + "\n";
    capacity(destination.size() <= maximum && header.size() <= maximum - destination.size(),
            "context byte limit exceeded; no content was truncated");
    const auto available = maximum - destination.size() - header.size();
    capacity(content.size() < available,
            "context byte limit exceeded; no content was truncated");
    destination += header;
    destination += content;
    destination += '\n';
}

boost::json::array tool_names_json(const CompiledContext& context) {
    boost::json::array names;
    for (const auto& name : context.tool_names) names.emplace_back(name);
    return names;
}

Json cache_descriptor(const CompiledContext& context) {
    return boost::json::object{
        {"format", "wls-context-cache-v1"}, {"role", role_text(context.role)},
        {"prefix_sha256", context.prefix_sha256},
        {"adapter_version", context.adapter_version},
        {"protocol_version", context.protocol_version},
        {"model_ref", context.model.model},
        {"reasoning_mode", reasoning_text(context.model.reasoning.mode)},
        {"reasoning_effort", context.model.reasoning.effort},
        {"tool_names", tool_names_json(context)}};
}

const boost::json::object& object(const Json& value) {
    require(value.is_object(), "context must be a JSON object");
    return value.as_object();
}

std::string string_field(const boost::json::object& value, std::string_view name) {
    const auto* field = value.if_contains(name);
    require(field != nullptr && field->is_string(),
            "missing or invalid context field: " + std::string(name));
    return std::string(field->as_string());
}

void check_fields(const boost::json::object& value,
                  const std::set<std::string>& allowed) {
    require(value.size() == allowed.size(), "missing or extra compiled context fields");
    for (const auto& field : value) {
        require(allowed.contains(std::string(field.key())),
                "unknown compiled context field: " + std::string(field.key()));
    }
}

std::string read_frame(std::string_view& bytes, std::string_view expected_name) {
    const std::string start = std::string(expected_name) + " ";
    require(bytes.starts_with(start), "compiled context has missing or reordered frame");
    bytes.remove_prefix(start.size());
    const auto end = bytes.find('\n');
    require(end != std::string_view::npos && end > 0 && end <= 20,
            "compiled context has invalid frame length");
    const auto digits = bytes.substr(0, end);
    require((digits.size() == 1 || digits.front() != '0') &&
            std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }),
            "compiled context frame length is not canonical");
    std::size_t size = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), size);
    require(parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size(),
            "compiled context frame length overflow");
    bytes.remove_prefix(end + 1);
    require(size < bytes.size() && bytes[size] == '\n', "compiled context frame is truncated");
    std::string content(bytes.substr(0, size));
    bytes.remove_prefix(size + 1);
    return content;
}

void validate_compiled_frames(const CompiledContext& context) {
    const ContextLimits limits;
    require(context.stable_prefix.size() <= limits.max_prefix_bytes &&
            context.dynamic_suffix.size() <= limits.max_total_bytes - context.stable_prefix.size(),
            "compiled context exceeds restore byte limits");
    ContextInput input;
    input.role = context.role;
    input.model = context.model;
    input.adapter_version = context.adapter_version;
    input.protocol_version = context.protocol_version;
    std::string_view prefix(context.stable_prefix);
    prefix.remove_prefix(std::string_view("WLS-CONTEXT/1\n").size());
    const auto tool_values = parse_json_strict(read_frame(prefix, "tools"));
    require(tool_values.is_array() && tool_values.as_array().size() <= limits.max_tools,
            "compiled tool frame must be a bounded array");
    for (const auto& tool : tool_values.as_array()) {
        const auto& source = object(tool);
        check_fields(source, {"name", "version", "definition"});
        input.tools.push_back({string_field(source, "name"), string_field(source, "version"),
                               source.at("definition")});
    }
    require(read_frame(prefix, "role") == role_text(context.role),
            "compiled role metadata differs from its actual frame");
    input.system_prompt = read_frame(prefix, "system");
    input.role_prompt = read_frame(prefix, "role_prompt");
    input.function_prompt = read_frame(prefix, "function");
    input.environment_card = read_frame(prefix, "environment");
    input.approved_rules = read_frame(prefix, "rules");
    input.project_snapshot = read_frame(prefix, "project");
    const auto dependency_values = parse_json_strict(read_frame(prefix, "dependencies"));
    require(dependency_values.is_array() && dependency_values.as_array().size() <= limits.max_dependencies,
            "compiled dependency frame must be a bounded array");
    for (const auto& dependency : dependency_values.as_array()) {
        const auto& source = object(dependency);
        check_fields(source, {"name", "version", "content_sha256"});
        input.dependencies.push_back({string_field(source, "name"), string_field(source, "version"),
                                       string_field(source, "content_sha256")});
    }
    require(prefix.empty(), "compiled context prefix has trailing frames");
    std::string_view dynamic(context.dynamic_suffix);
    input.task_contract = read_frame(dynamic, "task_contract");
    while (!dynamic.empty()) {
        require(input.messages.size() < limits.max_messages, "compiled message count exceeds restore limit");
        const auto message = parse_json_strict(read_frame(dynamic, "message"));
        const auto& source = object(message);
        check_fields(source, {"role", "content"});
        input.messages.push_back({string_field(source, "role"), string_field(source, "content")});
    }
    // Reuse the compiler's validation and canonical ordering. Metadata must
    // describe the exact tools and role bytes actually handed to the adapter.
    const auto rebuilt = ContextCompiler(limits).compile(input);
    require(rebuilt.tool_names == context.tool_names && rebuilt.stable_prefix == context.stable_prefix &&
            rebuilt.dynamic_suffix == context.dynamic_suffix,
            "compiled context frames differ from canonical metadata and bytes");
}

} // namespace

std::string CompiledContext::prompt() const {
    return structured() ? canonical_json(logical_input) : stable_prefix + dynamic_suffix;
}

ContextCompiler::ContextCompiler(ContextLimits limits) : limits_(limits) {
    require(limits_.max_prefix_bytes > 0 && limits_.max_total_bytes >= limits_.max_prefix_bytes &&
            limits_.max_tools > 0 && limits_.max_dependencies > 0 && limits_.max_messages > 0,
            "invalid context compiler limits");
}

CompiledContext ContextCompiler::compile(const ContextInput& input) const {
    validate_model(input.model);
    require(!input.adapter_version.empty() && !input.protocol_version.empty(),
            "context requires frozen adapter and protocol versions");
    require(!input.system_prompt.empty() && !input.role_prompt.empty() &&
            !input.function_prompt.empty() && !input.task_contract.empty(),
            "context requires system, role, function and authoritative task content");
    capacity(input.tools.size() <= limits_.max_tools &&
            input.dependencies.size() <= limits_.max_dependencies &&
            input.messages.size() <= limits_.max_messages, "context item limit exceeded");

    CompiledContext result;
    result.role = input.role;
    result.model = input.model;
    result.adapter_version = input.adapter_version;
    result.protocol_version = input.protocol_version;
    result.stable_prefix = "WLS-CONTEXT/1\n";

    auto tools = input.tools;
    std::sort(tools.begin(), tools.end(), [](const ContextTool& left, const ContextTool& right) {
        return left.name < right.name;
    });
    boost::json::array tool_values;
    std::set<std::string> seen;
    for (const auto& tool : tools) {
        require(!tool.name.empty() && !tool.version.empty() && tool.definition.is_object(),
                "tool context needs name, version and an object definition");
        require(seen.insert(tool.name).second, "duplicate context tool name");
        result.tool_names.push_back(tool.name);
        tool_values.emplace_back(boost::json::object{
            {"name", tool.name}, {"version", tool.version}, {"definition", tool.definition}});
    }
    append_frame(result.stable_prefix, "tools", canonical_json(tool_values), limits_.max_prefix_bytes);
    append_frame(result.stable_prefix, "role", role_text(input.role), limits_.max_prefix_bytes);
    append_frame(result.stable_prefix, "system", input.system_prompt, limits_.max_prefix_bytes);
    append_frame(result.stable_prefix, "role_prompt", input.role_prompt, limits_.max_prefix_bytes);
    append_frame(result.stable_prefix, "function", input.function_prompt, limits_.max_prefix_bytes);
    append_frame(result.stable_prefix, "environment", input.environment_card, limits_.max_prefix_bytes);
    append_frame(result.stable_prefix, "rules", input.approved_rules, limits_.max_prefix_bytes);
    append_frame(result.stable_prefix, "project", input.project_snapshot, limits_.max_prefix_bytes);

    auto dependencies = input.dependencies;
    std::sort(dependencies.begin(), dependencies.end(),
              [](const ContextDependency& left, const ContextDependency& right) {
        return left.name < right.name;
    });
    boost::json::array dependency_values;
    seen.clear();
    for (const auto& dependency : dependencies) {
        require(!dependency.name.empty() && !dependency.version.empty() &&
                valid_sha256(dependency.content_sha256), "invalid context dependency");
        require(seen.insert(dependency.name).second, "duplicate context dependency");
        dependency_values.emplace_back(boost::json::object{
            {"name", dependency.name}, {"version", dependency.version},
            {"content_sha256", dependency.content_sha256}});
    }
    append_frame(result.stable_prefix, "dependencies", canonical_json(dependency_values),
                 limits_.max_prefix_bytes);
    result.prefix_sha256 = sha256_hex(result.stable_prefix);
    result.cache_identity = sha256_hex(canonical_json(cache_descriptor(result)));

    const auto remaining = limits_.max_total_bytes - result.stable_prefix.size();
    append_frame(result.dynamic_suffix, "task_contract", input.task_contract, remaining);
    // Keep message order exactly. Worker history is append-only; management
    // snapshots are supplied afresh by the persistent host runner.
    for (const auto& message : input.messages) {
        require(message.role == "user" || message.role == "assistant" ||
                message.role == "tool" || message.role == "system",
                "invalid context message role");
        append_frame(result.dynamic_suffix, "message", canonical_json(boost::json::object{
            {"role", message.role}, {"content", message.content}}), remaining);
    }
    return result;
}

std::string compiled_task_contract(const CompiledContext& context) {
    if (context.structured()) return string_field(object(context.logical_input), "task_contract");
    std::string_view bytes(context.dynamic_suffix);
    return read_frame(bytes, "task_contract");
}

Json to_json(const CompiledContext& context) {
    if (context.structured()) return structured_context_to_json(context);
    validate_model(context.model);
    return boost::json::object{
        {"format", "wls-compiled-context-v1"}, {"role", role_text(context.role)},
        {"model_ref", context.model.model},
        {"reasoning_mode", reasoning_text(context.model.reasoning.mode)},
        {"reasoning_effort", context.model.reasoning.effort},
        {"adapter_version", context.adapter_version},
        {"protocol_version", context.protocol_version},
        {"stable_prefix", context.stable_prefix}, {"dynamic_suffix", context.dynamic_suffix},
        {"prefix_sha256", context.prefix_sha256}, {"cache_identity", context.cache_identity},
        {"tool_names", tool_names_json(context)}};
}

CompiledContext compiled_context_from_json(const Json& value) {
    const auto& source = object(value);
    if (string_field(source, "format") == "wls-compiled-context-v2")
        return structured_context_from_json(value);
    check_fields(source, {"format", "role", "model_ref", "reasoning_mode", "reasoning_effort",
        "adapter_version", "protocol_version", "stable_prefix", "dynamic_suffix",
        "prefix_sha256", "cache_identity", "tool_names"});
    require(string_field(source, "format") == "wls-compiled-context-v1",
            "unsupported compiled context format");
    CompiledContext context;
    context.role = parse_role(string_field(source, "role"));
    context.model.model = string_field(source, "model_ref");
    const auto reasoning = string_field(source, "reasoning_mode");
    if (reasoning == "off") context.model.reasoning.mode = ReasoningMode::Disabled;
    else if (reasoning == "provider_default") context.model.reasoning.mode = ReasoningMode::ProviderDefault;
    else if (reasoning == "effort") context.model.reasoning.mode = ReasoningMode::Explicit;
    else throw Error(ErrorCode::InvalidInput, "invalid compiled reasoning mode");
    context.model.reasoning.effort = string_field(source, "reasoning_effort");
    validate_model(context.model);
    context.adapter_version = string_field(source, "adapter_version");
    context.protocol_version = string_field(source, "protocol_version");
    context.stable_prefix = string_field(source, "stable_prefix");
    context.dynamic_suffix = string_field(source, "dynamic_suffix");
    context.prefix_sha256 = string_field(source, "prefix_sha256");
    context.cache_identity = string_field(source, "cache_identity");
    require(!context.adapter_version.empty() && !context.protocol_version.empty() &&
            context.stable_prefix.starts_with("WLS-CONTEXT/1\n") &&
            context.dynamic_suffix.starts_with("task_contract "), "invalid compiled context bytes");
    const auto& names = source.at("tool_names");
    require(names.is_array(), "compiled context tool names must be an array");
    std::set<std::string> seen;
    for (const auto& name : names.as_array()) {
        require(name.is_string() && !name.as_string().empty(), "invalid compiled tool name");
        const std::string text(name.as_string());
        require(seen.insert(text).second, "duplicate compiled tool name");
        context.tool_names.push_back(text);
    }
    require(std::is_sorted(context.tool_names.begin(), context.tool_names.end()),
            "compiled context tool names must use canonical order");
    require(context.prefix_sha256 == sha256_hex(context.stable_prefix),
            "compiled context prefix hash mismatch");
    require(context.cache_identity == sha256_hex(canonical_json(cache_descriptor(context))),
            "compiled context cache identity mismatch");
    validate_compiled_frames(context);
    return context;
}

} // namespace wls::s1
