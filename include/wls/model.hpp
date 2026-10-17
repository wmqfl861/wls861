#pragma once

#include "wls/context.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace wls::s1 {

struct ModelUsage {
    std::optional<std::int64_t> input_tokens;
    std::optional<std::int64_t> output_tokens;
    std::optional<std::int64_t> cache_read_tokens;
    std::optional<std::int64_t> cache_write_tokens;
    std::optional<std::int64_t> cost_micro_usd;
    bool operator==(const ModelUsage&) const = default;
};

struct ModelToolCall {
    std::string call_id;
    std::string name;
    Json arguments;
};

enum class ModelResultStatus { Completed, Error, Interrupted };

struct ModelRequest {
    std::string call_id;
    std::string attempt_id;
    std::int64_t role_call_index{1};
    Role role{Role::Worker};
    ExecutionBinding binding;
    std::string config_snapshot_id;
    ResolvedModel model;
    CompiledContext context;
    std::int64_t max_output_tokens{4096};
    std::int64_t reserved_input_tokens{};
    std::int64_t reserved_output_tokens{};
    std::int64_t reserved_cost_micro_usd{};
    std::chrono::milliseconds timeout{std::chrono::seconds(30)};
};

struct ModelResponse {
    std::string call_id;
    std::string attempt_id;
    std::int64_t role_call_index{};
    Role role{Role::Worker};
    ExecutionBinding binding;
    std::string config_snapshot_id;
    ModelResultStatus status{ModelResultStatus::Completed};
    std::string text;
    std::vector<ModelToolCall> tool_calls;
    ModelUsage usage;
    std::string error_code;
    std::string error_message;
};

struct AdapterCapabilities {
    std::string adapter;
    std::string version;
    bool real_calls{false};
    bool tool_calls{true};
    bool streaming{false};
    bool reports_usage{false};
};

// A bounded blocking step, suitable for an existing Scheduler callback. No
// method creates a thread. The host owns scheduling, reservations, deadlines,
// persistence and the current-version check before dispatching a tool.
class ModelAdapter {
public:
    virtual ~ModelAdapter() = default;
    virtual AdapterCapabilities capabilities() const = 0;
    virtual void validate(const ModelRequest& request) const = 0;
    virtual ModelResponse complete(ModelRequest request,
                                   std::stop_token stop = {}) const = 0;
};

struct FakeModelStep {
    Role role{Role::Worker};
    std::int64_t role_call_index{1};
    ModelResultStatus status{ModelResultStatus::Completed};
    std::string text;
    std::vector<ModelToolCall> tool_calls;
    ModelUsage usage;
    std::string error_code;
    std::string error_message;
    std::chrono::milliseconds delay{};
    // Exercises a transport that delivers after cancellation. The returned
    // response still carries the original tuple; the Store must reject it.
    bool deliver_after_cancel{false};
};

struct FakeModelLimits {
    std::size_t max_steps{512};
    std::size_t max_response_bytes{256U * 1024U};
    std::size_t max_tool_calls{32};
    std::chrono::milliseconds max_delay{std::chrono::seconds(30)};
};

class FakeModel final : public ModelAdapter {
public:
    explicit FakeModel(std::vector<FakeModelStep> steps,
                       std::string model_ref = "fake",
                       FakeModelLimits limits = {});
    AdapterCapabilities capabilities() const override;
    void validate(const ModelRequest& request) const override;
    ModelResponse complete(ModelRequest request,
                           std::stop_token stop = {}) const override;
private:
    std::map<std::pair<Role, std::int64_t>, FakeModelStep> steps_;
    std::string model_ref_;
    FakeModelLimits limits_;
};

// This checks the captured call envelope, not current state or permission.
// Even a valid complete response must pass the Store's current tuple gate.
void validate_response(const ModelRequest& request, const ModelResponse& response);
void require_complete_response(const ModelRequest& request,
                               const ModelResponse& response);

Json to_json(const ModelUsage& usage);
Json to_json(const ModelRequest& request);
Json to_json(const ModelResponse& response);
ModelUsage model_usage_from_json(const Json& value);
ModelRequest model_request_from_json(const Json& value);
ModelResponse model_response_from_json(const Json& value);
std::vector<FakeModelStep> fake_model_steps_from_json(const Json& value);

} // namespace wls::s1
