#pragma once

#include "wls/async_coordinator.hpp"
#include "wls/event_runtime.hpp"

#include <functional>
#include <memory>
#include <string>

namespace wls::s1 {

// The owner may wrap the selected Fake script for deterministic offline tests.
// Construction/validation runs on the owner; complete() runs on a model worker
// and must not access Store. Returning a live adapter is always rejected.
using PersistentEventAdapterFactory = std::function<std::shared_ptr<const ModelAdapter>(
    const CapturedModelCall&, const std::vector<FakeModelStep>&)>;

// Explicit finite Store 411 batches. This owns the existing AsyncCoordinator;
// it does not introduce another execution pool or a background inbox thread.
// Each poll advances ready results and may admit independent follow-up work
// while another request's callback is still waiting. Only Store T4 determines
// completed IDs; future readiness is never an inbox acknowledgment.
//
// The first slice supports plan, captured model AMEND, instruction, owner ack
// and note. Original HOLDs, unsupported workflow targets and current-instruction
// recovery notices remain visible. No live adapter, file or process executor.
class PersistentEventRuntime final {
public:
    PersistentEventRuntime(std::unique_ptr<Store> store,
                           std::shared_ptr<const SchemaRegistry> schemas,
                           AsyncCoordinatorLimits limits = {});
    ~PersistentEventRuntime();
    PersistentEventRuntime(const PersistentEventRuntime&) = delete;
    PersistentEventRuntime& operator=(const PersistentEventRuntime&) = delete;

    Store& store();
    // At most 64 explicit request batches per runtime; each request is added
    // once. Store mode must already have been enabled by its original Root.
    void add_request(std::string request_id, EventFakeScript script,
                     EventPumpOptions options = {},
                     PersistentEventAdapterFactory factory = {});
    std::size_t poll();
    Json report(const std::string& request_id);
    AsyncCoordinatorSnapshot snapshot() const;

    // Preserve the original captured control identity/CAS and coordinator's
    // bounded control queue. No refreshed identity is substituted on failure.
    std::future<Json> cancel(StoreActor root, std::int64_t expected_version,
                             std::string command_id);
    std::future<Json> amend(StoreActor root, std::int64_t expected_version,
                            std::string original_text, Json constraint_catalog,
                            std::string command_id);
    Json retained_results() const;
    void flush_retained_evidence();
    void close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wls::s1
