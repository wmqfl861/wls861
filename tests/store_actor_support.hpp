#pragma once

#include "test_support.hpp"
#include "wls/store.hpp"

namespace test {
// Obtain authority from the current Store. Historical epochs reproduce only
// the original command fingerprint for read-only exact replay; a new command
// must still pass the Store's current epoch, binding and permission guards.
inline wls::s1::StoreActor replay_actor(wls::s1::Store& store,
                                      const wls::s1::StoreActor& original) {
    auto current = store.host_actor(original.role, original.request_id, original.team_id);
    CHECK(current.role == original.role);
    CHECK(current.principal_id == original.principal_id);
    CHECK(current.request_id == original.request_id);
    CHECK(current.task_id == original.task_id);
    CHECK(current.team_id == original.team_id);
    CHECK(current.writer_session != original.writer_session);
    current.dispatcher_epoch = original.dispatcher_epoch;
    current.execution_epoch = original.execution_epoch;
    return current;
}
} // namespace test
