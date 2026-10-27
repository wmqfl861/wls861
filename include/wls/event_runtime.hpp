#pragma once

#include "wls/offline_runtime.hpp"
#include "wls/live_model.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wls::s1 {

// A script group is selected by the durable plan's node_id. Calls in each
// logical role/node restart their indices at 1 for each explicit pump; attempt
// identity and full history remain durable and never restart. A missing step
// blocks that actor without reserving an attempt or silently falling back.
struct EventFakeScript {
    std::vector<FakeModelStep> default_steps;
    std::map<std::string, std::vector<FakeModelStep>> by_node;
};

// Accept the existing FakeModel array or {"default": [...], "nodes": {...}}.
// Unknown fields and invalid/duplicate group data are rejected.
EventFakeScript event_fake_script_from_json(const Json& value);

struct EventPumpOptions {
    std::int64_t max_calls{12};
    std::int64_t max_worker_calls_per_task{1};
    Json amendment_dispositions = boost::json::object{};
};

struct LiveWorkflowOptions {
    EventPumpOptions limits;
    LiveExecutionAuthorization authorization;
    std::int64_t reserved_cost_micro_usd{};
    std::chrono::milliseconds max_batch_duration{};
    LiveCredentialResolver credentials;
    LiveUsageEvidenceHook usage_evidence;
};

// Pure configuration/host-bound validation: no Store mutation, network or
// credential lookup. The Store rechecks the original tuple and budget at
// reservation and the first-send claim.
void validate_live_workflow_config(const FrozenRequestConfig& config,
                                  const LiveWorkflowOptions& options);

// Finite, synchronous consumption of durable actor inboxes. pump retains the
// Fake path; pump_live requires explicit host bounds and original live accounting.
// Neither path provides a daemon, implicit retry, delivery or global quota scheduler. A batch
// acknowledges only claim items whose particular transition completed; omitted
// AMEND items, waits, failures and exhausted caps remain pending.
// Schema 406 adds explicit candidate acceptance and source-bound experience;
// terminal requests may run only the remaining experience management jobs.
// Package publication and receiving acknowledgement stay explicit host actions.
class EventRuntime final {
public:
    EventRuntime(Store& store, const SchemaRegistry& schemas,
                 std::map<std::string, WorkerTools*> tools_by_node = {});
    Json pump(const std::string& request_id, const EventFakeScript& script,
              const EventPumpOptions& options = {});
    Json pump_live(const std::string& request_id,
                   const LiveWorkflowOptions& options,
                   std::stop_token stop = {});

private:
    Json pump_workflow(const std::string& request_id,
                       const EventFakeScript& script,
                       const EventPumpOptions& options,
                       const LiveWorkflowOptions* live = nullptr,
                       std::stop_token stop = {});
    Store& store_;
    const SchemaRegistry& schemas_;
    std::map<std::string, WorkerTools*> tools_by_node_;
};

} // namespace wls::s1
