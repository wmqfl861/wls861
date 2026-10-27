#pragma once

#include "wls/config_loader.hpp"
#include "wls/store.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace wls::s1::runtime_detail {

// A runtime may pass only the immutable configuration already restored on
// its current call stack. Every use still reads the current request and
// matches both its complete canonical bytes and hash; no authority or
// mutable request state is retained here or shared with another call.
inline void require_matching_config(const Json& request, const FrozenRequestConfig& config) {
    const auto& hash = request.at("config_snapshot_sha256");
    if (!hash.is_string() ||
        config.sha256() != std::string_view(hash.as_string().data(), hash.as_string().size()) ||
        config.serialize() != canonical_json(request.at("config_snapshot")))
        throw Error(ErrorCode::Corrupt, "validated runtime configuration differs from the current request");
}

// These private variants preserve every current Store query. The public
// wrappers pass nullptr and continue to restore the full configuration.
std::vector<Json> visible_management_questions(Store& store, Role role,
    const std::string& request_id, const std::string& task_id,
    const FrozenRequestConfig* config);
Json next_workflow_action(Store& store, Role role, const std::string& request_id,
    const std::string& task_id, const FrozenRequestConfig* config);

} // namespace wls::s1::runtime_detail
