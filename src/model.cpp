#include "wls/model.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

namespace wls::s1 {
namespace {

void require(bool valid, const std::string& message) {
    if (!valid) throw Error(ErrorCode::InvalidInput, message);
}

bool valid_sha256(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

const boost::json::object& object(const Json& value, std::string_view description) {
    require(value.is_object(), std::string(description) + " must be an object");
    return value.as_object();
}

const Json& field(const boost::json::object& source, std::string_view name) {
    const auto* found = source.if_contains(name);
    require(found != nullptr, "missing model field: " + std::string(name));
    return *found;
}

std::string string_field(const boost::json::object& source, std::string_view name) {
    const auto& value = field(source, name);
    require(value.is_string(), "invalid model string: " + std::string(name));
    return std::string(value.as_string());
}

std::int64_t integer(const Json& value, std::string_view name) {
    if (value.is_int64()) return value.as_int64();
    if (value.is_uint64() &&
        value.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(value.as_uint64());
    }
    throw Error(ErrorCode::InvalidInput, "invalid model integer: " + std::string(name));
}

std::int64_t integer_field(const boost::json::object& source, std::string_view name) {
    return integer(field(source, name), name);
}

void exact_fields(const boost::json::object& source,
                  const std::set<std::string>& expected) {
    require(source.size() == expected.size(), "missing or extra model fields");
    for (const auto& entry : source) {
        require(expected.contains(std::string(entry.key())),
                "unknown model field: " + std::string(entry.key()));
    }
}

std::string status_text(ModelResultStatus status) {
    switch (status) {
    case ModelResultStatus::Completed: return "completed";
    case ModelResultStatus::Error: return "error";
    case ModelResultStatus::Interrupted: return "interrupted";
    }
    throw Error(ErrorCode::InvalidInput, "unknown model response status");
}

ModelResultStatus parse_status(const std::string& value) {
    if (value == "completed") return ModelResultStatus::Completed;
    if (value == "error") return ModelResultStatus::Error;
    if (value == "interrupted") return ModelResultStatus::Interrupted;
    throw Error(ErrorCode::InvalidInput, "unknown model response status");
}

void validate_usage(const ModelUsage& usage) {
    for (const auto* amount : {&usage.input_tokens, &usage.output_tokens,
                              &usage.cache_read_tokens, &usage.cache_write_tokens,
                              &usage.cost_micro_usd}) {
        require(!amount->has_value() || **amount >= 0, "model usage cannot be negative");
    }
}

Json nullable(const std::optional<std::int64_t>& value) {
    return value ? Json(*value) : Json(nullptr);
}

void validate_tools(const std::vector<ModelToolCall>& calls) {
    std::set<std::string> ids;
    for (const auto& call : calls) {
        require(!call.call_id.empty() && !call.name.empty() && call.arguments.is_object(),
                "model tool call needs an ID, name and object arguments");
        require(ids.insert(call.call_id).second, "duplicate model tool call ID");
    }
}

boost::json::array tools_json(const std::vector<ModelToolCall>& calls) {
    boost::json::array values;
    for (const auto& call : calls) {
        values.emplace_back(boost::json::object{
            {"call_id", call.call_id}, {"name", call.name}, {"arguments", call.arguments}});
    }
    return values;
}

std::vector<ModelToolCall> parse_tools(const Json& value) {
    require(value.is_array(), "model tool_calls must be an array");
    std::vector<ModelToolCall> calls;
    for (const auto& item : value.as_array()) {
        const auto& source = object(item, "model tool call");
        exact_fields(source, {"call_id", "name", "arguments"});
        calls.push_back({string_field(source, "call_id"), string_field(source, "name"),
                         field(source, "arguments")});
    }
    validate_tools(calls);
    return calls;
}

void validate_envelope(const ModelRequest& request) {
    require(!request.call_id.empty() && !request.attempt_id.empty() &&
            request.call_id != request.attempt_id && request.role_call_index > 0,
            "model call and attempt need distinct IDs and a positive role sequence");
    require(valid_sha256(request.config_snapshot_id), "model call needs a frozen configuration hash");
    (void)execution_binding_from_json(to_json(request.binding));
    require(request.max_output_tokens > 0 && request.reserved_input_tokens > 0 &&
            request.reserved_output_tokens >= request.max_output_tokens &&
            request.reserved_cost_micro_usd >= 0, "model call needs a sufficient explicit reservation");
    require(request.timeout.count() > 0, "model timeout must be positive");
    require(request.context.role == request.role && request.context.model == request.model,
            "model request does not match its compiled role/model");
    // Verify actual prefix bytes and all non-text cache parameters, including
    // when this request was restored from a persistent frozen snapshot.
    (void)compiled_context_from_json(to_json(request.context));
}

void validate_response_shape(const ModelResponse& response) {
    require(!response.call_id.empty() && !response.attempt_id.empty() &&
            response.call_id != response.attempt_id && response.role_call_index > 0,
            "invalid model response identity");
    require(valid_sha256(response.config_snapshot_id), "invalid response configuration hash");
    (void)execution_binding_from_json(to_json(response.binding));
    (void)status_text(response.status);
    validate_usage(response.usage);
    validate_tools(response.tool_calls);
    if (response.status == ModelResultStatus::Completed) {
        require(response.error_code.empty() && response.error_message.empty(),
                "a completed model response cannot carry an error");
        require(!response.text.empty() || !response.tool_calls.empty(),
                "a completed model response cannot be empty");
    } else {
        require(!response.error_code.empty(), "failed model responses require an error code");
    }
}

ModelResponse response_for(const ModelRequest& request) {
    ModelResponse response;
    response.call_id = request.call_id;
    response.attempt_id = request.attempt_id;
    response.role_call_index = request.role_call_index;
    response.role = request.role;
    response.binding = request.binding;
    response.config_snapshot_id = request.config_snapshot_id;
    return response;
}

} // namespace

FakeModel::FakeModel(std::vector<FakeModelStep> steps, std::string model_ref,
                     FakeModelLimits limits)
    : model_ref_(std::move(model_ref)), limits_(limits) {
    require(!model_ref_.empty() && limits_.max_steps > 0 && limits_.max_response_bytes > 0 &&
            limits_.max_tool_calls > 0 && limits_.max_delay.count() > 0 &&
            limits_.max_delay <= std::chrono::seconds(60), "invalid FakeModel limits");
    require(!steps.empty() && steps.size() <= limits_.max_steps, "invalid FakeModel script size");
    for (auto& step : steps) {
        require(step.role_call_index > 0 && step.delay.count() >= 0 &&
                step.delay <= limits_.max_delay, "invalid FakeModel sequence or delay");
        (void)role_from_string(role_name(step.role));
        validate_usage(step.usage);
        validate_tools(step.tool_calls);
        require(step.tool_calls.size() <= limits_.max_tool_calls, "FakeModel tool call limit exceeded");
        const auto payload_bytes = canonical_json(boost::json::object{
            {"text", step.text}, {"tool_calls", tools_json(step.tool_calls)},
            {"usage", to_json(step.usage)}, {"error_code", step.error_code},
            {"error_message", step.error_message}}).size();
        require(payload_bytes <= limits_.max_response_bytes, "FakeModel response byte limit exceeded");
        if (step.status == ModelResultStatus::Completed) {
            require(step.error_code.empty() && step.error_message.empty() &&
                    (!step.text.empty() || !step.tool_calls.empty()), "invalid complete FakeModel step");
        } else {
            (void)status_text(step.status);
            require(!step.error_code.empty(), "FakeModel failure needs an error code");
        }
        const auto key = std::make_pair(step.role, step.role_call_index);
        require(steps_.emplace(key, std::move(step)).second, "duplicate FakeModel role sequence");
    }
}

AdapterCapabilities FakeModel::capabilities() const {
    return {"fake", "1", false, true, false, false};
}

void FakeModel::validate(const ModelRequest& request) const {
    validate_envelope(request);
    require(request.model.model == model_ref_, "FakeModel cannot execute a different model reference");
    require(request.context.adapter_version == "1" &&
            request.context.protocol_version == "fake-v1",
            "FakeModel requires its frozen adapter/protocol version");
    require(request.timeout <= limits_.max_delay, "FakeModel timeout exceeds its bounded limit");
    const auto found = steps_.find({request.role, request.role_call_index});
    require(found != steps_.end(), "FakeModel has no scripted step for this role sequence");
}

ModelResponse FakeModel::complete(ModelRequest request, std::stop_token stop) const {
    validate(request);
    const auto& step = steps_.at({request.role, request.role_call_index});
    auto response = response_for(request);
    const auto wait_duration = std::min(step.delay, request.timeout);
    if (wait_duration.count() > 0 && (!stop.stop_requested() || step.deliver_after_cancel)) {
        std::mutex mutex;
        std::unique_lock lock(mutex);
        std::condition_variable_any changed;
        if (step.deliver_after_cancel) {
            changed.wait_for(lock, wait_duration, [] { return false; });
        } else {
            (void)changed.wait_for(lock, stop, wait_duration, [] { return false; });
        }
    }
    if (stop.stop_requested() && !step.deliver_after_cancel) {
        response.status = ModelResultStatus::Interrupted;
        response.error_code = "cancelled";
        response.error_message = "host cancellation requested; cost and usage remain unknown";
    } else if (step.delay > request.timeout) {
        response.status = ModelResultStatus::Interrupted;
        response.error_code = "timeout";
        response.error_message = "FakeModel deadline exceeded; cost and usage remain unknown";
    } else {
        response.status = step.status;
        response.text = step.text;
        response.tool_calls = step.tool_calls;
        response.usage = step.usage;
        response.error_code = step.error_code;
        response.error_message = step.error_message;
    }
    // Keep even an unavailable tool name in the returned raw response so the
    // host can durably account for the attempt before rejecting model output.
    validate_response_shape(response);
    return response;
}

void validate_response(const ModelRequest& request, const ModelResponse& response) {
    validate_envelope(request);
    validate_response_shape(response);
    if (request.call_id != response.call_id || request.attempt_id != response.attempt_id ||
        request.role_call_index != response.role_call_index || request.role != response.role ||
        request.binding != response.binding || request.config_snapshot_id != response.config_snapshot_id) {
        throw Error(ErrorCode::Stale, "model response does not match its original frozen call");
    }
    for (const auto& call : response.tool_calls) {
        require(std::find(request.context.tool_names.begin(), request.context.tool_names.end(),
                          call.name) != request.context.tool_names.end(),
                "model response contains an unavailable tool");
    }
}

void require_complete_response(const ModelRequest& request, const ModelResponse& response) {
    validate_response(request, response);
    if (response.status != ModelResultStatus::Completed) {
        throw Error(ErrorCode::NotReady, "incomplete model responses cannot dispatch tools or commit decisions");
    }
}

Json to_json(const ModelUsage& usage) {
    validate_usage(usage);
    return boost::json::object{
        {"input_tokens", nullable(usage.input_tokens)},
        {"output_tokens", nullable(usage.output_tokens)},
        {"cache_read_tokens", nullable(usage.cache_read_tokens)},
        {"cache_write_tokens", nullable(usage.cache_write_tokens)},
        {"cost_micro_usd", nullable(usage.cost_micro_usd)}};
}

ModelUsage model_usage_from_json(const Json& value) {
    const auto& source = object(value, "model usage");
    const std::set<std::string> names{
        "input_tokens", "output_tokens", "cache_read_tokens", "cache_write_tokens", "cost_micro_usd"};
    for (const auto& entry : source) {
        require(names.contains(std::string(entry.key())), "unknown model usage field");
    }
    const auto amount = [&source](std::string_view name) -> std::optional<std::int64_t> {
        const auto* found = source.if_contains(name);
        if (found == nullptr || found->is_null()) return std::nullopt;
        const auto result = integer(*found, name);
        require(result >= 0, "model usage cannot be negative");
        return result;
    };
    return {amount("input_tokens"), amount("output_tokens"), amount("cache_read_tokens"),
            amount("cache_write_tokens"), amount("cost_micro_usd")};
}

Json to_json(const ModelRequest& request) {
    validate_envelope(request);
    return boost::json::object{
        {"format", "wls-model-request-v1"}, {"call_id", request.call_id},
        {"attempt_id", request.attempt_id}, {"role_call_index", request.role_call_index},
        {"role", role_name(request.role)}, {"binding", to_json(request.binding)},
        {"config_snapshot_id", request.config_snapshot_id}, {"context", to_json(request.context)},
        {"max_output_tokens", request.max_output_tokens},
        {"reserved_input_tokens", request.reserved_input_tokens},
        {"reserved_output_tokens", request.reserved_output_tokens},
        {"reserved_cost_micro_usd", request.reserved_cost_micro_usd},
        {"timeout_ms", request.timeout.count()}};
}

ModelRequest model_request_from_json(const Json& value) {
    const auto& source = object(value, "model request");
    exact_fields(source, {"format", "call_id", "attempt_id", "role_call_index", "role", "binding",
        "config_snapshot_id", "context", "max_output_tokens", "reserved_input_tokens",
        "reserved_output_tokens", "reserved_cost_micro_usd", "timeout_ms"});
    require(string_field(source, "format") == "wls-model-request-v1", "unsupported model request format");
    ModelRequest request;
    request.call_id = string_field(source, "call_id");
    request.attempt_id = string_field(source, "attempt_id");
    request.role_call_index = integer_field(source, "role_call_index");
    request.role = role_from_string(string_field(source, "role"));
    request.binding = execution_binding_from_json(field(source, "binding"));
    request.config_snapshot_id = string_field(source, "config_snapshot_id");
    request.context = compiled_context_from_json(field(source, "context"));
    request.model = request.context.model;
    request.max_output_tokens = integer_field(source, "max_output_tokens");
    request.reserved_input_tokens = integer_field(source, "reserved_input_tokens");
    request.reserved_output_tokens = integer_field(source, "reserved_output_tokens");
    request.reserved_cost_micro_usd = integer_field(source, "reserved_cost_micro_usd");
    request.timeout = std::chrono::milliseconds(integer_field(source, "timeout_ms"));
    validate_envelope(request);
    return request;
}

Json to_json(const ModelResponse& response) {
    validate_response_shape(response);
    return boost::json::object{
        {"format", "wls-model-response-v1"}, {"call_id", response.call_id},
        {"attempt_id", response.attempt_id}, {"role_call_index", response.role_call_index},
        {"role", role_name(response.role)}, {"binding", to_json(response.binding)},
        {"config_snapshot_id", response.config_snapshot_id},
        {"status", status_text(response.status)}, {"text", response.text},
        {"tool_calls", tools_json(response.tool_calls)}, {"usage", to_json(response.usage)},
        {"error_code", response.error_code}, {"error_message", response.error_message}};
}

ModelResponse model_response_from_json(const Json& value) {
    const auto& source = object(value, "model response");
    exact_fields(source, {"format", "call_id", "attempt_id", "role_call_index", "role", "binding",
        "config_snapshot_id", "status", "text", "tool_calls", "usage", "error_code", "error_message"});
    require(string_field(source, "format") == "wls-model-response-v1", "unsupported model response format");
    ModelResponse response;
    response.call_id = string_field(source, "call_id");
    response.attempt_id = string_field(source, "attempt_id");
    response.role_call_index = integer_field(source, "role_call_index");
    response.role = role_from_string(string_field(source, "role"));
    response.binding = execution_binding_from_json(field(source, "binding"));
    response.config_snapshot_id = string_field(source, "config_snapshot_id");
    response.status = parse_status(string_field(source, "status"));
    response.text = string_field(source, "text");
    response.tool_calls = parse_tools(field(source, "tool_calls"));
    response.usage = model_usage_from_json(field(source, "usage"));
    response.error_code = string_field(source, "error_code");
    response.error_message = string_field(source, "error_message");
    validate_response_shape(response);
    return response;
}

std::vector<FakeModelStep> fake_model_steps_from_json(const Json& value) {
    require(value.is_array(), "FakeModel script must be an array");
    std::vector<FakeModelStep> result;
    const std::set<std::string> allowed{
        "role", "role_call_index", "status", "text", "tool_calls", "usage",
        "error_code", "error_message", "delay_ms", "deliver_after_cancel"};
    for (const auto& item : value.as_array()) {
        const auto& source = object(item, "FakeModel step");
        for (const auto& entry : source) {
            require(allowed.contains(std::string(entry.key())), "unknown FakeModel script field");
        }
        FakeModelStep step;
        step.role = role_from_string(string_field(source, "role"));
        step.role_call_index = integer_field(source, "role_call_index");
        step.status = parse_status(string_field(source, "status"));
        if (source.contains("text")) step.text = string_field(source, "text");
        if (source.contains("tool_calls")) step.tool_calls = parse_tools(field(source, "tool_calls"));
        if (source.contains("usage")) step.usage = model_usage_from_json(field(source, "usage"));
        if (source.contains("error_code")) step.error_code = string_field(source, "error_code");
        if (source.contains("error_message")) step.error_message = string_field(source, "error_message");
        if (source.contains("delay_ms")) step.delay = std::chrono::milliseconds(integer_field(source, "delay_ms"));
        if (source.contains("deliver_after_cancel")) {
            const auto& delayed = field(source, "deliver_after_cancel");
            require(delayed.is_bool(), "FakeModel deliver_after_cancel must be boolean");
            step.deliver_after_cancel = delayed.as_bool();
        }
        result.push_back(std::move(step));
    }
    // Constructor checks sequence uniqueness, response completeness and bounds.
    (void)FakeModel(result);
    return result;
}

} // namespace wls::s1
