#pragma once

#include "wls/config_loader.hpp"
#include "wls/model.hpp"
#include "wls/schema.hpp"
#include "wls/store.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace wls::s1 {

// Host-held value: only reserve_call constructs this from authenticated Store
// actors and persisted records. Never deserialize it from model arguments.
struct CapturedModelCall {
    ModelRequest request;
    StoreActor actor;
    std::int64_t expected_state_version{};
};

// A deliberately bounded first S1 runner. It connects real configuration,
// Schema, SQLite accounting and version gates to FakeModel. It supports one
// independent task and a durable note operation; it does not claim delivery,
// file/process execution, sandbox checks, a daemon, or supplier integration.
class OfflineRuntime final {
public:
    OfflineRuntime(Store& store, const SchemaRegistry& schemas);
    CapturedModelCall reserve_call(const ModelAdapter& adapter,
                                  const std::string& request_id, Role role,
                                  const std::string& task_id = {},
                                  std::int64_t role_call_index = 1);
    // Record usage and the raw response first. Invalid/stale responses never
    // authorize a tool; their consumed or unknown budget is still retained.
    Json accept_response(const CapturedModelCall& call,
                         const ModelResponse& response,
                         const std::string& amendment_disposition = {});
    // Each explicit invocation performs at most three fake calls, with no
    // implicit retries and no new threads. The caller owns its Store lease.
    Json run(const std::string& request_id,
             const std::vector<FakeModelStep>& steps,
             const std::string& amendment_disposition = {});
private:
    Store& store_;
    const SchemaRegistry& schemas_;
};

// Bounded regular-file read for explicit CLI inputs and trusted SQL assets.
// No tilde/env expansion, command execution, symlink or special-file reads.
std::string read_runtime_file(const std::filesystem::path& path,
                              std::size_t max_bytes = 1048576);
FrozenRequestConfig restore_request_config(const Json& request_record);

} // namespace wls::s1
