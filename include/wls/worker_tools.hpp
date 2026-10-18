#pragma once

#include "wls/config_loader.hpp"
#include "wls/context.hpp"
#include "wls/sandbox.hpp"
#include "wls/store.hpp"
#include "wls/workspace.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <stop_token>

namespace wls::s1 {

enum class ToolClass { Business, Diagnostic, Control };

struct ToolDefinition {
    std::string name;
    std::string version;
    ToolClass category;
    std::string side_effect;
    Json input_schema;
};

// Static host registrations. Neither category, effect, workspace, environment,
// sandbox policy nor an authenticated actor can be supplied by model arguments.
const std::vector<ToolDefinition>& worker_tool_registry();
const ToolDefinition& worker_tool_definition(std::string_view name);
void validate_worker_tool_arguments(std::string_view name, const Json& arguments);

// The optional manifest is trusted deployment input, not model output. Its
// runtime paths/hashes are frozen by bind_workspace alongside request policy.
// Empty manifests support bounded file tools, but cannot enable process run.
SandboxPolicy worker_sandbox_policy(const FrozenRequestConfig& config,
                                    std::string_view template_id,
                                    const Json& manifest = boost::json::object{});
Json worker_execution_policy(const FrozenRequestConfig& config,
                             std::string_view template_id,
                             const SandboxPolicy& policy);

class WorkerTools final {
public:
    WorkerTools(Store& store, const std::filesystem::path& workspace_root,
                Json sandbox_manifest = boost::json::object{},
                bool read_only = false);

    // Host calls this after the team exists and before exposing actual tools.
    // First binding is durable; later invocations must match every identity and
    // policy field. ReadOnly instances only verify an existing binding.
    void bind(const StoreActor& actor);
    std::vector<ContextTool> declarations(const StoreActor& actor) const;
    Json environment_card(const StoreActor& actor) const;

    Json execute(const StoreActor& actor, const ExecutionBinding& binding,
                 std::int64_t expected_task_version,
                 const std::string& business_key, const std::string& tool_name,
                 const Json& arguments, const std::string& command_id,
                 std::stop_token stop = {});
    Json diagnostic(const StoreActor& actor, const ExecutionBinding& binding,
                    const std::string& tool_name, const Json& arguments);

private:
    void check_actor(const StoreActor& actor) const;
    void verify_workspace(const StoreActor& actor) const;
    SandboxPolicy policy_for(const StoreActor& actor) const;
    void rate_limit(const StoreActor& actor, const Json& allowance);
    Json control(const StoreActor& actor, const ExecutionBinding& binding,
                 std::int64_t expected_task_version, const std::string& tool_name,
                 const Json& arguments, const std::string& command_id);
    Store& store_;
    Workspace workspace_;
    Json manifest_;
    bool read_only_;
};

} // namespace wls::s1
