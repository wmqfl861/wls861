#include "store_impl.hpp"
#include "wls/owned_capacity.hpp"

#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wls::s1 {
using namespace store_detail;
namespace {

bool allocation_v2(const Json& intent) {
    const auto* version = intent.as_object().if_contains("version");
    return version && *version == "owned-allocation-v2";
}
void owned_generation(const Json& value) {
    require(value.is_object(), ErrorCode::InvalidInput, "complete filesystem generation required");
    const auto& row = value.as_object();
    for (const auto* key : {"device", "inode", "mount_id", "birth_seconds", "birth_nanoseconds"})
        require(row.contains(key), ErrorCode::InvalidInput, "complete filesystem generation required");
    require(number(row, "device") > 0 && number(row, "inode") > 0 && number(row, "mount_id") > 0 &&
            number(row, "birth_seconds") >= 0 && number(row, "birth_nanoseconds") >= 0 &&
            number(row, "birth_nanoseconds") < 1000000000, ErrorCode::InvalidInput, "invalid filesystem generation");
}
Array owned_path_strings(const std::vector<std::string>& values) {
    // Retained allocation paths are original intent bytes, not tool grants:
    // apply the owned path domain while keeping the same nonempty/NUL rule,
    // stable dictionary order and duplicate rejection.
    Array result;
    std::set<std::string> unique;
    for (const auto& value : values) {
        text_field(value, "owned path", 4096);
        require(unique.insert(value).second, ErrorCode::InvalidInput, "duplicate owned path");
    }
    for (const auto& value : unique) result.emplace_back(value);
    return result;
}

} // namespace

#include "store_cleanup.inc"

} // namespace wls::s1
