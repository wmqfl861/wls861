#include "wls/structured_context.hpp"
#include "wls/worker_history.hpp"

#include "responses_compat.hpp"
#include "tool_descriptions.hpp"
#include "context_internal.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

// Only these three derived representations live on this invocation's stack.
// A source pointer never escapes, and a different representation regenerates
// its bytes. JSON equality alone conflates signed zero and integer kinds.
struct CanonicalArtifacts {
    const Json* input;
    std::optional<std::string> stable, dynamic, logical;
};
bool same_representation(const Json& left, const Json& right, std::size_t depth = 0) {
    if (depth > 128 || left.kind() != right.kind()) return false;
    switch (left.kind()) {
    case boost::json::kind::object:
        if (left.as_object().size() != right.as_object().size()) return false;
        for (const auto& item : left.as_object()) {
            const auto* other = right.as_object().if_contains(item.key());
            if (!other || !same_representation(item.value(), *other, depth + 1)) return false;
        }
        return true;
    case boost::json::kind::array:
        if (left.as_array().size() != right.as_array().size()) return false;
        for (std::size_t i = 0; i < left.as_array().size(); ++i)
            if (!same_representation(left.as_array()[i], right.as_array()[i], depth + 1)) return false;
        return true;
    case boost::json::kind::string: return left.as_string() == right.as_string();
    case boost::json::kind::int64: return left.as_int64() == right.as_int64();
    case boost::json::kind::uint64: return left.as_uint64() == right.as_uint64();
    case boost::json::kind::double_:
        return std::bit_cast<std::uint64_t>(left.as_double()) == std::bit_cast<std::uint64_t>(right.as_double());
    case boost::json::kind::bool_: return left.as_bool() == right.as_bool();
    case boost::json::kind::null: return true;
    }
    return false;
}
struct StructuredCompilation {
    CompiledContext context;
    std::string logical_bytes;
};

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
    Object identity{{"format", c.logical_input.at("format") == "wls-logical-input-v3"
            ? "wls-context-cache-v3" : "wls-context-cache-v2"},
        {"selection", selection(c)}, {"prefix_sha256", c.prefix_sha256}};
    if (const auto* policy = object(c.logical_input).if_contains("tool_policy")) identity["tool_policy"] = *policy;
    if (const auto* policy = object(c.logical_input).if_contains("model_projection")) identity["model_projection"] = *policy;
    return sha256_hex(canonical_json(identity));
}
void description(const Json& value) {
    require(value.is_string() && value.as_string().find_first_not_of(" \t\r\n") != std::string::npos,
            "v3 tool and parameter descriptions must be nonempty text");
    capacity(value.as_string().size() <= 1024, "v3 description exceeds 1024 bytes");
}
void describe_parameters(Json& schema, const std::string& key, bool project) {
    if (!schema.is_object()) return;
    auto& source = schema.as_object();
    if (auto* properties = source.if_contains("properties")) {
        require(properties->is_object(), "v3 properties must be an object");
        for (auto& property : properties->as_object()) {
            const auto path = key + "." + std::string(property.key());
            require(property.value().is_object(), "v3 parameter must have a described object schema");
            auto& body = property.value().as_object();
            if (body.contains("description")) description(body.at("description"));
            if (project) body["description"] = description_detail::lookup(path, body.if_contains("description"));
            require(body.contains("description"), "v3 parameter description missing: " + path);
            description(body.at("description"));
            describe_parameters(property.value(), path, project);
        }
    }
    for (const auto child : {"items", "additionalProperties", "contains", "not", "if", "then", "else"}) {
        if (auto* value = source.if_contains(child))
            describe_parameters(*value, key + (std::string_view(child) == "items" ? "[]" :
                std::string_view(child) == "additionalProperties" ? "{}" : ""), project);
    }
    for (const auto child : {"allOf", "anyOf", "oneOf"})
        if (auto* values = source.if_contains(child))
            for (auto& value : values->as_array()) describe_parameters(value, key, project);
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
        // The opaque original joins the same logical bytes, hashes and caps.
        if (!message.responses_output.is_null()) {
            require(message.role == "assistant", "only assistant messages carry opaque responses output");
            const auto projection = responses_compat::validate_payload(message.responses_output);
            responses_compat::require_projection_match(projection, message.content, message.tool_calls);
            result.back().as_object()["responses_output"] = message.responses_output;
        }
    }
    require(pending.empty(), "structured transcript ends with an unpaired tool call");
    return result;
}

// Separate semantic originals from live authority without editing either the
// complete host contract or any historical message/provider payload.
std::pair<Json, Json> contract_parts(const std::string& bytes) {
    if (!bytes.starts_with("{")) return {Object{{"task_contract", bytes}}, Object{}};
    auto state = parse_json_strict(bytes, 32U * 1024U * 1024U);
    if (!state.is_object() || !state.as_object().contains("request"))
        return {Object{{"task_contract", bytes}}, Object{}};
    Object stable, versions, original_bindings;
    auto& request = state.at("request").as_object();
    Object original_request;
    for (const auto* key : {"texts", "constraint_catalog"}) {
        original_request[key] = request.at(key); request.erase(key);
    }
    stable["request"] = std::move(original_request);
    versions["requirements_revision"] = request.at("requirements_revision");
    for (const auto* section : {"task", "instruction"}) {
        const auto* original = state.as_object().if_contains(section);
        const auto body = std::string_view(section) == "task" ? "brief" : "body";
        stable[section] = original && original->is_object() ? field(*original, body) : Json(nullptr);
        if (stable.at(section).is_object()) {
            Object binding;
            const auto identity = to_json(ExecutionBinding{});
            for (const auto& entry : identity.as_object())
                if (const auto* value = stable.at(section).as_object().if_contains(entry.key())) {
                    binding[entry.key()] = *value; stable.at(section).as_object().erase(entry.key());
                }
            original_bindings[section] = std::move(binding);
        }
        if (original && original->is_object()) {
            auto& metadata = state.at(section).as_object();
            for (const auto* key : {"task_revision", "instruction_revision"})
                if (const auto* revision = metadata.if_contains(key)) versions[key] = *revision;
            metadata.erase(body);
        }
    }
    state.as_object()["original_contract_bindings"] = std::move(original_bindings);
    state.as_object()["semantic_versions"] = std::move(versions);
    return {stable, state};
}
Json versions(const Json& state) {
    const auto* value = object(state).if_contains("semantic_versions");
    return value ? *value : Json(Object{});
}
Json dynamic_projection(const Json& input) {
    Object result;
    for (const auto& entry : object(input))
        if (entry.key() != "prefix" && entry.key() != "selection" && entry.key() != "format")
            result[entry.key()] = entry.value();
    return result;
}
std::string stable_projection(const Json& input) {
    return text(input, "format") == "wls-logical-input-v3" ? canonical_json(Object{
        {"prefix", field(input, "prefix")}, {"contract_anchor", field(input, "contract_anchor")}}) :
        canonical_json(field(input, "prefix"));
}
} // namespace

void continue_structured_contract(ContextInput& input, const Json& previous) {
    const auto [stable, state] = contract_parts(input.task_contract);
    input.contract_anchor = stable; input.contract_updates = Array{};
    if (previous.is_null()) return;
    exact(previous, {"attempt_id", "input"});
    const auto& prior = field(previous, "input");
    require(text(prior, "format") == "wls-logical-input-v3", "prior context is not verified v3");
    input.contract_anchor = field(prior, "contract_anchor");
    input.contract_updates = field(prior, "contract_updates");
    const auto& updates = array(input.contract_updates);
    const auto& last = updates.empty() ? input.contract_anchor : field(updates.back(), "contract");
    if (last != stable || versions(field(prior, "current_state")) != versions(state))
        input.contract_updates.as_array().push_back(Object{{"after_attempt_id", text(previous, "attempt_id")},
            {"contract", stable}, {"semantic_versions", versions(state)}});
}

void validate_structured_contract_continuity(const CompiledContext& context, const Json& previous) {
    ContextInput expected; expected.task_contract = compiled_task_contract(context);
    continue_structured_contract(expected, previous);
    require(field(context.logical_input, "contract_anchor") == expected.contract_anchor &&
        field(context.logical_input, "contract_updates") == expected.contract_updates,
        "v3 contract anchor or original update boundaries changed");
}

static StructuredCompilation compile_structured(const ContextInput& base,
        const std::vector<StructuredContextMessage>& messages, ContextLimits limits,
        StructuredContextFormat format, bool project, const CanonicalArtifacts* artifacts = nullptr) {
    require(base.messages.empty(), "legacy messages must be explicitly converted, never discarded");
    // Reuse the established validation of model selection, required sections,
    // tool definitions and dependency hashes. No adapter parses legacy frames.
    auto validation_base = base;
    validation_base.model_projection.clear();
    auto result = ContextCompiler(limits).compile(validation_base);
    auto tools = base.tools;
    std::sort(tools.begin(), tools.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    Array definitions;
    const bool v3 = format == StructuredContextFormat::V3;
    require(base.worker_history.is_null() || v3, "append-fold cannot change historical context formats");
    require(base.model_projection.empty() || (v3 && base.model_projection == "lean-v1"),
        "model projection requires explicit v3 lean-v1");
    require(base.tool_policy.empty() ? base.allowed_tools.empty() : v3 && base.tool_policy == "fixed-role-v1",
        "tool policy requires structured-v3 fixed-role-v1");
    if (!base.tool_policy.empty()) {
        const auto known = fixed_role_tool_names(base.role);
        for (const auto& tool : tools) require(std::find(known.begin(), known.end(), tool.name) != known.end(),
            "fixed role catalog contains an unknown or foreign role tool");
    }
    for (auto tool : tools) {
        if (v3) { description(Json(tool.description)); describe_parameters(tool.definition,
            tool.version == "wls-history-text-v1" ? "history_read_text" : tool.name, project); }
        definitions.push_back(Object{{"name", tool.name}, {"version", tool.version}, {"parameters", tool.definition}});
        if (v3) definitions.back().as_object()["description"] = tool.description;
    }
    auto dependencies = base.dependencies;
    std::sort(dependencies.begin(), dependencies.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    Array sources;
    for (const auto& dep : dependencies)
        sources.push_back(Object{{"name", dep.name}, {"version", dep.version}, {"content_sha256", dep.content_sha256}});
    Json prefix = Object{{"format", v3 ? "wls-logical-prefix-v3" : "wls-logical-prefix-v1"}, {"tools", definitions},
        {"system", Object{{"system_prompt", base.system_prompt}, {"role_prompt", base.role_prompt},
            {"function_prompt", base.function_prompt}, {"environment_card", base.environment_card},
            {"approved_rules", base.approved_rules}, {"project_snapshot", base.project_snapshot}}},
        {"dependencies", sources}};
    const auto encoded = encode_messages(messages, limits);
    result.logical_input = Object{{"format", v3 ? "wls-logical-input-v3" : "wls-logical-input-v1"}, {"selection", selection(result)},
        {"prefix", prefix}, {"task_contract", base.task_contract}, {"messages", encoded}};
    bool reuse = !v3 && artifacts && same_representation(result.logical_input, *artifacts->input);
    // V3 still checks its original intermediate encodings before projection.
    result.stable_prefix = reuse && artifacts->stable ? *artifacts->stable : canonical_json(prefix);
    result.dynamic_suffix = reuse && artifacts->dynamic ? *artifacts->dynamic :
        canonical_json(Object{{"task_contract", base.task_contract}, {"messages", encoded}});
    if (v3) {
        if (!base.worker_history.is_null()) {
            require(base.model_projection == "lean-v1" && base.role == Role::Worker,
                "append-fold projection needs Worker structured-v3 lean-v1");
            const auto checked = append_fold_history(Object{{"worker_history", base.worker_history}, {"messages", encoded}},
                Object{{"scope", base.worker_history.at("scope")}, {"through_sequence", base.worker_history.at("last_sequence")}, {"items", Array{}}});
            require(checked.at("messages") == encoded,
                "append-fold visible originals differ from compiled messages");
            result.logical_input.as_object()["worker_history"] = base.worker_history;
        }
        auto [stable, state] = contract_parts(base.task_contract);
        const auto anchor = base.contract_anchor.is_null() ? stable : base.contract_anchor;
        require(anchor.is_object(), "v3 original contract anchor is missing or invalid");
        const auto& updates = array(base.contract_updates);
        capacity(updates.size() + encoded.size() <= limits.max_messages, "v3 contract/history count exceeds limit");
        std::set<std::string> boundaries;
        for (const auto& update : updates) {
            exact(update, {"after_attempt_id", "contract", "semantic_versions"});
            require(identifier(text(update, "after_attempt_id")) && field(update, "contract").is_object() &&
                field(update, "semantic_versions").is_object() && boundaries.insert(text(update, "after_attempt_id")).second,
                "invalid or repeated v3 contract update boundary");
        }
        require((updates.empty() ? anchor : field(updates.back(), "contract")) == stable &&
            (updates.empty() || field(updates.back(), "semantic_versions") == versions(state)),
            "v3 latest contract projection differs from the complete host contract");
        Array windows, message_sources;
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            const auto& message = encoded[index];
            const auto& source = field(message, "source");
            if (!source.is_null()) {
                message_sources.push_back(Object{{"message_index", index}, {"source", source}});
                const auto* kind = source.as_object().if_contains("kind");
                if (kind && (*kind == "history_window" || *kind == "management_history_window"))
                    windows.push_back(message);
            }
        }
        state.as_object()["history_windows"] = windows;
        state.as_object()["message_sources"] = message_sources;
        auto& prefix_sources = prefix.at("dependencies").as_array();
        for (auto dep = prefix_sources.begin(); dep != prefix_sources.end(); ++dep)
            if (field(*dep, "name") == "worker_execution_policy") {
                state.as_object()["worker_execution_policy"] = *dep;
                auto environment = parse_json_strict(text(prefix.at("system"), "environment_card"));
                auto& execution = environment.at("execution").as_object();
                require(execution.at("execution_policy_sha256") == field(*dep, "content_sha256"),
                    "execution policy environment and dependency disagree");
                execution.erase("execution_policy_sha256");
                prefix.at("system").at("environment_card") = canonical_json(environment);
                prefix_sources.erase(dep); break;
            }
        result.logical_input.at("prefix") = prefix;
        result.logical_input.as_object()["contract_anchor"] = anchor;
        result.logical_input.as_object()["contract_updates"] = updates;
        result.logical_input.as_object()["current_state"] = state;
        if (!base.model_projection.empty()) result.logical_input.as_object()["model_projection"] = base.model_projection;
        if (!base.tool_policy.empty()) {
            result.logical_input.as_object()["tool_policy"] = base.tool_policy;
            Array allowed;
            for (const auto& name : base.allowed_tools) allowed.emplace_back(name);
            result.logical_input.at("current_state").as_object()["allowed_tools"] = allowed;
            (void)model_allowed_tools(result);
        }
        reuse = artifacts && same_representation(result.logical_input, *artifacts->input);
        result.stable_prefix = reuse && artifacts->stable ? *artifacts->stable : stable_projection(result.logical_input);
        result.dynamic_suffix = reuse && artifacts->dynamic ? *artifacts->dynamic :
            canonical_json(dynamic_projection(result.logical_input));
    }
    capacity(result.stable_prefix.size() <= limits.max_prefix_bytes,
             "structured context byte limit exceeded; no authoritative content was truncated");
    auto logical_bytes = reuse && artifacts->logical ? *artifacts->logical : result.prompt();
    capacity(logical_bytes.size() <= limits.max_total_bytes,
             "structured context byte limit exceeded; no authoritative content was truncated");
    result.prefix_sha256 = sha256_hex(result.stable_prefix);
    result.cache_identity = cache_hash(result);
    result.logical_input_sha256 = sha256_hex(logical_bytes);
    return {std::move(result), std::move(logical_bytes)};
}

CompiledContext compile_structured_context(const ContextInput& base,
        const std::vector<StructuredContextMessage>& messages, ContextLimits limits, StructuredContextFormat format) {
    return compile_structured(base, messages, limits, format, true).context;
}

static Json assemble_structured_json(const CompiledContext& c) {
    Object result = selection(c).as_object();
    result["format"] = text(c.logical_input, "format") == "wls-logical-input-v3"
        ? "wls-compiled-context-v3" : "wls-compiled-context-v2";
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

static Json serialize_structured(const CompiledContext& c, CanonicalArtifacts& artifacts) {
    require(c.structured(), "expected a structured context");
    require(field(c.logical_input, "selection") == selection(c), "logical selection differs from context metadata");
    const bool v3 = text(c.logical_input, "format") == "wls-logical-input-v3";
    (void)model_allowed_tools(c);
    artifacts.stable = stable_projection(c.logical_input);
    require(c.stable_prefix == *artifacts.stable,
            "structured context projections differ from original input");
    artifacts.dynamic = canonical_json(v3 ? dynamic_projection(c.logical_input) : Json(Object{
        {"task_contract", field(c.logical_input, "task_contract")}, {"messages", field(c.logical_input, "messages")}}));
    require(c.dynamic_suffix == *artifacts.dynamic,
            "structured context projections differ from original input");
    if (v3) {
        for (auto tool : array(field(field(c.logical_input, "prefix"), "tools"))) {
            description(field(tool, "description"));
            describe_parameters(tool.at("parameters"), text(tool, "name"), false);
        }
        capacity(c.stable_prefix.size() <= ContextLimits{}.max_prefix_bytes, "v3 serialization exceeds byte limit");
        artifacts.logical = c.prompt();
        capacity(artifacts.logical->size() <= ContextLimits{}.max_total_bytes, "v3 serialization exceeds byte limit");
    }
    return assemble_structured_json(c);
}

Json structured_context_to_json(const CompiledContext& c) {
    CanonicalArtifacts artifacts{&c.logical_input, {}, {}, {}};
    return serialize_structured(c, artifacts);
}

static context_detail::ContextRestoreResult restore_structured(const Json& value, CanonicalArtifacts* source) {
    exact(value, {"format", "role", "model_ref", "reasoning_mode", "reasoning_effort",
        "adapter_version", "protocol_version", "input", "prefix_sha256", "cache_identity",
        "logical_input_sha256", "wire_sha256", "tool_names"});
    const bool v3 = text(value, "format") == "wls-compiled-context-v3";
    require((v3 || text(value, "format") == "wls-compiled-context-v2") && field(value, "wire_sha256").is_null(),
            "unsupported structured context or fabricated wire fingerprint");
    const auto& input = field(value, "input");
    auto input_keys = std::set<std::string>{"format", "selection", "prefix", "task_contract", "messages"};
    if (v3) for (const auto* key : {"contract_anchor", "contract_updates", "current_state"}) input_keys.insert(key);
    if (v3 && object(input).contains("model_projection")) input_keys.insert("model_projection");
    if (v3 && object(input).contains("worker_history")) input_keys.insert("worker_history");
    if (object(input).contains("tool_policy")) input_keys.insert("tool_policy");
    exact(input, input_keys);
    require(text(input, "format") == (v3 ? "wls-logical-input-v3" : "wls-logical-input-v1"), "unsupported logical input format");
    // Bound canonical logical bytes before reconstructing a compiled context.
    const ContextLimits limits;
    CanonicalArtifacts local{&input, {}, {}, {}};
    if (!source || !same_representation(input, *source->input)) source = &local;
    if (!source->logical) source->logical = canonical_json(input);
    capacity(source->logical->size() <= limits.max_total_bytes, "structured restore exceeds byte limit");
    const auto& prefix = field(input, "prefix");
    exact(prefix, {"format", "tools", "system", "dependencies"});
    require(text(prefix, "format") == (v3 ? "wls-logical-prefix-v3" : "wls-logical-prefix-v1"), "unsupported logical prefix format");
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
    if (object(input).contains("model_projection")) base.model_projection = text(input, "model_projection");
    if (v3) {
        if (object(input).contains("worker_history")) base.worker_history = field(input, "worker_history");
        if (object(input).contains("tool_policy")) {
            base.tool_policy = text(input, "tool_policy");
            CompiledContext declared; declared.logical_input = input;
            for (const auto& name : array(field(value, "tool_names"))) {
                require(name.is_string(), "compiled tool name must be text");
                declared.tool_names.emplace_back(name.as_string());
            }
            base.allowed_tools = model_allowed_tools(declared);
        }
        base.contract_anchor = field(input, "contract_anchor");
        require(!base.contract_anchor.is_null(), "v3 original contract anchor is missing");
        base.contract_updates = field(input, "contract_updates");
        if (const auto* dep = object(field(input, "current_state")).if_contains("worker_execution_policy")) {
            exact(*dep, {"name", "version", "content_sha256"});
            require(text(*dep, "name") == "worker_execution_policy", "invalid v3 execution dependency");
            base.dependencies.push_back({text(*dep, "name"), text(*dep, "version"), text(*dep, "content_sha256")});
            auto environment = parse_json_strict(base.environment_card);
            environment.at("execution").as_object()["execution_policy_sha256"] = field(*dep, "content_sha256");
            base.environment_card = canonical_json(environment);
        }
    }
    capacity(array(field(prefix, "tools")).size() <= limits.max_tools &&
             array(field(prefix, "dependencies")).size() <= limits.max_dependencies,
             "structured restore exceeds item capacity");
    for (const auto& tool : array(field(prefix, "tools"))) {
        auto keys = std::set<std::string>{"name", "version", "parameters"};
        if (v3) keys.insert("description");
        exact(tool, keys);
        base.tools.push_back({text(tool, "name"), text(tool, "version"), field(tool, "parameters")});
        if (v3) base.tools.back().description = text(tool, "description");
    }
    for (const auto& dep : array(field(prefix, "dependencies"))) {
        exact(dep, {"name", "version", "content_sha256"});
        base.dependencies.push_back({text(dep, "name"), text(dep, "version"), text(dep, "content_sha256")});
    }
    std::vector<StructuredContextMessage> messages;
    capacity(array(field(input, "messages")).size() <= limits.max_messages, "structured restore exceeds message capacity");
    for (const auto& item : array(field(input, "messages"))) {
        auto expected = std::set<std::string>{"role", "content", "tool_calls", "tool_call_id", "source"};
        if (item.as_object().contains("responses_output")) expected.insert("responses_output");
        exact(item, expected);
        StructuredContextMessage message;
        message.role = text(item, "role"); message.content = text(item, "content");
        message.tool_call_id = text(item, "tool_call_id"); message.source = field(item, "source");
        capacity(array(field(item, "tool_calls")).size() <= 32, "structured restore exceeds tool turn capacity");
        for (const auto& call : array(field(item, "tool_calls"))) {
            exact(call, {"call_id", "name", "arguments"});
            message.tool_calls.push_back({text(call, "call_id"), text(call, "name"), field(call, "arguments")});
        }
        if (item.as_object().contains("responses_output"))
            message.responses_output = field(item, "responses_output");
        messages.push_back(std::move(message));
    }
    auto result = compile_structured(base, messages, limits,
        v3 ? StructuredContextFormat::V3 : StructuredContextFormat::V2, false, source);
    // Compilation has just checked selection, tools, native turns, projections,
    // descriptions and the same default byte limits. Assemble its own result;
    // source JSON equality does not establish identical canonical number bytes.
    if (v3) capacity(result.context.stable_prefix.size() <= ContextLimits{}.max_prefix_bytes &&
        result.logical_bytes.size() <= ContextLimits{}.max_total_bytes, "v3 serialization exceeds byte limit");
    auto rebuilt_json = assemble_structured_json(result.context);
    require(rebuilt_json == value,
            "structured context differs from its canonical structure, metadata or fingerprints");
    return {std::move(result.context), std::move(rebuilt_json)};
}

CompiledContext structured_context_from_json(const Json& value) {
    return restore_structured(value, nullptr).context;
}

context_detail::ContextRestoreResult context_detail::restore_structured_with_json(const Json& value) {
    return restore_structured(value, nullptr);
}

Json context_detail::validate_structured_to_json(const CompiledContext& context) {
    CanonicalArtifacts artifacts{&context.logical_input, {}, {}, {}};
    auto encoded = serialize_structured(context, artifacts);
    (void)restore_structured(encoded, &artifacts);
    // Mutable serialization keeps the original representation after checking it.
    return encoded;
}
} // namespace wls::s1
