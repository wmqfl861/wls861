#include "store_impl.hpp"
#include "wls/capacity.hpp"

#include <algorithm>
#include <mutex>
#include <set>

namespace wls::s1 {
using namespace store_detail;

#include "store_capacity.inc"

Json Store::capacity_status(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->capacity_state(id);
}

void Store::validate_capacity_checkpoint(const StoreActor& root, std::int64_t expected_window,
        const std::string& evidence, const std::string& command_id) const {
    const std::lock_guard lock(impl_->mutex);
    text_field(command_id, "command_id");
    const auto command = impl_->checkpoint_intent(root, expected_window, evidence);
    (void)impl_->authorize(root, {Role::Root}, false);
    if (impl_->replay(root.request_id, command_id, command)) return;
    (void)impl_->checkpoint_state(root.request_id, expected_window);
}

Json Store::checkpoint_capacity(const StoreActor& root, std::int64_t expected_window,
        const std::string& evidence, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex);
    text_field(command_id, "command_id");
    const auto command = impl_->checkpoint_intent(root, expected_window, evidence);
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(root, {Role::Root}, false);
    if (auto old = impl_->replay(root.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto state = impl_->checkpoint_state(root.request_id, expected_window);
    const Row result{{"request_id", root.request_id}, {"window", next(expected_window)},
        {"totals", state.at("totals")}, {"task_submissions", state.at("task_submissions")},
        {"closed_counts", state.at("counts")}, {"limits", state.at("limits")},
        {"audit_rows_retained", true}, {"budgets_reset", false}};
    impl_->capacity_raw_event(root.request_id, command_id, command, result);
    tx.commit(); return result;
}

} // namespace wls::s1
