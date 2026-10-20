#pragma once

#include "wls/offline_runtime.hpp"

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

// Finite, synchronous consumption of durable actor inboxes. No daemon, implicit
// retry, delivery, global quota scheduler or real adapter is provided. A batch
// acknowledges only claim items whose particular transition completed; omitted
// AMEND items, waits, failures and exhausted caps remain pending.
class EventRuntime final {
public:
    EventRuntime(Store& store, const SchemaRegistry& schemas,
                 std::map<std::string, WorkerTools*> tools_by_node = {});
    Json pump(const std::string& request_id, const EventFakeScript& script,
              const EventPumpOptions& options = {});

private:
    Json pump_workflow(const std::string& request_id,
                       const EventFakeScript& script,
                       const EventPumpOptions& options);
    Store& store_;
    const SchemaRegistry& schemas_;
    std::map<std::string, WorkerTools*> tools_by_node_;
};

} // namespace wls::s1
