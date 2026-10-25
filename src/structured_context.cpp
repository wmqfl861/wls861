#include "wls/structured_context.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string_view>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

void require(bool valid, const std::string& message) {
    if (!valid) throw Error(ErrorCode::InvalidInput, message);
}
void capacity(bool valid, const std::string& message) {
    if (!valid) throw Error(ErrorCode::Quota, message);
}
const Object& object(const Json& value) {
    require(value.is_object(), "structured context needs an object");
    return value.as_object();
}
const Json& field(const Json& value, std::string_view key) {
    const auto* result = object(value).if_contains(key);
    require(result != nullptr, "missing structured context field: " + std::string(key));
    return *result;
}
std::string text(const Json& value, std::string_view key) {
    const auto& result = field(value, key);
    require(result.is_string(), "structured context field must be text: " + std::string(key));
    return std::string(result.as_string());
}
const Array& array(const Json& value) {
    require(value.is_array(), "structured context needs an array");
    return value.as_array();
}
void exact(const Json& value, const std::set<std::string>& keys) {
    require(object(value).size() == keys.size(), "missing or extra structured context fields");
    for (const auto& entry : object(value))
        require(keys.contains(std::string(entry.key())), "unknown structured context field");
}
std::string reasoning(const ResolvedModel& model) {
    switch (model.reasoning.mode) {
    case ReasoningMode::Disabled: return "off";
    case ReasoningMode::ProviderDefault: return "provider_default";
    case ReasoningMode::Explicit: return "effort";
    case ReasoningMode::Inherit: break;
    }
    throw Error(ErrorCode::InvalidInput, "structured context needs resolved reasoning");
}
Role role(const std::string& value) {
    if (value == "root") return Role::Root;
    if (value == "dispatcher") return Role::Dispatcher;
    if (value == "team_lead") return Role::TeamLead;
    if (value == "worker") return Role::Worker;
    throw Error(ErrorCode::InvalidInput, "invalid structured role");
}
Json selection(const CompiledContext& c) {
    return Object{{"role", role_name(c.role)}, {"model_ref", c.model.model},
        {"reasoning_mode", reasoning(c.model)}, {"reasoning_effort", c.model.reasoning.effort},
        {"adapter_version", c.adapter_version}, {"protocol_version", c.protocol_version}};
}
std::string cache_hash(const CompiledContext& c) {
    return sha256_hex(canonical_json(Object{{"format", "wls-context-cache-v2"},
        {"selection", selection(c)}, {"prefix_sha256", c.prefix_sha256}}));
}
bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](char ch) {
            const auto c = static_cast<unsigned char>(ch);
            return c >= 33 && c <= 126;
        });
}
Array encode_messages(const std::vector<StructuredContextMessage>& messages,
                      const ContextLimits& limits) {
    capacity(messages.size() <= limits.max_messages, "structured message count exceeds limit");
    Array result;
    std::set<std::string> seen, pending;
    for (const auto& message : messages) {
        require(message.role == "user" || message.role == "assistant" || message.role == "tool",
                "dynamic structured messages cannot supply system authority");
        require(message.source.is_null() || message.source.is_object(), "message source must be object or null");
        Array calls;
        if (message.role == "tool") {
            require(message.tool_calls.empty() && identifier(message.tool_call_id) &&
                    pending.erase(message.tool_call_id) == 1, "orphan or repeated tool result");
        } else {
            require(pending.empty(), "tool turn must settle before another message");
            require(message.tool_call_id.empty(), "non-tool message cannot claim a result ID");
            require(message.role == "assistant" || message.tool_calls.empty(), "only assistant messages invoke tools");
            capacity(message.tool_calls.size() <= 32, "structured tool turn exceeds capacity");
            for (const auto& call : message.tool_calls) {
                require(identifier(call.call_id) && identifier(call.name) && call.arguments.is_object(),
                        "invalid structured tool invocation");
                require(seen.insert(call.call_id).second, "tool IDs must be unique across the transcript");
                pending.insert(call.call_id);
                calls.push_back(Object{{"call_id", call.call_id}, {"name", call.name}, {"arguments", call.arguments}});
            }
        }
        result.push_back(Object{{"role", message.role}, {"content", message.content},
            {"tool_calls", calls}, {"tool_call_id", message.tool_call_id}, {"source", message.source}});
    }
    require(pending.empty(), "structured transcript ends with an unpaired tool call");
    return result;
}
} // namespace

CompiledContext compile_structured_context(const ContextInput& base,
        const std::vector<StructuredContextMessage>& messages, ContextLimits limits) {
    require(base.messages.empty(), "legacy messages must be explicitly converted, never discarded");
    // Reuse the established validation of model selection, required sections,
    // tool definitions and dependency hashes. No adapter parses legacy frames.
    auto result = ContextCompiler(limits).compile(base);
    auto tools = base.tools;
    std::sort(tools.begin(), tools.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    Array definitions;
    for (const auto& tool : tools)
        definitions.push_back(Object{{"name", tool.name}, {"version", tool.version}, {"parameters", tool.definition}});
    auto dependencies = base.dependencies;
    std::sort(dependencies.begin(), dependencies.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    Array sources;
    for (const auto& dep : dependencies)
        sources.push_back(Object{{"name", dep.name}, {"version", dep.version}, {"content_sha256", dep.content_sha256}});
    const Json prefix = Object{{"format", "wls-logical-prefix-v1"}, {"tools", definitions},
        {"system", Object{{"system_prompt", base.system_prompt}, {"role_prompt", base.role_prompt},
            {"function_prompt", base.function_prompt}, {"environment_card", base.environment_card},
            {"approved_rules", base.approved_rules}, {"project_snapshot", base.project_snapshot}}},
        {"dependencies", sources}};
    const auto encoded = encode_messages(messages, limits);
    result.stable_prefix = canonical_json(prefix);
    result.dynamic_suffix = canonical_json(Object{{"task_contract", base.task_contract}, {"messages", encoded}});
    result.logical_input = Object{{"format", "wls-logical-input-v1"}, {"selection", selection(result)},
        {"prefix", prefix}, {"task_contract", base.task_contract}, {"messages", encoded}};
    capacity(result.stable_prefix.size() <= limits.max_prefix_bytes &&
             result.prompt().size() <= limits.max_total_bytes,
             "structured context byte limit exceeded; no authoritative content was truncated");
    result.prefix_sha256 = sha256_hex(result.stable_prefix);
    result.cache_identity = cache_hash(result);
    result.logical_input_sha256 = sha256_hex(result.prompt());
    return result;
}

Json structured_context_to_json(const CompiledContext& c) {
    require(c.structured(), "expected a structured context");
    require(field(c.logical_input, "selection") == selection(c), "logical selection differs from context metadata");
    require(c.stable_prefix == canonical_json(field(c.logical_input, "prefix")) &&
            c.dynamic_suffix == canonical_json(Object{{"task_contract", field(c.logical_input, "task_contract")},
                {"messages", field(c.logical_input, "messages")}}), "structured context projections differ from original input");
    Object result = selection(c).as_object();
    result["format"] = "wls-compiled-context-v2";
    result["input"] = c.logical_input;
    result["prefix_sha256"] = c.prefix_sha256;
    result["cache_identity"] = c.cache_identity;
    result["logical_input_sha256"] = c.logical_input_sha256;
    result["wire_sha256"] = nullptr; // No transport has run. Never alias the logical hash.
    Array names;
    for (const auto& name : c.tool_names) names.emplace_back(name);
    result["tool_names"] = names;
    return result;
}

CompiledContext structured_context_from_json(const Json& value) {
    exact(value, {"format", "role", "model_ref", "reasoning_mode", "reasoning_effort",
        "adapter_version", "protocol_version", "input", "prefix_sha256", "cache_identity",
        "logical_input_sha256", "wire_sha256", "tool_names"});
    require(text(value, "format") == "wls-compiled-context-v2" && field(value, "wire_sha256").is_null(),
            "unsupported structured context or fabricated wire fingerprint");
    const auto& input = field(value, "input");
    exact(input, {"format", "selection", "prefix", "task_contract", "messages"});
    require(text(input, "format") == "wls-logical-input-v1", "unsupported logical input format");
    // Bound canonical logical bytes before reconstructing a compiled context.
    const ContextLimits limits;
    capacity(canonical_json(input).size() <= limits.max_total_bytes, "structured restore exceeds byte limit");
    const auto& prefix = field(input, "prefix");
    exact(prefix, {"format", "tools", "system", "dependencies"});
    require(text(prefix, "format") == "wls-logical-prefix-v1", "unsupported logical prefix format");
    const auto& system = field(prefix, "system");
    exact(system, {"system_prompt", "role_prompt", "function_prompt", "environment_card", "approved_rules", "project_snapshot"});
    ContextInput base;
    base.role = role(text(value, "role")); base.model.model = text(value, "model_ref");
    const auto mode = text(value, "reasoning_mode");
    if (mode == "off") base.model.reasoning.mode = ReasoningMode::Disabled;
    else if (mode == "provider_default") base.model.reasoning.mode = ReasoningMode::ProviderDefault;
    else if (mode == "effort") base.model.reasoning.mode = ReasoningMode::Explicit;
    else throw Error(ErrorCode::InvalidInput, "invalid structured reasoning mode");
    base.model.reasoning.effort = text(value, "reasoning_effort");
    base.adapter_version = text(value, "adapter_version"); base.protocol_version = text(value, "protocol_version");
    base.system_prompt = text(system, "system_prompt"); base.role_prompt = text(system, "role_prompt");
    base.function_prompt = text(system, "function_prompt"); base.environment_card = text(system, "environment_card");
    base.approved_rules = text(system, "approved_rules"); base.project_snapshot = text(system, "project_snapshot");
    base.task_contract = text(input, "task_contract");
    capacity(array(field(prefix, "tools")).size() <= limits.max_tools &&
             array(field(prefix, "dependencies")).size() <= limits.max_dependencies,
             "structured restore exceeds item capacity");
    for (const auto& tool : array(field(prefix, "tools"))) {
        exact(tool, {"name", "version", "parameters"});
        base.tools.push_back({text(tool, "name"), text(tool, "version"), field(tool, "parameters")});
    }
    for (const auto& dep : array(field(prefix, "dependencies"))) {
        exact(dep, {"name", "version", "content_sha256"});
        base.dependencies.push_back({text(dep, "name"), text(dep, "version"), text(dep, "content_sha256")});
    }
    std::vector<StructuredContextMessage> messages;
    capacity(array(field(input, "messages")).size() <= limits.max_messages, "structured restore exceeds message capacity");
    for (const auto& item : array(field(input, "messages"))) {
        exact(item, {"role", "content", "tool_calls", "tool_call_id", "source"});
        StructuredContextMessage message;
        message.role = text(item, "role"); message.content = text(item, "content");
        message.tool_call_id = text(item, "tool_call_id"); message.source = field(item, "source");
        capacity(array(field(item, "tool_calls")).size() <= 32, "structured restore exceeds tool turn capacity");
        for (const auto& call : array(field(item, "tool_calls"))) {
            exact(call, {"call_id", "name", "arguments"});
            message.tool_calls.push_back({text(call, "call_id"), text(call, "name"), field(call, "arguments")});
        }
        messages.push_back(std::move(message));
    }
    auto result = compile_structured_context(base, messages);
    require(structured_context_to_json(result) == value,
            "structured context differs from its canonical structure, metadata or fingerprints");
    return result;
}
} // namespace wls::s1
