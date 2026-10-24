#pragma once
#include "wls/sandbox.hpp"
namespace wls::s1 {
// Trusted host observer; caller must hold the workspace's exclusive cooperative
// lock and ensure no business descendants are running. No shell is executed.
// Refuses partial observations, unsafe entries and unsupported inode generations.
Json observe_process_workspace(int root_fd);
Json process_workspace_changes(const Json& before, const Json& after);
// Pure evidence classifier. This does not grant dispatch or reconciliation.
// Synthetic inputs can exercise it in tests, never prove a deployed sandbox.
std::string process_operation_outcome(const ProcessResult& process);
}
