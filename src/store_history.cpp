#include "store_impl.hpp"
#include "wls/config_loader.hpp"
#include "wls/worker_history.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wls::s1 {
using namespace store_detail;

#include "store_history.inc"

Json Store::Impl::history_previous_context(
        const StoreActor& actor, const FrozenRequestConfig& config) {
    return previous_context_projection(*this, actor, config);
}

Json Store::Impl::history_append_delta(
        const StoreActor& actor, const Json& previous) {
    return append_history_delta(*this, actor, previous);
}

} // namespace wls::s1
