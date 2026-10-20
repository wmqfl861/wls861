#pragma once

#include "wls/config_loader.hpp"
#include "wls/model.hpp"
#include "wls/schema.hpp"
#include "wls/store.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace wls::s1 {

class WorkerTools;

// Host-held value: only reserve_call constructs this from authenticated Store
// actors and persisted records. Never deserialize it from model arguments.
struct CapturedModelCall {
    ModelRequest request;
    StoreActor actor;
    std::int64_t expected_state_version{};
    // A single host-selected question/submission target. The identical value
    // is inside the durable request's task_contract; model arguments never
    // choose an identity, CAS value, version, or a replacement target.
    Json workflow_action = nullptr;
};

// A deliberately bounded first S1 runner. It connects real configuration,
// Schema, SQLite accounting and version gates to FakeModel. It supports one
// independent task. An explicit WorkerTools host adds bounded real tools; the
// default preserves the note-only path. Neither path claims business delivery,
// a daemon, a complete event-driven role loop, or supplier integration.
class OfflineRuntime final {
public:
    OfflineRuntime(Store& store, const SchemaRegistry& schemas,
                   WorkerTools* worker_tools = nullptr);
    CapturedModelCall reserve_call(const ModelAdapter& adapter,
                                  const std::string& request_id, Role role,
                                  const std::string& task_id = {},
                                  std::int64_t role_call_index = 1,
                                  const std::vector<Json>& tool_feedback = {});
    // Record usage and the raw response first. Invalid/stale responses never
    // authorize a tool; their consumed or unknown budget is still retained.
    Json accept_response(const CapturedModelCall& call,
                         const ModelResponse& response,
                         const std::string& amendment_disposition = {});
    // Used only by the durable event host. The Dispatcher can atomically
    // allocate independent tasks or publish a plan with explicit per-node
    // AMEND decisions. Omitted existing tasks stay blocked. Other roles keep
    // the same captured-envelope and final Store guards as accept_response.
    Json accept_independent_response(const CapturedModelCall& call,
                                     const ModelResponse& response,
                                     const Json& amendment_dispositions = boost::json::object{});
    // At most one Dispatcher, one TeamLead and 1..16 explicit Worker calls.
    // Pending questions/submissions require the durable event pump; this
    // compatibility entry never replaces them with an automatic work order.
    // No implicit retries or new threads. The caller owns its Store lease.
    Json run(const std::string& request_id,
             const std::vector<FakeModelStep>& steps,
             const std::string& amendment_disposition = {},
             std::int64_t max_worker_calls = 1);
private:
    Json accept_captured_response(const CapturedModelCall& call,
                                  const ModelResponse& response,
                                  const std::string& amendment_disposition,
                                  bool independent_plan,
                                  const Json& amendment_dispositions);
    Store& store_;
    const SchemaRegistry& schemas_;
    WorkerTools* worker_tools_;
};

// Bounded regular-file read for explicit CLI inputs and trusted SQL assets.
// No tilde/env expansion, command execution, symlink or special-file reads.
std::string read_runtime_file(const std::filesystem::path& path,
                              std::size_t max_bytes = 1048576);
FrozenRequestConfig restore_request_config(const Json& request_record);

} // namespace wls::s1
